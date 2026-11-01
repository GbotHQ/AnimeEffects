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

## 5. Residency and upload

- **Unit**: a page (start at 256²) at a level, in a pool; a page table per resource maps (level, x, y)
  to a slot. On GL 4.0 there are no sparse textures: the pool is a 2D atlas, the indirection is a
  small texture sampled in the layer shader, and the fallback for a page that is not resident is the
  resident coarser chain of the same resource (never a hole).
- **Demand**: a feedback pass records which pages the frame sampled; priorities = visible area x
  deficit (levels below what the metric asks for).
- **Scheduling**: a per-frame upload budget (bytes), a quality floor expressed in "levels of
  deficit", and no sync: uploads are state changes, not readbacks.
- **Eviction**: capacity-driven, never change-driven. The cost asymmetry is the policy: a **sharp**
  page can only come back from RAM (an upload), a **coarse** page can be regenerated from any
  resident sharper page at almost no cost - so protect sharp pages, let coarse ones go. This is also
  what makes oscillation ("the layer jumps between two levels") free: both stay if they fit, and the
  budget is what decides, not hysteresis.
- **A 10x jump in one frame**: the frame samples what is resident (coarser than ideal: blur, not a
  hole), the deficit goes to the top of the queue, and the budget bounds how fast the sharp pages
  land. No N-frame rule; no oscillation stall.
- **At 1:1** the deepest zoom needs the viewport's pages, not the base level: that is the ceiling
  that disappears.

## 6. Staged path

- **Stage 1 - sampling foundations** (low risk, immediate quality): a real mip chain per texture
  with the branch's rules, correct MAX_LEVEL bookkeeping, `GL_TEXTURE_LOD_BIAS = -0.5`, and
  anisotropic filtering where the extension exists. Adds 33 % to resident memory; removes the
  minification aliasing. Gate: `verify_mipmap` (exists) plus an anisotropy case.
- **Stage 2 - the pyramid**: resources carry levels; generated at import/load, stored, decoded into
  RAM; no GPU change. Gate: format roundtrip, memory accounting, legacy load.
- **Stage 3 - pages**: pool + table + feedback + fallback + eviction behind a switch, with the
  current base-level path as the control. Gate: residency tracks the screen-derived bytes on the
  15000² project **while playing**, pixel equality against a full-chain reference, frame time inside
  budget.
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
- **Export**: renders at output size, may need base levels; a separate demand, and it must not
  thrash the pool the viewport is using.
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
