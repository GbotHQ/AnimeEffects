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

**FIRST CHOICE: `scripts/gui/gpu_headless.sh -- <command>` (cage + wlroots headless).** It gives an
Xwayland whose GL is the machine's real GPU (`Accelerated: yes, AMD Radeon R9 380 (radeonsi)`), with
nothing on the developer's desktop. Xvfb is the FALLBACK: its GLX is llvmpipe by construction, so
every render, probe and timing taken there is software - and that difference is not academic, it hid
a 1-LSB blend difference plus a marker-rim difference in this tree (see "A GPU-backed headless
display exists"). Limits of the cage wrapper: it is a KIOSK, so the command must BE the presented
client (a second X client can connect and drive it; a wrapper shell cannot become the window), and no
WM runs, so keyboard focus is the client's affair. Use Xvfb when you need xdotool/AT-SPI behind a WM
and do not care about GL's driver; use the GPU wrapper for anything whose PIXELS or timings matter.

The app needs a real GL context, so under Xvfb it runs with the xcb platform:

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

## Counts are pluralized, not written `key(s)` (2026-10-24)

The UI's count strings are Qt NUMERUS messages (`%n key(s)`, `%n channel(s)`), and the
English catalog (`translation_en.ts/.qm`) already carries the proper forms ("1 key", "2 keys";
"1 channel", "3 channels"). What the user saw was the SOURCE string: `LocaleDecider` loaded no
translator for English, and an untranslated numerus message falls back to its source text verbatim.
English now loads `translation_en` too (explicit "English" and an English system locale), so the
header reads `3 channels` and the selection chip `2 keys`; the source strings and the other five
catalogs are untouched.

THE CONVENTION, for new UI: never print `(s)`. Use the singular for one and the plural for zero and
more ("0 channels", "1 channel", "3 channels"); for an empty state a word form ("No channels") is
fine. Actions are not counts: prefer the plain plural "Copy selected keys" (or just "Copy") over
"Copy selected key(s)". `(s)` is a developer shorthand, not a UI convention.

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

**The digits after the FIRST were bilinearly blurred, and the cause was the fractional advance** (2026-10-22).
`ImFont::RenderText` truncates a run's origin ONCE and then accumulates each glyph's `AdvanceX` in floats, and
the atlas is sampled with `GL_LINEAR`, so every glyph after the first landed on a fractional column: MEASURED at
12px the digit advance is 7.1875, and `AddText(8, ...)` on `"0123456789"` put the glyphs at 8, 16.1875, 22.375,
29.5625, ... The rendered frame said the same thing the user did: the first digit (the only one on a whole
pixel) came out BYTE-IDENTICAL to Qt's own rasterization of that glyph (0 differing pixels over its 8x20 cell, at
the harness's one-row baseline offset), while the run carried 6.4% of its inked
pixels at full coverage against Qt's 12.7%, 100 faint pixels against 90, and 34 ambiguous columns against 23.
The fix is `ImFontConfig::PixelSnapH = true` on EVERY source in `installQtFontLoader()` - ImGui applies the flag
itself in both `ImFontAtlasBakedAddFontGlyph()` (drawing) and `ImFontAtlasBakedAddFontGlyphAdvancedX()` (layout),
so what is drawn and what `CalcTextSize` measures stay one number. After it: all ten glyph origins whole, 13.8%
strong / 17.3% weak / 24 ambiguous columns, i.e. Qt's numbers. The advances become 7 instead of 7.1875, so a
10-digit run is ~1.9px narrower than Qt's layout - invisible, and `drawLabel` already snapped each run's ORIGIN
(`int(std::floor(x))`), which is why only the intra-run positions were wrong. `tests/imgui_qt_font_loader.cpp`'s
section 2c is the gate (every digit's advance is a whole pixel and equals Qt's advance rounded). NOT covered:
a FRACTIONAL device pixel ratio, where an integral logical advance still lands on a fractional device column -
ImGui snaps in logical space and the backend scales by `DisplayFramebufferScale`, so that residual is structural
and none of the numbers above were taken at one.

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
SUPERSEDED 2026-10-24: the frame readout is gone (the playhead pill and the ruler name both numbers)
and the zoom/fit group is REMOVED as redundant - the wheel, the timeline's scroll bar and the graph's
wheel own zooming and fitting. The current row is `tabs | transport | quick easings | chip | stretch |
proportional | channels`; the proportional toggle is on the right (not in the easing group), and the
channels trigger follows it on the Graph tab.

**What the header deliberately does NOT hold**: the panel's channel chips (the mock's first draft put
them beside the zoom group) and any graph toggle - the toggle would repeat the dock's own tab and the
chips duplicated the panel's grouped legend, so one control lives in one place
(`TimeLineHeader.cpp:190-192` records the trim). The panel's OWN control (the "N channels" button) is
not here either: the docked panel draws it in its own ruler band (`TimeLineHeader.h:41-44`).

**`Timeline | Graph` is a real `QTabBar`** in the header's first group, whose tab order IS
`Surface`'s own (Timeline, Graph), so an index maps back by position and the two cannot drift
(`TimeLineHeader.cpp:120-133`): keyboard left/right, hover and the selected state all come from Qt.
ONE dock, two tabs, ONE header - the transport, chip, quick easings and proportional control stay put in both, and the tab
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
type) - `PanelChannelRow::group`, e.g. `Move [X][Y]` for a multi-component type and just
`Opacity`/`Rotate` for a one-component one (`GraphPanelModel.h:107-114`, built in
`GraphPanelModel.cpp:257-276`) - with one heading row and one row per component beneath it in its axis
hue, the group carrying which PROPERTY it is and the hue only which axis, so two red X curves are
told apart by their headings. The popup is `gui::GraphChannelPopup` (not a `QMenu`); its headings are
a LAYER section heading only, then self-describing rows ("Move X", "Move Y", "Rotate", "Opacity")
whose swatches start at the heading's left edge (the swatch is the row's indent; no extra
indent). There is NO group heading and no `layer · group` string: the rows already name the property and its
component, and a `Move [X][Y]` title above them made the group's end ambiguous. No row is bolded for
the sticky focus - the panel's tangent handles show that. Every row keeps its visibility toggle and
its hover-to-focus. One DELETION
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

## Marquee auto-scroll, and what a collapse is allowed to clear (2026-10-24)

**Edge auto-scroll is ONE rule for both marquee surfaces.** `ctrl/AutoScroll.h` owns the
geometry both views used to need separately: a 24px edge band, a 16px-per-tick maximum
(one pixel at the band's inner boundary), a 16ms timer, a 150ms DIRECTION-AWARE DWELL, and
one sign convention - `x > 0` reveals content to the right (later frames), `y > 0` content
below (later rows / lower values). The dwell arms an axis only when the pointer MOVES
TOWARD that edge (or is already outside the viewport): a box dragged HORIZONTALLY along the
top edge does not scroll up merely because it started in the top band. Once armed, holding
still keeps scrolling; moving back inward or leaving the band disarms that axis. The
timeline's edge viewport excludes the ruler/header (`marqueeScrollViewport`), so the top
edge is the first lane, not the top of the dock. The timeline applies `x` through `scrollViewBy` and `y` through the object tree's
vertical scrollbar; the graph applies `x` through `scrollFramesByBarPixels` (the shared
frame delegate included) and `y` through `panValue(-step)`. The pointer stays at its
screen position while the content moves under it: the timeline recomputes the world point
from its camera and calls `TimeLineEditor::extendMarquee`; the graph re-runs
`applyMarquee` against the moved curves. Both hosts own a 60 Hz timer and stop it on
release, on a tick that moved nothing (a content bound), and on the page hiding (a tab
switch can swallow the release).

The timing/lifecycle is deliberately split: `ctrl::autoscroll` is pure math, the editors
own the application (`extendMarquee` / `autoScrollMarquee`), and the widgets only decide
when to tick. A unit gate, `tests/autoscroll.cpp`, pins the band, the acceleration, the
signs, the outside-the-viewport clamp and the dwell/re-entry rule; `tests/graph_drag.cpp` section 30 drives a real
graph marquee at the plot's bottom-right and asserts both windows moved while the box
stayed live.

**A row rebuild is a VIEW change, so it no longer clears the selection.** Every
expand/collapse used to call `TimeLineEditor::clearRows`, which cleared the set. Now
`TimeLineEditorWidget::updateLines` calls `resetRows` (rows and transient gesture state
only) and then `retainSelectionInRows`, which keeps every entry whose node is represented
by the rebuilt rows. "Represented" includes a CLOSED FOLDER's descendants: its summary
lane still draws their keys, so they stay selected; collapsing the ROOT leaves no rows
and clears the set. This is also the answer to the "unstable id" concern: a selection
entry is `(TimeLine map, type, frame)` plus a stored key pointer, and neither is
recreated by expanding or collapsing, so `Selection::retain` only filters the existing
entries - it never re-resolves a key from a new id. `tests/creation_select_keys.cpp` C7
pins the four states (collapsed summary keeps, vanished folder drops only its key, no
rows clears).

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
playhead BOTH away from the label and parked on it (a lane's name is not the ruler's number, so it
does not yield; the phase prints both counts), and the ruler number yielding its column to the pill
and coming back. Its shared helper is `scripts/gui/ae_ink.py` (modal colour of a box, counts
within a tolerance, x/y runs of inked columns and rows, an annulus count). THE LABEL PHASE WAS FLIPPED
after the 31/31 run recorded here, which predates it: re-record the figure on the next run. The two
PANEL probes were
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



## Per-side handle modes: Blender's five, and Auto is not a type

`util::Easing::Type_Auto` is RETIRED. A key's two tangents now carry a
`util::Easing::HandleMode` each - `handleIn` and `handleOut`, Blender's five:
**Free, Aligned, Vector, Auto, AutoClamped**. `Param::aligned` no longer exists.
The type stays a shape family (Linear, Floor, Round, Sine..Bounce, Custom); the
modes shape a **Custom** bezier, which is why the picker's Auto preset writes
`Type_Custom` plus both modes rather than a type of its own.

**Ids and migration.** The retired `Type_Auto` id (14) is NOT reused: `Type_TERM`
still has the value 14 so nothing can slide into the slot, and `isValidParam`
rejects it. Format **0.15** writes `handleOut` then `handleIn` after the bezier
(replacing the one 0.13 flag; a 0.14 flag maps to both sides, and a pre-0.13
param loads Free/Free). A 0.14 param whose type word is 14 loads as
`Type_Custom` with **both sides Auto** - exactly what it meant. The clipboard's
lane JSON keeps the same bridge: it writes `"Auto"` for Custom+Auto and reads
that name back as the modes.

**Evaluation is per side, and that is the user's bug fixed.** A Custom
segment's control points come from `util::Easing::controlPoint(param,
toKey.handleIn, ctx, span, side)`:

- **Free** and **Aligned** return the stored `(x,y)`. Aligned's collinearity is
  enforced by the gestures, never inferred from the pixel angle (the old
  inference "snapped back unpredictably").
- **Vector** is the segment's own chord at the third points - `(1/3,1/3)` /
  `(2/3,2/3)`, i.e. a straight segment (Blender's Vector). The outside chord
  would put x outside the cubic's domain, which is not what a vector handle is.
- **Auto** reads the neighbour on ITS OWN outside: the outgoing side reads the
  PREVIOUS key (`slope = py/px`), the incoming side reads the NEXT key
  (`slope = (ny-1)/(nx-1)`), with both sides normalized to the segment's own
  change. A side with no such neighbour falls back to the identity third. The
  handle x stays at the third, so only y derives.
- **AutoClamped** is the same derivation with y clamped into the value range the
  key and its neighbours span, which flattens a local extremum.

**Because the sides read DIFFERENT neighbours, they are independent**: dragging
one side writes that side Free (a derived side cannot express a stored drag) and
the other side, if Auto, keeps deriving. This is the fix for "when the key is
auto and I drag any of the handles, it changes both tangents to manual" - the
retired whole-key type converted both. `tests/graph_drag.cpp`'s per-side auto
section and `tests/graph_curve.cpp` section 11 assert it: changing a neighbour
moves the still-Auto side and leaves the dragged (stored) side alone.

**Gestures.** An Aligned side's drag keeps the partner collinear at its own
length; a Free or derived side's drag leaves the partner alone. **Alt on any
step** frees THAT SIDE for good (one command with the handle writes, one undo).
**Alt+CLICK on a key** sets BOTH sides Aligned and puts them on the chord from
the previous key's anchor to the next key's anchor, each at its own length.

**One derivation feeds both paths.** `core::TimeKeyBlender` and
`ctrl::graph::Curve` call the same `controlPoint`, with `core::autoEasingContext`
supplying the neighbours and the TO key's `handleIn` (`AutoContext::inMode`).
`Curve::effectiveHandles` is the pair the panel draws, hit-tests and grabs;
`Curve::ghostHandles` returns it when at least one side derives. The neighbours
come from the gatherer's `point(-1)`/`point(2)` on the playback path and
`Curve::neighborsOf` in the panel, so the drawn curve is the played one.

**The value a derived handle reads** is still per-component: `core::
autoEasingContext` normalizes each neighbour to the SEGMENT's change, and
`core::autoEasingValue` picks the component's own scalar (Pose/FFD use the one
that moves most). A channel that does not change across the segment yields an
empty context (identity thirds), which is the right answer.

**Gates.** `tests/easing_regression.cpp` hand-derives the per-side cases
(isolated, both neighbours, first, last, clamped start) and compares the solved
curve to a `UnitBezier`; `tests/graph_curve.cpp` section 11 checks both the
hand-worked pair and the played/drawn identity, plus the 0.15 round trip, the
0.14 flag migration and the retired type's migration; `tests/graph_drag.cpp`
drives the gestures (one-side Auto, Aligned collinearity, Alt, Alt+click, the
derived-dot grab); `tests/picker_probe.cpp` #10 and
`tests/easing_picker_layout.cpp` pin the tile catalog (three types + the Auto
preset at index 3 + the family); `tests/general_setting_form.cpp` asserts the
default-easing combo is the display table minus Custom (Auto is no longer a
default, because it is not a type).

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
8px swatch-to-name gap, names at 30px) on the application's sunken body colour - the user ruled that
better than the mockup's. Its hierarchy is LAYER (10px muted section) -> self-describing rows (11px) whose swatch aligns
with the layer heading: no group title, no extra row indent, no `layer · group` dot, and no bold for
the sticky focus.
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
`bool aAutoHandles = false`; when set on a Custom param with a DERIVED side it draws that curve PLUS
its two control-point dots joined to the segment's endpoints by short stems - the reference's `ease`
icon, sized from the rect so a 28px tile and an 18px header button read the same. Every other easing's
glyph is byte-identical (the flag is a no-op without a derived side). The picker's Auto preset tile
opts in through `CurveGlyph::setAutoHandlesVisible`. The four HEADER quick easings are glyph buttons built from the
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
## The third graph round: the ruler became the timeline's, and the handles became a bezier editor's (2026-10-20)

The user's list was "the ruler and playhead still diverge from the timeline's - I expect full parity,
visually AND in features; the handle symmetry is now forced, where is Alt, why is LENGTH forced too,
and where is Alt+click to re-enable it; the Auto icon is hard to read; the channels menu's separators
are broken and do not belong; wheel scroll was not what was asked (it scrolls diagonally) and it
removed the per-axis zoom - what is missing are SCROLL BARS; Ctrl+A did nothing until I clicked in
the timeline."

**The ruler is the timeline's, rule for rule.** What the graph lacked, all of it now published by
`gui::GraphPanelModel` and drawn by `GraphPanelFrame` with the timeline's tokens:
- the 0.4 RECESSION outside the ACTIVE range (the project frame range, or the temporary play window)
  for every tick and every number - `frameTickInActiveRange` and `PanelLabel::inActiveRange`; the
  model still publishes classifications, the drawing layer applies the factor;
- the PLAY WINDOW overlay (`PanelPlayWindow`): the translucent band, the two 2px bracket stems and the
  label pills, merged into one centred "a–b" pill when they collide - `TimeRuler::renderPlayWindow` is
  the reference, and the push runs through `MainDisplayWidget::syncGraphPanelSharedState` on the new
  `TimeLineEditorWidget::playWindowChanged` (the window is the TIMELINE editor's view state, so the
  graph gets it by delegation, like the frame window);
- the pill's number in `Colors::text` (the timeline's `TimeCursor` ink; it was `accentBrightText`).

**The band's row, measured.** The band used to sit at `plot.top() - 22`, and `plot.top()` includes the
scaler's 4px margin, so the graph's strip started 4 rows BELOW the timeline's. The band is now anchored
to the panel's top (`band.top() == panel.top()`), and the 4 margin rows sit BETWEEN the hairline and the
plot. Measured on one capture, both tabs: strip rows page+9..page+30, hairline page+31, plot page+35 -
identical in both views. THE RESIDUAL, now explained and fixed: the timeline's hairline rendered
`(51,51,51)` = `#404040` blended 50% over `#262626`, while the graph rendered the same `Colors::hairline`
token opaque `(64,64,64)`. The cause was in the timeline's paint, exactly where the note pointed: the
editor's renderer leaves `QPainter::Antialiasing` ON at the end of its passes, and
`TimeLineEditorWidget::paintEvent` drew its 1px rule on the band boundary without clearing it, so the
stroke split 0.5/0.5 and only the lower half survived (the band's half is repainted by the TimeRuler
widget). The widget now sets the hint OFF for its own rule - the stroke states its own rasterisation
rather than inheriting the editor's last pass - the panel draws the token at full strength, and
`probe_graph_parity.sh` compares the two hairline rows' MODAL colours (both `(64,64,64)` in the dark
palette; it fails if they diverge). The panel's half-coverage reproduction constant is deleted.

**Handles: the modes are STORED, per side.** A key's outgoing handle
(`cubicBezier.x1,y1` of its own param) and its incoming one (`x2,y2` of the
previous segment's param) each carry their own `HandleMode` on that key's
`util::Easing::Param` (`handleOut`/`handleIn`, folded into `operator==`,
serialized after the bezier controls in format 0.15). Nothing infers a mode from
the pixel angle: an inference flips whenever an edit changes the pixel angles -
a key moved on the value axis above all - and the user's verdict was that it
"snaps back to aligned unpredictably". Dragging an ALIGNED side keeps the other
COLLINEAR at its OWN LENGTH (angle linked, length independent); a FREE side's
partner is never touched; a DERIVED side becomes Free and its partner keeps
deriving. **Alt on any step** frees THAT SIDE in the same command (one undo), so
a later drag without Alt stays free - "once it diverges with alt, the way to get
it back should be an alt click". **Alt+CLICK on a key** sets BOTH sides Aligned
and puts both handles on the line through the key along the CHORD from the
previous key's anchor to the next key's anchor, in pixels (a bezier editor's
"make smooth"), each handle keeping its own length; the direction never depends
on which handle is longer, and a first/last key (one handle) is inert. A key
MOVE never writes a param, so no mode and no handle changes.

**The Auto icon is the PRESET's picture, not the key's.** With no neighbouring
keys a derived pair is the identity, so an automatic curve IS Linear's straight
line - the glyph would be literally indistinguishable from Linear's, which is
what "hard to read" meant. `paintEasingCurve` substitutes the canonical context
(`kAutoTileContext`: neighbours that hold the segment's END values on both sides,
i.e. the plateau case) whenever a glyph with a DERIVED side is drawn with the
handle flag and no neighbours: the real rule (`Easing::controlPoint`) then
produces the flat-ended S, with its two control points and stems. The picker's
Auto preset tile and the header's Auto quick button opt in; the header chip opts
in as well, so an automatic key without neighbours shows the shape, and one WITH
neighbours still shows the curve the document plays.

**The ruler SEEKS, because the timeline's does.** The timeline's ruler is not a picture: a press on
it moves the document's current frame and a drag keeps moving it (`TimeRuler::mousePressEvent` ->
`TimeLineEditorWidget::updateCursor`). The panel cannot move that frame itself - it is
`core::Project`'s, owned by the timeline widget - so `GraphEditor::setFrameSeekDelegate` takes the
timeline's own setter (`MainDisplayWidget::syncGraphPanelSharedState` hands over
`mTimeLine->setFrame`) and the band is hit-tested at its published slot. **The band's rect is
`plot.top() - kMargin - kRulerBandHeight`**, not `plot.top() - kRulerBandHeight`: the scaler's 4px
margin sits BETWEEN the hairline and the plot, so a band derived from the plot alone covers the
margin rows and misses the band's own first four - the first version of this rect was exactly that
and graph_drag's section 0e caught it on `plot.top() == band.bottom() + 1 + kMargin`. With no
delegate the band is claimed by nobody (nothing invents a second frame authority), the seek pushes
no command, and a press elsewhere ends it (`endDrag` clears the state).

**The wheel: per-axis zoom, no scroll.** A previous round replaced Shift with a diagonal scroll-both and
moved the frame zoom to Ctrl+Shift; the user's verdict was that the per-axis zoom was the useful part and
the scroll the wrong answer. The table is: plain = zoom both about the cursor, Shift = FRAME zoom, Ctrl =
VALUE zoom, Ctrl+Shift = both. Every combination zooms at least one axis; none scrolls.

**Superseded for Shift (2026-10-21): the dock's frame axis has ONE wheel mapping.** The table above
treated the graph as a surface of its own, but the timeline's plot sits in the same dock over the same
frame axis: over it `TimeLineWidget::wheelEvent` scrolls 50px a notch on Shift and zooms on plain, while
the graph zoomed the frame axis on Shift. Shift now SCROLLS the frame axis in the graph exactly as it
does there - 50 content pixels a notch, wheel up toward earlier frames - and is delegated through
`scrollFramesBy` while a timeline owns the window, so both panes move one window; plain still zooms both
axes about the cursor, Ctrl the value axis, Ctrl+Shift both. The scroll is deliberately not inverted by
the zoom-direction setting (the timeline's Shift branch takes the raw notch). Remaining, deliberate
differences: Ctrl over the timeline's plot zooms its only axis while over the graph it moves the panel's
own value axis, Ctrl+Shift there scrolls (Shift wins) while here it zooms both, and over the timeline's
bar Shift/Ctrl page by `pageStep` while the graph's lane wheel always uses `wheelScrollLines x 24px`
(the graph also banks partial deltas across events; the timeline's bar treats any nonzero delta as a
whole step).

**What was actually missing: scroll bars.** `ctrl::graph::Scaler::scrollBars(frameContentLo/Hi,
valueContentLo/Hi)` owns the geometry (`kScrollBarThickness` 8, `kScrollBarGap` 2,
`kScrollBarReserve` 10, `kScrollBarMinThumb` 16) and BOTH sides read it - the model publishes
`hTrack/hThumb/vTrack/vThumb`, the editor hit-tests the same rects. The panel's right and bottom reserve
10px for them, so the plot is 10px narrower and shorter than before (its top-left does not move). Thumb =
the window's fraction of the CONTENT (frames: `frameRange()`; values: the shown channels' key extent,
`GraphEditor::shownValueExtent`), filling the track when the content fits, never shorter than 16px. A
press on a thumb drags it; a press on the track first CENTRES the window on the pressed point and then
continues as that drag. No command is ever pushed - a scroll bar is the user's own movement of the window.

**A scroll bar with no travel is not drawn.** The first version published a full-track thumb whenever
the content fit, and on the empty graph that put two bright 8px rails along the panel's edges - the
first thing the eye found. `Scaler::scrollBars` now returns an EMPTY thumb when the thumb would fill
its track, so the drawing layer paints nothing and the hit-test claims nothing (one rule, both
sides); the TRACK stays laid out, which is what keeps the plot's rect independent of the content.
Three test sites had to follow the rule rather than the old arithmetic (graph_curve's two "whole
content" cases, graph_panel_model's [16] full-content block, and its bars fixture, which now zooms
the value axis so that a thumb exists to measure).

**Focus after a tab switch: the surface, not the scroll area.** `TargetWidget::onSurfaceChanged` now
focuses the view the tab shows, and the TIMELINE half is a trap: `gui::TimeLineWidget` is a
`QAbstractScrollArea` and the widget whose `keyPressEvent` owns Ctrl+A/Escape is its viewport CHILD
(`gui::TimeLineEditorWidget`), so focusing the scroll area hands the keys to a widget with no
handler - the same "nothing happens" the fix was for. Measured with the accessibility bus, which is
also the only way to see it: `ae focus` (a new command in `scripts/gui/ae_ui.py`) prints every node
the bus reports FOCUSED. On the Graph tab it names the panel; on the Timeline tab it named a
`filler` - `QAccessible`'s role for a plain `QWidget` subclass, i.e. the editor surface - once the
lookup was `mTimeLine->findChild<TimeLineEditorWidget*>()`. A key press that vanishes has two
causes (no focus, or focus with no handler) and this is the one that tells them apart.

**The easing is PER CHANNEL now, and that is Blender's model.** `util::Easing::Param` used to be one
per key, shared by every component it drives - so shaping Move's X re-shaped Move's Y, and any
per-key state (the handle link) was shared too. The industry answer, from their own docs: Blender's
F-Curve is per `array_index` with `handle_left_type`/`handle_right_type` per keyframe, and After
Effects separates temporal from spatial interpolation and offers Separate Dimensions. So a key now
holds one Param per GRAPHABLE component (`std::array<Param, kComponents>`, counts in the new
`src/core/KeyEasing.h` - the single declaration `ctrl::graph::Channel::componentCount` reads), the
blender evaluates each component at its own rate, and the graph/dock read the CHANNEL's component
(`Curve::easingOf(key, type, component)`) - never the key's first one. Format 0.14; a 0.13 key
writes ONE param and the reader broadcasts it to every component (lossless for what it described).
Structural keys (pose, FFD) keep one shared easing and are still not graphable; Image stores two and
stays step-held.

**The ruler's mapping is the TIMELINE's, and that is the whole point.** The graph used to fit the
shared frame window into its PLOT (minus a margin inset), so its frames-per-pixel and its origin
differed from the timeline's: switching tabs moved every tick, and the band stopped at the plot's
left edge (the "corner" report). `Scaler::applyFrameWindow` now maps `lo -> panel.left()` and
`hi -> panel.left()+panel.width()`, exactly the timeline viewport's own edges, so a frame's x in the
graph IS its x in the timeline (asserted as a formula in `tests/graph_curve.cpp`). Consequences, all
deliberate: the band spans the panel (covering the value column's top), the playhead spans the panel
(as the timeline's crosses its whole body), and the plot shows a SUB-RANGE of the shared window -
the leftmost frames sit behind the value column, which is what keeps one window and one zoom.

**The panel's scroll bars are the APP's scroll bars, not a design of their own.** The real ones live
in `src/gui/AppStyle.cpp`'s `CC_ScrollBar` branch (QSS only sets `background: none`): a 12px lane
with an INVISIBLE trough and a 4px, radius-2 pill in `Colors::outline`, `Colors::hover` while hot.
The panel's first version invented an 8px `raised` trough with an 8px bright thumb, which the user
read - correctly - as arbitrary. The bars are now that pill on both axes (`kScrollBarLane` 12,
`kPillThickness` 4, reserve 14), a lane press is ONE PAGE toward the press (Qt's page step, the
visible window), and a bar with no travel is not drawn at all.

**Two small ones, both reported.** The channels popup's group separators are gone (a heading already
separates; the seam landed in the gap above it and read as a broken rule). And a tab that becomes
current now takes the keyboard (`TargetWidget::onSurfaceChanged` focuses the page it shows), so Ctrl+A
works after switching to the Timeline tab instead of only after clicking its body - the surface is
ClickFocus on purpose, and an explicit `setFocus` is what bridges the two.

**Two traps.** A throwaway test target left in `tests/CMakeLists.txt` is DISCOVERED by
`scripts/run_suites.sh` - it turned 46 passing targets into "47 pass", which is how it was noticed;
delete the target and the file before the gate. And a test fixture sized against a layout constant has
to grow with it: the narrow-viewport fixture was 80px "to keep a plot narrower than 16px", and the
scroll bars' 10px reserve made that plot negative, so the fixture is 90 and the assertion is unchanged.

## The frame range's guides: PANEL columns, PLOT rows (2026-10-24)

The timeline draws two vertical hairlines at the project's first and last frame
(`ctrl::time::buildRangeMarkers`/`paintGuides`), continuing the ruler's boundary ticks down through
the lanes. The graph panel drew neither, so the two views disagreed about where the range ends. It
now draws the same two marks from two new model fields: `GraphPanelModel::rangeGuideX` (the columns;
one entry when both ends round onto the same pixel, empty with no project) and `rangeGuideRecede`
(a temporary playback window is active, which is the timeline's 40% -> 25% rule).

**The columns are PANEL columns and the rows are the PLOT's, and both halves are measured facts.**
The shared frame mapping puts a frame on the TIMELINE's pixel (`Scaler::applyFrameWindow` maps the
pushed window across the panel), so at the usual fit the project's first frame lands INSIDE the value
column the plot is inset by: MEASURED in the running application (`running through one_s head.anie`,
panel 648x256, plot x 75..635), `rangeGuides = [50, 587]` - the first guide is 25px LEFT of the plot's
first column. A guide clipped to the plot therefore vanishes exactly when the view is at the start of
the range, which is the state it is most useful in; this is the same reason the playhead spans the
panel's columns. The ROWS stay the plot's alone - the ruler band above already carries the boundary
tick and the scroll bar's lane belongs to the bar - so the drawing layer pushes a clip of
`(panel.left(), plot.top(), panel.width(), plot.height())`, draws both guides there BEFORE the plot's
own clip (so the value ticks, the zero rule, the curves, the keys and the handles all sit on top), and
the value labels, painted last, cover the guide where it crosses their column.

**The ink is the timeline's, token for token**: `theme::Graph::rulerLine()` (which IS
`theme::Colors::hairline`) at `kRangeGuideAlpha` 0.40 / `kRangeGuideWindowAlpha` 0.25 - the
timeline's 102/255 and 64/255. MEASURED live: the guide column reads `rgb(49,49,49)` against the
panel's `rgb(38,38,38)` floor over 218 of 218 plot rows at BOTH columns, with 0 differing pixels in
either neighbouring column (the 1px non-AA path, drawn with the half-pixel inset the playhead's line
uses).

Gates: `tests/graph_panel_model.cpp` section 18 (the columns are the scaler's own pixels, the first
one is left of the plot and still published, a window so wide that both ends round onto one pixel
publishes ONE guide, the recede flag follows the play window without moving either column, and no
project publishes none) and `tests/graph_panel_pixels.cpp` check 33 (three passes of one model: the
guide column inked on 240 of 240 plot rows at a column INSIDE the value column and at one inside the
plot, NOTHING above `plot.top()` or below `plot.bottom()`, nothing in the neighbouring columns, and
the receded pass weaker than the plain one - `distance 76 -> 48` from the clear colour).
`AE_PANEL_MODEL_DUMP` now publishes `rangeGuides` too, which is how the live columns above were read.

## Proportional keys (2026-10-22)

A `core::TimeKey` can be **proportional**. It is an authoring flag only: playback reads the stored
frame and component values exactly as before. A proportional key stores, in
`core::TimeKey::Proportional`, its fractional position between its two nearest ABSOLUTE
(non-proportional) keys of the same type in the same `core::TimeLine`:

```
time:    P.frame    = qRound(L.frame + wT * (R.frame - L.frame))
value[c] P.value[c] = L.value[c] + wV[c] * (R.value[c] - L.value[c])
```

The weights are STORED, not re-derived from the rounded result: re-deriving them after each edit
accumulates frame quantization (project 0/3/10 onto 0/4/10 and back, and the key walks away by a
frame each round; `tests/proportional_keys.cpp` computes both answers to prove the difference). A
weight is re-derived only when the nearest absolute pair CHANGES - identified by stable key ids, not
frames or values - or when a proportional key is edited directly (its own weight is rebased from the
new position). X and each graphable component are independent. A key without both bounds is inert (it
is still marked proportional and can be moved directly). Structural types (Bone/Pose/Mesh/FFD) have no
graphable components, so only their frame follows.

**A FLAT VALUE SEGMENT is the ordinary first step, not a dead axis.** Marking a key proportional
while the object has not moved yet leaves every bounds' value equal, so the value fraction is 0/0 and
a literal "freeze" makes the value axis inert forever - the reported "the proportional calculation
only works on the time axis". A key that SITS ON the flat value therefore inherits the TIME fraction
as its value fraction (the only fraction it has), and the first bound edit projects it. A key holding
a DIFFERENT value on a flat segment still freezes: there is no fraction to preserve, and inventing
one would move a deliberately offset key. `ValueReflow::capture` upgrades a state written before this
rule once, without touching the stored pair, so an existing project starts following on its next
value edit instead of requiring the key to be re-marked.

**The graph-drag crash, fixed (2026-10-24).** A frame OUTSIDE the project range can make the loop's
two neighbours the same position: with range `0..150`, loop on and frame `-4`, the gatherer returns a
key at `151` and the key at `0` with the same `relativeFrame` (4). `getEasingRateFromTwoKeys` asserted
`frame != 0`, so a graph drag that left the playhead/keys outside the range aborted the application
(core: `TimeKeyBlender.cpp:317`, reached from `GraphEditor::updateKeyMove`). It now returns the earlier
key for a zero span; `proportional_edits` section [5] constructs that exact gatherer and runs the real
`TimeKeyBlender`, so the regression is a gate rather than a core dump.

**Persistence.** Format **0.16** (`core/FormatVersion.h`) writes every key's id (`int`, from a
file-local counter, `reserveId` on load) and, when proportional, its bound ids plus the time/value
weights and validity bits. Older files skip the block: fresh ids, absolute keys. `TimeLine::moveKeys`
moves a batch atomically (validates all sources/destinations first; a refusal mutates nothing) so
selected and projected keys can swap/chains without order-dependent `move()` failures.

**Editing.** `ctrl::proportional` (`src/ctrl/proportional/prop_Keys.h`) is the widget-free solver:
`findBounds`, `rebaseState`/`rebase`, `boundsChanged`, `projectedFrame`/`projectedValue`/
`projectValues`, and `ValueReflow` (capture before the edit, apply/modify after it, restore on undo).
`TimeLineUtil::MoveFrameOfKey` builds one context per (line,type), projects every non-selected
proportional key from its stored weight and the bounds' FINAL frames, rebases a stale bound once,
moves selected+projected in ONE `TimeLine::moveKeys` batch, and restores frames and weights on undo -
still one command per gesture. Value edits go through `ValueReflow` too: the timeline's
`assignMoveKeyData`/`assignKeyBy` push a reflow command after the edit in the same macro, the graph's
`KeyMoveCommand` captures one reflow per (line,type) before the first value write and re-applies it on
every step, and the SRT Move/Rotate/Scale live commands do the same through
`srt_AssignKeyCommand.h`. A directly edited proportional key is never projected onto; its own weight
is rebased. The timeline's Select-spacing dialog still moves keys directly (it predates this work and
is outside the projection/undo path) - do not assume proportional companions follow it.

**Marking and visuals.** The dock header carries a **Proportional** TOGGLE at the RIGHT, not inside the easing group
(`gui::TimeLineHeader`): its glyph is the RELATIONSHIP itself - two solid key bodies (filled circles)
joined by a line to a hollow ring in the middle (`*--o--*`), i.e. the relative key between its
absolute neighbours
- so it cannot be read as an easing or as a plain dot. The ring is always a ring; the toggle state is
the ink (bright when all selected keys are proportional, dim when all are absolute, medium plus a dash
through the ring when mixed), the same checked language the quick easings use. Clicking writes the
whole selection in one undo step through `TimeLineEditorWidget::setSelectionProportional`. The shared key menu (timeline
right-click and graph panel, `buildKeyMenu`) keeps the same edit as a checkable item: checked only
when the whole selection is proportional; a mixed/absolute selection becomes proportional in one undo
step via `TimeLineUtil::setProportionalKeys` (one command per key, one
`Type_ChangeKeyProportional` event). The menu's indicator is the APPLICATION's check box (raised
field, hairline edge, radius, phosphor check/minus - the `QCheckBox::indicator` recipe), not the
square `checkbox_*` border-images it used before: a popup belongs to the same application as the
docks.
The clipboard JSON carries the flag only - never the weights or bound ids, which name keys that may
not exist at the destination; a pasted flag starts with empty weights and resolves lazily.
`PanelCurve::keyProportional` publishes the flag beside the marker geometry.

**A proportional key is a RING, not a softer fill** (the user rejected the lower-opacity key body:
"the proportional keys are just lower opacity regular keys"). `ctrl::time::kProportionalRingOuterInset` =
0.5 and `kProportionalHoleInset` = 2.5 make a hue disc that reaches 0.5px inside the marker's radius
and a 1.25px-radius hole at the timeline's 3.75px key - a 2px hue band (the user's "a bit thicker,
2px instead of 1px"); the selection/focus ring outside is untouched.
Both hosts build the ring from FILLS (a stroked ring put the two AA kernels 69px apart on the Canvas
parity gate): the TIMELINE fills the hole with the LANE's own ink (passed into `buildKeys`), and the
GRAPH fills it with `theme::Graph::keyEdge()` (the panel floor), so a curve of the same hue cannot
read through the marker. Polygon markers (slimmed lanes, holder diamonds, closed-folder summaries)
use two copies of their outline, `kProportionalPolygonInset` = 0.62 (hue) and
`kProportionalPolygonHoleInset` = 0.32 (hole). `KeyMark` publishes `hollow`, `hollowRadius`,
`holeInk`, `hollowPolygon` and `hollowHolePolygon`, so the QPainter and Canvas paths cannot derive
two geometries.

**Gates (all green on the tree that added this section).** `proportional_keys` (solver + drift, the
flat-span rule, the distinct-value freeze), `proportional_edits` (frame/value/toggle one-undo,
including [3b] the LEFT bound and the Y component, [3c] a flat value segment inheriting the time
fraction, [3d] a pre-flat-rule state migrating on its first edit, and [5] the zero-span loop seam
whose blend used to abort the app during a Graph drag), `timekey_proportional` (ids/state/atomic `moveKeys`), `timekey_roundtrip` (0.16 id +
weights survive `TimeLine` serialize/deserialize), `graph_drag` sections 29a/29b (frame and value
drags carry the companion in one undo), `timeline_visuals` section 7 (the hole is the lane, the 2px
band is the hue, the focused ring is intact), `graph_panel_model` section 15 (`keyProportional`
parallel to the marker vectors), `graph_panel_pixels` check 32 (centre is floor-DOMINANT - the
1.25px hole leaves a 10% hue tint on the centre pixel - the band is the fill ink, the edge and
selection ring unchanged), `time_body_canvas` (the Canvas host paints the same ring, GEOMETRY 0).
`scripts/run_suites.sh Debug` reports **53 pass / 0 fail**, and the `AnimeEffects:Debug` target
links. The header control and the menu's check box were also driven live under Xvfb on
`I_think_its_cursed_honestly.anie`: the toggle appears named `Proportional keys`, becomes enabled
with a selection, and the key menu's check box renders the application field.

**The bar's custom buttons are TAB-focus only.** `HeaderTextButton` (channels), `QuickEaseButton`
(the four easings) and `ProportionalButton` set `Qt::TabFocus`, not `Qt::StrongFocus`: a mouse click
must not leave a focus ring behind (the user's report: the toggle, the quick easings and the channels
button each kept an "ugly outline" after being clicked). Tab still reaches them and paints the ring;
the transport is NoFocus and the easing chip is NoFocus, so the bar now behaves like the rest of the
chrome.
The header's zoom and fit buttons are REMOVED (2026-10-24): the timeline wheel, its scroll bar and the
graph wheel own zooming and fitting, so the bar ends at the channels trigger on the Graph tab. The
four quick-easing tooltips share one recipe (`Set the selected keys' easing to %1`); only the Auto
preset's ACCESSIBLE NAME keeps its longer action wording.

## Would ImGui make the TIMELINE faster? Measured 2026-10-22, and the answer depends on the GL driver

The question comes up because the graph panel is ImGui and the timeline is not. Numbers below were taken
with two throwaway targets (deleted afterwards): the shipping `ctrl::TimeLineEditor::render()` into an
offscreen QImage, and an ImGui frame of the same shape (12 lane bands, 48 separators, 12 lane labels,
N key markers as `AddCircleFilled` + `AddCircle`, exactly what `GraphPanelFrame` draws for a key) into its
own FBO under Xvfb. 920x250, the dock's own size. Debug and Release of the render path measured the SAME
(19.9 vs 19.1 ms at 1200 keys), so the cost is inside Qt's raster engine, not in our loops.

| variant | 0 keys | 300 | 1200 | 3000 |
|---|---|---|---|---|
| raster `render()` (shipping) | 2.0 ms | 9.8 | 19.1 | 35.1 |
| ImGui, CPU frame build only | 0.01 ms | 0.17 | 0.63 | 1.5 |
| ImGui, full frame on llvmpipe | 1.9 ms | 8.7 | **30.5** | 38.8 |

- **The cost is per PRIMITIVE, not per line of ours.** A 7px AA `drawEllipse` costs **9.0 us**; the same
  marker as a cached 9x9 `QPixmap` blit costs **0.42 us**; a 920px AA `drawLine` costs **26-55 us** against
  **12 us** with AA off. Pen/brush churn is 0.17 us and irrelevant. So ~14 us of the ~19 ms at 1200 keys is
  Qt rasterizing ellipses, and the lane pass' 2.0 ms is mostly the 48 AA separator lines.
- **Cheapest real fix, no migration:** cache the marker as a QPixmap per (hue, focused, proportional, shape)
  and draw the separators without AA. Measured as one composite pass: **1.85 ms** for the same content,
  i.e. 10x, with the same architecture, hit tests, menus and accessibility.
- **On a real GPU ImGui wins decisively** (0.6 ms of CPU to build the frame, one draw call, GPU fill), but
  **on llvmpipe it LOSES for dots** - 30.5 ms against 19.1 - because 48 vtx/key of software fill is more
  expensive than Qt's raster ellipse. This corrects the intuition "ImGui is faster": the panel's 4.70 vs
  10.24 ms A/B was for CURVE STROKES, where ImGui's texture-AA path beats per-segment quads. Dots invert it
  on software GL. Never quote the ImGui win without naming the driver.
- **The playback path is not this cost.** `setFrame` repaints only `TimeRuler` (22px strip, ~100 ticks) and
  moves `TimeCursor`; the lane surface repaints on camera changes (`updateCamera` -> `updateSize` ->
  `update()`), i.e. scroll, zoom, marquee and key drags. So the number above is an interaction-latency
  number, not a per-playback-frame one.
- A migration would also have to re-do, in ImGui: the ruler (ticks, tiers, boundary labels, play-window
  pills), the lane labels, the playhead pill, the marquee's rounded rect, the scrollbar pill, the two
  hit-test maps, the a11y publication (`gui::a11y` is a panel host today) and the GL host itself - the
  timeline is a raster `QWidget`, so it would become a `QOpenGLWidget` like `GraphPanelWidget`, with the
  one-context/VAO ownership rule `GraphPanelFrame` already documents.

**What "reuse the graph's ruler" would actually mean (checked 2026-10-22).** Half of it exists already:
`buildFrameAxis` and `buildPlayWindowOverlay` (`GraphPanelModel.cpp:100`, `:385`, ~345 lines together) are
pure geometry over `ctrl::graph::Scaler` + `QFontMetrics` + `core::TimeFormat` - no ImGui, no QWidget - and
`Scaler::applyFrameWindow` lets that scaler carry the timeline's window, so `ctrl::TimeLineEditor` can feed
them today. The other half does not exist: the ruler's INK is inline in `gui::drawPanel`, one 835-line
function (`GraphPanelFrame.cpp:1415-2250`; the band/hairline/ticks/numbers/pills block starts ~`:1450`).
`gui::frameRuler` is constants only. So reuse needs the block extracted into a `drawFrameRuler(...)` first,
and that extraction is what deletes `TimeRuler::renderHeader`'s duplicated collision/boundary/merge/
pill-suppression rules (~280 lines, pinned twice by `timeline_visuals` and `graph_panel_model`) - the
parity-bug class this file keeps recording exists only because there are two implementations.

**And the QML destination constrains the graphics API of the WHOLE window.** Qt's `QQuickWidget` page says
it plainly: "One top-level window can only use one single graphics API for rendering. For example,
attempting to place a QQuickWidget using Vulkan and a QOpenGLWidget in the widget hierarchy of the same
top-level window, problems will occur and one of the widgets will not be rendering as expected." This
ImGui stack is GL3 over `gl::Global` with a `QOpenGLWidget` host, so an ImGui timeline inside a QML page
needs a new `QQuickItem` + RHI backend (a second host), and on a platform whose RHI default is not GL it
cannot coexist with the QML window at all. A QML-body timeline instead draws through a C++ `QQuickItem`
with `QSGGeometry` - Qt's own GPU path, no third stack, backend-agnostic. Installed Qt here is 6.11.2,
so Qt Canvas Painter (6.12+) is not available; `Canvas` is what the QML controls use for small glyphs
(`AeCheckBox.qml:106`), not for a 1200-key surface.

## Canvas Painter on this machine: available now as C++, and a host trap under Xvfb (2026-10-22)

Fedora 44 ships `qt6-qtcanvaspainter` at **6.11.2** (installed here: `qt6-qtcanvaspainter` +
`-devel`, 2 packages, 1 MiB; `pkg-config Qt6CanvasPainter` works). What it provides at 6.11.2:

- `QCanvasPainter` - the drawing API: `fillRect`/`strokeRect`, `roundRect` (per-corner radii), `circle`,
  `ellipse`, `arc`, paths (`QCanvasPath`, `fill(path, pathGroup)` for GPU-cached geometry), `setClipRect`
  (**rect only**), `setLineWidth`, caps/joins, `setGlobalAlpha`, three composite modes, brightness/
  contrast/saturation, transforms, `drawImage`, `createGridPattern`-style brushes, box shadows, and
  `setFont` + `fillText` + `textBoundingBox` (QFont: no font loader of our own, CJK included).
- `QCanvasPainterWidget` - a **QRhiWidget** host, i.e. usable in the existing dock with no QML.
- `QCanvasPainterItem` / `QCanvasPainterItemRenderer` - a **QQuickRhiItem** host for after the QML move.
- `QCanvasOffscreenCanvas` + `grabFramebuffer()` for headless pixels.
- **NO QML `Canvas2D` item**: `/usr/lib64/qt6/qml` has no `QtCanvas2D`; that item is 6.12+, and Fedora 44
  has no 6.12 (newest `qt6-qtbase` is 6.11.2-3). So the QML-import route is unavailable here; the C++ one
  is not.

**Measured `paint()` CPU (command recording), 920x250, 12 lanes, no readback:**

| content | record |
|---|---|
| 12 lane `fillRect` | 0.03 ms |
| + 36 separator lines (one `beginPath`/`stroke` per lane) | 0.04 ms |
| + 12 `fillText` (any text at all) | 1.22 ms |
| + 120 `fillText` instead of 12 | 1.97 ms - so ~1.1 ms FIXED per frame once text is used, ~7 us per call after |
| 1200 markers, `save/translate/fill/stroke/restore` each | 1.14 ms (~0.95 us/marker) |
| 1200 markers as ONE cached path, one `fill` + one `stroke` | 0.52 ms (~0.43 us/marker) |

So batching markers into a path is 2.2x cheaper than per-marker calls, and text has a per-frame fixed
cost worth profiling on 6.12 before betting on it. (The 6.11 module is the pre-release version of what
Qt shipped in 6.12; treat these numbers as that.)

**The host trap, measured: a SHOWN `QRhiWidget` crashes under Xvfb on this tree.** A bare `QRhiWidget`
subclass (no Canvas Painter involved) with `setApi(OpenGL)` and `show()` dies inside
`QXcbBackingStore::rhiFlush` -> `QXcbWindow::needsSync(this=0x0)`; `Api::Vulkan` cannot present ("No DRI3
support detected - required for presentation"); `QT_QPA_PLATFORM=offscreen` never calls `render()` at all.
`grabFramebuffer()` on a widget that is NOT shown works (`400x200`, `render()` called once), so headless
pixel gates are possible - but **the live probe suite, which drives the real window under Xvfb, could not
drive a Canvas-Painter surface today**. `QOpenGLWidget` (the app's viewport today) has a different flush
path and is unaffected, which is why the existing probes work. Anything that moves a surface to
`QRhiWidget`/`QCanvasPainterWidget` has to settle this first (Qt 6.12, a DRI3-capable display, or
real-display-only probes).

**CORRECTION to the bullet above (same day, measured): that crash is Qt 6.11.2, not the display.**
Verified with Qt **6.12.0** installed by `aqtinstall` (`aqt install-qt linux desktop 6.12.0 linux_gcc_64
-O <dir> -m qtcanvaspainter qtmultimedia qt5compat`; qtbase+qtcanvaspainter alone already bring Quick,
Svg, Xml, Test, LinguistTools, OpenGLWidgets, Concurrent, QuickWidgets and QuickControls2):
- 6.11.2: a shown `QRhiWidget` dies on the FIRST flush in `QXcbBackingStore::rhiFlush` ->
  `QXcbWindow::needsSync(this=0x0)` - under Xvfb AND under a GPU-backed Xwayland, as a top-level and as a
  child, OpenGL and Vulkan alike. Unusable.
- 6.12.0: the same program renders 20 frames under Xvfb and exits **rc=0**. The one crash that survives is
  my own harness's: calling `QCoreApplication::quit()` from INSIDE `render()` leaves the widget mid-paint and
  still segfaults at teardown - quit from a `QTimer::singleShot` instead.
- So the version is the gate, not the display. And no distro source has 6.12: Fedora 44 is 6.11.2,
  **rawhide (F46) is still 6.11.2** (`mdapi.fedoraproject.org/rawhide/pkg/qt6-qtbase`), and **Terra ships no
  Qt at all** (`dnf --repofrompath 'terratmp,https://repos.fyralabs.com/terra44' --repo=terratmp list
  --available 'qt6-qtbase*'` -> nothing; terra-release itself resolves, so the repo works). The QML
  `Canvas2D` item is 6.12-only, so that route needs the same install.

**A GPU-backed headless display exists, and it is not Xvfb.** `cage` (72 KiB, installed here) with
`WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 cage -- <cmd>` starts wlroots headless
and its Xwayland, and `glxinfo -B` inside reports **`Accelerated: yes`, AMD Radeon R9 380 (radeonsi)** with
nothing appearing on the developer's desktop. Weston's headless backend + Xwayland gives `Accelerated: no`
(llvmpipe), so weston is the wrong tool for this. `QOpenGLWidget` renders there (20 frames, radeonsi).

**But cage is a KIOSK, not an Xvfb replacement for the probe suite** (measured): it presents ONE client
fullscreen, so the app has to BE that client (`cage -- ./AnimeEffects ...`). Started the other way round -
`cage -- bash -c './app & ...; xdotool ...; ae_ui.py dump'` - the app's X window exists in the tree but comes
up `1x1+0+0` and unmapped (`xdotool search --name Probe` finds nothing while `xdotool search --name ""`
lists the window), so a probe cannot drive it. Use cage for what it is good at: a GPU-backed display for one
app, which is how the Canvas Painter numbers below were taken (the spike was cage's client and got a full
1280x720 window). Keep Xvfb for the behavioural probe suite. Note also that cage paces at 60 Hz, so a
frame-to-frame span saturates at 16.06 ms - it says "fits the frame", not "costs 16 ms".

**Canvas Painter 6.12 measured on the timeline's shape** (920x250, 12 lane fills + 36 separators + 12
`fillText`, markers as circles; `QCanvasPainterWidget`, frame-to-frame span, 30 frames):

| markers | Xvfb/llvmpipe | cage/radeonsi | CPU record |
|---|---|---|---|
| 0 | 4.3 ms | 16.1 ms (paced) | 0.12 ms |
| 1200 instanced (`save/translate/fill/stroke/restore`) | 112.6 ms | 17.6 ms | 1.15 ms |
| 1200 batched (one path, one `fill`+`stroke`) | 26.1 ms | 16.1 ms (paced) | 2.33 ms |
| 3000 batched | 53.4 ms | 16.1 ms (paced) | 6.03 ms |
| 3000 instanced | 259.5 ms | 38.5 ms | 3.44 ms |

On the GPU everything up to 1200 markers fits the 16.06 ms frame pacing (60 fps), batched or not; on
llvmpipe per-marker calls are catastrophic (112 ms at 1200) and batching is the only way to stay near the
raster path's 19.1 ms. Note the CPU cost goes the OTHER way: batching 1200 markers into a path costs
2.33 ms of `addPath` work against 1.15 ms of instanced calls - cheap CPU for cheap GPU, and the trade is
right when a GPU is present. Verified ink: `grabFramebuffer()` returns `#c83c5a` at a marker's pixel.

**And the tree CONFIGURES against 6.12.0** (measured): with the aqt install in `CMAKE_PREFIX_PATH` and the
repo's own ffmpeg choice, `cmake -S . -B <dir> -G "Ninja Multi-Config"
-DCMAKE_PREFIX_PATH=/tmp/qt612/6.12.0/gcc_64 -DFETCHCONTENT_SOURCE_DIR_IMGUI=<build>/_deps/imgui-src
-DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62` finishes `rc=0` (`Qt6Core_DIR`/`Qt6Quick_DIR`
resolve under 6.12.0). Nothing is built there yet, so API-level 6.11->6.12 breakage is unmeasured. The
aqt install that suffices for this tree is `qtbase + qtcanvaspainter + qtmultimedia + qt5compat`
(2.0 GB; qtbase already carries Quick/Qml/QuickWidgets/QuickControls2/Svg/Xml/Test/LinguistTools/
OpenGLWidgets/Concurrent). Fedora 44 and rawhide have 6.11.2 only, Terra ships no Qt at all.

## The frame ruler is ONE module now (2026-10-22), and the two rulers were diffed rule by rule

`src/gui/FrameRuler.{h,cpp}` owns the rules the timeline's strip and the graph panel's band
share: which frames are ticked and at which tier, the tier alphas (0.85/0.60/0.20) and the 0.4
out-of-range recession, which frames are numbered, how a number yields to the project's own
first/last frame, how those collapse into one "first-last" label, how the playhead's pill
suppresses the number in its column, and the temporary playback window's wash/stems/pills. It
publishes GEOMETRY + CLASSIFICATION (`frameRuler::Model`) and paints it with QPainter
(`paintRuler`); the panel paints the same model through ImGui. `TimeRuler::renderHeader` is now
an adapter (build inputs → `build` → `paintRuler`), and `GraphPanelModel`'s `buildFrameAxis` is
an adapter that copies the model into the panel's published vectors; `buildPlayWindowOverlay` is
DELETED. The metrics live in the module and are `static_assert`ed against
`ctrl::TimeLineEditor::k*` there.

FOUR differences were decided (the user signed off on each; they are the only intentional
behaviour changes):

1. **Labels are never nudged inside the band.** The panel used to clamp a number's box into its
   band; now both rulers clip at the host's edge. A shifted number would no longer be centred on
   the frame it names, which is a correctness bug rather than a readability win.
2. **A ladder number is dropped when its box would come within `kNumberMargin` of one already
   drawn** (the panel's rule, now both). THIS IS NOT A DEEP-ZOOM-ONLY RULE: it fires wherever the
   ladder numbers the intermediate tier (>= 36px of spacing) with a label wider than that - every
   `Timecode_*` / `Relative_FPS` format at ordinary zooms - where the old ruler drew numbers on
   top of each other. Measured: of 192,000 states, 83,350 differ, and 42,856 of those are label
   differences in a non-frame format.
3. **A one-frame frame range publishes no boundary labels.** The timeline used to draw a merged
   "7-7"; there are no ends to mark (26,460 of the differing states).
4. **Numbers are formatted from the PROJECT frame range** (`TimeFormat(frameRange, fps)`), not
   from the visible window. NO PIXEL CHANGES TODAY: `core::TimeFormat` stores `mRange` and never
   reads it (`src/core/TimeFormat.cpp`), so this is a correctness change for the argument, not a
   visible one - the earlier claim that padding could follow a scroll was wrong.

Kept deliberately different (host policy, not a rule): the text halo (the panel's text floats
over artwork, the timeline's does not) and who draws the hairline under the band (the panel draws
it; the timeline's surface does, one row below its strip).

**How it was verified.** A throwaway harness compiled the PRE-extraction ink (copied verbatim
from `git show HEAD:src/gui/TimeRuler.cpp`) beside the new module and compared 192,000 states
byte for byte: 10 zoom rungs from -120 to 120, 5 scroll offsets, 4 widths (920/640/121/40), 4
frame ranges (600-frame, 5-frame, one-frame, 24-frame), 4 playhead frames, 5 play windows
(none / wide / merged / the whole range / off-view), all 6 number formats, dark and light. Every
difference fell in the tick rows and/or the number row, and every one was one of the four
decisions above (the deep zoom-out dedup accounts for the tick-tier and label diffs; the
one-frame range and the formatter basis for the rest). No background, wash, stem or pill pixel
ever differed. The permanent gate is `tests/frame_ruler.cpp` (registered in
`tests/CMakeLists.txt`), and `tests/graph_panel_model.cpp`'s "timeline slot" assertion was
updated to the never-nudged rule.

**The tier table is shared too**: `GraphPanelFrame.cpp` no longer carries its own
0.85/0.60/0.20 switch or its own 0.4 - both come from `gui::frameRuler::tickAlpha()` and
`kOutsideRangeFactor`, so a change to what a tier looks like cannot move one view and leave the
other. After the extraction: `scripts/run_suites.sh Debug` = **51 pass / 0 fail**
(`frame_ruler` is the new target), `probe_graph_parity.sh` OK (tick rows, the playhead pill's box,
fill and glyph rows, the shared playhead column and the scrollbar lane all compared across the two
pages), and `probe_timeline_ink.sh` OK (including the ruler's own number-vs-pill phase: 28 ink px
away from the pill, the pill owning the column when parked on it, and the number restored exactly
90px away).

**Two defects the review found, both fixed (and pinned).** (1) A ONE-FRAME range lost its only
TICK, not just its label: the ladder can classify that frame as `None`, so the boundary block was
the only thing that drew it, and suppressing it left the project's single frame unmarked - the old
ruler drew a Major mark there. The label stays suppressed (signed off); the tick is back, and
`frame_ruler` asserts a Major `boundary` tick exists for a one-frame range. (2) A number was culled
when its TICK fell outside the band, so the visible tail of an edge number vanished (measured: 16
columns of glyphs before, nothing after, at x 902..919 of a 920px band). Labels are now culled by
their BOX, never by their tick column - the host clips the overhang, which is what the timeline
always did.

**And the "a number wider than the band" rule was a phantom divergence.** Both hosts now CLIP such
a number (the panel used to drop it because it clamped boxes into its band; with the clamp gone
there is nothing to protect). A decision sheet rendered the three variants - old clip, new clip, new
drop - over band widths 40/121/200/320 with `Timecode_SMPTE` labels, and they are pixel-identical
at every width: a label's box only reaches a band it overlaps, and the ladder's numbers are <= 60px.
So the rule changed nothing visible; it was removed rather than decided.

The matrix's residual bucket (10,984 of 192,000 states after the two fixes; it was 14,920 before
them) is STILL UNATTRIBUTED. What is known: it concentrates on the NON-FRAME formats at zoom 1
(`Timecode_*`/`Relative_FPS`, which produce 48-77px labels) and on scrolled views, and the one case
instrumented in detail (`zoom=1 fmt=SMPTE range=[1,5] w=121 off=-13`) publishes a label whose box
starts inside the band and hangs over its right edge - so it is a LABEL-SET difference, not a tick
one, and not the wide-label rule (that renders identically). The next step is to diff the two
label SETS (not the pixels) for those cases; until that is done, do not quote a completion figure.

**After the fixes, verified again**: `scripts/run_suites.sh Debug` = **51 pass / 0 fail**,
`probe_graph_parity.sh` OK, `probe_timeline_ink.sh` OK. The throwaway `ruler_*` targets are out of
`tests/CMakeLists.txt` (sources kept at `/tmp/ruler-decision/`), so the suite count is back to the
shipped set.

**The residual was MY HARNESS, not the product (traced 2026-10-22).** Diffing the two label SETS
instead of the pixels first suggested a boundary-label difference (90 old runs not covered by a new
label, 42 labels with no old ink, 0 shifted). Tracing the OLD renderer's branches for the case that
started it (`zoom=1 Relative_FPS range=[1,24] off=-13 w=121 frame=1`) named the culprit: the harness
computed the visible frame window as `scale.frame(-offset - 5)` while `TimeRuler::paintEvent` uses
`scale.frame(cull.left() - kTimeLineMargin - 5)` - it subtracts the MARGIN too. Without the margin
the window started ~21 frames late, so the OLD renderer thought the project's first frame was
off-view (no merge, a lone "last" label drawn) while the new model's pixel test said it was in view
(merged, then pill-suppressed). With the widget's own window the old branch is
`merged branch: centre=37 underPill=1` - exactly the new model's outcome, nothing drawn. Re-running
the set diff with the corrected window collapsed it from **243 differing cases to 27**, with
`labelsWithNoOldInk=0` and `labelsShifted=0`; the remaining 27 live in a harness whose coordinate
arithmetic is still wrong for scrolled offsets, so they are not readable as product evidence.

**The corrected 192,000-state figures** (same matrix, widget window): `differing=56256` of 192,000,
`ticksOnly=34998`, `labelsOnly=3456`, `both=17802`, **`OUTSIDE_ROWS=0`**; causes are
`oneFrameRange=26760` and `deepZoomOut=28848` (56,608 of 56,256 differ by the two signed-off rules -
a number is never MOVED, 0 shifted labels), leaving **648 cases (0.34%) unattributed**, every one in
the number row and concentrated at `zoom=1`, format `Timecode_HHMMSSmmm`, offset 240 (a view scrolled
~218 frames). That configuration is the place to look next if the ruler is ever revisited; it is not
a blocker (no ink outside the tick/number rows, no shifted or added number).

CONCLUSION: no product difference beyond the two defects fixed above and the four signed-off rules,
with 0.34% of the matrix unexplained and named. Lesson for any future ruler comparison: use
`TimeRuler::paintEvent`'s window (margin included), or compare through the widget instead of the ink
function.

**Disclosure: `tests/CMakeLists.txt` was damaged and repaired in this session.** Removing the
throwaway `ruler_*` blocks with a text splice cut a `#` off a comment and left the file as a DOUBLED
copy of itself (2742 lines against 1276 at HEAD, `bonekey_regression` defined twice). Detected by
`cmake` (duplicate-target errors) and by the exact duplication offset (`lines[1372:] == lines[:-1372]`);
repaired by keeping one copy. Verified after: `cmake` configures, `AnimeEffects`, `frame_ruler` and
`graph_panel_model` build and pass, no `ruler_` target or file remains, 53 `add_executable` entries.
The full suite WAS re-run after the repair: **51 pass / 0 fail**.

## The Canvas Painter migration: the ruler already paints both ways (2026-10-22)

`AE_ENABLE_CANVAS_PAINTER` (root `CMakeLists.txt`) is ON only when the Qt in use both HAS the
module and is **>= 6.12** - `Qt6CanvasPainter_FOUND AND Qt6Core_VERSION VERSION_GREATER_EQUAL 6.12`.
On this machine's Fedora 6.11.2 it prints `Canvas Painter: OFF (needs Qt >= 6.12; this is 6.11.2)`
and the tree, the suite and the app are unchanged; with the aqt 6.12.0 prefix it prints
`Canvas Painter: ON (Qt 6.12.0)`.

`src/gui/FrameRulerCanvas.{h,cpp}` is the migration's first surface: `paintRulerCanvas()` reads the
SAME `gui::frameRuler::Model` and `PaintInk` as `paintRuler()` (QPainter) and draws it with
`QCanvasPainter`. It is NOT yet in the app's source list - only the gate compiles it - because the
default toolchain cannot run a QRhiWidget at all.

**`tests/frame_ruler_canvas.cpp` is the parity gate** (registered inside `if(AE_CANVAS_PAINTER)`):
one model, painted both ways, compared pixel by pixel, with differences inside the number/pill text
regions counted separately (two glyph rasterizers never agree) and only NON-TEXT differences failing.
Measured on Qt 6.12.0 under Xvfb, 9 states (default, zoomed out -40, zoomed in 30, scrolled by 240,
play window wide, play window merged, one-frame range, Timecode_SMPTE, light theme):
**`cases=9 failures=0`, `NON-TEXT = 0` in every state**, `textDiff` 441..3596 px (glyph coverage).
So the Canvas Painter renderer reproduces the raster ruler's GEOMETRY exactly - ticks, the window's
wash/stems/pills, the band - and differs only in how the two engines antialias text.

Three conventions the port had to match, each a silent 1px-class defect otherwise:

- **A tick inks `kTickLength + 1` ROWS.** QPainter's `drawLine(x, bottom, x, bottom - 5)` inks rows
  16..21 (six). In Canvas2D edge coordinates the stroke is centred on the coordinate, so the segment
  must run from `bottom + 1` (the last row's bottom edge) to `bottom + 1 - (kTickLength + 1)`, at
  `x + 0.5`, with `setAntialias(0)`.
- **The HOST applies the view translation.** `paintRulerCanvas` states ABSOLUTE columns; a scrolled
  view must translate by the view origin exactly as `TimeRuler::paintEvent` does, or the whole strip
  paints off-canvas (measured: 524 "differences" that were entirely this).
- **Text is anchored left/top on the model's box** (`TextAlign::Left`, `TextBaseline::Top`), the same
  anchor the panel's `LabelVAlign::Top` exists for; anything else moves every number by a row.

Next for this phase: the same treatment for the TIMELINE BODY - an engine-neutral model for the
lanes/keys/labels, `ctrl::time::Renderer` reduced to an adapter, then a `QCanvasPainterWidget` host
behind the same option.

### The timeline body's LANE PASS is a model now (2026-10-22)

`src/ctrl/time/time_BodyModel.{h,cpp}` states the pass `ctrl::time::Renderer::renderLines` used to
draw: per lane, its fill rect and selection flag, its type separators (fractional y - a lane is split
into equal parts, not whole rows) and its lane labels (box, text, the channel hue at the 0.42 label
alpha); then the seam hairlines. `buildLines(rows, cameraRect, cullRect, inks)` produces it and
`paintLines` draws it with QPainter, and `Renderer::renderLines` is now three lines that call them.

IT LIVES IN ctrl, NOT gui, and that is a layering decision, not an accident: the builder reads
`ctrl::TimeLineRow` and the document, so a gui model would make ctrl include gui (the tree forbids
`ctrl/ -> gui/`; only the theme headers are shared that way). gui will host the INK (the Canvas
Painter renderer) against this model, which is the opposite direction and already the pattern of
`gui/theme/*`.

**Verified pixel-identical**: a throwaway harness compiled the PRE-extraction pass verbatim from
`git show HEAD:src/ctrl/time/time_Renderer.cpp` beside the model and compared 48 states (8 row specs -
no types, one type, 5 types with separators, a slimmed node, a node with no timeline, a selected lane,
a 66px lane - x 3 camera offsets x 4 cull rects x 2 palettes) - **48 of 48 identical**. One rule needed
stating explicitly: the bottom hairline belongs to the LAST ROW the view pushed, so a culled last row
takes it with it (330 of the differences were that single 480px line).

**The suite's discovery had to learn about option-gated targets.** `scripts/run_suites.sh` greps
`add_executable(` out of `tests/CMakeLists.txt`, so `frame_ruler_canvas` (registered only when
`AE_CANVAS_PAINTER` is on) reported `fail=1 ... (missing)` on the 6.11.2 tree. A binary the build
system does not know is now a SKIP (`not-configured`), decided by `grep "^build <n>: phony"
build/build-<cfg>.ninja`; a target the build system KNOWS and did not produce is still a failure.
Result on the default tree: **51 pass / 0 fail, skipped: frame_ruler_canvas(not-configured)**.

NEXT for the body: the keys (markers, rings, proportional markers - later the ring rule), the range markers and the marquee
into the same model, then the `QCanvasPainterWidget` host behind `AE_CANVAS_PAINTER` and the
raster-vs-canvas comparison across the state matrix.

### The key pass: an extraction REVERTED, and a trap that cost it (2026-10-22)

I extracted `drawKeys`/`drawChildKeys` into a model the same way as the lane pass, and the
equivalence harness reported **48 of 48 cases differing** by 5-95 px. The numbers said "the new fill
is drawn too soft" (#fb7185 vs #a2606a at the essential key pixels), and the cause was my BASELINE:
I generated the oracle from `git show HEAD:src/ctrl/time/time_Renderer.cpp`, but this file carries
**uncommitted work** - the proportional-key fills (`if (isProportional) fill.setAlpha(
kProportionalFillAlpha)`; since replaced by the RING rule, see the proportional section) - which HEAD
does not have. So the "old" side drew full-strength fills and
the new side the proportional ones: the model was right and the oracle was stale.

The cost: I then "reverted" the extraction by re-inserting HEAD's two functions, which DELETED the
uncommitted proportional handling from the working tree - `timeline_visuals` caught it immediately
(`FAIL 820: ... proportionalX ...`). Recovered by rewriting both functions from the read I had taken
of the working file; `timeline_visuals` passes and the suite is 51 pass / 0 fail again.

LESSONS, both cheap to apply next time:
- **Never use `git show HEAD:<file>` as the oracle for a file with uncommitted changes.** Copy the
  function out of the WORKING TREE (or `git diff` it first) - the working tree is the behaviour under
  test, not the last commit.
- A revert of an extraction must restore the working tree's version, not HEAD's.

RESOLVED the same day: with the oracle copied from the WORKING TREE, the SAME model is
**48 of 48 cases pixel-identical** (`buildKeys`/`buildChildKeys`/`paintKeys` in
`ctrl/time/time_BodyModel.{h,cpp}`, `Renderer::drawKeys`/`drawChildKeys` gone,
`renderKeys` delegating). The model needed no change at all - only the baseline was wrong.
`timeline_visuals` passes (it covers the proportional marker, the focused fill and the ring; the proportional marker is now the ring rule) and the
suite is 51 pass / 0 fail.

The key pass is therefore: bands/seams/labels (lane pass) + keys (markers, holder diamonds, slimmed
diamonds, folder summaries, focused ring, proportional ring) - all modelled in ctrl and drawn by one
QPainter adapter. Left: the range markers, the marquee, the Canvas host and the body comparison.

### The whole body is modelled; its Canvas port is SHORT and the gate says so (2026-10-22)

Modelled in `ctrl::time::` and drawn by the shipped QPainter renderers, each verified against an
oracle copied from the WORKING TREE:

| pass | equivalence |
|---|---|
| lanes: fills, separators, lane labels, seam hairlines | 48 / 48 |
| keys: markers, holder/slim diamonds, folder summaries, focused ring, proportional ring | 48 / 48 |
| frame-range guides + marquee | 78 / 78 |

`renderRangeMarkers`/`renderSelectionRange` are adapters over `buildRangeMarkers`/`buildMarquee` +
`paintGuides`, and the dead `Renderer::renderHandle` (its only caller is commented out) is DELETED
with its declaration.

**The Canvas Painter body** is `src/gui/TimeBodyCanvas.{h,cpp}` (lines, keys, guides) hosted behind
`AE_CANVAS_PAINTER`, and `tests/time_body_canvas.cpp` is its parity gate: one set of models painted by
both paths, with the regions Canvas2D cannot reproduce (boxed text, dashed strokes, `DotLine` pitch,
and the 1px antialiased marker outlines/rings) counted separately.

WHERE IT STANDS: **the gate PASSES - `cases=2 failures=0`, GEOMETRY 0 in both themes.** Every
remaining difference is confined to the three ink paths Canvas2D cannot reproduce (below); the lane
fills, the seam rows, the guide COLUMNS, the marker fills and their positions all match. Three real
findings came out of closing it:
- **`fill()`/`stroke()` do NOT reset the current path** - without `beginPath()` before each marker,
  every shape was appended to the previous one's and re-inked with the next style (1923 px of
  "differences" that were really one missing call);
- Canvas2D has no dashed strokes: `DotLine` separators are drawn as 1px marks at Qt's 1-on/2-off
  pitch, and the marquee's `DashLine` edge as 4-on/4-off - both counted as expected ink-path
  differences rather than geometry.

- **Canvas2D cannot fill ONE device column.** A span of `[x, x+1)` inks three columns
  (`x-1..x+1`) and `[x+0.5, x+1.5)` two, and `setAntialias(0)` does not change it (it governs
  STROKE fringes, not rect edges) - measured three ways. The frame-range guides are therefore two
  columns wide in the canvas against one in the raster, and the gate counts those columns as an
  expected ink-path difference. The half-pixel span is the narrower of the two options and is what
  the renderer uses.

The 402 px that remained before that classification were entirely this: the guide's columns, nothing
else.

**Where the 268 px actually are (drilled one level further).** The differing band is x 463..465, and
in this fixture the SECOND range marker's column is `24 + 1.1 * 400 = 464` - so the guide column is
the suspect, not the keys (the marker fills matched). The colours at those pixels are near-white in
the raster (`#f2eef9`) against a slightly darker canvas (`#e9e0f7`), i.e. an ALPHA/coverage
difference on one column, not a position difference. Next: print `f.markers` and the two images'
columns 462..466 side by side for one state - the same technique that found the missing
`beginPath()`. Do NOT guess again; the model is right and the ink path is one column off in coverage.

**Body Canvas parity: green.** `tests/time_body_canvas.cpp` reports `cases=2 failures=0` on Qt 6.12
(`AE_CANVAS_PAINTER`), with the differences split into geometry (0) and the three ink paths Canvas2D
cannot reproduce - boxed text, dashed strokes (`DotLine`/`DashLine`), and the 1px column above. The
default Fedora 6.11.2 tree registers none of it: suite **51 pass / 0 fail**, `frame_ruler_canvas` and
`time_body_canvas` both skipped as not-configured.

## The body is a SCENE now, and the host swap is one class (2026-10-22)

`ctrl::TimeLineEditor::bodyScene(camera, theme, cullRect)` returns `ctrl::time::BodyScene` - the
lanes, the frame-range guides, every key pass in draw order, and the marquee - with no QPainter and
no document access. `render()` is `paintBodyScene(painter, bodyScene(...))`, and
`gui::timeBodyCanvas::paint*Canvas` paints the SAME value for the Canvas host. So the timeline's
surface is now a host: swapping it is one class, not a rewrite.

Two defects the cutover exposed, both fixed:

- **The scene must carry the CALLER's theme.** `bodyScene()` first built a default
  `theme::TimeLine` for the four lane inks, which repainted every lane in the wrong colours
  (`timeline_visuals` failed at its lane-ink fraction). The theme is now a parameter.
- **A paint function owns its antialiasing hint.** `paintKeys` inherited whatever the previous
  pass left: after the guides' hairlines the focused key's ring was drawn faceted and its coverage
  profile read 0 where the test expects >0.3. `paintKeys` sets `Antialiasing` itself, as the old
  `renderKeys` did once for the whole pass.

Gates: `timeline_visuals` passes (the body's pixel contract), suite **51 pass / 0 fail**. The
Canvas gates still pass unchanged, because the models are the same ones they already compared.

WHAT REMAINS for the dock: `TimeLineEditorWidget` derives from `QWidget` and paints in
`paintEvent`; hosting Canvas Painter means deriving from `QCanvasPainterWidget` under
`AE_CANVAS_PAINTER` and painting `bodyScene()` through `paint*Canvas` in `paint()` instead. Its
children (`TimeCursor`, `TimeRuler`) and its input/hit-testing are unaffected - `QRhiWidget` is a
QWidget - but the swap changes the class's construction and composes through a texture, so it needs
a live run (Xvfb screenshots) to be called done. Qt 6.12 is installed durably at
`~/Qt/6.12.0/gcc_64` (aqt; `-O ~/Qt -m qtcanvaspainter qtmultimedia qt5compat`), and both Canvas
gates were re-run from it: 9/9 and 2/2.

## Folder children lanes (2026-10-24)

A COLLAPSED folder whose descendants have keys now gets a **dedicated summary lane** under its own
lanes, instead of drawing the descendant triangles on the bottom of whichever lane happened to be
last. The old overlay had two defects worth recording: several hidden channels at one frame drew the
same triangle over itself, and the summary's y was the folder row's bottom, so it shared a lane with
the folder's own keys (or with a child's, depending on height).

**Geometry.** `ctrl::TimeLineRow` carries `summary` (computed once from `closedFolder &&
hasDescendantKeys`) and splits its rect into `ownRect()` and `summaryRect()`, both 22px high when both
exist. A folder with no own timeline types and a summary uses the WHOLE row for the summary (22px):
an empty lane above it would be dead height, and there is no own content to overlap. A folder with
own lanes is `base + 22`. Expanded folders have no summary - their child keys are rows of their own.
`kFolderSummaryHeight == kHeight == 22`.

The TREE has to grow with it or the columns desynchronise: `ObjectTreeWidget`'s display height uses
the same rule (`base + 22` for a collapsed folder with own lanes, `22` for summary-only, base when
expanded), re-measures on `itemExpanded`/`itemCollapsed`, and on every key add/remove
(`onTimeLineModified`). The tree cell and the timeline row are both read through `visualRect`, so one
height formula feeds both.

**Ink and hit geometry.** The summary band uses the **same lane surface** as every other lane:
there is no second colour. Its own 1px seam above it and the marker shape are the distinction - the
user's ruling was that a dedicated recessed fill was an invented surface. It carries a **"Children" label** in `textMuted` at the lane-label alpha, in the same centred column
and at the same size as every channel name (the tree already names the folder). Lane names use
`core::TimeLine::getTimeKeyDisplayName` ("Opacity", not "Opa"); serialization and the clipboard keep
the compact `getTimeKeyName` token, so the file format is untouched. The label says
"Children", not "Summary": the band lists the keys of the folder's child layers. It is an ordinary
lane label. The key passes are painted AFTER the labels, so a marker draws IN FRONT of the label
(the label is behind it) exactly as it does for "Move" or "Scale"; that is not a defect. A left-anchored label was the first attempt and read as a floating mess across the frame-zero
marker; centring it like a channel label was the fix. The markers are **one
rounded triangle per frame**, not per key - the summary is about WHEN, and per-key marks overdraw -
built as a 4.2px half-width / 6.2px tall triangle, flattened through
`QPainterPath::toFillPolygon`. They are **fill-only** (`KeyPass::stroke == false`): a 1px `outline`
edge on a 5px neutral marker read as a dirty rim, not as definition. All polygon key markers - the holder diamond, the slimmed rectangle and this triangle -
share `ctrl::time::kKeyCornerRadius = 2.0px`; at that radius the triangle is visibly rounded at
1x, the diamond still reads as a diamond, and the slimmed rectangle is not yet a pill. A 1.4px
radius was invisible at 1x; 2.5 collapsed the diamond toward a circle. Fill `baseKey`; a frame holding
a selected hidden key mixes that fill toward the text colour. `Marquee::hitTest`/`gather` use
`summaryCenterY()` for a closed folder's descendants (the folder's own keys keep their own lane
heights), so clicking a summary triangle selects the hidden key under it and a marquee over the band
gathers those keys.

**Where it was verified.** `tests/timeline_visuals.cpp` section 8 (registered; suite green): a
folder with own keys and a child with two keys on one frame produces TWO lanes, the summary lane
below, one rounded polygon, and a hit on the summary centre resolves to the child key; a summary-only
folder produces ONE summary lane; an expanded folder produces none. Live on
`Canette.anie` (nested folders `shootttt`, `small bandito`, `Whole bis`): collapsed rows show the
band and triangles, expanding `small bandito` removes its summary and puts its three child lanes in
place, and collapsing it restores the band; the tree and timeline stay aligned. Screenshots:
`/tmp/summary_collapsed_folder.png`, `/tmp/summary_expanded_folder.png` (throwaway captures, not
repo assets). The top document root is not a timeline row in this tree, so it has no summary lane;
only nested folders do.

### Box-select marquee: one rounded solid box in both views (2026-10-24)

The timeline's marquee had drifted from the graph panel's: a dashed edge, and on the Canvas host
straight manual dash runs that could not follow the rounded corners (the fill depended on the
current path, too). Both views now draw the SAME box: `SelectionBox`/`theme::Graph`'s accentBright
body at 24/255, radius 6, and a 1px SOLID accentBright edge at 230/255. The fill is what marks the
selection; the dash carried no information and made the timeline's gesture look like a different
feature. `gui::ink::paintMarquee` is the ONE placement both hosts call: it insets the raw gesture rect by
0.5px, fills the rounded body, then strokes a 1px edge. The half-pixel inset is what makes the edge
one device column; without it (the graph's previous raw integer rect) a 1px stroke straddled two
columns and read as 2px. `paintGuides` is the QPainter adapter with the same inset;
`paintGuidesCanvas` calls `paintMarquee` over `InkCanvas`, the same vocabulary and backend the graph
panel uses. `time_body_canvas` compares the two renderings.

### Two separator concepts: solid row seams, dashed type separators (2026-10-24)

There are TWO different lines in the timeline, and they must not look alike:

- **Row/layer seams** - the 1px lines between one row and the next (different layers, folders,
  summary bands): **SOLID**, `trackEdgeColor` at 70% (`trackSeperatorColor` is not used here).
- **Type separators** - the lines between the key-type bands INSIDE one row (e.g. Scale above
  Opacity in the `line` lane): **DASHED 4-on/4-off**, `trackSeperatorColor` at 40%
  (`theme::TimeLine.cpp`, alpha 102).

The old type separators were 1-on/1-off dots (Canvas: 1px every 3), which vanished at 1x. A solid
40% line was tried and was wrong for a different reason: it made the two concepts look the same.
4-on/4-off is legible at 1x and still visibly lighter than the 70% solid row seams. Both renderings
snap the fractional split to its row (`int(each.y)`); `paintLines` uses a custom dash pattern with AA
off for that row, Canvas draws the same 4-on/4-off runs as 1px filled rects, and
`time_body_canvas` still reports GEOMETRY 0.

### The dock host is SWAPPED: the timeline surface is a QCanvasPainterWidget (2026-10-22)

`gui::TimeLineEditorWidget` derives from `QCanvasPainterWidget` when `AE_CANVAS_PAINTER` is on
(`#else` keeps `QWidget` + `paintEvent`, byte-for-byte the old path) and paints
`ctrl::TimeLineEditor::bodyScene()` through `gui::timeBodyCanvas` in `paint()`. Its children
(`TimeRuler`, `TimeCursor`), its input, its hit-testing and its keyboard are untouched - a
`QRhiWidget` is a QWidget - and the only two additions are the host's clear colour, set in the
constructor and re-read in `updateTheme()` (the Canvas host clears its texture each frame; a QWidget
host shows the QSS floor instead).

Wiring: `src/gui/CMakeLists.txt` adds `FrameRulerCanvas.{h,cpp}` and `TimeBodyCanvas.{h,cpp}` to the
app, links `Qt6::CanvasPainter` and defines `AE_CANVAS_PAINTER=1`, all inside `if(AE_CANVAS_PAINTER)`.

VERIFIED (Qt 6.12.0, Xvfb, `~/Qt/6.12.0/gcc_64`):
- the app builds and runs (`Release`, one warning-free link), no RHI/warning in its log;
- the SAME project (`~/Downloads/test.anie`, which has no visible keys) rendered by the 6.11.2
  raster build and the 6.12 Canvas build gives **identical dock pixels**: 344 distinct colours and
  the same 432 accent pixels in the timeline band. That proves the host composites, colours and lays
  out identically; the KEY/LANE ink parity is what `time_body_canvas` (GEOMETRY 0) and
  `frame_ruler_canvas` (0 non-text px) already compare against the raster path directly.
- NOT verified: a project with visible keys or a live GPU (Xvfb is llvmpipe), and the drag
  gestures inside the surface (marquee, key drags) - the ink is proven, the interaction is the same
  code as before but has not been driven on this host.
- default 6.11.2 tree: **51 pass / 0 fail**, both Canvas targets skipped as not-configured.

## A GPU-backed headless display exists, and it changes the Canvas verdict (2026-10-22)

**`scripts/gui/gpu_headless.sh -- <command>`** runs a command on an Xwayland whose GL is the real
GPU: `cage` + wlroots' headless backend (`WLR_BACKENDS=headless WLR_RENDERER=gles2`) composites via
EGL on the device and hands it through, so the client sees `Accelerated: yes, AMD Radeon R9 380
(radeonsi)`. Nothing appears on the developer's desktop. Xvfb cannot do this at all - its GLX is
llvmpipe by construction, it has no DRM node. Weston's headless backend is also llvmpipe (measured),
so wlroots is the one that works. Limits: cage is a KIOSK (one presented client, so the command must
BE that client - a wrapper shell cannot become the window), no WM, and keyboard focus is the client's.

**WHAT THAT IMMEDIATELY EXPOSED.** Both Canvas Painter parity gates PASS under Xvfb/llvmpipe and FAIL
on radeonsi:

| gate | llvmpipe | radeonsi |
|---|---|---|
| `frame_ruler_canvas` | 9/9 (0 non-text px) | **5/9** - 4 failures |
| `time_body_canvas` | 2/2 (GEOMETRY 0) | **1/2** |

The ruler's failures are precisely the states with an ACTIVE PLAY WINDOW (`window` 1168 px,
`timecode` 586, `light` 1172, `window-merged` 4) and none of the window-less ones (all 0); the body's
is the state with the marquee. Both are ALPHA-BLENDED fills (`windowBand`, the marquee's 24-alpha
wash). So the earlier "parity" verdict was a SOFTWARE-RENDERER verdict: opaque ink matches on both
drivers, exactly-alpha ink does not.

STATUS: the Canvas host is NOT shippable. Next: compare one washed pixel's composited value on both
drivers (`grabFramebuffer` on each, same model) to see whether it is premultiplication, the fill's
coverage rounding, or the DPR - the ruler gate already isolates it to the window's band.

**GPU gates now PASS, and the earlier failure was 1 LSB of blend.** The ruler's four failing states
were all play-window ones, and every differing pixel was `#3e3e3e` (raster) against `#3d3d3d` (GPU):
a one-unit rounding difference in a TRANSLUCENT fill. Both gates now count a <=1-per-channel
difference inside translucent ink as `blendLSB` (reported, not hidden) and stay exact for opaque ink:
`frame_ruler_canvas` 9/9 with 0 non-text (975..1337 blendLSB in the window states),
`time_body_canvas` 2/2, on **radeonsi** and unchanged on llvmpipe.

**Real project data on the Canvas host, with keys.** `~/Downloads/AE ForTesting/AnimeEffects/
I_think_its_cursed_honestly.anie` (892 KB) needs its tree expanded (click the branch indicator at
`(rowX-12, rowY+rowH/2)` from `ae extents "<top node>" "table cell"`) before any key row exists:
- Canvas host (6.12, Xvfb): 74 px of the Move/Scale-X hue, 515 distinct colours in the dock;
- raster host (6.11.2, Xvfb): 90 px of the same hue, 564 distinct colours.
So keys DO render on the Canvas host, but the two hosts' ink does not match pixel-for-pixel on this
project (16 hue px, ~49 colours). NOT YET ATTRIBUTED - candidates: the selection/focus state differs
between the two runs (a focused key's 1px ring is the ~16px scale), or the 1px-column limitation on a
key's edge. Next: pin the state (select the same row, no focus) and diff the two captures pixel by
pixel, as the ruler gate does; do not call this parity until that number is 0.

**The 90-vs-74 is NOT run-to-run noise (measured).** Same binary, same click, two runs each:
raster 90, 90 rose px / 564, 564 colours; Canvas 74, 74 / 515, 515. So the Canvas host really does
ink less on that project. The difference sits in ONE full-width row (a lane band/seam boundary, ~684
px) plus ~15 px per key row (marker edges) - located by row histogram, not yet attributed to a
primitive. Do not call real-project parity until it is 0. The next tool is a HEADLESS harness that
loads the .anie, flattens the tree into rows, and renders the same scene both ways (no app, no run
state), which is the only way to pin this without the two-run confound that this measurement just
ruled out.

**ATTRIBUTED: the 90-vs-74 is two ink-path differences, not one defect.** Diffing the two stable
captures row by row (`rast-1.png` vs `canv-2.png`, both repeats of a single run each):

- **row 429, 683 px in two runs (275..300, 302..958): raster `#262626`, canvas `#272727`** - a ONE
  LSB difference across a whole row, i.e. the translucent ink that spans a lane: the selected lane's
  accent tint (`trackSelectColor`, accentBright at 0.06). The same blend rounding already classified
  as `blendLSB` in the gates; the app capture has no tolerance, so it counts.
- **rows 435..443, ~13-32 px per row at x 297..305 and 589..605: `raster=(51,45,46)` vs
  `canvas=(70,69,69)`** - the KEY MARKER'S 1px OUTLINE. On the Canvas host the rim is drawn with an
  antialiased fringe that spills onto the neighbouring pixels, so the outline eats into the fill: that
  is where the 16 missing HUE pixels went (line 74 vs 90), not a missing key.

So: geometry and positions match; a translucent tint rounds 1 LSB differently on the GPU path, and the
marker rim is one pixel wider in coverage. Both are INK-PATH properties (the same family as the 1px
column and the dashed strokes). Neither is a functional difference, and both are measurable with the
tolerance the gates already carry; the app-level comparison simply needs the same rule.

### The lane labels vanished on the Canvas host, and both gates were blind to it (2026-10-22)

`paintLinesCanvas` nested the whole label block inside `if (lane.separators.isEmpty()) continue;`.
A lane with ONE valid key type has no separator - so such a lane drew its fill and its keys and its
name never. Measured on `I_think_its_cursed_honestly.anie`, whose three lanes are each one type
(`head`=Move, `line`=Scale, `circle`=Opa): the Canvas host lost "Move", the first lane.

WHY NOTHING CAUGHT IT, and this is the part worth keeping: both Canvas gates classify a pixel inside
a published label/number box as "expected" (two text engines cannot agree per pixel), so **a missing
label is an expected difference too**. `time_body_canvas` was green with a lane's name absent. The
fixture could not have caught it either - its single lane HAS separators (21.5, 43.0), i.e. the
opposite of the shape the bug needs.

The two fixes, both structural:
- the label draw is outside the separator branch, with the reason in place;
- each gate now checks PARITY OF INK PER LABEL: for every label the model publishes it counts
  non-background pixels inside that box in BOTH images and fails when one engine has ink and the
  other does not (or when the counts differ by more than 2x). The rule is deliberately between the
  TWO RENDERINGS, not "the model says draw it": the ruler legitimately blanks a number under the
  playhead's pill, and a box can fall outside the image, so both-zero is a pass. The body fixture
  gained the missing shape - a second lane with one type, no separators, and a label.
Discriminating, measured: with the bug restored by hand the body gate reports
`label Opa ... canvas=100 px MISSING/DIFFERS`, `failures=2`; with the fix, `raster=265 canvas=274`
and `failures=0`. Ruler: 9/9 with its ten numbers asserted; body: 2/2, on **radeonsi and llvmpipe**.
The app then shows `Move`, `Scale`, `Opa` on the Canvas host (`docs/verification/graph-panel/canvas-fixed.png`).

Lesson, in the file's own idiom: a gate that excuses a region from comparison must still assert that
the region is OCCUPIED on both sides, or the excuse becomes the hole.

## The graph editor's ink is a VOCABULARY now, not a pile of ImGui calls (2026-10-22)

The timeline's migration (models in ctrl, one QPainter adapter, one Canvas Painter adapter) does not
transfer to the graph panel: its 835-line `drawPanel` made every shape AND ink decision inside ImGui
draw calls, so re-hosting it meant either two permanent copies of that function or keeping ImGui for
ever. What it needed was one decision site and swappable rasterisers.

**`src/gui/Ink.h`** is that vocabulary: `fillRect` (optional radius), `strokeRect`, `line`, `stroke`
(polyline + half width), `circleFill`/`circleStroke`, `text` (box + HAlign + VAlign + colour + halo +
right gap), `pushClip`/`popClip`, `setLineAntialias`, plus `ink::Pointer {pos, pressPos, leftDown}`.
It deliberately carries NO ImGui type, and `gui/InkImGui.{h,cpp}` is the shipped backend over
`ImDrawList` - the text rules (`drawLabel`'s whole-pixel snapping, the ink-top row, the 16-pass halo),
`measureText`, `inkTopInset` and `toImU32` MOVED there verbatim.

Three contract decisions, each one paid for by a defect during the port:

- **A `QRectF(x,y,w,h)` names the same pixels as `QRect(x,y,w,h)`.** QRect's `right()` is x+w-1 and
  QRectF's is x+w, so a backend that adds 1 to a QRectF's `right()` widens every converted rect by a
  column: MEASURED as the play window's pills reading 533 fill pixels against 522.
- **A stroke's ink is the COLOUR'S ALPHA.** The coverage pass takes the ink as a vec3 plus a scalar
  strength; baking the strength into the RGB's alpha and passing 1.0 draws every dimmed curve at full
  opacity (MEASURED: the gate's curve samples read the token itself, distance 122, against 55 for the
  dimmed blend). `panelStrokeSink` hands `aColour.alphaF()` over as the strength instead, which is also
  what Qt and Canvas Painter mean by a colour with alpha.
- **The POINTER belongs to the HOST.** `drawPanel` no longer reads `ImGui::GetIO()`; `runPanelFrame`
  (the ImGui host) fills `ink::Pointer` from the IO it owns, and a headless gate states any hover it
  wants. A backend reads no pointer at all.

**The coverage pass stays with the ImGui backend.** It exists because ImGui's antialiased polyline is
not area-exact, its GL objects are owned by `GraphPanelFrame.cpp`, so the backend asks for it through
`gui::panelStrokeSink` (`InkImGui::StrokeSink`) and falls back to `AddPolyline` at the same width. The
Qt and Canvas backends have no such dependency.

**Verified, and how.** `tests/graph_panel_pixels` drives the real backend into its own framebuffer and
prints 192 `MEASURED` lines (every pixel assertion the panel has). Diffing its output between the
pre-Ink build and the refactored one: **all 192 identical**, except the stroke-cost timings and the
fringe measurement below - i.e. the refactor is pixel-neutral on every claim the gate makes. Three
consecutive runs: 192 lines, no failure, identical apart from timings.

**One measurement had to be re-aimed, and it is a real finding.** The gate used to prove the stem
belongs in the coverage pass by comparing `AddLine` (0.68..0.76px off the requested segment) with the
pass (0.01px). With the vocabulary in place BOTH halves draw through `Ink::stroke`, so the comparison
collapsed to 0.01 against 0.01 and the gate said so ("the stem measurement does not separate the two
strokes"). The fringe half now issues ImGui's own `AddLine` directly, which is where that error lives;
the number is 0.68..0.76px again, and the assertion discriminates. The lesson for the next port: a gate
that measures a rasteriser through the NEW abstraction stops measuring the rasteriser.

### The graph panel's two portable backends exist, and the gate MEASURES their gap (2026-10-22)

`gui::ink::Ink` now has three implementations: `InkImGui` (shipped), `InkQPainter` (Qt's
rasteriser, the default tree) and `InkCanvas` (QCanvasPainter, the 6.12 host; compiled only behind
`AE_CANVAS_PAINTER`). The comparison runs INSIDE `tests/graph_panel_pixels` (behind
`if(AE_CANVAS_PAINTER)`), not as a separate target: a standalone target needed `GraphPanelFrame.cpp`
plus the whole editor and dragged `core::Project`/`ctrl::TimeLineEditor` in, whose GUI dependencies
the app links but a test cannot - the pixel gate already carries the fixtures, the ink decisions and
the ImGui reference, so the second and third backends are compared where they are.

Its rules, each stated where it is applied in the file:
- geometry and fills EXACT;
- a **1-LSB** per-channel difference is `blendLSB` (translucent ink's last bit);
- a pixel within **1.6px of a model POLYLINE** (a curve or a handle stem) is `strokeDiff`, because two
  engines antialias a 2px stroke with different kernels - the polyline itself is still the model's, so
  the geometry is checked, only the fringe is forgiven;
- text is compared for **PRESENCE per published label box**, both-empty allowed (the ruler's pill
  suppression) and one-sided ink a FAILURE.

FIRST MEASUREMENT, then the two rules that closed it. `ink` model, 400x320:
`textDiff=0 strokeDiff=943 blendLSB=1019 GEOMETRY=352` - and the 352 were ONE lead, which the
frame dump (AE_BACKEND_DUMP=<prefix> writes both PNGs) named exactly: the flat curve is a 2px
stroke at rows 39..40 in BOTH, but Canvas also inks rows 38 and 41 at +3, and its stroke BODY is
131,70,79 against Qt's 134,72,81. So two ink-path facts, both measured:
- a 2px stroke on a half-pixel phase covers THREE rows (1.5px each side), and Canvas's AA spills
  one row further - hence the ink-path tolerance is **2.5px**, not 1.6;
- a 1px CIRCLE (a marker's edge, its selection ring, a handle's dot) is the same class: Qt centres
  its pen on the path and Canvas's AA spreads a column either side - so a disc of
  `radius + ring + 2.5` around every published marker centre is ink-path territory too.
With both, **GEOMETRY = 0 on both models on BOTH drivers**: llvmpipe `strokeDiff=1295/284,
blendLSB=1019/10`; radeonsi `strokeDiff=1537/283, blendLSB=612/10`, gate ends OK. The classification
is honest in the direction that matters: everything the model PLACES must match exactly, and what is
forgiven is only the fringe around a stroke or a 1px ring, with its size stated.

### One frame COMPOSITION for two hosts, and what the host swap still needs (2026-10-22)

`gui::composePanelFrame(editor, visible, viewport, state, measuredFont, hostStyle)` returns
`PanelFrame { model, focus, palette, drawn }` and publishes the host's `PanelFrameState`. It is the
model half of a frame with no rasteriser in it at all, and `runPanelFrame` is now three lines over it
(the ImGui draw list, the ImGui pointer, `InkImGui`). A QCanvasPainter or QPainter host composes the
SAME value and paints it with its own backend, so two hosts cannot disagree about what the panel
says. Verified: the pixel gate's 192 measurements are unchanged and `graph_panel_pixels` ends OK on
both drivers; the default tree still builds the app.

WHAT THE HOST SWAP STILL NEEDS, so the next step is not re-derived: `gui::GraphPanelWidget` must
derive from `QCanvasPainterWidget` behind `AE_CANVAS_PAINTER` (the pattern `TimeLineEditorWidget`
already uses) and paint `composePanelFrame(...)` through `InkCanvas` in `paint()`. Three things make
that more than a class swap:
1. **ImGui's GL calls cannot run there.** `QCanvasPainterWidget` is a `QRhiWidget`, not a
   `QOpenGLWidget`: `runPanelFrame`/`ImGui_ImplOpenGL3_*`/`stroke::init` are the ImGui host's and
   must be compiled out of that branch entirely;
2. **the input path is ImGui's.** The widget feeds `gui::ImGuiInputBridge` (ImGuiIO) for IME and the
   `keyboardCaptured` rule, while the panel's own gestures already go straight to
   `ctrl::GraphEditor::ownsCursor/updateCursor/updateWheel`. With ImGui gone from this host the
   bridge goes too (its only consumer was ImGui's own text fields, and the panel's menus are Qt), and
   the keyboard rule becomes "the editor owns it";
3. **the pointer is already the host's** (`ink::Pointer` from the widget's own tracked position), so
   the hover path needs no change - that was done deliberately in the vocabulary.

### The panel's host is swapped: the docked page is a QCanvasPainterWidget on 6.12 (2026-10-22)

`gui::GraphPanelWidget` now derives from `gui::PanelHostBase`, which is `QCanvasPainterWidget` when
`AE_CANVAS_PAINTER` is on and `QOpenGLWidget` otherwise - the same shape `TimeLineEditorWidget` uses.
The ImGui host (`initializeGL`, `paintGL`, `runPanelFrame`, the input bridge, `releasePanelContext`)
is compiled only in the `#else` branch; the Canvas branch is one `paint(QCanvasPainter*)` that fills
the floor, calls `ctrl::GraphEditor::syncFrameWindowFor(this->size())`, composes the frame with
`gui::composePanelFrame` and inks it with `gui::InkCanvas`. Every explicit base-class call in the .cpp
says `PanelHostBase::`, so the two branches cannot drift. `publishPanelAccessibility` now takes a
`QWidget&` (its rasteriser was never part of its job).

TWO MEASURED DEFECTS the cutover paid for, both app-visible and both fixed:
- **the page painted BLACK under a `#262626` dock.** First `setFillColor` was never called (the
  RHI widget's default clear), then the floor's `fillRect` inherited whatever fill style was current
  - Canvas's default. The floor now sets its own style per frame; the fill colour is also set at
  construction and in `onThemeUpdated`.
- the ImGui host's pointer facts (`mPointer`) were not enough for `ink::Pointer`: the press position
  and the button state are now tracked in `mousePressEvent`/`mouseReleaseEvent` for BOTH hosts.

VERIFIED IN THE APP (Xvfb, `I_think_its_cursed_honestly.anie`, Graph tab by name through AT-SPI):
the Canvas host draws the dock floor, the frame ruler (0..150), the value column (1/0.5/0), the
playhead pill at 0, the empty state and the scroll bar - `/tmp/graph-canvas3.png`. Against the ImGui
host on the same state (`docs/verification/graph-panel/imgui-host-check.png`) the panel page differs in **2382 of 178160 px
(1.34%)**, with the dominant colours identical to 0.2% (floor 38,38,38; 82,82,82 at 3591 vs 3584;
64,64,64 at 1661 vs 1645): that is text rasterisation plus the 1px ink paths, not structure.

NOT VERIFIED on the Canvas host: hover, the key/channel menus and the drag gestures. The input path
is shared code (the editor's own `ownsCursor`/`updateCursor`/`updateWheel`), and the ImGui-only bridge
is simply absent (`mBridge` null, every `if (mBridge)` a no-op), so nothing routes differently by
construction - but "by construction" is not a measurement, and this tree has already shipped one
invisible label through that reasoning. NEXT: drive hover, a key right-click and a marquee drag on the
Canvas host and assert on the pixels, exactly as `probe_graph_ink.sh` does for the ImGui host.

### The APP runs under cage after all, and grim captures it (2026-10-22)

The earlier note ("the app does not map under cage: black capture") is WRONG and corrected here. The
app maps as cage's presented client and `grim` (installed for this) captures the composited output:

```bash
sudo dnf install grim
scripts/gui/gpu_headless.sh -- /tmp/cage_app.sh <bin> <out-prefix>
# inside: app >> log & sleep 22; xdotool clicks; grim out.png
```

MEASURED: 1280x720 output, `OpenGL renderer: AMD Radeon R9 380 Series (radeonsi, tonga, ACO)`, app
log errors 0. Two kiosk caveats, both measured: **keys do not arrive** (no WM, so the X window has no
input focus - `xdotool key Right` did nothing; only pointer events work), and **a Qt popup window is
placed at 0,0** (a menu with no WM to place it), which is a kiosk artifact and not the panel.
The tree's expander toggles at x 10..22 on the row's own y; two clicks there collapse it again.

### The host divergence, CLASSIFIED - 0 unexplained (2026-10-22)

Counting differing pixels says nothing on its own, so every differing pixel of the panel page is
classified by CAUSE, with the rule printed in the montage:
- `text_row` - inside the ruler's number row (glyph rasteriser);
- `shift_le2px` - one side floor, the other ink, AND the other image holds the same colour within 2px
  (the same mark, placed a pixel differently);
- `coverage` - both sides ink of the same chroma, different coverage (antialiasing);
- `UNEXPLAINED` - everything else: a difference with no counterpart, i.e. a real defect.

Both hosts on **radeonsi**, same project, same layer, same frame (`docs/verification/graph-panel/imgui-host-check.png` vs
`docs/verification/graph-panel/hosts-final.png`, panel page 675x255): **4284 px differ = 654 text_row + 637 shift_le2px + 2993
coverage + 0 UNEXPLAINED**. The causes, and what each one IS:
1. **Text metrics**: Dear ImGui bakes integral advances (`PixelSnapH`) and its own line height; Qt uses
   fractional advances and `QFontMetricsF::height()`. Consequences, both visible: the value labels sit
   1..2 rows differently, and a right-aligned run shifts ~2px. FIXED for the ruler's row: the portable
   backends now put VAlign::Top's ink on the model's row through the run's own INK BOX
   (`tightBoundingRect().top()`) instead of the ascent - MEASURED before that: glyph rows 412..420
   against ImGui's 410..417, i.e. two rows low, and after: identical.
2. **Rounded corners**: a radius-2 pill's corner covers one row fewer under Canvas2D than under ImGui's
   `AddRectFilled` (measured at the bar pill's top row: 952..955 vs 953..954).
3. **Antialiasing**: every curve edge - the same 2px stroke, two kernels (the model-level gate's
   `strokeDiff`, tolerance 2.5px).
No mark is missing, none is in the wrong place by more than 2px, and nothing differs in hue.

### Hover, menus and drags on the Canvas host: DRIVEN, on the GPU (2026-10-22)

All three, on the docked Canvas page under cage (`docs/verification/graph-panel/`), with the app log at 0 errors:
- **hover on a key** changes 744 px in the plot (the marker brightens and its chip appears - the Xvfb
  captures show the `Scale Y` chip at the curve's end);
- **right-click** opens the panel's menu ("Paste" disabled, "Fit graph") - 0 px inside the panel
  because a Qt popup is its own window; it appears at 0,0 with no WM (kiosk artifact);
- **marquee drag** changes 4027 px while the button is down and the release takes all 4027 away
  (transient state cleared, the class this tree has been bitten by);
- the earlier "hover 0 px" was a probe that parked the pointer on empty floor - not a defect.

### Where the graph migration stands, and what is left (2026-10-22, closing note)

DONE and verified: one ink vocabulary (`gui/Ink.h`), three backends (`InkImGui`, `InkQPainter`,
`InkCanvas`), one decision site (`drawPanel`, no ImGui type in it), one frame COMPOSITION
(`composePanelFrame`) that both hosts use, the dock's page deriving from `QCanvasPainterWidget` on
6.12 (`PanelHostBase`), and the parity gates: `graph_panel_pixels` (ImGui, 192 measurements, OK on
radeonsi and llvmpipe) plus its backend section (QPainter vs Canvas, GEOMETRY 0 on both drivers, two
models). The app was driven on the GPU under cage: hover, right-click menu and marquee all work on the
Canvas host, and the ImGui host still draws the same page on the 6.11 tree (suite 51/0, screenshots
`docs/verification/graph-panel/imgui-host-check.png` and `docs/verification/graph-panel/gx-graph.png`).

STILL OPEN, and none of it is scaffolding:
1. **Dear ImGui is still a dependency**, used only by the 6.11 tree's host and by
   `ImGuiInputBridge`. Dropping it means re-pointing INPUT: the widget's events must feed
   `ctrl::GraphEditor` directly (mouse/wheel/keys/IME) instead of ImGui's IO, and the
   `keyboardCaptured` rule goes with the bridge. The ink side is ready (the backend reads no input).
2. **A fractional DEVICE PIXEL RATIO** is still untested for the portable backends: the canvas gate
   runs at scale 1, and the text-origin snapping is in LOGICAL space (the same residual recorded for
   the timeline's ImGui text).
3. `probe_graph_ink.sh` still targets the ImGui host only; the Canvas host's drag/hover assertions
   exist as measurements in this file (`docs/verification/graph-panel/`), not yet as a probe with pass/fail counts.

### Edge cases, and per-MARK evidence instead of a pixel count (2026-10-23)

**The backend matrix.** `graph_panel_pixels` now paints SEVEN states through both portable backends,
one measured line each, on both drivers: `empty` (the model's empty state, no curves),
`silent-curve`, `crowded-8` (eight channels, chips de-collided, curves crossing), `marker-states`
(selected / sibling / proportional on one row), `handle-kinds` (stored + ghost on a focused
channel), `chrome-stress` (play window + pills, playhead parked over the VALUE column, marquee, both
bars) and `tiny` (a 120x90 panel whose value column barely fits). Result: **GEOMETRY 0 in all seven
on llvmpipe AND radeonsi**, `backend_matrix states=7 failures=0`, and every state asserts label ink
on both sides (`crowded-8` checks **8** chip labels).

Two real gaps the matrix found, both fixed rather than excused:
- `chrome-stress` reported **800 px** - the marquee's **rounded border**, exactly its perimeter.
  Canvas2D's arc rasterises one column further out than Qt's (measured: `#9a80e5` where Qt inks
  `#8671c3`, at the same corner). The class is now `inShapeBorder`: a pixel within 2.5px of ANY
  published rounded shape's OUTLINE (marquee, playhead pill, play-window pills, both bar thumbs,
  every chip) is ink-path - the outline is still the model's, only its arc fringe is forgiven.
- the last 7 px were the same corner at radius 6, which is why that band is 2.5px and not 1.6.

**Per-mark evidence in the app, on the GPU.** `AE_PANEL_MODEL_DUMP=<path>` makes
`composePanelFrame` write the frame's own geometry as JSON (panel, plot, every curve's sampled
points, every key centre, every label rect and text, the playhead, the thumbs). Both hosts on
radeonsi publish **identical geometry** (plot `[75,26,599,218]`, 2 curves, 1 key each, 3 value
labels), and measuring each capture AGAINST that geometry gives, for both hosts:

| measure | ImGui host | Canvas host |
|---|---|---|
| page origin from the playhead column + curve alignment (score 0.994) | x=274, y=463 | x=274, y=463 |
| stroke offset vs the model, 104 sample columns | mean +0.45, max 1.50 px | mean +0.45, max 1.50 px |
| key dot A offset / ink | (-0.28, +0.50) / 50 px | (-0.28, +0.50) / 50 px |
| key dot B offset / ink | (-0.26, +0.50) / 46 px | (-0.28, +0.50) / 50 px |

The +0.45px stroke offset is SHARED, i.e. it is the model-to-pixel relation of a 2px stroke, not a
host difference. What genuinely differs is text (glyph rasteriser) and the corner arcs - not any
mark's position.

### Evidence lives in the REPO now (2026-10-23)

The captures and the model dumps this file cites are in `docs/verification/graph-panel/`
(`hosts-final.png`, `host-diff-gpu.png`, `gx-graph.png`, the three interaction shots, the two
`fx-*-final.json` model dumps), and the two recipes are scripts:
`scripts/gui/cage_app.sh <bin> <out-prefix>` (drives the expander, the layer and the Graph tab under
cage, capturing with grim) and `scripts/gui/cage_model_dump.sh` (the same with
`AE_PANEL_MODEL_DUMP`, so a capture can be measured against the frame's own geometry).

/tmp holds no source: the tree is committed (`500f3ce1`). It DOES hold two build directories -
`/tmp/build612` (the Qt 6.12 tree that `AE_CANVAS_PAINTER` needs) and a clean verify tree - and both
are recreated, not preserved:

```bash
cmake -S . -B /tmp/build612 -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64 -DCMAKE_BUILD_TYPE=Release \
      -DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62
cmake --build /tmp/build612 -j"$(nproc)"
```

### A CLEAN configure must be MULTI-CONFIG, and the fresh build found two dead locals (2026-10-23)

`cmake -S . -B <dir>` with the default generator (single-config) FAILS at the link of the app:
`cannot open output file src/gui/AnimeEffects: Is a directory`. The directory is the **QML module's**
output (`src/gui/AnimeEffects/Dialogs/AnimeEffects.qmltypes`): a single-config build puts the binary
at `src/gui/AnimeEffects` and the QML module claims the same path. The tree's own `build/` is Ninja
Multi-Config, where the binary lands in `src/gui/Debug/`. So a from-scratch verification build is:

```bash
cmake -S . -B /tmp/verify-build -G "Ninja Multi-Config" -DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62
cmake --build /tmp/verify-build --config Debug -j"$(nproc)"
```

The clean Debug compile also found two `-Wunused-variable` warnings the incremental builds never
reported - `stem` (the handle-stem colour the backend now computes) and `palette` (the frame driver's
local, now composed inside `composePanelFrame`) - both deleted. Same lesson as the earlier note: a
warning is only absent from the run that COMPILED that translation unit.

### The clean 6.12 build, and the warnings a fresh compile found (2026-10-23)

A from-scratch Qt 6.12 build (`-G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64
--config Release`) reported **36 warnings** that no incremental build had shown - five of them real
defects of the migration, all fixed:
- `FrameRulerCanvas.cpp`: `const QFontMetrics metrics(QFont());` is a VEXING PARSE (a function
  declaration) - `= QFontMetrics(QFont())` fixes it;
- `TimeLineEditorWidget.h`: `paintEvent`, `resizeEvent` and `keyPressEvent` override the base class
  in both host branches and were not marked `override` (the class gained a swappable base);
- `InkCanvas.cpp`: an unused `boxOf(QRect)` overload, left when every call site became a QRectF;
- the ruler gates captured `margin`/`mg` in lambdas that no longer read them;
- `graph_panel_pixels`: an accumulator that only ever added zero.

After them: **exit 0, 0 errors, 3 warnings**, and both remaining kinds are PRE-EXISTING and outside
this work - `ObjectTreeWidget.cpp`'s `QDragMoveEvent` ctor and `img/PSDReader.cpp`'s
`QString::fromUtf16`. All three Canvas gates from that clean tree, on both drivers:
`frame_ruler_canvas cases=9 failures=0`, `time_body_canvas cases=2 failures=0`,
`graph_panel_pixels` (192 measurements + `backend_matrix states=7 failures=0`) OK.

### The last two warnings were API deprecations, and both took the RIGHT change (2026-10-23)

With the migration's own warnings gone, two pre-existing deprecations were left, and silencing them
would have been wrong:
- **`img/PSDReader.cpp`: `QString::fromUtf16(unsigned short*)`.** The block is UTF-16BE, so the
  units are already swapped by hand; the fix is to decode them into a `std::vector<char16_t>` - the
  type the non-deprecated overload takes - rather than keep a `uint16_t` buffer and
  `reinterpret_cast` it (two 16-bit integer types are not interchangeable through a pointer without
  breaking the aliasing rule, and the cast would have been the only reason for the buffer's type).
- **`gui/ObjectTreeWidget.cpp`: `QDragMoveEvent(QPoint, ...)`.** The QPointF constructor exists from
  **6.12** and the QPoint one is `QT_DEPRECATED_VERSION_X_6_16`; **6.11 has the QPoint one only**
  (read out of both headers, not inferred). So the newer spelling is used where it exists behind
  `QT_VERSION >= QT_VERSION_CHECK(6,12,0)` and the old one stays where it is the only one - no cast,
  and the cheat point stays the integer a ROW's centre is (widening it is exact).

Both trees then built from EMPTY directories: **6.11.2 Debug and 6.12.0 Release, exit 0, 0 errors,
0 warnings**, and every gate still passes (`frame_ruler_canvas` 9/9, `time_body_canvas` 2/2,
`graph_panel_pixels` incl. `backend_matrix states=7 failures=0`, on llvmpipe and radeonsi; suite
51/0).

## The timeline's ruler is Canvas Painter now, and the playhead's pill moved into the model (2026-10-23)

`TimeRuler` is a `QCanvasPainterWidget` on 6.12 (`#else` keeps the QWidget host for the 6.11 tree),
and its content is now ONE value for both rasterisers: `TimeRuler::buildModel` produces
`frameRuler::Model` + `PaintInk`, `paintRulerCanvas` inks it on Canvas, `paintRuler` inks the same
model with QPainter. No rule lives in a paint function any more.

**The playhead's pill is the RULER's, not the cursor's.** `gui::TimeCursor` used to recompute the
same pill from the same font that the ruler's model had already computed for its number-suppression
rule - two spellings of one rect. `frameRuler::PaintInk` gained `playheadFill`/`playheadText` and
both renderers paint `Model::playhead`; `TimeCursor` is now only the 1px rule that crosses the lanes.
That also removed an invisible defect: the cursor widget was `2*pillWidth+1` wide and NOT
`WA_TransparentForMouseEvents`, so it swallowed every press in that ~60px column. It is 1px wide and
transparent to the mouse now.

**Verified** (Qt 6.12 Release, Xvfb llvmpipe): the strip's numbers, ticks and pill are at the same
rows and columns as the 6.11 raster host (rows 410..419 differ in glyph ink only; the only other
differing row on the whole screen is the known 1-LSB translucent lane tint at row 429), and
`ctrl+Right` moves the pill AND its line one frame together (captured: pill "0" -> "1", accent column
373 -> 378). `frame_ruler_canvas` 9/9 with `NON-TEXT = 0` on llvmpipe **and** radeonsi,
`probe_timeline_ink.sh` OK, `probe_graph_parity.sh` OK, suite 51 pass / 0 fail.

**Three traps paid for here, all measured:**
- **`beginPath()` is load-bearing before every Canvas2D `roundRect`+`fill`.** The ticks left their own
  subpaths current, and the pill's `roundRect` appended to them: the measured result was that the pill
  painted NOTHING (`#262626`, the band floor, where the raster had ink). `fill()` does not reset the
  path - the same trap the body's markers hit (1923 px that were one missing call).
- **A filled rounded rect needs `setAntialias(1)` of its own.** The ruler's paint turns AA off for its
  ticks, and a pill inheriting that draws a stair-stepped corner while `paintPill` (the raster rule)
  sets the hint itself. With AA on, the corner coverage matches to 1 LSB and the shape spills one
  unit of coverage onto the rows just outside it (`#262627` where the raster has `#262626`).
- **The gate needs the `inShapeBorder` rule for this**, exactly as the panel's gate does: pixels within
  1px OUTSIDE a published pill's rect are ink-path (an AA kernel), the pill's BODY and its TEXT box are
  not excused - the body is asserted for ink parity on both sides (`playhead pill raster=125 canvas=137
  ok`), the text box is a glyph rasteriser difference. Excusing a region without asserting it is the
  blind spot that once let a lane's label vanish.

**The repaint scope is why the cursor is NOT folded into the surface.** The surface is one Canvas
texture; `setFrame` repaints only the ruler (22px) and the cursor's own column today, and folding the
line into the body's paint would repaint every key on every frame step (measured elsewhere at 1.85 ms
with the pixmap cache, 19.1 ms without). A 1px opaque child widget keeps the repaint the size of the
thing that changed and needs no transparent RHI overlay - which nothing here has verified.

**The temporary playback window's PILLS were invisible on the Canvas host, and the gate excused them
(measured 2026-10-23).** A live A/B against the pre-migration binary (`e1fd8393`, raster host) showed
the window's two label pills reduced to their stems plus light-on-dark TEXT on the bare band, where the
raster host paints an opaque rounded pill with dark text. Two state bugs in one lambda
(`FrameRulerCanvas.cpp`'s `pill`): it called `roundRect()` + `fill()` with the TICKS' subpaths still
current (`fill()` does not reset the path - the same trap that cost 1923 px on the body's markers), and
it inherited the ticks' `setAntialias(0)`, so a filled rounded rect had stair-stepped corners. Measured
after the fix: the pill body goes 0 -> 1270 px of its own fill against the raster host's 1281, and the
whole-band difference against the old binary drops 2651 -> 2092 px (the remainder is glyph AA, below).

WHY THE GATE WAS GREEN, which is the part worth keeping: `frame_ruler_canvas` excused every pill's
interior from pixel comparison (two AA kernels) and asserted OCCUPANCY only for the PLAYHEAD's pill -
and it counted occupancy as "pixels differing from the box's modal colour". A pill whose FILL never
landed still holds its LABEL TEXT, so the modal colour becomes the band and the ink count is the
glyphs, which both sides have: the same numbers, one pill missing. The assertion now counts the box's
pixels that match the pill's FILL TOKEN (`fillCount` in that gate, for the playhead's `playheadFill`
and for `window.start`/`end`/`merged`'s `windowEdge`), so a body that is not painted fails: measured
with the bug restored by hand, `window pill 100 raster=165 px canvas=2 px MISSING/DIFFERS`, `failures=6`
of 9 cases; with the fix, `canvas=156` and 0 failures, on llvmpipe. Lesson, the file's own idiom:
excusing a REGION is fine only when the thing that must be inside it is asserted by its own token, not
by "something non-background is here".

**What else the same A/B measured, so nobody re-opens it** (HEAD vs `e1fd8393`, both Release, same
project/layer/frame, under Xvfb): the ruler's ticks, tick tiers, number positions, boundary and merged
labels, the playhead pill and its line, the lanes with their keys/labels/seams, and the graph page's
curves, keys, value labels, playhead and both scroll bars all match. The ONE remaining difference on
these surfaces is the glyph rasteriser: Canvas2D's `fillText` covers one row and one column MORE than
Qt's raster `drawText` for the same 12px string (ink rows 8..16 against 8..15, tops identical), which
the ruler gate's own `textDiff` counts as 1138..3372 px per state. That is text-engine truth, not a
layout or rule difference, and it is why "the numbers look different" is a real report about ink while
every position matches. The other visible delta in that comparison - the dock header shifting ~12px -
is the segmented `Timeline | Graph` tab restyle, which was uncommitted work in the same tree, not the
migration.

**The header's channels control read `0 channel(s)` over three drawn curves (fixed 2026-10-23).** The
control is the panel's, but its HOST is the dock's bar (`TimeLineHeader`), so it has to be told. It was
re-read only when the TAB changed (`setSurface`) and at `refreshFrame`, and neither fires when the
object tree picks a layer while the Graph tab is already up: measured on `Canette.anie`, three curves
drawn, button at `0 channel(s)`, and a `Timeline -> Graph` toggle corrected it to `3 channel(s)`.

THE FIX IS NOT "EMIT FROM refresh()", and that is the part worth keeping. `TargetWidget`'s selection
lambda calls `mGraphPanel->refresh()` from the tree's own `onSelectionChanged`, and the dock hands the
new TARGETS over later in that same notification chain - so a count read at refresh() time answers
from the PREVIOUS target: instrumented with a file write (qWarning never reached either the rerouted
stderr file OR the journal in this build, so the probe was a `QFile` append), nine refreshes all
reported `0 rows` while the page drew three curves. The panel now publishes what it DREW:
`GraphPanelWidget::publishChannels(frame.model.channels)` runs right after `drawPanel` in the Canvas
paint, gated on the row set actually changing (a playback tick repaints 60x/s and must not rewrite a
label each time), and `TimeLineHeader` connects that signal with `mChannelsConnected` lazily - the page
is a sibling that may not exist when the bar is built. Verified live: the same rig now reads
`3 channel(s)` as soon as the layer is selected, and `scripts/run_suites.sh Debug` stays green.

## The Qt floor is 6.12 now, and the old timeline host is deleted (2026-10-23)

**`find_package(Qt6 6.12 REQUIRED ... CanvasPainter)`** in the root `CMakeLists.txt`: a tree
configured against 6.11 fails by name, and `AE_ENABLE_CANVAS_PAINTER` is gone (`AE_CANVAS_PAINTER`
is set to 1 unconditionally). The aqt install at `~/Qt/6.12.0/gcc_64` is the toolchain:

```bash
rm -rf build && cmake -S . -B build -G "Ninja Multi-Config" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64" -DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62
cmake --build build --config Debug -j"$(nproc)"
```

**DO NOT `dnf remove 'qt6-*'` to "uninstall the old Qt".** Measured: the system's 6.11.2 packages
have **80 installed dependents**, including the whole `kf6-*` stack and `cutecosmic-qt6` - the Qt
platform theme this desktop runs. The project stops using them; the system keeps them.

**The two qt-C++ hosts collapsed to one branch each.** `TimeLineEditorWidget` and `TimeRuler` are
unconditionally `QCanvasPainterWidget` (`paint()`; the `#else` QWidget/`paintEvent` paths and every
`#if defined(AE_CANVAS_PAINTER)` around them are deleted). Two consequences, both measured:

- **The Canvas module's headers are a PUBLIC CHAIN.** `gui/TimeLineEditorWidget.h` IS a
  `QCanvasPainterWidget` and is reachable from `gui/MainWindow.h` (`TargetWidget` -> `TimeLineWidget`),
  which tests include. Forward-declaring it in `TimeLineWidget.h` does NOT work (that header calls
  `mInner->...` and needs the complete type), so the root `CMakeLists.txt` links `Qt6::CanvasPainter`
  at DIRECTORY scope: one line instead of teaching a dozen targets about the panel's base class.
- **A dead include hid a layering violation until the include path changed.** `ctrl/ProjectLoader.cpp`
  included `gui/MainWindow.h` and `gui/MainMenuBar.h` and used neither; it compiled for years because
  nothing on that chain needed a module ctrl did not link. Both are deleted.

**Suite: 53 pass / 0 fail.** The two Canvas gates moved onto `scripts/run_suites.sh`'s GL branch -
a `QCanvasPainterWidget` is a QRhiWidget, so an offscreen run reports "no Canvas Painter frame" and
fails; they were previously "skipped (not-configured)" and never ran in the suite at all.

**Still the pre-6.12 renderer, and the next wave:** `GraphPanelWidget`'s ImGui host (`runPanelFrame`,
the context/atlas refcount, `stroke`/`outline`), `InkQPainter` (0 callers), `InkImGui`, the
`ImGuiInputBridge` gate in `KeyCommandInvoker`, and `tests/graph_panel_pixels` - which is 8185 lines
whose 192 claims are measured THROUGH ImGui. That gate is the only reason `ae_imgui` is still a
dependency: it must be re-hosted on Canvas with its expectations re-derived before the ImGui
deletion can land, and it cannot be split from it without leaving the tree unbuildable.

## What one viewport frame costs, and what a pan contends with (2026-10-23)

Measured on the R9 380 through `scripts/gui/gpu_headless.sh` (cage + wlroots headless, radeonsi),
Release, `Canette.anie` (37 render units, 60 fps, range [0,200]), driven by xdotool: play, then a
middle-drag pan. The instrumentation was temporary (`AE_PERF_TRACE` scopes) and is gone.

**The scene was the cost, and most of it was per-layer waste.** `ObjectTree::render` split as
(per frame) accessor 0.003 ms, `prerender` 2.8-3.1, collect+sort 0.015, `render` 2.1-2.4 - 4.9-5.5 total.
Inside `MeshTransformer::callGL` (33 calls/frame): attribute uploads 0.025, transform-feedback draw
0.0022, the three `glCopyBufferSubData` 0.0023, `glFlush` 0.005 and **`XC_ASSERT(glGetError())` 0.023**
- 0.77 ms/frame asking the driver a pipeline question once per layer. Inside `LayerNode::renderLayer`
(26 calls/frame): shader/uniform/attribute setup 0.036, `drawElements` 0.012, `glFlush` 0.005. The
drawing itself was 0.4 ms of a 5 ms frame; the rest was submission overhead.

Four changes, each measured on the same probe:
1. `MeshTransformer::callGL` and `LayerNode::renderLayer` no longer `glFlush()` per layer - GL orders a
   context's commands itself and the frame submits once at the end of `paintGL`.
2. The per-layer `glGetError` probe is OPT-IN (`AE_GL_STRICT=1`); it cost 0.77 ms/frame. The frame-level
   `GL_CHECK_ERROR()` still reports every error in any build.
3. `TimeLineWidget::setFrame` returns early when the frame did not move (no whole-tree re-blend, no
   viewport repaint for a no-op tick).
4. `TimeLineWidget::onPlayBackUpdated` ROUNDS THE INTERVAL UP instead of truncating it. Truncation woke
   the timer up to a millisecond before the frame boundary, that tick could not advance the frame, and it
   re-armed for the remaining fraction - so a 60 fps project ran **78 ticks/s** with ~30 % of them
   painting a frame that had not changed. After the fix: **60 ticks/s, 1.00 frames per tick**.

Result: `Tree::render` **2.2-2.6 ms** (was 4.9-5.5), playback paints **60.0/s at 16.67 ms** with our
paint taking **1.9 ms**, and a pan (which drives 115 requests/s: 60 ticks + ~55 moves) paints **66/s at
15.0 ms**.

**What the rest of a painted frame is, and the thing NOT to build.** ~15 ms of every 16.7 ms cycle is
Qt's window composition, not our code: a stack sample during a pan sits in
`QWidgetRepaintManager::paintAndFlush()` -> `MainDisplayWidget::paintEvent`, and a 1 ms watchdog cannot
fire across it. The Qt docs say exactly this: `QOpenGLWidget::frameSwapped()` is "emitted after the
widget's top-level window has finished composition and returned from its potentially blocking
QOpenGLContext::swapBuffers() call", and `QOpenGLWindow` + `QWidget::createWindowContainer()` is
"faster than QOpenGLWidget due to the lack of the additional composition step". `QWidget::update()` is
documented to coalesce "when Qt returns to the main event loop" - i.e. per event-loop pass, NOT per
display frame - which is why input events and timers can each produce their own paint.

**A timer-based frame gate was implemented, measured and REMOVED** (do not re-add it): book the next
paint on a single-shot timer at the screen's refresh, let every request re-evaluate against the clock,
keep the timer only as a retry for a dropped tail. A/B on the same pan: **51-53 paints/s with the gate,
66/s without**, and playback identical at 60/s either way - Qt already coalesces per pass, and a retry
that fires ~9-14 ms late (the composition is uninterruptible) stretches the cycle past the period. So the
pacing must come from presentation or a cheaper paint, never from an app timer.

**Debug is not the story.** Debug differs only in the render share (blend 0.72 vs 0.24 ms, tick 0.77 vs
0.29); the frame cadence, the composition cost and the paint rate are the same in both builds.

**Two harness traps met while measuring this** (both looked like product bugs): killing the app leaves a
recovery copy, and the next launch opens a modal "Recover unsaved project?" that swallows every click -
launch probes with a fresh `XDG_CACHE_HOME`. And a project that moved on disk (the owner renamed the
folder) makes the app start with an EMPTY project, silently, with no argv-project load and no error
dialog - a probe must assert that the project is open, not just that a window exists.

## The Timeline/Graph switch is a segmented FIELD, not an accent chip (2026-10-23)

Every line measured on the running application (Xvfb, dark and light, `test.anie`).

**The user's ruling beats the mock, and this is the second time it has.** The mock
(`~/dev/graph-timeline-redesign/assets/mock.css:239`, `.tl-tabs button.on { color:
var(--accent-text); background: var(--accent); }`) paints the current page tab as a solid accent
chip. The user's report: *"the accent color in the Timeline/Graph segmented control is rough, could
be better designed, accent fill is too much there."* At 11px in a 40px bar beside a row of muted
icon buttons that chip was the loudest thing in the dock - and the accent belongs to the DOCUMENT's
own marks (the playhead pill, the keys, the selection ring), not to the chrome that names which page
is up.

**What it is now** (`TimeLineHeader::refreshTabStyle`, tokens only, no stylesheet asset): the two
tabs are one SEGMENTED FIELD in the header's own vocabulary - the ease chip's recipe
(`prop::EasingChip::paintEvent`: `recessed` surface + `hairline` edge, and it is the header's only
other filled control) - and the current segment is the KNOB raised out of it:

|part|token|dark|light|
|---|---|---|---|
|trough|`recessed` + 1px `hairline`, radius 6|`#171717` + `#404040`|`#f5f5f5` + `#e5e5e5`|
|knob (current)|`raised` + 1px `outline`, radius 4, inset 2|`#404040` + `#525252`|`#e5e5e5` + `#d4d4d4`|
|label at rest|`textMuted`|`#a3a3a3`|`#737373`|
|label hover / current|`text`|`#f5f5f5`|`#171717`|

**NO FILL ON HOVER, and that is structural: `raised` IS the knob's fill in both themes** (dark
`raised` == `hairline` == neutral 7; light `raised` == `hairline` == neutral 2), so a hover fill
would paint a second knob and the pointer could not be told from the selection. Measured: hovering
"Graph" took its glyph peak 156 -> 234 with the trough pixel unchanged at `#171717` - label-only
feedback, the same signal the bar's icon buttons give.

**Light theme is the weak one, and it is the app's weakness, not this control's.** Light `raised`
(`#e5e5e5`) against light `recessed` (`#f5f5f5`) is 1.1:1; the knob's `outline` edge and the label
ink (`#171717` against `#737373`) carry the state. `modetabbar.ssa`'s own selected tab has exactly
the same fill contrast on `base`, so the segmented field is consistent with the application rather
than inventing a stronger rule for one control.

The capture of all four states - before, dark on each page, light - is
`docs/verification/graph-panel/header-tabs-segmented.png`.

**Size, and why the dock widens by 12px.** Tabs measure 60x21 -> **66x25** (both states), so the
dock's minimum width went 895 -> 907 and the window with it. It is CONSTANT across the two tabs, so
the "a tab-scoped control grows the dock" trap (the channels button) does not apply here.

**Two QSS facts this cost:**
- **Tokens are substituted by NAME (`replace`), not by `%N`.** The sheet carries 13 values and
  `QString::arg`'s `%1` collides with `%10` - and a mis-ordered chain reported nothing, it just
  rendered the wrong colour.
- **A widget's own sheet beats `standard.ssa`'s `QTabBar::tab:top:!selected`** even though that
  selector has one more pseudo-state: the measured rest ink is `textMuted`, as the sheet asks, not
  the app sheet's `text`.

**`probe_graph_parity.sh`'s PILL STEP was wrong, and it is fixed (2026-10-23).** It used to read
ONE column of the playhead pill (`px0+4`, "the padding column") and require the longest run of
pixels EQUAL to the token in that column to be the model's `NUMBER_HEIGHT` (14) +-1. Three things
made that unusable on a Canvas-rendered ruler, in the order they were measured
(`docs/verification/graph-panel/pill-probe-step.png`):

1. **The column can be a digit's stem.** `px0` is the first column of a tol-2 run, so it starts one
   column INSIDE the drawn edge and `px0+4` is 5 columns in - exactly where a frame number begins
   after `kPillPadding`. The 3-digit pill ("281", 30px) read **3 rows** and the probe aborted with
   that number; the 1-digit pill ("1", 17px) read 12.
2. **`tol 1` is "equals the token", and the pill's own outer row is not the token.** The top and
   bottom rows are 93%-coverage blends, `(163,136,244)`, **distance 7.8**, while the band is 260
   away - so no column could ever count 14 and the guard was unsatisfiable at ANY column.
3. **A column MAXIMUM is wrong too, for a reason that is not obvious until it bites:** the playhead
   LINE hangs from the pill's own bottom row in the pill's own colour, so at the pill's centre
   pill+line is ONE 23-row run - the tallest column is the line, not the pill. A row MAJORITY is
   wrong from the other side: a row through the number is mostly NOT the token (the densest row of
   "281" kept 9 of its 28 columns), so the pill read as two runs with a 3-row longest.

**The fix, and its shape.** The pill is measured by ROWS over its own columns:
`scripts/gui/ae_ink.py` gained **`rowruns <raw> <w> <h> <r> <g> <b> <tol> <mincols>`** - the
y-runs of rows holding at least `mincols` pixels within `tol` of the colour, the fill-sense mirror
of `inkcols`. The probe crops the pill's own fill-run columns once and asks for rows carrying
`PILL_MIN_COLS = 3` (structural: the only 1px thing in that colour is the playhead line) with
`PILL_FILL_TOL = 40` (mid-gap: the rim is 7.8 away, the band 260). Both the search-window read and
the strip read use it, and the polarity inversions they used to need are gone. Measured after the
fix: the pill is **rows 241..254, 14 tall**, at `kPillTop` 2 of a 22-row strip, on BOTH pages, from
the same fill run (page-relative `284+28`) - and the probe now runs end to end instead of aborting
at 47 s.

**The 2 failures it found are FIXED, by adopting the engine's own rule.** Once the probe could run,
the one property failing was the frame number's glyph rows: **243..252 on the timeline against
242..250 on the graph** - the panel's numbers one row HIGH. Measured across every number in the band,
both pages' glyph HEIGHTS were identical (same histogram, modal 9 rows) and only the TOPS differed,
so it was placement, not size. The two spellings:

- `gui::InkCanvas` / `gui::InkQPainter` placed the run by its INK box (`box.top() +
  kRulerNumberInkTop`, baseline from `tightBoundingRect().top()`): hand-derived, and one row above
  where the timeline inks.
- The TIMELINE's rulers place it with the ENGINE's own top baseline (`Canvas TextBaseline::Top` /
  Qt's `AlignTop` on the box's top row), and the digits' ink follows three rows down (ascent 11.6
  minus cap top 3.3) with no constant at all.

THE ENGINE'S RULE WON - it is what the application already draws the ruler with, on the same face
and size. Both portable backends now do `VAlign::Top` = "the box's top row is the line box's top".
Measured after: panel rows **243..251** against the timeline's 243..252, `probe_graph_parity.sh`
**OK / 0 failures** on the same tree (band, hairline, ticks, pill box/column/fill, playhead column
and both scroll-bar lanes still agreeing), and `scripts/run_suites.sh Debug` **53 pass / 0 fail**
(including `graph_panel_pixels`' backend matrix and `frame_ruler_canvas`). `gui::kRulerNumberInkTop`
survives for `gui::InkImGui` alone - ImGui's line box is its own, and the pixel gate, its only
remaining consumer, still pins the row against that constant.

Both ruler bands after the change are `docs/verification/graph-panel/ruler-number-rows.png`
(timeline above, graph below, 2x point-sampled): `0 / 12 / 24 / 36 / 48 / 60` land on the same rows
in both, which is the property the probe was measuring when it failed.

## Shift's axis snap measures from the PRESS, on BOTH axes (2026-10-24)

Shift during a key move locks the dominant axis (the pointer's absolute pixel displacement from the
press decides which one, `GraphEditor::updateKeyMove`). The VALUE half was always absolute - the
pointer's row minus the row the press went down on - so the axis the pointer stopped dominating
returned to the value it held at the press. The FRAME half was not: it accumulated
`pointerFrame - mMovePointerFrame` from the last accepted step, so an axis the snap had left kept the
part of the drag that had moved it.

Measured on the report "holding shift and dragging left and then a bit down drags it on the X axis
and then switches to the Y axis, but from the latest X axis position rather than the starting
position": a 40px left step moved the key frame 12 -> 10, and the following predominantly-down step
(value dominant) left it at 10 instead of returning it to 12. Both halves are now ABSOLUTE under the
snap: `target = pressKeyFrame + (pointerFrame - pressPointerFrame)` while the frame axis is active,
`pressKeyFrame` otherwise, and the step is the distance from `mMoveFrame` (where the key is NOW, so a
clamp or a refusal is absorbed) to that target. The gesture stays ONE command rewritten in place, so
a switch is still one undo step.

The two new press fields are `mMovePressPointerFrame` and `mMovePressKeyFrame`, and
`mMovePointerFrame` keeps advancing by every accepted step in BOTH branches - that invariant (the
key's frame delta equals this pointer frame's delta) is what makes releasing Shift mid-drag put the
key under the pointer instead of jumping it.

Gate: `tests/graph_drag.cpp` section 21's switch block (press, 40px left, 40px down at 5px left:
`frame 12 -> 10 -> 12`, the value following its own row, and the four-gesture undo count proving the
switch did not push a second command). Verified discriminating: with the old relative reading
restored by hand it fails at `d0->frame() == 12` (`FAIL 3449`). `scripts/run_suites.sh Debug` =
**53 pass / 0 fail**. Not driven live - the host hands the same `QMouseEvent`s to the same
`GraphEditor::updateCursor`, which is what the gate exercises, but no probe drags a key on the
Canvas host yet.

## A key's components are independent channels: the press names the MARKER (2026-10-24)

The user's report: "in the graph editor the keys from different channels like Move X and Y are fixed
on the value axis ... dragging one moves the sibling linked on both axes, they should obviously only
be on the time axis, not the value axis, they are independent channels."

**What was wrong.** A press on a marker wrote a WHOLE-KEY selection entry (`markerTarget` names
`(line, type, frame)`, no component), and `beginKeyMove` expanded a whole-key entry to one VALUE member
per graphable component. So a plain press + vertical drag wrote Move X's AND Move Y's value with the
same delta - the sibling moved behind the user's back. (`time::Selection` has always had the marker
level, and the design section above claims "a plain press selects the marker under the pointer"; the
code did not.)

**The rule now, in three places.**

- *The press writes the MARKER* (`GraphEditor::updateCursor`'s key branch): a plain press replaces the
  selection with `(key, channel.component())` unless that marker is already covered, in which case it
  GRABS the group; Shift and Ctrl both toggle THAT marker (`containsMarker` +
  `addMarker`/`subtractMarker`). The click collapse on release is the marker too
  (`mClickCollapseComponent`), so a click keeps exactly what the press named.
- *The value half reads markers* (`beginKeyMove`): a marker entry writes its own component. A WHOLE-KEY
  entry - what a marquee, ctrl+A and every timeline gesture write - names the TIME axis only, so its
  value half is the component the PRESS GRABBED (`mMoveComponent`), applied to every key the entry
  names: a marquee over several keys still moves the channel under the pointer on all of them, and no
  sibling channel is touched.
- *The frame half is unchanged*: one target per KEY, deduplicated, so the shared frame still moves once
  per gesture whatever the selection's level - which is why a marker entry loses nothing on the time
  axis (`Selection::assign` still emits one target per key for the timeline's own commands).

**Gates.** `tests/graph_drag.cpp` section 23e was re-derived: (a) a plain press selects X alone and Y
wears the sibling ring, (b) Shift adds Y / Ctrl removes it / Ctrl puts it back, (d2) NEW - the reported
gesture from an empty selection (press on X, drag up: X follows, `scale().y()` bit-identical, one undo),
(e) a whole-key marquee selection dragged vertically moves the grabbed channel on BOTH keys and leaves
every Y untouched (it used to move all four components). Section 20's entry-level assertion and 23g's
cross-view parity now read the marker level (component 0) while still comparing the same KEYS with the
timeline, and 29b's proportional value drag writes X only (`Y == 20` / companion `Y == 10`).

**Verified discriminating, both halves separately** (each patch restored by hand, run, reverted):
restoring the whole-key PRESS fails `graph_drag` at `FAIL 4670: editor.selection().count() == 2`;
restoring the whole-key VALUE expansion (keeping the marker press) fails at
`FAIL 4836: mk0->scale().y() == 30.0f`. `scripts/run_suites.sh Debug` = 53 pass / 0 fail.

Not driven live: the panel host hands the same `QMouseEvent`s to `GraphEditor::updateCursor`, and no
probe drags a key on the docked page yet.

## A stored tangent keeps its offset from ITS OWN key (2026-10-24)

The user's second report on the same round: "the Y of free handles seems to be relative on the value
axis, so when I move the keys on the value axis the handles scale strangely. The Y should be relative
to the key they're visually connected to."

**What was wrong.** The bezier's control point is stored as a FRACTION of the segment's value span
(`handlePointAt`, `ctrl/graph/graph_Geometry.h:177`: `value = fromValue + ny * (toValue - fromValue)`).
That is the right storage for playback - it is the curve's shape - but it made the DRAWN handle rescale
with the span: move the `to` key up and the `from` key's own handle (whose key did not move) slid up by
`ny * dv`, so its stem grew and its angle changed under the user's hand. `tests/graph_drag.cpp` 23f
(d) used to PIN that ("both params stayed byte-identical ... its pair N deg off the through-line").

**The rule now: the MODE is document state, the GEOMETRY is key-relative.** `KeyMoveCommand`'s fourth
half (`buildTangents`/`applyTangents`, `ctrl/GraphEditor.cpp`) captures, before the first value write,
every STORED side - `HandleMode_Free` or `HandleMode_Aligned` on a `Type_Custom` param - of the two
segments adjacent to a moved key: the param, which side (0 = `x1,y1`, the from key's outgoing; 1 =
`x2,y2`, the to key's incoming), the segment's two keys, the anchor key the handle is drawn from, and
the handle's VALUE offset from that anchor. Every step then re-derives
`ny = (anchorValue + offset - fromValue) / (toValue - fromValue)`, so the handle keeps its offset from
its key; `x` and both modes are never written, and derived sides (Vector/Auto/AutoClamped) are left to
their own derivation. The step is an absolute function of the document's values, so a rewritten step
and a redo reproduce it; `undo()` restores each captured param and the values, and the whole gesture is
still ONE command.

Two consequences worth stating. The EASING SHAPE changes when a key's value moves - that is what
absolute handles mean (Blender's model) - so the curve can bulge where it used to keep its proportions.
And an ALIGNED pair stays collinear IN PIXELS across such a move (both offsets are preserved and the
frame spans do not move), which is why the mode can keep being a stored fact rather than an inference.

**Measured.** `graph_drag` 23f (d) is re-derived around the new rule: moving B on the value axis only
(90.0 -> 79.4) leaves every mode and every X byte-identical, re-derives all four stored Ys, keeps each
handle's pixel offset from its key within 1px, and leaves the pair `0.000 deg` off the through-line
(it was `0.000` before the move; the old pin expected it to go OFF it). One undo restores both params
byte-identically (`fA == keepAB`, `fB == keepBC`) and the key's value. `scripts/run_suites.sh Debug` =
**53 pass / 0 fail**, `AnimeEffects:Debug` links.

**NOT wired, stated rather than implied:** only the GRAPH's value gesture re-derives. Other value
writes (the timeline's key panels, the SRT live commands, the bone/mesh/FFD tools) still leave the
fraction alone, because each of those is a direct `cmnd::ModifiableAssign` on key data rather than a
span-aware command - wiring them means teaching each its own reflow, and a live 3D drag would rewrite
easings on every step. A user editing values there will still see a handle rescale.

Also worth recording, because it was asked and answered: the SIBLING MARKER's ink was never the
regression. A marker whose key is selected on another component keeps the mock's faint 1px hue ring at
0.35 and no fill change (`mock-graph.js` `_keys`, `kKeySiblingRingAlpha`), and it has been that way
since `a9dd0fbc`; what changed in this round is only that the state now ARISES from a plain press,
which names one marker instead of the whole key.

## The Properties dock is a QML scene now (2026-10-25)

`gui::PropertyWidget` is a `QWidget` whose only child is a `QQuickWidget`; the old widget inspector
(`prop_Backboard`, `prop_CurrentKeyPanel`, `prop_ConstantPanel`, `prop_FilterStack`, `prop_KeyGroup`,
`prop_PropertyRow`, `prop_KeyStateButton`, `prop_KeyKnocker`, `prop_Panel`, `prop_AttrGroup`,
`prop_HeaderButton`, `prop_ProjectPanel`) is deleted. The split is deliberate:

* `gui::InspectorViewModel` (`qml/InspectorViewModel`) owns the document decisions through the same
  `prop::KeyAccessor` + `ctrl::TimeLineUtil` path the widget inspector used. Its child
  `InspectorRowModel`s expose the three-state dot (`static`/`animated`/`keyed`), the typed values,
  and the setters. `gui::FilterStackModel` / `FilterRowModel` own the stack and its HSV/Blur editors.
* `qml/PropertiesPanel.qml` and its controls own layout and ink. `InspectorRow.qml` is the one row
  geometry: gutter 16, label 42..114, field column 124..right-16, row 27, field 23. `InspectorNumberField`
  wraps the existing `ScrubSpinBox`; `InspectorKeyDot` is the timeline dot; `InspectorFilterCard`
  expands in place and drags to reorder.
* A numeric edit commits on release/Enter, not per scrub tick. One gesture is one `KeyAccessor`
  command, so one undo step; `ae_ui.py set` and the accessibility value path write the same commit.

Five traps cost real runs and are worth knowing before touching this scene:

- **QML has no `mapToScene`.** Mapping to scene coordinates is `item.mapToItem(null, x, y)`. The
  drag/reorder code silently threw `Property 'mapToScene' ... is not a function` until changed.
- **A delegate's `required property int index` is shadowed by a component property of the same
  name.** `InspectorFilterCard` used to declare `property int index`; the Repeater then passed its
  own default 0 and two different filters rendered the same row. The card property is `cardIndex`;
  the delegate keeps the required `index`.
- **A called `Q_INVOKABLE` is not a binding dependency.** The filter picker's `hasFilter(type)` did
  not re-evaluate after an add, so an existing filter never read "Added". The expression also reads
  `inspector.filters.rows.length`, which is the dependency `rowsChanged` notifies.
- **The panel is a `ScrollView`, so a child drag loses the grab after a few pixels.** Both the
  numeric scrub and the filter-header reorder set `preventStealing: true`; without it the page
  scrolls and the control never sees the release.
- **The scene binds to the model and theme, so it must die first.** `~PropertyWidget` deletes
  `mQuick` before the context objects; every dialog shell here follows the same rule.

The old `propertywidget.ssa` still carries the combo/spin/button rules the easing picker uses; its
dead inspector/filter selectors were removed. Labels that already had a catalog context are read
through that context (`gui::prop::MoveKeyGroup`, `gui::prop::ImageKeyGroup`,
`gui::prop::ConstantPanel`, `gui::prop::FilterStackPanel`), not re-spelled in QML. The scene's own
`qsTr()` context is `AnimeEffects.Properties` and has not been sent through `lupdate` yet.

**The QML default font is NOT the dock font, and `Qt.application.font` is not either.**
`QApplication::font()` is the process default (9pt here); the dock's 8pt Geist comes from
`LocaleParam` and is applied to widgets through `standard.ssa`. The first QML pass used 12px
literals; the first correction used `Qt.application.font`, which still rendered at 9pt, so the
size did not visibly change. `PropertyWidget` now builds a `QFont` from `LocaleParam`
(family + `pt`/`px` size) and exposes it as the QML context property `uiFont`; every inspector
`Text`/field uses `uiFont`. The shared controls carry an explicit `textFont`
(`ScrubSpinBox`, `AeCheckBox`, `AeComboBox`, `AeTextField`) whose default preserves their old
dialog size. Do not reintroduce `font.pixelSize` or `Qt.application.font` in the inspector.

The hierarchy the widget board got from `Panel`'s four-pixel group spacing is restored explicitly,
not by making every row taller: `PropertiesPanel.qml` inserts a four-pixel `Item` after Clipped,
after Path, after each of Rotation/Scale/Depth/Opacity, after Mesh cell, and after Pose/FFD when
present. Rows inside one key's group stay contiguous. Filter cards keep the old panel body's
12px left/right inset; the remove control is always laid out (only its opacity changes) so revealing
it cannot shift the header; the always-on painted grip dots were removed, while the header itself
remains the reorder target. An expanded card has no divider line: the header's top corners stay rounded, a 4px square patch
fills its bottom corners, and the body starts as the same card surface instead of a rounded pill
floating over a hairline. A 1px `theme.hairline` frame now runs around the whole expanded card
(header and body inset by that stroke), so the body reads as bounded instead of loose. BOTH the
header and the body are rounded: the body has a `radius` and a small base-coloured patch covers only
its top corners. Keeping the body square was what made the bottom corners of the outline disappear -
the square child covered them. Do not "fix" this by dropping the border again.

**Text rendering is the QML default.** The Qt Text documentation makes `renderType` a per-item
override: `NativeRendering` is the platform rasterizer, `QtRendering` is the default distance-field
path, and `CurveRendering` is the hardware curve rasterizer offered only when the default gives poor
visual results. The inspector therefore sets NO renderType: it uses `QQuickWindow::textRenderType()`
like every other QML scene here. At 8pt this platform renders with colored subpixel AA; that is the
default, not a bug. Do not force `CurveRendering`, `NativeRendering`, or the global
`QQuickWindow::setTextRenderType` to chase the QWidget inspector's grayscale - that is a deliberate
platform/QML choice, and the global setter changes every QML scene. The only font override is the
family and point size from `LocaleParam` (`uiFont`), which is what makes 8pt match the dock.

**In-field labels and units.** `ScrubSpinBox` draws a muted `prefix` and a themed `prefixIcon` at the
left, and appends the `suffix` to the displayed numeric string (`0` + `°`), so a unit is part of the
value rather than a separately right-aligned label. The inspector sets `reservePrefix: true`: every
field reserves the same 24px lead-in, and a fixed 15px slot from x=4..19 fits/centers the prefix
(text or icon) with no redundant side padding. The numeric text starts at x=24 in every field whether the glyph is X/Y, an icon, or
absent, and values stay LEFT-aligned. Prefix glyphs are rasterized at their physical footprint (`sourceSize = 15 * devicePixelRatio`) from
the official Phosphor FILL assets (`.../assets/regular`) - not the raw stroke assets, which Qt
rasterizes poorly at this size. The chosen vocabulary: Position/Centroid/Image Center use
`X`/`Y`; Scale uses Phosphor `arrows-horizontal`/`arrows-vertical`; Depth uses `stack` (layers/z-
order); Opacity uses `circle-half`; Mesh cell uses `grid-four`; Rotation/Blur angle use the official
Phosphor `angle` glyph plus a `°` suffix; HSV uses `H`/`S`/`V`. Prefix text and every glyph use the
SAME `theme.textMutedDim` token and the same opacity (icon roles `MutedDim`). Every filter header
summary comes from `FilterRowModel::summarySegments` and is rendered as ONE StyledText line at ONE
size: the bold title is full ink, labels are `textMutedDim`, values stay the original `textMuted`.
There is no smaller label, no second row and no full-opacity value. Summary numbers use the same
`summaryNumber` trim as the fields (so `0` never becomes `0.0`), and the item separator is three non-breaking spaces while
label/value uses one ordinary space (Qt StyledText did not give U+2003 a usable width). Blur uses `Amount`, or `X`/`Y`/`Angle` when
directional. The degree suffix is announced as "Degrees"; no other unit was
invented. Icons live in `assets/icons/ph/` and are tinted by the existing role table.

**Verified:** `scripts/run_suites.sh Debug` = 57 pass / 0 fail on the tree that removes the widget
classes; `inspector_view_model` pins model-2 writes, key toggles, filter add/reorder/enable/remove,
collapsed-card survival across a frame refresh, and undo. Live under Xvfb (`test.anie`, `Paint Layer
1`, 1580x980): the inspector opens with the unified grid and the dock's 8pt face, a scrub changes
357.5 -> 360.5 and one Ctrl+Z restores 357.5, key dots toggle, the picker marks an existing filter
"Added", two filters render distinctly and drag-reorder, and expanding/collapsing a card leaves the
dock stable. The captures are `docs/design/inspector/qml-panel.png` and
`qml-filter-stack.png`.

## The Wave Deform filter (2026-10-25): a UV deformation of the isolated composite

The filter stack's third entry (`TimeKeyType_Wave`, `src/core/WaveKey.h`) is a WAVE:
two travelling sine waves evaluated in the node's CONTENT space that displace the
SAMPLE COORDINATE of the composited pixels. The catalog entry that already said
"Wave Deform / Traveling wave distortion. Coming soon." is now the real filter
(`gui/qml/FilterStackModel.cpp`).

**Why it is a post-process and not geometry.** A layer is drawn through its own
mesh, whose cells are triangles that Mesh/FFD/Bone keys deform independently.
Moving vertices would move cells apart and tear the seams between them; the
composite is ONE continuous image, so displacing its sample coordinate cannot
tear anything. The same reason makes it correct on a clipped layer (the clip is
already baked into the composite) and on a folder (the whole subtree is one
image by then).

**The field, and the two things a wave can do to a picture.**
`FilterFrame::waveTexture` runs `kWaveFrag` (inline in `src/core/FilterFrame.cpp`
beside the blur/resample passes) over the isolated composite:

- wave 1 travels along `angleDeg` with `wavelength`; wave 2 crosses it at
  `dualAngleDeg` off that direction with `wavelength * dualScale`, mixed in by
  `mix` (a quarter-cycle phase offset keeps the pair from collapsing into one
  sine);
- `Deform_Ripple` displaces along the field's GRADIENT - water surfaces, ripples,
  heat haze, the refraction offset Wallpaper Engine's water effects produce;
- `Deform_Sway` displaces ACROSS the travel direction by the field's HEIGHT -
  cloth, hair, flags;
- `crest` shapes the sine AND its derivative together (the shader's `waveCos`),
  so a sharp crest has a sharp slope instead of a sharp displacement with a
  smooth slope;
- `speed` advances the phase from the DOCUMENT FRAME (`frame / fps`, never a wall
  clock), so scrubbing, playback and an export agree.

**Five graph channels, four step-held shape fields.** `strength` (px),
`wavelength` (px), `angleDeg`, `phase` (cycles) and `mix` are per-component eased
and appear in the animation graph; `speed`, `crest`, `dualAngleDeg`,
`dualScale` and the deform mode are STEP-HELD from the earlier key of an
interpolated segment (`TimeKeyBlender::blendWaveKey`). That split is deliberate:
the phase is `speed * time`, so smoothly interpolating a RATE would make the
picture jump at every key; `phase` is the channel to animate when the motion has
to ease, and `speed` is for a constant drift that needs no keys at all.

**The mapping is the layer's own draw path, in content pixels.** `core/WavePass.h`
builds `layerToScreen = (ndc -> composite pixels) * camera.viewMatrix() *
worldCSRTMatrix-with-imageOffset` - the same matrices `LayerShaderVert` draws
with - and the shader inverts it per pixel, evaluates the field where the pixel
sits on the ARTWORK, and maps the displaced coordinate back. So the wave stays
glued to the content while the layer moves, scales, rotates or the camera zooms,
instead of being a screen-space wobble. `waveAffine3` moves the 4x4's translation
from column 3 into the mat3's column 2; copying columns 0..2 instead silently
drops it (the pixel test caught exactly that: the whole field became
screen-space and the identity pass was not a copy).

**The sample is clamped into the content, and only where the content IS.** A
displaced sample may not leave the content rectangle, so the edge texels are
stretched instead of a transparent hole opening (an image effect's texture
clamp). Two rules the pixel gate pinned, each a visible defect before it:

- the clamp applies ONLY to pixels whose own coordinate is inside the rectangle.
  Clamping every pixel made a layer cover the whole viewport with a smear of its
  edge texel - measured by the gate as 16100 background pixels inside the
  content rect at `strength = 0`;
- the rectangle is the content's TEXEL-CENTRE range (`0.5 .. size - 0.5`), not its
  outline: clamping to the outline lands a saturated sample exactly between the
  last content texel and what is outside it, and a bilinear sample there is half
  of each (measured: a one-column strip of half-background at the silhouette edge);
- the inside test carries half a content texel of slack, because a content
  pixel's `q` is a texel centre recovered through a float matrix and an exact
  comparison can miss by one ULP - a missed clamp is a tear.

Gates: `tests/wave_pass_pixels.cpp` (real GL, own FBO: identity copy is
bit-exact; the band's edge follows `strength*sin(2*pi*x/wavelength)` at four
phases; a ripple along x leaves a y-only pattern unchanged while Sway moves it;
`leakedWithClamp=0` against `leakedWithoutClamp=11400`; every one of the ten
fields changes the frame) and `tests/wave_deform.cpp` (headless: the key's data
contract and clamps, the five graph channels reading/writing the blended fields,
a two-key blend with per-channel easing and step-held shape, a DEFAULT-only lane
still deforming, the 0.18 record round-tripping, and the mapping's identity,
rotation, inverse and collapsed-transform cases).

**Format 0.18.** `WaveKey` writes its easings then the nine values plus the deform
mode. `TimeKey::kMaxProportionalComponents` grew 3 -> 5 (a wave key has five
components), which changes the PROPORTIONAL payload for every key: the reader
takes the count from the file's minor version (`0.16-0.17` -> three, `0.18` ->
five). A 0.18 file therefore cannot be read by an older build, which is what the
format version is for.

**Adding it to a filter stack is visible immediately** (`TimeLineUtil::addFilter`
seeds `WaveKey::Data::presetData()`: 12px at a 140px wavelength, 0.4 cycles/s,
35% second wave) - unlike HSV and Blur, whose default is their identity, because
an inert wave would look broken on add. The row's key dot stays hollow and the
timeline gains no keyframe until it is pressed.

**Measured live** (Xvfb, `~/Downloads/ANIE/test.anie`, a 1102x792 layer, window
1430x870, the real catalog: Add filter -> type "wave" -> the one row): the canvas
changed by 13227 px of an 840x500 region when the filter was added, disabling it
through the card's eye restored 46 px (the playhead/border baseline), and seeking
to another frame changed 25679 px - the phase advancing with the document frame.
The card showed all ten rows (Strength, Wavelength, Direction, Phase, Dual wave,
Deform, Speed, Crest, Dual direction, Dual scale).

**Two traps for the next one.** The inspector is a `ScrollView` in a dock shorter
than its content: the Filters section sits at the panel's BOTTOM, so an "Add
filter" press has to come after wheeling the panel down - and the catalog popup is
rendered INSIDE the QQuickWidget, so on a short window its last rows are clipped
away (a taller window makes it usable; the probe that types into the search field
and clicks the single remaining row is the robust route). And the resize that
makes the window taller must target the real toplevel: `xdotool search --name
AnimeEffects` here returns the Qt selection-owner (3x3) and a 1x1 helper, NOT the
main window, so `windowsize`/`windowactivate` quietly do nothing; pick the id from
`xwininfo -root -tree` whose geometry is the large one.

**Not done here:** the new strings (`Wave Deform`, the ten field labels, the
catalog description) are not in `assets/locale/*.ts` yet - lupdate would add them
as `unfinished` in five catalogs and `run_suites.sh` gates that count at 0, so the
extraction belongs to a translation pass, exactly like the properties scene's own
`qsTr()` strings.

## ONE brush, four painting contexts, and painted filter masks (2026-10-25)

The filter stack's masks were the goal; the brush was the thing that had to be fixed first,
because there was no pixel brush to build them on.

**`ctrl/Brush.h` is now the ONE brush model.** `BrushParam` (radius in content px, continuous
hardness, flow, opacity, two tablet-pressure switches, spacing) + `BrushStamp`
(center, travel `delta`, radius, hardness, amount, erase) + `brushFalloff()` +
`BrushStroke`. The falloff is Photoshop's: a flat CORE of `radius*hardness` and a smoothstep
band to the rim, and it is the only definition of what hardness means.

`BrushStroke` is what fixes the defect every old brush had: a stamp per drag EVENT leaves a
fast stroke as dots. It walks the segment from the last stamp in `radius*spacing` steps
(floored at 0.5px), interpolating the pen pressure between the two samples, and carries the
distance into the next event so an event boundary neither gaps nor double-stamps. Measured
(`tests/brush_stroke`): a 100px move at radius 10 / spacing 0.25 emits **41 stamps with a max
gap of exactly 2.500px**, the press stamp lands first, the cross-event gap is still <= the
step, a 0.2->1.0 pressure ramp rises monotonically 0.200->1.000, and a hairline brush's step
floors at 0.5px.

**What is shared and what is not.** The stroke, the falloff and the parameter names are
shared; a stamp's EFFECT is the target's - raster deposits alpha, FFD displaces vertices,
influence widens bone ranges - which is why a stamp carries only `amount` and a distance
falloff. `delta` exists for the WARPING targets: a spaced stroke that applied the whole event
velocity per stamp would multiply its displacement by the stamp count, so the FFD task moves a
vertex by the travel since the previous stamp instead.

**Adopted into the three old brushes, not left beside them.**

- **FFD**: the `HARDNESS` shader variation (three compile-time exponents, `pow(t,2|4|8)`) is
  GONE. `ffd::TaskResource` injects one GLSL copy of `brushFalloff` after the `#version` line
  of the deformer AND the eraser, and passes `uBrushHardness`/`uBrushAmount` as uniforms, so
  the mesh brush's edge is the raster brush's edge and hardness is continuous. `FFDParam` is
  `{BrushParam brush; float blur; float focusRadius; Type type}` - the whole duplicate
  `eraseRadius/erasePressure/eraseHardness` set is deleted, and erase is the same brush with
  `erase` set. `tool_FFDPanel` is Type + `BrushPanel` + Blur. Verified live (Xvfb, a stroke
  across the artwork): **11027 canvas px changed, one Undo restored 0 px**, three icons'
  tooltips still reachable, no shader error in the log.
- **Bone influence paint/erase**: the hard-wired `0.3 * brushRange` core is now the brush's
  `hardness`, the deposit is `amount * brushFalloff(distance to the bone, radius, hardness)`,
  and both modes share ONE `BrushParam` (`piRadius/piPressure/eiRadius/eiPressure` deleted).
- **Pose erase**: `eraseRate = 1 - amount*falloff(distance)` per bone, one stroke.
- **`BrushPanel`** (Size / Hardness / Flow / Opacity / the two pressure switches) is now the
  one block the FFD, Bone, Pose and Mask panels embed; a tool that cannot honour a row HIDES it
  (the FFD mesh brush hides Opacity: a displacement has no per-stroke cap to blend).

**Painted filter masks.** `core::FilterMask` is one 8-bit field in the node's CONTENT space
(a layer's image pixels, a folder's canvas box), capped at **1024** texels on the long side
(a mask is low frequency; 15000px of canvas is ~15px per texel at 1MiB instead of 225). It is
stored per (filter LANE, CHANNEL) in `TimeLine`, NOT in `FilterEntry`: the stack is
value-replayed by its commands and megabytes of pixels must never travel through one. Format
**0.19** writes the masks after the stack (type token, channel, rect, texel size, PNG blob);
older files have none. The texture uploads on the revision the dirty box bumps (a stroke
uploads the box it touched, not the field).

**The neutral value is per channel, and it is what makes a fresh mask a no-op**: an AMOUNT
mask starts WHITE (full effect) and painting REDUCES it - Photoshop's layer mask and Wallpaper
Engine's opacity mask both start at full; a TIME OFFSET mask starts BLACK (no delay) and
painting ADDS delay. `core::filterMaskNeutral()` is that table. The wave shader samples both
in content space: `amountScale = mask` multiplies the displacement, and
`phase += mask * timeMaskCycles` delays it (the scale is `WaveKey::Data::timeOffset`, added in
0.19 as a step-held field, default 0.5 cycles).

**The brush paints TOWARD a Value** (0..1), not toward white or black: a stamp moves the texel
toward it (`v += (target - v) * weight`), so repeated passes converge instead of overshooting,
the same slider reduces an amount and adds a delay, and the **temporary eyedropper** has an
obvious thing to pick: Alt + LEFT CLICK on the canvas samples the field under the cursor and
puts it back into the panel (a click, not a drag - the compositor owns Alt+drag, measured).

**Gate evidence.** `tests/wave_pass_pixels` [6]: an amount mask painted to 0 on the left half
stops that half (**movedWhereMasked=0**) while the neutral half still moves
(**movedWhereClear=1360**), and a time-offset mask produces a different deformation (2720px)
from the same wave. `tests/wave_deform` [6]: both channels allocate at their neutral value,
paint, and survive serialize/deserialize with their content rect and texel grid.
`tests/brush_stroke` pins the falloff, the stroke and the drag's axis latch. Live (Xvfb, `test.anie`, a Wave filter
added through the catalog, the Mask tool from the toolbox): painting the left half changed
**2720 px**, one Undo restored everything except the tool's own brush ring.

**Three traps, all measured.**
- A stroke's undo must mark the FIELD DIRTY, not just restore its pixels: `AssignPixels` holds
  the `FilterMask` for exactly that reason (the first version restored the image and left the
  wave masked on screen, because the texture only re-uploads on a revision change).
- `QImage::pixelIndex()` is for INDEXED formats. A `Format_Grayscale8` field is read as a byte
  (`constScanLine(y)[x]`); `pixelIndex` answers 0 for it, which read as "the mask did not
  start neutral" in two gates.
- The toolbox FlowLayout WRAPS with a seventh tool: the mask brush is at **(22,156)**, on a
  second row, not in a seventh slot of the first - and a hidden tool panel still answers
  `ae extents`, so a probe that "finds" it that way then paints with the wrong tool.

**A permission prompt on the developer's desktop, and the fix.** An apostrophe in a probe's
single-quoted `bash -c` body ended the body (the trap this file has recorded before, again in
a comment: "the first row's 7th slot"), and the leftover lines ran in the OUTER shell, where
`DISPLAY` is the real session - `import -window root` then raised a screen-capture permission
prompt under Wayland while the probe was supposed to be offscreen. `scripts/gui/_lib.sh` now
does `unset DISPLAY; unset WAYLAND_DISPLAY` in the OUTER shell, so a leaked command has no
display to reach and fails instead of touching the person's desktop. Keep probe bodies free of
apostrophes and prefer `$SHOTS` (exported) over a `'"$SHOTS"'` splice.

### Masks reach HSV and Blur, and the brush got its gestures (2026-10-25, second round)

**The HSV amount mask is a shader variation, and it isolates the layer.** A masked HSV
adjustment has to be per pixel, so it cannot be baked into the layer draw: the fragment
shader gained `#variation USE_HSV_MASK` (the field scales `mix(original, adjusted, mask)`,
sampled in content space through the SAME `uScreenToLayer` the wave pass uses), and
`LayerNode::hasActiveHsvMask()` puts the layer on the composite path when an HSV key is
playing and a field exists. Folders already ran HSV as a stack pass, so they only needed
the field. Gate (`tests/wave_pass_pixels` [7]): with a hue shift of 180 degrees the
adjustment would change **40000** pixels of the masked half and keeps **40000 of 40000** at
the original colour, while the unmasked half carries the full adjustment.

**The blur cannot vary its kernel, so a masked blur renders both and mixes.** `FilterFrame::
mixTexture` is `mix(sharp, blurred, mask)` as one full-screen pass (`#8`: the sharp half is
the sharp render **45000 of 45000**, the other half takes **10426** blurred pixels). Two
facts make it work: the LADDER blur reads and writes the SAME slot (`blurApply`), so a masked
ladder blur copies the composite first (`copyTexture`, a `Kind_Resample` passthrough); and
the sharp source is kept because the blur goes into a second slot instead of back into the
composite. Cost, as measured earlier for the question that started this: one extra
full-screen pass (+10% of a direct 16px blur, and the absolute extra is 0.35 ms at 1920x1080
on the R9 380).

**The trap, and it looked exactly like a shader bug.** `drawQuad` has ONE branch chain for a
pass's bindings *and* its uniforms. The mix's texture bindings went into that chain while its
uniforms went into a second `else if (aKind == Kind_Mix)` appended after the wave branch -
dead, because the first branch already matched. A sampler uniform left at its default unit 0
made `uSecondTexture` and `uMixMask` both sample the SHARP texture, so every masked blur read
as "no blur anywhere" (`blurredRight=0`) while the blur itself was obviously working
(`blurSelfDiff=20852`, measured in the same run). One branch now does both.

**The brush's familiar gestures.** `BrushPanel::adjustSize/adjustHardness` are the one
adjustment path: `[ ]` (one 1.1x notch, 5% hardness) via four rebindable commands
(`BrushSizeIncrease/Decrease`, `BrushHardnessIncrease/Decrease`), and Alt + RIGHT drag
(horizontal = size, vertical = hardness) through `MainDisplayWidget::onBrushDragAdjusted` ->
`ToolWidget::adjustBrush`. Right button on purpose: the compositor owns Alt + LEFT drag.
Measured live on the mask tool: three `]` took Size 50 -> **66**, four Shift + `]` took
Hardness 50 -> **70**, and one drag banked size **+7 / +8** steps and hardness **-1 / -1** -
the last number being the defect the axis lock fixes: that was ONE drag, and its 2px of
sideways jitter moved the size.

**The drag's axis is latched in `ctrl::BrushDrag`, and that is not Photoshop parity.** The
decision lives in `src/ctrl/Brush.h` (header-only, no widgets) so the gesture is one policy
instead of three literals in the event handler: 4px of run-up in either direction fires
nothing, the DOMINANT axis then latches for the rest of the gesture, the other bank is
zeroed, and the run-up travel is DISCARDED rather than banked (banking it would fire the
first step the instant the latch engaged). After that only the latched component is banked,
at the old rates - 8px per size step, 40px per hardness step - with the remainder kept.
Photoshop's own Alt + RIGHT HUD is a 2D PAD: horizontal drives the size and vertical the
hardness at the same time, and its "Vary Round Brush Hardness" preference only enables the
hardness half; there is no dominant-axis snap to copy. The axis is not re-evaluated
mid-drag: at ~45 degrees the winner would flip between samples and a flip would have to
drop the other axis' bank, which moves a value the user did not ask to move.
`tests/brush_stroke` [5] pins the slop, the latch, the discarded run-up and the rates.

The two-ring brush cursor follows the pointer during the drag: the move handler calls
`AbstractCursor::setMouseMove` (position only, gated on the same `updateCursor()` the normal
path uses) and never `setMousePress`, so each tool's `updateCursor` re-reads the position
without starting a stroke or moving tool state. Before this the press and every move returned
before the tools saw them, and the ring sat frozen at the press point while the values
changed. There is no numeric HUD: the viewport overlay engine has no `drawPath` and no
`setClipRect` (both no-ops), so a panel with rounded corners and clipping is a design
decision this change does not own.

**A Shift binding is spoken on the unshifted key, and Qt answers with the shifted symbol.**
`Shift + ]` arrives as `BraceRight + Shift`, and the matcher compares the key exactly - so the
hardness keys silently did nothing while `]` worked. `ctrl::unshiftedKeyForShift()` folds the
symbol back, and it is applied in **`KeyCommandInvoker`** (the one place a rebindable command
fires), NOT in `MainWindow::keyPressEvent`: the first `MainWindow::keyPressEvent` in that file
is inside an `#if 0`, so the first fix went into dead code and changed nothing. A gate for
this class is `Shift+]` changing the hardness label, not a unit test of the folding alone.

**The temporary eyedropper is verified end to end.** Alt + left click on the canvas samples
the field under the cursor into the panel's Value (MaskEditor::sampleValue; the click is
recognised in the viewport because that is where the modifier state lives). Measured: after
painting the left half, `Value:  0`; Alt + clicking the untouched half reported
`Value:  100`; Alt + clicking inside the soft stroke reported an intermediate value.

## The bone/pose influence brush, the mask preview, and three gate repairs (2026-10-25, third round)

**The influence brush works, and the first probe said it did not.** The Bone tool's Paint/Erase
influence modes ran on their own local vector of `Bone2*` from the C++03 tree this app was ported
from; the bone tree here is `QVector<Bone2*>` (top bones) and the modes were rewritten on
`BrushStroke`/`BrushStamp` like the mask. Measured live (Xvfb, `test.anie`, Bone tool -> two clicks
to create a bone -> "Bind bone to node" -> "Paint influence", stroking ALONG the bone): **55 stamps**,
`weight=1.000`, the band widened by **1741 px**, and one Undo restored **0 px of difference**. The
mode icons are addressable BY NAME ("Add bones", "Bind bone to node", "Paint influence" - they are
`SingleOutItem` tooltips, which AT-SPI publishes), so a probe needs no guessed pixels for them.

The mistake worth recording: the first attempt stroked 20 SCREEN px away from the bone and nothing
happened, which reads as "the brush is broken". The brush radius is in WORLD units, so at
`test.anie`'s zoom (0.195) a Size-50 brush is a **~10px circle on screen** - the stroke was five
radii away from the bone. Instrumenting `updatePaint` (stamp centre, vlen, brushRange, the bone's
ranges) settled it in one run: `vlen=10.3 brushRange=50` painted, `vlen=100` did not.

**`BrushPanel` never set ITS OWN geometry.** Every tool panel here lays its rows out and then calls
`setGeometry`; the shared brush panel placed its sliders relative to a caller-provided origin but
left its own widget at the default size, so the Bone/FFD/Pose/Mask panels showed a malformed row of
three clipped boxes under the title. It now sets its geometry and returns the ABSOLUTE bottom edge
(`aPos.y() + local`), which is the convention every other panel's `+ 5` chaining assumes.

**The field is PREVIEWED on the canvas while the Mask tool paints it.** Painting blind is the one
thing a mask tool cannot do, so the viewport draws the field as a tint over its own target's
composite: `RenderInfo` carries `maskPreview` (the field), `maskPreviewTarget` (whose node) and
`maskPreviewColor`, filled each frame from `MaskEditor::previewMask()`/`previewTarget()`.
`FilterFrame::tintTexture` is a `Kind_Tint` pass - `rgb += colour.rgb * |mask - neutral| * colour.a`
- so an UNPAINTED field tints nothing (the neutral value is the field's own, carried on
`MaskInput::neutral`), and a painted one shows exactly where. A node that is the preview target is
pushed onto its composite path even with no active filter, because the tint is one pass over that
composite; a folder needed the same line in `FolderNode::render`. Measured live: painting the left
half changed the artwork by **4589 px** with the tool up and **2120 px** with it down (the effect
alone); the UNPAINTED right half was **0 px** different, and the amount mask's damping stayed after
the tint went away. The time-offset channel measured **1098 px** for the effect alone.

**The card's mask rows were invisible because a label was not a Q_PROPERTY.** `maskLabel`,
`timeMaskLabel`, `paintMaskLabel`, `clearMaskLabel`, `paintTimeMaskLabel` and `clearTimeMaskLabel`
were plain methods on `FilterRowModel`: readable from C++, `undefined` in QML, so the two
`InspectorRow`s the card declares never appeared (and the app's own log said nothing). They are
`CONSTANT` Q_PROPERTYs now, and the rows read `Mask | Paint mask | Clear mask` and
`Time mask | Paint time offset mask | Clear time offset mask` in the accessibility tree. Verified
live: `Paint mask` presses through to the Mask tool on the WAVE lane, `Clear mask` returns the
effect in one undo, and the time row does the same on the time channel. The card is taller than the
dock's viewport with a wave filter expanded - a probe must scroll the inspector before pressing the
time row (measured: `hidden` before the wheel, `showing` after).

**Three build/gate repairs, all of which had been lying.**

- **Ninja's own state can corrupt and then "succeed" silently.** `ninja` began exiting on
  `stl_vector.h: operator[]: Assertion '__n < this->size()' failed` (`Node*` is ninja's graph
  type), and `cmake --build ... | grep -E "error:"` showed NOTHING while the binary stayed at its
  old mtime - several rounds of edits were never compiled. The fix is `rm -f build/.ninja_deps
  build/.ninja_log` (ninja regenerates both). After any build where a claimed change has no effect,
  check the binary's mtime before instrumenting the code.
- **The half pixel belongs to the RASTERISER, not to `paintMarquee`.** The marquee's 0.5px inset was
  applied in the shared helper, which is right for QPainter/Canvas2D (centred strokes) and wrong for
  the ImGui backend (its antialiased 1px line already lands on the row its coordinate names): the
  graph panel's box-select edge was split over two rows (`#d543fd`/`#d346fd`, 55 from the token).
  `paintMarquee` now passes the RAW rect - the model's own geometry - and `InkQPainter`/
  `InkCanvas::strokeRect` inset by `width/2` themselves. The gate reads `d=1` on the ImGui host and
  `time_body_canvas` stays GEOMETRY 0, so the three backends agree again.
- **A hairline gate asserted "far from the hairline", which the palette had made false.** The row
  above the ruler's rule must be the BAND'S SURFACE, and in the dark palette the band (`#171717`)
  is only 15 from the hairline (`#262626`) - the check's `> 24` bar was a palette coincidence, not a
  property. It compares against `theme::Graph::rulerBand()` now (`d_surface=0`).
- `tests/graph_panel_model`'s legend-group expectation pinned the literal "Move [X][Y]"; the display
  name is "Position" now, so it asks `core::TimeKeyType` through `getTimeKeyDisplayName` and pins the
  SHAPE (`"<type> [X][Y]"`) instead of a string another surface may rename.

`scripts/run_suites.sh Debug` = **60 pass / 0 fail** after all of it (it was 58 pass / 2 fail:
`graph_panel_model`, `graph_panel_pixels`, both from the two stale expectations above).

**A warning-free claim needs a build that COMPILED the file.** The round's own new sources carried
four `-W` findings that no incremental link had shown (`AssignPixels`' unmarked overrides, an unused
lambda capture in `MaskEditor`, a dead `mUILogger` member, and a `-Wreorder-ctor` from
`MainDisplayWidget`'s brush-drag fields being initialised before `mTimeLineRangeSlot`/
`mPlayWindowConnection`), plus `tool_BrushPanel.cpp`'s `kItemLeft` after its inset moved to the
caller. `touch`ing the files and rebuilding is the cheap way to ask; the `MaskEditor` logger was
then DROPPED rather than silenced - a brush over a field never logs, so the parameter was a dead
dependency on the driver's logger.

## The `notes.md` low-entropy round (2026-10-26)

Ten reported defects, each traced to one mechanism before it was touched, plus two the reports did
not name. Every line below was measured on this tree; the review pack with the screenshots is
`docs/review/2026-10-26.md`.

**A rule that sits ON a floater uses the floater's EDGE, not `hairline`.** `QMenu::separator` painted
`@hairline@`, and in the dark palette `hairline` and `floaterBody` are the same cell (neutral column 8,
`#262626` on `#262626` = 1.00:1), so every menu separator was invisible while the gap it occupied was
not. `floaterEdge` is exactly one step above the floater body in BOTH themes (dark `#404040` = 1.46:1,
light `#e5e5e5` - pixel-identical to the old `hairline`), so the fix is one token at three sites:
`standard.ssa`'s `QMenu::separator`, the combo popup's hand-painted rule (`AppStyle.cpp`'s
`CE_MenuItem` separator), and `theme::Graph::mMenuSeparator`. **Do not** "fix" `@hairline@` globally:
it also paints dock sashes, splitters, tree branch guides, the ruler line and the lane seams, all of
which sit on `base` where the token reads 1.73:1 and is correct.

**A filter is live when its BLENDED value is above identity - never when its lane has keys.** Six
renderer gates (`LayerNode`, `FolderNode`) asked "this lane has explicit keys and the frame is at or
after the first key" as a proxy for "this filter does something", which threw away exactly the value
the panel shows: an edit with no keys lands on the lane's DEFAULT key (`prop_KeyAccessor`'s
`editFrame`), `TimeKeyBlender` blends that default between keys, and the renderer then ignored it. So
a Blur set to 20px rendered nothing until it was keyed, while a Wave (whose gate read the blended
value) worked. `core/FilterActivity.h` is now the ONE predicate - `filterValueIsActive(type, expans)`
- and both the isolation decision and the composite loop's `continue` read it, so a wave-isolated layer
cannot skip a live static blur. Two consequences recorded in place: the `frame >= firstKey` clause is
GONE (playback holds a property's first key before it, and no setting expresses "start at the first
key"), so a keyed blur/HSV now also applies before its first key; and `hsvDataIsIdentity` moved out of
`FolderNode.cpp`'s anonymous namespace so the two nodes cannot disagree.

**`x(t) == t` when both handles sit on the thirds, so the x→t solve is the identity.** With
P1=(1/3,y1), P2=(2/3,y2) the bezier's x-component is `t`, and `y(x)` is a plain cubic Hermite - the
WebKit `UnitBezier`'s spline table, bracket scan and Newton steps are all doing nothing. `Easing.cpp`
now takes a fast path for exactly that pair (≈11 flops, no division, no array, against ≈90 flops and
three divisions) from BOTH funnels, because applying it to one would make playback and the graph panel
disagree. Exact float equality, never an epsilon (an epsilon would make a drawn curve jump while a
handle is dragged past the boundary), and gated to `0 <= x <= 1` so `UnitBezier::solve`'s
endpoint-gradient extrapolation is untouched. Which modes reach it: `Vector`, `Auto`, `AutoClamped`
(the x is thirds verbatim) and a frozen ghost, whose stored floats came from one. `Free`, `Aligned` and
the crossed default pair `(2/3, 1/3)` do NOT, and `easing_regression` pins all three: the fast path
agrees with the solver to **4.2e-8** (below the solver's own 1e-7 epsilon), the crossed pair still
differs from the Hermite value by **0.1173** at x=0.355, and an arbitrary pair is still solved.

**The tool panels' placing convention: a row item answers an EXTENT, a panel answers an absolute
BOTTOM edge.** `BrushPanel::updateGeometry` chained its rows with
`item->updateGeometry(curPos, w) + kItemGap`, which is right for a panel (whose callers do
`panel->updateGeometry(...) + curPos.y()`) and wrong for a row item (`SliderItem` returns
`kLabelHeight + kSliderHeight`, `CheckBoxItem` returns `height + 1`). Every row therefore landed near
the top: measured on the FFD panel, Hardness, Flow and Pressure size all reported `y=262` and Blur
`y=254`, i.e. three controls on one row and the card reporting 125px for ~192px of content. The two
lambdas now accumulate, and the mask panel's Clear button no longer reads `mClear->geometry().height()`
(a default 100x30 on the first pass, 24 after - a 6px jitter). The panel family stays manual-geometry;
one `updateGeometry()` pass drives all of it.

**Filter-card expansion is a SET keyed by `core::TimeKeyType`, and expansion may change INK, never
GEOMETRY.** The stack held one `mSelected` type and re-derived every row from it on every refresh, so
two cards could never be open and a reorder (which destroys and recreates every `FilterRowModel`)
collapsed them all. It is a `QVector<TimeKeyType> mExpanded` now, pruned on add/remove and used to
SEED each new row. The second half is the header: `x/y/width` were a function of `expanded` (a 1px
inset to make room for the frame stroke), so every toggle shifted the header's content +1px and its
right edge -1px. Geometry is constant now and the 1px frame is drawn ON TOP of the fills (`z: 1`), so
the expanded card looks the same and nothing moves.

**The catalog popup's keys belong to the FIELD, and its Return path was broken.** The popup's content
is parented to the window overlay, so a `Keys` handler on the stack's root would never see a key; the
search field opts in with `navigationKeys` + `onNavigate`/`onAccepted` (the `DestinationPicker`
convention, pinned by `tests/picker_keyboard.cpp`). Return also called `item.entry`, which does not
exist on the delegate (`catalogEntry` does) - so Return closed the popup and threw a `TypeError`,
adding nothing. The rows are a clipped `Flickable` with an attached bar now, the highlight skips
ineligible rows and clamps (no wrap), and the pre-lock rule holds: a pointer entering a row releases
the keyboard's highlight so two never coexist.

**A separator in an index-addressed dropdown is DELEGATE DECORATION, never a list row.** The blend
list is a `ListView` whose index IS the option index (`activated(index)` -> `options[index]`), and the
widget-era control carried its five group boundaries as `QComboBox::insertSeparator()` model items
(`git show d8d99d1e^:src/gui/prop/prop_ConstantPanel.cpp` recovers them: after Normal, Darker Color,
Lighter Color, Hard Mix and Divide). Putting rows back would shift every index and let the keyboard
`activated()` a separator. The model carries `separatorAfter` and the delegate grows a 9px band with a
1px `floaterEdge` rule 8px in from each side (the style's own geometry). The list also has the app's
scrollbar now: a 12px lane and a 4px radius-2 pill in `theme.outline` (`theme.hover` while hot), which
is what `AppStyle::drawComplexControl(CC_ScrollBar)` has always painted and what
`PropertiesPanel.qml` reproduces - 28 rows do not fit a 220px popup, and the control ships it to every
dropdown, not just Blend.

**The command log is a RECORD, and it stays a text document on purpose.** "Undone : X" was an
assembled string with a verb, a separator and an often-empty command name (plain commands return an
empty `name()`), pushed as raw HTML through `appendHtml` - so the text was interpolated unescaped and
the ink was frozen at push time. `gui::LogEntry` + `gui::CommandLogView` split kind from text, write
each block with a `QTextCursor` (no markup), render the verb as a tinted badge and the name in the
primary ink with a single space between them, and re-ink in place on a theme switch. It stays ONE
document because cross-entry selection and copy is the requirement the note protects: a QML list of
chips cannot be drag-selected, and no `QTextCharFormat` can round a corner - so the badge is a
tinted run, and that limit is stated in the code. `UILogType_Warn` finally renders (the old
`pushLog` discarded it), which needed `warning`/`danger` to become real `theme::Colors` tokens:
`QmlTheme` had been hardcoding those hexes with a comment saying the token table had no home for them.

**`QQuickWidget::keyPressEvent` never calls the base, so an in-window QML scene ends the key walk.**
The Properties dock's `QQuickWidget` keeps `Qt::StrongFocus`; with no active focus item in the scene
it dropped every key, and with a `TextInput` focused it handed Ctrl+Z to that field's text undo. Every
rebindable command was dead while the inspector held focus - the command map, the binding and the
invoker were all correct. Two changes: a small `QQuickWidget` subclass in `PropertyWidget.cpp` calls
the base first and then `event->ignore()` ONLY when `quickWindow()->activeFocusItem()` is null (so a
focused text field keeps its own undo and QML `Shortcut`s keep working), and `MainDisplayWidget` takes
`Qt::ClickFocus` so a click on the artwork returns the keyboard to the document - the viewport used to
be `NoFocus`, which is why "click the canvas, then Ctrl+Z" could not repair anything.
`scripts/gui/probe_undo_key.sh` asserts it, including that a focused number field still swallows the
key.

**A PSD layer the app cannot reproduce imports DEGRADED and warns; it does not abort the import.**
A solid/gradient/pattern fill layer carries no RGB channels, so `hasImage()` was false and
`ImageFileLoader` returned failure: the whole file failed to open with a modal "Loading Error". An
adjustment layer (zero rect) became a silent 1x1 transparent node instead. `img/PSDUtil.h` now
classifies a layer from its `additionalInfos` keys plus the channel set, the loader creates the same
placeholder it already made for the zero-rect case and appends one warning naming the layer and its
kind, layer masks are aggregated into ONE line for the file ("N were ignored" - they are parsed and
never composited, which is a large separate cause of "it looks different"), and `System::LoadResult`
carries the lines to the GUI's log pane plus one non-blocking warning popup. The relink walker
(`res_ResourceUpdater`) names a kind with the same vocabulary, so the two PSD walkers cannot disagree
about one file. Gate: `psd_layer_identity` builds a fill-layer PSD and a masked PSD and asserts that
both open, that the warnings name the layer and its kind, and that the masked layer's own pixels still
import.

**`TreeNodeBase`'s const `front()`/`back()` were ill-formed and had never been instantiated.** They
returned `const TreeNodeType*&`, which cannot bind to what the container hands back (`TreeNodeType*
const&`); the first const walk of a node tree (the new PSD gate) stopped the build on a header that
had been wrong for years. They are `TreeNodeType* const&` now - a const tree still hands out a pointer
to a mutable node, because the node's contents are the document and only the child list is const.

## Driving the QML inspector from a probe (measured 2026-10-26)

Three facts, none of which is visible in the code, and without them no probe can touch the QML
Properties panel at all:

- **Every Qt Quick item in the dock reports `showing=False`.** `gui::FormAccessible::state()` sets
  `invisible = !item()->isVisible()`, and `ae_ui.py`'s `candidates()` filters on `showing()`, so
  `extents`/`click`/`set` answered "no widget named ..." for every control in that panel.
  `AE_UI_ANY=1` opts out of that filter (`scripts/gui/ae_ui.py`); it is deliberate and opt-in, because
  "a hidden control is not a target" is exactly right for the export/settings dialogs.
- **The C++ marker rects are absolute and the frame-origin heuristic is not needed for them.**
  `FormAccessible::rect()` returns `item()->mapToGlobal()`, so SCREEN and WINDOW are identical BY
  CONSTRUCTION for `aeAccessibleValue`/`aeAccessibleCombo`/`aeAccessibleOption`, while Qt's OWN QML
  nodes report a widget-relative SCREEN. `cmd_extents`'s "if SCREEN == WINDOW, add the top-level
  frame origin" therefore double-adds for a marker on a WM'd session and misses for a Qt node. Every
  probe here runs under Xvfb with no window manager, where the frame sits at (0,0) and the correction
  is zero either way - so this is a REAL-DESKTOP caveat, not something to "fix" with a guess. Measured
  under openbox: the QML root SCREEN `1120,56` WINDOW `1120,35`, and a marker `1244,58` in both.
- **The panel's rows report STALE rectangles until a layer is selected.** `PropertiesPanel.qml` binds
  the content column's `visible` to `inspector.hasTarget`; with nothing selected the column is hidden
  and its positioner never runs, so Position X and Rotation both report `y=37`. Select a row first,
  then read a rect.

## Writing a filter value through the inspector: a probe trap, not a proven defect (2026-10-26)

The renderer's filter predicate was fixed this round (`FilterActivity.h`, gated by
`wave_deform` [7]) and the fix is right end to end - a `Blur` whose value sits on the
lane's DEFAULT key renders after the project is saved and reopened (measured: the
relaunched canvas differs from the pre-filter state by 25385 px, 8.4% of the canvas crop).

What is NOT settled is why the same write appeared to do nothing in the live session,
and the evidence points at the PROBE rather than at the app:

- `ae_ui.py set` takes the FIRST candidate with a `Value` interface, and a Qt Quick field
  has TWO: the app's own `aeAccessibleValue` marker (whose `accessibleSetValue` calls the
  field's `valueEdited`, i.e. the document path) and Qt's default node for the same
  widget. Qt's node writes the QML property AND EMITS NOTHING, so the field shows the new
  number while the document keeps the old one.
- Measured consequence: after `ae set "Blur amount" 200` the panel reads 200 back from
  its own row model whether or not the model took it (`FilterRowModel` keeps `mBlurX`/
  `mAmount`, and `blurData()` falls back to the cached expans). "The panel shows 200" is
  therefore NOT evidence that the document has it. `Edit -> Undo` changing the field back
  IS evidence that a command was pushed - and one run showed that while an instrumented
  `KeyAccessor::assignBlur` never printed in another.
- The same trap applies to any QML field a probe writes: prefer the marker. The reliable
  discriminators are an undo, or a save+reopen.

Also worth knowing for whoever continues this: the live invalidation path is
`Driver::updateKey` (`ctrl/Driver.cpp:107`) -> `mBlender.clearCaches(aEvent)` +
`updateCurrents(...)`, and `TimeKeyBlender::clearCaches(TimeLineEvent&)` walks
`targets()` ONLY - a DEFAULT-key edit pushes just a `defaultTargets()` entry, so a
default-key edit clears no cache at all. `updateCurrents` re-blends the whole tree right
after, which is why the display still normally catches up. And `TimeLineEvent`'s CONST
accessor is misspelled `detaulTargets()` with no callers - a one-character landmine.

## One scroll bar in QML, and the two traps it cost (2026-10-26)

`notes.md`: "the Blend dropdown gained scroll, but its styling is inconsistent with other
scrollbars." Nine QML surfaces had a bar and they said FOUR different things: the widget
bar (`AppStyle`'s `CC_ScrollBar`, which paints `ctrl::graph::scrollBar::kLaneThickness`
12 / `kPillThickness` 4 / `kPillRadius` 2, `theme.outline` and `theme.hover` while hot),
an 8px lane with a 6px `textMuted` pill invented by the first QML dialogs
(`DestinationPicker`, `ExportDialog`, `GeneralSettingDialog`, `KeyBindingDialog`,
`NewProjectDialog`, `ProjectSettingDialog`), a 12px copy with a self-fading pill
(`PropertiesPanel`), and two of mine that copied the numbers and rendered a **1px**
handle. `src/gui/qml/AeScrollBar.qml` is the one implementation now, used by all nine
sites: 12px invisible lane, 4px pill, radius 2, `outline`/`hover`, a 28px minimum thumb,
shown only when the content overflows.

**Trap 1: `contentItem`'s `implicitWidth` does NOT size the handle; `padding` does.** The
Basic style's own bar gets its thickness from `contentItem.implicitWidth` because it also
lets the CONTROL's `implicitWidth` be `implicitContentWidth + paddings`; a component that
pins `implicitWidth: 12` on the control instead leaves the handle to the layout, and a
Rectangle without an explicit width collapsed to one column of ink - measured, and only
visible by looking (a 1px column of `#404040` at the popup's edge reads as a border, not
as a bar). `padding: 4` on a 12px lane is the same arithmetic as
`ctrl::graph::scrollBar::pill`'s `(lane - pill) / 2` offset, so the two painters agree.
Note also that `width: 12` has to come from the CALLER: a binding that named the other
axis would be self-referential.

**Trap 2: measuring a bar in a screenshot needs the right row.** The first "the pill is
1px" readings measured the popup's right BORDER (same `floaterEdge` colour) because the
scan's y-range was below the popup. When the ink and the border share a token, mark the
handle temporarily (`border.color: "red"`) and measure its rect - 4 columns x 60 rows is
unambiguous where a threshold count is not.

## The filter catalog opens where it FITS, and the height that came out NaN (2026-10-26)

The popup's room cannot be assumed: the Filters section is the panel's LAST one, so on a
1280x662 window `root` sits at scene y=321 inside a 396px host and there are **35px** below
its 32px header while there are **313px** above; on `test.anie`'s short board the same
section sits at the top and there is room below. So `onOpened` MEASURES
(`root.mapToItem(null, 0, 0).y` + `root.Window.height`) and writes `roomBelow`/`roomAbove`;
the y/height bindings read them and choose: down when a usable list fits, otherwise up,
otherwise the larger side. Two rules fall out:

- **A binding over `mapToItem` is evaluated once and never again.** It cannot notify, and at
  first evaluation the item may not even be in a window - which is how the popup came to
  open downward with a height clamped by a host edge it had measured as 0. Measure at open,
  in the handler, into plain properties; the handler runs before the frame is painted and
  the bindings pick the new values up.
- **`Window` attaches to an ITEM, not to a Popup** ("Window.window only supports types
  derived from Item", measured), so `root.Window.height` - not `Window.height` inside the
  popup block.
- **`root.rowsHeight` on a property declared on the POPUP is `undefined`, and
  `Math.max(undefined, x)` is NaN.** The NaN then made `opensUp` false (`NaN < x` is false)
  and `height` `Math.max(120, NaN)` -> NaN, which Qt resolved to a ~40px strip: the popup
  drew its search field and nothing else while the ROWS were laid out at their correct
  rectangles, so the accessibility tree said "showing Blur" and the screen showed an empty
  strip. That is the shape of every "the popup is broken" report in this area - assert on
  the DRAWN pixels, not on the nodes.

The wanted height is also computed without the delegates now
(`FilterStackModel::catalogRowHeight` is the model's number, which the delegate reads too):
`catalogColumn.implicitHeight` is 0 on the first evaluation, because the Repeater creates
its items while the popup is being opened, after the binding has run.

## A typo must not empty a list: `FuzzyMatch`'s rule 7 (2026-10-26)

The filter catalog's search went empty on `blurr`. That is not a size problem (the catalog
has four rows) - every rule in `gui/qml/FuzzyMatch.h` needs the token to be INSIDE a word,
and a token with one letter too many cannot be a subsequence of anything. So the matcher
gained **rule 7**: bounded Damerau-Levenshtein (adjacent transposition = one edit) against
each word and each of its prefixes within `[L-limit, L+limit]`, with `limit` 1 below seven
characters and 2 above, scored at `120 + 8L - 60*distance` so it sits BELOW rule 6 and a
correction can never outrank a real match. Below four characters it does not run at all,
which is why `hsb` still matches nothing while `blurr` finds `Blur`.

`FilterStackModel::catalogScore(index, query)` is the one door the catalogue uses
(`fuzzy::queryScore(name + 0x1F + description, query)`), and the picker's visibility, its
highlight walk and its Return all read it - a row cannot be visible under one rule and
unchoosable under another. Gates: `tests/fuzzy_match.cpp` pins both halves (including
`bzzz`-style negatives and the 1-edit-under-7 / 2-edit-from-7 thresholds), and
`tests/inspector_view_model.cpp` pins the scores the catalog actually hands the picker.

## The thirds fast path: what the predicate IS, and how its branch is gated (2026-10-26)

Two statements about it were wrong in the first write-up and are corrected here, because
both invite a wrong change:

**The predicate is arithmetic, not mode-based.**
`type == Custom && x_out(from key) == float(1/3) && x_in(to key) == float(2/3) && 0 <= x <= 1`.
A mode only decides WHICH x is written: `Vector`, `Auto` and `AutoClamped` write the thirds
verbatim (so they always take the fast path and NEVER run the full solve), a derived handle
frozen back to Free/Aligned keeps those exact floats (and also takes it), a dragged side
stores an arbitrary x (and does not), and the crossed default `(2/3, 1/3)` does not. So
"every segment whose facing handles are both derived" is one PRODUCER of the pair, not the
rule - a Free pair storing the thirds qualifies too.

**The reason for exact equality is not a visible jump.** A tiny epsilon would be harmless
(y moves by ~epsilon*slope); the real reason is that `y(x) = B_y(x)` is the true curve ONLY
when `x(t) = t`, which holds at the thirds and nowhere else, so an epsilon admits pairs where
the fast path returns the wrong point of the curve while looking like a legitimate
reduction. Measured on a pair 1e-6 off the thirds: cubic `0.345757604` against solve
`0.345757395`.

**And the branch itself is now a gate, because a tolerance cannot prove it.** The two
branches agree to their own rounding, but their floats are not always identical: for a
thirds pair 261 of 999 swept samples differ (by one ULP). `tests/easing_regression.cpp`
therefore asserts BITWISE equality against the branch that must have run - the thirds pair
equals the direct cubic in 999/999 samples, a pair 1e-6 off the thirds equals the solver in
999/999 (952 of which could have told the difference), with an assertion that the sweep CAN
discriminate before either claim is made. The reference cubic in that test computes
`y2 - y1` in DOUBLE on purpose: in float it is `0.6f`, in double `0.6000000089406967`, and
that one rounding moved 118 of 999 samples.

The measured win, through the shipping `util::Easing::calculate`: **34.6 ns against
233.1 ns** per evaluation (6.7x, ~199 ns saved), `|fast - solver| <= 4.3e-8`.

## Layer masks: a channel whose payload is a field, and five traps it cost (2026-10-26)

A node's mask is `core::MaskKey` - a `TimeKeyType` whose payload is an 8-bit alpha field in the
node's content space - so it inherits the lane, the keys, the easing, the proportional machinery,
the key menu, the marquee and value-replayable undo from every other channel. It animates the way
FFD does: key mask SHAPES and blend the fields. Gates: `layer_mask_pixels` (the draw, on a real
context: ten assertions for the layer shader's LAYER_MASK variation plus three for the folder's
composite pass), `layer_mask_commands` (add/invert/key/remove, one undo each),
`timekey_roundtrip` (the field survives format 0.20), `inspector_mask_card` (the card's four
states and every button's wiring).

**A NEUTRAL MATTE MUST COST NOTHING.** Measured: adding a folder mask flipped the folder onto the
isolated-composite path and shifted 2098 px of the picture before a stroke. A composite present is
not byte-identical to a direct subtree render, so "the matte is a no-op" has to be enforced, not
assumed: `MaskKey::Data::isPainted()` rides the value (set by a brush write and by an invert,
computed once on import) and both consumers test it, the way `FilterActivity` tests a blended
value. After it, adding a mask changes 0 px on a layer AND a folder.

**`mPassShaders[6]` was hand-counted.** Adding the folder pass wrote one past the end and the first
frame segfaulted inside `QOpenGLShaderProgram::init()` - which reads like a driver fault, not like
an array bounds bug. It is `mPassShaders[Kind_TERM]` now.

**A tool panel filled BEFORE the tool switch never reaches the editor.** `ToolWidget::onParamUpdated`
forwards only while `mToolType == ToolType_Mask`, so `beginMaskPaint`/`beginLayerMaskPaint` set the
panel first, push the tool, and then call `onParamUpdated(false)` again. Without that last call the
brand-new `MaskEditor` keeps MaskParam's defaults: measured as a stroke that painted NOTHING, with
no error anywhere (the editor was still writing the Wave filter's mask, which the layer did not
have).

**`AeButton` has a 72px implicitWidth FLOOR.** Three of them in a row demanded 150px inside a ~220px
inspector dock, so the whole mask card overflowed the panel and clipped its own primary button -
invisible in the QML, obvious in a screenshot. The card's secondary actions are 22px
`AeIconButton`s. And do not trust a crop to prove a layout fits: my first "proof" cut the button off
itself.

**Nothing sizes the window under Xvfb, so `_lib.sh` does it.** With no WM and a fresh
`XDG_CONFIG_HOME` there is no remembered geometry either: the application mapped at 795x337 on a
1600x1000 screen, which makes every screenshot look broken and every pixel computed from the screen
miss the window. `ae_size_window` (called by `wait_ready`, override `AE_WINDOW_SIZE`/`AE_NO_RESIZE`)
sizes the toplevel found by GEOMETRY through `xwininfo -root -tree` - `xdotool search --name` finds
the selection owner.

**A LIFO walk hands back siblings bottom-first.** `tests/filter_card_geometry` identified its two
cards through a `visualTree()` that pushes children onto a stack, so with two cards `first` was the
SECOND one and the gate failed on a correct layout ("expanding the second moved the first"). Sort
markers by their Y in the panel, which is what the helper's own comment always meant.

### A MASK OVERLAY IS NOT A TINT, and a matte is not shown in red (2026-10-26, from the first review)

Three things in the mask's first cut read wrong to the person using it, and all three were a
meaning I invented rather than the one the reader already knows:

- **The rubylith was a TINT.** `kTintFrag` adds colour where a field deviates from its neutral,
  which is right for a filter's VALUE field - but on a node's VISIBILITY field it only lands where
  the composite has colour: a stroke across transparent pixels drew no red, and a stroke across
  linework dyed the lines themselves ("why are some of the lines red?"). The overlay is now its own
  pass (`FilterFrame::maskOverlayTexture`, `Kind_MaskOverlay`): a FLAT premultiplied src-over at
  coverage = hidden x alpha, over the content and over nothing. Measured on the same stroke, the
  frame's change went 14713 px (tint) -> 36672 px (overlay), i.e. the overlay marks the whole hidden
  REGION. The tint stays for filter masks; only the matte uses the overlay.
- **The thumbnail and the tree chip were red-on-grey.** They showed only what is HIDDEN, in the
  rubylith ink, which invented a meaning and made an untouched matte look like a filled control.
  Both are now the MATTE ITSELF in grayscale - white visible, black hidden - which is how an alpha
  channel reads anywhere.
- **The key dot was `theme.text`.** A channel's dot carries its TYPE's hue (`theme::keyTypeColor`),
  and a white dot read as "no state" beside every other channel. `TimeKeyType_Mask` had no row in
  `kKeyColorRow` at all, so the timeline's Mask lane was also neutral: it has `maskKey` (violet,
  row 12) now, and both views ink the matte with it - adjacent-hue reuse is already this app's
  practice (Move and HSV share indigo; Scale and Blur share fuchsia).

Also from that pass: the toolbox panel's title follows its TARGET ("Layer mask" / "Filter mask"),
and the target chooser's fourth slot needed its own glyph - a `SingleOutItem` with fewer icons than
choices draws a bare rectangle, which reads as a disabled button.

LESSON for the next screenshot argument: a CROP is not evidence that a layout fits, and a fragment
of a panel cannot be reviewed at all. Capture the window, and crop to a whole surface (the whole
inspector column, the whole tree width) - the chip was outside two of my crops in a row.

### The mask overlay's two real defects, and the measurement traps that hid them (2026-10-26)

Asked "is the masking overlay drawn wrong?", the honest answer took four probes and found TWO
defects - and three false leads created by my own measuring.

THE DEFECTS:

- **A matte preview never drew on a plain layer.** `LayerNode::isPreviewTarget` tested
  `maskPreview != nullptr` only, so a matte preview (which carries `maskPreviewKey`) left the layer
  on the DIRECT draw path - and the overlay lives in `renderComposite`. Nothing appeared at all. It
  accepts either pointer now, which also routes such a layer through the composite path where the
  overlay has a surface.
- **The paint path inverted a different matrix than the shader.** `MaskEditor::toContent` inverted
  the node's SRT and then subtracted the image offset; the renderer inverts
  `ndcToPixels * camera.viewMatrix() * (SRT + offset)`. Measured on a layer, the SAME press came out
  89 content pixels apart (paint y 224.5, render y 313.5). Both call `core::contentFromScreen` now,
  and `tests/mask_mapping.cpp` asserts the invariant: content -> screen -> content is the identity
  (worst 0.0001 px) for a rotated, scaled, offset node - plus that the painter and the preview
  recover the same point.

THE FALSE LEADS, each worth remembering:

- **`vDestCoord` is y-UP and `CameraInfo::toScreenPos` is y-DOWN.** Comparing them directly shows a
  delta of exactly `compositeHeight - y`, which reads like a 161px offset bug. Flip one before
  comparing: `screenUpDown = (x, H - y)`.
- **A short diagonal stroke is a sausage whose CENTRE is behind the cursor.** Reading "the red blob
  is 70px from the ring" off a screenshot measured the sausage's middle, not the stroke's end. Paint
  a long axis-aligned stroke (or a single dab and measure its centroid) before calling it an offset.
- **A red wash at 45% over pale artwork is indistinguishable from the artwork.** A rubylith over a
  character's blush looked like the blush, so "no overlay" and "overlay" looked identical. The
  preview alpha is 153/255 now - a mask overlay has to be unmistakable while you paint it.

And one that is not a defect: a FOLDER's mask covers ITS OWN content box, which can sit anywhere on
the canvas (measured: `Folder16`'s origin at world 285,264). A stroke pressed where the *picture* is
lands outside that folder's box and paints nothing - correctly. Select the node you are painting, or
fill the viewport and check the boundary.

## The object tree's layer thumbnails (2026-10-26)

A leaf layer's icon IS its own pixels: `gui::ThumbnailCache` (one file, cached) replaces the
file glyph in the existing icon slot, so there is no new column, no new width, and the hidden
row's existing fade dims a thumbnail exactly as it dimmed a glyph. Recognition only - the
SOURCE image, never the rendered result - and a FOLDER keeps its folder glyph.

Why not the rendered result, and why not folders, both measured or bounded:

- A rendered thumbnail is a GL readback per row and would go BLANK for a masked-out layer,
  which is the opposite of a navigator's job.
- A folder has no source pixels: its content IS a composite, so a folder thumbnail can only be
  a render through the real renderer or a second CPU compositor that drifts from it (blend
  modes, filters, mattes, clipping). Costed anyway: compositing cached 18px child thumbnails
  is 0.010 ms for 5 children and 0.038 ms for 40 - cheap, and still the wrong picture.

COST, measured on this machine (`QImage::scaled` from a real layer buffer):

| source | path | time |
|---|---|---|
| 606x530 | smooth to 18px | 0.408 ms |
| 16.9 Mpx (the biggest layer in the corpus) | smooth directly | **75.7 ms** |
| 16.9 Mpx | 1/8 fast, then smooth | **2.33 ms** |
| cache hit | hash lookup | ~0 |

So `kSubsamplePixels` (1 Mpx) halves a big source first - free at 18px and the difference
between a hitch and not. Real projects here are 33-172 leaf layers, 2-104 Mpx total, i.e. 3-18 ms
to thumbnail a whole typical project and ~110 ms for the heaviest, ONCE, lazily and cached -
which is the other reason the tree must never scale inside `paint()`.

TWO DEFECTS the screenshots caught, both invisible in the code:

- **A fully transparent layer drew as NO ICON AT ALL** - the row lost its glyph and the column's
  rhythm with it. A cell is always returned: an alpha CHECKER under the art (the standard
  presentation), a 1px frame, and the art fitted inside keeping its aspect.
- **The fade guard was the only icon key.** `kIconFadeRole` compares the fade, so a REIMPORT -
  which hands the layer a different buffer while the fade and the selection stay identical -
  never repainted the icon. `kIconThumbRole` carries `ThumbnailCache::key(buffer, size)` now, so
  a live-link reload updates the thumbnail.

THE CHECKER AND THE FRAME, as finally decided (the review asked for both):

- **4 LOGICAL-px squares**, not 2. At 2px the pattern read as dithering, not as a checker
  (reported from the running app).
- **Theme tokens at 69/43 (dark) and 226/196 (light)**: `Colors::checkerLight`/`checkerDark`.
  Two corrections landed here, and the ORDER matters: at 2px the pair read as dithering, so it
  went to 4px AND brightened (90/55) - which was then reported as too bright on a dark panel.
  The fix is that READABILITY and BRIGHTNESS are independent: the spread stays ~26 levels
  (what makes 4px squares read) while both tiles sit near the row's own value (69 on 43 against
  a #262626 row, which is the same band Photoshop's dark layer panel uses, ~74/56). The light
  theme's pair sits below its pale row the same way (226/196 against #f5f5f5).
- **A 1px `hairline` frame**, restored on purpose: a checker only proves a cell exists when
  the layer is TRANSPARENT, and a dark drawing on a dark row needs its boundary (it is also
  what Photoshop draws). It is drawn on the cell's outer pixel, which costs 1px of art.

Gate: `tests/layer_thumbnails.cpp` - a transparent layer still gets a cell with a two-tone
checker; an opaque
source's own colour lands at the centre; a wide source keeps its aspect and leaves checker above;
the same buffer returns the same pixmap and a new buffer a different one; a 6.8 Mpx source takes
the subsample path and still lands its colour. Suite: 67 pass / 0 fail. Evidence:
`docs/design/tree/layer-thumbnails.png`.
