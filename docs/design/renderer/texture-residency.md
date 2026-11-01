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
- **Storage**: in the project, as an optional chunk (new format minor). Legacy projects build it at
  load; imports build it at import.
- **RAM**: decoded at load into RAM - all levels, coarse ones cost 1/3 of the base - so that no
  frame ever decodes. `nene` goes from ~1.94 GiB to ~2.6 GiB of decoded images **[D]**.
- **Content rules**: the `feat/mipmapping` branch's - premultiplied, halo-free, levels past the
  alignment cap rebuilt on the CPU, `GL_TEXTURE_LOD_BIAS = -0.5` as a policy on top.
- **Masks**: not low frequency (hard edges and painted texture are legitimate). Their pyramid is
  maintained **incrementally from the stroke's dirty box**, level by level, never regenerated whole.
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
- **Stage 2 - the pyramid**: resources carry levels; generated at import/load, stored, decoded into
  RAM; no GPU change. Gate: format roundtrip, memory accounting, legacy load.
- **Stage 3 - pages**: the pool, the table, analytic demand, the coarse chain and the eviction of
  §5, behind a switch. The switch is **a control, not a crutch**: it exists so the gates can compare
  the page path against the base-level path on the same frame, and so the two can be A/B'd for `ms`.
  It is not shipped as a user-facing fallback. **Removal criterion**: every gate in §5.6 green on the
  small, the mid-size and the 15000² project, and the counter showing no regression (residency,
  uploads within cap, `ms` within budget) - then the switch and the base-level draw path are
  deleted. If a project cannot pass, that is a design bug to fix, not a reason to keep the switch.
  Gate: residency tracks the screen-derived bytes on the 15000² project **while playing**, pixel
  equality against a full-chain reference, frame time inside budget.
- **Stage 4**: masks in the pool (incremental page updates from the stroke).
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

Running a big project requires a guard, not good intentions:
`systemd-run --user --scope -p MemoryMax=… -p MemorySwapMax=0` plus an RSS watchdog, on a headless
display. The 15000² project does not load under software GL inside 9 GB - measure it on the GPU, in a
bounded window.
