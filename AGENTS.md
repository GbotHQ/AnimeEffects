# Notes for coding agents

Practical recipes for working on this repository. Everything here was **measured by running it**;
where something is not verified, it says so. Add to it when you learn something that cost you time.

## Seeing what the application prints

**Qt does not send `qDebug`/`qInfo`/`qWarning` or QML `console.*` to stderr when stderr is a file
or a pipe.** Its default message handler writes to the **systemd journal** instead, so a redirected
run looks silent even while the application is complaining:

```bash
# silent, even if the app is warning about something:
./build/src/gui/Debug/AnimeEffects assets/sample.psd > /tmp/app.log 2>&1

# the fix:
QT_FORCE_STDERR_LOGGING=1 ./build/src/gui/Debug/AnimeEffects assets/sample.psd > /tmp/app.log 2>&1
```

`QT_FORCE_STDERR_LOGGING=1` is the modern name; `QT_LOGGING_TO_CONSOLE=1` and
`QT_ASSUME_STDERR_HAS_CONSOLE=1` are the older spellings.

**QML `console.log` is filtered on top of that.** It maps to `qCDebug` in category `qml`, which the
default logging rules disable; `console.info` / `warn` / `error` arrive without extra help. To see
`console.log`:

```bash
QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES="qml.debug=true" ./build/.../AnimeEffects ...
```

Measured, same program, stderr redirected to a file:

| environment | lines reaching the file |
| --- | --- |
| none | 0 |
| `QT_FORCE_STDERR_LOGGING=1` | 1 — `console.info` arrives, `console.log` still filtered |
| `QT_LOGGING_RULES=*.debug=true` alone | 0 |

Do **not** set `*.debug=true` routinely: Qt's font-database and pointer-trace chatter buries the
application's own output.

What stderr lost is in the journal:

```bash
journalctl --user --since "5 min ago" | grep -i <what-you-are-looking-for>
journalctl --user _COMM=AnimeEffects          # same thing, narrowed to the app (not exercised here)
```

`scripts/gui/_lib.sh` exports `QT_FORCE_STDERR_LOGGING=1`, so probe logs now carry the
application's warnings and QML errors instead of only their own `printf`s.

## Driving the GUI headlessly

The app needs a real GL context, so it runs under Xvfb with the xcb platform:

```bash
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb
```

**Both lines, every time — setting `DISPLAY=:77` is not enough.** Qt 6 prefers Wayland whenever
`WAYLAND_DISPLAY` is set, regardless of `DISPLAY`, so a one-off launched as `DISPLAY=:77 ./app`
quietly connects to the real compositor and **opens its window on the desktop you are working on**
(measured: a spike written that way did exactly that). `ae_guard_display()` below guards *input*
only; it says nothing about where the application itself will appear. When you find yourself typing
`DISPLAY=...` by hand instead of using `xvfb-run`, that is the moment to re-check these two lines.

**And the platform theme has to be pinned, or the FILE DIALOG opens on that same real desktop.**
`scripts/gui/_lib.sh` exports `QT_QPA_PLATFORMTHEME=generic`. A session that sets the variable
(COSMIC sets `cosmic`) makes Qt load that theme's plugin, and `libcutecosmictheme.so` asks
`org.freedesktop.portal.FileChooser` for its dialog — the *portal* draws that window in the live
session, while the probe types into Xvfb and waits for a dialog that is not there. Measured
2026-09-29: a probe sweep prompted for a filename over and over on the desktop, with the
counterpart line in the journal:

```
xdg-desktop-portal-cosmic[575979]: no file chooser dialog with ID Id(642)
```

Two wrong fixes to skip, both checked: `QT_NO_XDG_DESKTOP_PORTAL=1` does not stop it (cutecosmic
never reads that variable — zero occurrences of it in the plugin), and *unsetting*
`QT_QPA_PLATFORMTHEME` does not either (Qt's candidate list came out `("cosmic","generic")`, so the
portal theme is offered even when nothing was asked for). Only pinning a theme with no portal
strings works: of the four platform themes installed, `libcutecosmictheme.so` (1) and
`libqxdgdesktopportal.so` (2) contain the FileChooser bus name, `libqgtk3.so` and `libqt6ct.so`
contain none. With `generic` the dialog is an ordinary widget window inside Xvfb — verified by
opening one deliberately: a new window on the Xvfb display, "Save As" with `nene.mp4` already
selected in its filename field, and no portal line in the journal.

So before a long run that will open a dialog, run one dialog as a **canary** and ask the journal:

```bash
T0=$(date '+%Y-%m-%d %H:%M:%S')
# ... open exactly one dialog the probes would open ...
journalctl --user --since "$T0" | grep -i file_chooser | grep -v '\[systemd-run\]'   # must print nothing
xdotool search --onlyvisible --name . | sort            # the dialog's window must be in here
```

The `grep -v '\[systemd-run\]'` half is load-bearing: this harness runs every command through a
`[systemd-run]` scope whose journal START line records the command **text**, so a command that
merely *contains* the word `file_chooser` (say, a script that greps for it) contaminates its own
canary window and false-FAILs. Measured 2026-09-30: 4 journal hits in a clean window, all four the
harness's own echoes (one was the check itself), 0 real portal lines.

**And the check cannot attribute a line to a process.** On a desktop in active use, other apps'
file-chooser traffic trips it too: measured 2026-10-01, a sweep flagged `portal=1` on
`xdg-desktop-portal-cosmic … file_chooser: no file chooser dialog with ID Id(1064)` while the
journal's own neighborhood showed the user's `vicinae` clipboard activity and unrelated portal
traffic (same signature appeared at Id(872) with no agent work running at all). Before treating
`portal=1` as a leak: read the journal neighborhood — if the surrounding lines are other apps and
the canary was green, it is ambient traffic, not us. Our runs are identifiable by the pinned
`QT_QPA_PLATFORMTHEME=generic` + Xvfb (the canary proves dialog placement).

PROBE-SAFETY §R3 (`/home/gambot/dev/dsh-demo/PROBE-SAFETY.md`) predicted this class of failure —
this is what it looks like on a Wayland desktop.

**Coordinate clicks and AT-SPI actions work with no window manager** — that is what `scripts/gui`
relies on (`ae_press`, `ae_activate`).

**Keyboard input does not.** Without a WM the window never becomes active, Qt Quick has no active
focus item, and every key is silently dropped. Run a WM inside the display and target the window
properly:

```bash
xvfb-run -n 71 --server-args="-screen 0 1440x900x24" bash -c '
  openbox > /dev/null 2>&1 &          # openbox is installed on this machine
  sleep 2
  ./build/src/gui/Debug/AnimeEffects assets/sample.psd > /tmp/app.log 2>&1 &
  app=$!
  sleep 20                            # see "readiness" below — poll, do not sleep, in real scripts
  W=$(xdotool getactivewindow)                     # prefer this, or: search --class AnimeEffects
  eval $(xdotool getwindowgeometry --shell $W)     # gives X, Y, WIDTH, HEIGHT
  xdotool windowactivate --sync $W
  xdotool mousemove $((X + WIDTH/2)) $((Y + HEIGHT/2)) click 1
  xdotool key Down
  kill $app'
```

Six traps, all of which have cost real time here:

- `xdotool search --name "<title>"` frequently matches **nothing** — a Qt dialog's `WM_NAME` is not
  the title you expect. Use `xdotool getactivewindow`, or `--class`.
- A click at *guessed* coordinates can land **outside** the window and take focus away, which looks
  exactly like "keys do not work". Always use the window's own geometry.
- Verify focus **from inside the application** (`QGuiApplication::focusWindow()`,
  `QQuickWindow::activeFocusItem()`). "No keys arrived" has two very different causes: no focus, or
  focus with nothing handling the key.
- **A fixed `sleep` is not a readiness check.** The app is ready in ~4 s on an idle machine but takes
  >25 s under load, and a probe that slept 12 s failed 15/15 *while the application was healthy* —
  what it had found and was driving was a 200x71 splash screen. Poll for something that exists only
  once the project is loaded: `scripts/gui/_lib.sh` has `wait_ready`, which polls `ae wait "File"`.
  (With an `.anie` on the command line the GUI satisfies it with its built-in debug project first —
  see the next bullet.)
- **A run of this app needs a memory ceiling, and one instance at a time is not optional.** Measured
  2026-10-08 the hard way. `nene.anie` is a big project (15000x15000 canvas, ~112 image layers,
  2.4 GB of image data decoded to RGBA): ONE instance on it climbs to **~10 GB resident in ~80 s and
  then stays there** for minutes — the GUI and the headless `roundtrip` both do exactly that, so it is
  a heavy project, not a leak. Nothing stopped it being run four times over: what took the owner's
  46 GB machine down twice was a long-lived instance being tree-walked by `ae_ui.py`, the 24-test
  suite, and a 13-probe sweep (every probe opens its own instance) alive at once — the instance found
  holding **~38 GB resident / ~95 GB virtual**. `wait_ready` now starts `ae_memory_cap` for the app
  (12 GB default, deliberately above the ~10 GB an honest load needs; `AE_MAX_RSS_KB` to raise it) so a
  runaway dies instead of the machine. Because of that cost, `_lib.sh` picks the **smallest** `.anie` it can
  find as the probe default rather than the first one the filesystem returns (pass a path or set
  `AE_PROJECT` to test a specific project) — measured on a 425 KB project: ~9 s load and 449 MB resident,
  against ~80 s and ~10 GB for `nene.anie`. One probe is the exception: `probe_capped` asserts that the
  *natural* export lands over a destination's declared cap (that is how it tells fitted from unfitted),
  which no sub-megabyte project here can show — measured 246 KB, 232 KB and 13 KB against WeChat
  sticker's 500 KB cap — so `run_probes.sh` hands it the largest project (`AE_CAPPED_PROJECT` to
  override) and everything else stays light. The rules that follow: one app instance at a time, never a
  long-lived instance for a long session of a11y queries, never two probe/suite jobs at once.
- **`wait_ready` does not mean the project you asked for has loaded.** The GUI loads its debug project
  (`assets/sample.psd`) first and only then the `.anie` from argv, so the window and its File menu are
  up while the argument project is still loading — measured: `nene.anie` needs ~80 s and ~10 GB, and a
  probe driving the UI during that time is driving the *debug* project's document. Wait for something
  the requested project provides (a layer name, a canvas size) when the probe's subject is its content.
- **A QML item inside a `QQuickWidget` reports its rect in the *widget's* coordinates, not the
  screen's.** AT-SPI gave the export dialog's search field `x=14` while it actually sat at `x=75`,
  because the dialog window is at `x=61`. A drag aimed with the raw rect missed by exactly that
  offset, hit the row underneath, and looked like "the wrong control moved". Add the dialog's origin
  before driving `xdotool`; `ae_y` in `_lib.sh` does that for the vertical axis.
- **`DISPLAY` is the variable that points at the REAL desktop, and nothing in `_lib.sh` sets it.**
  `unset WAYLAND_DISPLAY` plus `QT_QPA_PLATFORM=xcb` only decide where *Qt* connects; `xdotool`
  reads `$DISPLAY`. Measured: an unwrapped run (journal, Sep 28 11:23:27) sent
  `xdotool mousemove 100 40 click 1`, `key Down` and `type "ab"` into the live session. `_lib.sh`
  now has `ae_guard_display()`, which **exits 97** unless an Xvfb is serving the current display; it
  runs from `wait_ready`, `ae_press` and `ae_drag`, and every probe calls `wait_ready` inside its
  `xvfb-run` body before its first raw `xdotool`. Before writing a one-off script, either wrap it in
  `xvfb-run -a --server-args="-screen 0 1440x900x24"` or call the guard first — a script that does
  neither drives whatever is under your cursor.

### Driving a dialog that opens mid-session

There is no window manager, so `xdotool getactivewindow` returns 0,0 and neither title nor class
matching can be trusted. Identify the dialog as **the window that did not exist before**:

```bash
before=$(xdotool search --onlyvisible --name . | sort)
# ... do whatever opens the dialog ...
after=$(xdotool search --onlyvisible --name . | sort)
dialog=$(comm -13 <(echo "$before") <(echo "$after") | head -n1)
eval $(xdotool getwindowgeometry --shell "$dialog")   # X, Y, WIDTH, HEIGHT
```

### When the desktop itself freezes

`libinput` writes its own signature to the journal when the compositor stops keeping up:

```
gnome-shell: libinput error: event2 - <your mouse>: client bug: event processing
             lagging behind by 3988ms, your system is too slow
```

Read the **number, not the line**. In one boot there were four such lines: 49 ms, 26 ms, 75 ms and
3988 ms. The first three are ordinary load; only the ~4 s one is a genuine stall. So this is a cheap
before/after check for anything that might affect input latency:

```bash
journalctl -b -1 2>/dev/null | grep "lagging behind"
```

It says whether an episode happened and how bad it was — it does **not** say what caused it. During
the 3988 ms episode `gnome-shell` itself was the hot process, and correlating it with whatever was
running at the time is not causation.

## Asking about key *routing* rather than about the platform

If the question is "which QML item receives this key", inject it in-process — that is what Qt's own
autotests do. No WM, no X, deterministic:

```cpp
QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
QCoreApplication::sendEvent(quickWidget, &press);   // reaches the scene's focus item
```

**You do not even need the widget, the window or any focus.** Sending the event straight to the
*item* reaches its `Keys` filter with `item->window() == nullptr` and `activeFocus == false`, and
whatever the filter does not consume still reaches the editor (measured: a `TextField` given a
character key really does grow its text and move its caret under `QT_QPA_PLATFORM=offscreen`). So a
key-routing test needs no display and no GL context, and can be an ordinary `tests/` target — see
`tests/picker_keyboard.cpp`, which loads the real `DestinationPicker.qml` and asserts on the model.

Two consequences worth knowing:

- `event.isAccepted()` **cannot** tell "the filter ate it" apart from "the editor ate it". Assert on
  an observable effect instead: pick a key whose editor action you can see (Left moves
  `cursorPosition`) and check that *that* did not happen.
- A test where the filter could not have fired proves nothing. Assert the preconditions inside the
  test — that some group is open, that a row below exists — so a run that could not observe the
  regression fails loudly instead of passing quietly.

Key propagation in Qt Quick follows the **item hierarchy**: an unhandled key travels to the parent
item and **never to a sibling**. A handler on a list will not see keys typed into a search field
beside it — the handler must live on a common ancestor, or the field must forward them.

## Two QML traps for geometry walks (measured 2026-09-30, the overflow increment)

- **`parentItem` is a C++ getter, not a QML property.** `a.parentItem` inside QML JavaScript is
  `undefined`, and a walk written with it dies **silently after one hop** (or parks the item at its
  fallback position — a tooltip sat 30 px past the viewport while its fit-rect was correct). Walk
  the item tree with `.parent`.
- **QML bindings cannot track `mapToItem` geometry.** An `x:`/`y:` binding that calls
  `mapToItem`/`mapFromItem` stays **stale after scrolls and resizes** even though the function's
  result would change — the binding engine cannot see through the call. The walk must explicitly
  read each ancestor's `x`/`y`/`width`/`height`/`contentX`/`contentY`; **those reads are the
  dependencies** the binding needs. Measured: `clampRect()` returned the right rect while `tip.y`
  kept the pre-scroll value until the reads were added.

## A `Q_INVOKABLE`/method CALL in a binding is not a dependency (measured 2026-10-01)

`visible: … && exportModel.manualMode()` — the **call** form of a `Q_PROPERTY` method registers
**no binding dependency**, so the binding evaluates once and freezes at its first value (measured:
the codec row stayed hidden in the mode it should have shown in; the dump said "hidden Codec" no
matter what). The **property** form `exportModel.manualMode` (with its `NOTIFY`) re-evaluates
correctly. Check every `()` binding on an object property when a state switch "doesn't take".

The last instance was the export dialog's Quality row (`ExportDialog.qml:511` label, `:516` combo,
both `!exportModel.manualMode()`). It threw `TypeError: Property 'manualMode' … is not a function`
on every dialog construction and left the row **visibly drawn in both modes** — and no test noticed,
because a throwing binding writes no value and the property kept its default `true`. Measured on the
`89da2ed` tree and fixed 2026-10-03, together with the predicate it should have had: the row follows the **artifact**, not the
mode (`qualityVisible` = `!imageQualityVisible`; see export-dialog-behavior.md item 46). A binding
that cannot evaluate is invisible to a suite that only asserts what it expects to see — assert both
directions, and keep the stderr check that catches the `TypeError`.

## Wayland

Xvfb is X11 and cannot stand in for a compositor when the behaviour *is* the platform:
window placement, popups and tooltips, IME, focus. For those, drive the real session with
**`ydotool`** (installed), remembering that the window must be activated first.

Three capture notes:

- `import -window root` (ImageMagick) does **not** contain the pointer — no X11 screen grab does,
  which is why `ffmpeg` grew `-draw_mouse`. It works end to end (measured: it captured the I-beam
  over the export dialog's search field):

  ```bash
  ffmpeg -loglevel error -f x11grab -draw_mouse 1 -video_size 1440x900 -i ":$DISPLAY" \
         -frames:v 1 -y shot.png
  ```

  A cursor is a handful of pixels — crop and scale it (`-filter point -resize 400%`) before judging
  it, or you will read "no cursor" off a frame that has one.
- To measure a **delay** (tooltip wake-up, hover), record instead of snapshot: one screenshot cannot
  separate "instant" from "appeared during ffmpeg's ~300 ms startup". Record a few seconds at 30 fps
  while moving the pointer, then compare, per frame, the **hover highlight** against the thing being
  timed and take the first frame where each changes. Done that way, the picker's tooltip and the row
  highlight changed on the same frame — ≤33 ms, i.e. no wake-up delay at all (contract line E26
  forbids exactly that).
- A Wayland-native window is invisible to X11 capture; a black frame usually means "you captured
  the wrong compositor", not "the app did not draw".

When tearing a manual run down: `pkill -f "<pattern>"` **matches the shell running the command**,
because that pattern is in its own command line — it SIGTERMs the command before the message it
would print. Kill by PID (`pkill -a Xvfb`, then `kill <pid>`), or match on the process name only.

## Translations (`assets/locale/*.ts`)

Extraction runs through the build, never by hand — lupdate renumbers every `<location>` in a
catalog, so a hand edit and a real run are indistinguishable in the diff except by being wrong:

```bash
ninja -C build 'src/gui/update_translations:Debug'   # the :Debug / :Release suffix is REQUIRED
```

Measured traps, all of which cost time here:

- `ninja -C build src/gui/update_translations` fails in 0.08 s with `unknown target`: on this
  Ninja Multi-Config tree the configuration is part of the target name. The `:Release` variant runs
  but prints **no** `Found … source text` summary, so anything you want to quote about extraction
  counts has to come from the `:Debug` target.
- The **aggregate** has the same trap, from the other side: `ninja -C build all:Debug` builds, and
  `ninja -C build all:Release` dies in 0.4 s with `ninja: error: 'all:Release' missing and no known
  rule to make it`. Ninja Multi-Config writes one ninja file **per configuration**, each with its
  OWN `all` — `build-Debug.ninja`, `build-Release.ninja`, `build-RelWithDebInfo.ninja` — and the root
  file aliases only a few targets across configurations (`:Release` is carried by 38 targets here,
  all of them `install`/`clean`). So reach another configuration through the front end or its config
  file: `cmake --build build --config Release`, or `ninja -C build -f build-Release.ninja all`.
  Measured on the tree that added this note: Debug `no work to do`, Release **88 steps**,
  RelWithDebInfo **707** (never configured to completion).
- **`en` is not a translated language.** It is absent from
  `qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES es ja ko ru zh)` (root `CMakeLists.txt:29`),
  so `translation_en.ts` is the source-language stub. lupdate prints `Updating …/translation_en.ts`
  and a `Found … source text(s)` summary (`Found 893 source text(s) (893 new and 0 already existing)`
  at the run that first measured this) and then leaves the file holding ONLY the `numerus="yes"`
  messages: after `73ba0919` that is **5 entries** - 2 under `GraphPanel`, 1 under `QCoreApplication`,
  2 under `gui::ImGuiOverlay` (one of those `type="vanished"`) - so a `grep` there for a plain source
  string finds nothing, which is the shape, not a gap. Do not "fix" it, and never read its
  `unfinished` count as debt.
- lupdate sees what the *translation units* contain. A `tr()` in a file missing from
  `AE_I18N_SOURCES`, or a `QT_TRANSLATE_NOOP` in a header nothing `#include`s, is invisible **with
  no error at all** — the string simply never appears in a catalog. `src/ctrl/ExportTargetStrings.h`
  is listed by name for exactly that reason (`src/gui/CMakeLists.txt:239-241`).
- `scripts/run_suites.sh` gates `grep -c unfinished` = 0 and runs `check_tr_contexts.py` plus
  `gen_export_targets_i18n.py --check` before any suite. `check_tr_contexts.py` proves only that a
  context *can be asked for*; none of the three proves a string survives `.ts` → built `.qm` → the
  context the application asks. That needs a real `QTranslator` over the built `.qm`: `lrelease`
  and `lconvert` are not on `PATH` here and there is no PySide6, so a few lines of C++ against
  `Qt6Core` (headers under `/usr/include/qt6`) is the cheap route.
- The context is whatever the call site names — the QML file's `pragma Translator` for a QML
  string, `QT_TRANSLATE_NOOP("ExportTargets", …)` for the destination table's. A lookup in the
  wrong context returns the **English source without any error**, so a check has to assert the
  translation differs from the source, and ask once in a deliberately wrong context to show the
  lookup is actually context-keyed.

Two more, measured after the redesign's i18n commit (`73ba0919`, which added 14 strings extracted
from the panel, the header and the chip):

- Graded by the toolchain rather than a grep: `lrelease` on each of the five translated catalogs
  reports **948 translations, 948 finished, 0 unfinished**, and 4/4 for `translation_en.ts`. The raw
  `<message>` counts differ per catalog - 1434 es, 1434 ja, 1387 ko, 1401 ru, 1427 zh - and each minus
  its own `type="vanished"` count (486/486/439/453/479) is exactly the same 948, so a raw entry count
  is a history measure and not progress. The binaries are not on `PATH`; they are at
  `/usr/lib64/qt6/bin/lrelease` (and `lconvert` beside it).
- The panel's translation context is FROZEN at `gui::ImGuiOverlay` even though the class that named
  it is gone - the file renamed to `GraphPanelFrame.{h,cpp}` (`aa92de2a`). The context is a CATALOG
  KEY, not a description: renaming it would silently ship English in five languages until every
  catalog is re-extracted and re-translated, and the code says exactly that
  (`GraphPanelFrame.cpp:198-203`). The consequence is why the rename cost no translation work: the
  panel's entries kept their ids, and the redesign added exactly 14 new ones (`git show 73ba0919`
  adds 14 `<source>` lines per catalog).

## The editing model: one open edit, one command, no half-solved document states

`cmnd` commands are value-replayable — `Base::undo/redo` must rebuild the document from the
values the command carries (`src/cmnd/Stable.h`), with no help from the editor that created
them. The invariant that follows, and the first thing to check before writing an editor:

> **every state the document holds must be reproducible by replaying the stack.**

What that forces, all of it visible here:

- An iterative or multi-body solver may not mutate the document. The pose tool's "Pull bones"
  solve runs 16 sub-steps whose intermediate poses mean nothing as document states, so it works
  on a scratch copy (`ctrl::pose::BoneDynamics`, a shadow tree built for one drag event) and
  commits a single `RotateBones` value command. Any value-based undo engine imposes this shape;
  it is not a shortcoming of this one.
- Coalescing is per *command*, not per *gesture*: a drag is one undo step because the editor
  keeps the command it pushed and rewrites it while `Stack::isModifiable` still holds
  (`cmnd/Stack.cpp:52/103/131/160/164`). That gate is re-implemented at 14 sites (grep
  `isModifiable`), each repeating its own macro + notifier + optional key-push ritual.
- Editors that need transient state *outside* the document hand-roll it, at two different
  layers: ctrl tools build scratch (shadow tree, mesh mover, brush state); GUI editors build
  drafts with their own session undo (`gui/prop/EasingPickWidget.h:190-194`,
  `gui/qml/AnimationSettingViewModel.h:27-62` — "the popup owns its undo; the model gets one
  step on close", pinned by `tests/anim_settings_undo.cpp`).

Nothing in the engine represents "an open edit", which is why every editor re-derives this and
why the bugs in this area are always in a *seam*, never in the model: scratch ownership and
teardown (`TreeUtil::deleteAll`'s use-after-free write — the 2026-10-07 "Pull bones" crash),
scratch pointers outliving their container (`BoneKey::deserialize`), key ownership while the
scratch also pushes a key (`KeyOwner` + `cmnd::GrabNewObject`), and re-entrancy when a widget
two-way binds to the model *and* pushes a command (the export aspect lock; hence
`ScopedUndoSuspender` in `ctrl/ScopedModifier.h`).

The engine half of the seam now exists: `cmnd::Gesture<tCommand>` (`src/cmnd/Gesture.h`) is the
open edit — `begin(name, builder)` opens the macro, runs the builder with it, pushes and adopts
the command it returns; `continues()` says whether that command is still the stack's modifiable
one; `end()` closes the edit. `tests/cmnd_gesture.cpp` pins those semantics.

The ctrl half was measured before writing it, and **declined**: the ritual is not one ritual. Of
the 15 coalescing sites there are ~7 structural shapes and at least ten axes that vary — two
listener designs (`bone::Notifier`/`mesh::Notifier` stateful-and-grabbed vs
`TimeLineUtil::Notifier` configured-then-grabbed), a rewrite branch that sometimes re-notifies
inline, sometimes through a separate `notifyAssign()` called from the paint path, and sometimes
not at all; four key-push shapes (none, `if (owns()) pushOwnsKey`, a per-target loop,
self-gating `pushOwning*Key`); three reset styles (press+release, press+clearState, lazy);
`ffd_BrushMode` and `TimeLineEditor` use a different gate entirely. A single helper absorbing
that would be a policy object — the duplication would move, not disappear.

So: migrate per family, coverage first, with the recipe above (`Gesture` + the builder keeping the
notifier wiring, the key push and the command construction byte-identical). Two sites also keep a
hand-rolled gate on purpose: `srt_MoveMode` gates three independent commands (move/rotate/scale)
that must stay separately re-appliable, and `ffd_BrushMode`'s create branch uses `!commandRef`
rather than the stack's gate.

Family status, measured:

- pose: **migrated** to `cmnd::Gesture` (TransBoneMode, DrawBoneMode, ErasePoseMode) and verified with
  `tests/pose_pull_crash --mode draw|move|erase`: every mode reports a bone hit and pushes exactly one
  command for the whole drag (measured under gdb: one `Stack::pushImpl` per gesture, before and after).
  Erase needed its own hit rule in the harness — it has no `bone::Focuser`, its edit is a brush
  multiply, and at a rest pose `eraseRate = 1 - eiPressure` is 0 so no value moves; the harness now
  also requires a `TimeLineEvent` on the press, which draw and move cannot produce. The three pose
  modes are the seam's reference implementation.
- timeline: the gate (`TimeLineEditor::modifyMoveKeys:371`) runs only on a drag, and the existing
  timeline tests (`creation_select_keys`, `rightclick_transient_selection`) only click, so it is
  unreached.
- bone: no harness. The hit rule would be `bone::Focuser` (same as pose draw/move), so a pose-style
  harness that selects a BoneKey target is the cheapest way in.
- mesh: no harness; needs a `mesh::MeshAccessor` over a real `MeshKey` and its own focuser.
- srt: no harness; hit rule is `srt_Symbol::findFocus` around the object's SRT symbol.
- ffd: no harness; its focuser is a GL transform-feedback task run through `DriverResources`.

## The animation graph panel (Dear ImGui), and its overlay host

The panel is **drawn by Dear ImGui 1.92.7** and has had ONE host since `aa92de2a`: the timeline
dock's `Graph` tab (`gui::GraphPanelWidget`), described in "The redesign as it now stands" below. The
fullscreen viewport overlay it had beside the dock is DELETED, together with its contrast-outline
composite, its framebuffer-pixel cache, its `topInset` forwarding and every input route it owned -
and with it the Tools dock's "Show animation graph" toggle and `MainViewSetting::showGraphEditor`
(the dock's own tab IS the control). `scripts/gui/probe_graph_overlay.sh` went the same way: a probe
that drives a removed host is worse than none. The class was renamed with the host -
`ImGuiOverlay.{h,cpp}` -> `GraphPanelFrame.{h,cpp}`, which is now the shared frame driver, the
`stroke` and `outline` modules and the accessibility publication - **so every `ImGuiOverlay.cpp:NNNN`
/ `ImGuiOverlay.h:NNNN` citation in this file names `GraphPanelFrame.{h,cpp}`, whose line numbers
have moved** (that header still carries the old `GUI_IMGUIOVERLAY_H` include guard). Pinned by a
`FetchContent` declaration in the root `CMakeLists.txt` behind
`AE_ENABLE_IMGUI` (default ON). The sources are NOT in this repository: they are
fetched into `build/_deps`. `-DFETCHCONTENT_SOURCE_DIR_IMGUI=<an existing
checkout>` makes the configure hermetic and fetches nothing (measured: same
configure time as with the option OFF, no `_deps` created). One static library,
`ae_imgui`, compiles ImGui's four core sources for the application and the tests.

**Why not the hand-rolled overlay engine.** `ctrl::GLCorePaintEngine` has no
path support (`drawPath()` is `{ (void)aPath; }`, `src/ctrl/Painter.cpp:127`) and
no clipping, so the panel's geometry had to be cut by hand and a curve could not
be stopped from stroking over the legend. ImGui clips with `PushClipRect` and
drew the same content **2.2x cheaper** in a measured A/B (928x589 viewport, 8
curves x 201 points + 72 keys + labels; painter 10.24 ms mean vs ImGui 4.70 ms).
CAVEAT, measured on llvmpipe only: the painter path pays per *command*, so on a
real GPU its absolute cost collapses and the ratio is not verified there. Note
also, from the same measurement: removing the grid does not make the painter path
cheaper (10.50 ms vs 10.24 ms) - the grid-free design saves coverage, not
commands.

**Where CJK comes from.** The hand-rolled engine never rasterizes text:
`GLCorePaintEngine::drawTextItem` (`src/ctrl/Painter.cpp:199-218`) asks Qt for
the string, draws it white into a texture, caches it **keyed by the string**
(`gl::TextObject::getMapKey`) and blits it tinted by the brush. fontconfig
fallback, FreeType, `.ttc`, CFF and variable fonts are all Qt's work. ImGui's
default loader is stb_truetype, which handles TrueType `glyf` outlines only and
**aborts** on the system's default CJK face (`NotoSansCJK-VF.ttc` →
`imstb_truetype.h:1590: stbtt_FindGlyphIndex: Assertion '0' failed`). So the
overlay installs its own loader through the public `ImFontAtlas::SetFontLoader`
(`imgui.h:3688`): `gui::imgui_qt::installQtFontLoader` rasterizes through
`QRawFont` (`glyphIndexesForChars`/`alphaMapForGlyph`/`advancesForGlyphIndexes`/
`supportsCharacter`) and finds the CJK face with `QRawFont::fromFont(font,
WritingSystem)` - no fontconfig call, no font-file path, no new dependency.
1.92's dynamic atlas means only the glyphs actually drawn are rasterized, so a
CJK layer name costs its own glyphs, not a 2500-glyph range.

**Three facts the integration depends on** (measured, and each is a silent
failure otherwise):
- The CALLER must call `ImGui_ImplOpenGL3_NewFrame()` before `ImGui::NewFrame()`
  - it creates the backend's device objects on the first frame. Skipping it makes
  the first `RenderDrawData` segfault with every handle 0.
- ImGui must draw AFTER `ctrl::PainterHandle::end()`, which flushes the queued
  QPainter command batch. Drawing before it puts the panel under the overlay.
- GL state read back before and after `RenderDrawData` comes out identical, but
  the viewport is NOT restored, the scissor box is left set, and a temporary VAO
  is created per draw.

**The panel's design, as implemented** (`src/ctrl/GraphEditor.h` and
`src/gui/GraphPanelModel.h` are the two durable specs). The bullets below describe the OVERLAY host,
which is DELETED (SUPERSEDED 2026-10-18, `aa92de2a`) - read them as history; the docked host kept
every rule except the two marked:
- fullscreen over the animation, **no background and no grid**: short tick marks
  and labels only, so the artwork stays readable through the panel (OVERLAY
  only: the docked host clears an OPAQUE floor of its own before the frame, so
  there is nothing to read through - `GraphPanelWidget.cpp:132-150`);
- text carries a **halo**, which is what keeps it legible over arbitrary pixels;
- **hit-only ownership**: the graph takes the pointer only on a curve, a key or a
  handle (or while a drag is in flight), so the viewport tools keep working while
  the panel covers everything (OVERLAY only: the docked page IS the dock's surface
  for its tab and takes its presses directly). The WHEEL is the exception at the
  overlay, and deliberately so: while the graph is visible it zooms the graph;
  hiding the graph gives the wheel back to the canvas. The docked page reads
  `invertTimeLineScaling` rather than the main view's, because it sits on the
  timeline's own body;
- a **sticky focus** marks one channel (a press that hits a curve sets it, a press
  on empty space leaves it alone). It is emphasis only, and SUPERSEDED as such:
  since the selection round the INK follows the SELECTION (full strength for a
  channel that owns a selected marker, 0.45 for the rest, everything dimmed with
  nothing selected - see "The redesign as it now stands" below), and the focus
  keeps two roles, which channel's tangent handles are drawn and what the
  accessibility summary says. The value axis does not follow the focus and its fit
  is not pending either: a fit happens when the SHOWN SET changes or on an
  explicit route (see "Selecting never moves the view" further down);
- double-click on the plot fits both axes (`GraphEditor::fitAll`); the graph's
  frame axis is its OWN `ctrl::time::Scaler` (by value in `graph::Scaler`), so
  fitting it moves the graph's window and not the timeline's - while a timeline
  owns the frame axis (`setFrameWindow`) the frame half of a fit does NOTHING, so
  the two views cannot disagree about frames.

**The units and their gates.** `gui::GraphPanelModel` is the panel's content as
engine-neutral geometry (it makes no appearance decisions and mutates nothing);
`gui::ImGuiInputBridge` is the complete Qt→ImGuiIO translation (mouse, keys,
keypad, IME incl. the candidate-window caret, tablet, touch, DPR) and owns the
rule the application must obey: **a rebindable command must not fire while
`ImGuiInputBridge::keyboardCaptured()`** (= `io.WantCaptureKeyboard`);
`gui::a11y` publishes the panel to the screen-reader tree through QAccessible
(the ImGui content itself exposes nothing - a measured byte-identical AT-SPI dump
with the overlay on and off). `tests/imgui_qt_font_loader`,
`tests/imgui_input_bridge`, `tests/imgui_a11y` and `tests/graph_panel_model` are
their headless gates; `scripts/run_suites.sh` discovers targets from
`tests/CMakeLists.txt`, so registering one there is all that puts it on the gate.

**Trap: the ambient platform theme breaks offscreen tests after they PASS.** With
`QT_QPA_PLATFORMTHEME=cosmic` exported, `libcutecosmictheme.so`'s watcher thread
calls back into Qt after teardown and the process can exit 134/139 having already
printed its PASS (measured: 16 of 60 runs, with a pure-Qt control crashing 2 of
30). `scripts/run_suites.sh` therefore drops that variable on its offscreen
branch.

**What this is verified on, and what is not.** Everything above was MEASURED on
Linux/X11 under Xvfb, with Mesa **llvmpipe** - a software rasterizer, so the
timings are pessimistic and the painter/ImGui ratio is not verified on a GPU.
Also verified here and expected to hold elsewhere, because they do not depend on
this platform: the GL3 backend and its loader shim (they call through the same
`QOpenGLFunctions_4_0_Core` instance the rest of the renderer uses), the
FetchContent declaration with its offline override, and the Qt-backed font loader
- the most portable piece by construction, since `QRawFont::fromFont(font,
WritingSystem)` is Qt's documented cross-platform way to ask for the face it would
use for a script (no fontconfig call of ours). NOT verified anywhere: macOS and
Windows builds, a real IME session on a compositor, and anything that needs a real
compositor - the probe and the GL tests run under Xvfb, which is X11.

**Which renderer, precisely** (measured): this machine HAS a GPU - an AMD Radeon
R9 380 (radeonsi, 4.6 core) - and tests that create their own context DO get it
(`tests/graph_panel_pixels` reports it, so the contrast-outline shader's pixels are
GPU-verified, not software-verified). The APPLICATION under Xvfb does not: GLX
there is llvmpipe (`Accelerated: no`), which is why every application-level timing
here - the painter/ImGui A/B and the animation-load numbers - is software
rendering. Timing the real thing needs a real session, and nothing here may drive
the developer's own display, so those numbers stay llvmpipe-attributed and the GPU
ratio is unclaimed.

**Fractional DPI IS verified** (measured, not assumed): `imgui_qt_font_loader`,
`imgui_a11y` and `graph_panel_model` all pass headlessly at `QT_SCALE_FACTOR` 1.0,
1.25, 1.5 and 2.0, and the application draws the panel correctly at 1.25 and 1.5 -
captured, with the ticks, labels and playhead landing where they should.

**Trap: on a scaled session every AT-SPI-driven click misses.** QAccessible
rectangles - Qt's own and this application's published ones - are in LOGICAL
pixels, while `xdotool` moves the pointer in DEVICE pixels. Measured at
`QT_SCALE_FACTOR=1.25`: the graph toggle reports `8 88 24 24`, byte-identical to its
unscaled rect, so a probe that presses the reported centre lands 20% short and
nothing happens - while the panel is perfectly fine and the application logs no
warning. The probes are written for scale 1 and run under Xvfb that way; driving a
scaled session needs the rect multiplied by the scale factor, and the same care
for every measurement taken in pixels.

**Two repaint lessons, both measured on the key menu.** Neither is visible in a
single frame, which is why both were first reported as "the panel is broken".

*An event the overlay consumes for its own popup needs a successor FRAME, and only
the frame end can ask for one.* ImGui applies a queued press and the release that
follows it on two different frames (`io.ConfigInputTrickleEventQueue` is ImGui's
default), and ImGui's `Selectable` fires on the RELEASE - but a `requestRepaint()`
from an event handler is a `QWidget::update()`, and Qt coalesces every update
requested while a paint is pending into that one pending paint. So an event handler
can only ever ask for the frame it is already in, never for the frame that applies
what ImGui kept. The request therefore lives at the END of `paint()`: if ImGui still
holds queued input (a non-empty `ImGuiContext::InputEventsQueue`), ask for the next
frame. Measured before the fix: one click on a menu entry changed nothing at all -
the popup's 29271 px stayed on screen and the easing did not move - because the
release waited for a frame nobody had requested; clicking the entry TWICE worked,
which is exactly how a defect hides from the test written for it. `press()` and
`doubleClick()` do not need it (what they do is synchronous: the swatch toggle and
`fitAll` act on the editor's own state), and `mouseMove()` does not either, since it
already asks while the menu is open - that request *is* its frame.

*A panel state change is not a canvas change, so the panel's own scalers need the
repaint too.* The wheel over the visible graph changes `GraphEditor`'s scale;
`MainDisplayWidget::wheelEvent` used to return straight after
`Driver::updateGraphWheel` reported the take, and since nothing had changed on the
canvas no frame was scheduled: the new frames-per-pixel stayed invisible until
something unrelated repainted (the probe only saw its 798 px of panel change because
it forced a repaint cycle, and that nudge was the tell). The take is reported
*because* it is not a canvas action - and that is precisely the case that needs the
request, mirroring the cursor path (`updateCursor` -> `updateRender`). Every other
route measured does repaint already: a cursor pass, a key pushed from the timeline
(2162 px with no nudge), the graph's own drag and easing edit, a tree selection
change, and the visibility/channel toggles.

**The key menu's keyboard route needed the Qt focus, which this widget does not
have by default.** Keys reach the overlay through
`MainDisplayWidget::keyPressEvent`, and that widget carries `QWidget`'s default
`Qt::NoFocus` policy - it never sets one, so pressing in the viewport never makes
it the focus object. Measured with the overlay instrumented and the probe driving
the menu: **zero** key events arrived at `ImGuiOverlay::key()` in a whole run, and
the popup stayed up through Escape. The overlay therefore takes the Qt focus while
the menu is open and hands it back when the menu closes (only if it still holds
it) - the same move this file already makes for a Dear ImGui text field. Without
that step, a key handler that looks perfectly correct is never called at all.

ImGui's own popup keyboard behaviour is gated on
`ImGuiConfigFlags_NavEnableKeyboard`, which `NewFrame` reads per frame, so the flag
is set only while the menu is open and cleared otherwise: Escape closes the menu,
the arrows move its entry highlight and Return activates. That toggle is not
cosmetic - left on, ImGui wants the keyboard for the whole viewport, which is the
application's keys. With the menu closed they are untouched, and the probe asserts
it on the document rather than on a return value (ctrl+Z undoes the pick the menu
made; ctrl+Right still moves the playhead).

**Three gesture rules the panel learned from being driven by hand** (2026-10-13). Each
was a report of "this is missing/backwards" that turned out to be a design decision
worth writing down.

*The panel is a fullscreen overlay, so every press it claims is a press the tools
LOSE.* Its plot covers most of the viewport, and the viewport's own handles live
there, so a PLAIN left press in the plot has to stay the tools' - the graph takes a
plot press only with **Ctrl**, and then the curve's band decides which of its two
meanings it gets: on a band it inserts the key the curve already passes through, off
every band it opens the marquee. (SUPERSEDED 2026-10-14: the timeline's convention
won for the overlay too - a PLAIN press inside the plot is the key press or the
marquee, and Ctrl on a curve's band is still the insert; see `graph_drag`'s summary
line in the second-view section.) Claiming plain presses there instead stops every
tool gesture while the panel is up; that is a constraint, not a preference. Alt is
not available as the modifier either: the compositor grabs Alt+drag to move windows,
and the application binds `Alt+E`, `Alt+Q`, `Alt+1`, `Alt+3`.

*The wheel zooms BOTH axes about the cursor* (Shift narrows it to the frame axis,
Ctrl to the value axis), and its DIRECTION matches the canvas and the timeline - a
positive Qt angle delta zooms OUT, at 1.1 per notch. Making the axes agree meant
flipping the value axis, whose old sense was opposite the frame axis beside it and
opposite the canvas below it; two axes that zoom in opposite directions cannot be
made into one gesture that keeps its shape.

**The frame axis' zoom, and the graph panel's window onto it** - the rules, as they are:

- *The ladder.* `ctrl::time::Scaler` owns the frame axis' rate: `pixelsPerFrame() = 1.1 ^ index`
  (`kZoomPerNotch` is the application's own zoom step, the one `gui::CanvasMover` and the graph
  panel's value axis take), so one notch is a constant x1.1 at EVERY zoom and the index bounds
  (-120..120 = 1.07e-5 .. 93000 px/frame) are the only limits. `indexForRate` inverts it, floored, so
  a caller with a rate in hand gets the sparsest rung that still fits; `setIndex` addresses a rung
  directly (the delta arithmetic `update` uses is easy to get backwards).
- *The ticks.* `majorStep` picks the rung whose on-screen spacing is closest to 96px, DOUBLING past
  the ladder's coarsest rung when even that is below the 48px floor - so a deep zoom-out labels ever
  wider spans instead of going blank. At the other end a frame wider than a number
  (`spacing >= kNumberSpacing`) is itself a numbered major. `tickStride` is the finest level drawn,
  and the ruler's and the panel's loops iterate frames in strides of it: the visible span is
  viewport / spacing and spacing can be 1e-5, so a per-frame loop would be millions of iterations.
  `attribute(int)` publishes every position (ruler ticks and numbers, keys, range markers) from
  `pixelsPerFrame()`, never from the index.
- *The frames outside the keys*, which is the project's own playback rule
  (`graph::Curve::setLoop`): inside the frame range, loop ON continues the eased segment from the LAST
  key back to the FIRST, wrapped by the range's span (the period `core::TimeKeyGatherer` also uses);
  loop OFF holds the nearest key. OUTSIDE the frame range the frame is CLAMPED into it, so the curve
  is FLAT at the range's edge value - the animation is defined over the range, and the gatherer is
  never asked about those frames (the playback range clamps its own).
- *The panel's X is the timeline's window as VALUES, in FRACTIONAL frames.*
  `TimeLineWidget::visibleFrameWindow` gives the viewport's exact frame edges; `graph::Scaler::applyFrameWindow`
  maps them into the plot's inset exactly (a fractional `framePpf`), so the frame under the cursor
  keeps its fraction of the window and the timeline's own anchored zoom arrives already anchored.
  The wheel's frame half is delegated with the cursor's frame SUB-PIXEL. `graph::Scaler::fitFrameAxis`
  and the tick rung both choose their rate with `indexForRate`.
- *Direction.* The panel takes the wheel from the main view, so its zoom direction is
  `gui::MouseSetting::invertMainViewScaling`, applied to BOTH halves of the notch before the frame
  half is delegated; `TimeLineWidget::zoomFramesAbout` passes `invert = false` because its delta
  arrives in the caller's sense. The timeline's own wheel reads `invertTimeLineScaling`.
- *The view stays with the content.* `TimeLineWidget::updateCamera` bounds `mHorizontalOffset` so the
  view and the content OVERLAP: a pan may go anywhere, it just cannot leave the content behind
  entirely. (Centring instead would undo every pan, and `scrollFramesBy` carries a fractional
  remainder between calls so a drag's small deltas cannot round away and drop the view behind the
  pointer.)

Gates: `tests/time_scaler.cpp` (the ladder's x1.1 at every index, its bounds, `indexForRate`'s
inversion, `tickStride` agreeing with `attribute` over 42k drawn ticks, and that every zoom level
marks something inside a viewport); `tests/graph_curve.cpp` and `tests/graph_drag.cpp` for the
panel's own axis, its fit and its mapping (a half-frame push moves the content half a frame's
pixels; a zoom keeps the anchor's pixel; the delegate receives a sub-pixel anchor);
`scripts/gui/probe_graph_anchor.sh` end to end (a 40px middle-drag moves the playhead column 40px at
0.8 and at 6.2 px/frame, and a notch with the pointer on a frame leaves it within a pixel).

*Selecting never moves the view.* Focusing a channel is emphasis only. The value
window fits when the SHOWN SET changes (panel opened, targets changed, a channel
hidden - without that the panel opens blank) and on the explicit routes (double
click, the accessibility fit action). A click that refits the window is a defect: it
was one until this was fixed, and it reads as "clicking a curve zooms it".

**The panel's frame cost is its own vertex count, and an idle frame rate is what
proves where the cost is not** (measured 2026-10-13 on the R9 380 at 1400x860,
through the shipped `drawPanel` and the shipped pass, on the user's own report of
"sluggish whenever the panel is enabled"):

- A key costs **~68 vertices with its tangent handles drawn**, ~16 without, at ~50 ns
  of CPU per vertex in a **Debug** build - i.e. 2.6 ms per frame at 600 keys, **32 ms
  at 8,000** and 165 ms at 40,000, paid on every viewport repaint while the panel is
  up. Handles are now drawn for the FOCUSED channel only (measured 4.2x fewer
  vertices, 4.4x less frame time), and `framebufferPixels()` - which was ~10 GL
  queries per frame - is cached (0.10-0.13 ms).
- FALSIFIED, all of it measured, so nobody re-chases these: there is **no
  self-driving repaint loop** (0 paints/s and 0.00 % of a core at IDLE with the panel
  ON, on llvmpipe and on radeonsi); the **accessibility publish** is ~10 us/frame; the
  **outline pass** is 0.20 ms of GPU at 1400x860 (GL timer queries) and only looks
  catastrophic on software GL (11.83 ms llvmpipe, 30.77 ms at 2560x1440).
- Two lessons for the next performance report: measure the IDLE frame rate with the
  panel OFF and ON first - that number separates "a repaint loop" from "an expensive
  frame" - and never quote a Debug-build number as if it were the application's. The
  application the user drives is a Debug binary unless someone builds Release.

**ImGui's antialiasing is two paths, and a FRACTIONAL stroke width picks the bad
one** (measured 2026-10-13). The curve ink was `AddPolyline(..., 1.5f)`: the draw
list already carried `ImDrawListFlags_AntiAliasedLinesUseTex` (flags 0xF), but 1.5
has a fractional part, and that is what keeps a polyline OFF ImGui's texture-AA path
and ON the geometry path - a 0.25px opaque core under a 1px ramp on each side, built
from per-segment quads joined by averaged normals. At `kCurveWidth = 2.0f` the same
stroke takes the texture path, and the pixel gate's own numbers say so exactly: 304
vtx / 804 idx -> 286 / 720, the 4->2 vertices per point and 18->6 indices per segment
of that path; the per-column profile goes from (0.58,1.00,0.13) to (0.83,1.00,0.38),
i.e. a full-pixel opaque core; ink per column 1.71 -> 2.21, constant to three decimals
over 41 columns. So "the graph is jagged" is NOT multisampling (measured separately:
the curve row is pixel-identical with the widget's 4x MSAA on and off) and NOT the
fringe scale (setting `_FringeScale` to the stroke width measured a 0.85 peak with no
opaque core - softer, not sharper). It is that one fractional number. A 1px LINE has
the same trap from the other end: ImGui's non-AA path is half-open, so the playhead
lost its first row until it was drawn from `top()-0.5` to `bottom()+0.5`.

**The panel's top must know what floats over the viewport.** The project tab bar is
drawn ON TOP of the viewport's first ~20 logical pixels, and a fullscreen panel draws
underneath it: the ruler numbers landed behind the tabs, which reads exactly like "the
X axis numbers are clipped at the top" while the label box itself is perfectly
correct - so the fix is not in the label layout. `MainDisplayWidget::paintEvent` now
hands the overlay `setTopInset()` on every frame (the tab bar's bottom edge in the
widget's own coordinates, in LOGICAL pixels, clamped to 0 when the bar sits above the
widget rather than over it), and the model moves only the ruler band - the ticks, the
frame numbers and any value label that would fall under it. SUPERSEDED (2026-10-18, `aa92de2a`):
the overlay host and its `setTopInset` are gone (`grep` returns 0), and the docked page keeps
`PanelFrameState::topInset` at 0 because nothing floats over the dock (`GraphPanelFrame.h:416`,
`GraphPanelWidget.h:213`) - the field and the clamp stay for a host that needs them. It moves NOT the
plot, the curves, the keys or the hit-test geometry: those are the editor's own rect
(`GraphEditor::panelRect`), and drawing them anywhere else would put the key a user
sees a tab bar's height away from the key a press grabs.

**The key menu's style is derived from the application palette, not hand-set**:
`theme::Graph` carries the menu tokens (body, edge, text, textDisabled, hover,
hoverText, separator, radii 8/4, paddings) taken from the app's own QMenu decisions in
`AppStyle`; `gui::applyPanelStyle` copies them into `ImGuiStyle`, and `paint()`
re-applies them from `theme::Colors::current()` before every NewFrame - so a runtime
accent or light/dark change tracks with no signal and no cache (pinned by re-deriving
the style from a second palette). The rounded hover pill is drawn by hand, because
ImGui's Selectable fill is a square with no rounding knob. Not expressible, and
approximated: Qt's font-relative paddings, item-pill rounding on ImGui's own nav
cursor, and the app's 8px separator inset.

**A themed popup changes FEWER pixels than a default-styled one, so a fraction cannot
be the test** (measured 2026-10-13). The probe's menu search judged a candidate point
by requiring 60% of the popup's expected rectangle to change. That held while the
popup was ImGui's default panel against artwork - and it rejected every candidate on
the first run after the popup's body became the application's own surface colour
(`theme::Graph::menuBody()` through `gui::applyPanelStyle`): the real menu changes
17165 px = 58.6%, so a probe failed while the product was working perfectly. The
replacement is a SHAPE rule - bigger than 60x60, inside the viewport, at least 3000
pixels changed - shared by the scan and the anchor search. The general form: when a
pixel test's bar is a fraction of something the THEME controls, the theme can move the
bar out from under it. (The same run also corrected a wrong attribution I had made
from a single state: the key really was at the fit centre in the probe's flow, because
the phase activates the published "Fit graph" action first - so a measurement taken in
one state is not a measurement of the gate's state.)

## Where the existing GUI tooling lives

Extend these instead of writing new one-off scripts:

- `scripts/gui/_lib.sh` — `ae_press`, `ae_activate`, `ae_report`, `assert_showing`/`assert_hidden`,
  the Xvfb/`QT_QPA_PLATFORM` setup, the settings sandbox.
- `scripts/gui/ae_ui.py` — the AT-SPI client: `click`, `set`, `value`, `text`, `report`, `wait`,
  `extents`, `dump`, addressing controls **by name**.
- `scripts/gui/ae_pix.py` — the raw-crop scanner: `line` (the column holding the most pixels of
  one colour) and `moved` (the colour that tops a column in two shots at a different column in
  each). It is how `probe_graph_anchor.sh` finds the graph panel's playhead without baking in a
  theme colour; `magick shot.png -crop WxH+X+Y +repage -depth 8 rgb:file` makes its input.
- `scripts/gui/run_probes.sh` and `scripts/gui/probe_*.sh` — the end-to-end checks; run one probe
  directly while iterating, the runner for the whole suite.
- `tests/` — headless unit tests (`QT_QPA_PLATFORM=offscreen`), including `export_form` and
  `destination_picker` for the export dialogs, and `picker_keyboard` for the key routing inside them,
  and `treeutil_delete_uaf` for the use-after-free write `util::TreeUtil::deleteAll` used to make in
  the pose tool's "Pull bones" shadow tree (a `Node` subclasses the real template with an allocator
  that poisons a block on delete and never returns it, so the invariant "nothing writes into a
  retired node" is checked without a sanitizer; it is the gate for that fix).
  `tests/pose_pull_crash` is the companion reproduction tool, not a gate: it needs a `.anie`
  argument, drives the real `ctrl::Driver` with `PoseEditMode_Draw` and synthetic cursor frames, and
  aborts on the pre-fix code with `free(): chunks in smallbin corrupted` from
  `BoneDynamics::deleteAll`; run it under `xvfb-run` (it needs a GL context) — see its header comment.

## Traps for anything that searches, lays out or drives a migrated QML scene

Every one of these cost real time on the General settings / Key bindings / Project settings ports
and the viewport-background and timeline-marquee work (2026-10-11), every one is silent, and each
looks like "the application is broken" when the application is fine.

- **A `Repeater`'s delegates are NOT under the root's QObject tree.** They are parented into the
  VISUAL tree (the `Row`/`RowLayout` that lays them out), so `root->findChildren<QObject*>()` — the
  search every scene test here started with — returns none of them. Measured: the tab strip's
  Repeater reported `count=4` while a QObject-only walk found zero of its four buttons, and the same
  walk missed the theme group and the accent swatches. A test that walks only `findChildren` reports
  them ABSENT while the dialog renders them perfectly. Walk `QQuickItem::childItems()` as well
  (`tests/general_setting_form.cpp`'s `collectItems` is the helper).
- **A zero-height item still PAINTS its children.** A delegate that hides a row with `height: 0`
  (the idiom for "one list over every group's rows, show only this tab's") leaves the row's children
  overflowing a zero-height parent, and they draw — measured on the running dialog, where the General
  tab showed two rows of overlapping, unreadable text at the y the collapsed Timeline and Tools rows
  sat at. `height: 0` is for the LAYOUT; `visible: false` is what removes it from the PAINT. (The
  inverse trap is in the same family: a `visible: false` control is reported HIDDEN by the probes,
  not absent, when its ancestors are showing — assert the state the design actually produces.)
- **`ae_memory_cap` had to be in `_lib.sh`'s `export -f` list.** `wait_ready` calls it from inside
  the `bash -c` body `xvfb-run` runs, and it was defined and never exported, so every probe printed
  `environment: line 3: ae_memory_cap: command not found` and the resident-memory ceiling the file's
  own comment block promises was applied to no probe at all. A function a child shell must call has
  to be exported, like `ae_report` and `wait_ready` beside it.
- **A `modal: true` popup overlay swallows the press that lands outside it — including the press on
  the control's OWN field.** `AeComboBox` set `modal: true; dim: false` to copy the pre-port
  control's scrim, and the cost was two reported defects: pressing the field's arrow did not close
  the open list, and pressing a second combo only closed the first. `modal: false` is the fix and
  needs no custom press handling — Qt's own `ComboBox` toggles correctly once the press reaches it
  (verified by a mouse-level probe, `scripts/gui/probe_settings.sh`). What is NOT a defect, and
  reads as one when two combos are stacked ~36px apart: an open list COVERS the fields below it, so
  a press there lands on a list row and chooses it. Assert pass-through in the direction where the
  list does not cover the target, or you will chase a geometry fact as a popup bug.
- **An apostrophe inside a `bash -c '…'` probe body ends the body there.** The body is one
  single-quoted string, so a comment like `# the application's route` closes it: everything after
  runs in the OUTER script — which has the developer's real `DISPLAY` — and `bash -c` reports
  `unexpected EOF while looking for matching '"'` while the probe appears to keep going. Measured
  2026-10-11: `DISPLAY` went from the Xvfb's `:101` to `:0` mid-probe, the Xvfb died with its last
  client, and only `ae_guard_display` stopped the clicks from landing on the live desktop. `bash -n`
  does NOT catch it. After editing a body, grep it for `'` (the `'"$SHOTS"'` interpolation is the
  only legitimate one).
- **`ae set` writes through the Value interface, which a `QLineEdit` does not have.** `ae set
  "HTML:" "#3366cc"` is refused ("no widget named … that holds a value") and the probe carries on,
  so a colour chooser driven that way silently returns its original colour. The stock `QColorDialog`
  offers a route a probe can actually use: press **Pick Screen Color**, then click the pixel whose
  colour you want — measured 2026-10-11, the canvas repainted the picked colour and one undo put the
  theme colour back (`scripts/gui/probe_projectsettings.sh`).
- **The `.anie` minor version gates more than the field you are adding.** Patching a header to claim
  a newer version on an older file makes the OBJECT TREE fail to load (`Failed to read object tree
  block … failed to deserialize node`), because other structures are version-gated too. A fixture
  that carries a new field has to be written by code that knows the whole format — the app, or
  `tests/roundtrip_main.cpp` — never by editing bytes in an existing project.
- **`run_probes.sh` globs `probe_*.sh`.** A new probe needs no registration; the only thing worth
  adding there is a `PROBE_TIMEOUT` override if it needs more than the 900 s default.
- **The export picker's search field does not answer `ae value`/`ae set` as a value-bearing control.**
  Measured 2026-10-11 while building `probe_exportbg.sh`: `ae value "Search destinations"` prints
  empty, `ae set "Search destinations" "discord"` left the list at 134 `list item` nodes (i.e. still
  everything), and the same call with `"twitch"` left 0. So a probe that reaches a destination by
  SEARCHING is not verifying what it thinks. Reach a row without the filter — the manual rows
  ("Video", "Animated image", "Image sequence", "Archive file (highest quality)") are at the top of
  the list — or `ae_press` the row (a real click at its centre lands inside it even when the name is
  reported by two nested nodes, which is why `ae_activate "Twitch emote" … "list item"` fails with
  "no accessibility action": the client picks the node that has none).
- **A transient view that a drag repaints on every move must be taken down by an explicit repaint.**
  `TimeLineEditor`'s marquee release cleared the state but asked for no view update, so the box's
  pixels stayed on the surface until something unrelated repainted the timeline — the reported "the
  box sometimes stays after I release". Clearing state and repainting are two different duties.
- **`AeFieldLabel`'s lane is content-sized, not a fixed 104px.** The export form's short labels fit
  104 and the settings dialog's long ones ("Language (needs restart)", "Global keybind delay (ms)",
  "Time (in minutes) between autosaves") were ellipsized while the field beside them sat half empty.
  `Layout.preferredWidth: Math.max(104, implicitWidth)` keeps the measured alignment of the short
  forms and widens the whole column for a long label. Note that `closePolicy` on a `ComboBox`'s
  popup is NOT yours to set: the C++ control re-applies `CloseOnEscape | CloseOnReleaseOutsideParent`
  (17) after the `popup:` block runs, so a QML `closePolicy:` there is silently overwritten.

## The animation-graph panel is a second view of the timeline (2026-10-14)

The round that made the panel behave like a tool of this application rather than one floating over it.
Every line below was measured; the cells marked MEASURED carry the number that decided the change.

**One selection, shared.** `ctrl::GraphEditor` used to keep its own `core::TimeLineEvent`. It now holds a
`time::Selection*` (`setSelectionSource`), and every selection method goes through that one pointer - the
panel owns an instance only while no timeline is attached (the unit tests' path). `ctrl::TimeLineEditor::selection()`
is the live set, and `Selection::rebuildFocus()` is what makes the TIMELINE renderer highlight those keys
(`TimeKey::isFocus`, read by `ctrl::time::Renderer`). Wiring, all values/delegates so no `ctrl/` -> `gui/`
include appears: `MainWindow` -> `MainDisplayWidget::setTimeLineWidget` -> `syncGraphPanelSharedState()`,
which runs on `setDriver` (the driver and its `GraphEditor` are created per project) and on every timeline
view-range change.

**The marquee is a PLAIN drag.** The Ctrl gate existed to keep the viewport tools' presses alive inside the
fullscreen plot; the timeline's convention won. Ctrl+press on a curve's band is still the insert. `graph_drag`'s
own summary line states the rule: *"a LEFT PRESS inside the plot (a key selects and drags, Ctrl on a curve's
band inserts, everything else marquees)"*.

**The X axis is the timeline's.** The panel's frame window is PUSHED IN (`GraphEditor::setFrameWindow`) from
`gui::TimeLineWidget::visibleFrameRange()`; while shared, `fitFrameAxis()` is a no-op and the wheel's frame
half and a middle-drag's horizontal half are DELEGATED (`scrollFramesBy` / `zoomFramesAbout`), so the two views
cannot disagree about frames. Unattached, the panel keeps its own X (that is the path `graph_drag`'s sections
0-22 exercise). Two consequences paid for here:

- **`GraphEditor::kFrameWindowMargin = 8`.** Pushing the timeline's window in put frame `min` back on
  `plot.left()`, and a key there was drawn half-clipped and picked off-centre - MEASURED in a 6x crop of a real
  capture: the marker showed only a half-disc, and because a selected key is drawn in `theme::Graph::selected()`,
  the same violet as the playhead, it read as nothing at all. 8 = the hit square's `+-kKeyRadius` (5) plus 2px
  of integer-origin/rounding slack, +1. The timeline solves the same problem with `kTimeLineMargin = 24`.
- Nothing is hidden by the inset: `ppf*span <= plotWidth - 2M`, so the whole pushed range stays inside the plot.

**Visuals and hit geometry are separate - the timeline's own lesson, applied.** `ctrl::graph::kKeyDotRadius`
(= 3.75, the timeline's own `ctrl::keyRadius` from `time_Renderer.h`) is what is DRAWN; the pick is
`kKeyHitRadius` (10) in `GraphEditor::hitTest`, and the right-click menu now uses that same 10px rule instead
of testing the drawn rect. `kKeyRadius`/`keyRect` are documented as HIT geometry only, and `PanelCurve::keyDots`
publishes the circles. MEASURED before: the panel painted a 10px square, 1.33x the timeline's key body across
and 2.3x its area.

**The key menu is the application's own `QMenu`.** SUPERSEDED (2026-10-17): the panel's right-click now
opens the EASING PICKER at the pointer, not a menu - `gui::TimeLineWidget::showKeyEasingPicker(targets,
globalPos)` (`TimeLineWidget.cpp:756-761`), reached from the panel as `ImGuiOverlay::openKeyMenuAt` ->
`MainDisplayWidget::showGraphKeyMenu` (`MainDisplayWidget.cpp:377-381`), and the 14-row flat
`mSelectEasing` QMenu the timeline's right-click used to build is DELETED with it (see "The redesign as
it now stands"). What the QMenu paragraph below established still holds for the two menus that remain
(the channel list and, in it, the same route). The ImGui popup, its
`ImGuiConfigFlags_NavEnableKeyboard` toggle, `SetNextFrameWantCaptureKeyboard` and the Qt focus take/hand-back
are DELETED (grep returns 0). An ImGui popup can never look like a `QMenu`, and its keyboard capture was also
what broke typing into the dock after the menu had been used ("phase 4b"): that defect is gone with it. Two
consequences worth knowing: a Qt menu is MODAL (the app's Delete/Undo cannot reach the timeline while it is
open), and it is a top-level window, so its pixels are NOT in the application window's screenshot -
`probe_graph_overlay.sh`'s old menu phase scanned for them and had to be re-derived.

Removing the ImGui popup ALSO cured the long-standing "phase 4b" red, and this is the measured
attribution: typing into the dock's Angle field used to stop committing once the key menu had been
used, and the cause was the popup's own keyboard machinery (`ImGuiConfigFlags_NavEnableKeyboard`,
`SetNextFrameWantCaptureKeyboard`, the Qt focus take), not the menu. MEASURED 2026-10-14: a typed
write commits after an Escape-closed menu, after a pointer-picked entry and after a Return-closed
menu. ONE residual, measured by the same pass and NOT yet fixed: after the **graph's own Ctrl-press
deselect in the plot** - the selection-toggle branch in `GraphEditor`, the gesture the plain-drag
marquee round introduced - a typed write into the dock does not commit for the rest of the session,
while the field still reports AT-SPI focused-and-editable and the X modifier state and the input
focus are both clean, so it is not a harness artefact. The probe records its reproduction beside the
phase that exercises it; it needs a fix in the panel's press routing and a real run to verify.

**The numerals were 1.298x too large.** `fontPixelSize()` used `QFontMetricsF(widget.font()).height()` - the
LINE height, 15.578125px - as the atlas's pixel size, while Qt rasterizes the same face at 12px (pointSize 9
at 96dpi). `imgui_qt::panelFontPixelSize(font, logicalDpi)` (pointSizeF*dpi/72, or `pixelSize`, 0 when the
font states neither) is the fix; the timeline sets no font at all, so its ruler numbers are the app font's own
12px. Digits measured EQUAL in advance at every size probed (7.187500 at 12px for all ten), so no `tnum`
request is needed - the bundled face already has tabular digits. The label colour moved from `textMuted` to
`Colors::text`, the token the timeline's ruler uses: `textMuted` falls BELOW `outline::kMinContrast` exactly
where the labels land over mid-grey artwork (1.20:1) and can then only be ringed black, never brightened.

**The label column is FORMAT-derived, never window-derived.** `Scaler::axisLabelWidth()` measures the widest
label the `'g'/4` format can emit (62px: `-3.403e+38`, `-0.0009999`; a float-range sweep agrees), adds the box
inset and the text gap, and clamps into [46, 96] - a 68px column. It must NOT be measured from the labels
currently visible: doing that made `plot.left()` move with the value window, MEASURED as a 40px middle-drag pan
moving `pixelX(frame 0)` by 41 instead of 40. The application's precedent is the timeline's own:
`TimeRuler.cpp:103` reserves `numberWidth = 6` per character rather than measuring.

**The curve stroke is a coverage pass, and the interesting part is what it did NOT change.** `gui::stroke`
draws each segment as a quad expanded to halfWidth+filter+1px and computes coverage per fragment from the
distance to the segment's centreline (Chan & Durand 2005; Rougier, *Shader-Based Antialiased, Dashed, Stroked
Polylines*, JCGT 2(2) 2013, `jcgt.org/published/0002/02/08`) - the family Vello implements. MEASURED, and this
corrects a claim that had been taken for granted here: ImGui's own texture-AA polyline is ALREADY area-exact
and angle-independent at 2px (integrated perpendicular width spread 0.053px across four angles, identical
before and after; the whole frame differs in 27 pixels, 25 of them caps). What the pass actually fixes:

- the JOIN SHOULDER: the fringe stroke puts 21% more ink at a corner than one capsule can explain (+0.093
  excess against a 0.357 bound, versus +0.000 for the pass). This is the assertion a geometry/fringe
  implementation cannot pass.
- the END CAP: a round cap is 0.793 coverage where the fringe stroke ends square and paints nothing.
- SUB-PIXEL WIDTHS: below one device pixel the stroke keeps a 1px footprint with its alpha scaled instead of
  thinning away.
- DPR: the filter is one DEVICE pixel (`uPxScale`), where ImGui's fringe is one LOGICAL pixel by construction
  (`InitialFringeScale = 1.0f`, imgui.cpp's own FIXME-DPI).
- the TANGENT HANDLE STEM: `AddLine` shifts its geometry by (+0.5,+0.5), MEASURED 0.68-0.76px off-position
  along the stem's own normal with 0.90-1.10px of ink per px of stem; through the pass it is +-0.01px and 0.95.

Overlap is accumulated with `GL_MAX` into a framebuffer-sized scratch and composited once per curve in the
overlay FBO's premultiplied convention, so a join cannot double-darken a dimmed (`kUnfocusedAlpha`) curve.

**The 1px chrome is deliberately NOT anti-aliased, and that is pinned.** The playhead, the frame and value
ticks and the zero rule are INTEGER positions in the model (`frameTickX`/`valueTickY`/`zeroY`/`playheadX` are
`int`), so they land exactly on a column at full coverage with zero neighbours; routing them through the
coverage pass splits a boundary line 0.500/0.500 into two grey columns. The gate asserts both, so the
"obvious" AA fix cannot be applied later by mistake. A fractional playhead would be a MODEL decision
(GraphPanelModel quantizes the frame to a whole column), not a renderer one.

**`outline::kMinContrast` reads INVERTED:** the shader rings where contrast is BELOW the constant, so a higher
number rings MORE. It is 2.0f now (was 3.0f) because the user's report was the opposite of what the name
suggests - the ring was showing up everywhere.

## The redesign as it now stands: the dock header, the Graph tab, the marker selection (2026-10-18)

Ten commits (`7f683ba3..9ba5edbc`) landed the approved timeline + animation-panel redesign. Where a
line above describes the overlay-only world, this section is the current one and the older line is
marked SUPERSEDED in place. THREE commits have landed since this section was written - `aa92de2a`
(the overlay host deleted, the frame driver renamed), `46978a29` (the axis hue table, the visibility
state deleted) and `73ba0919` (the 14 strings extracted and translated) - and what each changes is
marked in place, in the paragraph it changes.

**The dock header is 40px and ACROSS the dock** (`gui::TimeLineHeader`, `TimeLineHeader.cpp:25`
`kHeaderHeight`), above the ruler and spanning tree + surface:
`tabs | transport | frame readout | easing chip | stretch | zoom` (`TimeLineHeader.h:36-38`), with a
1x16 rule between three of the groups. The transport is the existing eight-button `PlayBackWidget`
group moved out of its fixed 32px column, at 28px buttons with 20px icons (the column's 24px/18px read
small in a horizontal bar: when the buttons were stacked, the button was the whole target); the QSS
pins the same 28px box, because a restyle resizes absolute children. The frame readout is `120 / 660`
- the frame a key lands on and the range it is clamped to - tabular 13px bold / 11px muted. Zoom is
- / + / fit driving `ctrl::time::Scaler`'s own ladder, so a button and a wheel notch are the same call
on the same rung, and the fit composes the timeline's own primitives (`indexForRate`,
`zoomFramesAbout`, `scrollFramesBy`) because the frame axis has no fit of its own. The DOCK gains the
bar's height rather than the lanes losing it: 296 instead of the old 256 (`MainWindow.cpp:165-175`),
with the ruler at 22px, a lane band at 22px and the tree column unchanged.

**What the header deliberately does NOT hold**: the panel's channel chips (the mock's first draft put
them beside the zoom group) and any graph toggle - the toggle would repeat the dock's own tab and the
chips duplicated the panel's grouped legend, so one control lives in one place
(`TimeLineHeader.cpp:190-192` records the trim). The panel's OWN control (the "N channels" button) is
not here either: the docked panel draws it in its own ruler band (`TimeLineHeader.h:41-44`).

**`Timeline | Graph` is a real `QTabBar`** in the header's first group, whose tab order IS
`Surface`'s own (Timeline, Graph), so an index maps back by position and the two cannot drift
(`TimeLineHeader.cpp:120-133`): keyboard left/right, hover and the selected state all come from Qt.
ONE dock, two tabs, ONE header - the transport, readout, chip and zoom stay put in both, and the tab
names the page, so the Graph page carries no title and no header row of its own. `TargetWidget` keeps
`ObjectTree | QStackedWidget` (`TargetWidget.cpp:44-57`): the TREE stays in both tabs, because it
names the layers and picks whose channels the graph draws. Becoming visible is the one moment the
panel re-reads the document and the frame (`TargetWidget.cpp:127-136`; Qt lays out and paints only
the current page). With `AE_ENABLE_IMGUI=OFF` there is no graph page, no Graph tab and no accessor a
caller could use - the whole panel unit set is registered inside `if(AE_ENABLE_IMGUI)`
(`src/gui/CMakeLists.txt:230-233`), and the header simply has no tabs.

**The docked panel is the SAME panel, on a SECOND host.** SUPERSEDED (2026-10-18, `aa92de2a`): the
overlay host is DELETED and `gui::GraphPanelWidget` is the panel's ONLY host. The frame driver is
still `gui::runPanelFrame` (`GraphPanelFrame.h:459`), but the contrast-outline composite stopped
being drawn: the pass survives because the headless pixel gate drives `gui::outline::composite`
directly (`tests/graph_panel_pixels.cpp:3510`), while the application's only caller of `runPanelFrame`
is the docked widget (`GraphPanelWidget.cpp:175`). `GraphPanelHost::Overlay` survives too, as the
model builder's DEFAULT host style (`GraphPanelModel.h:698,717`), which is what every harness gets
unless it asks for `Dock`. What follows describes how the two hosts related while both existed.
`gui::GraphPanelWidget` is a
`QOpenGLWidget` pinned over the dock's opaque floor (`theme::Colors::base`, cleared per frame -
`GraphPanelWidget.cpp:132-150`); the model lays out for `GraphPanelHost::Dock`, which is what puts
its one control in the plot's corner and suppresses the overlay's contrast-outline composite.
NOTHING about the panel is re-implemented: `ImGuiOverlay::paint` used to BE the frame and is now one
host among two, and both call `gui::runPanelFrame(...)` (`ImGuiOverlay.h:433-460`) - the ONE ImGui
context + GL3 backend + Qt font atlas (refcounted; the LAST release tears them down with a current
context), each frame setting `io.DisplaySize`/`DisplayFramebufferScale` from THIS host's size and
DPR and running `ImGui_ImplOpenGL3_NewFrame -> NewFrame -> stroke::init -> buildGraphPanelModel ->
drawPanel -> ImGui::Render` (`ImGuiOverlay.cpp:2382-2441`). The hosts differ in exactly one place:
the docked widget renders the returned draw data straight into its framebuffer, the overlay draws it
behind its contrast-outline composite. The atlas is built from whichever host acquires first, so
both draw the application face. `PanelFrameState` is per host, so a host's own hit tests and the
accessible items are tested against the geometry THAT host drew (`ImGuiOverlay.h:396-416`).

**The GL/ImGui sharing is CHECKED, not assumed.** The dock's page and the viewport are both children
of `MainWindow`, so Qt puts them in one context group sharing one default format
(`QSurfaceFormat::setDefaultFormat`,
`MainWindow.cpp:69-73`). A second host on its own context would need a second backend, which the
design refuses - one ImGui context, one backend, one atlas, whatever draws. So instead the panel
records the `QOpenGLContext` its objects were created against (`gPanelOwnerContext`,
`GraphPanelFrame.cpp:2116-2125`) and, on a frame whose host draws with a different one, tears the
objects down ON THE CURRENT CONTEXT and rebuilds them there (`rebindPanelObjects`,
`GraphPanelFrame.cpp:2206-2227`; called from
`runPanelFrame` at `:2282`), warning ONCE with the cause - "(vertex-array objects are not shared
between contexts, even in one window)" (`:2219-2223`). One host means the rebuild never fires in the
application; it is the guard that keeps a second host from reintroducing the VAO crash. Note that no
`QOpenGLContext::areSharing` call exists anywhere in the tree (`grep` returns 0) - the pointer
compare plus the rebuild IS the guard, not an are-they-sharing assertion.

**The selection is the CHANNEL MARKER, and that is the user's ruling** (see the contradictions
section below). `ctrl::time::Selection` carries both granularities in ONE container
(`time_Selection.h:15-46`): an entry names either a WHOLE key (`kKeyLevel` - every timeline gesture,
the marquee and ctrl+A write these, and such an entry covers EVERY marker its key owns) or ONE
component (`>= 0` - what a press on one curve writes). One identity - `(line, type, frame)` plus the
component - rather than two sets that would have to agree, and `assign()` still emits ONE target per
key, so every existing consumer (the picker, the timeline's key commands) keeps writing each key
once.

- A plain press selects the marker under the pointer, Shift adds one, Ctrl toggles. The marquee
  keeps its rule but writes whole keys, and under Shift it first collapses an enclosed key's partial
  entries, so an enclosed key becomes whole (`GraphEditor.h:250-270`).
- THE DRAG SPLITS BY AXIS. Horizontal moves each selected key ONCE - the frame is shared across a
  key's components, so a diagonal drag on both of a Scale key's markers moves the frame by ONE
  delta, not two (`tests/graph_drag.cpp`'s section 23e is the gate). Vertical writes every selected
  marker on ITS OWN component: a marker that was not selected receives no member and comes out
  untouched, so selecting `Scale X` alone and dragging up moves X and leaves Y. Still one
  `cmnd::Gesture<KeyMoveCommand>` per gesture, so still one undo step (`GraphEditor.h:47,56,123,531`).
- Ctrl+A (`selectAllMarkers`) selects every marker of the SHOWN channels' keys, written as one
  whole-key entry per key so a type whose two components are both shown cannot double it; Escape
  (`clearSelection`) empties the set. Both ANSWER whether they acted (`GraphEditor.h:305-324`). THE
  KEYS ARE NOT BOUND IN THIS TREE: this class is fed cursor and wheel events only, and its two
  keyboard actions are called by whatever owns the application's key map (`GraphEditor.h:275-300`);
  `selectAllMarkers` has no caller outside the editor and `tests/graph_drag.cpp:3262`.
- The selection ANNOUNCES itself (`Selection::onChanged`, `time_Selection.h:33-35,65,124-127`). The
  header chip subscribes to it, replacing the 200 ms polling timer it had been given as a workaround,
  so the chip no longer depends on the timeline surface painting to stay current (verified: no QTimer
  and no `kChipSyncIntervalMs` left in `TimeLineEditorWidget.cpp`).

**Emphasis follows the SELECTION, not the sticky focus.** A channel that owns at least one selected
marker draws at full strength, every other curve at 0.45, and with an EMPTY selection everything is
dimmed (`curveStrength`, `ImGuiOverlay.cpp:395-412`, `kUnselectedAlpha` `:158`). The model publishes
the fact this folds over: a channel is IN PLAY when it owns a SELECTED marker or the same KEY is
selected on a sibling channel (`keySelected`/`keySiblingSelected`, `GraphPanelModel.h:181-195`),
which is what keeps `Scale Y` at full strength when only `Scale X` is selected. The sticky focus
still decides which channel's tangent handles are drawn (the panel's cost rule) and what the a11y
summary says - it no longer decides the ink.

**The ruler is ONE ink with ALPHA TIERS, and the MODEL carries each tick's level.** The panel used to
draw hairline ticks under text numbers in two different tokens; it now takes the timeline's rule: ONE
token, prominence by ALPHA (0.85 major / 0.60 intermediate / 0.20 minor), one length for every tier
(`ImGuiOverlay.cpp:113-136`). The tier is NOT recovered from a pixel measurement: the model carries
each frame tick's own `ctrl::time::Scaler::TickLevel` (`time_Scaler.h:21`) in `frameTickLevel`,
alongside its position, same order and same length (`GraphPanelModel.h:316-322,635-638`), so the
drawing layer applies the tier it is told.

**The painted legend is gone: a chip per live/hovered curve, and one channels control.** The stacked
bottom-left box (10px swatch, `"layer : channel"`) overlapped the value labels and the zero rule. Its
replacement is a chip at each curve's own end in the channel's hue, for the channels that own a
selected key AND for whatever the pointer is over - the subset being edited, and nothing else
(`ImGuiOverlay.cpp:1806-1837`). ONE "N channels" button opens the list for visibility and focus
through the application's own QMenu (the key menu's route), with a hue swatch per row; its label is
the total while every channel is shown and `shown of total` once one is hidden, and with no channels
there is no control (`GraphPanelModel.cpp:288-305`). The control has TWO placements chosen by
`GraphPanelHost`: in the overlay 8px inside the PANEL's right edge (the reference's floating rule);
docked, 8px inside the PLOT's right edge, because the docked page is drawn edge to edge and its own
surface is the plot (`GraphPanelModel.cpp:316-340`). Either way it hangs BELOW the band's whole block
(the `kFrameTickLength` tick rows plus the numbers' line), with a model-level precondition that the
box and the band cannot intersect (`GraphPanelModel.cpp:342-350`).

**One channel hue table, and the rule is "axis where there is an axis, type otherwise".** Per-TYPE
colouring could not answer "which channel is this": Move X and Move Y shared one hue, Scale X and
Scale Y too, Blur's three too, and HSV reused Move's indigo while Blur reused Scale's fuchsia. The
assignment now lives in ONE table both views read - `theme::channelAxis(type, component)` and
`theme::channelColor(...)` (`theme/Colors.h:231-285`, `:597-604`, with `kChannelAxisRow` at `:262`
taking the palette's own rose / green / sky rows, dark `#fb7185` / `#4ade80` / `#38bdf8`). A
component that lies on an axis takes that axis's hue - the first and second component of Move,
Scale, Image and Blur's two radii - and every other channel keeps its type's hue, so Opa stays
amber, Depth and the two angles keep their type's hue, and no graphable channel is a Z today (the Z
cell is stated for the convention's completeness). `tests/graph_panel_model.cpp:893-904` fails if an
axis is ever assigned to an angle or to Depth. THE TIMELINE READS THE SAME TABLE: a timeline key
belongs to the TYPE, so `ctrl::time::Renderer` draws it in `channelColor(colors, type, 0)` - the X
hue for those four 2D types (`time_Renderer.cpp:51`) - and its lane label and its chip in the same
hue, so a lane label, a timeline key and a panel curve cannot disagree about a channel's colour. The
panel's palette copies the three axis cells once per theme (`theme/Graph.cpp:126-128`) and the
drawing layer inks each row, curve, marker and chip with it. Consequence the pixel gate had to
absorb: two FIRST components are both red now, so its fixture was rebuilt from channels the hue CAN
tell apart (Move X against Scale Y - `tests/graph_panel_pixels.cpp:564,578,589`), which is section
7's own point. The channel list is the GROUPED legend section 7 describes: one group per (layer, key
type) - `PanelChannelRow::group`, e.g. `Move [X][Y]`, `Opa [Opacity]` (`GraphPanelModel.h:107-114`,
built in `GraphPanelModel.cpp:257-276`) - with one heading row and one row per component beneath it
in its axis hue, the group carrying which PROPERTY it is and the hue only which axis, so two red X
curves are told apart by their headings. The heading is a disabled row in the application's own
`QMenu` (`MainDisplayWidget::showGraphChannelMenu`, `MainDisplayWidget.cpp:316`), so it is structure
and not a control, and every row keeps its visibility toggle and its hover-to-focus. One DELETION
came with the rule: the panel's copy of the table dropped its four unreachable entries -
Bone/Pose/Mesh/FFD, because `componentCount()` is 0 for them and `graphableTypes()` filters on that,
so a row could never be published - while `theme::Colors` keeps their tokens and `keyTypeColor()`
still names them for the TIMELINE (`theme/Graph.cpp:130-150`); the contradictions section records
which half of the design won that, and why.

**The selection's HULL and COUNT.** `GraphEditor::selectionHull()` returns the convex outline of
every selected key in DATA space, collinear points dropped and empty with nothing selected
(`GraphEditor.cpp:2037`); `selectionCount()` is the live count (`GraphEditor.cpp:2070`). The caller
maps them through the same Scaler the curves and the hit test use, so a hull drawn from them cannot
desync from the markers it encloses (`GraphPanelModel.cpp:359-397`). The drawing layer strokes the
CLOSED outline in the panel's selection accent through the coverage pass - nothing is drawn while the
hull is empty, which IS the "nothing is selected" signal - and prints the count in the box the model
measured (`ImGuiOverlay.cpp:1676-1720`).

**Auto's GHOST handles.** Auto's control points are derived, not stored, so
`Curve::ghostHandles(key)` returns the pair `Easing::autoHandles` computes (x = 1/3 and 2/3 -
`graph_Curve.cpp:362`) and `graph::handlePointAt` is the ONE mapping from a normalized pair to
pixels, shared with the stored handles, so a ghost cannot drift from a stored one. The drawing layer
paints them HOLLOW in the channel's hue (`ImGuiOverlay.cpp:1633-1660`); a drag on one writes
`Type_Custom` seeded with the derived third-points inside the drag's existing Gesture, so converting
a segment is one undo step (`GraphEditor.cpp:1893-1916`), and the stored `x1,y1,x2,y2` convention is
untouched. They are published only for the row whose handles the drawing layer INKS (the focused
channel, or every row while there is no focus - `GraphPanelModel.cpp:544-545`), and the hit-test
derivation is culled EXACTLY: the two third-point x-pixels are computed before the derivation, and a
segment whose both x's are further from the pointer than `kHandleHitRadius` is skipped without it
(`GraphEditor.cpp:1221-1250`), so the cursor path no longer scales with the key count.

**The easing picker moved into the header chip: ONE popover, TWO anchors.** The chip fills the
header's fixed 196px slot at 24px (`TimeLineHeader.cpp:41`; `prop_Items.cpp:230,309`) with fixed
slots for glyph, name, range/blend and count, so a selection change cannot re-flow the bar beside it.
Its three states: disabled "Select keys" with an empty selection, "Mixed" when the selection
disagrees, the count when it holds several (`prop_Items.cpp:341-358`). ONE `EasingPickWidget` serves
TWO anchors through ONE entry point - the chip's bottom-left and the pointer of the timeline's key
context menu (now `TimeLineWidget::showKeyEasingPicker`, `TimeLineWidget.cpp:756-761`, reached from
the panel as `MainDisplayWidget::showGraphKeyMenu` -> that call, `MainDisplayWidget.cpp:377-381`) -
because a second copy of the realize/clamp/anchor dance would be a second popover's worth of drift
(`EasingPickWidget.h:159-168`). The chip is DISABLED with no selection and enabled by the first one,
because the picker must not open on nothing (`prop_Items.cpp:276-283,286-290`).
A multi-key picker session is ONE undo step (one scoped macro, one notifier, one Assign per key),
where the old menu pushed one command per key.

**The graph editor's visibility state is DELETED, and re-arming it for the dock would be WRONG.**
The removed overlay host was the only thing that ever made the editor "visible", and the flag's only
readers were two `Driver` paths that arbitrated VIEWPORT events against the panel's rect:
`Driver::updateCursor`'s graph branch took the event when `mGraphEditor->isVisible() &&
mGraphEditor->ownsCursor(...)`, and `ownsCursor` tests `GraphEditor::currentPanelRect()`
(`GraphEditor.cpp:647`, which is `panelRect(aCamera.screenSize())` - the WHOLE surface it is handed,
`GraphEditor.cpp:341-357`, i.e. the whole viewport while the panel was the fullscreen overlay). A
canvas press whose coordinates landed inside it therefore opened a graph marquee instead of running
the current tool; that is why the flag could not simply be re-armed with the dock as its reader. So
`Driver::setGraphEditorVisible`,
`Driver::updateGraphWheel` (which had NO caller at all), the `isVisible()` guard in
`Driver::updateCursor` and `GraphEditor::setVisible`/`isVisible`/`mVisible` are gone, and the
viewport's events are the tools' alone (`Driver.cpp:71-78`). The one live side effect hiding ever had
- dropping a gesture when the page goes away - kept its door: `GraphEditor::panelHidden()`
(`GraphEditor.h:153`, `GraphEditor.cpp:324`) is `endDrag()` and nothing else, called from
`gui::GraphPanelWidget::hideEvent` (`GraphPanelWidget.cpp:201-213`, because the tab can be switched
away from with a key move, a handle drag, a marquee or a pan still in flight), and the selection, the
scale and the hidden channels are kept so the page coming back is the panel as it was. **The command
a dropped gesture had already pushed STAYS on the undo stack** - dropping a gesture is not an undo.
`tests/graph_drag.cpp`'s section 24 drives the hide WITH a key move open and pins exactly that, plus
that a further move after the drop changes nothing (`tests/graph_drag.cpp:3418-3500`).

## Where the redesign contradicts itself, and which side won (2026-10-18)

Four places the design document disagrees with itself. Each was decided; the reason is the deciding
fact.

- **The selection's granularity.** s4's caption: "A KEY belongs to the type, so `Move X` and
  `Move Y` share their frames, their easing and their selection - one selection lights both markers."
  s4's gesture row: "The selection is the channel marker, not the key: `Scale X` and `Scale Y` of one
  keyframe are two things you can select." THE ROW WON, and the user ruled for it: "the marker of the
  same key that is not selected keeps a faint ring and its curve stays at full strength ... but it
  never takes the selection ring". A caption cannot override the gesture the user drives, and the row
  is the half the reference mock implements. The container still holds whole-key entries, which is
  what keeps the timeline's own gestures and the picker unchanged (`time_Selection.h:15-46`).
- **The channel control's placement.** s1's own table removes the header chips ("one control, one
  place") while s1's text lists them as header content, and s4's last cell says both that "the
  panel's ruler band carries one small `6 channels` control" and that the control "sits with the
  other view controls: in the dock's header when the panel sits in the dock's header with the other
  view controls, never in the ruler band". The ROW'S RULE won, read as "never INSIDE the band": the
  docked panel keeps the control in its own band's corner, hanging BELOW the band's tick-and-number
  block, and the header does not carry it (`TimeLineHeader.h:41-44`, `GraphPanelModel.cpp:296-350`).
  The mock's hosted variant has no app-side host and is not implemented.
- **The four not-graphable types.** s7's note: the Bone/Pose/Mesh/FFD panel entries are unreachable
  (`componentCount()` is 0) and "should be deleted rather than coloured". s8's row: they "keep their
  hues". SECTION 7'S NOTE WON, one commit after this section was written (`46978a29`), and the code
  is what decided it: `componentCount()` returns 0 for the four and `graphableTypes()` is the filter
  that walks it, `buildChannels` walks that list - so a row or curve for them could never be
  published, i.e. the colours were unreachable rather than merely dead. The four entries are GONE
  from the panel's table, which now says so in place (`theme/Graph.cpp:130-150`); the TIMELINE still
  draws their keys, so `theme::Colors` keeps their tokens and `keyTypeColor()` still names all four
  (`theme/Colors.h:568-593`). The colours are still ONE table shared with the timeline, which is why
  removing them from the panel's table costs the timeline nothing.
- **The lane curves.** s1 offers two opt-in lane-curve modes (selected object only / every lane) and
  calls them off by default. THE USER'S RULING: the timeline's lanes NEVER draw curves. Nothing in
  this wave added one - `ctrl::time::Renderer` has no lane-easing pass (the fill, the separators,
  the labels, the seams, the range markers, the keys and the marquee, and nothing else), and the
  header's chip is what names an easing instead.

## Traps this redesign paid for (measured, 2026-10-18)

Each of these cost a defect that was invisible in the code that caused it.

- **ImGui's 1px path shifts its geometry by (+0.5,+0.5), so the panel's 1px strokes go through the
  coverage pass.** `AddLine`/`AddPolyline` move their geometry half a pixel on each axis
  (`imgui_draw.cpp`, the 1px rasterization convention). MEASURED on the handle stem: 0.68-0.76px
  off-position against -0.01/-0.00/+0.01px for `gui::stroke` (`ImGuiOverlay.cpp:1650-1662`). The same
  rule puts the selection hull's closed 1px
  outline in the pass, which would otherwise spill across two half-covered rows instead of one whole
  one (`ImGuiOverlay.cpp:1676-1698`). The INTEGER-positioned chrome (the playhead, the frame and
  value ticks, the zero rule) is the exception and MUST stay out: at a whole-column position the pass
  splits a 0.500/0.500 boundary into two grey columns, and the gate asserts both directions.
- **A packed `ImU32` handed to a `const QColor&` swaps R and B, and it COMPILES.** `ImU32` and
  `QRgb` are both `unsigned int`; ImGui packs `IM_COL32` as `A<<24|B<<16|G<<8|R` while
  `QColor(QRgb)` reads `A<<24|R<<16|G<<8|B`. The stem-ink callback took the packed token, which was
  invisible on the stored stem (the palette's neutral `textMuted`, R == G == B) and visible the
  moment ghosts were drawn in the channel's own hue. Both branches now pass the QColor and carry the
  alpha through the strength (`ImGuiOverlay.cpp:1636-1652`).
- **`ctrl::time::Selection`'s stored key pointer is only ever COMPARED, never dereferenced.** The
  header states it (`time_Selection.h:104-108`), and the chip's state push violated it: it
  dereferenced `item.key` and passed `*item.key` to the Auto-context walk, from `paintEvent` - so an
  entry that outlived its key was a USE-AFTER-FREE on a paint path. The chip now RESOLVES the key
  from the document by `(line, type, frame)`, the pattern every other new reader uses, and skips a
  stale entry (`TimeLineEditorWidget.cpp:1134-1158`).
- **`gui::a11y` is ONE installation for ONE widget, and a hand-over race silently caches the docked
  widget plain.** The overlay's first-frame `install()` was ENDING the dock's constructor-time
  `installFactoryOnly()` installation; a client that asked Qt about the dock inside that window
  cached the widget's own interface permanently, and a later install cannot be reached for it (the
  stale entry is deliberately left in place - an id is an AT-SPI path - and the situation is REPORTED,
  `ImGuiA11y.h:100-130`, `ImGuiA11y.cpp:462-476`). The dock now declares itself at construction
  (`declarePanelDockHost`, `GraphPanelWidget.cpp:54-63`; `ImGuiOverlay.cpp:2446-2458`), the overlay's
  publication YIELDS to it (`ImGuiOverlay.cpp:2464-2480`), and the dock's first frame re-installs for
  the SAME widget - same interface, same id, only the callback changes. In this transitional build
  the overlay's own a11y publication is therefore yielded; the design's end state has only the dock.
  A hidden tab publishes the EMPTY model rather than its last frame (`ImGuiOverlay.cpp:2465-2472`),
  and `buildAccessibilityModel` now assigns `Model::summary`, which it never had - the panel's root
  was published UNNAMED even for the overlay, against ImGuiA11y's own contract (the Pane's name IS
  the summary).
- **The chip reserved EMPTY slots and elided its own label: 43px for a 66.9px word.** The 196px slot
  was handing over the full width, but the chip's interior reserved glyph/range/count boxes in every
  state, leaving the name 43px for "Select keys" - 66.9px against the bundled font. The interior now
  reserves a slot only when it carries something (the EXTERIOR stays fixed, so the bar beside it
  cannot move); the empty state's name box goes 43 -> 163px, the count grows rather than elides, and
  the name elides last (`prop_Items.cpp:369-380`).
- **The channels control STARTED INSIDE the ruler band.** Its box was placed at
  `rulerBandTop + kChannelsButtonTopGap` (8), and `rulerBandTop` is where the band's content BEGINS,
  so the box started 8px into the band and covered the frame-number row (and in the overlay host its
  0.55-alpha backing, `kChannelsButtonAlpha`, was painted over the numbers). It now hangs below the
  band's whole block - `top = rulerBandTop + kFrameTickLength + 1 + textHeight +
  kChannelsButtonTopGap` - with a precondition that the box and the band cannot intersect, stated in
  the BAND's own measures so it cannot drift when the band moves down under a widget floating above
  the panel or when a font changes the number row's height (`GraphPanelModel.cpp:294-350`).
- **The chip de-collide pushed chips OUT of the plot instead of dropping them.** The top-to-bottom
  push is the reference's own, but it only ever moves a chip DOWN, so a crowded selection left the
  last names past `plot.bottom()`, where the plot clip cut them. A chip that no longer fits is now
  DROPPED for the frame - and the survivors keep the positions the full list gave them, so the column
  cannot shift from frame to frame as names appear and disappear - which is what the mock does: it
  skips an item within 6px of the edge and never moves one back in. The header's "pushed apart
  vertically ... and clamped into the plot" was wrong about both the code and the mock and was
  corrected to the drop (`ImGuiOverlay.cpp:1817-1845`).
- **A VAO is not shared between GL contexts, and the stroke pass's was process-global.** With the
  panel in the dock its tab ABORTED the process the first time it had anything to stroke: `paintGL`
  -> `ImGui_ImplOpenGL3_RenderDrawData` -> `gui::stroke`'s draw callback ->
  `gl::EasyShaderProgram::makeSureVBO` -> `GL_CHECK_ERROR` -> `Q_ASSERT(0)`, "OpenGL Error
  (code:1282)". Qt's sharable context group gives buffers, textures, shaders and programs but NOT
  vertex-array objects, so `glBindVertexArray` on a name the CURRENT context never generated is
  `GL_INVALID_OPERATION` on a core profile - and the pass and its VAO were process-global, created by
  whichever host ran first, always the viewport overlay (`initializeGL` fires before the dock page is
  ever shown). Reproduced **7/7**, including with the overlay toggled on first. The EMPTY panel drew
  fine - no keys, no stroke, no crash - which is exactly why the live checks that had verified the
  tab missed it. The fix is structural plus a guard: one host owns the objects by construction, and
  the frame driver records the context they were created on (`gPanelOwnerContext`,
  `GraphPanelFrame.cpp:2116-2125`) and, on a frame whose context differs, tears them down ON THE
  CURRENT CONTEXT and rebuilds them there (`rebindPanelObjects`, `GraphPanelFrame.cpp:2206-2227`,
  called from `runPanelFrame` at `:2282`), warning once with the cause (`:2219-2223`) - a pointer
  compare per frame with one host.
- **Removing a class can leave the file's NAMESPACE nesting broken, and the symptom is the compiler
  calling your own names "ambiguous".** The overlay surgery cut one `} // namespace` along with the
  class, so `outline`, `stroke` and `drawPanel` ended up nested inside an ANONYMOUS namespace - which
  is what made the reported names "ambiguous" and put `outline`'s own `Params`/`kFilterPx` out of
  reach. Nothing in the edit pointed at it; the statements still parsed. After the repair the
  functions the surgery had touched (`outline::init`, `nextDeltaTime`, `stroke::init`) were checked
  byte-identical to the original at `HEAD` with `git show`. The nesting at HEAD puts `outline` and
  `stroke` at `namespace gui`'s own scope, each inside its own anonymous-namespace helpers rather
  than the other way round (`GraphPanelFrame.cpp:48-49, 409, 551, 1029`).

## How to verify this area now (2026-10-18)

HEADLESS GATES, all registered in `tests/CMakeLists.txt` and discovered by `scripts/run_suites.sh`:
`timeline_visuals` (the timeline's ink rules, driven through the real `ctrl::TimeLineEditor::render()`
into an offscreen QImage with the geometry read off the editor's own public mapping, so a change to
the scale cannot silently move it), `graph_panel_model` (the model's content and geometry),
`graph_panel_pixels` (drawPanel's ink), `graph_drag` (the press/marquee/marker-drag/command rules),
`graph_curve` (the panel's own curve, incl. Auto's ghost pair) and `time_scaler` (the ladder,
`indexForRate`, `tickStride` against `attribute`). The last full-suite figure recorded in this arc is
46 pass / 0 fail on the dock wave's tree (`a55dfb2a`); the marker-ink wave that followed had its two
pixel gates red when this was written.

**A `-W` warning is only absent from the run that COMPILED that TU.** `46978a29` deleted three
warnings - two unused test symbols (`tests/time_scaler.cpp`'s `notchIn`,
`tests/easing_regression.cpp`'s `numCases`) and one dead lambda (`tests/picker_keyboard.cpp`'s
`spin`), all left behind by earlier edits - that NO incremental build had reported and that only a
fresh Release compile of the whole tree surfaced. So "the build is warning-free" is a statement
about the object files on disk, not about the tree: it is only true of a run that actually compiled
those translation units.

PROBES (live, under Xvfb, `scripts/gui/run_probes.sh`, each assertion naming the number it
measured): **`probe_timeline_ink.sh` PASSES, 31/31** - the selected tree row's 2px rail and its
disappearance, the lane band's accent@0.06 tint measured against BOTH composites with a share bound
the old 0.40 flood is 2.7x past, the focused key's ring 0 -> 20-25 -> 0 accent pixels in a 5..8px
annulus while its centre goes to the 0.45 highlight mix, the lane label's 0.42 ink present with the
playhead away and absent when it is parked on the label, and the ruler number yielding its column to
the pill and coming back. Its shared helper is `scripts/gui/ae_ink.py` (modal colour of a box, counts
within a tolerance, x/y runs of inked columns and rows, an annulus count). The two PANEL probes were
RETARGETED AT THE DOCK, and the retarget has landed: `aa92de2a` rewrote `probe_graph_ink.sh` to reach
the panel through the dock's own tab and the dock's own pixels (its header records the crash that
phase now pins - "WHY THAT FRAME USED TO KILL THE APPLICATION", `scripts/gui/probe_graph_ink.sh:54`)
and added `probe_graph_anchor.sh`, which asserts on the dock that a middle-drag pan moves the content
by exactly the pixels dragged and that a wheel notch holds the frame under the cursor. The viewport
overlay probe `probe_graph_overlay.sh` is DELETED with the host it drove, and its key-menu /
easing-picker phase went with it - no probe opens the picker at HEAD (`grep -r kTileOrder scripts/`
returns 0). Neither dock probe was run for this file: their PASS/FAIL on this tree, and their claim
counts, are NOT verified here.

TWO LIVE CHECKS FOUND REAL DEFECTS in this wave, and each is worth re-running:

- **The `ae dump` a11y check.** After the dock's declaration fix, the Graph page is a named `panel`
  with a `label` and `button | Fit graph` under it; "Fit graph" appears EXACTLY ONCE (so the two
  hosts are not double-publishing) and the ordering warning - `gui::a11y: the viewport was queried
  for accessibility before install(), so Qt's cache answers for it with the widget's own interface`
  (`ImGuiA11y.cpp:468-476`) - is 0. This is the check that RECOGNISED the docked page publishing
  nothing while the same panel published fine under the overlay, which is what led to the
  one-installation hand-over above.
- **The header screenshot.** The only way the chip's states were ever read. It showed the disabled
  chip elided to "Selec..." (the 43px/66.9px reservation above); after the fix it reads
  `Timeline | Graph`, transport, `1 / 600`, the chip's "Select keys" IN FULL, zoom - exactly the
  design's header, and the dock's size hint keeps the bar's height without the lanes losing theirs.

THE CATALOGS ARE TRANSLATED NOW. `73ba0919` extracted the wave's own user-visible strings (the chip's
`Select keys` / `Mixed` / `%n key(s)`, the dock's tabs and zoom actions, the channel control and its
accessibility value, the selection count, the drag readout's `Δframe %1   Δvalue %2` - its spacing
preserved in every language - and the two `Change key(s) easing` labels) and translated them into
es/ja/ko/ru/zh, with the source forms filled in en: `lrelease` reports 948 translations with 0
unfinished for each of the five and 4/4 for the en stub (the entry counts are in "Translations"
above). The same extraction is why `%1 (%n more channel(s) not listed: no room in the panel)` is
`type="vanished"` now - the chip DROPS a chip that no longer fits rather than listing it. Also not
re-tested here: the 2026-10-14 residual (a Ctrl-press deselect in the plot followed by a typed dock
write) - the press routing that paragraph blames was rewritten by the marker-selection commit, but
no run here exercised it.

## When a `bash -c '...'` probe dies silently, count the ARGV words (measured 2026-10-14)

Every run of `scripts/gui/probe_graph_overlay.sh` died ~37 s in with 0 PASS and no geometry file, and the
application's own log ended:

```
The X11 connection broke (error 1). Did the X11 server die?
clearing main window
```

That reads as an X problem and is not one. Xvfb was killed by `xvfb-run`'s OWN teardown, after the probe body
had already exited `rc=0` at its first comment: the body is ONE single-quoted `bash -c '<body>'` argument, and
commit `acfbffa6` had added two apostrophes inside a comment (`# THE FIELD'S OWN ...`). The apostrophe ENDS
that string, the shell splits the rest into ordinary words, `bash -c` is handed THIRTEEN arguments and runs
only the first - 12355 of 101808 bytes. `bash -n` passes on that file.

The diagnostics that actually work, both cheap:

```bash
tr '\0' ' ' < /proc/<body-pid>/cmdline      # ARGV-ELEMS 15, ARGV-LEN 90997 -- the body was 12355 bytes
# and a quoting walk over the source (same model the shell uses) measuring where word 0 ends
```

After the fix (`34ca0faf`) one full run completes: **111 PASS / 23 FAIL / 0 STALE in 25m56s**, the geometry
file appears 100.5 s in, and Xvfb lives 1540 s. So: when such a probe dies silently, read the APP's log for the
X11 line and then count the argv words - do not go hunting the X server. (The 23 remaining FAILs are all the
key-menu phase, which is written against the ImGui popup that no longer exists.)

## The viewport overlay's paint engine is a SUBSET, and its gaps are silent

Anything drawn in the viewport overlay pass — the tool gizmos (the animation graph panel was drawn
there too until `aa92de2a`; it is in the dock's own page now) —
goes through `ctrl::PainterHandle`, whose `GLCorePaintEngine` (in a `USE_GL_CORE_PROFILE` build) is a
hand-written `QPaintEngine` over `gl::PrimitiveDrawer`. Four limits cost real time on 2026-10-12 while
building the graph panel; all four fail with NO warning, so a renderer that looks correct can paint
nothing:

- **`GLCorePaintEngine::drawPath` is `{ (void)aPath; }`** (`src/ctrl/Painter.cpp:127`). `QPainterPath`,
  `QPainter::fillPath` and `drawRoundedRect` (Qt routes it through a path) paint NOTHING. Measured: a
  graph panel body drawn as a rounded rect and its curve polylines were invisible on screen while the
  same code painted correctly into a raster `QImage`, which is the trap — a raster pixel test cannot
  see this. Draw with `drawPoint`/`drawLine`/`drawPolyline`/`drawRect`/`drawEllipse`/`drawPolygon`/
  `drawConvexPolygon`/`drawPixmap`/`drawImage`/`drawText` only.
- **The engine declares no `QPaintEngine::Clip` capability** and its `updateState` reacts only to
  brush, pen and render-hint changes, so **`QPainter::setClipRect` is not applied**. Enforce a
  boundary geometrically (clip the segments yourself, intersect the rects).
- **A texture/text draw is dropped unless a BRUSH is scheduled.** `drawTexture` queues its command
  with `usePen = false`, and `gl::PrimitiveDrawer::pushDrawCommand` returns early for those while
  `mScheduledState.hasBrush` is false. Measured: the graph's axis labels were drawn with a pen only
  (the grid lines beside them need `Qt::NoBrush`), so every number was discarded. Set a brush before
  `drawText` — and note the brush colour is also what TINTS the glyphs, because
  `GLCorePaintEngine::drawTextItem` builds the text texture white and draws it through that colour
  (`bone::NodeSelector` sets a brush for its labels, which is why those always worked).
- **A core-profile context needs a bound VAO for every draw**, and `gl::PrimitiveDrawer` never binds
  one. `gui::MainDisplayWidget::initializeGL` creates `mDefaultVAO` and every overlay draw depends on
  it. Measured in a throwaway harness: `glDrawArrays` with no VAO bound is `GL_INVALID_OPERATION`,
  and the engine's `GL_CHECK_ERROR()` turns any pending GL error into `Q_ASSERT(0)` — an immediate
  abort in a Debug build, from a drawing call that looks fine.

**The subset is pinned by `tests/overlay_engine_primitives.cpp`** (run it under `xvfb-run`; it is in
the GL branch of `scripts/run_suites.sh`). It drives the real `ctrl::PainterHandle` into its OWN
400x300 RGBA8 framebuffer, reads it back with `glReadPixels` and counts pixels per probe. Measured on
llvmpipe (Mesa 26.2.2, GL 4.6 core): control 0, line 368, rect 4800, polyline 1177, ellipse 3156,
convex polygon 3344, pixmap 3072, **path 0**, **text with a brush 294**, **text without a brush 0**.
Two things are load-bearing for any harness of this kind and cost a previous attempt its life: bind a
VAO (the note above) and read your own FBO — `QOpenGLWidget::grabFramebuffer()` re-enters Qt's render
and invalidates the colour buffer, so a per-paint readback through it returned pixels for one probe
and zero for the other seven.

What that gate still does NOT cover is the application's own composition - that the panel and the
gizmos reach the screen over the canvas - because a raster or FBO harness never runs the widget's
real paint path. The probe that covered it was `scripts/gui/probe_graph_overlay.sh` (auto-discovered
by `run_probes.sh`), DELETED with the overlay host it drove (`aa92de2a`); the dock's equivalent now is
`scripts/gui/probe_graph_ink.sh`, which reaches the panel through the dock's page tab **by name** and
asserts with pixel diffs that the change lands inside the Animation dock's AT-SPI rectangle. What the
deleted probe measured is kept for reference: a layer selected, the graph toggle pressed **by name**,
18505 px changed, of which only 568 are the toggle's own highlight, and off-vs-off is 0 px. Two traps
for anything like it: the View Settings buttons are icon-only, so every AT-SPI lookup returned the
GROUP BOX's name until the toggle got its own `accessibleName`; and **normalise both screenshots to
truecolor sRGB before `magick compare`** - comparing a grayscale capture against an sRGB one reports 0
differing pixels for images that differ by 1200, because the first image's colourspace seeds the
metric.


## The object tree, and where a project's keys hide

**The tree's first `table cell` row is the top node, and its child layers stay invisible until the
folder is expanded.** The only expander is the branch indicator: click `(rowX - 12, rowY + rowH/2)`
using the row's own `ae extents "<row name>" "table cell"` rect - never a guessed coordinate. The
indicator is a toggle, not an expander: a second click on it collapses the folder again (measured on
`I_think_its_cursed_honestly.anie`: 1 row -> 4 rows (`animation`, `head`, `line`, `circle`) -> 1 row).
A double-click on the row collapses it too, which is why every walk here uses single indicator clicks
and never double-clicks a row. That same expansion is what makes a CHILD row's properties exist at
all: with the TOP node selected the whole Properties board is empty, because
`ObjectTreeWidget::findSelectingRepresentNode` (`src/gui/ObjectTreeWidget.cpp:229`) returns nullptr
for the top node, the dock then calls `setTarget(nullptr)` and every panel hides.

**Most small `.anie` files under `~/Downloads` are keyless at the top node.** Measured per node with
the channel dump (`target=<node> maps <type>:<key count> ...`): `PBB.anie` (pizzaboyeoriginal),
`miatest.anie` (mia), `valentine pagedoll` and `running through one_s head.anie` expose a top node
whose graphable channel maps are ALL empty. Four do carry keys: `I_think_its_cursed_honestly.anie`
(872 KB; the top node `animation` is also keyless, but the child layer `line` has 2 scale + 3 opacity
keys), `animated pagedoll ych.anie` (1418 KB, 2 keys), `tart.anie` (1274 KB, 2) and
`amongusych2.anie` (1598 KB, 5). Pass one as argument 1 (or `AE_PROJECT=...`) when a probe needs
curve data, and remember that the same file then reports its curves only for the layer that is
selected.

**The add-key affordance is NOT the check box named `Create rotate key`.** That check box is
`gui::tool::SRTPanel`'s Transform-tool option (`src/gui/tool/tool_SRTPanel.cpp:53`, "The
transformation will create a rotate key regardless of its type"): it lives in the Tools dock
`Transform` panel, reports `showing=False` with a `0x0` rect while the Toolbox is the active tool
panel, and pressing it only sets `mParam.necessarilyRotate`. The knob that adds an EXPLICIT key at the
cursor frame is a `prop::KeyKnocker` row (`src/gui/prop/prop_KeyKnocker.cpp`): a label named after the
channel (`Move`, `Rotate`, `Scale`, `Depth`, `Opacity`, `HSV`, `Blur`, `FFD`, `Image`) plus an UNNAMED
plus button, present only while the frame has no key for that channel (the row stands in for the panel
header). Any click on the row runs the knocker - the title label forwards its click to the row. Three
measured traps sit in front of it:

- the Current Keys panel is hidden while the TOP node is selected (see above), so select a CHILD row
  first;
- the board is taller than the dock viewport, so the Current Keys rows start BELOW the fold: the
  Rotate row was at y=394 while the board viewport ends at y=391, and a click there hits the log pane
  and adds nothing (the key count stayed 0). The wheel does NOT move the board (8 notches at the dock
  centre changed 0 px), but the panels above collapse through their own title labels
  (`prop::HeaderButton` toggles on a click its title label forwards), and collapsing `Constants` and
  `Default Keys` lifts the row into the viewport (measured: y 394 -> 318 -> 194);
- `ae extents "Rotate"` does NOT find the knob: the View Settings strip toggle carries the group name
  as its accessible name and `Rotate` as its DESCRIPTION, so a plain press activates a tool. Measured
  on `I_think_its_cursed_honestly.anie`: that press changed only the rotated artwork (8243 px, region
  `190x145+175+39`, entirely inside the central widget) and added NO key, because the graph panel
  draws only explicit keys (`TimeLine::map(type)`). Ask by role instead - `ae extents "Rotate"
  "label"` - and prefer a child-row select plus the collapse recipe.

**Adding a key writes three places, and the graph panel needs a repaint to show it.** Measured on the
keyed project, one knob press changed the whole screen by 17807 px over the region `698x365+0+26`:
the Properties dock row becomes the channel rows (7525 px), the Animation panel lane gains the key
(8432 px), and the project tab gains the modified marker. What the PLOT shows is 1790 px inside the
central widget on that project and 460 px on the default one - a channel with ONE key is a key DOT
plus the fit basis moving under it; the curve itself needs a second key. Two consequences for any
probe of this: a whole-screen change can never be confined to the viewport once a real key is added
(measure the plot inside the central widget instead), and the HUD does not redraw for a key added
while it was already on screen - the central widget changed 0 px across the press and shows the key
only after the panel is toggled off and on.

**A last harness fact that cost time here: `ae_activate` on a `table cell` does not reliably select
the row it names.** It does set the AT-SPI `SELECTED` state, but the reported cell rectangles are not
the rows: measured, `head` reported `(40,221,81,22)` and `line` `(40,243,115,22)` while the tree
widget itself is only 70 px wide at `(0,199)`, so the synthesized click lands beside the row - after
"clicking" `line`, the selected rows were `animation` and `head`. Read the selection back (the AT-SPI
`SELECTED` state per cell) and treat the graph target as the authority when a probe needs to know
which layer it is driving.



## The `Auto` easing: Blender's automatic bezier handles, per segment

`util::Easing::Type_Auto` is the one easing whose shape is NOT in the key: its control points are
**derived** from the keys around the segment every time the rate is evaluated, which is what an
automatic handle means. The reference implementation is a standalone multi-key curve tool
(`dev/curves/easing-demo.html` - `computeAllTangents` / `getBezierTangents` / `recalcHandles`); the
port keeps that arithmetic and runs it for **one segment**, because a segment is the unit an
AnimeEffects easing shapes.

**Where the port lives, and the four rules it keeps** (`src/util/Easing.cpp::autoHandles`, called from
`calculateAuto`). With the segment normalized to `(0,0) -> (1,1)` and the neighbouring keys as
`(px,py)` (before the from key, `px < 0`) and `(nx,ny)` (after the to key, `nx > 1`):

- an end that HAS a neighbour takes the **central difference over its own triple** - the "uniform"
  tangent mode, the one Blender's auto handles use: the start's is `(1 - py) / (1 - px)`, the end's
  `ny / nx`. (Both are the reference's `(y[i+1]-y[i-1])/(x[i+1]-x[i-1])` in the segment's units.)
- an end that has NO neighbour follows **the handle it faces**: `1.5 - 0.5 * m_other`, which is the
  reference's geometric construction (slope from the endpoint toward the other key's control point,
  with the chord slope = 1 in normalized space).
- either tangent is then **clamped ("auto clamped") so neither of its handles leaves the value range
  its three keys span**. This is what stops an automatic handle from easing PAST a key it sits
  between - the whole reason Blender's default is *Auto Clamped* rather than *Auto*. The reference
  exposes the clamp as a toggle; the port always clamps, because the option IS "auto clamped" and a
  curve that overshoots its neighbour is the thing users disable an automatic handle to avoid.
- each control point sits **a third of the way across its interval**, so only the y values are ever
  derived.

**A segment with NEITHER neighbour is the identity (= Linear), and that is a deliberate answer**, not
a fallback: the segment carries no information about its own shape. The consequence to remember
before writing a smoke test for this feature is that a two-key channel CANNOT show Auto doing
anything at all - the drawn curve is byte-identical to Linear (its only difference is the chip's
name). Seeing it move needs **three keys on one channel**, so that the middle segments have a
neighbour on each side.

**The value an Auto handle reads, and why it is not the key's own number.** The easing must not know
whether it is shaping pixels, radians or an opacity, and the segment decides which component of a
multi-component key is read: `core::autoEasingValue` (`src/core/AutoEasing.h`) takes **the component
that moves MOST across the segment**, so a y-only move is shaped by its own y and a one-bone-only
pose by that bone (ties and a flat segment fall to the first component; a type with no scalar at all
- bone, mesh, and image, whose offset is step-held - returns false and the context comes back empty).
`core::autoEasingContext` then normalizes each neighbour to the SEGMENT's own change (`0` at the from
key, `1` at the to key), which is what makes `autoHandles` unit-free. A channel that does not change
across the segment has no shape to derive and yields the empty context, i.e. Linear - the right
answer, since nothing changes.

**The neighbours must come from ONE place on both paths, or the drawn curve is not the played one.**
`core::TimeKeyBlender::getEasingRateFromTwoKeys` already holds them - `point(-1)` and `point(2)` are
the channel's neighbouring keys, loop-aware and null at either end - so the Auto branch there is
`core::autoEasingContext(*k0, *k1, point(-1), point(2))`. The graph panel's `ctrl::graph::Curve`
takes the same pair through `Curve::neighborsOf` (extracted from the Move channel's own Catmull-Rom
knot lookup, which wanted exactly the same two keys) and calls the same
`util::Easing::calculate(param, ctx, t, 0, 1, d)`. `prop::KeyAccessor::autoContext(type)` publishes
the same context for the dock's chip and the picker, re-read on every refresh through
`EasingItem::setAutoContextProvider` / `EasingPickWidget::setAutoContext`, so a preview follows the
document instead of freezing at the shape it first saw.

**Serialization and the display order.** `Type_Auto` is **appended after `Type_Round`** (id 15,
`Type_TERM` = 16): existing ids 0-14 are serialized as-is and must never shift, and the default
easing is stored as the ENUM ID (`int(typeAtDisplayIndex(index))`), so an old settings file keeps
meaning what it meant. The DISPLAY order is a separate table and puts Auto at index 13, immediately
before Custom -> **14 pickable entries** (the menu skips Custom). Anything that counted 13 had to
move, and each of those was silent: `scripts/gui/probe_graph_overlay.sh`'s `MENU_ENTRY_ROWS`/
`MENU_ITEM_ROWS` (the menu was one row per entry, so a stale count failed the height identity - BOTH
constants went with the 14-row menu when the picker replaced it, and the probe read the tile catalog
instead - it is deleted now, see below), `tests/golden/easing_picker_geometry.json` (it enumerated
`tile-0..12` only - the 14th tile now wraps into the second row at `229,133`), and
`grep -rn '"13"' tests/`.

**A trap worth its own line: `Type_Sine <= type && type < Type_Custom` is an ORDINAL test**, used by
the chip's name and by `calculate`'s weight blending. It reads "the analytic curves with a direction
and a blend", and Auto (15) is outside it - which is what we want (an Auto curve is derived, so a
stale `weight` or `range` must not skew it, exactly as for Custom). It works only because
Sine..Bounce are contiguous below Custom, so anything APPENDED is automatically "no direction, no
blend". Keep new entries appended for that reason too.

**The picker shows the blend slider for a type whose weight is inert** (Floor, Round, and now Auto -
`calculate` applies weight only inside the ordinal window above; Custom is the exception because its
own panel replaces the normal one). That is PRE-EXISTING for the two stepped holds; Auto joins that
set rather than making the picker inconsistent with itself.

**The settings dialog's easing combo asked for a context no catalog carries** (measured 2026-10-17,
found while adding Auto): `GeneralSettingViewModel::easingOptions()` built its own label table and
looked it up with `QCoreApplication::translate("util::EasingName", ...)`, while the names are filed
under `EasingName` (`util::EasingName`'s entries are all `vanished`, and lrelease drops those). It
therefore showed **English in every language** - including the new "Auto" - while the same names were
translated in the timeline's menu. It now builds the list from `util::Easing`'s own display table
(`displayCount` / `typeAtDisplayIndex` / `getTypeName`), so the combo, the menu and the picker cannot
disagree and the names translate by construction. The check that proves it, and the reason the
measurement exists at all: build the `.qm` (`ninja -C build 'release_translations:Debug'` - lupdate
alone does NOT refresh them, and a stale `.qm` makes every i18n claim unverifiable), then ask a real
`QTranslator`. A lookup in the WRONG context returns the source with **no error**, so the assertion
has to be "the answer differs from the source".

**Gates.** `tests/easing_regression.cpp` hand-derives five handle cases (isolated segment, both
neighbours, first key, last key, clamped start) and checks the solved curve against a
`UnitBezier` built from those handles plus both endpoints, that a local extremum is flat, and that
weight/range are inert; `tests/graph_curve.cpp` samples Auto through the panel's own curve;
`tests/picker_probe.cpp` #10 asserts the tile catalog offers **every** display-order easing except
Custom exactly once (remove one from `kTileOrder` and it reports `missing=1` - measured);
`tests/easing_picker_layout.cpp` + the golden pin the tile wrap; `tests/general_setting_form.cpp`
asserts the combo IS the table (count, order, index-as-value); `tests/anim_setting_i18n.cpp` and a
manual `QTranslator` pass cover the catalogs. The end-to-end one WAS
`scripts/gui/probe_graph_overlay.sh` - since the picker replaced the key menu it recognised the
picker by its OWN source constants (`kTileOrder`), opened it from the timeline's key context menu,
pressed a tile at a coordinate derived from the tile order and read the commit off the application's
own state line (there is no 14-row menu left to count: 14 display-order entries now means 14 TILES).
It is DELETED with the overlay host it drove (`aa92de2a`), and NO probe opens the picker at HEAD
(`grep -r kTileOrder scripts/` returns 0), so that phase is a gate to RE-ADD rather than to re-run.

## The graph panel as the timeline's second view: what the user's 17-item list changed (2026-10-19)

One round took a user's list of "the graph has several shortcomings" and made the panel behave as a
view of this application rather than an invention beside it. Every line below was measured; the
mockup at `~/dev/graph-timeline-redesign/` is the design authority and its own code is silent about
several items, which is recorded where it matters.

**The ruler IS the timeline's, and that is now a compile-time fact.** The panel's frame axis used to
be its own invention: no band, no hairline, 4px ticks pointing down, numbers only on Major ticks in
`QString::number` format. It now draws a 22px band of the ruler surface with a 1px hairline, 5px ticks
at the timeline's tier alphas, numbers in every tier carrying `showNumber`, formatted with
`core::TimeFormat::frameToString` and the editor's own `timeFormatType()` (read from
`generalsettings/ui/timeformat`, the timeline's key), on the 6px/char reserve, with the collision
margin, the forced first/last labels, the merged "first–last" and the pill-column suppression.
`GraphPanelModel` **static_asserts** its band height, number rows, tick length, pill padding and
radius against `ctrl::TimeLineEditor`'s, so the two views cannot drift apart without a compile error.
The playhead gained the timeline's pill (14px, radius 3, padding 5, accent fill, the formatted frame
in the accent's text ink) and the line starts under it.

**One selection highlight, and it is the key's own hue.** A selected key's fill is its channel hue
mixed 0.45 toward `theme::Colors::text`; its ring is the same hue at 0.85, 1px, at `dotRadius + 2.6`
- OUTSIDE the marker. The timeline's accent ring and the graph's white ring are both gone (the user
was unhappy with both, and "the same highlight colour as the keyframe fill" is the rule). The graph's
2.6px dark sleeve under that ring went with them: it existed to make a WHITE ring read, and a black
band hugging the dot is what made a selected key look smaller - measured as the report "the key
visually gets smaller when I select it". The marker's OWN 1px edge stroke stays on its radius, as the
reference draws it; it is identical on every marker, selected or not.

**The tangent pair is drawn by role, not by leftover grey.** Stored (Custom) handles are filled with
the raised surface and stroked/stemmed in the channel hue (0.9/0.85); Auto's derived pair is a hollow
ghost (hue at 0.7, radius r-1.5, stem 0.5). Dragging one control point now MIRRORS the other:
`(1-x, 1-y)` in the segment's normalized space, with x read back from the clamped write, so the mirror
can never leave [0,1] and y overshoot is not clamped. The fixture in `graph_drag.cpp` is AUTHORED as a
mirror pair - which is why the old "the other handle stayed put" assertion passed while drags were
asymmetric.

**Three surfaces are gone: the hull, the drawn count, the drag readout.** The design draws none of
them ("no gesture in the panel prints anything"), the user called them visual noise, and the tests
that pinned them were deleted with them (`graph_drag` blocks 23c/23f, the model's [14] hull block and
[15] readout passthrough, the pixel gate's `checkSelectionHull`/`checkDragReadout`). The count is not
lost: `selectionCount` is published in the panel's accessibility node ("%1, %n key(s) selected"), so a
screen reader still hears what the plot no longer prints.

**The dock header carries what the design says it does.** The frame/total_frames readout is REMOVED
(its two facts are the pill's number and the ruler's forced last label); the channels button moved out
of the panel's ruler band into the header, shown while the Graph tab is current, and its popup is a
hand-painted `gui::GraphChannelPopup` with the mockup's numbers (22px rows, 12x12 swatch at 10px, an
8px swatch-to-name gap, names at 30px, group titles at the same 10px padding so they align with the
SWATCH column) on the application's sunken body colour - the user ruled that better than the mockup's.
The four quick easings (Linear/Floor/Round/Auto) sit beside the chip and act on the selection in one
undoable step. The fit button now wears `arrows-out-line-horizontal`, because it fits the FRAME axis
and the four-arrow cardinal glyph says "fit everything". And the easing vocabulary is ONE: the chip's
private Hold/Step/Ease set is deleted in favour of `util::Easing::getTypeName`.

**Two redraw gaps, both measured and both one line of cause.** A bare pointer move returned false from
`GraphEditor::updateCursor`, so the widget's guarded `update()` never fired and a hover changed
nothing; the widget now repaints when the hovered target's identity changes (and drops the lit marker
when the pointer leaves). And `syncGraphPanelSharedState` pushed the new frame window to the panel's
editor and then repainted only the VIEWPORT, so the header's zoom and fit buttons left the docked
panel stale; it now refreshes the panel too.

**The graph's key menu is the timeline's, and the multi-selection bug was in the PRESS.** Right-click
used to open the easing PICKER (`showGraphKeyMenu -> showKeyEasingPicker`) at a point mapped with
`MainDisplayWidget::mapFrom(this, ...)` between SIBLING widgets - a documented ancestor-only call,
hence "at the wrong place". Both views now open one builder (`TimeLineEditorWidget::buildKeyMenu`;
copy / paste / select spacing / delete, no easing - that is the header chip's job) at
`mapToGlobal` of the key. "Only the key under the mouse drags" was NOT the move: a move already
carried the whole selection. The PRESS collapsed it - a plain press on a covered marker re-selected
that marker before the drag read the set - and block 23b passed only because its arming press was a
SHIFT press. The press now leaves a covered selection alone; the marquee takes the design's 4px click
slop and decides on its own travel, not the plot-clipped rectangle.

**Trap: a button that appears per tab makes the DOCK grow, and then the WINDOW.** The channels button
is shown only on the Graph tab; a hidden layout item's space collapses, so the header's minimum width
changed and the dock measured 848px on the Timeline page against 920px on the Graph page - the window
itself grew from 1168 to 1241px, and then to 1247px once the button's own text read "10 channel(s)".
Fixed with `QSizePolicy::retainSizeWhenHidden(true)` and a pinned width: the hidden control keeps its
box (Qt counts its spacing and minimum), so switching tabs moves nothing. Any NEW tab-scoped header
control must do the same or the dock jumps.

**Trap: an anchored zoom that rounds twice drifts.** One wheel notch or header zoom kept the frame
under the cursor, but TWENTY did not: `TimeLineWidget::zoomFramesAbout` rounded its anchor to the
integer content axis and the scaled pixel was rounded again, ~0.5 timeline px lost per notch and
amplified on the narrower panel plot - measured 6px at 7.0000 px/frame = 0.857 frame, against the
design's half-frame invariant. The delegate path now computes ONE exact translation from the
fractional anchor (`delta = frame * (oldRate - newRate)`) and CARRIES its sub-pixel part in
`mPendingZoomPixels`, the discipline `scrollFramesBy` already uses for pans. Simulated: old pair 0.655
frame, exact-anchor-without-carry still 0.561, exact + carried remainder 0.021. `tests/time_scaler.cpp`
drives 20 real notches, asserts the drift and asserts the OLD arithmetic DOES drift - a discriminating
gate, not a tautology.

**The dangling-project class: a driver dies with its project, and the widget still holds it.** Opening
a project from the command line segfaulted on every run (`rc=139`): `TargetWidget::setProject(nullptr)`
-> the header's `refreshFrame()` -> `GraphPanelWidget::channelRows()` -> `buildGraphPanelModel` ->
`GraphEditor::isLooping()` on a FREED project (`this=0x5be9`). `isLooping()` is null-guarded, so a null
check would not have helped: the pointer was dangling, not null. `setProject` now drops the panel's
driver FIRST when the project is cleared, and the comment that had warned about exactly this ("the
previous project's driver is already destroyed ... a refresh here would read a dead editor") finally
covers the header's own read. Any new question from the header to the panel belongs AFTER that drop.

**Probe traps this round cost four runs each.** (a) `probe_timeline_ink.sh` had been scoring 0/0 for
weeks: an apostrophe in a comment inside its single-quoted `bash -c` body (`this suite's own`) ended
the body 300 lines early, so every later `say` ran in the OUTER shell - the documented trap, now
caught by counting apostrophes over the body, and the probe scores 31/31. (b) The anchor probe's
colour-column finder was never fooled by the new 14px pill (the 1px line's column holds 246 rows
against the pill's 14), but it now scans below the 22px band BY CONSTRUCTION. (c) The dock rectangle is
NOT constant across tabs (see the growth trap), so a probe must re-read it per state and mask with the
rect of the state it captured. (d) A pan of 40px near the content's end is CLAMPED by
`updateCamera`'s overlap bound - the probe asserts `min(drag, room)`, not the drag.

## The second graph round: the ruler became a section, and the inks became one family (2026-10-19)

The user's second list ("set todo again and orchestrate") was about the panel looking like a
different product beside the timeline. Every line measured.

**The ruler is a SECTION now, not a fill under the graph.** It used to be an `AddRectFilled` inside
`plot`, painted first, and every curve/key/handle was clipped only to `plot` whose top EQUALS the
band's top - so the document's ink painted straight over the ruler, which is exactly what
"seems to be behind the graph" meant. `ctrl::graph::Scaler::plotRect()`'s top inset is now
`kMargin + kRulerBandHeight` (22), the model publishes `rulerBand = QRect(plot.left(),
plot.top()-22, plot.width(), 22)` with `band.bottom()+1 == plot.top()`, and the band is painted
BEFORE `PushClipRect(plot)`, so nothing in the graph can enter it. `Scaler::kRulerBandHeight` is
`static_assert`ed against `gui::frameRuler::kBandHeight` AND the timeline's
`ctrl::TimeLineEditor::kHeaderHeight` - the three cannot drift. The playhead (line + pill) is drawn
LAST, after the chips, so a curve name can no longer cover it. A deeper occlusion (the tab-bar inset)
HIDES the band rather than pushing it into the graph; the plot never moves for it.

**Every selection mark is now the key's own ink.** The ring was the raw channel hue at 0.85 while the
fill was the hue mixed 0.45 toward the text ink - two different colours for one state. Both now use
the same opaque `mixedToward(hue, axisText, 0.45)`; the ring keeps its radius (dotRadius + 2.6) and
1px width. Auto's tangent dot is a SOLID dot at the smaller radius (it was hollow), so the two handle
kinds differ by radius and fill, not by hollowness. Handle stems and dots are mixed 0.2 toward the
panel's halo, i.e. slightly lower chroma than their own curve - a handle now reads where it sits ON
that curve instead of vanishing into it (alpha could not do this: alpha changes coverage, not chroma).
And the hovered handle stops being accent: it is the handle ink brightened 0.45 toward the text ink.
The accent is left to the two structural marks it already owned and shares with the timeline - the
playhead and the marquee.

**The Auto glyph finally says "automatic handles".** `gui::prop::paintEasingCurve` gained a trailing
`bool aAutoHandles = false`; when set on a Type_Auto param it draws the derived curve PLUS its two
control-point dots joined to the segment's endpoints by short stems - the reference's `ease` icon,
sized from the rect so a 28px tile and an 18px header button read the same. Every other easing's
glyph is byte-identical (the flag is a no-op off Auto). The picker's Auto tile opts in through
`CurveGlyph::setAutoHandlesVisible`. The four HEADER quick easings are glyph buttons built from the
same helper now (`QuickEaseButton`, 28x28 with an 18x18 glyph), placed LEFT of the easing chip, and
their checked state is the text ink rather than the accent.

**The channels popup had an accent rectangle, not an outline.** It was a `QPen(colors.accent, 1.0)`
`drawRect` around the CURRENT row, seeded to row 0 on open and moved with the pointer - so it read as
an accent outline that followed the mouse. Removed; the pointer's mark is the row's `raised` fill, and
keyboard focus uses a neutral `floaterEdge` inner line gated by a `mKeyboardCursor` flag (so nothing
appears on open and nothing follows the mouse).

**A background right-click now opens the timeline's own empty-space menu.** `openKeyMenuAt` returned
false whenever `panelKeyAt` missed a key within 10px, so the plot's background had no menu at all. It
now routes `MainDisplayWidget::showGraphBackgroundMenu` into the timeline surface's context-menu
route (a `QContextMenuEvent` delivered to `TimeLineEditorWidget`), so both views show one menu: with
no selection it is a single (clipboard-gated) `Paste key`; with a selection it is the key menu acting
on the whole selection. A right-click on a key is unchanged.

**The wheel gains SCROLL and loses a dead combination.** The graph used Shift/Ctrl for per-axis ZOOM
and, with both modifiers, consumed a notch that moved neither axis. The timeline's own wheel is
Shift = scroll, plain = zoom; the graph now mirrors that with a dual axis: plain = zoom both (about
the cursor), **Shift = scroll both** (frames through the timeline's `scrollFramesBy`, so both views
stay the same window, at the timeline's own 50px per notch; value by 50px locally), **Ctrl = value
zoom**, **Ctrl+Shift = frame zoom** (where Shift's frame zoom went). Select-all: the graph had it
(`GraphEditor::selectAllMarkers`); the timeline gains `ctrl::TimeLineEditor::selectAllKeys()` (every
key of every shown row, closed folders contributing their descendants) wired to Ctrl+A on the
timeline surface. The two do NOT share an implementation - the same shared `time::Selection`, two
different universes.

**Tangent symmetry is about the KEY, and that is a storage fact.** `cubicBezier` lives in the FROM
key's param: `(x1,y1)` is that key's OUTGOING handle and `(x2,y2)` is the TO key's INCOMING handle.
The segment-space mirror `(1-x, 1-y)` therefore wrote the NEXT key's handle - the user's "it
symmetrizes with the left side handle of the next key". The drag now reflects the grabbed handle about
the KEY in PIXELS (`2*key - dragged`, converted back through the NEIGHBOUR segment's own frame and
value spans, x clamped, y free), because a normalized mirror would bend the tangent where the two
segments have different value spans - measured on a fixture with 90 vs 60 value units and 30 vs 10
frames. First and last keys have one handle each: no mirror. An Auto neighbour is converted to Custom
first so the mirror is what playback reads. The gesture is now a two-param `HandleDragCommand`, so one
undo restores both. `graph_drag` section 23f holds six cases (both handle directions, first key, last
key, the clamp, and an Auto neighbour).

**Trap for the next test of this kind:** adding panel assertions to `tests/time_scaler.cpp` broke that
target's LINK (`gXCAssertHandler`, `AudioPlaybackWidget`, `QMediaPlayer` undefined) because it pulls
`libctrl`/`libcore` into a test that deliberately links neither. Panel-side wheel/zoom assertions
belong in `graph_drag`, which already links them.
