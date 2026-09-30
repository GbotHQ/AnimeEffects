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

- Seven files stated a constraint as fact that is now measured false — `AeTextField.qml:7-9` (DONE,
  converted to `TextField`; see below), `ScrubSpinBox.qml:4-6`, `AeComboBox.qml:7-12`, and copies at
  `AeCheckBox.qml:8-11`, `AeSectionHeader.qml:15-16`, `AeToolTip.qml:7-11`. "QC2 brings its own
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

## Increment 2, landed: the search field is a `TextField`

`AeTextField.qml` is now Qt Quick Controls' `TextField` with the design's own `background`. Same file
name and same addressing, so no call site moved. The header comment that claimed Controls "brings
its own chrome and its own palette, and cannot be customised per site" is gone; the chrome *is* a
delegate and the design's background is a drop-in for it.

Measured on this installation (the Basic style's `TextField.qml`, read from
`/usr/lib64/qt6/qml/QtQuick/Controls/Basic/`):

- `T.TextField` **inherits `TextInput`** — it *is* the editor, which is why the earlier probe found
  no `contentItem`: there is nothing between the control and its text.
- The style supplies the placeholder itself, as a child: `PlaceholderText` positioned from
  `control.leftPadding` / `rightPadding` / `verticalAlignment`, coloured by `placeholderTextColor`,
  and hidden while there is `length` or `preeditText`. So the hand-written placeholder `Text` is
  gone and the leading glyph and the clear button are paid for with `leftPadding` / `rightPadding`
  rather than with anchors.
- Basic's defaults are `padding: 6`, `leftPadding: padding + 4`, `verticalAlignment: AlignVCenter`.
- It installs `ContextMenu.menu: TextEditingContextMenu` — right-click cut/copy/paste/undo, which the
  hand-rolled field did not have.
- **`hoverEnabled` defaults to `false`**, so a background that reacts to `hovered` must set it. The
  hand-rolled version got hover from its own `MouseArea`.
- Two more measured API facts the conversion depends on: `textEdited()` and `accepted()` take **no
  arguments**, so this wrapper must not declare `signal accepted(...)` (duplicate name with the
  inherited one — Return already reaches the caller unchanged) and the clear button, whose clear is
  programmatic and therefore emits nothing, is the only reason the custom `edited(string)` survives.

What the conversion actually bought, in the terms the contract uses: caret motion, keyboard
selection, drag-select, word jumps, the clipboard shortcuts and undo, input methods, the horizontal
scrolling that keeps a long line's caret in view, and the I-beam cursor. That last one is B12, one of
the reported defects: the full-bleed `MouseArea` that focused the hand-rolled field also painted an
arrow over its text.

**Behaviour decided here, not merely preserved:** B9. The field no longer forwards Left/Right at all
— `navigationKeys` is now the VERTICAL keys only — so an arrow pressed while typing can no longer
collapse or expand the highlighted platform. Left/Right stay with the caret. B14 was rewritten to
say this, and Q4 was answered (focus selects nothing) and moved out of the open list.

Verified by `tests/picker_keyboard.cpp`: the real `DestinationPicker.qml`, built offscreen, with the
keys injected into the item, asserting on the model — 32 checks, B6/B7/B8/B9/B13/B15. Run it against
the old forwarding and exactly three B9 checks fail and it exits 1, so the test is known to be able
to fail. B11 (click outside removes focus) and B12 (the cursor shape) are deliberately absent: both
need a real window, which this test does not have.

## Verification by driving the UI, one interaction at a time

The alternative to a probe script is just *looking*: one application on one Xvfb display, driven with
`xdotool`, captured with `import`, inspected between steps. About ten minutes, no encoded navigation,
and it covers everything a person sees. This is how the search-field conversion was verified after
`picker_keyboard`:

| line | observation |
| --- | --- |
| B6 | typed `telegram` → list filters to the four Telegram rows, clear button appears |
| B7 | two × `Down` → highlight moved row 1 → row 3; text still `telegram`, caret still at the end |
| B9 | two × `Left` → **group still open**, all four rows present, text and highlight untouched |
| B9, caret | then typed `X` → **`telegrXam`**, i.e. `Left` really moved the caret to position 6 instead of being swallowed |
| B12 | `ffmpeg -draw_mouse 1`, cropped and scaled 400 % → **I-beam** over the text (first end-to-end use of that path in this repo) |
| C17 | platforms open collapsed |
| C18 | childless platforms (Bilibili, Dailymotion, Douyin, LINE sticker) align with the section icons rather than being indented |

One defect **found**, that nothing had asserted:

- **E26 — the tooltip has no wake-up delay.** Contract line 26 requires "the platform's wake-up
  delay — never instantly". Measured from a 30 fps recording while the pointer entered a row: the row
  highlight and the tooltip changed on the **same frame** (51 → 52), so the delay is ≤33 ms —
  effectively instant. A single screenshot cannot show this, because ffmpeg's own ~300 ms startup
  makes any quick tooltip look like it had a delay: the instrument has to compare the first-change
  frame of the hover highlight against the first-change frame of the tooltip.

The contract already prescribes the instrument for this line ("timestamped hover frames, 0 ms →
wake-up delay → timeout"), and that is the whole argument in miniature: a probe has to *encode* that
measurement to have it, an agent driving the UI takes it while looking, and neither one gives you a
regression guard for free — that part stays a test.

## E26, landed: the tooltip waits, and it was not a matter of picking a number

`AeToolTip` now arms on `shown` and reveals after **500 ms**, retires itself after **10 s**, and
cancels immediately if the pointer leaves. Two named constants at the top of the file.

The interesting part was proving there was nothing to read. Contract line 26 says "the platform's
wake-up delay", which sounds like a lookup:

- `QStyleHints::toolTipWakeUpDelay` is **Qt 6.12+**, and a grep of the whole installed 6.11.2 header
  tree matches **no** `showDelay`, `toolTipDelay` or `hideDelay` at all.
- Qt Quick Controls' `ToolTip.delay` **defaults to 0** ("a tooltip with a negative delay is shown
  immediately", and `timeout` defaults to -1 = never), and **no installed style sets either** — so
  switching to Controls would have reproduced the defect.
- Qt's *own* `QToolTip`, measured with a self-timing Widgets spike (records `QEvent::Enter`, polls
  for the tooltip window, prints the delta):

  ```
  ENTER    at 2846 ms
  TOOLTIP  at 2848 ms -> DELTA 2 ms
  ```

So following the platform on Qt 6.11.2 **is** the defect: Qt's tooltip is instant here. The delay is
therefore the design's own constant (500 ms / 10 s, chosen as the conventional desktop values), and
the file says so, with a note that `Qt.styleHints.toolTipWakeUpDelay` should replace the first one
if the project moves to 6.12.

**The hole worth recording:** the picker sets `shown: hoveredRow >= 0`, which stays `true` while the
pointer scans from row to row — only `text` changes. Driving it that way showed the label jumping to
the next row instantly, i.e. the same E26 defect through a different door. `onTextChanged` now
re-arms, which also restarts the hide timeout.

Verified by the instrument the contract prescribes (timestamped hover frames, 30 fps):

| E26 requirement | measurement |
| --- | --- |
| appears after the delay, never instantly | cursor at frame 021, tooltip at **036** → 15 frames = **500 ms** |
| hides when the pointer leaves | drop at **081**, the frame the pointer left |
| hides after the timeout | frame 036 → **336** = 300 frames = **10.0 s** |
| scanning rows re-delays | row B: vanish at **096**, reappear at **112** (+16 frames ≈ 500 ms) |

Before the fix the same measurement put the hover highlight and the tooltip on the **same frame**.

## R1, landed: `AeButton` is a `Button`

Same file name, same call sites (they use only `text`, `kind`, `iconName`, `enabled`, `onClicked`).
Chrome comes from delegates, so the design keeps its three fills and its rule that a focus ring is
the only border ever drawn; Qt supplies hover, pressed, `focusPolicy`, `checked`, the button role,
and `click()`.

Measured before converting — three things had to be checked rather than assumed:

| question | answer | consequence |
| --- | --- | --- |
| does Qt fire `clicked` on Space? | **yes, on release** (press-only fires nothing) | the hand-written `Keys.onSpacePressed` would have **fired it twice** — dropped |
| Return / Enter? | **no** — Qt's Button has no default-button concept in QML | contract line A2 still needs `Keys.onReturnPressed` — **kept** |
| cursor after dropping the `MouseArea`? | `HoverHandler.cursorShape` exists | the pointing-hand cursor survives without a MouseArea that would double-handle the click |

Also measured: `visualFocus`, `hovered`, `pressed`, `down`, `focusPolicy`, `checked`, `palette` and
`click()` all exist, `clicked()` takes **no** argument (calling it with one prints "Too many
arguments, ignoring 1"), and Basic's padding is symmetric (8/8) so centred content stays centred.
`icon.source` is a *group* and reads invalid through `QObject::property`, which is why the content
stays ours rather than moving to Qt's `IconLabel` — Qt colourises images, and this theme hands out
pre-tinted SVGs.

One behaviour change worth naming: the focus ring now binds `visualFocus` (Qt's, true for keyboard
focus) instead of `activeFocus`, which also lit the ring after an ordinary mouse click. Contract line
A1 asks for a focus ring on the focused control, and a ring left behind by the pointer is not one.

Verified by driving it: rest state has no border, hover gives the lighter fill **and** the hand
cursor (captured with `-draw_mouse`), a Tab puts the ring on Cancel, and Return with that focus
closes the picker (`Use this destination`: showing → absent). Suites 16/16 in Debug; Release built
zero-warning and also 16/16.

## R1, landed: `AeCheckBox` is a `CheckBox`

Same file name, same call sites (`text`, `checked`, `enabled`, `onToggled` — all four are inherited,
none redeclared). The box, the tick and the words are the `indicator` and `contentItem` delegates;
Qt supplies the toggle, Space, `focusPolicy`, hover, `checked`/`toggled`, the checkbox role and the
press action. The old header's reason for hand-rolling — *"a QC2 indicator draws its own chrome"* —
was the same non-reason as the button's: that chrome is the delegate being replaced.

Measured before converting (`.git/dsh-scratch/key-spike`, offscreen):

| question | answer | consequence |
| --- | --- | --- |
| does `toggled()` exist? | **yes** (AbstractButton's), and it fires **only on activation** — `click()`, Space, a press on the row. Writing `checked` from a binding or directly does **not** fire it | exactly what `onToggled: model.value = checked` needs: no write-back when the model pushes a value |
| Space? | toggles **on release**, with no `Keys` filter involved | the hand-written `Keys.onSpacePressed` would have toggled **twice** (our press, Qt's release) — dropped |
| Return / Enter? | **neither consumed nor acted on** (`accepted=0`, `checked` unchanged) | the dialog's own Enter handling still receives them, and no handler is needed — the old control had none either |
| does Qt place our delegates? | in **x/y** our anchors win; but it writes their **width and height** from its padding: the label came out 556×12 where the hand-rolled `Text` was 17 tall | centring a box Qt had just shrunk moved every word **3 px down** (1205 differing pixels). Fixed by anchoring the label's **top** to its own line box: `topMargin = floor((height - implicitHeight)/2)` = 3, where they were |
| can the label anchor to the box? | no: `anchors.left: ind.right` throws `ReferenceError: ind is not defined` at creation | the offset comes from `boxSize`, not from the other delegate's width |
| `hoverEnabled` default | **false** | hover has to be asked for |
| `focusPolicy` default | **0xB**, which *is* `Qt.StrongFocus` — the enum is Tab \| Click \| 0x8, not 3 | Tab and Click, matching the widget `QCheckBox`; a click that left the previous control focused is how Space after ticking a box types into a text field |

**The focus ring is contract line A1**, which the hand-rolled control had no provision for at all.
Its shape is not invented: `standard.ssa` already specifies it for the widget checkbox —
`QCheckBox::indicator:focus { border: 0.04em solid @focus@ }` — so the box's hairline turns to
`theme.focus`, focus ahead of hover exactly as the stylesheet's cascade has them. `activeFocus`
rather than `visualFocus` (the button's choice) because the stylesheet says `:focus` and not a
keyboard-only variant, and because the widget `QCheckBox` is `StrongFocus`: clicking it focuses it
and lights the same border.

### Verification: offscreen first, then the real application

Two instruments, because a probe stack costs a machine that is being used
(`PROBE-SAFETY.md`: measure the compositor before and after, abort when it is hot):

1. **`geom` renders both versions offscreen** — old file from git, new from the working tree, four
   states each (rest, hover over the box, hover over the label, focus), ~2 s, no display:

   | old vs new | differing pixels |
   | --- | --- |
   | rest | **0** |
   | hover over the box / over the label | **0** |
   | focus | **72** — the ring: `focus vs rest` is 0 for the old control and 72 for the new |

   Each state prints an `achieved=` line, and it earned its keep twice: the first run's "rest" was
   silently a **hover** grab (the offscreen pointer sits at 0,0, on top of a control drawn there),
   and posting the hover event to the root never reached the hand-rolled control's `MouseArea`
   *child*, so the two sides were in different states and their difference meant nothing. Driving
   the hover through the **window**, the way a pointer does, fixed both.

2. **The real application**, three runs: the before run, the run with the fix, and a repeat of the
   same build:

   | comparison | differing pixels |
   | --- | --- |
   | the control's own box, any state | **0** in every pair |
   | geometry string | identical (`rect=436,619 568x24`) in all three |
   | hover lights | the same 72 pixels in both builds, at all three hover points |
   | focus ring | absent after 30 Tabs before; present on Tab #9 (72 px) after |
   | Space | `checked` yes → no → yes, read from the accessibility tree |
   | the form as a whole | **52** between two runs of the *same* build, 53 and 72 across builds |

   That last row is why the comparison now has two kinds of assertion. Qt's rasteriser does not
   reproduce glyph edges between processes: two runs of one binary differed in 52 scattered pixels
   **including buttons nobody touched**, and 36 of them were the same pixels in both run pairs —
   so a dialog-wide number is judged against a measured floor (2×), while the box, the geometry,
   the hover delta and the ring are asserted exactly. A real regression clears that floor by an
   order of magnitude: the 3 px label drift measured **1205** in the row alone.

Two counting traps, both measured: ImageMagick stamps a `date:timestamp` into every PNG it writes,
so **compare with `magick compare -metric AE`, never a file hash**; and a raw
`-compose difference` over un-flattened PNGs counts RGB differences *inside fully transparent
pixels* (745 against the true 72, while the alpha channels were identical) — flatten first, or use
`compare`.

## R1, landed: `AeDisclosure` is a `ToolButton`

Both call sites are unchanged — `text`, `open`, `onToggled`, `Layout.fillWidth` — and the header's
original argument survives, narrowed to one property instead of ruling out the toolkit: the widget
form used `QToolButton:checked`, and that paints the checked state with the QSS `@focus@` colour,
which would fill "Advanced" with the accent. So `checkable` stays **off**, `open` remains the
model's word, and Qt still supplies activation, Space, hover, the focus policy and the button role.

Measured before converting:

| question | answer | consequence |
| --- | --- | --- |
| does `toggled()` exist, and can we emit it? | **yes**, and `QMetaObject::invokeMethod(..., "toggled")` returns true | `onClicked: root.toggled()` keeps `onToggled` working at the call sites while the control is deliberately not checkable — Qt's own `toggled` never fires, so there is one signal and one owner of it |
| Space? | fires `clicked` **on release** (press only sets `down`) | `Keys.onSpacePressed` would have toggled twice — **dropped** |
| Return / Enter? | **neither consumed nor acted on** (`accepted=0`) | unlike the checkbox, this control has always handled Return, so the design's `Keys.onReturnPressed` is **kept** and cannot double-fire |
| `hoverEnabled` default | **false** | hover asked for explicitly |
| `focusPolicy` default | 0xB (StrongFocus) — but the **widget `QToolButton` is TabFocus** (measured) | stated as `Qt.TabFocus`, which is also what the hand-rolled control did: `activeFocusOnTab` plus a `MouseArea` that never took focus |
| `click()` | exists and fires `clicked` | `Accessible.onPressAction` goes through it, as on `AeButton` |

**The ring is contract line A1 again**: this header declares `activeFocusOnTab`, so it must show one,
and it drew none. `standard.ssa` has no `QToolButton:focus` rule to borrow — it has `QPushButton:focus
{ border-color: @focus@ }` — and Qt's own Basic `ToolButton` draws a border on `visualFocus`, so the
ring is AeButton's: 1 px of `theme.focus` around the row, the only border it ever draws.

Verification, offscreen (`geom`, both files in one engine, four states each):

| old vs new | differing pixels |
| --- | --- |
| rest | **0** |
| hover over the row / over its right end | **0** (both light the whole 568×30 row: 17036 px, identically) |
| focus | **1208** — a 1 px ring around 568×30, sampled at `#a78bfa` = `theme.focus` |

and within a version: `focus vs rest` is **0** for the hand-rolled control (the defect) and **1208**
for the new one. One instrument detail worth keeping: the ring binds `visualFocus`, so the grab must
give focus with `Qt::TabFocusReason` — focus given for any other reason photographs as "no ring" and
reads like a defect that is not there.

## R1, landed: the picker's rows are `ItemDelegate`s

The last `Rectangle` in the picker became Qt Quick Controls' `ItemDelegate`: the control
supplies activation (`clicked`), the pointer machinery and the focus policy; the row keeps how it
looks (its own `background`), what a click means (`onClicked` → toggle a section, select a
destination), and — unchanged — the keyboard, which the ListView's `activeFocusOnTab` plus
`Keys.onPressed` have always owned. The caret, badge and labels stayed anchored to the delegate
root, whose parent is still the control, so the style's padding cannot move them, and the explicit
`width`/`height` stay for the same reason: a Control sizes its delegates from padding and
`contentItem`, and the collapsed row's `height: 0` is load-bearing (it is how expansion and
filtering avoid renumbering rows).

Measured before converting:

| question | answer | consequence |
| --- | --- | --- |
| `hoverEnabled` default | **true** (unlike `Button`/`CheckBox`, which default false) | see the latch defect below — the property ends up **false** on purpose |
| `focusPolicy` default | 0x0 = **NoFocus** | a row is not a Tab stop; contract A1's tab sequence is untouched, nothing to state |
| Basic style's own `ItemDelegate` | `padding: 12`, a `background` that draws for down/highlighted/visualFocus, an `IconLabel` `contentItem` | our `background` replaces all of that — no pressed fill appears that this design does not have |
| does `clicked` coexist with a `TapHandler`? | yes: one tap gives `clicked=1 AND tapped=1`, a double tap adds `double=1` | `onClicked` owns select/toggle, the handler keeps **only** `onDoubleTapped`; wiring both would toggle a section twice. A double tap on a section still nets two toggles — two clicks plus a `doubleTapped` that ignores sections — exactly as before |

### The latch defect, found by the verification rather than by reading

The first conversion had the background read the control's `hovered`. The drag state of `geom`
(press a row, drag 100 px, release) then disagreed with the old build by **31544 px** over a
bounding box of exactly two rows. A per-event trace named the mechanism: the gesture delivers
**MouseMove to every crossed row and no HoverLeave at all** — not during the drag, not after it —
so `hovered` latched true on all three crossed rows and two of them (the third was the selected
row, whose colour the selection token overrides) kept `theme.raised` after the pointer had long
left. The same trace exposed a second defect: during that gesture the row's own `HoverHandler`
reported nothing (`listArea.hoveredRow` stayed −1 where the old Rectangle tracked 1→2→3), so the
tooltip and the pointing-hand cursor would have diverged too.

The fix is the old design, restated with the reason: `hoverEnabled: false` (its `hovered`
deliberately unread) and the colour reads `rowHover.hovered` again, the handler that also feeds
the cursor and the tooltip. One divergence remains and was accepted as-is: **while a button is
held**, the handler no longer follows the pointer across rows the way the old Rectangle did; the
first buttonless move after release puts it back exactly where the old code left it, all five
pixel states are equal, and in a normal click the freeze is invisible.

### Verification

| gate | result |
| --- | --- |
| builds | Debug **0 warnings**, Release **0 warnings** |
| units | `destination_picker` 82 checks PASS both configs; `picker_keyboard` **32 checks, 0 failures** both configs; full suites **16/16** Debug and Release |
| `geom` preconditions | **10/10 `achieved=yes`**, 0 `no` |
| old vs new, `magick compare -metric AE` | rest **0**, hover-over-box **0**, hover-over-label **0**, focus **0**, drag **0** |
| catalogs | six files, `grep -c unfinished` = **0** |
| end-to-end sweep | **13/13** probes, 0 stale, 0 portal lines (dialog canary first); `cosmic-comp`'s main thread 0–7% across 189 samples, never near the 35% abort |

Instrument notes worth keeping: `findChildren<QQuickItem*>` reaches **0** ListView delegates
(QObject parenting) — walk `childItems()`; a scratch binary left uncompilable by a previous edit
made the first run silently use a **stale binary**; and the old baseline only loads from the
directory that holds the component symlinks (`oldpicker/`), since the picker's `Ae*` siblings are
not on its own.

## R1, landed: `AeComboBox` is a `ComboBox`

The last risky R1 item. The root is now `Controls.ComboBox`; the call sites (Format, Codec,
Quality) did not change — `options`, `index`, `flat`, `overlay`, `value`, `text`, `activated` all
survive, and every user choice still reaches the model through `activated(int)` with `index`
written only by the call sites' bindings (the control mirrors `index → currentIndex` one-way and
never writes `index`, so no binding can be destroyed).

### The header's reason for existing was false — and irrelevant to the design

The old header claimed Controls' ComboBox cannot render its popup outside the hosting
QQuickWidget. Measured on this Qt (6.11, QQuickWidget host): the stock popup is
`popupType: Popup.Item`, parented to the overlay **inside the widget's own window** (window count
unchanged), clipped to the widget, with Qt's positioner doing below → flip-above → clamp — the
same dance `updatePopupGeometry()` hand-wrote. `Popup.Window` also works here (a fourth window
drawing over the title bar, screenshot captured). The design decides anyway: `export-dialog-
behavior` G37 — *"the popup is the design's in-scene list"* — so in-scene stays, now as a choice.

### Decisions taken during the conversion (all from the contract, none invented)

| question | verdict | basis |
| --- | --- | --- |
| type-ahead (Q6/G39) | **adopted**, ships with `textRole: "label"` | user decision 2026-09-29; without the role `keySearch` has no text (idx never moved) and the style logs `Unable to assign QVariantMap to QString` — the role fixes both |
| Enter, popup closed | stock **swallows**; observably identical to old | the old key propagated into a **void**: no QML Return handler (only `Ctrl+Return`/`Escape` shortcuts), no C++ key handler, no default `QPushButton` for `QDialog` — an earlier claim that it fired Export was retracted as inference from acceptance flags, never observed |
| Escape, popup closed | stock **propagates** → cancels | A2 `[D] Escape cancels`; the old control's unconditional `event.accepted = true` failed A2 whenever the list was closed — a defect the conversion fixes |
| Down/Up with list open | **choose** (emits `activated`, list stays) | design reference = current UI (which chose) and G37 lists Down among the choose actions; stock's highlight-only deviates |
| Enter with list open | stock: **commit + dismiss** | G37 says dismiss; old left the list open (its Enter would have reached Export with the list up) |
| Space | stock's **release** timing; with list open it also chooses | `AeDisclosure` precedent (release, not press); G37's literal "Space … choose and dismiss" |
| Tab with list open | **dismisses the list** | decided 2026-09-29, recorded in the contract (item 37): old's list-and-scrim-left-behind was an unowned accident, not a specified behaviour |
| click focus | `focusPolicy: Qt.TabFocus` — click opens, does **not** focus | old = Tab-only; stock's default StrongFocus would take focus on click (measured) |
| outside click while open | `popup { modal: true; dim: false }` | old's transparent scrim swallowed the press (measured 0→0); stock's default non-modal passed it through (0→1) — clicking Export under an open list would have fired it |
| pointer cursor | `HoverHandler { cursorShape: Qt.PointingHandCursor }` | old field's `MouseArea` carried it; every sibling control keeps it (AeButton precedent). **Pixel comparison cannot see a pointer** — the contract's own verification table names that trap — so it was caught by diff review, not by AE=0, and verified by `ffmpeg -draw_mouse 1` photographs of both controls on Xvfb |

### Five defects the gates caught, each measured then re-measured

1. **`flat` is Qt's own FINAL property** (`Q_PROPERTY … REVISION(2,1)`) — redeclaring it killed the
   file at load, while the app build's qmlcache compiled happily. Dropped ours; no call site ever
   set it; the background reads Qt's.
2. **The style's double-arrow drew anyway** — overriding `background`/`contentItem` does not remove
   `indicator`: 133 px in every closed-field state, a second caret beside ours. `indicator: null`
   (the style null-checks it) → 0.
3. **The popup opened AT the field** (y=0) — the positioner's baseline is a *style binding*
   (`y: control.height …` in Basic/ComboBox.qml). `y: root.height + 2` + 4 px margins restored
   old's geometry exactly: open box at 130 = 130, flip at 134 = 134, same positioner dance.
4. **Qt auto-scrolled the list to the current row on open** (contentY=48 vs old 0). No contract line
   asks for the scroll; the reference decides → `onOpened: list.contentY = 0`.
5. **`highlightedIndex` is read-only from QML** — the first Keys preempt threw mid-handler, so
   Down-with-list-open committed nothing (the closed case worked only because stock rescues it).
   Caught by the against-the-file key matrix. The shipped design: the filter only *marks* Down/Up
   (`_keyNav`), stock moves the highlight, and `onHighlightedIndexChanged` mirrors it to
   `activated` **once, only when a key put it there** — type-ahead (unmarked) stays highlight-only
   per the ruling, and the flag resets on the next key/open/close.

### Verification

| gate | result |
| --- | --- |
| builds | Debug **0 warnings**, Release **0 warnings** |
| units | suites **16/16** both configs (includes `export_form`, which covers the combos, and the real 45-assertion `media_exporter_e2e`) |
| `geom` preconditions | **12/12 `achieved=yes`** (rest, hover×2, focus, popup-open, popup-flipped-above) |
| old vs new, `magick compare -metric AE` | all six states **0** |
| key matrix, shipped file vs baseline (`run_keys_final.log`) | parity on Down/Up closed+open (exactly one `activated` per press, list stays), Space, Escape (as ruled), Enter (as ruled), type-ahead (decided behaviour: moves + emits closed; highlight-only open), Tab/Shift+Tab closed, outside-click swallow 0→0, wheel; one decided divergence (Tab + list open) recorded in the contract |
| cursor | both controls photographed showing the pointing hand (`out/cursor_final_pair.png`) |
| targeted probes (`format`, `quality`, `axes`, `dialog` — the combo drivers, via AT-SPI) | **4/4 PASS**, 446 s, no file dialogs opened (no canary needed) |
| catalogs | untouched, `grep -c unfinished` = 0 |

Instrument note: the cursor hunt shows why a synthetic pointer *teleport* (`QCursor::setPos` /
xdotool XTEST jumping across windows in one event) is not evidence about hover: only the
in-window approach exercised the entry event, and only there did the HoverHandler's cursor apply —
the old `MouseArea` path survived the jump by setting the cursor per-motion, which is why the old
control always looked right in the same test. Photograph the real path, or photograph nothing.

## R1, landed: `ScrubSpinBox` keeps its shell — F31 and F33 fixed, two headers measured true

The last R1 item was never a conversion (the inventory's numeric-field row decides the scrub shell
is behaviour no toolkit ships, ours on any base): two review defects inside the control, plus two
header comments whose stated reasons had stopped being true.

### F31 — the pointer follows the mode, and the defect was actively wrong, not stale

Measured on the real X pointer path (`ffmpeg -draw_mouse 1`, judged only at 600% nearest zoom) in
**three** edit states — stationary after the click (the acceptance frame), after a 2px move-and-back
jiggle (fresh hover re-evaluation), and after leaving to the background and re-entering over the
text. The baseline shows `Qt.SizeHorCursor` over the text in **all three**, so no amount of hover
re-evaluation was going to fix it. The fix has two owners, matching the two ways the cursor can
arrive: `cursorShape: editInput.activeFocus ? Qt.IBeamCursor : Qt.SizeHorCursor` on the scrub
`MouseArea` (the switch lands at the click itself — the property write applies at the focus flip,
before the area disables itself and without pointer motion), and `HoverHandler { cursorShape:
Qt.IBeamCursor }` on `editInput` (owns a fresh hover in edit mode). Scrub-hover keeps
`Qt.SizeHorCursor` (F32): rest-frame AE before vs after = **0**.

| state | baseline | after |
| --- | --- | --- |
| rest / scrub hover | SizeHor | SizeHor (F32, AE=0 before-vs-after) |
| edit, stationary after click | **SizeHor (the defect)** | **I-beam** |
| edit, after jiggle | **SizeHor** | **I-beam** |
| edit, leave + re-enter | **SizeHor** | **I-beam**, AE=0 vs the stationary frame |

### F33 — the selection is the theme pair, sampled not asserted

Live-object readings of the real `editInput` (objectName `scrubEdit`, both fields in one window):
baseline `#ff000080`/`#ffffffff` (Qt default palette), after `#666d28d9`/`#fff5f5f5` — **exactly**
`theme.selection`/`theme.selectionText` as `QmlTheme` prints them (the only pair QML can bind;
`Colors::textSelection` exists in the C++ struct but is not exposed, and the design reference
`AeTextField` binds `selection`/`selectionText`). Rendered proof: the highlight's clean 22x3 block
is `#52367D` = `theme.selection` (alpha 0x66) composited over the measured `theme.raised` `#404040`
— `magick compare -metric AE` against a synthetic blend = **0**; the baseline's navy sample fails
the same check **66/66 px**. Selected-glyph pixels are subpixel-AA blends at 11px (never full
coverage), so `selectionText` is honestly evidenced by the live-object reading plus the baseline
contrast, not by pixel sampling.

### The two headers, each rewritten only after measuring its claim

- `ScrubSpinBox.qml:3-6` claimed "a QC2 SpinBox cannot be restyled … nor given scrub-drag
  behavior". Unmeasured and false in the interesting half: the inventory row measured that a
  `SpinBox` **can** host the styling (delegate-based; its editor is `SpinBox.contentItem`, a
  `TextInput` with `readOnly: !editable`, from `Basic/SpinBox.qml`) but assumes an always-present
  editor, which contradicts the design's display-until-clicked mode and the scrub shell's
  click/focus needs. The header now says exactly that, with the citation.
- `AeSectionHeader.qml:15-16` claimed "NOT a Controls Label, for the same reason every control in
  this dialog is hand rolled". The premise died with `f5aa28d..f7ef22e` (AeButton/AeCheckBox/
  AeDisclosure/AeComboBox all import QtQuick.Controls — verified). A fresh offscreen probe settled
  the true reason: `Label` **is** a `Text` (`QQuickLabel → QQuickText`; Qt docs "Inherits: Text"),
  its only property delta over `Text` is `background` + the four inset slots (chrome this heading
  never fills), default colour and font are identical, and the one live difference is the palette
  binding (`palette.windowText` green moved `Label.color`, not `Text.color`) — which this design
  replaces with `theme.textMutedDim` anyway. And no Controls type is a ruled heading.

### Traps recorded

- **ImageMagick hex is `#RRGGBBAA`, Qt's is `#AARRGGBB`** — and ImageMagick's 16-bit internal depth
  inflates AE even between identical 8-bit colours. Compare at `-alpha off -depth 8 -colorspace
  sRGB -type TrueColor` (the AE=0 above is at matched depth).
- AE=0 still cannot see a pointer (the contract's own table): the cursor claims are photographs,
  per state, with the re-enter frame AE=0 against the acceptance frame as the stability check.

### Verification

| gate | result |
| --- | --- |
| builds | Debug **0 warnings**, Release **0 warnings** (logs in `scrub-spike/gates/`; artifacts rebuilt after the edit) |
| units | `scripts/run_suites.sh` **16/16 Debug, 16/16 Release**, exit 0 both (orchestrator re-run; includes `picker_keyboard` and `media_exporter_e2e`) |
| cursor | three edit states photographed I-beam after / SizeHor before; rest SizeHor both (AE=0) |
| selection | live-object tokens exact; rendered blend AE=0 after / 66-px fail before |
| field crops | harness symmetric at baseline (old-vs-new AE=0); fix delta in edit only (AE=640, 0.37%); rest unchanged (AE=0) |
| catalogs | untouched, `grep -c unfinished` = 0 |

Evidence: `.git/dsh-scratch/scrub-spike/` (`out/before` the baseline set, `out/after` the fixed set
incl. `_edit_hover`/`_edit_reenter`, `gates/` the gate logs; `run.sh` extended with the jiggle and
re-enter states).

## The host question, decided: keep the `QQuickWidget` (2026-09-30, measured on the live Wayland session)

The last open architecture question — host the export dialogs in the existing `QDialog` +
`QQuickWidget` shell, or in real QML windows — turned on exactly one unknown: whether in-scene
popups and tooltips can work from a `QQuickWidget` host on Wayland ("Not settled" item 1), and how
the positioner treats them at the window edges (item 2). The user authorized driving the live
Wayland session for it; a two-variant spike (`--host=widget|window`, the real `AeComboBox.qml` and
`AeToolTip.qml` via symlinks) answered it.

### Measured — placement is identical in both hosts, on both platforms

Zero-input selftest (programmatic `popup.open()` / `shown = true`) on Wayland-native windows, once
per host; the same sequence under Xvfb/xcb exercised the real pointer path. **Every geometry line
identical** across host × platform (raw logs `host-spike/out/selftest/*_app.log`):

| probe | logged geometry | verdict |
| --- | --- | --- |
| combo popup at the bottom edge (G38) | `x=20 y=108 w=200 h=220 win 500x400`, settled same — baseline below was y=366 | the positioner **flips above** and stays fully in-bounds — G38 satisfied from either host, Wayland-native included |
| `AeToolTip` at the right edge (E28) | `x=290 y=142 w=324 h=22` → spans 614 of 500 | **114 px overhang, clipped** — the real `AeToolTip.qml` has no flip and no clamp (`x: parent.width/2 - width/2`); identical in both hosts |
| stock `Controls.ToolTip` (E27 baseline) | `x=180 y=183 w=140 h=29`, `popupType=0 sameWindow=true` | an **in-scene Item popup**, placed **above** its owner even with room below — E27 wants below-first, so a delegate must place it, stock does not |
| `Popup.Window` (prior measurement, not re-run) | — | created but **never mapped from a `QQuickWidget`** on Wayland; maps from a real window host — the one behavior that differs between hosts |

### The ruling, and the decision it yields

The E28 wording was put to the user: "The design here allows for either. This is nuanced." — both
resolutions (overhang unclipped, or flip/shift inside) satisfy the line; clipping is the defect.
Consequences, in order:

1. No contract line requires a tooltip that overhangs the dialog ⇒ no line requires window-backed
   tooltips ⇒ **no line requires the real-QML-window host** (its only measured advantage on Wayland
   is mapping `Popup.Window`, which the design does not need).
2. The chosen resolution in this host is **flip/shift-inside**: it is plain positioning code ("ours
   either way") and works identically in both hosts — so the fix for the 114 px clip is the tooltip
   increment's clamp work, not a host migration.
3. **Verdict: keep `QQuickWidget`.** By the settled principle — use the abstraction we already have
   unless a contract line says otherwise — and against the measured evidence there is no line that
   says otherwise. **Reopen condition** (the only one): wanting the overhang look for tooltips
   would require window-backed tooltips, which on Wayland requires the real-window host.

### Verification honesty

- Placement/flip/clip: **Wayland-native, measured** (selftest logs above; screenshots via
  `cosmic-screenshot --interactive=false --notify=false`).
- Input semantics (real hover reveals the tooltip after the 500 ms delay, real click opens the
  combo): exercised under Xvfb/xcb only. Wayland-native input delivery through `ydotool` did not
  land (its absolute-move/acceleration caveat) and was NOT retried past the budget — a re-run with
  pointer acceleration disabled is available if Wayland input-path proof is ever wanted. The
  placement math itself is platform-independent Qt Quick code, and the measured zero divergence
  between platforms supports that.
- Disturbance (user-facing disclosure): three brief spike windows ≈45 s + two selftest windows
  ≈50 s, one ~5 s window from a mechanics check that leaked onto the XWayland desktop (should have
  used `xvfb-run`), and one driver bug whose garbage targets injected ~18 synthetic events near the
  screen's left edge. No typing beyond Escape, no file dialogs.

## Post-R1, landed: Escape belongs to the picker policy and the edit's first moment (B10, F34)

The first post-R1 increment, both lines decided by the user 2026-09-30. Tests are the evidence home
here (`tests/picker_keyboard.cpp` extended, `tests/scrub_escape.cpp` new — auto-discovered by
`run_suites.sh`, so the suite count is now 17): keys are observable headlessly, so every behavior
below is a **failing-on-regression assertion**, not a photograph.

### Measured first: three routing facts the design rests on

Scratch probes (`escape-spike/`): a key sent straight to an item reaches only **that** item's `Keys`
filters and editor and **never bubbles to ancestors** (direct send is a same-item probe), while a
key sent to a `QQuickWindow` under `offscreen` goes to the `activeFocusItem` and **does** bubble up
the parent chain (so window delivery is the honest end-to-end route, headlessly), and specific
`Keys.onXPressed` handlers auto-accept while generic `Keys.onPressed` does not. Hence B10's policy
sits on the scene **root** and the tests drive it **through a window** — the route is under test,
not just the policy.

### Before → after

| scenario | before | after |
| --- | --- | --- |
| picker Escape, text present | text kept; key leaks to the shell (which cancels — with text present)* | clears text + query, hidden rows return, **not** rejected |
| picker Escape, field empty | leaks to shell → closes | `model.rejected` (= the shell's `QDialog::reject` — the Cancel route) |
| same two steps with LIST focus | n/a (no policy existed) | identical clear-then-close |
| F34: type "12", owner re-syncs `value` 95→50 mid-edit, Escape | **keeps 50** (the property at Escape time) | restores **95**, owner re-synced, edit exits |
| F34: committed 30, second edit, Escape | already right (keep-committed == edit-start absent a re-sync) | restores 30 — not 95 ("dialog open") |
| F34: sentinel "Auto" (−1), type "30", re-sync to 7, Escape | **keeps 7** | restores **−1** and the placeholder word returns |
| F34: plain cancel / Enter commit | already right | unchanged |

\* The before column's shell close is the known `QDialog::reject`-on-unaccepted-Escape path plus the
measured "the scene leaves Escape unaccepted"; the model-level observable cannot see the shell's
close — recorded so nobody reads more into the row than was measured.

### The F34 rule the implementation embodies

`editStartValue` is snapshotted where the edit **begins** (both entry routes: `beginEdit()` and the
editor's focus-gain), and Escape restores *that* — "keep the committed value" was the old wording
and is only equal to it while nothing re-syncs `value` mid-edit (the dialog re-syncs on aspect locks
and range clamps; the a11y setter writes the same path — measured). The restore is a scrub-style
write: `changed()` fires only if the value actually moved, `committed()` never fires (it has **no
consumers** — verified before relying on that).

### Regression proof (deliberate breaks, run, reverted)

Removing the F34 restore failed exactly its 4 restore assertions (value/owner/sentinel/word);
removing the B10 clear failed 7 (both clear assertions and the close coupling — without the clear
the field is never empty so nothing closes, in both focus states). Clean reverts, 0 failures.

### Harness lesson this increment bought: the canary false-FAIL

`probe_inputs` opens an OS file dialog by design, so the portal canary ran first and said FAIL —
**wrongly**. The journal grep for `file_chooser` was matching the harness's own `[systemd-run]`
scope START lines, which log the command **text**: any command containing the word contaminates its
own canary window (measured: 4 hits in a clean window, all echoes — one was the check itself — 0
real portal lines; the only genuine portal line all day preceded this work). Fixed in
`canary_dialog.sh`, `sweep_runner.sh` (both checks) and the AGENTS.md recipe with
`grep -v '\[systemd-run\]'`, keeping the broad match so real leaks still trip it.

### Verification

| gate | result |
| --- | --- |
| builds | Debug **0 warnings**, Release **0 warnings** (affected units force-recompiled with kept logs) |
| units | `scripts/run_suites.sh` **17/17 Debug, 17/17 Release**, exit 0 (orchestrator re-run; `scrub_escape` in both) |
| regression proof | deliberate breaks caught by exactly 4 and 7 assertions |
| targeted probes | `dest`, `inputs`, `axes`, `quality` **4/4 PASS** (~8 min; `noproject` skipped — its Escape asserts the picker's absence, untouched here) |
| catalogs | 0 `unfinished` ×6; no strings changed |
