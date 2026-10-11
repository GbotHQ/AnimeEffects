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
- **`en` is not a translated language.** It is absent from
  `qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES es ja ko ru zh)` (root `CMakeLists.txt:29`),
  so `translation_en.ts` is the source-language stub. lupdate prints `Updating …/translation_en.ts`
  and `Found 893 source text(s) (893 new and 0 already existing)` and then leaves the 4-line,
  0-entry file **byte-identical**. Do not "fix" it, and never read its `unfinished` count as debt.
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

## Where the existing GUI tooling lives

Extend these instead of writing new one-off scripts:

- `scripts/gui/_lib.sh` — `ae_press`, `ae_activate`, `ae_report`, `assert_showing`/`assert_hidden`,
  the Xvfb/`QT_QPA_PLATFORM` setup, the settings sandbox.
- `scripts/gui/ae_ui.py` — the AT-SPI client: `click`, `set`, `value`, `text`, `report`, `wait`,
  `extents`, `dump`, addressing controls **by name**.
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

