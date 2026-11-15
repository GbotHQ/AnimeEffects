# Upstream fix series — 2026-09-18

Nine patches against `abb4b88`, one per defect, in `.git/dsh-scratch/upstream-fixes/`. They are
exported from the scratch branch `upstream-fixes` (worktree `.git/dsh-scratch/wt/fix`) and
**nothing has been pushed or sent anywhere**.

**Verified:** each patch applies to a pristine `abb4b88`, and applying all nine in order
reproduces the fix branch's tree **exactly** (both trees hash to `650c17e0`). Compile verification
is separate and noted per item.

| # | patch | bug | why it is here |
|---|---|---|---|
| 1 | `0001-build-spell-the-Windows-header…` | U14 | Without it **no MinGW cross-build works at all** — the six `<Windows.h>` includes cannot resolve on Linux. |
| 2 | `0002-gui-include-QComboBox-unconditionally` | U15 | Same: upstream master does not compile on Windows/MinGW as it stands. |
| 3 | `0003-gui-use-__declspec…` | U3 | The third Windows-build blocker, and the one my own build hit first. |
| 4 | `0004-locale-name-the-CJK-fonts…` | U6 | The shipped CJK fonts are unreachable by the names the preferences ask for. |
| 5 | `0005-gui-fix-the-recent-projects-list…` | U1 | Four defects in one path: the reachable SIGSEGV, the eight leaked QActions per refresh, the startup hang on a >8-entry list, and the just-opened project never being recorded. |
| 6 | `0006-gui-stop-replacing-the-default-font…` | U4 | `hasFamily("")` is true, so every default install lost its font *and* its size. |
| 7 | `0007-gui-leave-the-application-wide-stylesheet…` | U5 | The app-wide sheet **replaced** `dialogbutton.ssa`, un-styling `QPushButton#browser`. |
| 8 | `0008-gui-validate-the-values-pasted-as-a-custom-easing` | U2a | NaN/inf and out-of-range x reached the solver; the error text was untranslated. |
| 9 | `0009-core-blend-custom-easing-by-weight…` | U2b | The weight scaled the curve's domain by (2−w) instead of blending; weight 1 is unchanged. |

## Deliberately NOT in the series

- **The `-Wall` pair** (`140b779` enable, `3122528` delete-dead-code). They conflict in
  `tests/CMakeLists.txt` because they were authored on top of the exporter test targets. Upstream
  enables **no warning flags at all**, so "turn on `-Wall -Wextra` and delete what it finds" is a
  separate conversation with its own review; bundling it here would bury the bug fixes.
- **U7** (the truncated `static_cast<int>(fontScale)`, the unitless `font-size`, the ignored `ok`,
  the point/pixel mix in `time_Renderer`) — not in the agreed scope. The pixel-branch truncation is
  the most user-visible of them and makes a good next patch.
- **U8** (the Comic Sans egg and its `throw` control flow) — the owner considers the egg fine; the
  only real defects are the untranslated strings and the reliance on main's catch-all.
- **U10** (the DWM revert) — waits for the Windows A/B result.
- **U2b's sibling**: the clipboard key copy/paste that drops the bezier (and the `"eRange"` vs
  `"aRange"` mismatch) is adjacent to patch 9 but is a different code path; left for a follow-up.

## Verified behaviour, not just compilation

Patch 9 was checked against a probe that links the **real** evaluator, upstream versus patched, at
`b=0 c=1 d=1` (how `TimeKeyBlender` calls it), with an identity bezier:

| weight | upstream | patched |
|---|---|---|
| 1.0 (the default) | `0.0138 0.0677 0.2011 0.5000 0.7989 0.9323` | **bit-identical** |
| 0.5 | `0.1875 0.3750 0.5625 0.7500 0.9375 **0.5343 1.3192 0.9066**` | `0.1250 0.2500 0.3750 0.5000 0.6250 0.7500 0.8750` |

Upstream overshoots past 1 and then runs backwards; the patched values stay monotonic in [0,1]. The
default weight is unchanged to the last digit, which is what the patch message claims. A sweep of
every probe row confirms the patched evaluator is in range and non-decreasing wherever the input is
sane — and the two rows that remain out of range are exactly the inputs patch 8 now refuses
(`2,0,-1,1` and `nan,inf,-inf,1`), i.e. the two patches cover each other's gap:
**8 stops bad values entering, 9 stops good values being mis-evaluated.**

## How to send them

    # review
    git log --oneline abb4b88..upstream-fixes
    # send (when you decide to)
    git send-email --to=… .git/dsh-scratch/upstream-fixes/*.patch
    # or open a PR from a fork branch carrying them

Patches 1–3 are what make a MinGW build possible; if you would rather keep the upstream diff
minimal, 1–2 are the ones an upstream maintainer is most likely to accept without discussion,
since they are pure portability with no behaviour change.
