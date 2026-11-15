# Verification artifacts — where the evidence lives

Everything is under `.git/dsh-scratch/` (invisible to `git status`). Nothing here is needed to
build the app; it exists so any claim in `01-commit-review.md` / `02-bugs.md` can be re-checked.

Remember: **`/tmp` is wiped between shell calls**, so extract-then-inspect must be one command.

## Per-agent

| dir | subject | key artifacts |
|---|---|---|
| `s1/` | fonts: name tables, Qt matching, Geist figures | `FINDINGS.md`, `parse.py` (hand-written sfnt parser, no fontTools here), `deep.py`, `lookups.py`, `cjk_deep.txt` |
| `s2/` | font/scaling claims vs Qt semantics | extracted upstream `Main.cpp`/`MainWindow.cpp`/`AppStyle.cpp`, `exp/` with `probe2…probe10.cpp`, `qss1.cpp` (unitless `font-size`), `block_main.inc`/`block_mw.inc` (sed-extracted real code run verbatim) |
| `s3/` | was the double `setStyleSheet` preexisting | `MainWindow_before.cpp`, `MainWindow_after.cpp`, `MainWindow_master.cpp` |
| `s4/` | recents: disconnect, lifetimes, null deref | `probe_recents.cpp`, `probe2.cpp`, `probe_nullcast.cpp` (exit 139), extracted `MainMenuBar.*`, `GeneralSettingDialog.cpp` |
| `s5/` | paste-custom-easing end to end | `easingprobe.cpp` (links the real `Easing.cpp`), `EasingName.*` stub, `qstrtest.cpp`, `REPORT.md`, extracted serializer/deserializer |
| `s6/` | compiler flags, i18n catalogs | `narrow.cpp` (flag matrix), `equiv.cpp` + built `equiv_g++`/`equiv_clang++` (85.9 M-input equivalence), `int32_*.cpp`, `i18n/probe.cpp` + `i18n/um.ts` + `real_es.qm`, `tscan.py`, `lit.py` |
| `s7/` `s7-check/` | dGPU mechanism research + my MinGW check | `one.cpp`/`two.cpp` (`_declspec` vs `__declspec`), `u1.cpp`/`u2.cpp` token-only isolation; research report with vendor URLs was delivered in-session |
| `s8/` | DWM revert analysis | `up/`, `r/`, `pr/` (extracted upstream tree, revert attempt, patched), `PostRevert_AppStyle.cpp`, `qmenu_test*.cpp` (the `#include <QMenu>` compile test) |
| `s9/` | `46c4f25` audit + patch | **`fix-46c4f25.patch`**, `verify/` (patched `abb4b88` tree), `build-abb`/`build-patched`/`build-nofin`/`build-nouic` + `cfg-*.log` (differential configure evidence) |
| `s10-probe/` | my check: `hasFamily("")` | `hf.cpp` + `hf` — prints `hasFamily("") = 1`, `families().contains("") = 0`, `QFont(QString()) → 12pt` |
| `msvc-exc/` | my check: `std::exception("…")` | `t1.cpp` (1-arg), `t2.cpp` (MSVC 2-arg), `t3.cpp` (bare) — g++/clang++ fail on t1/t2, pass on t3 |
| `upstream/` | my diff dumps | `<sha>.diff` for each of the 10 upstream commits |

## My own split tooling

| file | what it does |
|---|---|
| `classify.sh` → `classified.tsv`, `commits.tsv` | classifies the 172 branch-only commits (exporter / mixed / other) with per-commit file lists |
| `trial_replay.sh` → `replay-phase1-on-upstream.tsv` | replays a commit range onto `upstream/master`, one commit at a time, recording conflicts |
| `split_trial.sh` → `split-trial.tsv` | builds a contrib candidate then replays the exporter on top (shows cascade failures — read the *first* conflict, not the count) |
| `contrib_candidate.sh` → `contrib-candidate.tsv` | the real recipe: 61 phase-1 commits + 2 surgically stripped `-Wall` commits + 13 non-exporter phase-2 commits = 75, with 1 conflict (`b093680`) |
| `cherry.txt` | `git cherry upstream/master HEAD` — all 172 are genuinely new (0 patch-equivalent) |

## Re-running the expensive ones

- **Fonts** (`s1`): `python3 .git/dsh-scratch/s1/parse.py` for name tables/widths; the Qt probes
  need `pkg-config --cflags --libs Qt6Gui Qt6Core` and `QT_QPA_PLATFORM=offscreen`, and — this
  matters — **one font per process**, or Qt merges the five `Geist` faces and resolves the wrong file.
- **`hasFamily("")`** (`s10-probe`): `g++ -std=c++17 -fPIC hf.cpp -o hf $(pkg-config --cflags --libs Qt6Gui Qt6Core)`
  then `QT_QPA_PLATFORM=offscreen ./hf`. Watch for the vexing parse: use `QFont f{QString()};`, not `QFont f(QString());`.
- **`std::exception`** (`msvc-exc`): `g++ -std=c++17 -c t1.cpp` must fail, `t3.cpp` must pass.
- **`_declspec`** (`s7-check`): `x86_64-w64-mingw32-g++ -c u1.cpp` (fails) vs `u2.cpp` (passes).
  Including `Windows.h` needs the sysroot on the include path; the token-only files avoid that.
- **The patch** (`s9`): `git -C .git/dsh-scratch/s9/verify apply --check fix-46c4f25.patch` — it is
  written against `abb4b88`, so it will **not** apply to your tree (`data/stylesheet` → `assets/stylesheets`).
