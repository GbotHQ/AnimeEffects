# Windows test plan — the laptop

**Machine:** `DellikM`, Windows 11 Pro, `10.0.26200` → **build 26200 > 22000**, so
`QOSV::microVersion() > 22000` is TRUE and the **DWM branch** is the live path there. Windows 10
(≤ 19045) takes the `else` Qt-flags branch and **cannot be tested on this machine** — say so in
any upstream discussion rather than implying both were covered.

**Access:** `ssh -F upstream-review/sshconfig laptop` (that shim is a 1-line file: `Include
~/.ssh/config`; the system ssh config is broken here, see README). `scp -F upstream-review/sshconfig`
works the same way.

**Launching a GUI over ssh:** a Windows ssh session is not your desktop session, so a GUI started
directly from ssh may be invisible. Use a scheduled task with `/it` (interactive) instead:

    ssh -F upstream-review/sshconfig laptop 'schtasks /create /tn ae_test /tr "C:\ae\AnimeEffects.exe" /sc once /st 23:59 /it /f'
    ssh -F upstream-review/sshconfig laptop 'schtasks /run /tn ae_test'

If the window still doesn't appear, you double-click it — that's the fallback, not a failure.
Screenshots can be captured from an *interactive* task (PowerShell
`[System.Windows.Forms]`+`CopyFromScreen` → PNG → scp back); from a plain ssh session they come
back black. I'll try it and tell you if it worked.

---

## Prerequisite (DONE): the cross-build script no longer dies

U13 is fixed in the working tree (conditional `data/` copy, verified by a real `--deploy-only` run:
53 DLLs, no `data/` dir, 49 MB zip). See `02-bugs.md` U13.

## Environment finding that shapes the test: Qml/Quick is not cross-buildable here

The MinGW sysroot (`/usr/x86_64-w64-mingw32/sys-root/mingw`) has Qt6 Core, Widgets, Multimedia,
Svg, Xml, Concurrent, OpenGLWidgets and Core5Compat — but **no `Qt6Qml`/`Qt6Quick`, and no
`LinguistTools`**. Upstream's `find_package` does not need them, so **upstream-based builds work**;
our branch needs all three (`CMakeLists.txt:21` lists `Quick Qml QuickWidgets LinguistTools`, and
we added the QML pilot ourselves in phase 1). So **build B (our tree) cannot be cross-compiled here
as configured** — it would need `mingw64-qt6-qtdeclarative` (+ Qt6 `LinguistTools`) installed, which
is a root/network action for you to approve. `build-win/` and `xwin-dist/` are from 2026-08-25,
i.e. before the rename and before the QML pilot, which is why they configured fine then.

---

## Builds — U vs R is the comparison that matters

| build | what | where |
|---|---|---|
| **U** | stock `abb4b88` — DWM branch live on this laptop (build 26200 > 22000) | `C:\ae\u-dist`, task `ae_u3` |
| **R** | `abb4b88` + **our `AppStyle.cpp`/`.h`** (the files the contrib branch supersedes) + the three MinGW patches | `C:\ae\r-dist`, task `ae_r` |
| **F** | `abb4b88` + the nine fix patches | `C:\ae\f-dist`, task `ae_f` |
| **Q** | `abb4b88` with the DWM call deleted and upstream's own Windows-10 Qt-flags line forced everywhere | `C:\ae\q-dist`, task `ae_q` — now **redundant**, kept only as a data point |
| **B** | our branch `c9ec3c3` | not cross-buildable here (below) |

**Why R rather than Q.** `fc5a703`'s DWM code is *not* in the contrib branch: it was added upstream
after our merge-base, while `AppStyle.cpp` on the contrib branch is ours (OR-in
`FramelessWindowHint`, the `QBitmap` mask through `eventFilter`, no Windows API at all). So when the
contrib branch lands upstream, the DWM block is **replaced by our implementation as a consequence of
the merge** — the "revert" is not a separate patch to argue about, it is a conflict resolution plus a
visual judgement. R is that file on top of stock upstream, i.e. what upstream's popups would look
like after the merge. Q only isolates the DWM call by removing it and keeping upstream's fallback
flags, which is a weaker proxy for the same question.

The corner test is therefore **U vs R** — if R's corners are as good as U's, the contrib branch's
styling can stand and the DWM code goes with it; if R is visibly cruder (aliased corners, lost
shadow), the merge should keep the Win11 DWM call and use the Qt-flags path only as the fallback.

All three deployed folders sit side by side under `C:\ae\`, so nothing needs reinstalling to switch.
**Smart App Control will block these unsigned builds** — the script warns about it; it is currently
**off** on the laptop, which is why they run.

### Test-only patch set for the upstream-based builds (U and Q)

Upstream cannot be cross-built on Linux as it stands — three real breakages, all patched **only in
the scratch worktrees** (`.git/dsh-scratch/wt/{u,q}`, 7 files each), never in the tracker:

1. **Casing (U14):** `#include <Windows.h>` → `<windows.h>` in 6 files. The sysroot has only the
   lowercase name; Windows itself is case-insensitive, so this is also the correct upstream fix.
2. **`_declspec` (U3):** → `__declspec` in `src/gui/Main.cpp` (2 lines). This one is not optional —
   it is the compile error.
3. **`QComboBox` (U15):** `GeneralSettingDialog.h`'s `#ifndef Q_OS_WIN` guard removed so the include
   is unconditional; MSVC masks the omission, MinGW does not (`'QComboBox' does not name a type`).

Reproduce with:

    for t in u q; do
      sed -i 's/_declspec(dllexport)/__declspec(dllexport)/' .git/dsh-scratch/wt/$t/src/gui/Main.cpp
      git -C .git/dsh-scratch/wt/$t grep -l '#include <Windows\.h>' -- src | while read f; do
        sed -i 's|#include <Windows\.h>|#include <windows.h>|' .git/dsh-scratch/wt/$t/$f; done
    done
    # plus: drop the `#ifndef Q_OS_WIN` / `#endif` around `#include <QComboBox>` in GeneralSettingDialog.h

Then `cd .git/dsh-scratch/wt/<t>-build && env -u CC -u CXX make AnimeEffects -j10`, and package with
`BUILD_DIR=<abs> OUT_DIR=<abs> ZIP=<abs> scripts/build-windows.sh --deploy-only` (absolute paths —
relative ones break the script's `cd`-then-zip step).

Each of those three is worth reporting upstream on its own; the first two are one-liners, the third
is a two-line guard removal. The four questions to answer are in T1 below.

**Decision rule.** If Q looks as good as U (or better), the DWM call is unnecessary on Win11 and we
revert it — via upstream's own Qt-flags line, after the contrib branch lands (our `4814b5f` +
`7fd0d16` are phase-1 commits, so the Qt-flags mechanism arrives with it). If Q looks crude — aliased
corners or a lost shadow — we take the conservative variant: keep the Win11 DWM call and adopt the
Qt-flags fix only for the `else` fallback. If Q is *borderline*, we build **R** and let the mask
decide.

---

## T1 — DWM corners (the decision the split hinges on) · builds U vs Q

**Setup:** light and dark theme both, but compare in the default **dark** theme. Fresh settings
so both builds start identically (delete `HKCU\Software\AnimeEffectsProject\AnimeEffects`).

**Steps:** open a `QMenu` (right-click the timeline, or the File menu) and a combo drop-down
(Settings → Theme selector).

**What I need you to tell me, per build:**

1. Are the corners **rounded and smooth** (antialiased) or rounded but **jagged/aliased**?
2. Does the corner cut show the app behind it — **not black**, not a solid square of palette colour?
3. Is there a **square drop shadow/border** around the rounded body?
4. Does the drop-down still **close when you click outside** it (Qt::Popup semantics)?

**Decision rule.** If R's corners look crude, or it loses the shadow in a way you dislike, we take
the conservative variant instead: keep the Win11 DWM call and adopt our Qt-flags fix only for the
`else` fallback (strictly safe, but it doesn't delete the DWM hack). If R looks as good or better,
we revert the DWM half upstream — after your contrib branch lands, since `4814b5f`/`7fd0d16`
(our fix) are **phase-1** commits.

---

## T2 — Fresh install gets a family-less font (U4) · build U

**Steps:** with no `generalsettings/ui/font` value present (fresh registry key), launch, then
open Settings → General.

**Expected before the fix:** the UI font is *not* the normal default (measured offscreen here:
family-less 12 pt instead of the platform default), and the **Font field is empty** — because
`qApp->font().family()` is `""`. **After the fix:** the default family is shown and the size is
unchanged.

**Report:** the font field contents, and whether the app's overall text looks off-size next to
build B.

---

## T3 — "Reset recent files list" crashes (U1) · build U

**Steps:** make FFmpeg unavailable (rename `ffmpeg.exe`, or point the setting at a bogus path),
then File → export a video. The General Settings dialog opens on the FFmpeg tab → switch to the
QoL tab → click **Reset recent files list**.

**Expected before the fix:** the "Success" box appears and then the app **dies** (SIGSEGV; on
Windows "AnimeEffects has stopped working"). After the fix: the list resets and the app survives.

---

## T4 — CJK font is not applied (U6) · build U

**Steps:** Settings → Language → 日本語 (or 한국어 / 中文), restart.

**Expected before the fix:** the UI font is **unchanged** — the locale asks for family `NotoJP`
which exists in no shipped font — and nothing warns. If someone "fixes" only the name to
`Noto Serif JP`, expect a **hairline ExtraLight** look instead. **Report:** whether the text looks
like a CJK face at all, and whether it looks too thin.

---

## T5 — Custom easing accepts garbage (U2) · build U

**Steps:** in the property panel, Easing → Custom → the bezier editor → **Copy** (this puts
`x1,y1,x2,y2` on the clipboard). Then put `nan,inf,-inf,1` on the clipboard by hand, right-click a
timeline key → **Paste custom easing**.

**Expected before the fix:** no error; the key's motion breaks (NaN) — values may read `nan` or the
object may stop rendering sensibly. Also try `2,0,-1,1`: the motion visibly **backtracks** instead
of easing forward.

---

## T6 — Browse button loses its styling (U5) · build U

**Steps:** File → New Project → look at the small button next to the file-name field.

**Expected before the fix:** no folder/file glyph (the `#browser` rules live only in
`dialogbutton.ssa`, which the app-wide sheet replaces). After the fix: the icon button looks right
again.

---

## T7 — Untranslated strings (U8/U11) · build U

**Steps:** set a non-English UI language, then (a) close a modified project → the unsaved-changes
dialog shows **Save / Discard** in English while "Cancel" is translated; (b) set the Font field to
`Comic Sans` → the joke dialog shows `SIGILL` and the HAL line in English while its first line is
translated.

---

## Optional: confirming the paint-loop warning spam (U7)

`time_Renderer.cpp:214` calls `setPointSizeF(font.pixelSize() * uiScale)` with `pixelSize() == -1`
on a point font, which makes Qt warn **once per drawn frame label per repaint**. A Windows GUI app
has no console, so to see it either run from a console with
`QT_ASSUME_STDERR_HAS_CONSOLE=1` set, or build a temporary `qInstallMessageHandler` that appends to
a file. Low value visually (the scale simply has no effect there) — skip unless you want the log.

---

## Quick verification pass — three builds, one sitting

Everything is installed on the laptop already, and each build has a launcher task (GUI apps have to
run in your session, so `schtasks /run /tn <name>` from anywhere, or just double-click):

| build | path | launch | what it is |
|---|---|---|---|
| **U** | `C:\ae\u-dist\` | `schtasks /run /tn ae_u3` | stock `abb4b88` — DWM branch live (build 26200 > 22000) |
| **R** | `C:\ae\r-dist\` | `schtasks /run /tn ae_r` | `abb4b88` + our `AppStyle` — **tested in round 8: rounded popup wearing a square/boxy shadow** |
| **F** | `C:\ae\f-dist\` | `schtasks /run /tn ae_f` | `abb4b88` + all nine fixes |
| **Q** | `C:\ae\q-dist\` | `schtasks /run /tn ae_q` | optional: DWM call deleted, upstream's own Qt-flags line used |
| **R2** | `C:\ae\r2-dist\` | `schtasks /run /tn ae_r2` | R with **upstream's popup block verbatim** (DWM rounding >22000; mask/hack removed) — the proposed fix |
| **R3** | `C:\ae\r3-dist\` | `schtasks /run /tn ae_r3` | R's block **plus** the DWM call — can the mask and DWM rounding coexist? |

Do them in this order; ~10 minutes total.

1. **U vs R — the popup-styling question (T1). ANSWERED in round 8: R is the crude one** (rounded
   corners, square/boxy shadow around the popup; U and F look right). Root cause and the follow-up
   builds are in `09-popup-rounding.md`. What is left is the R2/R3 check below.

2. **U — the bugs, then F — the fixes.** Same action in both, and the difference *is* the fix:

   | do this | U (stock) | F (fixed) |
   |---|---|---|
   | Settings → General, fresh profile | Font field **empty**, UI font not the usual default | your font shown, size unchanged |
   | Export a video with FFmpeg missing, then click **Reset recent files list** | **app dies** | list resets, app lives |
   | File → New Project, look at the small browse button | no folder glyph | glyph back |
   | Put `nan,inf,-inf,1` on the clipboard, right-click a key → **Paste custom easing** | accepted, key breaks (NaN) | refused with a message saying what the format is |
   | Open a project with the list already full of 8 recents | the one you just opened isn't listed | it is, at the top |

   Two of these (the crash and the easing values) destroy work if you use a real project — use a
   scratch one, or let me hand you a throwaway `.anie`.

3. **U — the translations (T7), if you care.** Non-English UI: the unsaved-changes dialog shows
   **Save / Discard** in English, and the Comic Sans dialog shows `SIGILL` plus the HAL line in
   English while its first line is translated. Not fixed by the series — the "egg" is yours to
   keep — but it is a two-line job if you want it.

## What I'll need back

Anything you can tell me — even "Q looks the same" — unblocks the DWM decision, which is the one
change that needs your eyes before it goes upstream. For each item: which build, what you saw, and
whether it matches the expectation. For T1 specifically, the four numbered answers — a side-by-side
screenshot of both builds' menus if you feel like taking one, since that single comparison decides
whether the DWM code stays upstream. I won't capture your screen myself.

---

## Round 8 (2026-09-18) — T1 came back, and the answer was ours, not upstream's

**Result of the U/Q/R sitting:** three builds were launched; two looked correct, the third showed
**rounded corners with a square/boxy shadow around the popup**. The owner closed the odd one out and
the PID read back from the laptop mapped it to **R** (`C:\ae\r-dist`) — stock `abb4b88` carrying our
`AppStyle`. So the popup defect is in *our* code, and upstream's DWM path (U, and F which shares it)
is fine on build 26200.

**Root cause, in one line:** our `AppStyle::polish()` rounds the popup client-side
(`Qt::FramelessWindowHint` + an 8px 1-bit `QBitmap` mask → `SetWindowRgn`) and never asks DWM to
round the window frame, which is what Windows 11 draws the shadow for. Upstream's `fc5a703` does the
opposite (>22000: `DwmSetWindowAttribute(… DWMWA_WINDOW_CORNER_PREFERENCE …)`, keeping the frame);
that commit is in `upstream/master` and **not** in our history, which is why our branch still has the
workaround. Full evidence: `09-popup-rounding.md`.

**What to look at now — R2 vs R3 vs U.** Same routine as T1, ~3 minutes: dark theme, open the **File
menu** and the **Settings → Theme** drop-down in each build, and check whether the popup still wears
a square/boxy shadow, and whether the corners are smooth or jaggy (a 1-bit mask gives hard, stepped
corners; DWM's rounding is antialiased). R2 is the proposed fix, so the expectation is "looks like
U"; R3 answers whether our mask can stay (only relevant for pre-22H2 Windows 10, which this laptop
cannot test).

| build | launch | expectation |
|---|---|---|
| **R2** | `schtasks /run /tn ae_r2` | rounded corners, **no** square shadow, smooth edge — i.e. indistinguishable from U's popups |
| **R3** | `schtasks /run /tn ae_r3` | **expected to still look wrong**: Microsoft's guidance says a window region (our mask) and per-pixel alpha layering (frameless + translucent) make a window un-roundable "even if they call the opt-in API". A wrong R3 confirms the rule; a correct R3 would mean the rule does not apply here and the reading in `09-popup-rounding.md` needs revising |
| **R4** | `schtasks /run /tn ae_r4` | Qt-only attempt (`Qt::ExpandedClientAreaHint`, no Win32 call) — open question: Qt documents that flag for title-bar/safe-area, not corners |
| **U** | `schtasks /run /tn ae_u3` | the reference (already confirmed good) |

**Round 8 outcome (measured — this is what decided the fix).** Both of those expectation columns
turned out to be wrong, and the corrected picture is in `09-popup-rounding.md` §"Round 9" and
§"Outcome":

| build | configuration | verdict |
|---|---|---|
| R2 (upstream's block in our style) | translucent + DWM round, no frameless, no mask | **black background, transparency lost** — this is upstream's own state (U/F too) |
| R3 | our mask + frameless + DWM round | QMenu correct; drop-down never checked |
| **R5** | frameless + translucent + DWM round, **no mask** | **both popups correct → this became the fix** |
| R6 | our block, no DWM call (baseline R) | QMenu correct, drop-down has a rectangular shadow |
| R7 | R3 with `DWMWCP_DONOTROUND` | same as R6 — so the corner *value* matters |
| R4 | `Qt::ExpandedClientAreaHint` | popup never appears → Qt has no route |

Applied to the branch: keep frameless + translucent, add the DWM `ROUND` call on 22H2+, install the
region-mask event filter only off Windows. Verified on Linux (full build + suites) and cross-built for
Windows as **R5b**, which is deployed as the only running build and labelled
`[R5b] THE FIX: branch AppStyle` — that is the code in the tree, so it is the one worth a last look.

**Labelling and cleanup, so no PID guessing is ever needed again:** `ae_tag`
(`tag_windows.ps1`) stamps each build's variant name into its native title bar; `ae_probe`
(`probe_windows.ps1`) dumps a popup's `WS_EX_LAYERED` / window-region / corner-preference state;
`ae_watch` (`probe_watch.ps1`) samples popups every 3 s so menus opened by hand get measured without
coordination. Injecting keystrokes to open menus remotely does not work here — probe *after* they have
been opened. Old builds are closed from here (`Stop-Process` over SSH) rather than by the owner.

Both were built, deployed and launched on 2026-09-18: **R2** (`C:\ae\r2-dist`), **R3**
(`C:\ae\r3-dist`), **R4** (`C:\ae\r4-dist`), alongside U and F. If one of them is wrong, closing it
and reading the surviving PIDs back identifies it (the round-8 protocol, which is how R was caught) —
but you should not need that any more, because the windows are labelled:

### Telling the builds apart without PIDs (task `ae_tag`)

Each build's main window carries its variant name in the **native title bar** (and therefore in the
taskbar and Alt-Tab), set from outside the app:

| window title starts with | build |
|---|---|
| `[U]  stock upstream` | `C:\ae\u-dist` — reference, popups correct |
| `[F]  nine fixes` | `C:\ae\f-dist` — popups same as U |
| `[R2] no mask + DWM round` | `C:\ae\r2-dist` — the candidate fix |
| `[R3] mask + DWM round` | `C:\ae\r3-dist` — expected still broken |
| `[R4] Qt ExpandedClientAreaHint only` | `C:\ae\r4-dist` — Qt-only attempt |

Re-apply any time with `schtasks /run /tn ae_tag` (script: `.git/dsh-scratch/tag_windows.ps1`,
deployed to `C:\Users\pokus\`, result written to `C:\Users\pokus\tag_result.txt`). The app resets the
caption if it ever calls `setWindowTitle` (opening a project, for instance), so re-run the task if a
label goes missing.

Three things this cost, worth not rediscovering: the enumerating script **must run in your desktop
session** — from SSH, `Get-Process … MainWindowHandle` is 0 and `EnumWindows` finds nothing, because
windows belong to the interactive session (hence the scheduled task with `InteractiveToken`); the
deployment is identified by the **parent directory** of the running exe, not the exe name (my first
attempt keyed on the latter and labelled all five windows `AnimeEffects.exe`); and `GW_OWNER` is 4,
not 3. Qt's window class here is `Qt6111QWindowOwnDCIcon` (Qt 6.11.1), and only top-level, visible,
captioned, owner-less windows are touched — Qt's dialogs and menus have an owner, and its hidden
helper windows (`_q_titlebar`, IME windows, …) have no caption.
