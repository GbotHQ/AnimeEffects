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
