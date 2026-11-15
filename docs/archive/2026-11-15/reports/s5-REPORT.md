# Verification report — upstream 56b04b2 "Paste custom easing"

Scope note: the four subject files are byte-identical at `56b04b2` and at `upstream/master` (abb4b88):
`git diff --stat 56b04b2 upstream/master -- src/gui/TimeLineEditorWidget.cpp src/gui/TimeLineEditorWidget.h src/util/Easing.cpp src/util/Easing.h` → **empty output**.
So every `:line` below is valid for both revisions. All line numbers are from `56b04b2`.

Method: source reading via `git show`/`git grep` + two standalone probes compiled against the system Qt 6.11.1
(`.git/dsh-scratch/s5/probe/`): (1) a QString semantics probe, (2) a probe that links the **real upstream
`src/util/Easing.cpp`** (only `EasingName` stubbed, since it only needs `tr()`) and evaluates
`util::Easing::calculate(Param, ...)` exactly the way `TimeKeyBlender` calls it. No repo file was modified.

---

## 1. CLAIM (a) — `toFloat()` with no `ok` check → silent 0.0f

**CONFIRMED**, with a correction of consequence.

`src/gui/TimeLineEditorWidget.cpp:748-753`:
```cpp
    if (QStringList list = clip->text().split(","); !list.isEmpty() && list.size() == 4) {
        util::Easing::CubicBezier bezier{};
        bezier.x1 = list[0].toFloat();
        bezier.y1 = list[1].toFloat();
        bezier.x2 = list[2].toFloat();
        bezier.y2 = list[3].toFloat();
```
Probe output (Qt 6.11.1, `qstrtest`):
```
[letters] input="a,b,c,d" splitSize=4 isEmpty=0
    field[0]="a" toFloat=0 ok=0
```
Consequence correction: the resulting control set is `(0,0,0,0)`, and `cubicBezier` with x1=y1=x2=y2=0 makes
`x(t) = y(t) = t³`, i.e. **y = x — an identity/linear curve**. Probe (`easingprobe`, b=0 c=1 d=1 w=1):
```
a,b,c,d -> 0,0,0,0   x1=0 y1=0 x2=0 y2=0 : 0.0000 0.1268 0.2500 0.3750 0.5000 0.6250 0.7500 0.8750 1.0000
```
So garbage input is silently accepted, the key's type is still forced to `Type_Custom`, and the visible result is
"looks like Linear" — not an obviously broken 0-valued curve. The user gets no feedback at all.

Also note the missing `ok` check is **not the only hole**: `toFloat` *succeeds* on `"nan"`/`"inf"` (see §2), so an
`ok` check alone would not stop the worst case.

## 2. CLAIM (b) — no validation that x1,x2 ∈ [0,1]

**CONFIRMED** (as a missing check), but the *rationale* in the prior analysis is imprecise.

The paste path (`:746-765`) checks only the field count. The codebase itself documents x∈[0,1] as a real invariant —
`src/gui/prop/bezierCurveEditor.h:60-61`:
```cpp
    static constexpr qreal kMinX = 0.0; // x stays a function domain:
    static constexpr qreal kMaxX = 1.0; // cubic-bezier requires x ∈ [0, 1]
```
and the canvas enforces it on drag (`bezierCurveEditor.cpp:28-30 clampX`, applied at `:119-120`, `:133-137`, then
written back at `:139-144` "The editor is authoritative"). Two gaps bypass it: the spin boxes
(`splineWidget.h:138-139`: `spin->setMaximum(2); spin->setMinimum(-2);` for all four of x1,y1,x2,y2) and **both**
paste paths. So the invariant is real but unenforced exactly where arbitrary text enters.

Precision the prior analysis lacked: `x1 > x2` by itself is **harmless** here —
`"1,1,0,0"` gives `x(t) ≡ y(t)` → exactly linear (probe: `0.0000 0.1250 … 1.0000`). What actually breaks is a
**non-monotonic x(t)**, i.e. control points outside [0,1] (see §2 table).

## 3. CLAIM (c) — untranslated "Error"/"Invalid clipboard data", information icon for an error

**CONFIRMED** (with a "duplicated, not invented" nuance).

`src/gui/TimeLineEditorWidget.cpp:764`:
```cpp
    else { QMessageBox::information(this, "Error", "Invalid clipboard data"); }
```
Neither string is `tr()`-wrapped, while 100 lines above in the *same file* the sibling handler does it correctly
(`:648-649`: `QMessageBox::warning(nullptr, tr("Operation Error"), tr("Failed to paste keys. ..."))`). Qt doc
(QClipboard/`QMessageBox` page, MCP): "The QMessageBox class provides a modal dialog for informing the user or for
asking the user a question and receiving an answer." `QMessageBox::information(...)` is the Information-icon
convenience API, so an error is reported with an information icon.
Nuance: the identical untranslated pair already exists in the other paste implementation,
`src/gui/prop/splineWidget.h:224`: `QMessageBox::information(splineWidget, "Error", "Invalid clipboard data");` —
the new code copied the pattern, so there are now **two** sites to fix.

## 4. CLAIM (d) — `!list.isEmpty()` is redundant next to `list.size() == 4`

**CONFIRMED**, and for the stronger reason:
* `QStringList::isEmpty()` is exactly `size() == 0`.
* `QString::split()` never returns an empty list. Probe (Qt 6.11.1):
  `[empty] input="" splitSize=1 isEmpty=0`.
So `!list.isEmpty()` can never be false when `list.size() == 4`; the size check alone is the operative guard (and it
is what rejects the empty clipboard, because the empty string splits into one empty field).

## 5. CLAIM (e) — const-correctness disagreement between the two helpers

**PARTLY TRUE.**

```cpp
// TimeLineEditorWidget.cpp:654-655
static void setEasingTo(util::Easing::Param& aEasing, int aEasingType, const bool aAssignEasing,
                        const util::Easing::CubicBezier* aBezier) {
// TimeLineEditorWidget.cpp:662-663
static void assignEasing(const util::LinkPointer<core::Project>& mProject, int easingType,
                         const core::TimeLineEvent::Target* target, const core::TimeKey* key, int frame, bool assignEasing,  util::Easing::CubicBezier* bezier = nullptr){
```
The signatures are quoted correctly, but the specific "disagreement" named is a no-op: `const bool` on a **by-value**
parameter is a meaningless top-level const (it is ignored in the function type) and buys no const-correctness.
The genuinely inconsistent part is the pointer: `setEasingTo` takes `const CubicBezier*` (read-only, and it only
reads: `:658 if (aBezier) aEasing.cubicBezier = *aBezier;`) while `assignEasing` takes a mutable `CubicBezier*`
although it only forwards it. Plus the naming-convention split (`aXxx` vs bare). Cosmetic; no behavioural impact.

---

## 6. Q1 — IS THE FEATURE USABLE? (producer of the clipboard format)

**A producer EXISTS — and it is not the timeline.** The expected payload `"x1,y1,x2,y2"` is written by the Copy
button of the bezier editor dialog `Ui_splineWidget` (`src/gui/prop/splineWidget.h:181-191`):
```cpp
        QToolButton::connect(toolButton, &QToolButton::clicked, [=]() {
            QString str;
            str.append(QString::number(bezier->x1)).append(",");
            str.append(QString::number(bezier->y1)).append(",");
            str.append(QString::number(bezier->x2)).append(",");
            str.append(QString::number(bezier->y2));
            QClipboard *clip = QGuiApplication::clipboard();
            clip->setText(str);
```
and the same dialog's Paste button parses that very format (`splineWidget.h:196-205`, size==4 + `toFloat`), i.e. the
format is a (de facto) app-internal convention, defined only by this pair of lambdas. `QString::number(float)` emits
C-locale `"0.666667"` / `"1e-08"` (probe), which `toFloat` re-parses fine — the round trip works.

Reachability of the producer: the property dock's **Easing item → "Custom" button (tooltip `"Bezier editor"`)** —
`src/gui/prop/prop_Items.cpp:241-256`:
```cpp
        mCustomEasing->setToolTip(QCoreApplication::tr("Bezier editor"));
        mCustomEasing->connect(mCustomEasing, &QToolButton::clicked, [=]() {
            mBox[0]->setCurrentIndex(util::Easing::Type_Custom);
            auto* splineWidgetClass = new Ui_splineWidget();
            auto* splineWidget = new QDialog();
            auto* cubicBezier = new util::Easing::CubicBezier(mCubicBezier);
            splineWidgetClass->setupUi(splineWidget, mGUIResources, cubicBezier);
```
So the working flow is: dump the numbers in the property panel's editor → Copy → right-click a timeline key →
"Paste custom easing". It is *not* discoverable from the timeline: **there is no "Copy custom easing" action, no
documented format anywhere** (`git grep -i "custom easing" upstream/master -- '*.md'` → no matches; the only
Markdown files are READMEs, ISSUE_TEMPLATEs, `src/deps/pugixml/LICENSE.md`, `tests/golden/README.md` and three
`tools/*/README.md`), **no tooltip/status-tip/What's-This** on the action (`:183` sets only the text:
`mPasteCustomEasing = new QAction(tr("Paste custom easing"), this);`), and **no hand-typing hint** either. A user who
has never opened the property panel's bezier dialog has literally nothing telling them what to paste or that the
format is four comma-separated numbers.

Menu placement and state gating — `src/gui/TimeLineEditorWidget.cpp:396-423`:
```cpp
    mTargets = core::TimeLineEvent();
    const bool clickedOnKey = mEditor->selectKeysAt(mTargets, aPos);
    mPastePos = aPos;
    mPasteKey->setEnabled(mCopyTargets.hasAnyTargets());
    if (clickedOnKey || mEditor->retrieveSelectionTargets(mTargets)) {
        ...
        menu.addMenu(mSelectRange);
        menu.addAction(mPasteCustomEasing);   // :415 — only in the key-menu branch
```
* `mPasteCustomEasing` is added only in the key-menu branch; it has **no** `setEnabled(...)` call anywhere (contrast
  the line above it, where `mPasteKey` is explicitly enabled from `mCopyTargets`). So whenever the key menu appears,
  the item is enabled.
* `mTargets` is populated by `selectKeysAt` (`:400`; `src/ctrl/TimeLineEditor.cpp:340-357` — full selection if the
  right-clicked key is selected, otherwise the clicked key becomes the selection) or by
  `retrieveSelectionTargets` (`src/ctrl/TimeLineEditor.cpp:359-364`, which returns `false` when `mSelection.empty()`).
  Therefore when the item is shown, `mTargets` is non-empty.
* **No project**: not reachable. `TimeLineEditorWidget::setProject(nullptr)` (`:221-241`) calls
  `mEditor->setProject(nullptr)` → `clearRows()` → `clearSelection()` (`src/ctrl/TimeLineEditor.cpp:63-80`), and with
  no rows the hit test cannot return a key, so `clickedOnKey == false` and `retrieveSelectionTargets` returns false →
  only the else branch (Paste key) is offered. So no extra gating is needed for the "no project" case (verified by
  code reading, not executed).
* **Silent no-op case**: the loop skips Bone/Mesh/Image keys (`:757`). A selection consisting only of such keys
  leaves the item enabled, does nothing, and prints no message. Same for a successful paste: **no success feedback**
  at all (the only message is the failure branch).

Working-tree caveat (context for the reviewer): on the checked-out branch `feat/export-ui-redesign` (c9ec3c3)
`56b04b2` is **not** an ancestor (`git merge-base --is-ancestor 56b04b2 c9ec3c3` → NO), `onPasteCustomEasingTriggered`
does not exist there (`git grep -c` → no match), and `src/gui/prop/splineWidget.h` was **deleted** as dead code by
`68d5998 "refactor: delete the dead generated splineWidget dialog"` (branch-local commit). So neither the feature nor
its producer exists in the current working tree.

## 7. Q2 — input matrix (all traced through the actual code)

Gate is `clip->text().split(",")`, `size() == 4`. Qt doc (QClipboard page, MCP): `text()` "Returns the clipboard
text as plain text, **or an empty string if the clipboard does not contain any text**."

| Clipboard text | split size | Verdict | What happens / what the user sees |
|---|---|---|---|
| empty (`""`) | 1 | **rejected** | `QMessageBox::information` "Error / Invalid clipboard data" (untranslated) |
| `"1,2,3"` | 3 | **rejected** | same info box |
| `"1,2,3,4,5"` (also `"1,2,3,4,"`) | 5 | **rejected** | same info box |
| `"a,b,c,d"` | 4 | **accepted, silently corrupted** | all four `toFloat` fail → `(0,0,0,0)`; type forced to Custom; curve ≈ **linear** (probe) — no message |
| `"1,2,x,4"` | 4 | accepted, silently corrupted | `x2` silently becomes `0.0f` |
| `"0,0,1,1"` | 4 | accepted | probe: exactly linear output; harmless, but type becomes Custom |
| `"2,0,-1,1"` (x1=2,x2=−1) | 4 | accepted, **wrong curve** | probe: `0.0000 0.0014 0.0061 0.6304 0.5000 0.3696 0.9939 0.9986 1.0000` → jumps forward then **runs backwards**; no message, no crash |
| `"1,1,0,0"` (x1>x2) | 4 | accepted, benign | `x(t) ≡ y(t)` → exactly linear (probe). x1>x2 is *not* the problem per se |
| `"1, 2, 3, 4"` (spaces) | 4 | **accepted** | whitespace is tolerated: probe `field[1]=" 2" toFloat=2 ok=1`; same for tabs/newlines (`"1,\t2\n, 3 ,4 "` → 1,2,3,4, all ok=1) |
| `"nan,inf,-inf,1"` | 4 | **accepted with ok=1!** | probe: `toFloat=nan ok=1`, `inf ok=1`, `-inf ok=1` → curve is `nan` at every sample → NaN reaches transforms |
| `",,,"` | 4 | accepted, silently zeroed | `(0,0,0,0)`, ≈ linear |

Crashes from these inputs: **none**. No input in this matrix triggers the null-key dereference discussed in §9, and
the Newton solver is loop-bounded (§8).

## 8. Q3 — does anything downstream validate, clamp or sanitise the bezier?

**No. Nothing validates, clamps or sanitises `cubicBezier` anywhere in the tree.**

Full consumer list (`git grep -n "cubicBezier\|Type_Custom" upstream/master`):
* `src/core/Serializer.cpp:85-95` — writes type/range/weight/x1,y1,x2,y2 unconditionally.
* `src/core/Deserializer.cpp:123-140` — reads them only for `mVersion >= QVersionNumber(0, 8)`.
* `src/util/Easing.cpp:214-233` — `calculateBezier` / `calculate(Param&)` (the only runtime evaluator).
* `src/gui/prop/prop_Items.cpp:247-256,308,317` — property item.
* `src/gui/prop/splineWidget.h:147-161,249-251` — dialog UI + `QEasingCurve` preview.
* `src/gui/prop/bezierCurveEditor.cpp:119-144` — canvas `clampX`/`clampValue` (drag only).
* `src/gui/TimeLineEditorWidget.cpp:760` — the new paste path.

Validation that exists is useless for this: `Easing::Param::isValidParam()` (`Easing.cpp:8-16`) checks only
`type`, `range`, `weight` — the bezier struct is not examined, so the deserializer's "error correction"
(`Deserializer.cpp:138-141`) cannot catch a bad bezier either.

Solver (`Easing.cpp:195-212`):
```cpp
double solveTForX(const double x, const double x1, const double x2, const double epsilon = 1e-6) {
    double t = x; // Initial guess
    constexpr int iter = 6; // Max iterations
    for (int i = 0; i < iter; ++i) {
        const double xt = cubicBezier(t, 0.0, x1, x2, 1.0);
        const double dx = cubicBezierDerivative(t, 0.0, x1, x2, 1.0);
        if (std::abs(dx) < 1e-8) break;
        double tNext = t - (xt - x) / dx;
        if (std::abs(tNext - t) < epsilon) break;
        t = std::clamp(tNext, -2.0, 2.0);
    }
    return t;
}
```
* **No infinite loop** (fixed 6 iterations), **no division by zero** even for `x1 == x2` (guarded by the `dx` check;
  probe rows `0.5,0,0.5,1`, `0,0,0,1`, `1,0,1,1` all terminate with sane values).
* **No NaN guard**: for a NaN control point, `dx` is NaN, `std::abs(dx) < 1e-8` is false, `std::abs(tNext - t) <
  epsilon` is false, so the loop runs all 6 iterations and returns a NaN `t`; `cubicBezier` returns NaN.
* **No monotonicity enforcement**: for non-monotonic `x(t)` Newton converges to the wrong branch → wrong curve
  (probe `2,0,-1,1` above). This is silent: no diagnostic, no clamp.

Impact path: `src/core/TimeKeyBlender.cpp:322`
`return util::Easing::calculate(easing, -p0.relativeFrame, 0.0f, 1.0f, frame);` (i.e. b=0, c=1, d=frame, weight = the
key's weight) → the returned rate is used directly as the spline parameter and lerp factor
(`TimeKeyBlender.cpp:355-370`: `aExpans.setPos(aExpans.spline().getByLinear(time).toVector2D());`,
`aExpans.setCentroid(k0->centroid() * (1.0f - time) + k1->centroid() * time);`) → NaN positions/centroids for NaN
curves, backward motion for non-monotonic curves.

### Additional bug found in the same code path (weight ≠ 1)
`Easing.cpp:214-221`:
```cpp
float calculateBezier(const Easing::Param& aParam, const float t, const float b, const float c, const float d) {
    auto [x1, y1, x2, y2] = aParam.cubicBezier;
    // Initial linear extrapolation
    const float result = c * (t / d) + b * aParam.weight + (c * (t / d) + b) * (1.0f - aParam.weight);
    const auto bezier = cubicBezierEasedPercent(result, x1, y1, x2, y2);
```
`result` is the *percent fed into the x-solver*, but with `weight != 1` it is not in [0,1] (for b=0,c=1,d=1,w=0.5 it is
`1.5·t`), so the solver is asked for x up to 1.5 — outside the curve's domain. Probe (identity bezier `0,0,1,1`, w=0.5):
`0.0000 0.1875 0.3750 0.5625 0.7500 0.9375 0.5343 1.3192 0.9066` — output **>1 and non-monotonic**. The paste path
forces `Type_Custom` while **keeping the key's existing weight** (it only sets `type` and `cubicBezier`, `:656-658`),
and weight is user-editable in the property panel (`prop_Items.cpp:298-306`, `mDBox->setRange(0.0f, 1.0f)`), so this is
reachable: paste a custom easing onto a key whose weight < 1 → broken curve.

## 9. Q3b — serialization / save-reload survival

**It survives save+load. No version/offset change is needed, and 56b04b2 made none.**

* `src/core/Serializer.cpp:85-95` already writes the bezier for every easing param:
```cpp
void Serializer::write(const util::Easing::Param& aValue) {
    mOut.write((sint32)aValue.type);
    mOut.write((sint32)aValue.range);
    mOut.write((float32)aValue.weight);
    mOut.write((float32)aValue.cubicBezier.x1);
    ...
```
  and keys write it through that overload (e.g. `src/core/MoveKey.cpp:69`, `src/core/ImageKey.cpp:227`,
  `src/core/PoseKey.cpp:57`).
* `src/core/Deserializer.cpp:123-137` gates on `bool postBezier = mVersion >= QVersionNumber(0, 8);`, else
  `aValue.cubicBezier = util::Easing::CubicBezier();`.
* Format version at 56b04b2 is already `0.10` (`src/core/FormatVersion.h:15-16`), and the version bump that
  introduced the bezier field is `bb462a4 "- Added cubicbezier to serializer and deserializer - Version bump -
  Project bump"` which is an **ancestor** of 56b04b2 (`git merge-base --is-ancestor bb462a4 56b04b2` → BEFORE). So the
  on-disk record already carries the values; the parser reads them for any file ≥ 0.8.
* `git show --name-only 56b04b2` lists only `CMakeLists.txt`, 11 `src/gui/**` files and `TimeLineEditorWidget.h/.cpp`
  — **no** `src/util/Easing.*`, **no** `src/core/Serializer/Deserializer/FormatVersion`, **no**
  `GeneralSettingDialog.*`. The commit therefore needed no version/offset/table change.
* The assignment goes through `cmnd::Assign<Data>` on the key data (`ctrl/TimeLineUtil.cpp:166-200`,
  `assignKeyData`), i.e. the working key data is really replaced, so the next save writes the new bezier.

**Round-trip gap that does exist — but on the clipboard-key path, not the project path**: "Copy key"/"Copy key to
clipboard" serialises the easing *without* the bezier —
`src/gui/TimeLineEditorWidget.cpp:436-442`:
```cpp
    json->insert("eType", util::Easing::getTypeName(data.easing().type));
    json->insert("eRange", util::Easing::getRangeName(data.easing().range));
    json->insert("eWeight", data.easing().weight);
```
and the paste side reads only those fields — `src/ctrl/TimeLineEditor.cpp:378-385` (`objToEasing`) — so copying a key
whose easing is `Custom` and pasting it gives `Custom` with the **default** bezier, silently. (Same pair also has a
pre-existing name mismatch: writer `"eRange"` vs reader `obj["aRange"]` → the range is lost too. Both are
pre-existing, not introduced by 56b04b2, but they matter for any "does custom easing round-trip?" claim.)

## 10. Q4 — initialisation / lifetime of `mPasteCustomEasing`

**No read-before-write path; ownership is correct.**

Constructor (`src/gui/TimeLineEditorWidget.cpp:94-112`) member-init list ends at `mTimelineTheme()` and does **not**
mention `mPasteCustomEasing` (it also omits `mSelectEasing`, `mSelectRange`, `mSelectSpacing`, `mCopyToClipboard`).
But the assignment is inside the plain, unconditional scope block opened at `:121` and closed at `:188`, and there is
no `return`/`throw` in the constructor body before it:
```cpp
    {
        mCopyKey = new QAction(tr("Copy key"), this);
        ...
        mPasteCustomEasing = new QAction(tr("Paste custom easing"), this);        // :183
        mPasteCustomEasing->connect(mPasteCustomEasing, &QAction::triggered, [=] {onPasteCustomEasingTriggered();}); // :184
        mSelectSpacing = new QAction(tr("Select spacing between keys"), this);    // :186
```
The only uses are the lambda at `:184` (fires no earlier than the event loop) and `menu.addAction(mPasteCustomEasing)`
at `:415` (needs a context-menu event), so it is always assigned first — for **every** construction path, since the
block is unconditional.

Ownership: parented to `this` (`new QAction(tr("Paste custom easing"), this)`), and there is **no user-declared
destructor** (`git grep -n "~TimeLineEditorWidget" 56b04b2 -- src/` → no match), so the generated one plus Qt's
child-object deletion handles it; the `[=]` lambda captures `this` and is connected with `mPasteCustomEasing` as the
context object, so it dies with the action. No leak, no dangling.
Unrelated pre-existing wart visible in the same block: `QAction* mSetSpacing;` is declared
(`TimeLineEditorWidget.h:140`) and **never** assigned or used anywhere in the tree (the ctor assigns
`mSelectSpacing`), i.e. a dead member.

## 11. Q5 — the skip list and `XC_PTR_ASSERT`

**Skip list: CORRECT and exactly complete** relative to `assignEasing`'s switch.
`git grep` of `src/core/TimeKeyType.h:7-19` gives the full enum:
`Move, Rotate, Scale, Depth, Opa, Bone, Pose, Mesh, FFD, Image, HSV, Blur, TERM`.
`assignEasing` handles exactly nine:
`TimeKeyType_Move` (`:666`), `Rotate` (`:672`), `Scale` (`:678`), `Depth` (`:684`), `Opa` (`:690`), `Pose` (`:696`),
`FFD` (`:702`), `HSV` (`:708`), `Blur` (`:714`), plus `default: qDebug("You somehow tried to modify an unsupported
key, you absolute fool!");` (`:720-721`).
The loop skips `Bone`, `Mesh`, `Image` (`:757`), leaving *exactly* the nine handled types → no key type can reach the
`default` branch, and no handled type is wrongly skipped. Bone/Mesh have no easing member at all
(`git grep -n easing src/core/BoneKey.h src/core/MeshKey.h` → no matches), so skipping them is type-correct. Nuance:
`ImageKey` **does** have an easing param (`src/core/ImageKey.h:27`, serialized at `ImageKey.cpp:227`), so skipping
Image keys is a deliberate/pre-existing limitation, not a type limitation — and the failure is silent.

**`timeKey()` can return nullptr.** `src/core/TimeLine.cpp:85-95`:
```cpp
TimeKey* TimeLine::timeKey(TimeKeyType aType, int aIndex) {
    if (mMap.at(aType).contains(aIndex)) { return mMap[aType][aIndex]; }
    return NULL;
}
```
**`XC_PTR_ASSERT` is a no-op in release.** `src/XCAssert.h:35-42`:
```cpp
#define XC_PTR_ASSERT(ptr) \
    do { \
        if (!(ptr)) { \
            if (gXCAssertHandler) \
                gXCAssertHandler->failure(); \
            Q_ASSERT(0); \
        } \
    } while (0)
```
* `Q_ASSERT` in release, from the *installed* Qt 6.11.1 header `/usr/include/qt6/QtCore/qassert.h:29-30`:
  `#  if defined(QT_NO_DEBUG) && !defined(QT_FORCE_ASSERTS)` → `#    define Q_ASSERT(cond) static_cast<void>(false && (cond))` — nothing executes.
* `QT_NO_DEBUG` is injected by Qt itself for any non-Debug configuration:
  `/usr/lib64/cmake/Qt6Core/Qt6CoreConfigExtras.cmake:15` → `INTERFACE_COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG>`.
* The repo never defines `QT_FORCE_ASSERTS` (`git grep -n QT_FORCE_ASSERTS upstream/master` → no matches), and it does
  not set `CMAKE_BUILD_TYPE` in `CMakeLists.txt` (no match), so a plain release build is in the `QT_NO_DEBUG` case.
* The handler itself is inert by default: `AEAssertHandler::failure()` (`src/gui/Main.cpp:43-52`) only calls
  `gBackTracer.dumpCurrent()` under `USE_MSVC_BACKTRACE`, otherwise it does nothing.
Therefore, in a release build a null `key` falls straight through to `switch(key->type())` (`:665`) → null-pointer
dereference (UB/crash), with no diagnostic. This is **not new**: `onSelectEasingTriggered` (`:726-733`) and
`onSelectRangeTriggered` (`:736-743`) have the identical `timeKey(...)` + `XC_PTR_ASSERT` pattern, and the paste path
just copies it. I could not construct an input that reaches the null case (see §15).

## 12. Q6 — fidelity of the `setEasingTo` refactor (`assignEasing == true` and `== false`)

**Faithful; no behaviour change for existing callers.**

Parent `56b04b2^:src/gui/TimeLineEditorWidget.cpp` (read in full) had nine *identical* bodies of the form
```cpp
        auto newData =  dynamic_cast<const core::MoveKey*>(key)->data();
        if(assignEasing) { newData.easing().type = static_cast<util::Easing::Type>(easingType); }
        else { newData.easing().range = static_cast<util::Easing::Range>(easingType); }
        ctrl::TimeLineUtil::assignMoveKeyData(*mProject, *target->node, frame, newData);
```
The new helper (`:654-659`) performs exactly that `if/else` and nothing else but the added
`if (aBezier) aEasing.cubicBezier = *aBezier;`. For `bezier == nullptr` the two paths are semantically identical for
all nine cases (same cast, same `newData` copy, same `TimeLineUtil` call).
* Range path: `onSelectRangeTriggered` (`:735-744`) passes six arguments → `bezier` defaults to `nullptr` →
  `cubicBezier` is untouched. **No wrong overwrite occurs today.**
* **Latent hazard (real, but unreachable in this commit)**: `setEasingTo` applies the bezier *independently of*
  `aAssignEasing`, so a future caller doing `assignEasing(..., false, &bezier)` would assign a **range** and
  simultaneously overwrite `cubicBezier` — semantically wrong for a range-only edit. The only caller that passes a
  bezier is `:760`, and it passes `assignEasing = true`.
* `static` added to `assignEasing`: the function exists nowhere else in the tree (`git grep -n assignEasing 56b04b2
  -- src/` → only this file), so this only removes external linkage; no behaviour change, strictly safer.

**Easing-menu indices stay consistent.** The new action is *appended* — `:152` (`auto* custom = new QAction(tr("Custom"), this);`)
and `:155` (`none, linear, sine, quad, cubic, quart, quint, expo, circ, back, elastic, bounce, custom`) — so the loop
index `x` maps 0..11 exactly as before and `x == 12 == util::Easing::Type_Custom` (`src/util/Easing.h:25`). The
lambda captures `x` by value (`[=]`, `:160-162`), so the mapping is correct (a `[&]` would have sent 13 for every
item).
* `GeneralSettingDialog` was **not** touched by 56b04b2 (`git show --name-only 56b04b2` → absent) and already had
  the Custom entry at both `56b04b2^` and `56b04b2`: `kEasingTypeCount = 13` (`GeneralSettingDialog.cpp:14`),
  `easingToIndex("Custom") → 12` (`:59-61`), `indexToEasing` case 12 → "Custom" in both the translated (`:104-105`)
  and untranslated (`:135-136`) branches, and the combo is filled with `for (int i = 0; i < kEasingTypeCount; ++i)`
  (`:433-434`). `util::Easing::getTypeNameList()` loops `i < Type_TERM` = 13 (`Easing.cpp:128-134`), so
  `prop_Items.cpp:215 mBox[0]->addItems(util::Easing::getTypeNameList())` also yields 13 entries with index == Type.
  ⇒ The prior analysis's framing that this commit added Custom "to GeneralSettingDialog's easing list" is wrong; the
  commit added it to the **timeline context menu**, which *fixed* the previous 12-vs-13 mismatch.
* Consequence worth knowing: the timeline menu can now set `Type_Custom` with **no editor at all**, leaving the key
  with its stored bezier (the default smoothstep `(2/3,0,1/3,1)`, `Easing.h:36-39`) — consistent with the property
  panel's default, so not a bug, just a second entry point.

---

## (i) Corrections owed to the prior analysis

1. (a) CONFIRMED but mischaracterised in effect: the corrupted value isn't a visible "0.0f" curve — `(0,0,0,0)`
   evaluates to **y = x (linear)**, so the corruption is invisible; and `toFloat("nan")/("inf")` returns `ok == true`,
   so adding an `ok` check would still not stop the worst input.
2. (b) CONFIRMED as a missing check, with two corrections: the codified invariant is x∈[0,1]
   (`bezierCurveEditor.h:60-61`) and it is already violated by the app's own spin boxes (±2), and `x1 > x2` is *not*
   harmful by itself (`"1,1,0,0"` is exactly linear); the harmful case is non-monotonic x from out-of-[0,1] values.
3. (c) CONFIRMED, and additionally: the same untranslated `"Error"/"Invalid clipboard data"` pair already exists at
   `splineWidget.h:224` (duplicated, two sites to fix). The icon choice matches the sibling dialog's existing habit.
4. (d) CONFIRMED, with the sharper reason: `split()` never returns an empty list (probe: `""` → size 1), so the
   `isEmpty()` term is not what rejects the empty clipboard.
5. (e) PARTLY TRUE: `const bool` on a by-value parameter is a no-op, not extra const-correctness; the real (and
   trivial) inconsistency is `CubicBezier*` in `assignEasing` vs `const CubicBezier*` in `setEasingTo` on a
   read-only/forwarded parameter.
6. The prior analysis **omitted** the answers to my own questions that change the picture: the producer exists
   (splineWidget Copy button) but is undiscoverable from the timeline; the clipboard *key* round-trip drops the
   bezier; NaN input is accepted with `ok == true`; the weight≠1 breakage in `calculateBezier`; image keys silently
   skipped; and the feature is entirely absent from the checked-out branch.

## (ii) Real bugs, ranked

1. **HIGH — NaN/inf control points silently accepted → NaN animation.** Path:
   `TimeLineEditorWidget.cpp:750-753` (`toFloat` accepts `"nan"`, `"inf"`, `"-inf"` with `ok == true`, no check) →
   `:760 assignEasing(..., Type_Custom, ..., &bezier)` → `Easing.cpp:225-227` → `calculateBezier :214-221` →
   `solveTForX :195-207` (NaN passes both break conditions, returns NaN) → `TimeKeyBlender.cpp:322` →
   `setPos/setCentroid` with NaN (`TimeKeyBlender.cpp:355-370`). No validation anywhere (`isValidParam` ignores the
   bezier). Probe: every sample `nan`.
2. **HIGH/MEDIUM — out-of-range x silently produces a wrong, backtracking curve.** Path: `:748` (only field count is
   checked) → `:750-753` → `Easing.cpp:195-212` (Newton assumes a monotone x, none is enforced) →
   `TimeKeyBlender.cpp:322,355-370`. Probe `"2,0,-1,1"`:
   `0.0000 0.0014 0.0061 0.6304 0.5000 0.3696 0.9939 0.9986 1.0000`. The codebase itself declares x∈[0,1] required
   (`bezierCurveEditor.h:60-61`) and clamps it on drag, but the paste path (and the spin boxes, ±2,
   `splineWidget.h:138-139`) do not.
3. **MEDIUM — malformed fields silently become 0.0f, with the easing type still forced to Custom.** Path:
   `:750-753` (no `ok`), then `:760`. `"a,b,c,d"` → `(0,0,0,0)` → ≈ linear curve (probe), no message. The user
   believes a custom easing was applied, gets a linear curve, and a `Custom` type in the project file.
4. **MEDIUM — `Type_Custom` combined with `weight != 1` yields a broken curve (>1, non-monotonic).** Path: paste
   keeps the key's existing weight (`setEasingTo :656-658` only sets type + bezier) → `Easing.cpp:217` feeds
   `c*(t/d) + b*w + (c*(t/d)+b)*(1-w)` (up to 1.5 for w=0.5) into the x-solver → probe with the identity bezier and
   w=0.5: `0.0000 0.1875 0.3750 0.5625 0.7500 0.9375 0.5343 1.3192 0.9066`. Reachable because weight is
   user-editable (`prop_Items.cpp:298-306`).
5. **MEDIUM — "Copy key"/"Paste key" loses a custom bezier (and the range).** Path: writer
   `TimeLineEditorWidget.cpp:436-442` (eType/eRange/eWeight only) vs reader `ctrl/TimeLineEditor.cpp:378-385`
   (`objToEasing`, reads `eType`/`aRange`/`eWeight`) → pasted `Custom` key silently reverts to the default bezier;
   `"eRange"` vs `"aRange"` also drops the range. Pre-existing, but it breaks the natural "copy a key with my nice
   bezier, paste it elsewhere" workflow that this feature otherwise enables.
6. **LOW — silent success and silent no-op; error dialog is untranslated with an information icon.** Path:
   `:755-762` (no feedback on success; Bone/Mesh/Image-only selection does nothing) and `:764`
   (`QMessageBox::information(this, "Error", "Invalid clipboard data")`, duplicating `splineWidget.h:224`).
7. **LOW — no discoverability**: no "Copy custom easing" action, no tooltip/status tip on `mPasteCustomEasing`
   (`:183`), no documented format in any `.md`; the only producer is the property panel's bezier dialog Copy button
   (`splineWidget.h:181-191` reachable via `prop_Items.cpp:241-256`).
8. **LOW/LATENT — `setEasingTo` applies the bezier even when assigning a RANGE** (`:658`, unconditional on
   `aAssignEasing`); no current caller misuses it.
9. **LOW/LATENT — null `key` would dereference in release** because `XC_PTR_ASSERT` compiles to nothing
   (`XCAssert.h:35-42`, `qassert.h:29-30`, `QT_NO_DEBUG` from `Qt6CoreConfigExtras.cmake:15`); identical in the
   pre-existing `onSelect*Triggered` handlers, and I found no reachable input.
10. **LOW — dead member `QAction* mSetSpacing;`** (`TimeLineEditorWidget.h:140`) never assigned or used; pre-existing.

## (iii) What I could NOT determine, and why

* **No application build/run.** No network and no build attempted; every behavioural statement comes from reading the
  code plus two standalone probes compiled against the system Qt 6.11.1: a QString semantics probe
  (`.git/dsh-scratch/s5/qstrtest.cpp`) and a probe that links the **real upstream `src/util/Easing.cpp`**
  (`.git/dsh-scratch/s5/probe/easingprobe.cpp`, cwd `.git/dsh-scratch/s5/probe`, only `EasingName` stubbed). I did not
  observe the real menu, dialog, undo behaviour or saved project bytes at runtime.
* **Whether a real user selection can ever make `line()->timeKey(type, frame)` return nullptr** in this path. It is
  possible in principle (the accessor returns `NULL`), but the target's key comes from the same line at the same
  frame, so I could not construct a reachable case without an instrumented build. Correspondingly I cannot claim the
  null-dereference is live — only that it would be uncaught in release.
* **Qt prose quotes for `QString::toFloat` / `QString::split`**: the MCP `qt_documentation_read` of `qstring.html`
  was truncated (~133 KB omitted) before the per-function sections, and the MCP search only returns page-level
  excerpts, so I established those semantics with the compiled probe against Qt 6.11.1 instead (calling the real
  library). I did quote the MCP pages for `QClipboard::text()` ("or an empty string if the clipboard does not contain
  any text") and `QMessageBox` ("modal dialog for informing the user…"), and the installed Qt headers/cmake for
  `Q_ASSERT`/`QT_NO_DEBUG`.
* **Property-panel interactivity**: the canvas clamping and the `mStamp != value()` path (`prop_Items.cpp:279`) with
  `Param::operator==` ignoring `cubicBezier` (`Easing.cpp:18-20`) were read, not exercised. Because the bezier-button
  path calls `onValueUpdated(mStamp, value())` directly, I could **not prove** any bug from that equality gap; the
  subject feature's own write path (`cmnd::Assign`, `cmnd/BasicCommands.h:81-96`) does not compare at all, so the
  paste is applied and undoable (`mPrev` snapshot).
* **CI environment**: the repo tree contains no `QT_FORCE_ASSERTS` and no `CMAKE_BUILD_TYPE`, but I did not inspect
  external CI configuration beyond the repo (`.github/` had no matches), so an exotic CI configuration could still
  activate `Q_ASSERT`.
