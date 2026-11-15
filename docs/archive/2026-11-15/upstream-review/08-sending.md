# Sending it — three artefacts, in this order

Nothing has been pushed. Everything below is prepared, verified, and reversible. This file is the
thing to read when you actually decide to send.

---

## A. Bug-fix series — 9 patches against current `master` (`abb4b88`)

**What it is:** `.git/dsh-scratch/upstream-fixes/0001…0009`. Bugs in *upstream's own code*, found
while reviewing the 10 commits our branch lacks. Verified: each applies to a pristine `abb4b88`, all
nine applied in order reproduce the fix tree exactly (`650c17e0`), and the whole series **compiles
with the MinGW toolchain** (the same compiler that builds the Windows artifact).

| # | fixes | note |
|---|---|---|
| 1–3 | `Windows.h` casing, `QComboBox` include, `_declspec` | **without these, a Linux MinGW cross-build of master is impossible** — three compile errors |
| 4 | CJK font family names | the shipped Noto fonts are unreachable by the names the preferences ask for |
| 5 | recents: crash, leak, startup hang, MRU | the crash is reachable: export with FFmpeg missing, then "Reset recent files list" |
| 6 | fresh-install font | `hasFamily("")` is true, so a default install loses its font *and* its size |
| 7 | app-wide stylesheet ownership | it replaced `dialogbutton.ssa`, un-styling the new-project browse button |
| 8–9 | custom-easing validation and weight | NaN/inf and out-of-range x reached the solver; the weight scaled the curve's domain by (2−w) |

**Cover letter:**

> These are fixes to code in master, found while cross-building and reviewing recent changes. Three of
> them (the `Windows.h` casing, the `QComboBox` include guarded by `#ifndef Q_OS_WIN`, and
> `_declspec`) mean a MinGW build of master cannot compile at all — MSVC hides all three, which is why
> they survived. The rest are behaviour bugs: the recent-projects reset crashes when the dialog was
> opened from the export path; a default install replaces the platform font with a family-less
> `QFont("")` because `QFontDatabase::hasFamily("")` returns true; the application-wide stylesheet
> set from `MainWindow::onThemeUpdated()` replaces the one `GUIResources` installs for parentless
> dialogs, taking the only `QPushButton#browser` rules with it; "Paste custom easing" accepted `nan`,
> `inf` and out-of-range control points, and its weight term scaled the bezier's domain by (2 − weight)
> instead of blending. Each commit says how it was verified; the CJK one only renames the families to
> what the files contain and deliberately does not decide the font-set question.

---

## B. Our upstream-bound work — two branches

**What it is:** `contrib/upstream-2026-09` (74 commits) and `contrib/upstream-2026-09-warnings`
(6 more). Both build and pass their suites; the exporter is provably absent (zero files under
`src/media/`, `assets/export/`, `ExportTarget`, `ExportFacts`, `DestinationPicker`).

- `contrib/upstream-2026-09` — the editor/build/i18n work: timeline, property panel, core frame
  ranges, the asset-embedding and translation pipeline, plus the small cleanups that came with them.
- `contrib/upstream-2026-09-warnings` — the warning policy and what it found, including a real bug:
  `ExportParams::update()`'s `frameTick += 1` moved a local pointer, so the export dialog's
  "Current frame" counter never advanced. Warning-clean (0 first-party, 0 vendored).

**Based on `832195d`, which is an ancestor of current master**, so the PR diff is exactly these
commits and needs no rebase to display correctly. Sent from your fork.

**Cover letters:**

> **Editor + build/i18n series.** This is the non-export half of a larger branch, split out so you can
> review it on its own. It carries the timeline and property-panel work, the core frame-range rework,
> the asset-embedding and `qt_add_translations` pipeline, and the cleanups that came with them. The
> exporter work that shares these files is deliberately *not* here. Builds with
> `Ninja Multi-Config` (375 targets, zero first-party warnings), suites run and pass; the two probes
> that need arguments are noted in the commit messages.

> **Warning policy + the dead code it found.** The project compiles with no warning flags at all,
> which is why a test section could be defined, read as coverage and never called. This turns on
> `-Wall -Wextra` through a per-directory `ae_enable_warnings()`, deliberately not `-Werror`, and
> deletes what it found — including one genuine bug (`ExportParams::update()` advanced a local copy
> of a pointer, so the export dialog's frame counter never moved) and one real GUI defect. Vendored
> deps are marked SYSTEM so their warnings stay theirs. Not in this series: the GUI-probe runner,
> which belongs with the export probes.

---

## C. `46c4f25` cleanup — one patch against `abb4b88`

`.git/dsh-scratch/s9/fix-46c4f25.patch` — 5 files, `+2/−25`: the commented-out `CMAKE_MODULE_PATH`
line, the `FindWindowsSDK` bracket-comment (the file exists in no revision), the no-op `LIBRARY
DESTINATION` on an executable target, the duplicate `QMenu` rule whose stated rationale is false
(`QWidget:disabled` is 0-1-1 and beats `QMenu` 0-0-1), an unused include, and
`qt_finalize_executable(AnimeEffects)` → `(${PROJECT_NAME})` + trailing newline.

**It cannot be folded into B**: our branch already supersedes that code (its own CMake, the stylesheet
renamed to `assets/stylesheets/`), so the patch has no subject there. It is upstream-only cleanup and
applies cleanly to `abb4b88`.

**Cover letter:**

> A tidy-up of leftovers in current master: a commented-out `CMAKE_MODULE_PATH` line pointing at a
> directory that exists in no revision, a `FindWindowsSDK` block commented out for the same reason, a
> `LIBRARY DESTINATION` that can never apply to an executable target, a duplicate `QMenu` rule whose
> comment claims a specificity it does not have, and an unused include. Kept `CMAKE_AUTOUIC`,
> `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` and `qt_finalize_executable` where they are load-bearing.

---

## Order, and why

1. **A** — small, self-contained, and the three build blockers are pure portability with no behaviour
   change; easiest to accept, and it makes master cross-buildable.
2. **C** — tiny, adjacent to the same files, and best reviewed while A's context is fresh.
3. **B** — the substantive review. Split into as many PRs as you like by topic: the branch is ordered,
   so `git log --oneline 832195d..contrib/upstream-2026-09` will cut along its own seams.
4. **B-warnings** — most opinionated (it changes the build for everyone), so it deserves its own
   conversation; every commit in it also stands alone.

## Before sending — checklist

- [ ] Decide the two judgement calls that are yours, not mine: whether the Comic Sans egg stays
      (its `throw` relies on `main`'s catch-all), and whether the popup styling in the contrib branch
      is visually acceptable on Win11 — build **R** on the laptop answers the second.
- [ ] `git -C .git/dsh-scratch/wt/fix log --oneline abb4b88..upstream-fixes` and read the messages.
- [ ] `git log --oneline 832195d..contrib/upstream-2026-09` and the same for `…-warnings`.
- [ ] Re-run the checks the recipes already encode: patches apply, branch builds, suites pass,
      exporter absent (`git ls-tree -r --name-only <branch> -- src/media assets/export | wc -l` → 0).
- [ ] Nothing about the exporter leaks: it is the one thing that must not appear.

## Not done, deliberately

- **Pre-merging current master into B.** The measured cost is ~23 conflicted files, and several are
  decisions rather than mechanics — above all the font set (upstream now ships a variable Geist plus
  three Noto CJK files, ~48 MB, whose family names need the A/4 fix; our branch embeds four static
  Geist weights and has no `preference_*.txt` mechanism at all). Doing that merge means choosing a
  font policy on your behalf, so it waits for you. The branch displays correctly without it.
- **Pushing anything, or opening any PR.** Yours to approve.
