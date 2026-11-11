# Bone influence feedback: the weight heatmap and bone-scoped painting

Status: **landed 2026-11-11.** No format change. This is the feedback half of
influence editing; it does not author weights.

Scope: how the built envelope weights are shown while a bone is edited, how a
bone becomes the target of the brush, and the one overlay-engine capability the
display needed. The influence cache itself is covered by
`docs/design/skeletal/influence-cache-plan.md`.

## 1. The gap

The Bone editor's envelope tools say **where** a bone reaches (the filled
polygon) but not **how strongly**: the envelope's falloff is only visible when
the layer is skinned and posed. The painter's notes ask for a heatmap next to
"proper weight painting", and the cache work (rebuild 3.4-4.5x cheaper, 3.3-5.7x
smaller) is what makes reading the built map every painted frame practical.

The two halves landed here are deliberately separate:

- the **view** reads `BoneInfluenceMap`'s stored arrays; it creates no cache and
  changes no data;
- the **target** is one bone, so the brush stops widening every envelope whose
  polygon passes under the stroke. This is a behaviour change in the two
  influence paint modes, stated in §3.

## 2. What the heatmap is

For one selected bone, over every layer the key skins, each map vertex is drawn
at the bone's **normalised weight**:

- the ramp is blue (0) -> cyan -> green -> yellow -> red (1); weights below
  `0.002` are not drawn, so an uninfluenced area stays transparent instead of a
  blue wash;
- the point is 6 px, alpha 150, quantised into 16 colour buckets so the painter
  emits one point-cloud call per bucket (per 8192-vertex chunk), rather than one
  per vertex;
- a mesh above 120,000 vertices is sampled on a stride; the field is smooth
  enough that the samples describe it.

The position is **the build's own matrix**, not an approximation:
`cache->innerMatrix()` translated by `TimeKeyBlender::getOriginOffset()`, then
the key owner's target matrix and the camera. `BoneKey::updateCaches` transforms
the mesh positions by exactly the same product before the weights are computed,
so a point sits where its weight was measured. A cache whose map or mesh has a
different vertex count is clamped to the smaller one; a map with no influence
slots is skipped.

The weight lookup is `BoneInfluenceMap::Accessor::weight(bone, vertex)`. It is
the only reader that knows both storage forms:

- one slot: the index array only, and the weight is the implicit `1`;
- two to seven slots: slot-major `uint8` index plus `float32` weight;
- eight slots: the dense two sets of four.

Unused slots are `(index 0, weight 0)`, so the same first-match rule returns bone
0's real weight when bone 0 has one and zero when it does not. The bone index is
`BoneKey::Data::boneIndex(bone)`, the pre-order index over the top bones - the
same order `BoneInfluenceMap::makeBoneList` pushes. A bone outside that list is
`-1`; a bone past the map's cap is not shown.

The heatmap is drawn **before** the envelope polygons and the skeleton, so the
weight field never hides the rig.

## 3. How a bone becomes the target

- Hovering a joint or a bone connector focuses it (the existing `bone::Focuser`)
  and previews its field; the last hovered bone stays in the display after the
  pointer leaves.
- **Right-click selects**: the bone turns the selected ink and the heatmap stays
  on it. Left is always the stroke, so a press that starts painting can no
  longer also mean "select this bone".
- In **Paint influence** and **Erase influence**, the brush now edits **only the
  selected bone**. If no bone is selected when a left stroke starts, the nearest
  envelope to the stroke's start point is selected first
  (`BoneKey::Data::nearestBone`; parentless roots carry no envelope and are
  skipped). The additive/erase arithmetic, the shared brush and the one-command-
  per-stroke undo are unchanged - only the population changed from "every bone
  whose polygon the brush touches" to "the bone you chose".
- **Adjust influence** (the handle mode) keeps its own selection and now shows
  the heatmap of the bone it is dragging (or hovering). Its editing scope was
  already one bone.

The new tool hint ("Right-click a bone to target it; the brush edits only that
bone") and the **Show weights** toggle are in the Bones panel. The toggle is on
by default and offered only in the three influence modes.

## 4. The overlay capability

`QPainter::drawPoints` used to loop `gl::PrimitiveDrawer::drawPoint`, which is
one tiny 8-segment ellipse **per point**. A 347-vertex head is fine; a
262,000-vertex project is not.

`GLCorePaintEngine::drawPoints` now calls a batched
`gl::PrimitiveDrawer::drawPoints`, which schedules one `GL_POINTS` draw per slot
chunk. The pen colour is the point colour and the pen width is the point size
(`glPointSize` is applied for a `GL_POINTS` draw at flush). The overlay's drawer
holds **8192 vertices per slot** (was the default 512); eight slots of two
vertex buffers is about 1 MB. A 9,000-point cloud is two draws; a full
120,000-point field is at most one draw per non-empty colour bucket per chunk.

The gate is `tests/overlay_engine_primitives` (the GL overlay's own pixel
contract), now **12 probes**:

| probe | measured in the 400x300 harness |
|---|---|
| `points` (one 10 px point) | **100 pixels** - a 10x10 disc, proving the pen width reaches `glPointSize` |
| `points_batch` (9,000 points, past the 8,192 slot) | **14,400 pixels** over the 100x150 cell, proving the cloud is split across slots |

Renderer in that run: `AMD Radeon R9 380 Series (radeonsi, tonga, ACO, Mesa
26.2.2)`. A 1 px regression in either probe fails the gate (the 10 px point
would count about 1, not 100).

## 5. Interaction verification

The heatmap is interactive, so the committed gates cover its data
(`bone_influence_storage`, `bone_cap`) and the engine (`overlay_engine_primitives`);
the full display was checked in the real application (under Xvfb, `tart.anie`,
layer "head", Bone editor, Paint influence):

- hovering bone 1 draws its field over the face, and hovering bone 3 draws the
  local left-cheek patch - the fields differ as the weights do;
- the **Show weights** toggle off then on is **pixel-identical** to the original
  (0 differing pixels in the canvas crop);
- right-clicking a connector selects the bone and the field **persists** after
  the pointer moves off the skeleton;
- the skeleton and the envelope ink stay above the field.

This is a manual check, not a probe: the heatmap's fields depend on a live
project and a camera, and a pixel gate for the dock's layout would be a second
subject. The data path and the engine path have their own gates.

## 6. What this is not

- **No authored per-vertex weights.** The envelope is still the only source, and
  the cache remains derived and rebuilt (format 0.21). Painting a field per
  vertex, curve interpolation, multiselection and reusable rigs are the next
  workstream; they need an authored format and a bone identity that survives
  edits, so they follow the plan's versioning rule rather than being slipped into
  this view.
- **No saved state and no format bump.** A heatmap is a function of the built
  map; persisting it would be a second derived cache.
- **No change to the influence build.** The same float weights the shader uses
  are the ones drawn; the view cannot disagree with the deformation.
- **The LBS palette and the shader are untouched.** This is an overlay, not a
  skinning variation.

The questions the authored-weight phase will have to answer, recorded here so
they are not rediscovered: whether the data belongs to a `BoneKey` (per frame)
or a node (shared across keys); how a bone is named across joint/bone edits
(the current index is stable only while the skeleton is); how per-node records
serialise and resolve on load (the legacy cache used reference IDs before the ID
solver); and where painted weights sit relative to the envelope and the top-eight
normalisation.

## 7. Gates

- `tests/bone_influence_storage` - new `Accessor::weight` checks in every
  storage form: one-slot implicit 1, two-slot and five-slot sparse lookups, the
  dense groups, and a bone that is not in a vertex's slots reading 0.
- `tests/bone_cap` - `BoneKey::Data::boneIndex` (root 0, 33rd 32, outsider -1)
  and `nearestBone` (nearest segment, before-root clamp, root-only skeleton).
- `tests/overlay_engine_primitives` - the two new point-cloud probes above.
- Full Debug suite: recorded in the commit.
