# Texture residency, deformation and the frame's cost model

**Status: design, written 2026-11-01 from measurements on this tree.** It exists so the next
session does not have to rediscover the argument. Numbers marked **[M]** were measured by
`gl::FrameStats` on this tree; **[D]** is arithmetic from measured inputs; **[O]** is open.

Companions in the build today: `src/gl/FrameStats.{h,cpp}` (the frame line), the density meter in
`core/MeshTransformer.cpp`, `tests/density_meter.cpp`, `scripts/gui/probe_camera_transform.sh`.

---

## 1. What the renderer does today

A painted frame: `ObjectTree::render` → `prerender` every node (skin) → sort by depth → draw every
visible layer into the frame FBO. A filtered layer isolates into a `FilterFrame` slot **at device
size** and runs the stack there; a non-Normal blend mode captures the destination first; clippers are
singulated into `ClippingFrame` and the clipped layers draw through that mask.

Textures: **one dense `GL_TEXTURE_2D` per image key, base level only** (`F_MAKE_MIPMAP 0`), RGBA8,
mutable `glTexImage2D`. Masks are `GL_R8` at the content's own resolution (2026-11-01). No screen
bounds test exists anywhere: the only culls are `!mIsVisible` and blended opacity 0.

Deformation: FFD/bone **key values blend on the CPU** into `expans.ffd().positions()`; one
transform-feedback pass per layer per painted frame applies SRT + dual-quaternion skinning
(`MeshTransformVert.glsl`), and the visible draw adds only `uViewMatrix`. Since 2026-11-01 that pass
is cached on `TimeKeyExpans::revision()`: a camera-only frame re-skins nothing **[M: 11 → 0 passes
for a pan on the small project; 128 layers on the big one]**. Playback still re-skins every visible
layer every frame, by definition.

Measured on `nene.anie` (15000x15000, 133 layers, 68 folders) at a 1364x717 view **[M]**:

| | |
|---|---|
| layers considered / drawn / culled | 130 / 128 / 2 (the 2 are the opacity test) |
| fill (sum of clipped screen quads) | 1,038,179 px |
| resident textures | 2,088,114,920 B = **1.94 GiB** (peak = live) |
| process RSS | 7.6 GiB (the decoded layer images) |

Small project, same view **[M]**: 11 layers, fill 259,122 px, 19.8 MB of textures.

Deformation density on `test.anie` during playback **[M]** (see §3 for the metric): 46.6 % of the UV
area at density 1.0-1.41 (undeformed region), a fat tail, maximum 105-220x. That tail is real, not a
meter artefact: the trace prints the same triangles' raw areas, and frame 50 renders identically on
`3cb05a2c` (before the recovered work) and on this tree.

## 2. The problem, stated exactly

Residency is **per layer, per base level, for the whole session**: every image key that has been
drawn keeps its full texture (`ImageKey::Cache`, no eviction). So VRAM tracks the *document*, while a
view can only ever display its own pixel count.

The corollary that decides the design: **a viewport samples at most `view width x height` texels per
layer**, no matter how large the layer is. At 1:1 a 1364x717 view shows 1364x717 content pixels. The
900 MB a 15000² layer's base level costs is memory no view can consume at once.

Two consequences:

1. **Memory**: the fix is residency (what is resident, not what exists).
2. **Quality**: with no mip chain at all, minification at zoom-out is a point-sampled
   `GL_LINEAR` fetch from a huge texture - aliasing plus cache thrash. Mips fix the *sampling*; they
   add 33 % to whatever is resident and do nothing for §2.1.

Masks are now full content resolution too (`core/MaskKey`, `core/FilterMask`), 225 MB for a 15000²
mask **[D: 15000² x 1 B]**. They are painted, so their pyramid must be maintained incrementally.

## 3. The metric: what a view needs

The level a region needs is not a function of the camera alone: vertices move, UVs do not, so density
varies across a layer. Per triangle:

```
J            = d(screen or mesh) / d(uv)          the 2x2 Jacobian
s_max, s_min = its singular values
density      = area(mesh) / area(uv) = s_max * s_min      (what is measured today)
anisotropy   = s_max / s_min
level        = L_screen + ceil(log2(s_max))       per region
```

**Anisotropy [O]**: the histogram carries `det(J)` only, i.e. the *product* - a 4:1 anisotropic
triangle with the same area needs one level sharper along its major axis than `det` implies
(`s_max = sqrt(det * ratio)`). Two consequences: the store must carry `s_max` (and ideally the ratio)
per triangle, and the sampler must use `GL_TEXTURE_MAX_ANISOTROPY_EXT` where available. Neither is in
the engine today **[M: no anisotropy occurrence in src/]**; the meter's next revision should emit
`dens_smax` bins and an anisotropy histogram (the same reduction, one more attachment).

**The statistics that matter** (measured on a real project, §1): the max is not a policy input (200x
-> 8 levels, and it is one sliver-ish boundary triangle), the mean hides the tail, and the
**area-weighted histogram** is the honest object. A whole-layer `p95` would leave ~5 % of the area up
to 4 levels coarse - visible on a small, genuinely stretched region (an eye highlight, a strap under
a pose). That is why the answer is per-region (pages), not a percentile on a whole layer; a
whole-layer fallback should be chosen from the histogram *with a cap* and its blur reported.

**Measurement**: per frame, in the skinning pass (the deformed positions are there), a geometry
shader draws the mesh once more into a 16x1 R32F target - additive blending sums UV areas per
`log2(density)` bin, a second attachment keeps the true max - read back two frames later through
ping-pong PBOs. Zero GL objects, draws or readbacks when `AE_FRAME_STATS` is unset. Slivers
(< 0.25 px² UV) are dropped (never clamped) and counted in the trace.

*Why the PBO, when the payload is 128 bytes*: `glReadPixels` without one **synchronises the
pipeline** - the CPU waits for every queued command, i.e. a frame of GPU work, to read a value the
frame does not need. The cost is the sync, not the bytes. (Worth measuring both ways once; the
`ms` field is the judge.)

## 4. The pyramid (authoring data, not runtime derivation)

- **Scope**: one pyramid per **image resource**, not per key - `ImageKey::serialize` stores only a
  resource id, so every layer using the same image shares one.
- **Storage: every chain is stored, without exception.** Images and masks, imported or painted in
  the app - what is in memory at save time is what the file holds, and a load decodes and uploads it
  with no GPU downsample at all. "Once per image" therefore means "once, ever": the cost lands on
  the import (or on the stroke that last changed it), never on an open, and the pixels are the ones
  the authoring session produced rather than whatever the driver's shader arithmetic yields today.
  Mutability is a separate axis: a mask being painted updates the boxes its stroke touched, in
  memory and in the file at the next save.
  The GPU downsample path therefore exists for exactly two cases, and neither is a load of a current
  file: (a) a legacy project with no stored chain (built once at load, after which it is authoring
  data), and (b) the stroke's dirty boxes while painting.
  Format note: masks were never released, so their chain needs no compatibility path; image chain
  chunks must be optional and version-gated, since images ARE released.
- **RAM**: decoded at load into RAM - all levels, coarse ones cost 1/3 of the base - so that no
  frame ever decodes. `nene` goes from ~1.94 GiB to ~2.6 GiB of decoded images **[D]**.
- **Content rules**: the `feat/mipmapping` branch's - premultiplied, halo-free, levels past the
  alignment cap rebuilt on the CPU, `GL_TEXTURE_LOD_BIAS = -0.5` as a policy on top.
- **Masks get mips too, for the same reason images do** - they are sampled minified exactly like a
  layer, so a mask without a chain aliases at fit zoom. And like an image, a mask is often IMPORTED
  (a PSD mask arrives through `ImageFileLoader::attachMask`), so the default case is identical: the
  chain is built once at import or creation, stored with the field, and decoded at load.
  The one difference is that a mask is not immutable: the first stroke makes it mutable, and from
  then on its chain is updated **incrementally from the dirty box**, level by level (each level's
  box halves) - never regenerated whole. So "built once" for a mask means "until the first stroke",
  and the incremental path is a continuation, not a separate mechanism.
  Deferring mask chains was a staging choice that cost real quality in the meantime; they belong in
  the same stage as the image chain, immediately after the import fix below, not in a later one.
- **The importer's 1-texel ring is GONE (done).** It copied the artwork at texel (1,1) with a
  transparent border, so every level from 1 down averaged across the content's own 2x2 blocks with a
  half-texel phase error (the branch measured 112/255 at 2x). `prepareForSampling` now copies the
  image size for size at (0,0), `mipAlignedTextureSize` and the 128-texel alignment are deleted, the
  upload is the content size, and `uImageSize` is that same size - so content texel (x,y) is texture
  texel (x,y), pinned by the probe's 1:1 checkerboard.
- **Resolution rule: ceil-div, NO padding, and the chain is built on the GPU.** GL's mip level sizes
  are `max(1, side >> l)`, so a size with no trailing zero bits makes level 1 already non-halving
  (a 15001^2 base, or every level of a 1023^2 one) - the case that decides the design, because a CPU
  rebuild there reads back the whole base. Levels are therefore built by a GPU area pass:
  - **the reach-back rule** (`gl::Texture::mipSourceLevel`, `kMipChainSpan = 3`): levels 1..3 are
    built from level 0 - exact for any size, odd included, three base passes - and level `l >= 4`
    from level `l - 3`, an 8:1 step whose source texels are themselves averages. Three levels back
    and not one, because a chained level's error is its SOURCE's error averaged over a wider box, so
    a one-level reach keeps the source's spikes.
  - **the study that chose 3** (`tools/mip_compare`): 50 files - photographs, illustrations, PSD
    composites, screenshots and text pages, flat-colour art, sprite sheets - each level built five
    ways (`exact` from level 0, `chain1`/`chain2`/`chain3` from level - 1/- 2/- 3, `bilinear` as a
    2-tap driver-style chain), the four non-reference ones measured against an exact area average of
    the premultiplied base in 8-bit code values. 45 produced metrics; 5 were dropped (one CMYK PSD
    and four files Qt cannot decode). Worst per-texel deviation per level band, `max / mean` in
    255ths:

    | policy | 1-2 | 3-4 | 5-6 | 7-8 | 9+ | worst sample |
    |---|---|---|---|---|---|---|
    | `chain1` (level - 1) | 53 / 0.47 | 65 / 3.09 | 52 / 2.42 | 38 / 1.28 | 38 / 10.67 | 65 (mean 5.16) |
    | `chain2` (level - 2) | 0 / 0.00 | 27 / 0.06 | 16 / 0.30 | 13 / 1.66 | 13 / 3.36 | 27 (mean 0.06 at that level) |
    | `chain3` (level - 3) | 0 / 0.00 | 8 / 0.12 | 7 / 0.03 | 7 / 0.32 | 3 / 0.97 | 8 (mean 0.34) |
    | `bilinear` (2-tap) | 128 / 0.61 | 178 / 1.38 | 143 / 0.75 | 188 / 16.44 | 182 / 123.50 | 188 |

    `exact` is the reference every column is measured against, so it is 0 by construction. Cost, in
    base reads: 1.33 for `chain1`, 3.33 for the shipped rule (levels 1-3 from the base, then base/4
    + base/16 + ...), 8.33 for the superseded rule that read the base through level 7.
  - **what the eye saw** decides it: contact sheets, x8-amplified delta maps and level images
    upscaled x4 nearest, at levels 2, 4 and 7, make `exact`/`chain1`/`chain2`/`chain3`
    indistinguishable; `chain1`'s delta map lights up broadly on flat-colour artwork and on text,
    `chain3`'s is almost black, and `bilinear` is visibly wrong. The criterion for the rule is
    "looks exact", and reaching back 3 meets it.
  Masks follow the same rule for a whole-field build; their per-stroke update takes the deepest
  exact level instead, because the box is box-sized and exactness is then nearly free.
  Re-running the study: `./build/tests/Debug/mip_compare <image> <outdir>` per sample (no GL
  context needed; it writes `metrics.json` and one PNG per level per policy), once per path in
  `tools/mip_compare/samples.txt`, whose header records how the 50 were selected.
  No readback, no sync, no per-upload CPU work; the pass saves and restores the full GL state
  (FBOs, viewport, VAO, program, blend/depth/scissor/cull) because it runs inside the caller's draw.
- **Open [O]**: premultiplied vs straight for the *stored* levels needs the written analysis against
  the branch's halo evidence, the blend path (`img::Util::makeTextureUpload` premultiplies today),
  the storage size and the export path.

## 5. The page pool, in detail

### 5.1 Units, addressing, table

- A resource is one image (a layer's `ImageKey` resource, or a mask field). Its levels come from the
  pyramid (§4). A **page** is 256x256 texels of one level.
- The pool is a **growable set of atlases**, not one fixed texture: atlases of 4096x4096 (16x16 = 256
  slots of 256^2) per format - RGBA8 for images, R8 for masks - allocated in steps as demand exceeds
  capacity, and released when the budget says so. A fixed single atlas is wrong for this engine: a
  4K viewport needs ~32 MB of RGBA pages at 1:1 **[D: 3840x2160 x 4 B]**, an 8K export target needs
  ~132 MB, and the GPUs in use differ by an order of magnitude in VRAM.
  Pages sit 4 texels apart and the gutter repeats the edge texel, so a bilinear tap inside a page can
  never reach a neighbour's content.
- A slot is therefore `(atlas, index)`: the table entry carries the atlas index and the slot index, or
  a flat page id that the pool resolves. `R16UI` still holds it (4 bits atlas, 8 bits slot, a valid
  bit).
- **Why separate 4096² textures rather than one bigger one** - measured reasoning, not taste:
  - *Resizing* costs an allocation of the new size PLUS a copy of the old contents (no
    `glCopyImageSubData` at GL 4.0: an FBO blit or a re-upload), with both textures live at the peak,
    i.e. 5x the old size momentarily, and every table entry rewritten. Adding an atlas costs one
    allocation and nothing else.
  - A single huge texture (16384² RGBA = 1 GB) is far more likely to be refused or to fragment VRAM,
    and `GL_MAX_TEXTURE_SIZE` is 8192/16384 on much of the hardware this must run on.
  - Sampling does not care how large the texture is; the differences are binds (one unit per atlas,
    cached per resource) and the shader's atlas selection - a dynamically uniform sampler index (legal
    from GLSL 4.00, our floor) or a short if-chain for <= 4 atlases.
  - Only whole atlases can be released when idle (the shrink of §5.5); one big texture cannot be
    partially freed.
  - A `GL_TEXTURE_2D_ARRAY` of 4096² layers is the named-and-rejected alternative: one bind and one
    sampler, but its depth is fixed at allocation, so growth is the same new-array-plus-copy trade.
  - The one thing to A/B rather than argue: `ms` with 1 atlas vs 3 on the same scene, once the page
    path exists - expected within noise, cheap to prove.
- **Page table**: per resource, an `R16UI` texture of `(levels, pagesX, pagesY)` mapping a page to a
  slot, with 0 meaning "not resident". A 15000² resource at 256² pages is 59x59 at level 0 and a
  handful at each level below - the whole table is tens of KB, not MB **[D]**.
- The shader replaces one `texture()` with: fragment -> content px (the existing `uScreenToLayer`) ->
  the level from a per-vertex Jacobian varying -> page coords -> table fetch -> pool fetch. Two extra
  fetches. Their cost is measured before anything ships (a dedicated `ms` A/B on the big project).

### 5.2 Demand is ANALYTIC, not sampled feedback

The density pass already walks every triangle of every drawn layer with the deformed positions. The
same data yields, per triangle, the level it needs and the page rectangle it covers; their union per
resource is this frame's demand.

- **No feedback pass, no lag.** Demand is exact for the frame being drawn, so nothing has to be
  conservative "because the answer is two frames old".
- **No GL 4.2 dependency.** Sampled feedback wants image load/store; analytic demand does not, and
  this engine targets `QOpenGLFunctions_4_0_Core`.
- The error is one-sided: a page fully occluded by another layer still counts as demanded. That costs
  a bounded over-fetch (measured with the counter), never a hole.
- Feedback remains a possible *later* optimisation to trim over-fetch; it is not on the path.

### 5.3 No holes, no pop-out: the always-resident coarse chain

Every resource keeps its **coarse chain resident and complete** (all levels at or below a size where
one level costs a few pages). A page that is not resident samples the coarse chain: **blur, never a
hole**. A page arriving is a quality *increase*, which is not a pop; the thing that would pop - a
resident page vanishing while still sampled - is prevented by 5.4.

### 5.4 Eviction, budget, and what staleness actually costs

- Capacity is pool slots, per format. Priority to keep: the coarse chain > pages demanded in this
  frame > pages demanded within the last N frames (N = 3) > everything else, oldest first.
- Demand is analytic, so "last demanded" is exact; the N-frame window exists only so that a one-frame
  occlusion or a jitter at a boundary cannot evict a page that is about to be sampled again.
- Under pressure the **sharpest, oldest** pages go: the result is local blur, bounded by the budget,
  never a hole and never a colour pop.
- A 10x zoom in one frame: demand is exact for that frame, so the top-priority upload starts
  immediately; until it lands the coarse chain is sampled (blur), and the per-frame budget bounds the
  landing rate. No N-frame rule, no oscillation.
- Eviction is capacity-driven, never change-driven. The cost asymmetry is the policy: a sharp page can
  only come back from RAM (an upload), a coarse page regenerates from a resident sharper one at
  almost no cost - so under pressure sharp pages are the ones worth protecting *only while demanded*.

### 5.5 Pool growth, budget and hardware variance

- **Budget**: one number, in bytes, enforced across formats. Default derived from the hardware where
  it can be read (`GL_MAX_TEXTURE_SIZE`, the renderer string) and, more usefully, **configurable** -
  the same project must run on a 2 GB and a 24 GB card, and the user is the one who knows.
- **Growth**: when demand exceeds capacity and the budget allows, allocate another atlas (a step of
  64 MB for RGBA8, 16 MB for R8) and add it to the free list. Growth is bounded by the budget, never
  by the driver: an allocation that fails with `GL_OUT_OF_MEMORY` is caught, the last atlas is
  dropped and the budget is lowered for the session - the frame degrades to blur, it never crashes.
- **Shrink**: an atlas with no demanded pages for a sustained period is released (its slots evicted
  first). A window that is resized smaller, or a project closed, returns memory without a restart.
- **Both paths share it**: the exporter is the same paged renderer with a policy object -
  `demand = synchronous`, `budget = export budget`, `target = output size` - not a second
  implementation. The viewport's policy is `demand = analytic/async`, `budget = viewport budget`.
  That is the whole difference; a project export renders at a different resolution, and its demand
  follows its target, not the canvas.

### 5.6 Where a new page goes, and how the frame stays consistent

Allocation is a slot in the pool's fixed grid; there is no size fragmentation because every page is
the same size.

1. **Free list first**: slots never used since the pool was created, then slots freed by eviction.
2. **Otherwise a victim**: the eviction order of §5.4 picks one; its table entry is cleared and the
   slot is reused.
3. **Write the texels**: `glTexSubImage2D` of the page's 256x256 from the RAM pyramid into the slot's
   rect **including its 4-texel gutter** (the gutter is filled from the same level's neighbours, so a
   bilinear tap at a page edge cannot bleed). Unpadded rows are handled the way the mask uploads do it
   (`GL_UNPACK_ROW_LENGTH`, `GL_UNPACK_ALIGNMENT 1`).
4. **Publish**: the resource's table texel `(level, px, py)` = slot, written after the texels, both
   before the frame's draws - GL executes in order, so the same frame samples the page it just
   uploaded. A page that is already resident is a no-op (the table already names a slot).
5. **Budget**: the queue is sorted by (deficit levels x visible area) and drained until the frame's
   byte budget is spent; the rest waits, and the coarse tier covers the wait.

The bookkeeping is one map per pool, slot -> (resource, level, x, y); it is what eviction and the
table both update, and it is what a fixture asserts (allocate, evict, reuse: the texel at the slot's
origin equals the page's origin, and the table names the slot).

**Two tiers, so the fallback is never evicted**: the always-resident coarse chain of §5.3 lives in a
*pinned* region of the same atlas (its slots are never in the free list and never victimised), while
the pool proper holds sharp pages. That makes "no hole" structural rather than a policy: the fallback
is resident by construction. Its cost is the coarse levels themselves - at a 512 px threshold, 1.33 x
512² x 4 B = 1.4 MB per resource **[D]**; a 256 px threshold costs 0.35 MB and is the knob for
projects with many resources.

### 5.7 What can regress, and the gates that catch it

The page path is shipped behind a switch, and the switch exists **only** as the control for these
gates and for A/B measurement - not as a user-facing fallback. Its removal criterion is stated in §6:
when every gate is green on every project and the counter shows no regression, the switch is deleted.

| regression | gate |
|---|---|
| a wrong pixel vs the full-resolution path | pixel equality against the control at a matrix of zooms and poses, including the extreme FFD/bone case from §1 |
| a hole (a sample with no resident level) | assert every sample resolves through the chain; a sentinel in the table must never reach the shader |
| a pop-out (resident page evicted while sampled) | a zoom/pan sequence asserts residency never drops a page demanded within N frames |
| upload spikes | `pages=` shows uploaded bytes per frame <= the cap, on the big project while playing |
| shader cost | `ms` A/B, page path vs control, same scene and view |
| editing a mask | the page path's result equals the control within the same frame budget, from the dirty box only |
| export | export reads the RAM pyramid and is independent of pool state (a gate with the pool emptied) |
| two formats in one frame | a layer and a mask painted in the same frame, both paged |

### 5.8 Risks and unknowns

- **Shader addressing cost**: two extra fetches per fragment; measure before committing to the design.
- **Trilinear across levels**: bilinear inside a page is the gutter's job; sampling between two levels
  needs both tables - the coarse chain covers it, at the cost of a hard transition at the boundary.
- **Table size for 15000²**: small, but the *page count* at level 0 is 3481 pages per resource, more
  than one pool: the budget and the coarse chain are what make that fine.
- **Anisotropy** (§3): page level from `s_max`; the sampler needs `GL_TEXTURE_MAX_ANISOTROPY_EXT`
  where available, and the gutter widens if it is used.

## 6. Staged path

- **Stage 1 - sampling foundations** (low risk, immediate quality): a real mip chain per texture
  with the branch's rules, correct MAX_LEVEL bookkeeping, `GL_TEXTURE_LOD_BIAS = -0.5`, and
  anisotropic filtering where the extension exists. Adds 33 % to resident memory; removes the
  minification aliasing. Gate: `verify_mipmap` (exists) plus an anisotropy case.
- **Stage 1.5 - the ring and mask chains**: drop the 1-texel import ring in favour of the border
  colour, fix `uImageSize` and the content rects, and give masks a chain maintained from the stroke's
  dirty box. Gate: `verify_mipmap` ported as a build target (its M1/M3/M17/M20 already measure
  exactly this), plus a mask-stroke case that asserts the chain tracks the paint.
- **Stage 2 - the pyramid: MEASURED AND DROPPED (2026-11-01).** The premise was that persisting the
  levels would save the chain build at load. It does not pay. Real projects, `roundtrip` in Release
  under Xvfb (llvmpipe, so the GPU side is if anything overstated): a 54 MB project loads in 16.7 s -
  resources decode 0.62 s, object-tree deserialize 5.0 s, solve+upload 0.45 s, twice (load and reload).
  A 3.6 MB project: 4.9 s, the same shape (object tree 0.63 s). With `buildMipTailOnGpu` compiled out the
  load moved by 0.03 s and 0.14 s: the chain is **< 1 % of load**. Storing the levels would add 33 %
  more pixels to compress, write, decode and upload, to avoid a build that costs less than decoding that
  copy would. So no stored pyramid, and no format bump for one.
  **Where that difference actually sits** (measured, because "the chain looks blurrier" is the report
  it produces): away from a discontinuity the chained level is bit-identical to the exact one - mean
  delta 0.017/255 and high-frequency energy -0.08 % on a chart, +0.01 % on a line drawing. Every texel
  ADJACENT to an edge moves by ~4.9/255, because the coarse texel straddles the edge and its average -
  part one side, part the other - is handed over whole. The visible loss is therefore the share of edge
  texels times that delta: 12.5 % of texels and -10.6 % high-frequency energy on a chart, 3.5 % and
  -0.8 % on a line drawing, 0 % on a smooth ramp. That is why line art and text read as globally
  softer while a photograph barely changes, and why the artefact looks global even though every texel
  that moves is at an edge. The EXACT level does not get softer on non-dividing sizes: its box is 2^L
  wide either way (measured high-frequency energy of exact level 2 from 1024/1023/1000/1001 crops of
  one image: 1.616 / 1.624 / 1.666 / 1.659 - content, not divisibility).

  **Divisibility is the switch, not the cause** - and it is why 04-AUTUMN.psd (1938x2531) is the
  corpus's worst offender at level 2. Its step-by-step chain, per level, against whether that step
  tiles (prev/target exactly 2):

  | level | size | step | tiles? | max delta | texels over 2/255 |
  |---|---|---|---|---|---|
  | 1 | 969x1265 | from level 0 | - | 0 | 0.00 % |
  | 2 | 484x632 | 2.0021 x 2.0016 | no | **39** | **4.82 %** |
  | 3 | 242x316 | 2.0000 x 2.0000 | yes | 19 | 3.13 % (inherited) |
  | 4 | 121x158 | 2.0000 x 2.0000 | yes | 6 | 0.38 % (decayed) |
  | 5 | 60x79 | 2.0167 x 2.0000 | no | **44** | **15.30 %** |
  | 6 | 30x39 | 2.0000 x 2.0256 | no | 24 | 20.77 % |

  A cutting step takes half of a source texel's *mixture* (part line, part background) where the exact
  level splits it correctly: the filter does not widen, one texel of a two-texel-wide line is replaced
  by that line's average with its neighbour. Cores stay (the darkest 1 % is identical), shoulders go 2
  lighter and single contour texels move up to 39/255 - which is why a drawing reads as globally softer
  while only 4.8 % of its texels change. The shipped reach-back does not do it at these levels: 0 delta
  at levels 1-3 and 6 at level 4 on the same image, because level 4's source is level 1 - an 8:1 step,
  where a straddling texel weighs an eighth instead of a half.

  **Is there a better step than the area box?** Yes and no, and both halves are measured on
  04-AUTUMN (the two candidate kernels are `half` - integer boxes, no split - and `cubic` - the parent
  reconstructed with Catmull-Rom and integrated, `tools/mip_compare`):

  | step | area box | cubic | integer box |
  |---|---|---|---|
  | level 2 from the exact level 1 (2:1, cuts) | 39, HF -2.19 % | **27, HF -0.50 %** | 182, HF +0.7 % |
  | level 4 from the exact level 1 (8:1, cuts) | **6, HF -0.50 %** | 18, HF +0.90 % | - |
  | repeated (a chain of that kernel) | damps: 39 - 19 - 6 | **compounds: 27 - 27 - 20** | drifts |

  So a reconstruction kernel does what it says - it restores the high frequencies the parent's rect
  removed - and at 2:1 from an exact source it also comes CLOSER to the truth (27 against 39). But at
  8:1, and on a repeated chain, it loses: the area box is a contraction (each level averages its
  source's error away, 39 -> 19 -> 6) while a sharpening reconstruction is an amplifier (27 -> 27 ->
  20). There is therefore no step that is both sharp and accurate without either reading the base
  (anchoring) or making the grids integral (padding). The shipped rule already spends its anchoring
  exactly where the straddle weight is worst - near 2:1 - so its first chained step is the 8:1 case,
  where the plain area box is the better kernel.

  **SHIPPED RULE (decided 2026-11-01): span 1, with the window tightened to 0.8 from level 2 up.**
  `gl::Texture::kMipChainSpan = 1` and `kMipStepShrink = 0.8f`; level 1 keeps the plain area average of
  the base, because a source that has not itself been averaged has no lost width to restore (measured:
  tightening level 1 overshoots - 000026 renders +2.65 % against the ideal at LOD 1 without it and
  +4.21 % with it, while 000169 goes -0.09 % -> +1.90 %). The mask chain's whole-field build calls the
  same rule, so an image and a mask cannot drift. Import cost falls from ~3.3 base reads to ~1.3 and the
  visible cost is 0.1-0.3 points of rendered sharpness at LOD 3-4. The probe holds the behaviour: 128
  checks, including the analytic ramp (the reference equals the closed form to 1/255), the hard edge
  (confined to its own column), and the shipped chain at levels 2 and 8 - the first is the box to 0.3
  when its source is exact and tiles, the second is not by design (10.7 of 255) and is bounded at 15.

  **Tightening the box** (a reviewer's suggestion worth its numbers): the extra blur is not sampling
  between texels - the taps are whole texels - it is that the overlap-weighted 3-tap average is
  slightly WIDER than the ideal 2.002-texel box. Mixing the area kernel with a 2-tap box snapped to
  the nearest integer start is a single knob `s` (0 = area, 1 = snapped), measured on 04-AUTUMN level
  2 against the exact level:

  | s | high-frequency energy vs exact | max delta | mean delta | texels over 2/255 |
  |---|---|---|---|---|
  | 0.00 (area, shipped shape) | -2.20 % | 38.9 | 0.334 | 5.30 % |
  | 0.50 | -1.42 % | 53.7 | 0.592 | 5.55 % |
  | 0.75 | -0.74 % | 75.2 | 0.806 | 5.53 % |
  | 1.00 (snapped) | +0.14 % | 98.2 | 1.045 | 5.66 % |

  So yes - the blur can be bought back exactly, and `s` is a clean dial between "uniformly soft" and
  "hard but displaced". What the dial really trades is error *shape*: the snapped kernel puts each
  destination box on the nearest integer lattice, so texels land up to half a source texel away from
  where the exact level puts them - a sawtooth across the image. In a still that reads as harder,
  slightly ragged edges on high contrast; in an animated layer scaling it would shimmer, because the
  offset pattern is fixed in texture space while the sample point moves. The texels that are wrong do
  not change with `s` (about 5.5 % at every setting); only their magnitude does. And a 3x3 (or 3x2)
  tap set is what the area step already is - more taps cannot help, because the ideal footprint is
  only 2.002 texels wide.

  **Tightening around the true centre** - narrowing the window instead of snapping it - is the version
  that works, and it is free (the same 3 taps; raising the centre tap IS narrowing). 04-AUTUMN, level 2
  (a 2:1 step), window width as a multiple of the exact footprint, same centre throughout:

  | window | HF energy vs exact | max delta | mean delta | texels over 2/255 |
  |---|---|---|---|---|
  | 1.00x (as shipped) | -2.20 % | 38.9 | 0.334 | 5.30 % |
  | 0.90x | -1.50 % | 33.3 | 0.260 | 4.77 % |
  | **0.80x** | **-0.62 %** | **27.9** | **0.233** | **4.48 %** |
  | 0.75x | -0.11 % | 33.2 | 0.245 | 4.38 % |
  | 0.70x | +0.42 % | 34.3 | 0.276 | 4.38 % |

  Unlike snapping, this improves sharpness AND accuracy together (it compensates the discrete
  approximation's extra width rather than displacing the box), and the error map dims visibly: in a
  4-up of exact / area / 0.80x / snapped on a line-art detail, the 0.80x difference map is the
  cleanest of the three approximations and the snapped one is by far the brightest - a shifted edge is
  plainly perceptible.

  **But it is a function of the step ratio, and it must not be a constant.** The same test at an 8:1
  step (level 4 from level 1, 9 taps over an 8-texel window, where the discrete overlap already
  matches the box) goes the other way: 1.00x is best (-0.54 %, max 5.7) and 0.90x is already worse
  (+3.08 %, max 17.9). The correction is needed only where the tap set is coarse for the footprint -
  i.e. near 2:1. That is also why the shipped rule needs none: it chains only from 8:1 upward, and
  anchoring levels 1..3 means the coarse 2:1 step is never chained at all. If anchoring is ever
  dropped, the chained 2:1 step should use the tightened window (derive the factor from the ratio,
  never a magic 0.8).

  **How far back should a chained level reach?** Measured on four images that actually exercise it
  (thin outline art, dense hairlines/text, an engraving's hatching, and cel art), level 3 against the
  exact level:

  | image | from level 2 (a 2:1 step, twice) | from level 1 (one 4:1 step) |
  |---|---|---|
  | 11-ae.psd (outlines) | 33.3 / 0.269 / HF -2.54 % | **11.4 / 0.077 / -0.41 %** |
  | 35-img-000 (text) | 36.4 / 0.930 / -6.80 % | **13.2 / 0.387 / -2.78 %** |
  | 43-example-44 (hatching) | 24.5 / 1.053 / -4.57 % | **19.9 / 0.664 / -2.08 %** |
  | 04-AUTUMN (cel art) | **0.8 / 0.026 / 0.00 %** | 19.2 / 0.173 / -1.27 % |

  (max delta / mean / high-frequency energy; AUTUMN is the exception because that particular 2:1 step
  tiles.) So stepping twice through an intermediate level is worse than one wider step from a level
  that is exact - by 2-3x on the content that shows it. Which is the argument for a SPAN rather than
  always stepping one level: fewer chained steps, each from a better source.

  The cost/quality curve this gives, in base reads and worst level-3 error: anchor 1..3 (span 3, ~3.3
  base reads) leaves level 3 exact and level 4 at <= 6; anchor 1..2 (span 2, ~2.4) makes level 3 the
  first chained one at 11-20; anchor 1 only (span 1, ~1.3) makes level 2 the first, at 20-40 with
  -1.4 to -7.3 % high-frequency energy. Span 3 is where the visible levels are still exact.

  **What makes an image useful for this test**: fine high-contrast structure at several scales -
  hairlines, text, hatching, outline art. The predictor is the share of texels adjacent to a strong
  gradient (12.5 % on a chart, 3.5 % on a line drawing, ~0 on a soft painting), not its subject:
  smooth painted work and flat-colour sprites show almost nothing at any level.

  **And the criterion that actually matters is the RENDER, not the level.** Comparing a level against the
  exact area average measures a reference nobody sees: the screen shows the layer sampled at a scale,
  through trilinear blending of two levels. Measured that way (000022, base 1500x1440, chain built up
  from level 0 so all three share one source, LOD 1.0 to 4.0 in quarter steps, sharpness = mean gradient
  of the rendered layer):

  | LOD | exact | chain1 (span 1) | chain3 (span 3) |
  |---|---|---|---|
  | 1.00 | 11.25 | 11.25 (0.00 %) | 11.25 (0.00 %) |
  | 2.00 | 14.26 | 14.25 (0.08 %) | 14.26 (0.00 %) |
  | 3.00 | 15.00 | 14.58 (2.77 %) | 15.00 (0.00 %) |
  | 4.00 | 17.81 | 17.19 (3.44 %) | 17.77 (0.22 %) |

  So the whole anchoring question is worth at most **~3.4 % of rendered sharpness** even for the cheapest
  chain, and ~0.2 % for the shipped one - while the sampling model itself moves sharpness by 30-40 %
  between LOD integers (11.25 at 1.0 against 14.26 at 2.0, the same jump in every policy). Sub-pixel
  phase sensitivity (a shimmer proxy: standard deviation of the render across four sub-pixel offsets) is
  identical to three decimals across policies (1.578 / 1.578 / 1.578 at LOD 2).

  Which means the span should be chosen on COST, not on perceived quality: span 1 is the cheapest and
  renders within 3.4 % of exact at the deepest scale measured. The anchor is not a correctness measure,
  it is buying a few percent of sharpness at depth - so if the chain's cost ever matters, drop it; keep
  it if the extra base pass is free, because it makes the levels exact for their own sake.

  **And the kernel is not open either: with span 1, TIGHTEN.** Rendered sharpness against the ideal (the
  base area-averaged at the same scale), 000022, trilinear, span-1 chains built from level 0:

  | LOD | ideal | exact (= span 3 here) | span 1 area | span 1 tightened 0.8 |
  |---|---|---|---|---|
  | 2 | 14.249 | 14.257 (+0.06 %) | 14.249 (0.00 %) | 14.249 (0.00 %) |
  | 3 | 16.829 | 14.996 (**-10.90 %**) | 14.585 (-13.33 %) | 14.977 (**-11.00 %**) |
  | 4 | 20.268 | 17.805 (**-12.15 %**) | 17.198 (-15.15 %) | 17.750 (**-12.43 %**) |

  So plain span 1 is 2.4-3.0 points softer than span 3 in the render, and tightening puts it back within
  0.1-0.3 points - at the same 3x3 taps, no extra work, no extra memory. Sub-pixel shimmer is unchanged
  across every policy (2.41-3.07, spread under 2 %). The factor is not fragile: 0.75 / 0.80 / 0.85 / 0.90
  give -10.46 / -11.00 / -11.56 / -12.11 % at LOD 3, i.e. the whole sensible range lands within ~1.5 % of
  span 3, with 0.8 the sweet spot and 0.75 beginning to overshoot.

  That is the recommendation: **span 1 with a tightened window** - cheapest chain, renders like the
  anchored one.

  WHICH RATIO, THOUGH. A span-1 step is nominally 2:1, but the actual per-axis ratio is
  `S / floor(S/2)`: exactly 2 only while the level's side is EVEN, and `2 + 1/C` from the first odd
  side onward, growing as levels shrink - 000022 runs 1500, 750, 375, 187, 93, 46, 23, 11, 5 and its
  ratios are 2, 2, 2.0054, 2.0108, 2.0217, 2, 2.0909, 2.2. So the factor is not a function of a
  constant 2:1, it is a function of how far above 2 that step's ratio sits.

  It does not need a formula, because the two regimes are already separable and tightening is FREE
  where it must not act: on a step that TILES, the centred window of 1.6 texels covers the same two
  texels with equal weights as the 2.0 window does, so narrowing is an exact no-op - measured on
  750x720 -> 375x360: 1.00 / 0.90 / 0.80 / 0.70 windows all give max 1.4, mean 0.223, HF -0.09 %,
  identical. On a step that CUTS it acts as measured (375x360 -> 187x180: HF -4.36 / -2.43 / -0.51 /
  +1.40 % for the same four windows).

  So the rule is a single constant: **tighten by 0.8 at every chained level**. It is inert wherever the
  step is exact and compensates wherever it is not. The 8:1 step measured earlier is the same story -
  its ratio is 8.008 and tightening there HURT, which is why the shipped span-3 tail uses the plain
  area box.

  The object-tree block WAS the target, and the profile found it: `ObjectTree::deserialize` was 10.2 s
  over the harness's four deserializes, of which 8.4 s was `new LayerNode` - each layer built its own
  `MeshTransformerResource`, i.e. compiled the same three mesh-transform programs 344 times. Sharing one
  resource per `ShaderHolder` (`ShaderHolder::meshTransformerResource`) took the load cycle from 16.7 s
  to 8.3 s on the 54 MB project and 4.99 s to 3.92 s on the 3.6 MB one; the ctor is now 0.06 s and what
  remains of the block is ~0.4 s per load, mostly blend-shader variants compiled once each.
- **Stage 3 - pages**: the pool, the table, analytic demand, the coarse chain and the eviction of
  §5, behind a switch. The switch is **a control, not a crutch**: it exists so the gates can compare
  the page path against the base-level path on the same frame, and so the two can be A/B'd for `ms`.
  It is not shipped as a user-facing fallback. **Removal criterion**: every gate in §5.6 green on the
  small, the mid-size and the 15000² project, and the counter showing no regression (residency,
  uploads within cap, `ms` within budget) - then the switch and the base-level draw path are
  deleted. If a project cannot pass, that is a design bug to fix, not a reason to keep the switch.
  Gate: residency tracks the screen-derived bytes on the 15000² project **while playing**, pixel
  equality against a full-chain reference, frame time inside budget.
- **Stage 4**: masks in the pool (incremental page updates from the stroke). The blocker found
  when scoping it: a mask's levels ARE a GPU texture built incrementally by `MaskMipChain`, so
  paging needs the level boxes from a source that is not the full texture - a CPU pyramid (the
  chain's area rule is deliberately the "single rule on the CPU", but its x pass stores to an R16F
  scratch, so bit-exactness is a gate question) or a transient box pipeline. Neither is built; a
  4096² mask measures 21.3 MiB of R8 chain. `~/Downloads/kemono_psd_1` supplies real masked PSDs
  (000145.psd: 24 masks, rendered paged vs control at max 1/255), but its masks are decorative and
  small; the memory target is a large-canvas painted mask.
- **What to skip**: a per-layer level manager as a shipping state. It is throwaway once pages land,
  and Stage 3's coarse fallback gives the same behaviour at the right granularity. The one useful
  checkpoint is Stage 2 + "upload the whole needed level" as a *debug path* to validate upload and
  eviction plumbing without the page table.

## 7. Moving FFD to the GPU

Today the CPU blends FFD key positions into `expans.ffd().positions()` and uploads them; the pass then
skins. Worst case (animated FFD on a 15000² layer) that is ~890 k vertices walked on the CPU and
~10 MB/frame uploaded **[D: from `kDefaultMeshCellSize = 16`]**, which is the cost the GPU pass
exists to avoid.

Proposal: one pass does FFD blending (weights/positions in a buffer) and then SRT + skinning, so the
CPU stops touching vertices per frame. The CPU keeps the authoring copy for editing (a drag needs
positions: a readback of the edited mesh, or a small CPU mirror updated on edit, not per frame).
Consequences to design: the density meter and the clipper bounds read the same output; the
`TransformKey` cache's inputs change (the blend state moves into the pass).

Gate: fixture parity between the CPU blend and the GPU blend (bit-exact where the arithmetic is the
same, tolerance documented where it is not), playback cost from the counter, and drag latency.

## 8. Clipping, with GPU-side deformation measurements

Today clippers are singulated into `ClippingFrame` (a mask texture) and clipped layers draw through
it. There is no scissor and the clipper's own bounds are not used: the singulation pass and the frame
clears run at full device size.

With the deformed positions on the GPU (and the same reduction the density meter uses) a clipper's
**screen AABB** becomes available per frame. That allows: (a) scissoring the singulation pass and the
clipped layers to the AABB, (b) skipping clipping entirely when the AABB misses the viewport, (c)
smaller clears, (d) a precise cull for every layer, not just clippers - the same AABB signal answers
the "no screen-bounds test exists" gap in §1.

Costs: one reduction per clipper/layer per frame (the density meter's second attachment is the same
shape of work), and a policy for the one-frame staleness of an asynchronous readback. Gate: pixel
equality against the current path, and the counter showing the pixels saved.

## 9. Open questions

- **Anisotropy** (§3): metric fields, sampler support, ratio behaviour beyond the hardware limit.
- **Filters that need sharper-than-screen input**: a blur or wave samples the layer texture at the
  frame's resolution; a page/level chosen from the screen can starve the filter. Each filter needs a
  stated demand (measure with the counter), and a policy for layers that carry one.
- **Export has no frame budget, but a hard memory budget.** "Time is free" was the wrong phrase:
  there is no *frame* budget, so a blocking upload is acceptable - but GPU memory is finite, and a
  15000² layer's base level is 900 MB, so an export that uploads every layer at full resolution does
  not fit either. The export is the same paged renderer with a different **policy**, not a different
  implementation:
  - *synchronous demand*: the analytic demand of the frame being exported is uploaded immediately and
    the draw waits for it - time is free here, latency is not a concern;
  - *a hard memory cap*: the pool evicts by LRU between layers (a layer is drawn once and never
    needed again in that frame), so the peak is the render target plus the pages of the layers in
    flight, not the whole document;
  - *graceful degradation, never failure*: if one layer alone exceeds the cap, its level is reduced
    for that export and the reduction is reported - the same blur-vs-fail choice the viewport makes,
    made loudly instead of silently;
  - the render target itself remains bounded by the output size, which is the exporter's existing
    cost and is not affected by residency.
  The export gate still empties the pool first and asserts correctness from an empty pool, and it
  additionally asserts the peak stays under the cap while exporting a project whose layers together
  exceed it.
- **Premultiplied vs straight** storage (§4).
- **Format**: the pyramid chunk, its version, and the legacy build-on-load path.
- **Budgets**: what a project may spend, per platform; the counter is the referee.

## 10. Instruments (already in the tree)

| tool | what it answers |
|---|---|
| `gl::FrameStats` (`AE_FRAME_STATS=<path>`) | per-frame `ms`, layers considered/drawn/culled, `mesh` passes, `fill_px`, live/peak texture bytes, mask bytes, density histogram and max |
| `<path>.trace` (`AE_FRAME_STATS_DENSITY_TRACE=<n>`) | per layer: mesh source, uv vs rest vs mesh vs content areas, `det_wi`, sliver counts, the worst triangles |
| `tests/density_meter.cpp` | the metric itself: 1x/2x/4x exact, and an animated case where placement changes every frame |
| `scripts/gui/probe_camera_transform.sh` | a camera move skips the pass; a frame step re-runs it |
| `tests/verify_mipmap` (branch) | chain correctness: premultiplied averages, halo, the level a footprint reads |

## Sub-pixel shimmer is Nyquist content, not the chain and not anisotropy

Root-caused 2026-11-01 after a viewer reported hair strands flickering while zoomed out and playing.

The law. Sampling one texel pair `(a, b)` with GL_LINEAR at offset `d` gives `g(d) = a + d(b - a)`, so
over a full pixel of movement the temporal standard deviation is `|b - a| / sqrt(12) = 0.2887 |b - a|`.
For a sinusoid of frequency `w` the modulation is `|(1 - d) + d e^{iw}|`, which at the sampling Nyquist
(`w = pi`) is `|1 - 2d|` - the signal goes to ZERO at half a pixel. Content at the pixel frequency
therefore blinks at 100 % amplitude, whatever produced the image.

Measured on 000022, level 0 sampled 1:1, an 8-offset sub-pixel sweep: the std against adjacent-texel
contrast has slope **0.2859** (predicted 0.2887, 1 % apart) and correlation **0.999**; the 2.9 % of
texels whose neighbour differs by more than 40 of 255 carry **40.6 %** of all the shimmer - the hair
highlights and strand edges, exactly where it was seen. A 2x coarser representation sampled at the same
size shimmers **1.6x less**, which is the box filter's response at Nyquist.

Two things it is NOT. Not a mip property: level 0 sampled at its own size shimmers, with no chain
involved and no shrink at levels 0-1. Not anisotropy: a uniform zoom gives a square footprint (ratio 1),
where anisotropic filtering is a no-op by definition (it is on, 16x, `ImageKey::layerMaxAnisotropy`).

The only levers are those that change the RECONSTRUCTION, and the reconstruction turns out to be the
actual fault - a viewer's eye said so before the numbers did ("sharp at some offsets, blurrier at
others"), and the first metric I used hid it.

WHAT BILINEAR ACTUALLY DOES. At 4x minification, level 2 IS the exact 4-texel box, so sampling it poses
no chain question at all. Sharpness of the render (mean horizontal gradient) against the truth - the base
area-integrated over the moving footprint - as the art slides by fractions of a pixel:

| offset | truth | bilinear |
|---|---|---|
| 0.00 | 7.476 | 7.479 |
| 0.25 | 7.476 | 6.205 (-17 %) |
| 0.50 | 7.468 | 5.690 (-24 %) |
| 0.75 | 7.478 | 6.192 (-17 %) |

Bilinear does not TRANSLATE a sub-pixel shift, it BLENDS two texels: at half a pixel it is a two-texel
box, 24 % softer. The truth's sharpness is constant to 0.1 %. That oscillation of sharpness with phase
is what the eye calls roughening or flicker, and it is a defect of the filter, not of the content.

At the shipped LOD (bias -0.5 = a 50/50 blend of two levels) the swing is 14 %, and a 4-tap Catmull-Rom
read cuts it to ~3 % (with +7 % overshoot at the on-centre phase) for 16 taps per pixel instead of 4:

| offset | truth | bilinear | bicubic |
|---|---|---|---|
| 0.00 | 7.476 | 7.475 | 8.008 |
| 0.50 | 7.468 | 6.398 (-14 %) | 7.227 (-3 %) |

WHY THE FIRST METRIC WAS WRONG: mean per-pixel temporal standard deviation scored bilinear at 2.231 and
the truth at 2.540 - it rewards blur, because a blurred image changes less between phases. A metric for
this has to measure sharpness CONSISTENCY across phases, not variance, and it should be read against the
truth per phase. Anything that only reports variation will keep recommending blur.

## The sampling rule this line of experiments converged on

`tools/render_probe` made every one of these measurable on the real artwork (deterministic, one output
frame per project frame, no display - `QT_QPA_PLATFORM=offscreen`). What holds up, with the numbers:

- **level dominates.** With the sampling held fixed, level 0 gives error 0.18 of 255 against a 4x
  supersampled reference, the shipped trilinear blend (LOD 0.6, i.e. 40 % level 0 + 60 % level 1) gives
  1.96 even with 16 taps per level, and level 1 gives 0.90 at its best. The tap count, once the level is
  right, is worth 0.18 vs 1.15 (2 taps) vs 0.46 (3 taps).
- **2 taps at the finest level already halves it**: 4 texel fetches (the same count the hardware spends)
  take the error from 2.18 to 1.15; 3 taps (9 fetches) reach 0.46; 16 taps 0.18.
- **level transitions must be a blend, not a `ceil`.** Sweeping zoom, a discrete level choice reverses
  its sharpness trend exactly at zoom = 1/2^(L+1) (-12 % then -20.5 % at 0.5; -24 % then -37.6 % at
  0.25); blending two properly-tapped levels is monotone (-12.3 %, -12.1 %; -23.4 %, -26.9 %).
- **anisotropic footprint** (layer stretched 4:1 in the loaded project): candidate 0.26, hardware with
  16x anisotropy 0.40.
- **deformed layer** (a MeshKey built from the layer's own grid, displaced; candidate error 0.25,
  hardware 0.41).

The mechanism the numbers point at: pick the level from the footprint, take 2 taps per axis on each of
two adjacent levels, blend by the LOD fraction - 8 texel fetches, ~0.2 ms per screen-covering layer at
1080p, no LOD bias needed.

TOOL TRAPS FOUND WHILE MEASURING (each cost a wrong answer): `--zoom` is an absolute screen scale and
must be multiplied by the supersample factor, or a supersampled comparison is framed differently; a
shader edit does not rebuild `assets.qrc`, so `touch assets/assets.qrc` is required; a MeshKey must be
installed with `TimeLine::adoptKey` (the MAP) because the mesh blend reads `map(TimeKeyType_Mesh)`,
never the default-key slot; and a perturbation made after load must emit `Project::onTimeLineModified`
AFTER the Driver is constructed, because the Driver is what listens.

**IMPLEMENTED, MEASURED ON THE REAL PROJECT, AND REVERTED (2026-11-02).** The 4-tap path was built: the
layer fragment shader samples `sampleLayer()` (Catmull-Rom, 4 taps per axis on each of the two levels the
LOD falls between, hardware read below one texel of footprint), with `ImageKey::sharpLayerEnabled()` and
`AE_LAYER_SHARP` as the switch and the LOD taken from the layer's transform. Measured by dumping the
scene FBO during playback of a real project (12 frames, 603x679, layers at ~0.3 scale - hand-drawn
strokes, i.e. sub-pixel features):

| | mean rendered sharpness | per-frame std | per-pixel temporal std |
|---|---|---|---|
| 4-tap reconstruction | **4.430** | 0.018 (0.4 %) | 6.871 |
| hardware trilinear | 4.024 | 0.020 (0.5 %) | 6.601 |

So it is 10 % sharper and no steadier on actual content - the per-frame sharpness varies by half a
percent either way, because the artwork moves far more between frames (7.6 of 255) than the phase effect
it was built for. The owner's verdict, watching both builds side by side: "not really better in motion".

And it had a correctness defect: the LOD came from `-log2(cameraScale x layerScale)`, which is the
IMPOSTOR pattern - valid only for a planar, uniformly scaled draw. A layer with FFD, bone or pose
deformation has a different texel-to-pixel ratio across its own surface, and screen DERIVATIVES are the
mechanism every GPU and engine uses for exactly that reason (they are per-pixel correct under
deformation; they wobble with the sub-pixel phase, which trilinear blending is smooth enough to hide).
The app already computes the correct local scale per triangle (`s_max` of the uv->mesh Jacobian) in the
density pass, but only into a histogram, not into anything the draw can read.

Reverted for both reasons: no perceptible gain, and an LOD rule that would be wrong on every deformed
layer. What survives is the measurement: bilinear really does blend instead of translating (24 % of the
rendered gradient within a pixel of travel at 4x), the truth does have its own temporal variation, and a
4-tap read costs +0.43 ms per screen-covering layer at 1080p. If this is ever revisited, the LOD must
come from the per-triangle Jacobian, not from the transform.



WHICH FILTER, AND WHAT IT COSTS. Measured on the same phase sweep; "swing" is the peak-to-peak change of
rendered sharpness across a pixel of travel (the visible artefact), "err" the mean absolute difference
from the truth, "fetches" the texture reads per pixel (the built-in trilinear is 8 at a fractional LOD,
4 at an integer one):

| filter | fetches | swing | mean \|err\| |
|---|---|---|---|
| bilinear, one level | 4 | 23.9 % | 1.869 |
| two bilinear samples straddling the footprint ("box2") | 8 | 5.1 % | 1.869 - but uniformly at 6.0 sharpness against the truth's 7.48 |
| Catmull-Rom 4-tap, one level | 16 | 12.8 % | 1.692 |
| bilinear on both levels (ships) | 8 | 14.4 % | 0.913 |
| cubic on the dominant level, bilinear on the other | 20 | 14.9 % | 0.845 |
| Catmull-Rom (sharp cubic) on both levels | 32 | 9.7 % | 0.914 |
| Mitchell (B = C = 1/3) on both levels | 32 | **6.9 %** | **0.851**, at 6.71 sharpness against the truth's 7.47 |

The two 32-fetch kernels are the final dial: Catmull-Rom keeps the sharpness closest to the truth (7.23
against 7.47 at half phase) with a 9.7 % swing; Mitchell is the steadiest (6.9 %) and about 10 % softer.
Both beat bilinear on the axis that was visible (sharpness at half phase: 6.40 for bilinear).

Three conclusions. (1) 4 taps per axis is the cheapest INTERPOLATING kernel - a 3-tap kernel is a
B-spline, i.e. an approximating one, and measured it is soft everywhere (sharpness 4.8 -> 1.7 across the
sweep), so "cheaper than 4 taps" means "blurrier", not "as good for less". (2) The 20-fetch hybrid buys
nothing: the bilinear half still blends, and the swing stays at 14.9 % against 14.4 %. (3) The "box2"
trick removes the swing by being uniformly soft - the same trap the temporal-std metric fell into.

So cubic on both levels (32 fetches when the LOD is fractional) is the one that works, and its cost can
be gated: the layer's scale is known on the CPU, so the 4-tap path only needs to run when the layer is
MINIFIED (or rotated); magnification is exact with bilinear. A non-uniform scale can drop to 4 taps on
one axis only.

WHAT IT COSTS, MEASURED rather than estimated (`/tmp/texbench`: EGL pbuffer, 1920x1080, 200 frames per
case, a 2048^2 mipmapped texture on an AMD R9 380 / Mesa 26.2.2). The baseline matters: what ships today
is ONE trilinear `texture()` call, which the hardware implements internally - my first benchmark compared
against "8 taps" that the sampler never takes:

| sampling | ms per screen-covering layer (1080p) |
|---|---|
| 1 trilinear `texture()` - what ships | **0.196** |
| 2 `texture()` (the two levels of a trilinear blend) | 0.199 |
| 32 `texelFetch` (4-tap per axis on both levels) | **0.630** |
| 32 `textureLod` (filtered taps - do not do this) | 1.194 |
| 16 `texelFetch` (4-tap on one level) | 0.328 |

So a 4-tap implementation costs **+0.43 ms per screen-covering layer** at 1x coverage (0.85-1.7 ms at a
character sheet's 2-4x overdraw) - and, more usefully, it bounds the whole question: the ENTIRE texture
sampling of a full-screen layer is 0.2 ms of a 16.7 ms frame today, so sampling is not where this app's
frame time goes and the addition cannot move it much either way. Use `texelFetch`, not `textureLod`:
asking the hardware to filter each of 32 taps doubles the cost for nothing.

These are synthetic (warm cache, no blending). The figure that decides shipping is the same experiment in
the app - run the project with `AE_FRAME_STATS` on both shader variants and compare `ms`.
## Stage 3, built (2026-11-05)

**Status: implemented behind `AE_VIRTUAL_TEXTURE` (default on), with the full-resolution path kept
as the A/B control.** This section records what was built, where it differs from the design above,
and what the gates measure. The design text is left as written so the reasons survive; where the
implementation differs, the difference is stated here.

### What a paged image is now

`core::VirtualTexture` (one per `ImageKey`, owned by `ImageKey::Cache`) is three things:

1. **The coarse chain.** Every level whose longest side is at most `AE_VT_COARSE` (default 512)
   down to 1x1, as one ordinary mipmapped texture. It is always resident: a page that is not
   there samples it, so a miss is blur. It is **copied level for level** from the production
   chain, not rebuilt, so the fallback and the control cannot disagree about a texel.
2. **The page table.** One `R16UI` texture of `(pagesX(0), sum of pagesY(level))`: 0 means "not
   resident", otherwise `(atlas << 8) | (slot + 1)`. Levels sharper than the coarse chain are
   paged; the rest are the coarse texture. It is rebuilt from `mEntries` in **one**
   `glTexSubImage2D` whenever the resident set changes, after **every** resource's uploads for
   the frame (a second resource's upload can evict a first one's page; publishing per resource
   would leave a table naming a slot someone else owns).
3. **The demand map.** One `R8` texel per page of the same grid, marked by a geometry pass that
   walks each drawn layer's own triangles with the skinned positions and the camera. It is one
   LAYER of a shared `GL_TEXTURE_2D_ARRAY` (one layer per resource), not a texture per resource:
   a clear, a copy and a fence per FRAME, however many resources there are. The per-resource map
   was built first and measured - on `nene` (105 paged resources) 105 `glGetTexImage` calls and
   105 fences cost ~40 ms of a 94 ms frame, against ~8 ms for the geometry pass itself. Two PBOs
   and a fence make the readback asynchronous; a page is wanted for `kKeepFrames = 3` frames
   after it was last marked.

The **pyramid** (`core::ImagePyramid`) is the page source: levels 1..coarse-1 captured from the
GPU chain into RAM with `glGetTexImage` at key creation, level 0 generated from the resource's own
pixels (premultiplied with the upload's exact rounding) when a level-0 page is asked for. Level 0 is
not stored: it is the larger half of the chain and the resource image already holds it, so storing
it again would double the cost of every paged image for a memcpy. The capture is once per resource,
at load; nothing reads a texture back during a frame.

### The pool

`gl::PagePool` is the slot allocator, deliberately knowing nothing about images or levels:

| | |
|---|---|
| page | 256x256 content texels |
| gutter | 2 texels on every side (one filtered read reaches one texel past its centre) |
| pitch | 260 |
| atlas | 4096x4096, 15x15 = 225 slots (3900 of 4096 texels used), 60.8 MB RGBA8 |
| atlases | up to 4 (the shader's four-way branch), 243 MB |
| budget | `AE_VT_BUDGET_MB`, default 256 MB, clamped to 4 atlases |
| upload cap | `AE_VT_UPLOAD_MB`, default 8 MB/frame (29 pages); 32 MB was measured to cost 60-140 ms on the frame that spent it |
| eviction | least recently touched page; every requested page is touched before any upload |

The gutter is 2, not the design's 4, and the slots are 15x15, not 16x16: 16 slots of 260 pitch do
not fit 4096. A slot that cannot have a gutter is not a slot.

Growth is another atlas when the budget allows; an allocation that fails drops the budget to what
is already allocated and the frame falls back to the coarse chain. An atlas with no resident page
and no use for 600 frames is released, so a smaller window or a closed project returns memory
without a restart.

`PagePool` owns the pages' GL memory, not their content: the caller uploads the page's 260x260 (the
`VirtualTexture` fills the gutter from the level's neighbouring texels, or transparent outside the
resource) and then the table entry is published. A pool **generation** changes when the pool is
purged or its context is recreated; a `VirtualTexture` that sees a new generation clears its table
before sampling, so a dead slot number can never reach the shader.

### Demand

The design's "the density pass already walks the triangles" became a dedicated geometry pass,
because the density meter exists only under `AE_FRAME_STATS` and a paged renderer cannot have its
demand switched off. The pass is one indexed draw per layer, before the layer's own draw, reading
the same skinned position buffer; it does not read anything back synchronously.

Per triangle it computes `J = d(screen)/d(texel)` from the three screen positions and the three
uvs, takes `s_max` (the demand's level is the one the fragment shader's derivatives pick:
`floor(log2(1/s_max) + bias + 0.5)`), and rasterises the triangle's **page rectangle** at that
level into the demand map. The rectangle is the triangle's own uv box grown by the tap reach in
level texels - a tap centre is at most `0.25 * major` along the major axis plus `0.2 * major` of
its own block, so the page box grows by `0.5 * anisotropy + 1` page units, clipped to a page. The
pass marks **the triangle's own level and no other**: a 2x2 derivative quad that straddles two
triangles rounds to one of *their* levels, and both triangles mark their own, so no margin is
needed. One level of margin plus a full-page border was built and measured: 474 pages (128 MiB)
against 79 (20 MiB) on nene, and it did **not** bring the frame closer to the control (mean 0.83
of 255 against 0.61) - the extra coverage bought nothing, so it is not built.

The page coordinates are computed from the same `(inTexCoord + uTexCoordOffset)` the layer
shader divides by `uImageSize`, not from the mesh's raw uvs: a mesh origin offset (a custom
`MeshKey`) moves the sample by a part of the image, and demand on the untranslated uvs names the
wrong pages. The default grid mesh cancels the offset (`originOffset == imageOffset`), which is
why the first gate did not see it.

The pass runs with `uScreenSize` in **device pixels**: the camera's view matrix maps to NDC, so
the pixel scale is half the device screen. Leaving that out made every footprint read about
`log2(width/2)` levels too coarse - which is exactly the bug the first version had, and it is why
the gate asserts demanded pages, not just rendered pixels.

The array's layers are written with `gl_Layer` in a geometry shader and an FBO with the array
attached as a layered color target (GL 3.2); the per-layer grid is a sub-rect of the cell, and the
rest stays cleared. Reallocation (a new resource with a larger grid) recreates the array empty -
the map is cleared every frame anyway, so there is nothing to copy.

### The shader

`assets/shaders/LayerSampling.glsl` holds `sampleLayer` (the two-tap, one-level reconstruction) and
its `VIRTUAL_TEXTURE` fetch; both `LayerDrawingFrag.glsl` (the layer draw) and
`ClipperWritingFrag.glsl` (the clipper's singulation pass) `#include` it, which `gl::ExtendShader`
expands before the variation pass. The geometry is unchanged; only the fetch changes:

- one level texel -> page -> table -> slot -> atlas coordinate, with the gutter supplying the
  bilinear neighbours. Every tap resolves its own page: it can cross a page boundary, and the
  gutter only covers the read *inside* a page.
- the content's border is the gutter's zeros: a read within **one level texel** outside the content
  is still served by the page, because clamp-to-border blends the edge texel with the transparent
  outside at exactly that distance. Only a coordinate whose whole 2x2 footprint is outside returns
  0; zeroing every coordinate outside `[0,1]` dropped that half-texel blend for a reconstruction
  tap that steps just outside the content.
- a level the coarse chain owns (`level >= coarseLevel`) is resident in full, and is sampled
  from the coarse chain with the **same** two-tap reconstruction (the coarse chain's level index is
  shifted by `coarseLevel`).
  The coarse chain holds the full chain's own levels, so this is the control's read, not a
  fallback approximation. Taking one bilinear tap there instead was the whole residual difference
  from the control (816 px over 8/255, max 115, on nene's Layer 6 - see below); the cost is the
  control's eight fetches for those pixels, which the full path pays anyway.
- a table entry of 0 (a page **not** resident) samples the coarse chain at
  `max(0, level - coarseLevel)` with one filtered read: blur, never a hole, and never an undefined
  sampler. This is the transient case - the pages are wanted and arriving.
- magnification (`lod <= 0`) is one level-0 filtered read. Both paths state the level explicitly
  (`textureLod(..., 0)` on the control, the level-0 page on the paged path): the branch is entered
  on the *biased* level, so the unbiased footprint can still be minifying, and a `texture()` read
  would let the hardware blend mip levels (and filter anisotropically) that no page stack can
  reproduce. Under true magnification the two reads are identical.

The layer shader variant index gains one bit (`ShaderHolder`), and only pageable keys reserve the
VT programs.

**Clippers take the same sampler.** The clipper's singulation pass used the hardware's implicit-LOD
`texture()` read - a different filter from the layer's reconstruction at every clipped edge, and
one no page stack can reproduce (it blends mip levels and may filter anisotropically). It now
includes `LayerSampling.glsl`, so the mask and the layer it clips are one read. That changes the
mask's look on both paths (sharper, phase-consistent at the edge), and it is what makes a paged
clipper exact rather than approximate. The clipper's demand is marked by its own `render`, the
mask samples the same mesh, so the demand-only pass has nothing extra to do. The clipper's program
gains the same `VIRTUAL_TEXTURE` variation (four programs: `IS_CLIPPEE` x `VIRTUAL_TEXTURE`), and
`renderClipper` binds the coarse chain to unit 0 when the layer is paged.

### What is deliberately not paged yet

- **Masks are not paged.** `core::MaskKey` still has its own mip chain and texture (Stage 4).
- **The pyramid is per ImageKey, not per resource.** Duplicated layers that share an image still
  build two pyramids. Sharing is a RAM optimisation with a lifetime question (the resource may be
  reloaded under both keys); it is not needed for correctness and is not done.
- **The exporter has a different policy, not a different renderer.** A frame is written once, so
  the exporter runs a `RenderInfo::demandOnly` pass for that frame's camera, calls
  `VirtualTexture::synchronize()` (wait on the fence, map, upload), and then draws. The first
  exported frame is as sharp as the last; the viewport keeps the asynchronous two-frame lag.

### Controls

| variable | meaning |
|---|---|
| `AE_VIRTUAL_TEXTURE=0` | the full-resolution control path (the gates' A/B) |
| `AE_VT_BUDGET_MB` | pool budget, default 256, clamped to 4 atlases (243 MB) |
| `AE_VT_UPLOAD_MB` | per-frame upload cap, default 8 |
| `AE_VT_COARSE` | longest side of the always-resident chain, default 512 |
| `AE_VT_STATS=1` | one line per frame: resources, wanted, resident, pool pages/bytes |
| `AE_VT_DEMAND=0` | diagnostic: skip the demand pass and its readback (every paged layer samples the coarse chain through the VT shader) - the control for "is the cost the shader or the demand machinery" |

`AE_FRAME_STATS`'s line gains `vt_pages`, `vt_bytes`, `vt_up`, `vt_evict`.

### Gates (all in `tools/verify_mipmap`; run it under Xvfb like the other GL gates)

- **V1 paged layer vs the full-resolution control**, at scales 0.25, 0.0625x0.25 (a 4:1 footprint
  on the coarse tier), 0.3 (level 0 paged, the layer's edges on screen), 0.5, 0.6 (the magnified
  branch entered while the unbiased footprint is minifying) and 1.0, plus a no-holes case that
  purges the pool and forbids a new atlas. The paged and control frames are compared pixel for
  pixel; the scale sweep also asserts how many pages the paged levels demand, that all of them are
  resident after eight frames, and that the pool has not silently served the frame from the coarse
  chain. Tolerances are 1 unit (2 for the on-edge 0.3 case): the fixture measures 0 for the
  paged and magnified cases under `gpu_headless` and 1 on the Xvfb/xcb path's sub-texel rounding,
  while each bug class measures 23-53 on it (see Measured).
- **CL1 clipper singulation**: a 900x700 disc clipper over a 900x700 opaque clipped layer, both
  paged, against the full-resolution control. Asserts both keys took the paged path, the clipper's
  pages were demanded and are all resident, the paged pair is within 2/255 of the control (the
  disc's mask edge), and the mask is doing something: the clipped frame leaves a corner empty that
  an unclipped render covers. Without the clipper-demand check the gate would pass with the mask
  served from the coarse chain.
- **P1/P2 page pool**: slot 0 at the gutter origin, slot 1 one pitch over, slot 15 the first of
  the second row, a slot's texels read back exactly; the per-frame upload cap; eviction order and
  the owner callback; cancel returning a slot and refunding its bytes; purge changing the
  generation and releasing every atlas.
- `media_exporter_e2e` runs the export path with the demand preparse.

### Measured

- **nene.anie** (15000x15000, 130 layers, now 108 paged resources - the three clipper layers
  included, 640x640 fit view), the same frames with `AE_FRAME_STATS`, after the demand set
  settled:

  | | full resolution | paged |
  |---|---|---|
  | `tex_live` | 2,778,072,136 B (2.59 GiB) | 133,814,840 B (128 MiB) |
  | steady `ms` | 93.5 | 96.4-98.0 |
  | load (project load + frames, wall) | 79.4 s | 74.9 s |
  | max RSS | 8.08 GB | 8.55 GB |

  The VRAM is **20.8x lower**. The paged `tex_live` is one 64 MiB atlas holding 30.8 MB of pages,
  the always-resident coarse chains, and the images too small to page: the **443 MiB of clipper
  layers** (one 4343x4351, two 2702x12604) that dominated the old 569 MiB figure are now paged.
  They add 35 pages (114 in all) and 115 MB of pyramids (summed from their level sizes), and the
  paged RSS measured 8.55 GB - lower than the 8.63 GB from before they were paged, although the
  pyramid total rose from 609 to 724 MB; that measurement is one session's, and the row's control
  figure is an earlier one.
  The paged steady state is 96.4-98.0 ms (the last three frames of a `--frames 3..10` run, after
  all 114 pages landed) against the control's 93.5 ms for the same frames and view. The ~3 ms gap
  is compared across sessions (a later same-session control run was unusable under the machine's
  browser load) and includes the clipper pass's new reconstruction, which now costs the layer
  draw's eight taps over the masks' screen area instead of the old single hardware read.
  The demand is the triangle's OWN level and a page box grown by the tap reach in level texels, not
  by a whole page. The first version marked the level either side and a full-page border: 474
  pages (128 MiB) on nene, and (measured before the coarse-tier fix) it did **not** bring the frame
  closer to the control (mean 0.83 of 255) than the tighter set did (0.61). The page set is 114
  (the layers' 79 plus 35 the clippers demand); at the 8 MB/frame default they land in four frames
  (31/62/93/114 measured), with the landing frames at 100-104 ms against the 96-98 steady. A 32 MB
  cap was measured first and is why the default is 8: the frames that spent it cost 60-140 ms. The
  page fill copies the level's rows (one memcpy per row) instead of a memcpy per texel, which was
  most of that upload frame.
  The pyramids are 609 MB for the layer resources plus 115 MB for the three clippers (summed from
  their level sizes); a clipper no longer rebuilds a full chain and capture when the policy
  changes, because there is no policy. Load time did not regress (74.9 s against 79.4), and the
  pyramid capture is SYNCHRONOUS: it cost 1.47 s across the 136 keys, worst 130 ms for one
  3841x12658 image (a 15000² import scales to ~0.6 s, inference from the 48 MP sample). The three
  clippers add ~87 MP of capture, ~0.23 s at that rate (inference).
  **The residual difference was the coarse tier, not the page path.** With `AE_VT_DEMAND=0` (no
  page uploaded at all, every paged level falling back to the coarse chain) the same-frame
  difference from the control was byte-for-byte the same 816 px over 8/255, max 115; with every
  wanted page resident it was the same again; and the paged frame against the no-demand frame
  differed by max 2, mean 0.0023 of 255 - the atlas read was already exact. The shader was taking
  ONE bilinear tap for levels the coarse chain owns, where the full path runs the two-tap
  reconstruction: a different filter, visible only on fine hatching. The coarse tier now takes the
  same eight taps from the coarse texture, at the level index shifted by `coarseLevel`. Isolated to
  Layer 6 exactly (`--only-layer '=Layer 6'`, 4408x4370, 640x640) it is **bit-identical**. The
  magnified-branch read below closed the last 1-unit edge pixels, and the whole 640x640 frame now
  measures **0 pixels over 8/255, max 1, mean 0.00041 of 255** (168 pixels differ by 1) once the
  114 pages have landed (frames 7-8). The landing frames (5-6) show up to 3072 px over 8 - the
  page transient, not a sampling difference. The plan's pixel-equality gate is met on this project,
  clipper layers included.
- **V1 parity**: 900x700 source, 320x320 canvas, coarse level 1, 12 level-0 pages wanted and all
  resident. The source is deterministic per-texel **noise**, not the old smooth curve: a ramp
  hides a filtering difference in 1/255 rounding, noise turns the same difference into tens. At
  0.25 (isotropic) and 0.0625x0.25 (4:1, level 1) - both coarse-tier cases - the paged and control
  frames measure **max 1 and max 0**; at 0.5 and 1.0, **max 1 and max 0** (over the interior; the
  layer is larger than the canvas). The 1-unit pixels are the atlas read's sub-texel rounding on
  the Xvfb/xcb path: under `gpu_headless` the 0.5 and 1.0 cases measure max 0, so the tolerance
  is 1 for driver rounding, not for the read. 0.3 (level 0 paged, the layer's own edges on screen, every border
  tap exercised) is **max 2** on the edge row; 0.6 - a footprint of 1.67 texels, where
  `lod = log2(minor) - 1.35 <= 0` enters the magnified branch while the unbiased footprint is
  still minifying - is **max 1**. The gate has power: with the two old reads temporarily restored
  the same source measures **max 23 / mean 1.495** (the coarse single tap), **max 53 / mean 0.883**
  (the 4:1 coarse case) and **max 31 / mean 4.429** (the implicit-LOD magnified branch), so the
  1-unit tolerances excuse only driver rounding, not a wrong read.
- **test.anie** (six paged resources, 640x640 fit view): 52 pages, 14,060,800 B of pages in one
  atlas, all wanted pages resident after the second frame; steady `ms` 4.9-5.1 against the
  control's 4.5-6.2. Same-frame paged vs control: **bit-identical, 0 pixels differ**. The
  6873-px difference that used to be here (mean 0.076, max 43; mostly 1 unit across each layer,
  with full-width rows at the layer edges) was the magnified branch: with the -1.35 bias it is
  entered while the hardware LOD can still be minifying, and the control's `texture()` then
  blended mip levels (with anisotropy) where the page path read level 0. The control now takes
  `textureLod(..., 0)` - the same read as the page path, and the same read as `texture()` under
  true magnification - so the band is gone rather than excused.
  `tex_live` on this small project goes UP, 33.0 MB -> 87.4 MB, because the pool's first atlas is
  a fixed 61 MB; that floor is the price of the scheme on projects whose whole texture set is
  smaller than one atlas. A 2048 first atlas was built and measured and is not the answer: 57
  pages then needed a second 4096 atlas for 8 pages, 77 MB against 61 MB.
- **Masks** (`MA1`): a 900x700 layer mask on a *paged* layer matches the full-resolution control
  within 1/255 (mean 0.047), and the mask is proven active (its hidden half is empty, its revealed
  half covered). The mask's cost is its R8 chain: 900x700 = 839,797 B, **4096x4096 = 22,369,621 B
  (21.3 MiB)**. A 15000² mask is ~300 MB by the same arithmetic (the design's 225 MB was level 0
  alone).
- **A real masked PSD** (`~/Downloads/kemono_psd_1/000145.psd`, imported through the app's
  `ctrl::ImageFileLoader`): 24 masked layers; paged vs the full-resolution control is **0 px over
  8/255, max 1, mean 0.00017**. That dataset's masks are small (the masked layers sum to ~2.2 M
  texels, ~2.9 MB of chains by their content sizes), so it is functional coverage, not the memory
  target; the target remains a large-canvas painted mask.
- **Clippers**: nene's three clipper layers are paged (443 MiB of full textures gone; 35 more
  pages, all resident; 0 px over 8 on the steady clipped frames). The synthetic `CL1` pair - a
  900x700 disc clipper over a 900x700 opaque layer, both paged - is **max 2, mean 0.003 of 255**
  against the full-resolution control, the 2 units on the disc's mask edge (the atlas read's
  sub-texel rounding on a coverage field); the clipped layer is demanded (not left to the coarse
  fallback), the mask is proven active by an unclipped render that covers the corner the clipped
  one leaves empty. The mask's read changed from the hardware's implicit-LOD `texture()` to the
  layer's own two-tap sampler on BOTH paths: that is what makes paging exact, and it removes the
  old mismatch between the mask's filtering and the clipped layer's.
- **Export**: `media_exporter_e2e` passes with the demand preparse; the first exported frame is
  the one that exercises it.

### Open, in priority order

1. The demand pass's ~8 ms on 130 layers: it re-walks every visible triangle every frame even
   when the camera and the transform cache have not changed. A static view could skip it; the
   density meter already reads the same positions, so the two could share one pass under
   `AE_FRAME_STATS`. Beside it, the pool's landing rate: 8 MB/frame still costs 10-20 ms while it
   fills, and a large import's synchronous capture is a visible pause; an asynchronous/deferred
   capture and a page upload batched per atlas row are the two obvious next reductions.
2. Masks (Stage 4).
3. Per-resource pyramids for duplicated layers.
4. The first-atlas floor (61 MB) for projects whose whole texture set is smaller than it: a
   demand-aware first allocation is the measured-shaped answer (see the pool's header comment).

The parity gaps are closed - the nene residual (the coarse tier's single tap) and the test.anie
band (the control's implicit-LOD read in the biased magnified branch) - and the clipper layers are
paged through the same sampler, with `CL1` gating a paged clipped pair. The pixel-equality gate is
met on both projects.
