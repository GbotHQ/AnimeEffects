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

Five traps, all of which have cost real time here:

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

## Wayland

Xvfb is X11 and cannot stand in for a compositor when the behaviour *is* the platform:
window placement, popups and tooltips, IME, focus. For those, drive the real session with
**`ydotool`** (installed), remembering that the window must be activated first.

Two capture notes:

- `import -window root` (ImageMagick) does **not** contain the pointer — no X11 screen grab does,
  which is why `ffmpeg` grew `-draw_mouse`. When a line is about the *cursor shape*, capture with
  `ffmpeg -f x11grab -draw_mouse 1 -frames:v 1` (this repo has not exercised that end to end yet).
- A Wayland-native window is invisible to X11 capture; a black frame usually means "you captured
  the wrong compositor", not "the app did not draw".

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
