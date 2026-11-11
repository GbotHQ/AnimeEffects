# Authored bone weights: painting, constraints, and the 0.22 format

Status: **Proposal, not started.** Every decision below is a proposal unless it is
marked *measured* or *existing*, and every "recommended" item is open to being
overturned before code lands. The envelope model, the influence cache and the
weight heatmap are the starting point; the product source is `notes.md` (lines 82,
92, 94, 129, 141, 143, 145) and the open questions in
`docs/design/skeletal/influence-display.md` §6.

Scope: authoring per-vertex, per-bone influence weights on top of the envelope
model, the on-disk format that makes them authored data, the brush and
point/curve tools, multi-selection, and the policy for mesh edits. Not in scope:
the skinning math, the dual-quaternion palette, pseudo-3D/perspective depth, and
a full reusable-rig export (a prerequisite is noted, the export/import is a
separate plan).

## 1. What exists, and what is missing

Existing, measured:

- `core::BoneInfluenceMap` builds a per-(BoneKey, node) weight map from
  `BoneShape` envelopes. It stores only the slots a map uses: 1 B/vertex for one
  influence (the weight is implicit), 5 B/slot for 2-7, 40 B/vertex at the
  eight-slot cap. The build is 8.6-10.8 ms in Release on `2.anie`
  (`influence-cache-plan.md` §7); it is derived state, rebuilt at load by
  `ProjectLoader` and on edits by `BoneKeyUpdater`.
- The envelope itself is authored as `Bone2::range(0/1)` and `BoneShape`; the
  Paint/Erase influence tools change those ranges (now one selected bone at a
  time, `influence-display.md` §3).
- The weight heatmap shows the **final normalised** weights of one selected bone
  over every layer the key skins, at the positions the build used
  (`influence-display.md` §2). It is a view; it changes nothing.
- The format is **0.21**. There is no authored weight data in a project.
- `TimeKey` has a stable `id()` (format 0.16) that survives map moves and undo.
  `Bone2` has no stable id: its `boneIDAssigner` is a per-serializer reference
  map for its JSON form, not an identity. The influence map's bone index is the
  pre-order position in `BoneKey::Data::topBones()` and is only as stable as the
  tree order.
- The object tree already supports multi-selection (`ObjectTreeWidget::
  selectedNodes()`), but `ctrl::Driver` has one target node (`mCurrentNode`,
  `setTarget`), and the Bone editor edits that one node's rig.
- The mesh does not carry stable vertex ids: `MeshVtx::index()` is recomputed by
  `MeshKey::Data::updateVtxIndices()`, `MeshKey` add/split/delete changes the
  order, and `ImageKey::resetGridMesh()` regenerates the whole grid.

Missing, in the user's terms:

- a way to paint an actual per-vertex weight, rather than an envelope's shape;
- a weight field editable at a few hand-placed points or curves, with
  interpolation, pinning and additive blending (`notes.md` line 143);
- weight painting and FFD working across a multi-selection (line 141);
- a place to store all of that, and a format that keeps it authored;
- eventually, reusing a painted rig in another project (line 145).

## 2. Product model

The envelope remains valuable: it gives a smooth, connected initial field and
it is what every existing project has. The painted data must be a **correction
layer**, not a replacement with an empty default.

**Proposed rule: a painted weight is an absolute override for one (bone,
vertex).**

- A vertex with no entry for bone `B` uses `B`'s envelope weight, exactly as
  today.
- An entry replaces the envelope weight for that pair, including `0.0`. A zero
  is therefore a real "pin this bone out" edit, not "no data".
- Unpainted vertices keep the envelope. A stroke only bakes the vertices it
  touches; nothing else changes.
- The existing normalisation and top-eight selection stay the final stage: an
  override participates like any other weight; a zero simply does not reach the
  slot list. The heatmap already shows the final normalised result.
- The first brush stamp reads the current final weight (the envelope if there is
  no entry), adds/subtracts/replaces with the brush falloff, clamps to
  `[0, 1]` and stores the override. This is the standard weight-paint model and
  matches the user's "flat 0, then raise the inside" example.

Why absolute overrides and not additive deltas over the envelope: the envelope
is a shape the user can still edit later; if a painted edit stayed a delta, a
bone's range change would move the painted result with it. The user is taking
ownership of that vertex. "Additive blending" in the notes (line 143) belongs to
the curve/constraint tool (§6), where several constraints combine before they
are committed, not to the stored model.

Two tools, one data model:

1. **Paint weights** (new mode): a per-bone, per-vertex brush with
   add/subtract/replace, the shared brush parameters, the heatmap, one gesture =
   one command. This is the direct tool.
2. **Weight constraints** (later phase): place points/curves with values and
   radii, box-select vertices, pin some, interpolate the rest, then commit. The
   result is the same override map, so the two tools compose.

The envelope tools stay: "Paint influence"/"Erase influence" change the broad
shape, "Paint weights" corrects individual vertices. A "Clear weights" action
removes overrides for the selected bone (or the selected vertices), returning
those pairs to the envelope.

## 3. Data model

### 3.1 A stable bone id

The influence map's one-byte index is a **GPU attribute** and must remain the
pre-order index into the built bone list. The authored data needs a different,
stable identity.

Proposal:

- `Bone2` gains a persistent `int id()` (call it `boneId()` to keep it distinct
  from the influence index). The counter follows `TimeKey`'s pattern: a fresh
  id on a new bone, `reserveBoneId()` on load so stored ids stay unique, and the
  id is serialised with the bone.
- The copy constructor and `Data` clone **preserve** the id: a key clone must
  address the same bones, and a posed shadow is still the same bone. A new bone
  (Creation mode) gets a fresh id. If a "duplicate bone" command is added, the
  duplicate gets a fresh id unless the duplicate is explicitly meant to share
  weights; that is a product decision to make then.
- 0.21 files have no bone ids. The 0.22 reader assigns them deterministically in
  pre-order (top-bone order, then children), reserves the maximum, and saves
  them. That makes an old file's weights impossible to address (there are none)
  and makes a 0.22 save stable.
- The influence build keeps a per-bone `(index, boneId)` pair; the map's
  `BoneParam` gains the id. The shader continues to see the index; only the CPU
  build and the authoring layer know the id.

The id is project-local, not globally unique. A future reusable-rig export maps
the source ids to the destination skeleton explicitly; do not assume two
projects' ids mean the same bone.

### 3.2 Where the weights live

Recommendation: **on the bone key owner node** (the node that holds the
`BoneKey`), not on a `BoneKey` instance and not on the mesh key.

- The owner node is the rig anchor. Its bone keys may change with frames, but
  the weights are a property of that rig on that node.
- A `BoneKey` instance is a time key. Storing weights there would duplicate them
  per key and make a new key start blank, which is not how skin weights behave.
- A mesh key knows vertices but not bones, and grid meshes are regenerated; the
  weights would be re-created or lost with the geometry.
- The object tree serialises each node in its own block; the owner's block can
  carry the records and resolve descendant node references through the existing
  ID solver (the pre-0.21 cache did exactly this: `aOut.writeID(node)` and
  `aIn.orderIDData`). No new global block is needed.

`core::SkinWeights` (new, on `ObjectNode`):

```
struct Entry { quint32 vertex; float weight; };          // sorted by vertex
struct BoneWeights { int boneId; QVector<Entry> entries; };
struct NodeWeights {
    util::LifeLink::Pointee<ObjectNode> node;            // or a pruned raw pointer
    int vertexCount;
    quint64 meshSignature;                               // §3.4
    QVector<BoneWeights> bones;                          // sorted by bone id
};
QVector<NodeWeights> mNodes;                             // sorted by node pointer/id
```

- Only painted entries are stored. `weight(boneId, vertex)` returns a value and
  a `bool` "has override"; the build needs the presence bit for zero.
- The per-bone entries are sorted by vertex. The build walks them as a cursor
  beside its vertex loop, so the override merge is O(vertices + entries), not a
  hash lookup per bone/vertex.
- The object is a value type for undo (copy one node's `NodeWeights` or the whole
  `SkinWeights`; a stroke is local, so the sparse map is small).
- The node records use a life-linked pointer so a deleted descendant goes null
  instead of dangling. A record whose node is null is ignored and can be pruned
  on save.

The alternative - store on the `BoneKey` - is cheaper to implement (its
serializer already has the owner and the cache list) but makes weights
per-key. The owner-node choice is the one that makes "paint once, animate many
keys" work; it is the main architectural decision to confirm before Phase 1.

### 3.3 Build integration

`BoneInfluenceMap::writeAsync(Project&, topBones, groupMtx, mesh, const
SkinWeights::NodeWeights* weights)`:

```
for each bone index i with bone id b:
    cursor = weights ? weights->entriesFor(b) : none
    for each vertex v:
        float w = envelopeInfluence(bone[i], v)
        if (cursor has v) w = cursor.weight, advance
        if (w >= FLT_EPSILON) tryPushBoneWeight(i, w)
```

- A zero override must **not** call `tryPushBoneWeight`; it removes the bone
  from that vertex.
- No weights (null or empty) takes the current code path exactly: no extra
  allocations, no change to any existing project.
- The output arrays, top-eight selection, normalisation and slot packing are
  unchanged. This is the invariant the format and the pixel gates protect.
- The build remains per key and per node; the owner's `SkinWeights` is read-only
  during the build. The cache is still derived: it is never persisted.

**Dirtying.** The weights are on the owner node, not a timeline key. A new
project event (or a small dedicated signal) `SkinWeightsModified` must reset the
caches of **every** BoneKey on that owner (all frames), not just the current key.
`BoneKeyUpdater` is the natural listener; it already owns "reset caches of this
node". A stroke pushes one command; the command's notifier emits the event.
Do not reuse a pose/move event: those rebuild only the affected key and are
also used by unrelated edits.

### 3.4 Mesh identity

Vertex indices are the storage key, and they are not stable across mesh edits.
The node record stores `meshSignature = hash(vertexCount, positions, uvs)` and
the vertex count.

- On build, entries with `vertex >= vertexCount` are ignored. If the signature
  mismatches, all entries are still loaded, but the tool warns once that the
  mesh changed and some weights may no longer line up.
- On load, a mismatched signature is **not** a load failure: the mesh is
  authored data too, and a user may have changed it deliberately. The entries
  are kept; out-of-range ones are dormant until the mesh is repaired or the
  weights are cleared.
- A save writes the signature current at save time (see §7 for what resetting
  the signature after a mesh edit means).

This is the honest v1: it preserves work, never silently shifts weights to
different vertices, and makes the mismatch visible. A remap is Phase 5.

## 4. Format 0.22

Authored data, so the minor bumps and the reader gates it. A 0.21 file has no
weight block and loads envelope-only; a 0.22 reader knows both. The existing
version guard rejects a 0.22 file in an older binary, as it does for every
newer minor.

### 4.1 Block

The block is appended to the **owner node's object block** (inside
`ObjectTree::serializeNode`, after `LayerNode`/`FolderNode::serialize`, so one
implementation covers both node types). The node's own type block is unchanged.

```
"WGT0" tag, int32 byteLength
int32 recordCount
per record:
    int32 nodeReferenceId        // Serializer::writeID, resolved by orderIDData
    int32 vertexCount
    QList<int> meshSignature     // two int32 words: the serializer has no 64-bit scalar
    int32 boneCount
    per bone:
        int32 boneId
        int32 entryCount
        per entry: int32 vertex, float32 weight (LE, as every other float)
```

- A node with no records writes `recordCount = 0`; every node gets the block in
  0.22. The block header (tag, length, count) is a small constant, not per
  vertex; measure its exact size when implementing. Use the serializer's
  existing `int`/`float`/`QList<int>` overloads rather than adding scalar types.
- The tag and byte length make the block self-describing for a future minor
  that wants to skip or reinterpret it; the reader validates both. (Authored
  data is read, not skipped, but the length is cheap and makes a corrupt block
  fail at its own boundary.)
- Counts are checked against the remaining stream and a sane maximum before
  allocation. Negative or overrunning counts fail. A non-finite weight or a
  weight outside `[0, 1]` is a corrupt record: fail the load with the record's
  node and bone named. A **vertex index beyond the stored `vertexCount` is
  tolerated**: the record is kept, the build ignores the dormant entry, and the
  tool warns once. A mesh is authored data and may legitimately have changed;
  an old index must not make the project unopenable.
- A bone id that is not in any `BoneKey` of the owner is **not** a load failure:
  bones can be deleted while their weights remain dormant for undo/save. The
  build ignores it; a save prunes it only when the user confirms a cleanup (see
  §4.3).
- `FormatVersion.h`: bump to 0.22 and add the history line. The 0.21 rule is
  the worked example: authored data may gate the reader; the influence cache
  remains derived and is never required.

### 4.2 Reader order

`ObjectTree::deserializeNode` reads the node's own block, then the weight block.
The node references are not bound yet (descendants are serialised after the
owner in the tree walk), so the reader pushes an `orderIDData` solver that
resolves each record after the whole tree is bound, exactly like the pre-0.21
cache. An unresolved id fails the load with "invalid weight node reference":
this is authored data, not a cache, so silently dropping it would lose the
user's paint.

### 4.3 Deletion, undo and pruning

- A deleted descendant's record is life-linked to null and ignored. If the
  delete is undone and the same object returns, the record becomes live again;
  if the object is gone for good, the record is dormant.
- A deleted bone leaves its entries dormant (the build ignores an absent id).
  This keeps a bone-delete undo from losing the paint. The cost is a few
  orphaned entries until the user saves; a "Remove unused weights" action can
  prune them, and the save can report the count rather than dropping data
  silently. This is a deliberate no-surprise rule, not an optimisation.
- A node moved out of the owner's subtree is not influenced any more; its
  record stays with the old owner (dormant) and can be pruned by the same
  action. A node moved in starts with the envelope only.
- `resetCaches`/`updateCaches` already walk the owner's descendants; the
  weights lookup uses the same nodes. No second tree walk is needed.

### 4.4 Compatibility tests

- A 0.21 fixture loads with no weight block; the influence hashes and the
  rendered frames are identical to the pre-change build.
- A 0.22 save/reload reproduces the in-memory sparse map exactly (entry order
  normalised by sorting at save).
- A corrupt block (negative/overrunning count, truncated entries, NaN weight,
  unresolved node) fails with a named error and no partial load. A vertex index
  beyond the stored count is not corruption; it loads dormant and the build
  ignores it.
- A 0.22 file with weights in one node and none in another round-trips both.
- The legacy 0.20 cache discard path is unaffected.

## 5. Paint weights

A new Bone editor mode, `PaintWeights`, beside the envelope modes.

### 5.1 Target selection

The Bone editor's target is the bone key owner (the rig). The **paint target
set** is the subset of its influenced layers the user wants to edit:

- the object tree's selected descendants of the owner, if any;
- otherwise every cache node under the brush (today's envelope-brush
  population, but now writing per-vertex weights).

Only nodes with an influence cache for the current key and a mesh at the current
frame are paintable. A selected node outside the owner's subtree is ignored with
one message; it is not silently painted on another rig.

The targeting interaction from `influence-display.md` is reused: right-click a
joint/connector to select the bone (left is the stroke); hovering previews its
heatmap; the first stroke with no selected bone picks the nearest envelope.

### 5.2 One stamp

For each target node, the brush stamp is transformed into the node's mesh space
through the same matrix the cache uses (the cache's `innerMatrix` translated by
the origin offset, then the target matrix; the heatmap already does this
inverse direction). For every vertex within the stamp:

```
t      = brushFalloff(distance, radius, hardness) * amount
before = entry ? entry.weight : envelopeWeight(bone, vertex)   // current final
after  = clamp(before + t, 0, 1)            // add
       = clamp(before - t, 0, 1)            // subtract
       = clamp(t, 0, 1)                     // replace
store an entry (bone, vertex, after)
```

- Reading the envelope for an unpainted vertex makes the first stroke start
  from what the heatmap shows.
- Keep the entry even when `after` happens to equal the envelope: comparing
  floats to decide storage would be fragile, and the entry is what makes the
  painted vertex independent of later envelope edits. The explicit "return to
  envelope"/"clear weights" action is what removes it, not an implicit equality
  test. The sparse map is local, so an entry that matches today's envelope costs
  a few bytes, not a vertex array.
- The target set may contain several layers; one stamp writes each layer's own
  `(bone, vertex)` entries. Each layer's mesh has its own vertex count and
  signature.
- The target set may contain several layers; one stamp writes each layer's own
  `(bone, vertex)` entries. Each layer's mesh has its own vertex count and
  signature.

### 5.3 One stroke, one command

- `cmnd::Gesture` (or a dedicated stroke command) captures the affected
  `NodeWeights` before the first stamp and rewrites one command as the stroke
  moves. On release the command holds the before/after snapshots.
- A command spanning several layers is still one command and one undo step.
- The command's listener emits the `SkinWeightsModified` event (§3.3), which
  resets the owner's keys' caches once after the stroke (and on every undo/redo
  replay). During the stroke the cache must not rebuild per sample: either
  rebuild once per painted frame after `flushPendingWork`, or defer to the
  release. The existing frame duties already have a place for this; do not add
  a synchronous rebuild in the mouse handler.
- The heatmap reads the **built** map. During the deferral it shows the previous
  frame's field; the plan's acceptance is "one frame of lag during the stroke,
  exact on the next frame", or an explicit live preview evaluated on the CPU
  without rebuilding the whole map. Start with the one-frame lag and measure;
  the influence build's Release cost (8.6-10.8 ms on `2.anie`) is the budget.
- Undo restores the before map and the command's listener rebuilds; redo the
  after. A no-op stroke pushes nothing.

### 5.4 UI

- Mode label "Paint weights"; the shared brush section; an Add/Subtract/Replace
  selector; a "Clear weights" action for the selected bone (and the selected
  vertices, if there is a vertex selection).
- The heatmap toggle already exists; it should be on for this mode.
- A readout of the current value under the cursor (the final normalised weight,
  and the authored value if different) is useful; bone names do not exist, so
  the readout names the joint by its index/path or the selected highlight.
- A "return to envelope" action for the current bone/vertices is the inverse of
  painting.

## 6. Points, curves and interpolation

This is the user's line 143: edit weights at a few hand-placed points/curves,
interpolate the rest; curves resampled first; additive blending; pinning; more
than linear interpolation.

### 6.1 Constraint model

```
struct WeightConstraint {
    QVector<QVector2D> points;   // one point, or a resampled polyline/bezier
    float weight;                // the value this constraint pins
    float radius;                // influence radius in content space
    bool pin;                    // a hard value at its points, or a soft field
    bool additive;               // add to other constraints instead of blending
};
```

- Points and curves live in the **target layer's content space** (the space the
  image, mesh, and mask all use). A curve is placed and edited in that space;
  it is resampled by arc length with the same spacing rule as a brush stroke
  before use, as the user asks.
- A constraint maps to vertices by proximity: the nearest point on the
  resampled curve, within `radius`; a pin is a hard boundary, a soft constraint
  contributes a falloff.
- The tool can also use a **box selection of mesh vertices** as a hard pin set
  (the user's "box selects the inside points, raises them"). A direct selection
  edit sets those vertices' weights and can feed the solver as pins.

### 6.2 Interpolation

Two stages, in order of fidelity:

1. **Nearest/radius falloff (phase 4a).** Each vertex gets the weighted average
   of the constraints that reach it (inverse-distance or a smooth radial
   kernel), clamped. Additive constraints are summed then clamped. This is
   predictable and fast, and it is enough for "pin the outside to 0, paint a
   line inside to 1".
2. **Mesh-graph solve (phase 4b).** Treat the constraint values as boundary
   conditions and solve a Laplacian (harmonic) system over the mesh's edge
   graph, or an RBF over the constraints. This gives a smooth field that
   respects the mesh topology instead of screen-space distance, and it is the
   "interpolation other than linear" the user asks for. The solver works on
   scratch and commits one `SkinWeights` command, like `BoneDynamics`; it never
   mutates the document during iteration.
- The exact solver is an open decision: harmonic diffusion on the mesh graph is
  the default recommendation because it follows edges and needs no global
  matrix; a thin-plate spline is a later alternative if the field needs to
  ignore topology.
- Additive blending applies to the constraints before the solve; after the
  solve the values are clamped and written as overrides. The data model remains
  one absolute value per (bone, vertex).

### 6.3 UI

- A "Weight field" tool (possibly the same Paint weights mode with a
  constraint sub-mode): place points, draw a polyline/bezier, set the value and
  radius, toggle pin/additive, box-select vertices, "Apply" or a live preview.
- Undo one apply; a constraint edit is not an undo step until committed.
- The heatmap previews the active bone; a "show authored vs final" distinction
  is useful once normalization is in play.

## 7. Mesh edits

This is the part that decides whether the feature is usable long term. The
current vertex identity is an index into a regenerated array; the notes already
ask for new vertices to keep interpolated attributes until commit (line 137).

- **Position edits** (moving vertices/edges) keep indices: the weights follow.
- **Add/split/delete** changes indices. Proposal for phase 5:
  - new vertex from a split: interpolate its weights from the source edge's two
    vertices (the same interpolation the mesh editor uses for other attributes),
    and keep the transient until the edit commits (the user's note);
  - delete: drop that vertex's entries; a split/delete remaps later indices;
  - a structural edit that cannot be remapped safely: keep the old map dormant
    with a mesh-signature mismatch and warn, rather than silently assigning
    weights to different vertices.
- **Grid cell size** (`ImageKey::resetGridMesh`) regenerates the grid. Proposal:
  lock the cell size while painted weights exist, or remap by normalized UV
  from the old grid to the new one. Remap is the better UX and is a phase 5
  item; until then, changing the cell size invalidates the map with a warning.
- **Save/load after a mesh edit** keeps the entries and their signature; the
  UI warns once, and out-of-range entries are dormant. Do not fail the load:
  the mesh is authored data and may be edited independently.
- A future stable vertex id would make this exact; it is not needed for the
  first phases and would touch every mesh writer. Record it as the long-term
  fix, not a prerequisite.

## 8. Multi-selection

The object tree has multi-selection; the Driver does not. The proposed split:

- The Bone editor keeps a **primary target** (the bone key owner) for its
  skeleton, and a **paint target set** for the layer meshes it edits.
- Entering the Bone editor on the owner establishes the rig. Multi-selecting
  descendants adds/removes paint targets; the brush paints every target in the
  set, and one gesture is one command across all of them.
- The Heatmap can show all targets or only the selected ones; the plan's
  default is all influenced layers (as today), with a visual distinction for
  the selected ones if it is needed.
- The Driver's single `setTarget` remains the authority for the tool's primary
  node. A separate `setPaintTargets`/`setTargets` on the Bone editor (fed from
  the tree's selection) is the smallest change. If the tree selection changes
  the primary target to a non-owner layer, the Bone editor must keep the owner
  it was entered with, or refuse and explain; that is a UI rule to design with
  the object-tree selection model.
- FFD's multiselection note is the same plumbing on a different editor. The
  plan includes a shared "target set" abstraction only if it falls out cleanly;
  do not force the Bone editor and FFD into one class until both uses are
  known.

The alternative - multiple bone owners/rigs painted at once - is a larger
Driver change and is out of scope for the first version. A rig per character is
the common case; a layer multi-selection within one rig is the note's request.

## 9. Reusable rigs and perspective

- **Reusable rigging** (line 145) needs the stable bone ids and node-independent
  weight records from §3, plus a mapping between the source and destination
  meshes (UV/vertex correspondence) and a rig export/import format. This plan
  builds the identity and storage prerequisites; the export is a separate
  plan.
- **Pseudo-3D/perspective** (lines 82, 94, 129) needs per-vertex depth and a
  transform that is not a weighted 2D dual quaternion; painting a 2D weight
  cannot make a linear skin transform into a 3D one. It is a separate research
  workstream (see the notes) and must not be smuggled into the weight map.

## 10. Phases

Each phase ends with the focused gates green and a measured number in this
document; the full suite is for the end of a coherent change. The phase order
keeps authored data out of the file until the format and the build are proven.

### Phase 0 - decisions and fixtures

- Confirm §3.2 (owner node vs BoneKey) and §2 (absolute override vs delta).
- Build fixtures: a two-bone chain with a small quad mesh (a painted 0/1 field
  is visually unambiguous), a grid mesh, and a 33-bone/dense-map fixture for the
  cap. These can be constructed in tests, like `tests/bone_cap.cpp`'s chain.
- Record a 0.21 project fixture for the compatibility gate.

### Phase 1 - ids, storage, format, build (no UI)

- `Bone2` id + `reserveBoneId`, serialise/deserialise, copy/clone semantics.
- `core::SkinWeights` on `ObjectNode`; sparse per-node/bone entries; life-linked
  node records; value semantics.
- `BoneInfluenceMap::writeAsync` override merge; no-weights fast path; build
  invariants.
- `ObjectTree` 0.22 weight block; `orderIDData`; `FormatVersion.h` 0.22; prune
  policy; `SkinWeightsModified` event and cache reset for all keys of the owner.
- Gates: `bone_ids`, `skin_weights`, `weight_roundtrip`, extended
  `bone_influence_storage` (override merge) and `roundtrip_main`.
- Acceptance: every 0.21 project loads and renders bit-identically (no weights
  present -> current path); a 0.22 synthetic map round-trips; a zero override
  removes a bone; normalisation and top-eight are pinned; a 0.21 file with no
  block still loads; corrupt blocks fail with named errors.

### Phase 2 - the weight brush

- New `PaintWeights` mode; target set; per-vertex stamp; add/subtract/replace;
  one command; deferred rebuild; heatmap integration; Clear weights.
- A GUI probe: enter the mode, select a bone, paint a stroke, see the heatmap
  change, undo, save/reopen.
- Acceptance: painted weights survive save/load; undo restores exactly; the
  envelope-only path is unchanged; a stroke's rebuild is coalesced (measured);
  the heatmap shows the final field.

### Phase 3 - multi-selection

- The paint target set from the tree selection; one command across layers; the
  primary owner rule; messages for out-of-rig selections.
- Acceptance: two selected layers both receive the stroke, one undo; unselected
  influenced layers are unchanged; the heatmap covers the set; the FFD target
  set is at least designed, not necessarily implemented.

### Phase 4 - constraints and interpolation

- Constraint points/curves, arc-length resampling, radius, pin, additive;
  vertex box selection; nearest/falloff first, the mesh-graph solve second;
  preview/commit; one command.
- Acceptance: a two-pin fixture (0 outside, 1 on a line) produces the expected
  field on the mesh (a numeric check plus a heatmap/pixel check); additive
  overlaps clamp; pins are exact; undo one apply.

### Phase 5 - mesh edits

- New/split/delete vertex interpolation until commit; index remap; grid cell
  size lock or UV remap; mesh-signature warning and dormant entries; a "clear
  invalid weights" action.
- Acceptance: a split keeps the new vertex's weight at the interpolation; a
  grid change either preserves the field by UV or warns and preserves the old
  dormant data; no crash; save/load keeps the state.

### Phase 6 - reusable rig (separate plan)

- Bone id mapping, mesh correspondence, export/import. Only the prerequisites
  are in this plan; do not schedule the export until Phases 1-2 have shipped.

### Phase 7 - pseudo-3D/perspective (separate research)

- Not a weight-paint feature; record it as a separate workstream.

## 11. Verification

### Unit and format gates

| gate | what it pins |
|---|---|
| `bone_ids` | fresh ids, reserve on load, copy/clone preservation, 0.21 assignment, serialise |
| `skin_weights` | sparse entries, zero override, envelope fallback, top-eight and normalisation, node life-link, orphan policy, mesh-signature handling |
| `bone_influence_storage` (extended) | the override merge in every slot layout, no-weights fast path |
| `weight_roundtrip` | 0.21 no-block load, 0.22 save/load, corrupt counts/vertices/NaN, unresolved node |
| `weight_paint_commands` | one gesture per stroke, undo/redo replay, multi-layer command, no-op |
| `weight_constraints` | resampling, falloff, additive clamping, pins, mesh-graph solve exactness |
| `weight_mesh_edits` | split interpolation, delete/drop, remap policy |

### Pixel and rendering

- `render_probe --require-gpu` on a fixture with a two-bone chain and a painted
  field: the deformed frame differs from the envelope-only control in the
  expected region, and undo/save/reload reproduce it.
- A paint/undo round-trip is pixel-identical to the pre-stroke frame.
- Envelope-only projects: `--dump-influence` hashes and 10 sampled frames remain
  bit-identical to the current build (the authored block is empty; the build
  path is unchanged).
- The heatmap's own gate remains `overlay_engine_primitives` + the manual GUI
  check from `influence-display.md`.

### Performance and memory

- `--bench-influence` on `2.anie` with no weights must match the current
  Release 8.6-10.8 ms (no new work).
- With weights, the merge cost is measured separately from the containment; a
  painted project's map bytes and entry count are reported.
- A stroke's rebuild latency (deferred to the frame or release) is measured on
  the largest dataset project; the heatmap's 120k-sample cap bounds the view.
- Save size grows by the sparse entries plus the constant per-node block.

### Probes and compatibility

- GUI probe: paint a stroke, heatmap updates, undo, save/reopen, multi-select.
- All 36 loadable dataset projects open, render envelope-only, and save/reload
  with no weight block (or an empty one); their influence hashes are unchanged.
- A 0.20 file still discards its legacy cache; a 0.21 file loads; a 0.22 file
  is rejected by the old binary, as every newer minor is.

## 12. Risks and open questions

1. **Storage location is the main decision.** Owner node (proposed) shares
   weights across keys and survives key edits; BoneKey-local is simpler to
   serialise but per-key; mesh-local follows geometry but has no skeleton.
   Confirm before Phase 1.
2. **Bone identity.** A global int id with `reserveBoneId` matches `TimeKey`.
   A project-local id is enough for this app; reusable rigs need an explicit
   mapping. The duplicate-bone rule and shadow-id rule must be decided with the
   command that duplicates bones (if one is added).
3. **Override vs delta.** The plan stores absolute overrides. If the user wants
   the envelope to remain live under a *correction*, that is a different model
   (a per-vertex delta), and it changes the build and the clear action.
4. **Normalisation visibility.** The heatmap shows final weights, but the
   brush stores pre-normalisation values. The UI must say which number it is
   editing; the plan recommends showing both the authored value and the final
   field, with the final as the heatmap.
5. **The top-eight cap.** A vertex with more contributions than eight loses the
   smallest. The user needs a diagnostic (the heatmap of the selected bone may
   not show a dropped influence). Decide whether the paint tool shows a "this
   bone is not in the final eight" warning.
6. **Mesh topology changes.** Index-based storage is exact until a structural
   edit. The plan's warning/dormant policy prevents silent corruption but
   leaves work to redo. Stable vertex ids or a UV remap are the long-term fixes;
   neither is free.
7. **Multi-selection architecture.** The tree has multi-selection, the Driver
   does not. The proposed paint target set is the smallest change; a multi-rig
   editor is a bigger one. The primary-owner rule when the selection moves is
   a UI decision.
8. **Constraint solver.** Screen-space falloff is simple and predictable;
   a mesh-graph harmonic solve is the "not just linear" answer but needs a
   solver, boundary conditions and performance work. The plan stages them.
9. **Format and release.** No release has shipped; 0.22 is a dev-only bump, but
   the 0.21 reader must still open old projects. The weight block is authored,
   so it is read, not skipped; a corrupt block fails the load. The orphan policy
   is a data-retention decision.
10. **Mesh attributes and weights.** The notes' "new vertices keep attributes
    interpolated until commit" is a mesh-editor change that this plan depends on
    for Phase 5. It should be designed with the mesh editor's other attributes,
    not as a weight-only special case.
11. **Translation.** New mode, action and hint strings need the usual catalog
    pass; the repo's new-string rule (English until the next lupdate) applies.
12. **Performance of the heatmap and build during a stroke.** The display is
    already capped; the build is not incremental. Phase 2 measures the stroke
    latency before adding the curve solver, which is heavier.

## 13. Handoff checklist

- [ ] Confirm storage location, override semantics and bone-id scope (§2, §3)
- [ ] Phase 0 fixtures: two-bone quad, grid, dense-map, 0.21 compatibility
- [ ] Phase 1: bone ids, `SkinWeights`, 0.22 block, build merge, event
- [ ] `bone_ids` and `skin_weights` gates
- [ ] 0.21 compatibility: all loadable projects render bit-identically
- [ ] Phase 2: `PaintWeights` mode, command, heatmap, Clear weights
- [ ] GUI probe for paint/undo/save/reopen
- [ ] Phase 3: paint target set and multi-layer command
- [ ] Phase 4: constraints, resampling, falloff, mesh-graph solve
- [ ] Phase 5: mesh edit policy and grid remap
- [ ] Performance numbers recorded: build, stroke, save size, entry count
- [ ] Full Debug suite green (the documented pre-existing failure excepted)
- [ ] `AGENTS.md` updated with the landed rules
- [ ] Revisit reusable rig and pseudo-3D as separate plans
