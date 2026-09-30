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

## Where the existing GUI tooling lives

Extend these instead of writing new one-off scripts:

- `scripts/gui/_lib.sh` — `ae_press`, `ae_activate`, `ae_report`, `assert_showing`/`assert_hidden`,
  the Xvfb/`QT_QPA_PLATFORM` setup, the settings sandbox.
- `scripts/gui/ae_ui.py` — the AT-SPI client: `click`, `set`, `value`, `text`, `report`, `wait`,
  `extents`, `dump`, addressing controls **by name**.
- `scripts/gui/run_probes.sh` and `scripts/gui/probe_*.sh` — the end-to-end checks; run one probe
  directly while iterating, the runner for the whole suite.
- `tests/` — headless unit tests (`QT_QPA_PLATFORM=offscreen`), including `export_form` and
  `destination_picker` for the export dialogs, and `picker_keyboard` for the key routing inside them.
