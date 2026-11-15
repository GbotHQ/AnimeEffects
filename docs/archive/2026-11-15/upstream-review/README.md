# Upstream review — 2026-09-18 (verified)

Working memory for the review of p-yukusai's 10 commits that `upstream/master` carries and
`feat/export-ui-redesign` does not. Written to be read **without** the chat session that
produced it. Every claim here was verified by a separate agent against the repo, the Qt
6.11.1 headers/docs/runtime, or a real compiler; the raw evidence lives in
`.git/dsh-scratch/s1…s10/` (index in `04-artifacts.md`).

**What this is not:** these are *their* commits, not present in your tree. `56b04b2`,
`09c75b3`, `779cbda`, `dbd319a`, `fc5a703`, `133fb51`, `46c4f25`, `25501ba`, `abb4b88` are
all ancestors of `upstream/master` (abb4b88) and **none** are ancestors of your HEAD
(`c9ec3c3`). HEAD is 172 ahead / 10 behind, merge-base `832195d`.

## The files

| file | what it holds |
|---|---|
| `01-commit-review.md` | Each of the 10 commits: what it does, what is wrong, verdict, confidence |
| `02-bugs.md` | The 12 findings as actionable specs — location, evidence, fix, where it should land |
| `03-windows-test-plan.md` | Test procedures for the laptop (DellikM, Win11 Pro build 26200), what to look for |
| `04-artifacts.md` | Index of every verification artifact, and how to re-run each probe |
| `05-split-and-backup.md` | Exporter-split status, conflict map, backup/restore state |
| `06-fix-series.md` | The nine upstream fix patches: what each fixes, what is deliberately excluded |
| `07-warnings-series.md` | The warning-clean second contrib branch, and the upstream bug the warning policy found |
| `08-sending.md` | The three artefacts to send, paste-ready cover letters, send order, pre-send checklist |
| `09-popup-rounding.md` | Why build R showed a square shadow around its rounded popups, and the R2/R3 fix test |

`SCRATCHPAD.md` §5 carries the decision-level summary and points here.

## Status at a glance

- **Repo state: clean.** `feat/export-ui-redesign` @ `c9ec3c3`, same 14 branches, same 4
  worktrees, `git status --porcelain` empty — except for the one uncommitted fix below.
  Nothing else in the tracked tree was modified by this review; all writes are in
  `.git/dsh-scratch/`, `.git/backups/`, and this directory (excluded via `.git/info/exclude`).
- **Backups: verified.** `.git/backups/animeeffects-full-20260918.bundle` (44 MB, complete
  history) + 19 `refs/backup/*` refs, restore-tested. Not yet copied off this machine.
- **Prepared but unapplied:** `.git/dsh-scratch/s9/fix-46c4f25.patch` (applies to
  `abb4b88`, not to your tree — your paths were renamed).
- **Nothing was pushed anywhere, and no upstream PR was opened.**

### Round 1 (2026-09-18): U13 fixed, and the DWM test builds exist

- **U13 fixed and verified** in the working tree (uncommitted): `scripts/build-windows.sh` now
  copies `data/` only when the tree has one. Verified by a real `--deploy-only` run — 53 DLLs,
  no `data/` dir, 49 MB zip; the old line aborted there.
- **Builds U and Q cross-compiled** (`abb4b88`, our MinGW toolchain) and deployed to the laptop:
  `C:\ae\u-dist` (stock, DWM branch live) and `C:\ae\q-dist` (upstream's own Qt-flags path forced
  on every Windows version). U is running there now.
- **Three upstream Windows-build breakages found by doing it** — all real compile failures, all
  patched *only* in the scratch worktrees: `Windows.h` casing (U14), `_declspec` (U3, `Main.cpp:27`
  — this was previously only a portability note, now a demonstrated error), and
  `GeneralSettingDialog.h`'s `#ifndef Q_OS_WIN` around `#include <QComboBox>` (U15). Upstream
  cannot be built with MinGW as it stands; MSVC hides all three.
- **Our own build (B) is not cross-buildable here**: the MinGW sysroot lacks Qt6 `Qml`/`Quick`
  (we added the QML pilot in phase 1) and `LinguistTools`. Installing those is a root/network
  action, so it needs your approval.
- Laptop access notes: `ssh -F upstream-review/sshconfig laptop`; GUI apps must be launched via a
  **scheduled task created from XML** with `InteractiveToken` — `schtasks /tr "…" /it` silently
  does nothing here (the trivial probe task ran with result 0 and wrote no file), which cost most
  of an hour to pin down.

### Round 3 (2026-09-18): the contrib branch is verified end to end

- **`contrib/upstream-2026-09` @ `db96b95`** — 72 commits over `832195d`. Builds clean with the
  project's own generator (`Ninja Multi-Config`, 375/375, 0 errors, 0 first-party warnings); 10 of
  12 test binaries pass, the other two take arguments (as the project's own runner notes); no
  exporter files, no conflict markers, no dangling includes. Tip rewritten once to drop a comment
  naming a harness the series does not contain (`backup/contrib-before-comment-fix` is the safety
  ref).
- **Nine fix patches** (U14, U15, U3, U6, U1, U4, U5, U2a, U2b) in `.git/dsh-scratch/upstream-fixes/`;
  applying all nine to a pristine `abb4b88` reproduces the fix tree exactly.
- **The fixed tree is cross-built for Windows** so the fixes can be *seen* working on the laptop
  (build **F**, `C:\ae\f-dist`, task `ae_f`) — all nine patches compile with the MinGW toolchain and
  F launches and stays resident. Stock **U** (`ae_u3`) and the Qt-flags variant **Q** (`ae_q`) are
  beside it; the ten-minute check is at the end of `03-windows-test-plan.md`.
- Two traps recorded: `qt_add_qml_module` makes a directory named after the target (single-config
  generators cannot link on Linux), and a cherry-pick loop that resolves only *some* conflicts will
  happily commit markers — the build is part of the verification, not a formality.

### Round 5 (2026-09-18): U13 committed into both lines; the DWM decision is subsumed

- **U13 committed** as `205711a` on `feat/export-ui-redesign`, and added to the contrib recipe
  (`CONTRIB_EXTRA`) — the branch had been carrying the *stale* line, because it was built from
  committed history and the fix had only ever been a working-tree change.
- **Contrib branch rebuilt: 74 commits, tip `b459f79`**, rebuild-stable (the round-3 comment fix is
  now a recipe step rather than a one-off amend that the next rebuild reverted).
- **The DWM question is largely answered by the merge itself:** `fc5a703`'s DWM code is not in the
  contrib branch, whose `AppStyle.cpp` is ours. So the corner test is **U vs R**, where R = upstream +
  our `AppStyle` + the MinGW patches — the code a merge would actually leave in place. Q is demoted to
  a secondary data point.

### Round 6 (2026-09-18): the split is now complete, and warning-clean

- **`contrib/upstream-2026-09-warnings`** holds the four commits the first branch had left out, plus
  the vendored-SYSTEM-include fix (`a22c964`, which my subject-based curation had wrongly excluded)
  and one **real upstream bug** the warning policy found: `ExportParams::update()`'s
  `frameTick += 1` moved a local pointer instead of the caller's counter, so the legacy export dialog
  reported "Current frame: 0/N" for the whole export. **Six commits; builds 375/375 with 0 errors and
  0 warnings; suites pass.**
- **Split coverage audited properly** (by subject — cherry-picks change SHAs, which is what made my
  first audit wrong): all 61 phase-1 commits are in the split, and every phase-2 commit left out is
  exporter work. Nothing non-exporter remains.
- **Two lessons recorded:** a clean cherry-pick produced a broken tree (the dead-code commit removed
  exported-class members that are live in the *legacy* exporter — found only by building), and
  `git grep` without `-I` cries wolf on binary fixtures.
- Patch 9's behaviour is now verified, not just compiled: upstream at weight 0.5 overshoots to 1.3192
  and runs backwards; the patched evaluator stays monotonic in [0,1]; weight 1 is bit-identical.

### Round 7 (2026-09-18): the package is ready to send

- **`08-sending.md`** — the three artefacts (bug-fix series, the two contrib branches, the `46c4f25`
  cleanup patch), what each is for, paste-ready cover letters, the send order, and a pre-send
  checklist. Nothing pushed; this is the file to read when you decide.
- **The `46c4f25` cleanup cannot be folded into our branches** — our tree already supersedes the code
  it fixes (own CMake, stylesheet renamed to `assets/stylesheets/`), so it stays an upstream-only
  patch against `abb4b88`. Verified: it applies cleanly there, and all nine fix patches still apply in
  order.
- **The pre-merge conflict set is now measured, not guessed: 23 files.** Twelve of them are a single
  *decision* (the font/locale policy: four static Geist weights vs a variable Geist plus ~48 MB of
  CJK, and whether the `preference_*.txt` mechanism returns), and eleven are mechanical-but-read.
  That is why the pre-merge waits for you; the branches display correctly without it, since their base
  `832195d` is an ancestor of current master.

### Round 8 (2026-09-18): the popup defect is ours, not upstream's

- **T1 answered.** Of the three builds on the laptop (U stock, F fixes, R ours), U and F look correct
  and **R's popup shows rounded corners inside a square/boxy shadow** — mapped to R by closing the
  window and reading the PID back, not by assumption.
- **Root cause:** our `AppStyle::polish()` rounds the popup client-side (`Qt::FramelessWindowHint` +
  an 8px 1-bit `QBitmap` mask → `SetWindowRgn`) and never asks DWM to round the **window frame**,
  which is what Windows 11 draws the shadow for. Upstream's `fc5a703` does exactly that call, and it
  is in `upstream/master` but **not** in our history — our branch forked before it, and `4814b5f`
  re-invented the region trick. Full evidence: `09-popup-rounding.md`.
- **Early reading, later refuted:** the answer is not "revert `fc5a703`", and it also turned out **not**
  to be "port upstream's block and delete our workaround" — measurement showed upstream's block is the
  broken one on our theme. See the outcome at the end of this section.
- **Two test builds built and deployed** to settle it: **R2** (`C:\ae\r2-dist`, task `ae_r2`) =
  upstream's popup block verbatim; **R3** (`ae_r3`) = our block plus the DWM call, to see whether the
  mask can coexist with DWM rounding. `03-windows-test-plan.md` §"Round 8" has what to look for.
- **Recorded as unverified at the time:** the mechanism was a hypothesis until the builds reported;
  `Qt::NoDropShadowWindowHint` (upstream's pre-22H2 flag) is not consulted anywhere in the Windows QPA
  sources fetched (qtbase 6.9/6.11), so that path works by dropping the frame if at all.

**Outcome (same day, measured — supersedes the reading above).** The builds were labelled in their
title bars and run with a Win32 probe, and the answers were the opposite of the early guess:

- **U and F are the broken ones for our theme**: their popup is neither layered nor region-masked, so
  Qt drops the alpha channel and the rounded panel sits on black corners; and their corner preference
  is set on a popup with **no frame styles**, so DWM has nothing to round. `R2` (their block in our
  style) reproduces it. That is a reportable upstream bug in `fc5a703`.
- **Our frameless/translucent block is the part that must stay** — it is what makes Qt layer the popup
  (`QWindowsWindow::setWindowLayered`: `hasAlpha && hasNoNativeFrame`) and therefore what makes the
  transparency real.
- **The fix is one call:** `DwmSetWindowAttribute(… DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND…)` on
  Windows 11 22H2+, which rounds the *window* so the system shadow matches the 8px panel. With it, the
  mask is unnecessary on Windows (verified in **R5**: frameless + translucent + DWM round, no mask —
  both the menu and the combo drop-down correct). Without it, or with `DWMWCP_DONOTROUND` (R7), the
  un-masked combo drop-down keeps a rectangular shadow — the owner's "QMenu is correct, but the
  drop-down has non-rounded shadow".
- **Qt has no route of its own:** `Qt::ExpandedClientAreaHint` on a `QMenu` (R4) makes the popup never
  appear, and Qt never calls the corner-preference API itself.
- **Applied to `feat/export-ui-redesign`** (uncommitted): `AppStyle::polish()` keeps
  frameless+translucent, adds the DWM call, and installs the region-mask event filter **only off
  Windows**. Full detail, measurements and the open items (pre-22H2, our own two popovers, the upstream
  report) are in `09-popup-rounding.md`.

## Decisions pending (need you)

1. `46c4f25` patch → upstream PR, or drop it? (Your branch already lacks all six items.)
2. **DWM: settled and measured — neither revert nor "adopt upstream's block".** Keep our
   frameless + translucent popup (that is what makes the transparency real), add the
   `DWMWCP_ROUND` call on Windows 11 22H2+, and drop the region mask **on Windows only** (a window
   region is what makes a window un-roundable for DWM, so the two levers are mutually exclusive); the
   mask stays on other platforms. Applied to the branch, uncommitted. Two sub-questions are yours:
   (a) the **pre-22H2 (Windows 10) picture** cannot be tested on this laptop — accept the current
   "no system shadow there" behaviour, or try to do better blind? and (b) whether to **fix our own two
   popovers** (`EasingPickWidget`, `AnimationSettingDialog`) the same way — they mask themselves, so
   they should show the same rectangular shadow on Windows 11, but neither can be built for Windows
   here until the MinGW sysroot gains Qt6 `Qml`/`Quick` and `LinguistTools` (a root install).
3. Do the verified bugs go upstream as issues/PRs at all? All of them are in p-yukusai's own
   commits, so framing matters. My suggested order is in `02-bugs.md` §"Landing order".
4. Push the backup refs to `origin` as the off-machine copy?
   `GIT_SSH_COMMAND='ssh -F upstream-review/sshconfig' git push origin refs/backup/snapshot-20260918/feat/export-ui-redesign:refs/heads/backup/export-ui-redesign-20260918`

## Environment notes that cost time to discover

- **`ssh` is broken for non-interactive use on this machine.** `/etc/ssh/ssh_config` has
  `Include /etc/ssh/ssh_config.d/*.conf`, and `20-systemd-ssh-proxy.conf` is a symlink owned
  by `nobody`; OpenSSH 10.2 aborts on it (exit 255) before resolving the host. Your keys are
  fine — `ssh -F /dev/null -T git@github.com` authenticates as GbotHQ.
  Workaround used everywhere here: `-F upstream-review/sshconfig`, a two-word file
  containing `Include ~/.ssh/config`. Git needs it too:
  `GIT_SSH_COMMAND='ssh -F upstream-review/sshconfig' git fetch …`.
- **`/tmp` is wiped between shell calls**, so any extract-then-inspect must happen inside one
  command. All probes were written accordingly.
- `fontTools` is **not** installed; the font evidence came from a hand-written sfnt parser
  plus Qt probes (see `04-artifacts.md`).
