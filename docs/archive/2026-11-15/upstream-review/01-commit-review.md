# The 10 upstream commits — summary and verdict

`upstream/master` = `abb4b88` (2026-09-11). These are the commits your branch does not have
(merge-base `832195d`; you are 172 ahead, 10 behind). Each entry: what it does, what is wrong,
verdict, and confidence. Raw evidence in `.git/dsh-scratch/` (see `04-artifacts.md`).

**Confidence key:** HIGH = verified by execution/compilation/measurement; MED = verified from
source + docs; LOW = argued from source only, or platform-dependent and untestable here.

---

## `324e456` — Merge PR #35 (your timeline branch) · Yukusai
Brings **nothing** beyond your base: `832195d..324e456` contains only the merge commit itself.
Your base already is the merged tip. No action.

---

## `46c4f25` — "- Misc" · 6 files, +39/−6
**Does:** adds `CMAKE_AUTOUIC ON`, `LIBRARY DESTINATION`, `qt_finalize_executable(AnimeEffects)`;
`kRadius` → `constexpr`; a duplicate `QMenu { background: none; }` in `standard.ssa`;
`GUIResources::{viewportBackground,themeList}` → `static`; a stray `#include "AudioPlaybackWidget.h"`;
four blocks commented out with `#[[ … ]]`.

**Verdict: debris, with two keepers.** Six of eight items are removable (patch ready, U9).
My initial "API churn" and "`qt_finalize_executable` is redundant" calls were **wrong**: the
`static` on `themeList` is load-bearing (its only caller uses the static form), and
`qt_finalize_executable` is load-bearing because the target is created with `add_executable`.
Nothing was re-enabled later except two of the commented blocks, which `fc5a703` un-commented —
evidence they were scaffolding. Confidence: HIGH (patch verified by applying it to `abb4b88`
and configuring).

---

## `fc5a703` — "Fix DWM not compositing rounded windows correctly, hacky fallback for Windows 10 or older" · 3 files
**Does:** re-enables the commented `WIN32` CMake blocks; on Win11 (`microVersion() > 22000`,
i.e. build > 22000) calls `DwmSetWindowAttribute(…DWMWCP_ROUND)`; otherwise installs a
`WinResizeFilter` that calls `CreateRoundRectRgn`/`SetWindowRgn` on every resize. Also drops
`<QPainter>`, `<QPainterPath>`, `<QStyleOption>` includes and deletes your explanatory comments
about popup translucency, replacing them with "Windows is dumb and stupid…".

**Verdict: the DWM half is revertible with prerequisites.** The filter is already dead
(`779cbda` replaced it and commented the class out). Not-Windows builds were broken by this
commit until `133fb51`. A naive revert **does not compile** — it must add `#include <QMenu>`
(the file has no direct include; it only compiles via `ctrl/System.h` → `MenuBar` → `QMenuBar`
→ `qmenu.h`), verified by compiling `abb4b88`'s tree. Confidence: HIGH for code facts; the
Win11 visual outcome is **LOW** (needs the laptop test).

---

## `133fb51` — "Forgor to gate the class behind an #if for Windows" · 1 file
**Does:** wraps `WinResizeFilter` in `#ifdef Q_OS_WIN`.

**Verdict: a fix for this commit's own breakage**, two days late — Linux/macOS master was
uncompilable in between (the class used `HWND`/`CreateRoundRectRgn` unguarded). It is
**content-neutral today** (`779cbda` commented the whole block out afterwards), so reverting it
buys nothing; reverting it faithfully would also re-add `<QStyleOptionSlider>`, papering over
half of the missing-include problem while leaving `QMenu` broken. Confidence: HIGH.

---

## `dbd319a` — "Update locale preferences, added CJK fonts." · 13 files
**Does:** deletes the four static Geist weights, adds variable `Geist.ttf`; adds
`NotoCN/JP/KR.ttf`; adds `preference_kr.txt` (later renamed `_ko`); per-locale magic font sizes.

**Verdict: the feature does not work, and it costs ~48 MB.** The requested family names
(`NotoJP`/`NotoCN`/`NotoKR`) exist in none of the shipped fonts — the real families are
"Noto Serif JP", "Noto Serif SC" (**Serif**, in a sans app) and "Noto Sans KR", each with only
an ExtraLight/Thin instance reachable. `hasFamily`/`QFont`/QSS all fail silently (and
`QWidget::font().family()` still echoes the request, so it *looks* right). `preference_kr.txt`
was dead on arrival because `LocaleDecider` builds `preference_ko` — a bug the next commit
fixed. Geist: tabular figures were baked into Regular/Medium/Bold but **not** SemiBold; the VF
defaults to proportional, though `tnum` remains available via `QFont::setFeature`; hinting and
the Medium/SemiBold/Bold styles are lost. Confidence: HIGH (hand-written sfnt parser + Qt 6.11.1
probes; statics verified byte-identical to the pre-deletion blobs).

---

## `09c75b3` — "UI scaling (only fonts for now), custom font options, recent file list update" · 21 files, +374/−216
**Does:** adds `generalsettings/ui/{uiScale,font,customFont}` + their settings UI;
`MainMenuBar::onRecentsUpdate()` with three call sites; tab reshuffle; `#include
"gui/MainMenuBar.h"` in `ctrl/System.h`.

**Verdict: the substantive commit, and the most consequential.** Five separate problems:
1. **Layering inversion** — `ctrl/System.h` includes `gui/MainMenuBar.h`;
   `ctrl::ProjectLoader` gained a `gui::MainMenuBar*` member (`mMMB`) that is **never read**
   (verified: only the assignment and the declaration); `MainWindow` sets
   `mSystem.mainMenu = mMainMenuBar`. The codebase already has `util::Signaler` for this.
2. **The recents reset crashes** (U1) — reachable SIGSEGV via the two MainWindow-parented
   dialog sites, plus 8 leaked QActions per refresh and a non-terminating padding loop.
3. **Font application is broken in four ways** (U4, U7): `hasFamily("")` is true so a default
   install gets `QFont("")`; the `uiScale.isValid()` guards are constant-true (the author meant
   the *font* key) which also makes `fontScale = pointSizeF()` a dead store; the pixel branch
   floors the scale; the QSS emits a unitless `font-size` (silently dropped) next to a `%1pt`
   rule.
4. **It clobbers `dialogbutton.ssa`** (U5) — the app-wide sheet replaces GUIResources' sheet.
5. **`onRecentsUpdate` is the old code moved, not fixed** — the 8-variable `firstPath…eigthPath`
   switch survives verbatim, with the `qDebug` cookie line, and the `disconnect` loop is a no-op
   (sender/receiver mismatch: `openRecent->disconnect(action)` removes the *menu's* outgoing
   connections). Confidence: HIGH.

---

## `779cbda` — "UI Scaling now applies to most icons / Bugfixes / Pain and suffering" · 14 files
**Does:** icons scale via `QSettings` reads at each call site; Windows sash line 2 px vs 1 px;
the Comic Sans easter egg.

**Verdict: smells, and one item is genuinely bad.** `time_Renderer.cpp:212-215` reads
`QSettings` **per label per repaint** and calls `setPointSizeF(font.pixelSize() * uiScale)` —
`pixelSize()` is −1 for a point font, so it warns once per label and does nothing (U7). The
`WinResizeFilter` class is kept as a `/* … #1# … */` comment hack rather than deleted. The
easter egg saves your project and then **exits the app** through main's catch-all, and cost two
follow-up CI commits (U8). Profanity in comments; `highFactor` added twice to a radius. The
`QMenu`/`QWidget` specificity claim in the stylesheet is false. Confidence: HIGH.

---

## `56b04b2` — "Multi-key Bezier pasting / bugfixes / disable C++ extensions / dGPU / bigger icons" · 13 files
**Does:** `CMAKE_CXX_EXTENSIONS OFF`; the dGPU export symbols; "Paste custom easing";
`1.2` factors on the icon constants; `exportWithAudio = false` for png/gif.

**Verdict: the mechanism is documented, the details are not.** `_declspec` is copied
token-for-token from both vendors' own examples — and it **breaks our MinGW cross-build** (U3).
The easing feature accepts NaN/inf and out-of-range x, and the weight term scales the bezier
argument by (2−w) (U2). The `1.2` fudge multiplies on top of `uiScale` and the "24×24 with 18 px
glyphs" comments are now wrong (28.8). The `setEasingTo` refactor itself is **good** — it removes
real duplication, and my claim that it added a Custom entry to `GeneralSettingDialog` was
**wrong** (it added the timeline menu entry, fixing a 12-vs-13 mismatch). Confidence: HIGH
(compilers, probes against the real `Easing.cpp`).

---

## `25501ba` + `abb4b88` — "- Fix CI(?)" and "- Fix CI" · 1 line each
**Do:** `throw std::exception("Comic Sans hate", 4)` → `("Comic Sans hate")` → `()`.

**Verdict: two commits of CI budget spent keeping a joke compiling.** I verified the cause:
`std::exception(const char*)` and the MSVC 2-arg form fail on g++ 16 *and* clang++ 22; the bare
form compiles. So the construct, not the message, is what breaks — and the second "fix" removed
the punchline while keeping the control flow (`throw` caught by main's catch-all → app exits).
Confidence: HIGH.

---

## Cross-cutting observations

- **Their process:** three of the ten commits are repairs to the two commits before them
  (`133fb51` → `fc5a703`; the two CI fixes → `779cbda`), and one is a substring of another
  ("- Misc"). Two commits shipped code that does not compile on GCC (MSVC-only `std::exception`
  ctor; ungated Windows API in a style file). Review standards there are loose.
- **Their build has no warning flags at all** — no `-Wall`, `-Wextra`, `-Wconversion`, `-Werror`
  anywhere first-party. `-Wall -Wextra` is *your* `ae_enable_warnings()`, added on this branch.
- **Their translation catalogs are hand-committed and stale** — there is no regeneration
  pipeline upstream; yours has one.
- **Their locale/font work assumes the on-disk `data/` layout**, which your phase-1 work
  replaced with `assets/` + qrc embedding — the main merge hotspot.
