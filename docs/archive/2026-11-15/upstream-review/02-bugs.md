# Verified bugs — 2026-09-18

Every item below was verified by an agent against `upstream/master` = `abb4b88` (or against
your own tree where marked), with raw evidence in `.git/dsh-scratch/` (see `04-artifacts.md`).
Line numbers are for `abb4b88` unless stated. Nothing here is present in your branch unless
the item says so.

Severity is "how bad is it for a user", not "how hard to fix".

---

## U1 — HIGH: "Reset recent files list" crashes the app

**Where.** Handler `src/gui/GeneralSettingDialog.cpp:475-481`; cast at `:479`, call at `:480`.
Construction sites: `src/gui/MainMenuBar.cpp:243` (parent = MainMenuBar — fine),
`src/gui/MainWindow.cpp:1127` and `:1679` (parent = MainWindow — **null**).

**Why it crashes.** `qobject_cast<MainMenuBar*>(this->parent())` returns `nullptr` for the two
MainWindow-parented dialogs; `mmb->onRecentsUpdate()` then writes through a null `this` (first
member access: `recentfiles = settings.value(...)`, `MainMenuBar.cpp:521`). Agent probe
reproduced the call as exit 139 (SIGSEGV).

**How a user gets there.** Export with FFmpeg missing/broken →
`GeneralSettingDialog::ffmpegCheck` / `ffmpegCheckFailed` `exec()` that dialog (with
`selectTab(2)`, lines ~910-911, ~933-934, ~860) → user clicks "Reset recent files list" on the
QoL tab → crash, after the modal "Success" box.

**Fix.** Prefer `ViaPoint::mainMenuBar()` (`ViaPoint.h:42`) or pass the menu bar explicitly
instead of casting `parent()`; a null check alone is the minimum.

**Same function, same fix pass.**
- `MainMenuBar.cpp:548-555` — the eight path actions are created with **no parent** and their
  last pointers are dropped by `actions.clear()`, so every refresh **truly leaks 8 QActions**
  (the placeholder at `:547` is owned and merely accumulates). One-line fix: parent them to
  `openRecent`, then `clear()` deletes them.
- `:530-534` — `while (length != 8) append(...)` only terminates from below: a settings value
  with **>8 entries never terminates** → the app hangs at startup. Fix: truncate or pad
  downward (`while (length < 8)` after capping).
- `src/ctrl/System.cpp:123-139` (pre-existing, but this commit makes it visible) — when the
  list is full it `pop_front()`s and **never appends the file just opened**, so the project you
  opened is missing from the menu the same function refreshes.
- `:563` `"Path placeholder"` is both the padding sentinel and a possible real path;
  `:598` `qDebug() << "You've earned yourself a cookie!"` fires from a live menu entry.

**Not in your tree** — yours has the pre-commit inline version.

---

## U2 — HIGH: "Paste custom easing" can produce NaN, backtracking, or silently-linear curves

**Where.** `src/gui/TimeLineEditorWidget.cpp:748-764` (parse + apply), `:663` (`assignEasing`),
`:655` (`setEasingTo`); evaluator `src/util/Easing.cpp:195-233`.

**Bugs, in order of severity.**
1. **NaN/inf are accepted.** `toFloat("nan")` returns `ok == true`, and `solveTForX` has no NaN
   guard, so the curve is NaN at every sample → propagates via
   `src/ctrl/TimeKeyBlender.cpp:322` into `setPos`/`setCentroid` (`:355-370`).
2. **No `[0,1]` validation on x1/x2** → non-monotonic x → the solver lands on the wrong branch.
   Probe input `2,0,-1,1` walks forward then **backwards**. The invariant is codified at
   `src/gui/prop/bezierCurveEditor.h:60-61` and clamped on canvas drag, but the app's own spin
   boxes allow ±2 (`splineWidget.h:138-139`).
3. **Malformed input is silently zeroed** while the type is still set to Custom —
   `(0,0,0,0)` renders as *~linear*, so the user believes a curve was applied.
4. **weight ≠ 1 corrupts the curve.** `Easing.cpp:217` computes
   `c*(t/d) + b*w + (c*(t/d)+b)*(1-w)` == `c*(t/d)*(2-w) + b`, i.e. the argument fed to the
   bezier is scaled by **(2−w)** — 1.5× at w=0.5, 2× at w=0. The non-custom path
   (`Easing.cpp:229`) does the sane thing: `result*w + linear*(1-w)`. Mirror it.
5. **Clipboard key copy/paste loses the bezier *and* the range**: writer
   `TimeLineEditorWidget.cpp:436-442` emits only eType/eRange/eWeight; reader
   `src/ctrl/TimeLineEditor.cpp:378-385`. The writer's `"eRange"` never matches the reader's
   `"aRange"` (pre-existing).
6. **No feedback at all**; silent no-op for Bone/Mesh/Image-only selections (the skip list is
   otherwise exactly the nine handled types); untranslated `"Error"` / `"Invalid clipboard
   data"` at `:764` (twin at `splineWidget.h:224`); `!list.isEmpty()` is dead (`split()` never
   returns an empty list); `const bool` by-value is a no-op while the bezier pointer's
   constness differs between the two helpers.
7. **Null key is unguarded in release**: `TimeLine::timeKey` can return `NULL`, and
   `XC_PTR_ASSERT` compiles to nothing (`Q_ASSERT` is off; `QT_NO_DEBUG` is defined for
   non-Debug configs).

**Producer.** The `x1,y1,x2,y2` format is written by `Ui_splineWidget`'s Copy button
(`upstream/master:src/gui/prop/splineWidget.h:181-191`), reachable from the property dock's
Easing → "Custom" (Bezier editor) button (`prop_Items.cpp:241-256`). **Your branch deleted
`splineWidget.h` (`68d5998`), but `src/gui/prop/EasingPickWidget.cpp:801-825` writes and parses
the identical format — with a per-field `ok` check.** So the two lines are complementary;
reuse your validation upstream rather than inventing one.

**Survives save/reload:** `Serializer.cpp:85-95` / `Deserializer.cpp:123-140` already carry
`cubicBezier` (format 0.10, gated at ≥0.8). No version bump needed; none was made.
**Not in your tree.**

---

## U3 — HIGH (for us): `_declspec` breaks the MinGW cross-build

**Where.** `src/gui/Main.cpp:20-23` (from `56b04b2`):
`_declspec(dllexport) DWORD NvOptimusEnablement = 1;` and the AMD twin.

**Verified by hand:** `x86_64-w64-mingw32-g++ -c` → `error: expected constructor, destructor, or
type conversion before '(' token` for `_declspec`; clean for `__declspec`. Native g++ rejects
`__declspec` (expected, Windows-only attribute).

**Confirmed for real on 2026-09-18** while cross-building upstream itself: `src/gui/Main.cpp:27`
fails with exactly that error, so this is not a portability nicety — it is *the* Windows
cross-build failure for any MinGW toolchain.

**Why it matters here and not upstream.** Upstream's Windows CI is MSVC
(`win64_msvc2022_64`, `setup-msvc-dev`), where `_declspec` is a documented compatibility
synonym — they never see it. Our Windows path is the MinGW cross-build
(`scripts/build-windows.sh`, added by us in `dfc8ab1`; `build-win/` is configured with
`x86_64-w64-mingw32-gcc-ar` and `moc_predefs.h` shows `__MINGW64__`), and `Q_OS_WIN` is true
for MinGW Qt builds, so the `#ifdef` does not exclude it. **Our Windows build breaks the
moment we take `56b04b2`.**

**Fix.** Two underscores, or a portable macro:
`#if defined(_MSC_VER) || defined(__clang__)` → `__declspec(dllexport)`,
`#elif defined(__GNUC__) && defined(_WIN32)` → `__attribute__((dllexport))`.

**Also worth adding (not a bug):** log the adapter actually in use —
`glGetString(GL_RENDERER)`/`GL_VENDOR` once the context is current — because the exported
symbol is only a *request* (the OS preference wins on Win10 20H1+; the driver reads the image
at process start, so no runtime opt-out is possible). The mechanism itself is the documented
one; both vendors' own examples even use the same single-underscore spelling.

---

## U4 — HIGH: fresh installs get a family-less font at the wrong size

**Where.** `src/gui/Main.cpp:203-206` and `src/gui/MainWindow.cpp:128-131`.

**Mechanism.** With no `generalsettings/ui/font` key, `QSettings::value(key)` returns an
invalid QVariant → `toString()` is `""`; the guard is `uiScale.isValid()` (always true, see U7);
**`QFontDatabase::hasFamily("")` returns true** — I verified this with a Qt 6.11.1 probe
(`.git/dsh-scratch/s10-probe/hf.cpp`: `hasFamily("") = 1`, `families().contains("") = 0`) — so
the code takes `appFont = QFont("")`, discarding the platform default font **and its size**
(measured here: Sans Serif 9 pt → family-less 12 pt; label height 17 px → 15 px).
Secondary effect: `GeneralSettingDialog.cpp:266` then displays an empty Font field, because
`qApp->font().family()` is `""`.

**Fix.** `const QString fam = userFont.toString(); if (!fam.isEmpty() && QFontDatabase::hasFamily(fam)) {…}`
and keep the default size when no custom font is set.

**Also latent:** if a future edit removes the constant-true guard's default argument, the shared
font would be scaled twice (Main.cpp then MainWindow ctor) — the `QFont(family)` reassignment is
what currently hides that.

---

## U5 — MEDIUM-HIGH: the app-wide stylesheet silently disables `dialogbutton.ssa`

**Where.** `src/gui/MainWindow.cpp:536` `qApp->setStyleSheet(fontOption + standard)` (new in
`09c75b3`); `GUIResources::applyAppearance()` ends with `:128
qApp->setStyleSheet(dialogButtonStyleSheet())`, and every `onThemeChanged` emission happens
immediately after it (`GUIResources.cpp:40/48, 82-83, 90-91`). MainWindow's call always runs
last, so it **replaces** the purpose-built sheet.

**Consequence.** `QPushButton#browser` is styled only in `dialogbutton.ssa` (6 rules) and
`propertywidget.ssa` (5) — `standard.ssa` has **zero** occurrences — and
`NewProjectDialog.cpp:49` sets that objectName, so its file-picker button loses its styling.

**Fix.** Own the app-wide sheet in one place: `dialogButtonStyleSheet() + fontOption + standard`
inside `GUIResources::applyAppearance()`. Or drop the app-wide QSS entirely and rely on
`qApp->setFont`, which is what the font feature needs and which Qt documents as inherited.

**Do not "optimise" the rebuild.** Qt QSS has no variables; `theme::Colors::substitute`
regenerates `@token@`s, so a fresh string per palette change is required. The per-window
`setStyleSheet` is pre-existing; only the app-wide call is new. The `qDebug()` at `:535` dumps
~55 KB, but only in debug builds (release disables `QtDebugMsg`, `Main.cpp:132-135`).

---

## U6 — MEDIUM-HIGH: the CJK/locale fonts cannot be selected, at ~48 MB

**Family names don't exist.** `preference_ja.txt` asks for `NotoJP`; the file's families are
**"Noto Serif JP"** (nameID 16) and **"Noto Serif JP ExtraLight"** (nameID 1). Likewise
`NotoCN` → "Noto Serif SC"/"…ExtraLight"; `NotoKO` → **"Noto Sans KR"**/"…Thin". The strings
`NotoJP/NotoKR/NotoKO/NotoCN` appear **nowhere** in the fonts (whole-file byte scan, ASCII and
UTF-16BE). Qt matches family names only, so `QFont("NotoJP")`, `hasFamily("NotoJP")` and QSS
`font-family: NotoJP` all fail silently — and `QWidget::font().family()` still echoes the
*requested* string, so only `QFontInfo::exactMatch()` exposes the fallback.

**Even fixed, the weights are wrong.** All three are variable fonts whose only reachable
instance is ExtraLight/Thin (JP/CN Serif; KO Sans at weight 100) — hairline CJK unless
`QFont::setVariableAxis` is used or proper instances are shipped.

**Cost.** NotoJP 12.9 MB + NotoCN 25.0 MB + NotoKO 10.4 MB ≈ **48 MB**, and our branch embeds
`assets/` into the binary via qrc → 48 MB in the executable for fonts nobody can select.

**Geist.** The four statics were byte-identical to the pre-deletion blobs. Regular/Medium/Bold
had tabular figures baked in (600/612/636 units, uniform); **SemiBold never did**
(683,427,…); the variable `Geist.ttf` defaults to proportional (663,384,…). `tnum` still exists
and works via `QFont::setFeature(QFont::Tag("tnum"), 1)`. Also lost: hinting (statics were
`ttfautohint v1.8.4.16`; the VF has none) and weight styles (`styles("Geist")` collapses from
`[Regular, Medium, SemiBold, Bold]` to `[Regular]`).

**Not churn:** `preference_kr.txt` was dead on arrival (`LocaleDecider.cpp:67-69` builds
`preference_ko`); `09c75b3` fixed it.

**Your side:** no `preference_*.txt` mechanism and no Noto fonts at all — this area is a
rename+modify conflict hotspot.

---

## U7 — MEDIUM: font/UI scaling — 13 read sites, several broken

| symptom | where | fix |
|---|---|---|
| `static_cast<int>(fontScale)` truncates: on the pixel branch uiScale ∈ [0.5,1.9) is a no-op, else floor-rounded (0.5 → `Pixel size <= 0` warning, size unchanged) | `Main.cpp:219`, `MainWindow.cpp:143` (**cast line is pre-existing 2023 code**, `2bcabf09`; `09c75b3` only made its input fractional) | `qRound`, or apply the scale once in one place |
| dead store + guard that never guards: `qreal fontScale = appFont.pointSizeF();` is always overwritten because `QSettings::value(k, 1.0).isValid()` is constant-true; the guard clearly meant the *font* key | `Main.cpp:196,198,204`; `MainWindow.cpp:122,129` | test the right key, delete the seed |
| `font-size: 11;` with no unit while the sibling header rule uses `%1pt` | `MainWindow.cpp:509` vs `:511-512`; `:527` vs `:529-530` | emit `pt` |
| unitless declaration is **silently dropped** by Qt (measured, zero messages) → the rule is dead text; `QString::number(x,'f',0)` rounds half-away-from-zero, visible in the header rule (±0.5 pt on a +1 pt step) | same | as above |
| `pointSize()` returns −1 on a pixel font → `font-size: 0pt` → header hierarchy silently vanishes | `MainWindow.cpp:487` | guard |
| paint loop reads `QSettings` per label and mixes units: `setPointSizeF(font.pixelSize() * uiScale)` with `pixelSize() == -1` → one warning per label per repaint, scale has no effect; on a pixel font the *metrics* font becomes point-sized so widths stop matching the painted text | `src/ctrl/time/time_Renderer.cpp:212-215` (from `779cbda`) | derive metrics in points, once, outside the loop |
| five icon sites read `uiScale` unclamped into `setIconSize`/`setFixedSize` → `uiScale = 0` gives 0×0 invisible buttons | `tool_ModePanel.cpp:42,64`, `tool_ViewPanel.cpp:27,66`, `tool_Items.cpp:45,61`, `prop_KeyKnocker.cpp:49` | clamp once at the read |
| `ok` from `toDouble(&ok)` never read → malformed locale size becomes 1 pt | `MainWindow.cpp:492-494` | restore the check |
| liveness is inconsistent: `MainDisplayStyle` and `KeyKnocker` are restart-only while the icon panels refresh live; the "Reset UI scale" button neither updates the spin box nor says a restart is needed (Language and Timeline format say "(needs restart)") | `MainWindow.cpp` ctor / `GeneralSettingDialog.cpp:345` | pick one policy |

---

## U8 — MEDIUM: the Comic Sans egg quits the app, and cost two CI commits

**Where.** `GeneralSettingDialog.cpp:957-971` (sets `bHal`), `MainMenuBar.cpp:246-249`
(`onSaveProjectTriggered(); throw std::exception();`).

**Behaviour.** The throw is caught by main's catch-all (`Main.cpp:117`) → modal "An unexpected
error has occurred, please restart the application" (with `result` still 0, so it reads
"Error code: 0") → the app exits. Choosing that font quits the application.

**Why CI broke, verified by me:** `std::exception("Comic Sans hate", 4)` and the 1-argument
form are MSVC-only; both fail on g++ 16 and clang++ 22 (`no matching function for call`), while
`std::exception()` compiles. Both `- Fix CI` commits removed the argument, not the construct.

**If the egg stays:** throw/catch a dedicated type locally rather than relying on main's
catch-all; keep the message; translate `"SIGILL"` (`:964`) and the HAL line (`:966`) — note
`:965` *is* already `tr()`'d.

---

## U9 — LOW-MEDIUM: `46c4f25` debris — patch ready, unapplied

`.git/dsh-scratch/s9/fix-46c4f25.patch` (5 files, 7 hunks, +2/−25) applies cleanly to `abb4b88`
(verified via `git archive` + `git apply --check` + real `patch -p1`; configure exits 0 after).

Removes the commented `CMAKE_MODULE_PATH` line; the `FindWindowsSDK` bracket-comment block
(`src/cmake/` exists in no revision); `LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}` (no-op for
an executable — the generated install script installs to `bin/` only); the duplicate
`QMenu { background: none; }` (it **cannot** do what its comment claims: `QWidget:disabled` is
0-1-1 and beats `QMenu` 0-0-1); the unused `#include "AudioPlaybackWidget.h"` (verified by
compiling the TU without it); and `qt_finalize_executable(AnimeEffects)` →
`(${PROJECT_NAME})` + trailing newline.

**Do not touch:** `CMAKE_AUTOUIC` (Qt's own convention, breaks a future `.ui` file);
`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` (dead in the default static/exe layout but live with
`BUILD_SHARED_LIBS=ON`); `qt_finalize_executable` itself (the target is `add_executable`, so
nothing else finalizes it — load-bearing on Windows/Android/WASM and for the `Qt6::Qml` our
branch links); `static themeList()` (its only caller uses the static form — removing it breaks
`GeneralSettingDialog.cpp:360`); `constexpr kRadius`; the by-value `const` on `AccentColor`.

Cannot apply to your tree (`data/stylesheet` → `assets/stylesheets`), and your branch already
lacks all six items.

---

## U10 — MEDIUM: DWM revert — revert-with-prerequisites, needs the Win11 test

- Live upstream state: `AppStyle.cpp:71-86` — unconditional attributes, then
  `#ifdef Q_OS_WIN` with `microVersion() > 22000` → `DwmSetWindowAttribute(…DWMWCP_ROUND)`,
  `else` → `setWindowFlags(FramelessWindowHint | Popup | NoDropShadowWindowHint)`.
- `WinResizeFilter` is **dead** (commented out by `779cbda` with a `#1#` hack), so `133fb51` is
  content-neutral; the deletions worth making are `AppStyle.cpp:36-67` and `:76-85`.
- **A hand-written patch is required** (`git apply -R` fails), and it **must add
  `#include <QMenu>`** — the file has no direct include and only compiles because
  `ctrl/System.h` → `MainMenuBar.h` → `EasyDialog.h` → `QMenuBar` → `qmenu.h` pulls it in.
  Removing `ctrl/System.h` without adding it yields `incomplete type 'QMenu'` + a
  `qobject_cast` static_assert (agent-verified by compiling `abb4b88`'s tree).
- CMake: **keep `user32`** (`DockSash.cpp:82` `SetWindowRgn`); only `dwmapi` becomes droppable.
- Your fix is `4814b5f` + `7fd0d16`, both **phase 1** → contributable; same function, same
  widget guard, ORs `FramelessWindowHint`, plus a 1-bit `QBitmap` mask via `eventFilter`.
- **Honest caveat:** on Win11 22H2+ the DWM path rounds antialiased and adds the system shadow;
  a 1-bit mask cannot antialias — the revert may trade better corners for cruder ones. The
  laptop is build **26200** → exactly the DWM branch, so the test in `03-windows-test-plan.md`
  settles it.

---

## U11 — LOW: smaller verified items

- `standard.ssa:277-280` duplicate `QMenu` rule — dead, and its stated rationale is false (U9).
- `mSetSpacing` (`TimeLineEditorWidget.h:140`) declared, never used (pre-existing).
- `MainWindow.cpp:1127/1679`: `aDiag` is never deleted and on the success path never `exec()`'d
  → one hidden dialog accumulates per export attempt.
- Upstream has **no translation regeneration at all** (`lupdate`/`qt_add_translations`/
  `LinguistTools` appear nowhere), and `09c75b3`'s own new strings are absent from every
  catalog — so `Save`/`Discard` is one instance of a systemic gap. Measured with `lrelease` +
  `QTranslator`: `gui::MainWindow/"Save"` falls back to English; the pre-existing `"Save"` in
  `gui::ProjectTabBar` does not help (lookup is by context+source). Your branch has the
  pipeline (`qt_add_translations`, `src/gui/CMakeLists.txt:210-224`).
- `INT32_MAX` sentinel (`TimeLineWidget.cpp:444`) is functional (forces an audio re-seek on loop
  wrap; matters at `maxFrame == 1`) but undocumented; `INT32_MAX` resolves only via Qt's
  transitive `<stdint.h>`, and the obvious `<climits>` fix would **not** work.
- `trackPositionMs` (`AudioPlaybackWidget.cpp:32`) cast removal is behaviour-preserving
  (85.9M inputs, 0 mismatches) — **not** a defect. `-Wall -Wextra` does not warn about it, and
  upstream enables no warning flags at all (that is our `ae_enable_warnings()`).

---

## U14 — MEDIUM (tooling, ours): capital-case `Windows.h` blocks every Linux MinGW cross-build

Found 2026-09-18 while building upstream for the DWM test. `#include <Windows.h>` fails in a Linux
cross-build: `fatal error: Windows.h: No such file or directory`. Fedora's
`mingw64-headers-13.0.0-3.fc44` ships **only lowercase `windows.h`**, and `find -iname` over the
whole sysroot finds no capital variant (only 8 capitalised headers exist in that include dir, none
of them Windows).

**Upstream has six such includes** (`src/gui/{AppStyle,DockSash,MSVCBackTracer,MSVCMemoryLeakDebugger,Main,MainMenuBar}.cpp`).
**Our branch has none** — `dfc8ab1` ("build: add mingw-w64 Windows cross-build tooling", phase 1)
normalised them to `<windows.h>` *and* added the tooling, which is exactly why the 2026-08-25
`xwin-dist` build succeeded and why the same script now dies on upstream. Upstream never sees this:
their Windows builds are native/MSVC (`build_and_run.ps1`, CI `windows-latest`), where the
filesystem is case-insensitive.

Fix is six one-line edits to lowercase, valid on every Windows toolchain. Test-only versions of
that patch are applied in `.git/dsh-scratch/wt/{u,q}`.

---

## U15 — MEDIUM: `GeneralSettingDialog.h` omits `QComboBox` exactly on Windows

    #ifndef Q_OS_WIN
    #include <QComboBox>
    #endif

`09c75b3` removed the unconditional `#include <QComboBox>` from that header and `779cbda` replaced
it with this guard — so the include is dropped on the one platform that needs it. It compiles under
MSVC only because some other header pulls `qcombobox.h` in transitively; MinGW exposes it
immediately: `src/gui/GeneralSettingDialog.h:57: error: 'QComboBox' does not name a type` (four
members). Fix: include it unconditionally (which is also what the type's use requires).

Same family as the earlier finds — a header relying on transitive includes that happen to hold on
one toolchain only.

---

## U12 — OPEN (our side): we never finalize the executable target

Our branch has no `qt_finalize_executable`; upstream calls it. We use `add_executable` +
`qt_add_qml_module` + `Qt6::Qml`. `qt_add_qml_module` *registers* executable finalizers
(`INTERFACE_QT_EXECUTABLE_FINALIZERS`, `Qt6QmlMacros.cmake:4864-4886`) but I found no call to
`qt6_finalize_target` in it — only `qt_add_executable`/`qt_add_library`/`qt_add_plugin` do
(`Qt6CoreMacros.cmake:947/1036/1041`). Effect is nil on Linux/clang but material on
Windows/Android/WASM/Apple. Test: configure and look for the deploy-QML-imports finalizer
running. Low priority unless Windows packaging misbehaves.

---

## U13 — FIXED 2026-09-18 (ours, uncommitted): the Windows script copied a directory that no longer exists

`scripts/build-windows.sh:69` ran `cp -r "$SRC_DIR/data" "$OUT_DIR/data"` under
`set -euo pipefail`, but phase 1 renamed `data/` → `assets/` (`34083a8`) and then removed the
runtime dir entirely (`dd511a8`) — fonts now come from `:/fonts` and translations from `:/i18n`
(`Main.cpp:143-176`). So deploy **died mid-way** on our tree. (Against upstream the line is
*correct*: upstream still resolves `appDir + "/data"`, `Main.cpp:164` — which is why it was written
that way, and why the fix is conditional rather than a deletion.)

**Fix applied** (working tree, not committed): the copy is now conditional —

    if [ -d "$SRC_DIR/data" ]; then cp -r "$SRC_DIR/data" "$OUT_DIR/data"; fi

so upstream-shaped trees still ship `data/` and ours skips it; the header comment and the final
`echo` now say "when the source tree ships one".

**Verified:** `bash -n` clean; `BUILD_DIR=build-win OUT_DIR=<abs> ZIP=<abs> scripts/build-windows.sh
--deploy-only` assembled 53 DLLs, created **no** `data/` directory, and produced a 49,398,254-byte
zip — the exact run that used to abort at the copy.

Incidental wart found while verifying: the script assumes absolute `ZIP`/`OUT_DIR`; with a relative
pair the packaging `cd` resolves the zip path wrongly ("zip I/O error"). Pre-existing, only bites
hand-rolled invocations.

---

## Landing order (my suggestion)

1. **U1** (crash) and **U4** (fresh-install font) — hit every install, small diffs.
2. **U2** (NaN easing) and **U5** (dialogbutton clobbering) — correctness, contained diffs.
3. **U6** (font family names) — largest bytes saved for the smallest diff.
4. **U3** (`__declspec`) — one line, and it unblocks *our* Windows build.
5. **U10** (DWM) after the laptop test; **U7/U8/U9** as follow-ups; **U11/U12** opportunistically.
6. **U13** (our stale cross-build script) — one line, and it unblocks the Windows test builds.
