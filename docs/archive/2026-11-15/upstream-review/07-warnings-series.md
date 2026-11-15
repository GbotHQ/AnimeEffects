# Warnings + picker series — `contrib/upstream-2026-09-warnings`

The last of the non-exporter work the first contrib branch had to leave out, plus one upstream bug
the warnings paid for. **Six commits** on top of `contrib/upstream-2026-09`, in
`.git/dsh-scratch/contrib-warn-wt`.

**Why a separate branch.** Upstream enables **no warning flags at all** — no `-Wall`, `-Wextra`,
`/W4`, nothing (the only first-party `add_compile_options` is the ASan one). "Turn on `-Wall -Wextra`
and delete the ~18 pieces of dead code it finds" is its own conversation with its own review;
bundling it into the bug-fix series would bury the fixes. Both are upstream-bound, they are just
different arguments.

| commit | what it is |
|---|---|
| `5a3e761` | **build: enable -Wall -Wextra for first-party code** — a per-directory `ae_enable_warnings()` policy, deliberately no `-Werror`, MSVC left as a documented gap |
| `d691f06` | **refactor: delete the dead code -Wall found** — ~18 sites, plus one real GUI defect (the audio track panel's row-0 label) |
| `1fcd8ce` | **build(media): treat vendored deps as SYSTEM headers** — `src/deps/zip_file.h` produces 24 `unused function` warnings per including TU; `-isystem` is the honest fix |
| `50789a1` | **test(picker): give the probe the theme icons, and restore scenario #8** |
| `ded6402` | **Make picker_probe a gate, so its stale expectations cannot hide** — mismatches now reach the exit code |
| `58e30b2` | **fix: the export dialog's frame counter never advanced** — see below; this one is a bug, not hygiene |

## The bug the warning policy paid for

`ExportParams::update()` took the caller's counter as `const int*` and "advanced" it with
`frameTick += 1;` — which moves the local copy of the pointer. Nothing else incremented the
caller's `frameTicks`, so the legacy exporter's dialog showed **"Current frame: 0/N" for the whole
export**. `getFrameMessage(tick, dots)` builds exactly that string from the value, and the parameter
is passed by address for that reason. The fix is `(*frameTick)++` and dropping the const on the
pointee. Found by `-Wunused-but-set-parameter` — the instrument the series is about.

## Resolutions, and the trap in one of them

1. **`src/media/CMakeLists.txt` — dropped.** The `-Wall` commit adds the policy call to that module,
   which is the exporter's and is not in this series.
2. **`tests/CMakeLists.txt` — kept ours, prepended the policy.** The original also removed a
   target-specific `-Wall -Wextra` block belonging to the exporter test target; with no such target
   here, the removal has no subject while the addition does.
3. **`src/ctrl/Exporter.cpp`/`.h` — the dead-code commit's hunks stripped.** This is the trap worth
   remembering. The cherry-pick applied **cleanly** and still produced a broken tree: it removed
   `mVideoInCodec` / `mVideoInCodecQuality` because they are dead in *our* exporter, but in the
   **legacy** exporter — the one this branch keeps — they are used at `Exporter.cpp:298-313`. The
   build failed with 17 errors. **A clean cherry-pick is not a correct cherry-pick**; that is why the
   build is part of every verification here, not a formality. Resolution: restore those two files to
   the branch's version, keep everything else from the commit.
4. **`ded6402` — the `scripts/run_suites.sh` half is dropped, the test half kept.** That runner comes
   from `2765cf2` ("Track the GUI probes…"), which is entangled with the export probes this series
   excludes, and the commit modifies it. The deviation is stated in the commit message itself.
5. **`1fcd8ce` was not in either branch at first.** My original curation excluded it because its
   subject says "media" — but it touches only `src/img/CMakeLists.txt` (marking `src/deps` as
   SYSTEM), `src/img/{ORAReader.h,Util.cpp,oraParser.h}` and
   `src/gui/res/res_ResourceUpdater.cpp`. With `-Wall` on, it is *required*: without it the build
   emits ~2976 vendored warnings. The subject misled the curation; the file list did not.

## Verification

- **No conflict markers** (checked with `git grep -I`, which skips binaries — a plain scan flags the
  `.psd` fixtures, whose bytes contain the marker sequence).
- **Configures clean**: `-G "Ninja Multi-Config"`, exit 0, zero CMake errors.
- **Builds warning-clean**: 375/375 targets, exit 0, **0 errors, 0 first-party warnings, 0 vendored
  warnings**. That is the series' whole claim, and it holds on this tree now.
- **Suites pass**: `easing_regression`, `picker_probe`, `picker_pos_probe`, `anim_settings_undo`,
  `bonekey_regression` — all exit 0.

## Sending it

    git -C .git/dsh-scratch/contrib-warn-wt format-patch --no-signature \
        -o .git/dsh-scratch/warnings-series contrib/upstream-2026-09..

If the maintainer declines the warning policy, `d691f06`, `50789a1`, `1fcd8ce` and especially
`58e30b2` stand on their own merits: dead code, a probe whose scenario #8 was silently wrong,
vendored-noise suppression, and a frame counter that never counted.
