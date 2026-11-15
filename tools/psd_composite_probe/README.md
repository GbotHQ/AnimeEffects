# psd_composite_probe — real-file PSD import + composite-diff analysis

Answers, on a real PSD, the questions a golden test cannot: did the container parse, did the
importer build the tree the file describes, and **does AnimeEffects' own composite of that tree
reproduce the composite the PSD already stores?** It does that by loading the file through the
production importer (`ctrl::ImageFileLoader`), rendering one frame through the production
composite wiring (the `tools/render_verify` offscreen GL fixture), and diffing the render against
the PSD's section-5 merged image, decoded independently (`extract_composite.py`).

It is a diagnostic, not a pass/fail suite: nothing is asserted, everything is printed and dumped
so a specific import feature can be blamed for a specific region of the canvas.

## Run

```sh
# 1. reference composite (independent decode of the PSD's own section 5) -- MUST run first
python3 tools/psd_composite_probe/extract_composite.py \
    "/home/gambot/Downloads/ALL FATHER F3 color fixes.psd" /tmp/psd_composite_probe/composite.png

# 2. build + run (~40 s for the 114 MB file)
AE_BUILD_DIR=build tools/psd_composite_probe/build_and_run.sh

# build without running; or run an already-built binary
AE_BUILD_DIR=build tools/psd_composite_probe/build_and_run.sh --build-only
AE_BUILD_DIR=build tools/psd_composite_probe/build_and_run.sh --no-build
```

Must run from the repo root (the script `cd`s there). `build_and_run.sh` links the static
libraries of `$AE_BUILD_DIR` (`libcore libctrl libmedia libcmnd libimg libgl libthr libutil
libpugixml`, all inside one `-Wl,--start-group`) with Qt6 + FFmpeg via pkg-config; it does **not**
rebuild them, so after touching `src/` run `cmake --build build --config Debug` (or pass `--libs`
to have the script do it first). `assets/assets.qrc` is compiled into the binary with `rcc`
(otherwise the first `LayerNode` aborts on an `XC_FATAL_ERROR` shader `FileIO` error); the
generated resource and the binary live in the build tree (`$AE_BUILD_DIR/psd_composite_probe`,
`$AE_BUILD_DIR/qrc_assets_psd_probe.{cpp,o}`), never in this directory. Both cached artifacts are
invalidated by the `assets/` tree, not just by `assets.qrc` itself (the .qrc keeps its own old
mtime when only a shader changes) — a stale embedded shader would silently invalidate every render
the probe reports. The GL fixture is the repo's `tools/render_verify/scene.{h,cpp}`
(`-Itools/render_verify`), not a private copy.

Runs with `QT_QPA_PLATFORM=offscreen`; when the binary exits 2 (GL bootstrap failure) the script
retries with `QT_QPA_PLATFORM=xcb DISPLAY=${DISPLAY:-:0}`, like `tools/render_verify`. Exit codes:
0 = completed, 1 = import failed or the composite PNG is unusable, 2 = GL bootstrap failure,
64 = bad arguments. A shader in `assets/assets.qrc` that does not compile is *not* recoverable by
that fallback: the probe's error handler turns the `XC_FATAL_ERROR` into `abort()`, so the process
dies with SIGABRT (exit 134) — check `git status assets/shaders/` before blaming the tool.

With no positional arguments the script runs the probe on `$PROBE_PSD`, `$PROBE_OUT/composite.png`,
`$PROBE_OUT` and frame `$PROBE_FRAME`; if the composite PNG does not exist it is produced with
`extract_composite.py` first (the ordering constraint above is therefore satisfied automatically
in that case).

### Probe CLI

```
psd_composite_probe <file.psd> <composite.png> <outDir> [frame]
```

Any positional arguments given to `build_and_run.sh` are passed straight through in this order.

### Env flags

| variable | default | effect |
|---|---|---|
| `AE_BUILD_DIR` | `build` | build tree whose static libraries are linked |
| `AE_PROBE_CXX` | `clang++` | compiler used for the probe |
| `RCC` | `$(qtpaths6 --query QT_INSTALL_LIBEXECS)/rcc`, else `/usr/lib64/qt6/libexec/rcc` | resource compiler for `assets/assets.qrc` |
| `PROBE_PSD` | `/home/gambot/Downloads/ALL FATHER F3 color fixes.psd` | PSD used when no positional arguments are given |
| `PROBE_OUT` | `/tmp/psd_composite_probe` | output directory when no positional arguments are given (kept outside the repo on purpose) |
| `PROBE_FRAME` | `0` | frame passed to the probe when no positional arguments are given |
| `PROBE_BLEND` | unset | enable the blend probe section (see below) |
| `PROBE_GREEDY` | unset | enable the greedy layer-removal search (slow: re-renders the whole stack for every visible node, up to 12 steps) |
| `PROBE_LAYER_DUMP` | unset | comma-separated layer names whose decoded RGBA is also written as `layer_<name with '_' for spaces>.png` |
| `PROBE_GROUP` | `more tint` | group searched for by the blend probe |
| `PROBE_LAYER` | `Layer 49` | layer inside that group used by the blend probe |
| `PROBE_CELL` | unset | re-mesh every image key with this grid-mesh cell size before rendering. A cell size at or above a bitmap's size makes `GridMesh::createFromImage` fall back to an exact bounding quad (identity mapping, no cell culling, no burr reduction), which is the control for "does the generated layer mesh change what a layer draws?" - `PROBE_CELL=30000`. Measured on the 114 MB file: mean 4.5790 with the generated mesh against 3.9942 with the quad, see BLEND_IF_ASSESSMENT.md section 13.3. Only legal before the frame is rendered (the cached FFD data is a copy of the mesh). |

`PROBE_BLEND`, `PROBE_GREEDY`, `PROBE_LAYER_DUMP`, `PROBE_GROUP`, `PROBE_LAYER`, `PROBE_CELL` are read by the
binary itself (`qEnvironmentVariable*`), so they work both through the script and when running
`build/psd_composite_probe` directly.

## Printed sections

1. **`== PSDReader (production container parse) ==`** — `resultCode`/`msg`, header
   (w×h, channels, depth, mode), `layerAndMaskInfo` (`layerCount(field)` vs the number of records
   actually parsed, global mask, global additional infos), the image-resource block count and ids,
   then one `rec[NN]` block per layer record: entry type (`layer`/`folder-open`/`folder-closed`/
   `bounding`), name, rect, opacity byte, clipping, flags, visibility bit, blend key, channel list
   with stored data lengths, `mask=YES/no` and the additional-info keys with their lengths.
   Per record it also prints: alpha / user-mask coverage (mean, `clear` = share of samples < 8,
   `opaque` = share > 248), `iOpa` fill opacity as int **and** float, the Blend If ranges read by
   the production parser (`blendIf composite src=[..] dst=[..]` followed by one `ch<id>=[..|..]`
   pair per channel, so a file that sets per-channel ranges is visible), the `lfx2` layer effects
   parsed by `core::parseLayerEffects` (each effect with `(on)`/`(off)`, the overlays' colours,
   the gradient's style/blend/angle/scale/stops, each silhouette's blur, distance, angle,
   tint, opacity, blend mode and under/over side, and the stroke's position, size, paint type,
   blend mode, opacity, gradient stops and whether this build can draw it), the mask record
   (rect, real rect, default colour, flags),
   and the decode line `decode: <bytes> fnv1a=<hash>` — FNV-1a over the interleaved RGBA bytes
   `img::PSDUtil::makeInterleavedImage` produces (`decode: NULL` for the records that carry no
   image at all, i.e. the `folder-open`/`bounding` group markers). A matching hash in `layers.py`
   output proves the independent decoder and production agree byte-for-byte for that layer.
   `MASK layer ...` lines list the layers with a real (non-empty) user mask; the merged-image
   section prints
   `merged decode vs composite.png: differing channel samples=N maxDelta=D` — a cross-check of the
   production merged decode against the independently decoded PNG (`0 0` = byte-identical) — plus
   the merged channel ids and a dump `merged_decode_AE.png`.
2. **`== ImageFileLoader (production import) ==`** — the progress reporter's sections, `load()`
   result, the loader log, wall-clock ms and the resulting canvas size.
3. **`== imported object tree (top-first) ==`** — one line per node: `Layer`/`Folder`, name,
   `initialRect` (AE adds a 1 px texture border around the record rect), visibility, clipping, the
   default `Opa` key value, the renderer's blend mode name, and a `slim` marker. Compare it against
   the record list reversed (`check_structure`-style: records are bottom-first, the tree is
   top-first); `meta.py`/`layers.py` supply the record side.
4. **`== render frame N ==`** — render time, readback size, an alpha histogram
   (`min`, fully transparent px, partially transparent px) and two dumps: `render_raw_premultiplied.png`
   (raw GL rows combined into a single RGB+alpha PNG) and `render_straight.png` (view orientation,
   un-premultiplied straight alpha — this is the file every later comparison uses).
5. **`== diff: render vs the composite stored in the PSD ==`** — composite size/format check
   against the canvas, then `printStats` lines
   `max=M mean=X  >2: N (P%)  >8: N  >32:N  >64:N`, where the metric is the **per-pixel maximum
   absolute difference over the R,G,B channels** (alpha is not compared) and `mean` is its mean
   over all pixels. The decimal separator is the locale's (a comma under `cs_CZ`), so a known-good
   line reads `mean=40,3606`.
   - *leave-one-out attribution*: every visible node is hidden in turn, the stack re-rendered and
     re-diffed; entries are sorted by mean ascending and the best 14 plus the worst 5 are printed
     as `hidden mean=... delta=...`. A hidden mean **below** the baseline means AE renders that
     node (or its subtree) differently from the stored composite — this is what points at a
     dropped import feature. It ends with `restored baseline mean=... (must equal ...)`, a
     self-check that hiding/restoring left the tree unchanged.
   - *orientation sanity*: the as-is render plus X/Y/XY flipped variants, so a transposed or
     y-flipped readback cannot be mistaken for an import bug.
6. **`== blend probe: group='...' layer='...' ==`** (`PROBE_BLEND`) — hides everything above the
   named group, renders the stack up to and including the named layer into
   `blend_upto_layer.png`, then hides that single layer and renders the same stack into
   `blend_backdrop.png`, printing diff stats for both. Those two dumps are the exact inputs for
   `blend_ref.py`: an independent implementation of the blend can be run on the **identical**
   backdrop and source pixels, which separates "AE's blend math is wrong" from "AE's inputs are
   wrong".
7. **`== greedy layer removal (baseline mean=...) ==`** (`PROBE_GREEDY`) — up to 12 steps; each
   step re-renders with every still-visible node hidden once and keeps the single removal that
   lowers the mean the most, or reports `no removal improves the match`. Answers "does the stored
   composite correspond to this stack at all, and which layers would have to disappear for it to?".
8. **Tile analysis** — the worst twenty 128×128 tiles by summed max-channel diff, and a coarse
   mean-diff map (`. : + * #` at 0.5/2/6/12). The heatmap is dumped as `diff_heatmap.png`, and the
   worst tile is cropped into `worst_tile_heat.png`, `worst_tile_render.png` and
   `worst_tile_composite.png` for eyeballing.
9. **`done. dumps in <outDir>`**.

## Ground truth: the PSD's own section 5

The reference image is the composite **the PSD itself stores** (the merged image data Photoshop
wrote into section 5), decoded by `extract_composite.py` with its own container parse and its own
PackBits RLE decoder; it is **not** generated from the layers, so it is independent of both
AnimeEffects' importer and the layer records. Diffing the render against it is therefore the only
available ground truth for PSD features that no third-party importer reproduces:

- **Blend If** (per-layer blending ranges) — GIMP 3.2 ignores it. Measured: GIMP's flatten is much
  closer to a *no-blend-if* composite of the same stack (mean 21.3) than to a blend-if-aware one
  (56.8), i.e. GIMP's output agrees with "Blend If dropped", not with the file's own composite
  (58,98 vs the stored composite). An importer that ignores Blend If cannot be validated against
  GIMP.
- **Krita 5.3.2.1** refuses to open the real 114 MB file at all (exit 1, no output; its own
  `assets/sample.psd` exports fine).

The composite is only as good as the file: it is a snapshot of the layer stack **at the moment it
was saved**, so `PROBE_GREEDY` exists to test whether the stored composite still corresponds to the
current stack before any divergence is blamed on the importer.

## Python scripts

All of them are independent implementations (own container parse, own RLE decoder) that take the
PSD as `argv[1]` (default: the 114 MB file above) and write relative paths unless told otherwise.
`extract_composite.py`, `layers.py` and `meta.py` need only python3; `komp.py` and `blend_ref.py`
additionally need numpy + PIL.

| script | what it does |
|---|---|
| `extract_composite.py [file.psd] [out.png]` | decodes the PSD's section-5 merged image (own struct parse + own PackBits) and writes it as an RGB PNG. **Run this first** — it produces the `composite.png` the probe takes as `argv[2]`. Prints dimensions, compression and per-plane min/max. |
| `layers.py [file.psd] [layer name ...] [--out DIR]` | independent layer-record parse + channel decode; prints per-layer blend/opacity/flags/channel ids and the FNV-1a of the interleaved RGBA bytes, to be matched against the probe's `decode: ... fnv1a=` lines (equal hashes = byte-identical decode). Named layers are optionally dumped as raw RGBA. The name column is blank for files whose record-level Pascal name is empty (true for this one — Photoshop keeps the real name in the `luni` block, which `meta.py` decodes); a digest of `cbf29ce484222325` is the FNV-1a offset basis, i.e. an empty decode (a `pass` group marker). |
| `meta.py [file.psd]` | metadata-only parse (never touches pixel data): flags (`infx`/`knko`/`clbl`/`lspf`), `iOpa`, mask presence, and the layer blending ranges — i.e. the **Blend If inventory**, listing every layer whose source/destination sliders are not the `0/0/255/255` default, with the slider values. |
| `komp.py [file.psd] [out.png] [--no-blendif] [--no-mask] [--no-fillopa] [--no-effects] [--composite PNG] [--ae-render PNG]` | independent numpy compositor (Normal/Multiply/Overlay/Linear Burn/Color Dodge/fdiv/Color, straight-alpha `over`, weight = alpha × opacity × fill opacity × mask × blend-if). Prints `iOpa` values seen and a diff table against `--composite` (default `./composite.png`) and `--ae-render` (default `./render_straight.png`, the probe's dump). Ablations isolate each dropped feature: *all features* ≈ the stored composite while *blend-if disabled* ≈ the AE render is the signature of a Blend-If-ignoring importer. `--no-effects` is accepted for symmetry but is a no-op: layer effects (`lfx2`) are never synthesized. |
| `blend_ref.py [outDir] [composite.png] [--layer NAME]` | PDF-spec reference for one non-separable blend (default Color: `SetLum(Cs, Lum(Cb))`, `Lum = 0.3R+0.59G+0.11B`, ClipColor holding luminance) applied to AE's **own** backdrop and layer pixels from the blend-probe dumps (`blend_backdrop.png`, `layer_<NAME>.png`), compared against `blend_upto_layer.png`. Small residuals prove AE's blend implementation is faithful and that a divergence comes from the inputs, not the math. Writes `<outDir>/blend_reference.png`. |
| `mesh_silhouette_diff.py <dirA> <dirB> [--tsv F] [--crop X Y W H --png DIR]` | compares two `PROBE_MESH_MASK` dumps layer by layer on the axis the content level actually moves: the mesh SILHOUETTE (area lost/gained per layer, how far each of the four boundary sides moved, and an optional side-by-side crop with the missing pixels in red). Vertex and triangle counts are density, not shape, and a render can hide the whole effect behind an opaque layer above. |
| `engine_diff.py <treeA> <treeB> [--variants const10,nocut] [--tsv F]` | compares two `cutoff_batch.sh` output trees, i.e. two revisions of the mesh code, on the same files and variants: how many pixels the engine change moved, and each engine's mean difference against the PSD's own stored composite (the artwork's ground truth for coverage defects - it cannot arbitrate the level itself, since it contains whatever the level decides). |
### Decision on the content level

The artist compared the 30-file corpus and chose the MEAN rule: per layer,
`T = clamp(round(0.05 x mean(alpha : alpha > 2)), 1, 10)`, instead of the fixed 10.

Landed in `src/img/MeshContentLevel.h` and used as the default level of
`GridMesh::createFromImage`. `render_verify` runs against it (S22 for the relaxing direction, S26 for
the strict one), all 24 external goldens still pass because their layers are opaque, and the corpus
renders of the "mean" variant are byte-identical to what the application now produces. In the batch
trees the `mean` variant is therefore the current production behaviour and `const10` is the level it
replaced.

| `render_sanity.py <batchDir> [--files F ...]` | gates a render batch for GPU faults: each variant is a separate process and variants differ only by the mesh, so a pixel far from the PSD's own composite AND far from every sibling variant is not explainable by any cutoff. Found three bad renders in a 30-file corpus (sparse saturated speckles, up to 250 levels wrong) that re-rendering fixed; exits 1 when anything is flagged. |
| `cutoff_batch.sh <list> <outDir> [variant ...]` | renders every PSD in the list once per cutoff: `constant` (dir `const10`, T=10, what ships today), `none` (dir `nocut`, T=0 - nothing cut, the "what does a cutoff still remove" reference), `fixed:<T>`, `mean`, `ratio:<anchor>`. Pins its engine with `PROBE_BIN=<binary>` so a rebuild cannot silently change which engine produced a tree. |
| `cutoff_site.py <batchDir> <siteDir>` / `cutoff_server.py <batchDir> <siteDir> [port]` | the comparison page: per-file numbers against both references plus the 1:1 tile sheets, rebuilt continuously and served over HTTP. |

## Output

Into `<outDir>` (default `/tmp/psd_composite_probe`; nothing is written inside the repo):

`merged_decode_AE.png`, `render_raw_premultiplied.png`, `render_straight.png`, `diff_heatmap.png`,
`worst_tile_heat.png`, `worst_tile_render.png`, `worst_tile_composite.png`, optionally
`layer_<name>.png` (`PROBE_LAYER_DUMP`) and `blend_upto_layer.png` / `blend_backdrop.png`
(`PROBE_BLEND`).

## Reference run

`ALL FATHER F3 color fixes.psd` (114 136 005 B, 3840x1240, RGB/8, 57 layer records, RLE
composite), frame 0, `QT_QPA_PLATFORM=offscreen`:

```
  resultCode=0 msg=success. (end of file) ''
  layerAndMaskInfo: layerCount(field)=57 records=57 globalMask=no globalInfos=2
  merged decode vs composite.png: differing channel samples=0 maxDelta=0 (0 == byte-identical)
  load()=true  log='success'  22202 ms
  render 130 ms, 19046400 bytes
  render alpha: min=255 transparent=0 partial=0 of 4761600 px
  whole canvas           max=255 mean=40,3606  >2: 4461489 (93,70%)  >8: 3975781  >32:2459816  >64: 990710
  [orientation] flip-X    mean=96,1181 max=255
  [orientation] flip-Y    mean=101,1372 max=255
  [orientation] flip-XY   mean=107,4129 max=255
```

(The whole-canvas line is the one to diff against when re-running: at a fixed tree state it must
match bit for bit. It is a fingerprint of the *linked libraries plus embedded shaders*, not a
constant of the tool: this line was reproduced exactly while `build/src/{core,ctrl,img}` were the
pre-Blend-If build, and the first re-run after those libraries and `LayerDrawingFrag.glsl` were
rebuilt with the in-progress Blend If work reported `mean= 9,4568  >2: 2837818 (59,60%)` instead —
i.e. it measures the renderer, so re-run it whenever `src/` moves.)

So the container and every pixel decode are correct, the tree matches the records (51 layers,
3 groups), and the render still differs from the stored composite by mean 40.36/255. The
leave-one-out pass, `komp.py` ablations and `meta.py` attribute that divergence to the import
dropping, in order of effect: **Blend If ranges (7 layers)**, layer masks (6), Photoshop layer
effects (`lfx2`, 10) and fill opacity (`iOpa`, 9). AE's own blend math is not implicated:
`blend_ref.py` reproduces AE's Color blend from AE's own backdrop to mean 0.28/255 (max 0.6).

`komp.py` on the same file, all differences in mean per-pixel max channel error:

| variant | vs stored composite | vs AE render |
|---|---|---|
| blend-if + masks + fill opacity | **8.36** | 45.48 |
| blend-if disabled | 42.39 | **8.27** |

i.e. an implementation that honours Blend If lands on the stored composite, and one that drops it
lands on AnimeEffects' render — a 5× separation that no downstream check can fake. The residual
8.36 sits almost entirely in the title band (x 1653-3016, y 129-605), where the `lfx2` layer
effects and the masked / fill-opacity-0 layers live; neither AE nor `komp.py` synthesizes those.

## Where the remaining difference lives

Sections 13 and 14 of BLEND_IF_ASSESSMENT.md are the measurement of the residual that used to be
written off as this file's noise floor, and the two bugs it found in this repository:

1. the layer mesh's content test (`alpha > 10`) silently discarding the faint end of every
   layer's antialiased edge - 13 % of the whole-canvas difference, plus a latent out-of-bounds
   cell write (S22 pins the invariant now);
2. **Blend If's two range edges combining with a product instead of `min()`** - the rules agree
   on every ordinary range and differ only when the split handles cross, which 3 of this file's
   7 range layers do (a 4th entry looks crossed but is degenerate, `255,255 / 0,0`).

Whole canvas, mean max-channel difference against the composite the PSD itself stores:

| build | whole | outside the title band's effect rects |
|---|---|---|
| before | 4.5790 | 3.9400 |
| + the mesh fix | 3.9943 | 3.2215 |
| + `min()` for the range edges | **1.3024** | **0.7446** |

Outside the ten `lfx2` effect rects (86.4 % of the canvas) only **257 pixels of 4.1 M** differ by
more than 8 levels and 59 by more than 32; what is left is the title band's effect models, which
are knowingly incomplete and reported. The independent numpy compositor confirms the rule from
its own parser and blend math: 556 489 pixels of >8 before the correction, 301 after.

Three further findings from the same pass, each with its evidence in the assessment document:

- **The solid-colour stroke was never drawn** (section 16): `isSolid()` tested `PntT == "SCol"`,
  a key no producer writes - Photoshop writes `SClr` (32 strokes across the corpus, 0 occurrences
  of `SCol` anywhere). The harness wrote the same wrong key, so the tests confirmed the bug.
- **Nothing reported the effects the renderer cannot draw** (section 17):
  `isDrawnAsConfigured()` had no caller outside the tests, so an imported bevel was silently
  absent. `ctrl::undrawnEffectNotes` now feeds the import log and every reload.
- **A reload decodes every layer even when nothing changed** (section 18): 6.2 s of the 114 MB
  file's reload is decode, against 78 ms to parse it and 129 ms to digest its layers' compressed
  bytes - which is the sound, cheap change signal the reload should use.
  `build_and_run.sh --cost [<psd>]` re-measures all four stages on any file.

A fourth finding is a design one rather than a bug (section 19): the mesh's content test serves
two masters - what must be drawn and what deserves geometry - and the artist's two cases (erase
residue at alpha 1..10 that should stay out, a soft glow's skirt in the same range that must stay
in) cannot be separated by any single threshold: the split between faint pixels attached to a
solid core and faint pixels in a core-less island barely moves from T = 2 to T = 32.

The first rule proposed on top of that - keep whole connected components that contain a seed -
**does not hold**, and the corpus is what says so: over 15 artwork PSDs it would delete 1 704 498
pixels, and 41.8 % of them are soft gradients (fading, ramped, i.e. real painted content) rather
than the 49.8 % that are uniform veils. What separates the two populations is the region's own
alpha field - a veil's boundary sits at ~0.95 of its own peak, a soft feature's at ~0.30-0.53 -
not connectivity. `build_and_run.sh --mesh [<psd>] [--seed T]` measures the candidate policies and
their geometry; `build_and_run.sh --audit [<psd>]` measures what a rule would drop and writes
amplified crops so the dropped regions can be looked at.

What does separate them is the region's **mean alpha over its alpha > 0 pixels**: veils average
1.00-1.12 and soft gradients 2.12-4.00 across the 16 files, with no overlap, and a cut at the
format's floor (mean <= 1) removes 517 553 veil pixels and *zero* soft-gradient pixels. Section
19.5 has the tables.
