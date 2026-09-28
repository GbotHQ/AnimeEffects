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

### What "delegate" means here, because it is two different things

- **A style component.** A Controls type is a C++ object owning the state and the behaviour, plus
  QML objects that draw that state. The documentation calls those objects *delegates*:
  `background` (the surface), `contentItem` (what the control shows or edits), `indicator` (the mark
  — checkbox tick, combo caret), `handle` (a draggable part; `ScrollBar` has none, its
  `contentItem` *is* the handle), `popup` (a detached surface — the combo's list, the tooltip's
  bubble). The C++ object instantiates them, sizes itself from them, and pushes state into them
  (`down`, `hovered`, `pressed`, `visualFocus`, `palette`), which is why replacing them is the
  intended hook rather than restyling from outside.
- **A model-row renderer.** A control that has a model also has a `delegate` that renders **one
  row**: `ListView.delegate`, `ComboBox.delegate`. Same word, unrelated job — it renders data, not
  the control's own state.

`ComboBox` has both, which is the whole confusion: its `background`/`contentItem`/`indicator`/
`popup` are style components, and its `delegate` is the row renderer. So the `background:
Rectangle {}` in a `Button` **is** a delegate, and that snippet is what R1 means: keep the C++
behaviour (focus, press, Return/Space, the accessible action) and replace the components that paint
it.

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

**R2 has no members as a per-element choice, but it does have a shape: a custom style.** The Qt
Quick Controls documentation's "Creating a Custom Style" is R2 exactly — a directory of
`QtQuick.Templates`-rooted QML files plus a `qmldir`, found on the QML import path and selected by
name through `QQuickStyle::setStyle()` (or `-style`), with `import QtQuick.Controls.Basic auto` in
the `qmldir` as the fallback for controls the style does not implement. With a style, every use site
becomes R0 — `Button { text: qsTr("Export") }` — and the design lives in one directory instead of
being repeated in N wrapper files.

That matters because our `AeButton`/`AeCheckBox`/… files are the documentation's **second** way
("move the code into its own QML file within your project"), not a different idea: converting them
to R1 keeps the same file structure and changes only what is inside each file. Which of the two
routes to take — a wrapper per control, or one custom style — is a deliberate architectural choice
and the only one on this page that is not per-element. It is the difference between the design being
restated at each control and being defined once.

One element cannot be done per-site at all, and the documentation says so: **the attached
`ToolTip`**. "The three approaches mentioned here do not work for customizing the attached ToolTip,
as that is a shared item created internally … To customize the attached ToolTip, it must be provided
as part of your own style." So the tooltip's E-cluster is satisfied either by instantiating
`ToolTip { }` objects with our `background`/`contentItem` and our `delay`, or by a style-provided
ToolTip. That is a real constraint on E26–E30, not a preference.

(The install also carries `Qt/labs/StyleKit` — a declarative styling layer over the templates, and a
*Technology Preview* in Qt 6.11. It is another way to define the design once; not recommended here
because of the preview status.)

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

   The documentation says the same thing in different words — each control "consists of" a number
   of *visual items*: `Button` two (background, contentItem), **`TextField` one (background)**,
   `CheckBox` three (adds indicator), `ComboBox` five (adds popup, indicator, delegate), `ScrollBar`
   two, `SpinBox` four (adds up/down indicator). That agrees with the table, and `TextField`'s
   single item is the documented form of the no-`contentItem` finding.
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
| Tooltip (`AeToolTip` 43 l) | **R1, but not the attached form** | an instantiated `ToolTip` — `background`, `contentItem`; `delay`/`timeout` are properties | E28 (a real popup, not clipped by the dialog), E27 placement and flip, E30 pass-through, one shared instance. **The stock `delay` is 0** (`qquicktooltip.cpp:111-130`), so E26 is met by the value the design sets — true on any rung. The *attached* `ToolTip` is excluded from per-site customisation by the documentation, so this is an instance or a style, not `ToolTip.text:` on the owner |
| Numeric field (`ScrubSpinBox` 205 l, `AeNumberField` 121 l) | **R3, with an R1 editor inside it if we want one** | Three responsibilities, and only two of them are a delegate anywhere. **Value semantics** — `from`/`to`/`value`/`stepSize`/`editable`/`live`/`validator`/`textFromValue`/`valueFromText` — are `SpinBox`'s (measured from the type's property list). **The editor** is `SpinBox.contentItem` (a `TextInput` with `readOnly: !editable`, measured in `Basic/SpinBox.qml`). **The scrub shell** — display-until-clicked, horizontal drag, shift-fine, click-to-edit, Escape-revert — is a delegate in *no* toolkit, because it is behaviour rather than a component | the scrub is ours on any base. F31/F33 (cursor, selection tokens) are a one-line fix each in the existing control. **[open, and the reason this row is not a conversion]** `SpinBox` assumes an editor is always present (`readOnly: !editable`), while the design hides the caret until the field is clicked — so a SpinBox base still needs the mode switch written by hand *and* has to stop the always-present editor from taking the click and focus the scrub shell needs |
| Disclosure (`AeDisclosure` 63 l) | **R1** | a checkable `Button` (`checked` = open) with a `contentItem` (caret + label) and a `background` (hover) | today it hand-writes a `MouseArea`, `Keys.onSpacePressed`/`onReturnPressed` and its own `Accessible.role`/`onPressAction`; `Button` already has all of that (`text`, `down`, `pressed`, `checked`, `checkable`, `indicator`, `activeFocusOnTab`). There is no Controls type that *is* a disclosure, so what remains ours is the caret and the words |
| Section header (`AeSectionHeader` 46 l) | **R3** | `Label` + hairline `Rectangle` | nothing; no type is a ruled heading, and `T.Label` is `Text` plus a palette binding we do not use |
| Fact row (`AeFactRow` 67 l) | **R3** | two `Label`s | nothing; not interactive, so no Template has behaviour to contribute. Its `Accessible.role: StaticText` is required on any rung |
| Field label (`AeFieldLabel` 32 l) | **R0** | `Label` | **nothing** — every property it sets is set anyway, and `Layout.*`/`Accessible.*` are not Controls features. This is the honest shape of R0 here: `Label` is `Text` plus a palette binding we do not use |
| Frame range row (`AeFrameRangeRow` 76 l) | **R3** | composition of two numeric fields + a trash button | composition is not a rung decision; its parts have theirs |
| Destination list **rows** (`DestinationPicker.qml`'s row `Rectangle` + `Text` + `MouseArea`) | **R1** | `ItemDelegate` — `background`, `contentItem` | this row was missing from the first draft and belongs here. `ItemDelegate` publishes `highlighted`, `down`, `pressed`, `checked`/`checkable`, `text`, `background`, `contentItem` and handles focus and the accessible action; C21 (hover paint) and C20 (the highlighted row the keyboard drives) are then one state binding each, not a MouseArea we reimplement. The picker's *list* stays core `ListView`; only the rows change |
| Destination list (`DestinationPicker.qml`) | **not a Controls question** | `ListView` (Qt Quick core) over the flat row model | the model's rows are non-contiguous once filtered, so `ListView.section` cannot own the groups; `TreeView` resolves in this Qt (measured) but would move expansion and filtering into Qt's model contract and out of the headless tests. C18 indentation and the model-backed selection stay ours on every rung |
| Form scroll container (`ExportDialog` parameters) | **R1** | `ScrollView` with the same ScrollBar delegate, or a core `Flickable` | D25's wheel/drag/keys; Q9 is a layout question, not a toolkit one |
| Dialog shell (`QDialog` + `QQuickWidget`) | out of this table | — | the host question belongs to E28's Wayland measurement |

## What this means for the files we have

- Two elements were under-called in the first draft and are corrected above: the **disclosure**,
  which hand-writes focus, Return/Space and its own accessible action that `Button` already
  provides; and the picker's **rows**, where `ItemDelegate` publishes the `highlighted` state that
  C20 and C21 are about.
- The one place an R1 conversion is plausibly **negative** is the numeric field. `SpinBox` would
  replace roughly seven lines of clamping and parsing with a dependency, and its always-present
  editor contradicts the design's display-until-clicked mode; F31 and F33 are a one-line fix each
  in the control we already have. Converting it is the conversion that has to be justified, not the
  one to assume.

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
