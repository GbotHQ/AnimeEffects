# Implementation notes per contract line — search/list/scroll/tooltip cluster

Companion to `docs/export-dialog-behavior.md`. That file is the design; this one records what each
line actually costs, from the Qt 6.11 sources (file and line) and from measurement where reading
could not settle it. Verdicts are marked **[read]**, **[driven]** or **[open]**.

## Method — and the two instruments I first wrote off, both wrongly

- **Read:** the Qt 6.11 sources of the Basic style and the templates, cited inline.
- **Driven — keyboard, in-process:** key events injected into the QQuickWidget, which is how Qt's
  own autotests drive a scene. Isolates QML routing from the platform input path:

  ```cpp
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
  QCoreApplication::sendEvent(quickWidget, &press);   // reaches the scene's focus item
  ```

- **Driven — keyboard, at the OS level: it works.** My "not one key ever reached the application"
  claim was a scripting bug, not a limitation. Under Xvfb **with a window manager** the window does
  take focus — measured `focusWindow=yes active=1 activeFocusItem=QQuickTextInput` — and `xdotool`
  keys arrive (`sceneKeys=KEY 65 / KEY 66`). What actually broke it: `xdotool search --name "^fx$"`
  returned nothing, so focus went to the wrong window, and my click used guessed coordinates that
  fell outside the window, which *removed* focus. The recipe that works:

  ```bash
  xvfb-run -n 53 --server-args="-screen 0 1024x768x24" bash -c '
    openbox & sleep 2
    ./app & sleep 3
    W=$(xdotool getactivewindow)                     # or: search --class <name>
    eval $(xdotool getwindowgeometry --shell $W)     # X, Y, WIDTH, HEIGHT
    xdotool windowactivate --sync $W
    xdotool mousemove $((X + WIDTH/2)) $((Y + HEIGHT/2)) click 1
    xdotool key Down'
  ```

  Coordinate clicks need none of this, which is why the existing probes get away with `ae_press` and
  never noticed the gap. `ydotool` stays the tool for a real Wayland compositor, where Xvfb cannot
  stand in. Keyboard lines can therefore be verified with **real key events**, not only injection.

- **Qt and QML log output: not swallowed, redirected.** Same program, stderr redirected to a file:

  | environment | marks reaching stderr |
  | --- | --- |
  | none | 0 |
  | `QT_FORCE_STDERR_LOGGING=1` | 1 — `console.info` arrives, `console.log` still filtered |
  | `QT_LOGGING_RULES=*.debug=true` alone | 0 |

  and the journal holds what stderr lost (`journalctl --user`, `fx[…] INFO_MARK scene ready`). Two
  separate documented mechanisms: Qt's default handler writes to **journald** when stderr is not a
  console (`QT_FORCE_STDERR_LOGGING`; formerly `QT_LOGGING_TO_CONSOLE` /
  `QT_ASSUME_STDERR_HAS_CONSOLE`), and `console.log` maps to `qCDebug` in category `qml`, which the
  default rules filter (`*.debug=true` or `qml.debug=true`). **Consequence for this project: the
  probes launch the app with its output redirected to `/tmp/probe_app.log`, so every Qt warning and
  QML error has been going to the journal instead of the log — `scripts/gui/_lib.sh` now exports
  `QT_FORCE_STDERR_LOGGING=1`.**

## Measured: where a key pressed in a focused TextField actually goes

Scene: a `TextField` with focus, a sibling `ListView` with its own `Keys.onDownPressed`, and a
handler on the root `Item`. Injected `Down Down Up Right`:

```
FIELD DOWN (not accepted)      <- the focused control's own handler
ROOT DOWN -> index=1           <- then its ANCESTOR, because the field did not accept
FIELD DOWN (not accepted)
ROOT DOWN -> index=2
ROOT UP -> index=1
ROOT saw key=16777236 accepted=false
caret=1 text='a'               <- text insertion; the arrow never moved the caret
```

- The **sibling list's handler never fired once**. Qt Quick key propagation follows the **item
  hierarchy, not the visual list**: an unhandled key travels up the parent chain, never sideways.
- Therefore contract **B7** ("Down/Up move the LIST's highlighted row while the field has focus")
  cannot be met by putting a `Keys` handler on the list, however the list is implemented. It needs a
  handler on a common ancestor, or explicit forwarding from the field. **[driven]**
- Consequence for **B9** and **C16** (arrows must not expand/collapse while searching): the arrows
  *do* arrive; the behaviour that consumes them is our own key handling. Toolkit-neutral.

## Per line

| Line | Mechanism | Evidence | Verdict |
| --- | --- | --- | --- |
| **B12, F31** pointer is an I-beam over text | Stock `TextField` sets it itself, including the read-only nuance | `QQuickTextField` calls `setCursor(isReadOnly && !selectByMouse ? Qt::ArrowCursor : Qt::IBeamCursor)` (`qquicktextfield.cpp:262`) and `setCursor(Qt::IBeamCursor)` (`:364`) | **stock does it** [read]; our `ScrubSpinBox` sets `SizeHorCursor` for the whole field, which is the defect |
| **B13** selection tokens | Basic binds them to the palette | `selectionColor: control.palette.highlight`, `selectedTextColor: control.palette.highlightedText` (`Basic/TextField.qml`) | **stock, via palette or two overrides** [read] |
| **D23** scrollbar drags the content | Template implements press/move → `setPosition` | `qquickscrollbar.cpp:304-330` | **stock does it** [read]; the picker's bar is decorative |
| **D24** scrollbar look | Basic's bar is a `Rectangle` content item | `Basic/ScrollBar.qml` | **ours either way** — the design's tokens are a delegate, not a stock look |
| **C19** grouped list with headers | `ListView.section.property` / `section.delegate` | Qt Quick core, documented | **free** [read] |
| **G39** combo type-ahead | Template implements it | `QQuickComboBoxPrivate::keySearch` (`qquickcombobox.cpp:287,743`) | **stock has it** [read]; whether the design wants it is Q6 |
| **G37** combo Up/Down | Template moves the highlight | `setHighlightedIndex(highlightedIndex ± 1, …)` (`qquickcombobox.cpp:700,715`) | **stock does it** [read] |
| **E26** tooltip delay, never instant | `int delay = 0`, and the delay timer only starts `if (delay > 0)` | `qquicktooltip.cpp:111-130` | **the stock default is instant** — the line requires setting the delay explicitly, in any implementation [read] |
| **E27** tooltip placement | Basic positions it above its parent with `margins: 6`; the shared instance's position is the framework's | `Basic/ToolTip.qml`; `qquickpopuppositioner.cpp` | **mechanism exists; the design's placement must still be specified and driven** [open] |
| **E28, G38** never clipped by the dialog | `Popup.Window` creates a real popup window | Measured earlier: X11 correct geometry; **Wayland: created but never mapped from a QQuickWidget**, while a real `QQuickView` host mapped correctly | **open, and it decides the host** [driven] |
| **C21** no wrong hover frame | State binding in our own delegate | — | **ours either way**; needs a driven reproduction first [open] |
| **C18, C20** indentation, keyboard selection | Our model and delegate | — | **ours either way** [read] |
| **B6, B10, B11** search behaviour | Our field's code | — | **ours either way** [read] |

## What this settles, and what it does not

### The principle, since this document keeps being asked the question

**Use the abstraction we already have unless a contract line says otherwise.** Stock Qt Quick
Controls is the default; hand-rolling one is justified per line, in writing, by the line it fails —
not by a preference for owning the pixels, and not by a constraint that is asserted instead of
measured. "Which toolkit" is a real question with a real answer, and the answer is a mix: five lines
of this cluster are met outright by stock code (see the table), the rest are the same work either
way, and where stock is close but not exact the delegate/template override is still less code than
a control from scratch. My own `AeToolTip.qml` is the counter-example to keep in mind: its comment
states, as fact, a limitation I never tested ("a QQuickWidget cannot render a popup outside its own
bounds") and that limitation is not even true for Qt 6.8+ with `Popup.Window`.

### Settled

My earlier claim that a Controls-based implementation "is not less work" was wrong. Stock covers the
I-beam cursor, the scrollbar drag, combo type-ahead and keyboard highlight, list sections, and
palette-driven selection colours — five lines for free.

The lines the review actually hit — routing arrows from a focused field into a list, tooltip delay
and placement policy, expansion, indentation, hover paint, keyboard selection semantics — are
**neutral**: our code in either implementation, and no toolkit choice removes them. Measured, the
defect cluster sits in behaviour we wrote and never tested.

### Not settled (do not claim these)

1. Whether popups and tooltips can be made to work from the *existing QQuickWidget host* on Wayland.
   Needs a real compositor and real input (`ydotool`), not Xvfb. This is the one line that decides
   the host question.
2. Whether the popup positioner keeps an in-scene tooltip inside the window bounds. The source
   suggests it clamps; nobody has watched it.
3. The picker's specific section + expansion + focus interaction, in the design's own terms.
