# Split plan — exporter vs. everything else (2026-09-18)

Goal: contribute the non-exporter work upstream (p-yukusai/AnimeEffects) while keeping the
new exporter local, without polluting either line with the other.

## Measured facts

Source branch: `feat/export-ui-redesign` @ `c9ec3c3` = `upstream/master` merge-base `832195d`
+ 172 linear commits (single author, no merges), never pushed.

Two clean phases, split at the first exporter commit `3e114c2`
("feat(media): link FFmpeg as a library and add a V0 capability probe", 2026-08-27):

| phase | range | commits | content |
|---|---|---|---|
| 1 | `832195d..3e114c2^` | 61 | timeline / prop / core frame-range / gui / build+asset+i18n infra |
| 2 | `3e114c2..HEAD` | 111 | exporter era (media engine, destinations, export UI, plus embedded cleanups) |

Trials (scratch worktrees, all deleted afterwards; results in `.git/dsh-scratch/*.tsv`):

* Phase 1 replayed onto its own base: **61/61 clean, tip tree hash identical** to `3e114c2^`.
* Contrib candidate (phase 1 + 13 non-exporter phase-2 commits + 2 surgically stripped
  `-Wall` commits): **75 commits, 73 clean, 1 conflict** (`b093680`, `scripts/run_suites.sh`),
  3 exporter paths stripped (`src/media/CMakeLists.txt`, `src/ctrl/Exporter.cpp/h`).
  Candidate tree contains **no** `src/media/`, `assets/export/`, `ExportFacts.cpp`,
  `DestinationPicker.cpp` — only the pre-existing legacy `src/gui/exportdiag.h`.
* Phase 1 replayed onto today's `upstream/master` (9 commits ahead): 47 conflict stops.
  The same divergence as one **merge** = 23 conflicted files (9 font/locale binaries +
  11 code files: `src/core/Project.h`, `src/ctrl/TimeLineEditor.h`,
  `src/ctrl/time/time_Renderer.cpp`, `src/gui/AppStyle.cpp`, `src/gui/CMakeLists.txt`,
  `src/gui/GeneralSettingDialog.{cpp,h}`, `src/gui/Main.cpp`, `src/gui/MainMenuBar.cpp`,
  `src/gui/TimeLineEditorWidget.cpp` + 3 `preference*.txt`/`.ssa`).

Dependency direction (exporter → phase 1), verified:

* `ExportTargets::defaultPath()` returns `:/export/ExportTargets.txt`; that resource is
  registered in `assets/assets.qrc`, a file **created by phase 1** (`7762479`, +105 lines).
* `src/media/` does not exist before `3e114c2`.
* i18n catalogs come from phase 1's `qt_add_translations` (`579c0a2`).

So the exporter must stay stacked on the contrib branch; it cannot sit on upstream alone.

## Branch layout

    feat/export-ui-redesign      <- stays EXACTLY as it is (local only, no rewrite)
    contrib/<name>               <- new: phase 1 + non-exporter phase-2 commits, pushed to fork
    (optional later) two PR branches cherry-picked from contrib/<name>:
        build/asset/i18n infra   +   editor: core frame range, timeline, prop, gui

## Procedure (reproducible)

0. `git fetch upstream` first (currently BLOCKED, see below).
1. Rebuild the contrib candidate — `.git/dsh-scratch/contrib_candidate.sh` does exactly this
   (75 commits; deterministic; runs in ~1 minute).
2. Resolve the single conflict: `b093680` (`scripts/run_suites.sh` edits exporter suite lines)
   — either hand-edit to drop the exporter suites, or leave that commit with the exporter.
3. Bring contrib up to today's `upstream/master`: **merge** (23 files, one pass) preferred over
   **rebase** (47 stops). If rebasing, enable `git rerere` first — most stops repeat.
4. Verify: build + run suites on contrib in its own build dir; confirm the app's export
   dialog is the pre-existing legacy one.
5. Push `contrib/<name>` to `origin` and open the PR against `p-yukusai/AnimeEffects`.

Not in scope: rewriting `feat/export-ui-redesign`. After upstream merges the contrib work,
rebase the exporter onto the new upstream/master — that rewrite touches only local history.

## Blocked / open

* **SSH is broken on this machine**: `Bad owner or permissions on
  /etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf` (symlink owned by `nobody`), so
  `git fetch upstream` and any push to `origin` fail. Fix that before step 5, and before
  copying the backup off-machine.
* Decide: one contrib branch or two topic PRs (two = two conflict passes, smaller reviews).
* Decide: merge vs rebase for the upstream advance.
* `06b1ce4` ("one temp dir per test process, and a lock around the GL suite") is mixed
  (touches export tests); needs the same hunk surgery if it is to be contributed.
