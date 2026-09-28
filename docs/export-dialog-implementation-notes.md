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

# Control inventory: which rung each element lands on

The order asked for was: Controls used directly first, then delegates, then templates, then
hand-rolled. This is that walk over the exporter's controls, done in that order, with each element
stopped at the first rung that meets the design. "Rung" is not a quality ranking — R3 is correct
where no Controls type fits — it is the order a type gets tried in.

- **R0 — Controls directly.** Instantiate a Controls type and set declared properties only (`text`,
  `model`, `palette.*`, `padding`, `delay`, `orientation`, `elide`). No `background`,
  `contentItem`, `indicator`, `handle`, `popup` or `delegate` assignment; the look is the style's.
- **R1 — delegates over a Controls type.** The same type with the same C++ behaviour; the
  style-owned delegates are replaced with the design's.
- **R2 — templates.** `import QtQuick.Templates as T` and build the control from the template,
  owning the whole assembly — the route a custom style takes. For when the part that must change has
  no delegate slot, or the style's own assembly is wrong. One type looks like this case and is not:
  `TextField` has **no `contentItem` at all** (measured), because it *is* its own editor, so it is
  R1 with its text behaviour already intact rather than R2.
- **R3 — hand-rolled.** QtQuick primitives only, which is what the exporter is today.

**R2 has no members in this inventory**, and that is a result rather than a gap: every part the
design needs to change turns out to be a delegate the style already publishes. The rung stays
listed because it is the honest answer when a delegate *is* missing, and because `TextField` shows
the shape of the near-miss — not a missing delegate, but no delegate slot at all, since the type is
its own editor.

## Three premises, measured rather than asserted

1. **Controls works in this host.** A spike scene hosted in a `QQuickWidget` — `Button`,
   `TextField`, `CheckBox`, `ComboBox`, `ScrollBar`, `SearchField`, `ToolTip`, `SpinBox`,
   `ScrollView`, `ItemDelegate`, `Label`, `TreeView` — loads and renders, `palette.highlight` is
   writable, and the delegate hooks are populated by the Basic style where they exist. Which hooks
   *exist* is not uniform, and one absence changes a rung (below). Measured property by property,
   `exists=0` meaning the property does not exist at all as opposed to holding null:

   | type | `background` | `contentItem` | `indicator` | `popup` | `delegate` | other |
   | --- | --- | --- | --- | --- | --- | --- |
   | `Button` | yes | yes | — | — | — | |
   | `TextField` | yes | **no such property** | — | — | — | `placeholderTextColor` exists |
   | `SpinBox` | yes | yes | — | — | — | |
   | `ComboBox` | yes | yes | yes | yes | yes | |
   | `ScrollBar` | yes | yes | — | — | — | **no `handle`**; `contentItem` *is* the handle |
   | `CheckBox` | — | yes | yes | — | — | |
   | `ScrollView` | — | yes | — | — | — | |

   (Source kept at `.git/dsh-scratch/controls-spike` for re-measurement.)
2. **The default style here is not Basic.** With no style requested, `QQuickStyle::name()` is
   `Fusion`. Basic — the documented base for customisation — must be selected explicitly, before the
   first scene is created.
3. **The design's look does not fit in a palette.** The `theme` context property carries 25 colour
   tokens (26 properties; `closeIcon` is a URL). `QQuickPalette` exposes 20 roles ×
   active/inactive/disabled: `base`, `brightText`, `button`, `buttonText`, `dark`, `highlight`,
   `highlightedText`, `light`, `link`, `linkVisited`, `mid`, `midlight`, `shadow`, `text`,
   `toolTipBase`, `toolTipText`, `window`, `windowText`, `placeholderText`, `accent`. Exactly six
   of the 25 have a true role: `base`, `text`, `textDisabled` (the disabled group), `accent`,
   `selection` (`highlight`), `selectionText` (`highlightedText`). The surfaces the design is built
   from — `raised`, `raisedHover`, `recessed`,
   `active`, `activeHover`, `hairline`, `outline`, `focus`, `hover`, `textMuted`, `textMutedDim`,
   `accentHover`, `accentBright`, `accentText`, `icon`, `danger`, `warning`, `floaterBody`,
   `floaterEdge` — have none.

**So R0 is nearly empty by construction, and for a different reason than the hand-rolled files
give.** The chrome is replaceable — that is what `background`/`contentItem` are for. The palette is
the part that is genuinely too small: three quarters of this design's colours have nowhere to live
in it, so every control that paints a surface needs at least one delegate no matter how good
Controls is. "Use Controls directly where we can" therefore has a real, short answer rather than a
disappointing one.

## The inventory

| Element (today) | Rung | Type, and what is overridden | Lines met, and what the rung buys |
| --- | --- | --- | --- |
| Picker search field (`AeTextField`, 150 l, hand-rolled `TextInput`) | **R1** | `TextField` — `background`, `padding`, `placeholderTextColor`; **no `contentItem` exists to override**, because `T.TextField` is not a Control but the editor itself (measured). The leading glyph and the clear button are therefore composition beside the field, shifted in by `leftPadding`/`rightPadding` | B12 I-beam and B13 selection/`selectByMouse` come from the type; `placeholderText`/`accepted`/context menu follow. Ours either way: focus-on-open (B6), `navigate()` forwarding (B14), click-outside (B11) |
| Buttons (`AeButton` 106 l, `AeIconButton` 81 l) | **R1** | `Button` / `ToolButton` — `background`, `contentItem` | A1 focus ring, A2 Return/Space, `down`/`hovered`/`pressed`, `Accessible.Button` and the press action Qt already exposes for that role, `checkable`/`checked` for the aspect toggle. The design's look is those two delegates |
| Checkbox (`AeCheckBox` 88 l) | **R1** | `CheckBox` — `indicator`, label colour via `palette.text` | one click/Space toggle action, `checked`, focus, the accessible toggle action |
| Combo (`AeComboBox` 254 l) | **R1** | `ComboBox` — `background`, `contentItem`, `indicator`, `popup`, `delegate` | G39 type-ahead, G37 Up/Down highlight and Space/Escape/Enter, highlight-follows-keyboard, and the positioner that flips and clamps (G38). **The in-scene popup is not abandoned to meet G38** — `popup` is our delegate either way; what stops being ours is the list, its highlight and its keys |
| Scrollbar (today a decorative hand-drawn Rectangle) | **R1** | `ScrollBar` attached to the list — `contentItem` (which *is* the handle; there is no `handle` property, measured) and `background` (the track) | D23 press/drag → `setPosition`, the visible/total ratio, wheel and paging, `policy: AsNeeded`; D24 is the two delegates |
| Tooltip (`AeToolTip` 43 l) | **R1** | `ToolTip` — `background`, `contentItem`; `delay`/`timeout` are properties | E28 (a real popup, not clipped by the dialog), E27 placement and flip, E30 pass-through, one shared instance. **The stock `delay` is 0** (`qquicktooltip.cpp:111-130`), so E26 is met by the value the design sets — true on any rung |
| Numeric field (`ScrubSpinBox` 205 l, `AeNumberField` 121 l) | **R1 + R3** | `SpinBox` for range/step/decimals/typing — `contentItem` (a published delegate, measured) and `background`, with `up`/`down.indicator` hidden; the **scrub drag stays R3**, because no Qt control has horizontal-drag-to-scrub | range/step/validation and `Accessible.SpinBox` come for free; F31/F33 (cursor, selection tokens) are a delegate/property either way, and the C++ `QAccessibleValueInterface` factory stays because Qt exposes no value protocol to a QML spin box on *any* base. **[open]** Whether the replacement `contentItem` must remain a text editor (the C++ edits through it) is not measured — the private headers are not installed and the source has moved out of the cgit tree; it changes R1 to R2 in name only, not the work |
| Disclosure (`AeDisclosure` 63 l) | **R1, optional** | a checkable `Button` with a `contentItem` | focus and keys; no Controls type is a disclosure, so R3 as today is equally defensible |
| Section header (`AeSectionHeader` 46 l) | **R3** | `Label` + hairline `Rectangle` | nothing; no type is a ruled heading |
| Fact row (`AeFactRow` 67 l) | **R3** | two `Label`s | nothing; its `Accessible.role: StaticText` is required on any rung |
| Field label (`AeFieldLabel` 32 l) | **R0** | `Label` | **nothing** — every property it sets is set anyway, and `Layout.*`/`Accessible.*` are not Controls features. This is the honest shape of R0 here: `Label` is `Text` plus a palette binding we do not use |
| Frame range row (`AeFrameRangeRow` 76 l) | **R3** | composition of two numeric fields + a trash button | composition is not a rung decision; its parts have theirs |
| Destination list (`DestinationPicker.qml`) | **not a Controls question** | `ListView` (Qt Quick core) over the flat row model | the model's rows are non-contiguous once filtered, so `ListView.section` cannot own the groups; `TreeView` resolves in this Qt (measured) but would move expansion and filtering into Qt's model contract and out of the headless tests. C18 indentation, C20 keyboard selection and C21 hover paint are ours on every rung |
| Form scroll container (`ExportDialog` parameters) | **R1** | `ScrollView` with the same ScrollBar delegate, or a core `Flickable` | D25's wheel/drag/keys; Q9 is a layout question, not a toolkit one |
| Dialog shell (`QDialog` + `QQuickWidget`) | out of this table | — | the host question belongs to E28's Wayland measurement |

## What this means for the files we have

- Seven files state a constraint as fact that is now measured false — `AeTextField.qml:7-9`,
  `ScrubSpinBox.qml:4-6`, `AeComboBox.qml:7-12`, and copies at `AeCheckBox.qml:8-11`,
  `AeSectionHeader.qml:15-16`, `DestinationPicker.qml:3-6`, `AeToolTip.qml:7-11`. "QC2 brings its own
  chrome and its own palette" is half true (the palette is too small, the chrome is a delegate);
  "a QQuickWidget cannot render a popup outside its own bounds" is untested and wrong for Qt 6.8+
  with `Popup.Window`. Whichever rungs we take, those comments must stop giving untested limits as
  the reason.
- The accessibility layer is orthogonal and survives every rung: `QmlFormAccessibility`'s factory,
  the `aeAccessible*` nodes and the value interface exist because Qt exposes no value protocol for
  QML spin boxes — not because these controls were hand-rolled.
- One type decision is worth taking before the search field is converted: `SearchField` (Controls
  6.10, measured present) is the B-cluster's exact shape — magnifier indicator, clear indicator,
  `delegate`/`highlightedIndex` — but it is a `ComboBox` subclass, so it announces ComboBox and
  carries a suggestion popup the picker has no use for. `TextField` keeps the accessible role the
  picker's probes already address. Recorded as a decision, not left implicit.
