# Split + backup status — 2026-09-18

## Backup (done, verified)

- `.git/backups/animeeffects-full-20260918.bundle` — 44 MB, "records a complete history":
  every local branch, remotes, tags, `refs/stash`, all `refs/backup/*`, each worktree HEAD.
- `refs/backup/snapshot-20260918/*` — 18 frozen refs (every branch tip + each worktree HEAD).
- `refs/backup/bugfix-worktree-20260918` (`bca629c`, tree `b6aeed34`) — the dirty `bugfix`
  worktree captured through a throwaway index, including the untracked `tests/support/`, so the
  worktree itself was never touched (it was 50 changed paths before and after).
- Restore-tested: test clone reproduced the tip tree hash, all 172 branch-only commits, the
  stash, and the worktree snapshot. Details: `.git/backups/BACKUP-NOTES-20260918.md`.
- **Not off-machine yet.** The bundle lives inside `.git/`, so `git clean` can't take it but a
  fresh clone would. Push a copy:
  `GIT_SSH_COMMAND='ssh -F upstream-review/sshconfig' git push origin refs/backup/snapshot-20260918/feat/export-ui-redesign:refs/heads/backup/export-ui-redesign-20260918`

## The split

`feat/export-ui-redesign` @ `c9ec3c3` = `832195d` + **172 linear commits** (one author, no
merges), **never pushed**. Upstream is now `abb4b88` (10 ahead of your base).

| phase | range | count | content |
|---|---|---|---|
| 1 | `832195d..3e114c2^` | 61 | timeline / prop / core frame-range / gui / build+asset+i18n infra |
| 2 | `3e114c2..HEAD` | 111 | exporter era, with non-exporter work interleaved |

First exporter commit: `3e114c2` "feat(media): link FFmpeg as a library and add a V0 capability
probe" (2026-08-27). **`src/media/` does not exist before it**, and the exporter reads
`:/export/ExportTargets.txt` — a resource registered in `assets/assets.qrc`, **created by phase 1**
(`7762479`). So the exporter must stay stacked on the contrib branch; it cannot sit on upstream alone.

**Trials (all reproducible, see `04-artifacts.md`):**
- phase 1 replayed onto its own base: **61/61 clean, tip tree hash identical**.
- contrib candidate = 61 + 2 surgically stripped `-Wall` commits + 13 non-exporter phase-2
  commits = **75 commits, 73 clean, 1 conflict** (`b093680` / `scripts/run_suites.sh`), 3 exporter
  paths stripped (`src/media/CMakeLists.txt`, `src/ctrl/Exporter.{cpp,h}`). Its tree contains no
  `src/media/`, no `assets/export/`, no `ExportFacts.cpp`/`DestinationPicker.cpp`.
- **The real cost is the upstream advance — measured, not estimated.** A dry-run merge of
  `upstream/master` into the contrib line conflicts in **23 files**
  (`.git/dsh-scratch/premerge-conflicts.txt`), in two groups:
  - **12 files are one decision, not twelve problems — the font/locale policy.** `assets/fonts/Geist-{Bold,Medium,Regular,SemiBold}.ttf`
    (ours: four static weights) vs `assets/fonts/{Geist,NotoCN,NotoJP,NotoKO}.ttf` (upstream's:
    variable Geist plus ~48 MB of CJK), plus `assets/locale/preference_ko.txt` against
    `data/locale/preference{,_ja,_zh}.txt` (the layout our phase 1 renamed away). Resolving them means
    choosing a font set, deciding whether to embed 48 MB of CJK, and deciding whether the
    `preference_*.txt` mechanism returns — after which `assets/assets.qrc` must list what is kept.
    That is a design call, so it waits for you; note this cluster is also where the CJK family-name bug
    lives (fix-series patch 4).
  - **11 files are mechanical but must be read, not rubber-stamped**: `assets/stylesheets/standard.ssa`
    (comment wording, plus the duplicate rule the cleanup patch removes) and eight code files where
    upstream's `09c75b3`, `779cbda` and `56b04b2` churned functions we rewrote —
    `src/core/Project.h`, `src/ctrl/TimeLineEditor.h`, `src/ctrl/time/time_Renderer.cpp`,
    `src/gui/AppStyle.cpp`, `src/gui/CMakeLists.txt`, `src/gui/GeneralSettingDialog.{cpp,h}`,
    `src/gui/Main.cpp`, `src/gui/MainMenuBar.cpp`, `src/gui/TimeLineEditorWidget.cpp`. "Take ours" is
    right for most of them (our versions are the ones this series verified), but `AppStyle.cpp`
    carries the DWM decision and `GeneralSettingDialog.*` the recents/stylesheet fixes.
  Rebasing instead of merging costs **47 conflict stops** for the same divergence; enable `git rerere`
  if you go that way.

**Recommendation:** leave `feat/export-ui-redesign` exactly as it is (zero exporter rewrite); build
`contrib/<name>` from the scripted recipe; push that upstream; rebase the exporter later, once
upstream has the contrib work.

## `contrib/upstream-2026-09` — BUILT and verified (2026-09-18; rebuilt in rounds 3 and 5)

**74 commits** over `832195d` (61 phase-1 + 11 non-exporter phase-2 + the U13 fix + one hygiene
commit), tip **`b459f79`**, produced by `.git/dsh-scratch/build_contrib.sh` — deterministic, so it can
be rebuilt from scratch at any time.

**Verified:**
- The new exporter is absent: `src/media/`, `assets/export/`, `ExportTarget`, `ExportFacts`,
  `DestinationPicker` — zero files each. No CMake reference to the media module, no dangling
  includes of the excluded headers, and no reference to the deleted exporter tests.
- **No conflict markers** anywhere (that check exists because the first attempt *did* commit them —
  see the recipe note below).
- **Carries the U13 fix** (`scripts/build-windows.sh:75` copies `data/` only when the tree has one).
  This mattered: built from committed history, the branch had inherited the *stale* `cp -r data/` and
  would have proposed a cross-build script that fails on its own tree.
- **Builds clean** with the project's own generator: `-G "Ninja Multi-Config"`, 375/375 targets,
  exit 0, zero errors, zero first-party warnings.
- **Tests: 10 of 12 binaries pass** (`anim_setting_i18n`, `anim_settings_undo`,
  `attribute_observable`, `bonekey_regression`, `creation_select_keys`, `easing_picker_shot`,
  `easing_regression`, `picker_pos_probe`, `picker_probe`, `rightclick_transient_selection`).
  The other two (`roundtrip`, `easing_picker_layout`) take arguments — a `.anie` file and a golden
  JSON respectively — which is exactly what the project's own `scripts/run_suites.sh` annotates
  ("needs .anie arguments"), so this is invocation, not regression.
- Rebuild-stable: the round-3 comment fix is now a recipe step (its own commit) rather than a one-off
  `--amend` that the next rebuild silently reverted.

**Known limitations of the series** (deliberate, decide later):
- `scripts/run_suites.sh` is **not** included: it comes from `2765cf2` ("Track the GUI probes…"),
  which is entangled with the export-probe scripts. So the branch ships test binaries without their
  runner — a reviewer would run them by hand, as documented above.
- The golden JSON for `easing_picker_layout` and the `.anie` fixtures for `roundtrip` come from
  exporter-era commits (`a7acb64`), so those two tests cannot run on this branch as it stands. Both
  have a `--write-golden` mode, so the gap is recoverable without taking exporter code.
- The `-Wall` pair and, with them, `ddf9c10`/`b093680` (picker-probe commits) are excluded; they
  belong in a separate "warning flags + the dead code they found" series.

## Recipe note — the mistake worth not repeating

The first build of this branch committed **conflict markers** into `tests/CMakeLists.txt`. Cause: the
surgery step cherry-picked the `-Wall` commits, resolved the *exporter* paths by hand, and then
`git add -A`'d whatever else was still unmerged. The configure died at `<<<<<<<` and it was only
caught because the build was run. `build_contrib.sh` now aborts on any leftover unmerged path and
refuses to commit a tree containing markers — and the build is part of the verification, not an
afterthought.

## Conflict map (what collides, and why)

- **`data/` vs `assets/`.** Phase 1 renamed the source dir (`34083a8`) and then made the app
  self-contained by embedding assets in qrc (`7762479`, `913a387`, `922beef`, `dd511a8`). Upstream
  still resolves `appDir + "/data"` at runtime (`Main.cpp:164`) and still ships `data/`. Every
  upstream font/locale/stylesheet change therefore lands in a directory that doesn't exist on your
  side, and your asset manifest (`assets/assets.qrc`) lists the four `Geist-*.ttf` that upstream
  deleted, while missing the three Noto files it added.
- **Locale fonts have no counterpart here.** Upstream's `preference_*.txt` mechanism
  (`LocaleDecider.cpp:67-69`) and its Noto fonts do not exist on your branch at all — you keep
  locale font defaults in QSettings (`Main.cpp:169-170`).
- **`standard.ssa`.** Same sheet, different comment wording on the `QWidget:disabled` vs `QMenu`
  block, and upstream adds a duplicate `QMenu` rule your copy doesn't have → rename+modify conflict.
- **`AppStyle.cpp` / `MainWindow.cpp` / `MainTimeLine…`** — upstream's `09c75b3`/`779cbda`/`56b04b2`
  churn the same functions your phase-1 work rewrote (`onThemeUpdated`, `polish`, the timeline
  context menu).

## Cross-branch interactions worth knowing before you choose a fix location

- **The DWM revert's prerequisite is already yours.** `4814b5f` (+ `7fd0d16`) — the Qt window-flags
  fix and the mask-scope refinement — are **phase 1** commits. So "port our fix upstream" is not a
  separate port: it arrives when the contrib branch lands, and the DWM revert becomes a follow-up
  PR. (Caveat and test in `02-bugs.md` U10 / `03-windows-test-plan.md` T1.)
- **The custom-easing feature and your picker are complementary.** Upstream's producer
  (`splineWidget.h`) is deleted by your `68d5998`, but `src/gui/prop/EasingPickWidget.cpp:801-825`
  writes and parses the **identical** `x1,y1,x2,y2` format with per-field `ok` checks. Reuse that
  validation rather than inventing one — and note that deleting the dead dialog does **not** strand
  the feature.
- **`-Wall -Wextra` is ours, not upstream's** (`ae_enable_warnings()`, `CMakeLists.txt:48-52`). The
  phase-2 cleanup commits that assume those flags will read differently to an upstream reviewer who
  has none.
- **`_declspec` breaks *our* Windows build, not theirs** (U3). Upstream's CI is MSVC; ours is the
  MinGW cross-build (`scripts/build-windows.sh`, ours). Expect a one-line fix on our side after the
  merge unless it lands upstream first.
- **U13 (ours):** `scripts/build-windows.sh:69` copies `$SRC_DIR/data`, which no longer exists after
  the phase-1 rename, under `set -euo pipefail` → the cross-build **dies mid-deploy**. The line is
  also obsolete (fonts `:/fonts`, translations `:/i18n`). Delete it and the `data/` mentions in the
  header/echo. This blocks the Windows tests for build **B**.
- **U12 (open, ours):** no `qt_finalize_executable` while upstream has one; nil effect on
  Linux/clang, material on Windows/Android/WASM/Apple.

## Repo hygiene still to do (separate from the split)

- Local `master` (`322a523`) = stale `origin/master` (`0c6ce51`) + 4 blur commits, of which
  `git cherry` shows 3 are patch-equivalent to commits already in HEAD — a duplicate line of history.
- Four `backup/*` branches, one stale stash (`refs/stash`, "folder-opacity verification temp cases"),
  and three parallel worktrees (`bugfix` 2 ahead/85 behind, `docking-ui` 41/172, `mipmapping` 43/31).
  `bugfix` now has a snapshot ref, so its 50 dirty paths are finally safe.
- All of the above is *frozen* in the bundle; nothing here needs preserving before cleanup.
