# Upstream contribution — plan and status (2026-11-15)

Fork-side plan for contributing the accumulated AnimeEffects line to
`p-yukusai/AnimeEffects`. This document is fork-only; it is not part of any
upstream diff.

## 1. What is contributed

The accumulated line on `feat/recovered-2026-10-28`: 524 commits on top of
`832195d` (the merge-base with `upstream/master`), plus the uncommitted
mesh-content-level work committed on 2026-11-15. It is the editor / renderer /
exporter rewrite: frame ranges and the timeline, the graph panel, the QML
inspector, filters and masks, the in-process libav exporter, texture residency,
and the skeletal weight work.

`upstream/master` moved nine commits past the merge-base (fonts, UI scaling,
recents, DWM, multi-key bezier). The line predates all of them.

## 2. Archive (fork-only, never upstream)

Everything local-only was preserved in the fork before cleanup:

- `archive/2026-11-15/branches/*` — every local branch tip (23 branches).
- `archive/2026-11-15/worktrees/*` — one snapshot commit per dirty worktree
  (14 snapshots), including the Blend If worktree and the Windows variant
  trees. Parent = worktree HEAD; tree = the dirty state, untracked files
  included, .gitignored build output excluded.
- `archive/docs-2026-11-15` — `docs/archive/2026-11-15/`: SCRATCHPAD, notes,
  `upstream-review/`, the reports, split TSVs, the nine fix patches, and
  `REFS.md` (the archive-ref map).
- `.git/backups/animeeffects-full-2026-11-15.bundle` (62 MB, `git bundle
  verify`: complete history).
- All 14 tags pushed to the fork.

The Blend If work is preserved in `archive/2026-11-15/worktrees/blend-if` and
`.../branches/fix/bugfix`. It is deliberately **not** merged: its scope is
undecided and the worktree is not in a state to merge.

## 3. Reconciliation baseline

`contrib/2026-11-reconciled` (fork) is the full line merged with current
`upstream/master` and reconciled. It keeps the merge history and the
unmodified master fonts, so it is a working reference, not a submission.

- Build: 714 targets, Debug, exit 0.
- `scripts/run_suites.sh Debug`: 93 passed, 0 failed.
- Policy: where master and the line both changed a file, the line's version
  wins; files only master changed are kept. The line already carries DWM
  window rounding (`gui::PopupWindow`).

### Master's nine newer commits — disposition

| upstream | what | disposition |
|---|---|---|
| `324e4562` | merge of PR #35 (our timeline branch) | already the base |
| `46c4f250` | "- Misc": AUTOUIC, dead CMake, QSS duplicate | superseded by the line |
| `fc5a703e` | DWM rounded windows | the line has its own DWM path |
| `133fb518` | gate `WinResizeFilter` for Windows | no such class in the line |
| `dbd319a0` | variable Geist + Noto CJK, locale prefs | **series carries the four static Geist weights only**; the variable/CJK files and `preference_ko.txt` are omitted as unused by the line's qrc |
| `09c75b35` | UI scaling, custom font, recents | not merged; the line's settings/QML shell replaces this area. Decision pending |
| `779cbda` | icon scaling, Comic Sans egg | not merged; the line's icon handling is its own |
| `56b04b2` | multi-key bezier paste, CXX extensions, dGPU | not merged. Decision pending on the `setEasingTo` refactor |
| `25501ba`, `abb4b88` | CI fixes for the egg | obsolete with the egg |

## 4. The series

Twelve **stacked** branches, each one squashed commit on the previous branch,
all based on `upstream/master`. Phase N's PR base is phase N-1's branch, not
master. Every phase was built in Debug; the tip passes the whole suite.

| # | branch | content | historical tip | commits | build | suites |
|---|---|---|---|---|---|---|
| 1 | `contrib/2026-11-p1` | frame ranges, ruler/playback window, property easing, QML pilot | `f7a1e628` | 40 | pass | no runner yet |
| 2 | `contrib/2026-11-p2` | embedded assets, in-process libav encoder, one export path | `5263f585` | 47 | pass | no runner yet |
| 3 | `contrib/2026-11-p3` | export destinations, dialog rework, warning policy | `c9ec3c30` | 85 | pass | 15/15 |
| 4 | `contrib/2026-11-p4` | DWM popups, Windows deploy | `ab92e172` | 6 | pass | 15/15 |
| 5 | `contrib/2026-11-p5` | Qt Quick export dialog and destination picker | `ab4ca9ce` | 57 | pass | 21/21 |
| 6 | `contrib/2026-11-p6` | `cmnd::Gesture`: one open edit | `0dfbb09a` | 3 | pass | 23/23 |
| 7 | `contrib/2026-11-p7` | ImGui graph panel, menu, gestures | `d0427efe` | 15 | pass | 44/44 |
| 8 | `contrib/2026-11-p8` | docked Canvas Painter hosts, Auto easing, Qt 6.12 | `57f5ec23` | 64 | pass | 54/54 |
| 9 | `contrib/2026-11-p9` | QML inspector, filters, masks, tree thumbnails | `3cb05a2c` | 78 | pass | 67/67 |
| 10 | `contrib/2026-11-p10` | footprint sampling, render probe, residency plan | `08076129` | 63 | pass | 75/75 |
| 11 | `contrib/2026-11-p11` | paged layer textures, residency stages | `36a6535f` | 49 | pass | 75/75 |
| 12 | `contrib/2026-11-p12` | skeletal weights, base values, docking, mesh content | `324ce343` | 17 | pass | 93/93 |

Construction rule: each phase's line-owned tree is exactly the historical
tip; paths the line later deleted are gone, master-only files are kept. Four
historical defects were repaired in the phase that owns them:

- phase 1: a missing `#include <QSettings>` in `tool_ViewPanel.cpp`;
- phase 9: `tests/asset_remove_layers` did not compile `ThumbnailCache.cpp`;
- phase 8: backports `08c79a23` — the marquee's half-pixel inset belongs to
  the rasteriser (`InkQPainter`/`InkCanvas`), plus its two stale expectations;
- phases 10/11: the mask test now calls `announcePendingPaint()` per the
  per-paint contract, and the tree test re-addresses its delegate after the
  model reset and performs a real press+double-click.

`notes.md` and `AGENTS.md` are excluded from every phase (fork-only working
memory). The four static Geist weights are kept; master's variable Geist and
Noto CJK files are not in the series.

## 5. Verification

```bash
# build a phase
git -C /home/gambot/dev/AnimeEffects-wt/reconcile checkout contrib/2026-11-pN
cmake --build build --config Debug -j"$(nproc)"

# the Debug suite (exists from phase 3)
./scripts/run_suites.sh Debug
```

The suite was run on every phase that has the runner: phases 3-12 are all
green (15, 15, 21, 23, 44, 54, 67, 75, 75, 93 tests). Phases 1-2 predate
`scripts/run_suites.sh`; their gate is the Debug build. The reconciled base
also passes 93/93.

## 6. Decisions still open

1. **UI scaling / custom font / recents:** port master's version, replace it
   with the line's settings, or drop.
2. **Multi-key bezier paste:** port the `setEasingTo` refactor (the review
   found the rest of that commit unsafe).
3. **Exporter:** the series includes `src/media/`, `assets/export/` and the
   export UI. The September plan excluded the exporter; this one includes it
   because it is part of the accumulated line. Say if it should be split out.
4. **Grouping:** twelve PRs as above, or fewer/larger.
5. **Fonts:** the series' static-Geist-only choice is reversible; the CJK
   files are in master and in `contrib/2026-11-reconciled`.

## 7. Sending

Nothing was pushed to `upstream` and no upstream PR was opened. The branches
are in the fork:

```
git push origin 'refs/heads/contrib/2026-11-p*:refs/heads/contrib/2026-11-p*'
```

Open PRs in order, each with base = the previous phase branch (phase 1's base
is `master`). A cover letter for phase 1 should say that the series is a
twelve-part review of the accumulated line and that master's newer settings,
font and recents work is superseded by the line's own implementation, with
the four decisions above called out.

## 8. Not upstream

- every `archive/*` ref and `archive/docs-2026-11-15`;
- `notes.md`, `AGENTS.md`, `SCRATCHPAD.md`, `upstream-review/`;
- the Blend If worktree snapshot;
- the local bundle and `.git/dsh-scratch` remnants.
