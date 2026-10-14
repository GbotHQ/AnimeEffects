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

## The animation graph overlay (Dear ImGui)

The panel is a **fullscreen overlay drawn by Dear ImGui 1.92.7**, pinned by a
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
`src/gui/GraphPanelModel.h` are the two durable specs):
- fullscreen over the animation, **no background and no grid**: short tick marks
  and labels only, so the artwork stays readable through the panel;
- text carries a **halo**, which is what keeps it legible over arbitrary pixels;
- **hit-only ownership**: the graph takes the pointer only on a curve, a key or a
  handle (or while a drag is in flight), so the viewport tools keep working while
  the panel covers everything. The WHEEL is the exception, and deliberately so:
  while the graph is visible it zooms the graph; hiding the graph gives the wheel
  back to the canvas;
- a **sticky focus** marks one channel (a press that hits a curve sets it, a press
  on empty space leaves it alone). The value axis follows the focus, and the fit
  it implies is **pending**: it is applied when the gesture is over, never
  mid-press - otherwise the key or handle the user just grabbed slides out from
  under the pointer, and a drag would start from geometry the fit invalidated;
- double-click on the plot fits both axes (`GraphEditor::fitAll`); the graph's
  frame axis is its OWN `ctrl::time::Scaler` (by value in `graph::Scaler`), so
  fitting it moves the graph's window and not the timeline's.

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
every band it opens the marquee. Claiming plain presses there instead stops every
tool gesture while the panel is up; that is a constraint, not a preference. Alt is
not available as the modifier either: the compositor grabs Alt+drag to move windows,
and the application binds `Alt+E`, `Alt+Q`, `Alt+1`, `Alt+3`.

*The wheel zooms BOTH axes about the cursor* (Shift narrows it to the frame axis,
Ctrl to the value axis), and its DIRECTION matches the canvas and the timeline - a
positive Qt angle delta zooms OUT, at 1.1 per notch. Making the axes agree meant
flipping the value axis, whose old sense was opposite the frame axis beside it and
opposite the canvas below it; two axes that zoom in opposite directions cannot be
made into one gesture that keeps its shape.

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
frame numbers and any value label that would fall under it. It moves NOT the plot, the
curves, the keys or the hit-test geometry: those are the editor's own rect
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

**The key menu is the application's own `QMenu`.** `gui::TimeLineEditorWidget::showKeyContextMenu(targets, globalPos)`
- the same `mSelectEasing` menu the timeline's right-click builds - reached from the panel as
`ImGuiOverlay::openKeyMenuAt` -> `MainDisplayWidget::showGraphKeyMenu`. The ImGui popup, its
`ImGuiConfigFlags_NavEnableKeyboard` toggle, `SetNextFrameWantCaptureKeyboard` and the Qt focus take/hand-back
are DELETED (grep returns 0). An ImGui popup can never look like a `QMenu`, and its keyboard capture was also
what broke typing into the dock after the menu had been used ("phase 4b"): that defect is gone with it. Two
consequences worth knowing: a Qt menu is MODAL (the app's Delete/Undo cannot reach the timeline while it is
open), and it is a top-level window, so its pixels are NOT in the application window's screenshot -
`probe_graph_overlay.sh`'s old menu phase scanned for them and had to be re-derived.

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

Anything drawn in the viewport overlay pass — the tool gizmos, and now the animation graph panel —
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
real paint path. That is `scripts/gui/probe_graph_overlay.sh` (auto-discovered by `run_probes.sh`):
it selects a layer, presses the graph toggle **by name**, and asserts with pixel diffs that the
change lands inside the central widget (bounds derived from the `page tab` and `Properties` AT-SPI
rectangles) and that toggling off restores the pixels exactly. Measured there: 18505 px changed, of
which only 568 are the toggle's own highlight, and off-vs-off is 0 px. Two traps for anything like
it: the View Settings buttons are icon-only, so every AT-SPI lookup returned the GROUP BOX's name
until the toggle got its own `accessibleName`; and **normalise both screenshots to truecolor sRGB
before `magick compare`** - comparing a grayscale capture against an sRGB one reports 0 differing
pixels for images that differ by 1200, because the first image's colourspace seeds the metric.


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


