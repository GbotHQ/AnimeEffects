# Back after a month — where everything stands (2026-09-27)

Written to be read without the chat. Companion files: `SCRATCHPAD.md` (the durable working
memory, §0 worktrees, §2 export design, §5 upstream review) and `upstream-review/README.md`
(the review index; `08-sending.md` is the "when you decide to send" front door).

---

## 0. What was done in this session

- **Release build of `feat/export-ui-redesign` is green**: 408/408 targets, exit 0, 0 errors,
  0 first-party warnings (`cmake --build build --config Release -j 8`).
- **The app was launched with the new exporter** and stays resident:
  `build/src/gui/Release/AnimeEffects` (PID 1076041, `DISPLAY=:0 QT_QPA_PLATFORM=xcb`).
- **The full Release suite passes: 15/15, exit 0** (`./scripts/run_suites.sh Release`). That
  includes `media_exporter_e2e` (the real render → real file content/audio gate),
  `media_export_target` (every destination row), `destination_picker`, `export_form`, the i18n
  context checks, and the GUI widget tests.
- Working tree of the main checkout is **clean**; the build did not need any source change.

To rebuild/rerun later:

```
cmake --build build --config Release -j 8
./scripts/run_suites.sh Release
env -u WAYLAND_DISPLAY DISPLAY=:0 QT_QPA_PLATFORM=xcb ./build/src/gui/Release/AnimeEffects
```

---

## 1. The four threads, one line each

| Thread | State |
|---|---|
| New exporter | Built, green, runs — but **not ready to ship** (§6). |
| Redesigned export UI | Functional and probed, but **your verdict is that it is not an objective improvement**; keep it local. |
| Upstream contribution | **Fully prepared, never sent** — 3 artefacts, verified, nothing pushed (`08-sending.md`). |
| Worktrees | 3 sibling worktrees + 2 contrib + 1 fix + a cluster of scratch Windows-variant trees; **one has real uncommitted work** (§3). |

---

## 2. Worktrees in detail

### Sibling worktrees (`/home/gambot/dev/AnimeEffects-wt/`)

| Worktree | Branch | Tip | vs integration line | Dirty | What it is |
|---|---|---|---|---|---|
| `bugfix` | `fix/bugfix` | `22181ed` | 2 ahead / 91 behind | **50 files** | PSD Blend If / masks / effects import + derived mesh content level, plus a large uncommitted refactor (§7). The one with live work. |
| `docking-ui` | `feat/docking-ui` | `12a793a` | 41 ahead / 178 behind | clean | Docking panels, unmerged. |
| `mipmapping` | `feat/mipmapping` | `65f4fe3` | 43 ahead / 37 behind | clean | Mipmapping + export-path level work, unmerged. |

### Contribution worktrees (`.git/dsh-scratch/`)

| Worktree | Branch | Ahead of `upstream/master` | Dirty | What it is |
|---|---|---|---|---|
| `contrib-wt` | `contrib/upstream-2026-09` | 79 | clean | The editor/build/i18n slice (exporter provably absent). |
| `contrib-warn-wt` | `contrib/upstream-2026-09-warnings` | 80 | clean | `-Wall -Wextra` policy, dead code, and the frame-counter bug it found. |
| `wt/fix` | `upstream-fixes` | 9 | clean | The nine bug-fix patches against `abb4b88`. |
| `wt/{u,q,r,r2…r7,r5b,applycheck}` | detached `abb4b88` | — | dirty (expected) | Windows popup/DWM variant test builds. **Disposable**; their recipes are `.git/dsh-scratch/{mk_variants.py,build_variant.sh}`. |

### Topology (18 local branches, 8 remote refs)

- **Integration line is `feat/export-ui-redesign` @ `ab92e17`** — 178 ahead / 10 behind
  `upstream/master` (`abb4b88`), clean.
- `master` (`322a523`) is **4 ahead / 111 behind** `upstream/master` — stale, not a base for
  anything.
- The contrib branches are based on `832195d`, an ancestor of `upstream/master`, so their PR
  diffs display exactly and need no rebase.
- `backup/*` refs (5) and `.git/backups/animeeffects-full-20260918.bundle` (45 MB) exist;
  **the bundle still only lives on this machine** — a pending decision.

---

## 3. `fix/bugfix` — what is actually in there

Committed (2 commits on top of the integration line):

- `36da5fc` — **PSD Blend If, layer masks, layer effects; derived mesh content level.** Adds
  `src/core/BlendIf.*`, `LayerMaskBake.*`, `LayerEffects`, format minor bumps 12 and 13, the
  property-panel groups, `tests/panel_blendif.cpp`, `tools/render_verify`, and the
  content-level derivation in `GridMeshCreator`.
- `22181ed` — probe docs only.

Uncommitted (~42 tracked files + a staged `tools/verify_blur` → `tools/render_verify` rename +
untracked `tests/support/`): a broad refactor across `FormatVersion`, `ImageKey`, `LayerEffects`,
`ResourceData`, `SourceDocument`, plus test/tooling moves. **Nothing here is committed**, so it
is the one place a month of absence could have left work stranded.

---

## 4. Encoder selection — the design you had forgotten (reconstructed)

This is the part you said you did not recall. It is **policy, not measurement**, and that is a
deliberate decision from 2026-09-13.

**Where it lives:** `src/ctrl/ExportTarget.cpp:98-240` (the ladder), the `<quality_control>` and
`<bucket_positions>` blocks in `assets/export/ExportTargets.txt`, and
`media::ffmpeg::encoderUsable()` in `src/media/`.

**The model**

1. **A codec family = one format.** Within a family the encoders differ only in *how* they
   produce it: software has the full rate-control toolbox, hardware has speed.
   `familyEncoders()` (`ExportTarget.cpp:113`) fixes the declared order, software first:
   AV1 `libsvtav1 → libaom-av1 → av1_nvenc/qsv/vaapi/amf`; H.265 `libx265 → hevc_*`;
   H.264 `libx264 → h264_*`; lossless `ffv1`.
2. **Which families, by destination class × bucket** (`ladderFor`, `:147`):
   - **Direct** (the platform plays the file as-is) → **H.264 only at every bucket**, because
     decode compatibility is the requirement and quality-per-bit is not. AV1/HEVC stay
     reachable only by naming a codec in Advanced.
   - Otherwise: **Lossless** → FFV1 → H.265 → AV1 → H.264; every other bucket → **AV1 → H.265
     → H.264** (a fact: per bit, AV1 > H.265 > H.264).
3. **The one thing the bucket changes** (`fastFirst`, `:167`): time-sensitive buckets
   (Smaller/Balanced) prefer a hardware encoder *within* a family; **Best** keeps the declared
   software order. Direct is always `fastFirst`.
4. **A candidate is only used if it actually runs** (`resolveLadderCodec`, `:705`): the filter
   is `encoderUsable()` (a cached one-frame test encode), never `encoderAvailable()`, because
   `ffmpeg -encoders` lists encoders that fail at runtime.
5. **Quality intent is a per-codec knob, not a bitrate** (`applyQualityIntent`, `:203`): the
   bucket picks a position between the codec's own `best` and `worst` values from
   `<quality_control>`, applied through `encOptions`. This is what makes "Smaller/Best" work
   for fixed-codec destinations too.
6. **What was deliberately deleted** (`SCRATCHPAD.md` §4, third pass): the `<time_budgets>`
   wall-clock gate, `encoderThroughput` and its eight-frame probe, and the "nothing is fast
   enough" message. Reason: the projection was work-aware, the threshold was not — it rejected
   every candidate on a long 4K export and collapsed to "fastest available, whatever it is".
   **Owned consequence:** a machine with no AV1 hardware picks software AV1 for a long export
   with no time warning; the escape hatch is Advanced. **If a warning is added, it should be a
   user sentence, not a measurement.**

**Also still design-locked (do not re-litigate):** `SCRATCHPAD.md` §2.4–2.7 — no VMAF / RD
sweeps / corpus / sampling (rejected in §2.6), the crossover fact (software AV1 at real-time
presets is worse than HEVC; hardware AV1 is exempt), and the two-phase constraint model
(deterministic pre-encode, then measure-then-fit).

---

## 5. Codec / auto-resize review — what looks iffy (verified this session)

You were right to flag this. A read-only review produced concrete findings; the top ones were
checked against the source, not just read once.

1. **Hardware priority is reversed.** `resolveLadderCodec` builds the order with
   `order.prepend(name)` (`ExportTarget.cpp:719-727`), so on a machine where several run, the
   *last* hardware entry wins — `h264_amf` before `h264_nvenc`, the opposite of the declared
   order. *(Verified by reading the loop.)*
2. **The quality knob silently does nothing on a hardware/ladder codec.**
   `applyQualityIntent` is called with the *resolved library name* (`:1010`), but
   `<quality_control>` only keys `libvpx-vp9, libsvtav1, libx264, libx265, libwebp, gif`
   (`ExportTargets.txt`). A resolved `*_nvenc/qsv/vaapi/amf` finds no entry, the intent is
   dropped, and there is **no note** — the note only exists for the missing-`bucket_positions`
   case. On this build the software paths happen to be chosen, so the failing branch is exactly
   the untested one. *(Verified.)*
3. **`fastFirst` contradicts its own comment.** `:167` sets `fastFirst = (bucket != Best)`, so
   Lossless also takes hardware first, and Direct forces hardware H.264 even at Best/Lossless —
   while the comment promises fidelity buckets keep the software order.
4. **No software fallback at export time.** The ladder picks once; a hardware encoder that
   passes the 64×64 probe can still fail `avcodec_open2` on the real option set/resolution and
   there is no step down to `libx264`.
5. **The fit loop's "frames" phase shortens the duration, not just the size.** It raises the
   stride (`:1572-1590`) but `plan.fps`/`cfg.fps` are never lowered (`plan.fps` at `:818`,
   `cfg.fps` at `:928`), so a fitted video plays at the original rate for less time. The
   documented degrade order treats duration as the *most* destructive trade, and no test
   asserts the fitted video's duration. *(Verified: the fps assignments are untouched by the
   fit path.)*
6. **The app's `fitRequest` is not the plan's request.** `MainWindow.cpp:1457-1465` drops
   `playhead` and `imageStem` (set at `:1395-1404`), so a capped Still destination re-plans
   from frame 0 and can write a differently-named sequence than the plain path — a fitted
   artifact that differs from the plan the user saw.
7. **`requiredSize` omits `SizeMode::MaxMinSide`** (`:1244`), so the one row with an exact-side
   *floor* (LINE) can be shrunk below it by the phase meant to protect it. The line's own
   comment claims eight exact rows are protected and this is not one of them.

Lower-severity, also recorded: `applySizeRule` has no validation for degenerate table data
(0-side → NaN/2×2); `evenFloor`'s floor of 2 can breach a hypothetical `exact 1`; image-artifact
attempts silently drop `encOptions`/`muxerOptions`/`codecTag`; audio bitrate silently defaults
to 128000; the search can return an unverified endpoint when its attempt budget runs out.

**None of these is fixed.** They are review findings, and items 1–6 are the ones to decide
before calling the exporter ready.

---

## 6. Exporter — what "not ready" concretely means

Still open, in priority order (from `SCRATCHPAD.md` §2.8 and the later passes):

1. **Audio is now largely modelled** (per-track switch, per-destination codec/bitrate/channels/
   rate) — but the design-research list still predates that; verify against the rows before
   treating it as closed.
2. The review findings in §5 (encoder selection + fit loop).
3. **Untranslated user-visible strings in `src/media`** — 46 raw literals (28 `VideoEncoder.cpp`,
   13 `GifEncoder.cpp`) plus `ImageFileLoader` etc., so translated headlines sit above English
   diagnostics. Separate sweep, not started.
4. **`exportProject()` is still one long function and three call sites still build their own
   `ctrl::ExportRequest`** / there is no plan-to-widget extraction. Named as the one scope item
   that was deliberately not done.
5. Feature gaps still absent: still-image export kind, sprite sheets, multi-range/trim, aspect/
   crop/safe-area, loop count & direction, framerate conversion, colour space/HDR/10-bit,
   flash-rate warning, chroma-key fallback, preset/batch/overwrite polish.
6. **The GUI dialog itself is not gated** — `scripts/gui/probe_*.sh` assert it, but they are run
   by hand, not by `run_suites.sh`.

---

## 7. The Blend If question (your design debate)

**What is committed:** `36da5fc` imports Photoshop's per-layer "Blend If" ranges (source and
destination sliders) as first-class renderer state, plus layer masks, layer effects, and a
derived mesh content level.

**Why it looked worth doing:** measured against the PSD's own stored composite on a real
3840×1240 file, the whole-canvas mean error fell **40.36 → 8.75** (outside the title band
40.85 → 3.94), which is the floor of what an independent compositor reaches (8.36 / 3.45) for
the same reachable feature set. It closed a real import-fidelity gap.

**Why the design doubt is reasonable** (the two sides, so the decision is yours and not an
implicit one):

- **For first-class:** it is faithful and non-destructive; source ranges *can* be baked to a
  mask (`core::LayerMaskBake`, exposed as "Bake This Layer range"), but **destination ranges
  cannot** — they depend on the backdrop — so dropping the feature would lose something that
  cannot be baked away.
- **Against:** it is a Photoshop compositor concept, not an animation concept; it adds
  per-draw uniform state to the renderer, a new property-panel surface, two format-version
  bumps (12, 13), and a widget test target. If AnimeEffects is a mesh-deformation animation
  tool, per-layer blending ranges may be fidelity that does not belong in its own model.

**Options:** (a) keep it first-class; (b) keep the import but bake source ranges and drop the
persistent UI/property (destination ranges then lost or approximated); (c) drop the feature.
The uncommitted UI work in the worktree is the panel side of option (a).

---

## 8. Upstream plan — the certain changes, as separate PRs

Everything below is **prepared, verified, and reversible**; nothing has been pushed. Full
detail and paste-ready cover letters: `upstream-review/08-sending.md`.

**A. Bug-fix series — 9 patches against `abb4b88`** (`.git/dsh-scratch/upstream-fixes/`).
These are fixes to *upstream's own master*, found while reviewing and cross-building:

| # | Fix | Note |
|---|---|---|
| 1–3 | `Windows.h` casing; `QComboBox` include behind `#ifndef Q_OS_WIN`; `_declspec` | **Without these, a MinGW cross-build of master cannot compile at all**; MSVC hides all three. |
| 4 | CJK font family names | The shipped Noto fonts are unreachable by the names preferences ask for. |
| 5 | Recents: crash, 8-QAction leak, startup hang, MRU | Reachable crash: export with FFmpeg missing → Reset recent files list. |
| 6 | Fresh-install font | `hasFamily("")` is true, so a default install loses font and size. |
| 7 | App-wide stylesheet ownership | It replaced `dialogbutton.ssa`, un-styling the browse button. |
| 8–9 | Custom-easing validation and weight | NaN/inf/out-of-range reached the solver; the weight scaled the curve's domain by (2−w) instead of blending. |

Ideal split: **1–3 as one portability PR (or three tiny ones), then 4/6 font, 5 recents, 7
stylesheet, 8–9 easing** — each is independently reviewable and the "certain" set you described.

**B. `contrib/upstream-2026-09` — 79 commits** (editor/timeline/property-panel/core
frame-ranges/asset-embedding/i18n pipeline + cleanups). Branch is ordered, so it can be cut
along topic seams into as many PRs as you want.

**C. `46c4f25` cleanup — one patch** against `abb4b88` (`+2/−25`: dead CMake paths, a no-op
`LIBRARY DESTINATION`, a duplicate `QMenu` rule, an unused include). Upstream-only — our tree
already supersedes the code it fixes.

**D. `contrib/upstream-2026-09-warnings` — 6 commits** (`-Wall -Wextra` policy, the dead code
it found, and a real bug: `ExportParams::update()` advanced a local pointer so the export
dialog's frame counter never moved). Most opinionated; its own conversation.

**Suggested order:** A (pure portability, easiest to accept, makes master cross-buildable) → C
(tiny, adjacent files) → B (the substantive review) → D.

**Must NOT go upstream:** the exporter (`src/media/`, `assets/export/`,
`ExportTarget`/`ExportFacts`/`DestinationPicker`), the export UI redesign, docking UI,
mipmapping, and (until you decide) the Blend If work. The exporter's absence from the contrib
branches is already verified (`git ls-tree` → 0 files).

**Deliberately not done:** pre-merging current `upstream/master` into B. The measured cost is
~23 conflicted files, and **twelve of them are one decision — the font/locale policy** (our
four static Geist weights vs upstream's variable Geist + ~48 MB of Noto CJK). That merge
encodes a policy, which is yours to set; the branches display correctly without it.

---

## 9. Decisions waiting on you

1. **Blend If:** (a) keep first-class, (b) import-but-bake, or (c) drop — §7.
2. **Font/locale policy** for the pre-merge (the 12-file decision in §8).
3. **Send the upstream artefacts, and in what split** — the 9 fixes as separate PRs, or one
   series? (§8)
4. **Backups:** push `.git/backups/animeeffects-full-20260918.bundle` off this machine.
5. **`docking-ui` and `mipmapping`:** still wanted? They are clean and unmerged; decide whether
   they are future work or should be archived.
6. **`fix/bugfix` uncommitted refactor:** commit it on the branch (or a topic branch) before it
   is lost, independent of the Blend If decision — the refactor and the feature are separable.

## 10. Suggested next order

1. Decide §9.1 (Blend If) and §9.6 (commit or park the uncommitted refactor) — stop the only
   bleeding.
2. Send **A** (and **C**), which are independent of every open design question.
3. Fix the reviewer-approved exporter findings (§5 items 1–6) as their own commits, with tests
   that can fail.
4. Then decide the font policy and the UI verdict before any merge or B split.
