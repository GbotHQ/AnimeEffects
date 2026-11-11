# Authored bone weights: painting, constraints, and the 0.22 format

Status: **Proposal, not started.** Revised 2026-11-12 around the per-bone
Envelope/Baked state (the rasterize analogy). Every decision below is a proposal
unless it is marked *measured* or *existing*, and every "recommended" item is
open to being overturned before code lands. The envelope model, the influence
cache and the weight heatmap are the starting point; the product source is
`notes.md` (lines 82, 92, 94, 129, 141, 143, 145) and the open questions in
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
- **Bone keys are per keyed frame, not per node.** The model allows one per
  frame. Measured 2026-11-12 with `render_probe --bone-stats` over the 35
  loadable projects in `~/Downloads/ANIE` (the `gif COPY2` file does not load):
  32 have one BoneKey per node; 3 have a node with **two** - `2.anie` node 164,
  `cmws81` nodes 109/112/117/120, `TaigaBoykisser EXTENDED cropped.anie` nodes
  13/31 - and none has more than two. "One rig per node" is the common case, not
  a document guarantee; §3.2 keeps the weights on the owner node and §3.3
  requires the keys to share the baked bone's geometry.
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

The first version of this document treated painting as sparse per-vertex
overrides over a live envelope. The model below is the per-bone **bake** (the
Photoshop rasterize analogy): a bone is Envelope or Baked. That is a cleaner
state for the UI, a simpler build branch, and it makes "what is envelope and
what is baked" a property of the bone rather than of each vertex.

## 2. Product model: a per-bone envelope or baked state

The envelope is the live, procedural definition of a bone's weights. It is what
every existing project has, and it stays the default. The authored state is
chosen **per bone**, not per vertex:

| state | source of the bone's weights | analogue |
|---|---|---|
| **Envelope** | `BoneShape::influence`, evaluated every rebuild | a text layer, still editable as text |
| **Baked** | a per-vertex weight map authored for the influenced meshes | a rasterized text layer, now pixels |

**Baking** a bone evaluates its envelope once, for every influenced vertex of
every influenced mesh at the current rig, and stores the result as authored
per-vertex data. From then on the envelope is not consulted for that bone;
manual paint edits change the baked map. It is the weight-painting equivalent
of Rasterize Layer: the source of truth changes from a live definition to an
authored one.

- The state is per bone. A rig can have some bones baked (a leg whose envelope is
  close but not precise) and the rest live (the ears, still being shaped).
  Normalisation and the top-eight selection stay the final stage across both.
- A baked bone's map is the **complete** field for that bone: a vertex with no
  entry is **0**, not "use the envelope". That is what makes the state
  unambiguous. The bake writes every nonzero envelope value; painting can remove
  entries.
- An empty baked map is still a state (the bone is baked and contributes
  nothing); the presence of the baked record is what says so. Do not infer the
  state from the entry count.
- **Bake is explicit** (recommended; §12 keeps it as an open UX decision). Like
  rasterizing a layer, it is a document change: a named, undoable command, not a
  side effect of a stray stroke. Entering Paint weights on an envelope bone
  shows the state and a "Bake bone weights" action; the brush is disabled until
  the bone is baked. A "Bake and paint" variant performs the same command and
  then starts the stroke; one click either way, the difference is whether the
  conversion is named.
- **Clear visual distinction.** The panel shows the selected bone's state
  ("Envelope (live)" or "Baked (n vertices)") with the Bake/Revert action. The
  viewport marks baked bones differently from envelope bones; the exact ink is a
  UI decision (the user's bone-visual note is still UNDECIDED in `notes.md`, so
  do not lock a shape here). The heatmap carries a state label so the field
  being drawn is known to be live or authored.
- **Revert to envelope** removes the baked map (confirm if it has manual edits)
  and returns the bone to the live field; one undo restores the baked map.
- **Rebake** replaces the baked map with the current envelope (confirm: manual
  edits are lost). This is the relation to the envelope brush: editing an
  envelope bone's ranges changes the render; editing a baked bone's ranges
  changes the shape but not the weights until a rebake.
- **Clear weights** on a baked bone empties the map but leaves the bone baked
  (it contributes nothing). It is distinct from Revert, which removes the baked
  state entirely.
- A new influenced layer under an already-baked bone has no baked data. Proposal:
  it uses the envelope for that layer until the user bakes it, and the bone's
  panel state says "Mixed (this layer is live)" rather than silently filling in.
  Whether the bone's state should be global (auto-bake new layers) or per
  (bone, layer) is an open question (§12).
- The brush and constraints only edit **baked** bones. Painting a live bone is a
  no-op with the bake affordance; it does not implicitly bake unless the chosen
  UX is the "Bake and paint" variant.

Two authored tools, one baked map:

1. **Paint weights** (new mode): a per-bone, per-vertex brush with
   add/subtract/replace, the shared brush parameters, the heatmap, one gesture =
   one command. It edits only baked bones.
2. **Weight constraints** (later phase): place points/curves with values and
   radii, box-select vertices, pin some, interpolate the rest, then commit. It
   builds the same baked map; the bone must be baked first.

The envelope tools stay: "Paint influence"/"Erase influence" change the broad
shape. For an envelope bone they remain live; for a baked bone they change the
shape only, and the panel offers Rebake. This is the same distinction as editing
the text of a rasterized layer: it does not change the pixels until you
rasterize again.

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

- The owner node is the rig anchor. A node may have several BoneKeys, one per
  keyed frame; the weights are a property of that rig on that node, not of a
  frame. Measured 2026-11-12 with `render_probe --bone-stats` over the 35
  loadable projects in `~/Downloads/ANIE` (the `gif COPY2` file does not load):
  32 have one BoneKey per node; 3 have a node with **two** - `2.anie` node 164,
  `cmws81` nodes 109/112/117/120, `TaigaBoykisser EXTENDED cropped.anie` nodes
  13/31 - and none has more than two. "One rig per node" is the common case, not
  a document guarantee.
- A `BoneKey` instance is a time key. Storing weights there would duplicate them
  per key and make a new key start blank, which is not how skin weights behave.
  It would also make the three dataset projects with two keys require two
  independent paints for one rig.
- A mesh key knows vertices but not bones, and grid meshes are regenerated; the
  weights would be re-created or lost with the geometry.
- The object tree serialises each node in its own block; the owner's block can
  carry the records and resolve descendant node references through the existing
  ID solver (the pre-0.21 cache did exactly this: `aOut.writeID(node)` and
  `aIn.orderIDData`). No new global block is needed.
- The owner-node choice is robust to one key or several. If the product later
  decides one rig per node, the same data fits; that would be a UI/document
  policy, not a storage reason. The multi-key case is handled in §3.3 by
  requiring the keys to share the baked bone's authored geometry.

`core::SkinWeights` (new, on `ObjectNode`):

```
struct Entry { quint32 vertex; float weight; };          // sorted by vertex
struct BakedBone {
    int boneId;                     // the authoring identity, §3.1
    QVector<Entry> entries;         // nonzero field; empty = baked to all-zero
};
struct NodeWeights {
    util::LifeLink::Pointee<ObjectNode> node;            // or a pruned raw pointer
    int vertexCount;
    quint64 meshSignature;                               // §3.4
    QVector<BakedBone> bakedBones;                       // sorted by bone id
};
struct SkinWeights { QVector<NodeWeights> nodes; };      // sorted by node pointer/id
```

- **The presence of a `BakedBone` is the state**: absent means the bone is in
  Envelope mode for this node; present means it is baked. An empty `entries`
  list is a baked bone that contributes zero everywhere, not an envelope bone.
- A baked bone's missing vertex is **0**, not the envelope. The map is the whole
  field for that bone.
- The weights are per node because the vertex arrays are; a bone can influence
  several nodes, and each has its own record. The bone's panel state aggregates:
  baked when every influenced node is baked, "Mixed" when some are.
- The per-bone entries are sorted by vertex. The build walks them as a cursor
  beside its vertex loop, so the baked merge is O(vertices + entries), not a
  hash lookup per bone/vertex.
- The object is a value type for undo (copy one node's `NodeWeights` or the whole
  `SkinWeights`; a bake is potentially whole-rig, a brush stroke local).
- The node records use a life-linked pointer so a deleted descendant goes null
  instead of dangling. A record whose node is null is ignored and can be pruned
  on save.

### 3.3 Build integration

`BoneInfluenceMap::writeAsync(Project&, topBones, groupMtx, mesh, const
SkinWeights::NodeWeights* weights)`:

```
for each bone index i with bone id b:
    const BakedBone* baked = weights ? weights->bakedFor(b) : nullptr
    if (baked):
        cursor = baked->entries (sorted by vertex)
        for each vertex v:
            float w = (cursor has v) ? cursor.weight : 0.0f
            advance cursor
            if (w >= FLT_EPSILON) tryPushBoneWeight(i, w)
    else:
        for each vertex v:
            float w = envelopeInfluence(bone[i], v)
            if (w >= FLT_EPSILON) tryPushBoneWeight(i, w)
```

- A baked bone's missing entry is a real zero; the envelope is never consulted
  for it. This is the difference from the earlier override proposal, and it is
  what makes "baked vs envelope" a state the user can see.
- No baked records (null or empty) takes the current code path exactly: no extra
  allocations, no change to any existing project.
- The output arrays, top-eight selection, normalisation and slot packing are
  unchanged. This is the invariant the format and the pixel gates protect.
- The build remains per key and per node; the owner's `SkinWeights` is read-only
  during the build. The cache is still derived: it is never persisted.

**Baking is a command.** The bake evaluates `BoneShape::influence` for the
selected bone at the current key and mesh, for every influenced vertex, and
stores the nonzero values in a `BakedBone` on every target node. It is one
`cmnd` command ("Bake bone weights"); undo removes the records and returns the
bone to Envelope mode. A bake whose envelope field is empty still writes the
record (baked to zero), so the state survives.

**Multi-key rule.** A baked bone's weights are shared by every key of the owner.
The bake evaluates the envelope from the current key. Before it writes, it
checks every BoneKey of the owner for a bone with the same id and compares the
authored geometry that the envelope depends on (local position/angle, range,
shape/bend). If they all match, the bake proceeds: the keys share the rig and
the baked field describes all of them. If any differs, the bake is refused with
the key's frame named; the user can make the rig shared (or remove the
divergent key) and bake again. Silently applying one skeleton's field to
another key's different skeleton would be the worst outcome. Pose rotations and
frame-specific poses are not part of this comparison; the envelope is built
from the rest bones and the node's mesh, not from the pose.

**Dirtying.** The weights are on the owner node, not a timeline key. A new
project event (or a small dedicated signal) `SkinWeightsModified` must reset the
caches of **every** BoneKey on that owner (all frames), not just the current key.
`BoneKeyUpdater` is the natural listener; it already owns "reset caches of this
node". A bake, a stroke or a revert pushes one command; the command's notifier
emits the event. Do not reuse a pose/move event: those rebuild only the affected
key and are also used by unrelated edits.

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
    int32 bakedBoneCount
    per baked bone:
        int32 boneId
        int32 entryCount         // 0 is a baked bone that contributes zero
        per entry: int32 vertex, float32 weight (LE, as every other float)
```

- **The record is the state.** A bone listed here is Baked for this node; a bone
  with no record is Envelope. There is no mode byte: the presence of the baked
  bone record is the mode, and an empty entry list is the all-zero baked field,
  not an envelope bone. This is the one invariant the format must never lose.
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
  bones can be deleted while their baked records remain dormant for undo/save.
  The build ignores it; a save prunes it only when the user confirms a cleanup
  (see §4.3).
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
- A deleted bone leaves its baked record dormant (the build ignores an absent
  id). This keeps a bone-delete undo from losing the paint. The cost is the
  dormant map until the user saves; a "Remove unused weights" action can prune
  it, and the save can report the count rather than dropping data silently.
  This is a deliberate no-surprise rule, not an optimisation.
- A node moved out of the owner's subtree is not influenced any more; its
  record stays with the old owner (dormant) and can be pruned by the same
  action. A node moved in starts with the envelope only.
- `resetCaches`/`updateCaches` already walk the owner's descendants; the
  weights lookup uses the same nodes. No second tree walk is needed.

### 4.4 Compatibility tests

- A 0.21 fixture loads with no weight block; the influence hashes and the
  rendered frames are identical to the pre-change build.
- A 0.22 save/reload reproduces the in-memory baked maps exactly (entry order
  normalised by sorting at save). A baked bone with zero entries round-trips as
  **baked**, not as envelope; this is the state-preservation test.
- A corrupt block (negative/overrunning count, truncated entries, NaN weight,
  unresolved node) fails with a named error and no partial load. A vertex index
  beyond the stored count is not corruption; it loads dormant and the build
  ignores it.
- A 0.22 file with baked bones in one node and envelope bones in another
  round-trips both.
- The legacy 0.20 cache discard path is unaffected.

## 5. From envelope to baked: bake, paint, revert

A new Bone editor mode, `PaintWeights`. The mode never operates on a live
envelope directly: the selected bone must be **baked** first, because baking is
the document change and the brush is the edit after it.

### 5.1 The state and the action

The panel names the selected bone's state and offers its action:

| state | panel | allowed actions |
|---|---|---|
| Envelope | "Envelope (live)" | **Bake bone weights**; the brush is disabled |
| Baked | "Baked (n vertices)" | paint; **Clear weights**; **Rebake**; **Revert to envelope** |
| Mixed (some influenced layers baked, some live) | "Mixed - this layer is live" | **Bake this layer**; painting is disabled for the live layer |

**Bake** evaluates the selected bone's envelope at the current key and frame,
for every influenced vertex of every target node, and stores the nonzero values
as the `BakedBone` record on each node. It is one `cmnd` command ("Bake bone
weights"):

- The target set is §5.2: the selected descendants of the owner, or every
  influenced layer by default.
- A node with no mesh at the current frame is skipped and named; its state stays
  Envelope (Mixed for the bone).
- The multi-key shared-rig check (§3.3) runs first. A divergent same-id bone on
  another key refuses the bake with the key's frame named; it does not write a
  field that describes only one skeleton.
- An envelope that is zero for the whole bone still writes the baked record
  with no entries: the bone is Baked and contributes nothing. The record's
  presence is the state, not its size.
- **The bake is exact, not a resample.** The values are the same floats the
  build computes; immediately after the bake the built map's hashes and the
  rendered frame must be bit-identical to the envelope-only control. This is
  the acceptance gate that makes the state transition safe.
- Undo removes the record(s) and returns the bone to Envelope; redo restores
  them.

**Revert to envelope** removes the baked records for the selected bone (all
target nodes) and returns it to the live field. If the baked map has entries,
confirm first: manual paint edits are lost (undo restores them). It is one
command.

**Clear weights** empties the entries of the baked map but leaves the bone
Baked; the bone contributes nothing. It is the "transparent pixels" state, not
the "convert back to text" state. It is one command.

**Rebake** replaces the baked map with the current envelope (confirm: manual
edits are lost). This is how the envelope and baked states relate: editing an
Envelope bone's ranges changes the render; editing a Baked bone's ranges changes
the shape only, and the panel offers Rebake. A rasterized layer behaves the same
way when its source text changes.

**Bake and paint** is an optional variant: the first left press on an Envelope
bone runs the same bake command and starts the stroke inside one gesture. It
removes one click but hides the conversion; the recommendation is explicit
bake, and §12 keeps the choice open.

### 5.2 Target selection

The Bone editor's target is the bone key owner (the rig). The **paint target
set** is the subset of its influenced layers the user wants to edit:

- the object tree's selected descendants of the owner, if any;
- otherwise every influenced cache node.

Only nodes with an influence cache for the current key and a mesh at the current
frame are bakeable/paintable. A selected node outside the owner's subtree is
ignored with one message; it is not silently painted on another rig. The bake
and the brush use the same set, so the state a bake writes is the state the
brush can edit.

The targeting interaction from `influence-display.md` is reused: right-click a
joint/connector to select the bone (left is the stroke); hovering previews its
heatmap; the first stroke with no selected bone picks the nearest envelope. In
this mode the selection chooses the bone whose state the panel shows, not a
brush target inside the envelope.

### 5.3 One stamp

For a **baked** bone, for each target node, the brush stamp is transformed into
the node's mesh space through the same matrix the cache uses (the cache's
`innerMatrix` translated by the origin offset, then the target matrix; the
heatmap already does this inverse direction). For every vertex within the stamp:

```
t      = brushFalloff(distance, radius, hardness) * amount
before = baked.weight(bone, vertex)        // missing = 0 in a baked bone
after  = clamp(before + t, 0, 1)            // add
       = clamp(before - t, 0, 1)            // subtract
       = clamp(t, 0, 1)                     // replace
if after <= FLT_EPSILON: remove the entry   // missing already means 0
else: store/update the entry
```

- The baked map's missing value is **0**, so removing a zero entry is exact and
  keeps the sparse map sparse. The bone remains Baked; the record is not
  removed. Do not confuse this with Revert, which removes the record.
- The brush never reads the envelope for a baked bone. If the user wants to
  start from the current envelope after changing it, Rebake is the action.
- The target set may contain several layers; one stamp writes each layer's own
  `(bone, vertex)` entries. Each layer's mesh has its own vertex count and
  signature.
- The brush is disabled on an Envelope bone (or the Bake-and-paint variant is
  engaged); a stamp that would paint one is a no-op with the bake affordance.

### 5.4 One stroke, one command

- `cmnd::Gesture` (or a dedicated stroke command) captures the affected
  `NodeWeights` before the first stamp and rewrites one command as the stroke
  moves. On release the command holds the before/after snapshots.
- A command spanning several layers is still one command and one undo step.
- The bake, the stroke, a Clear, a Rebake and a Revert are each their own
  command; Bake-and-paint is the one variant that combines the first two.
- The command's listener emits the `SkinWeightsModified` event (§3.3), which
  resets the owner's keys' caches once after the command (and on every
  undo/redo replay). During the stroke the cache must not rebuild per sample:
  either rebuild once per painted frame after `flushPendingWork`, or defer to
  the release. Do not add a synchronous rebuild in the mouse handler.
- The heatmap reads the **built** map. During the deferral it shows the previous
  frame's field; the plan's acceptance is "one frame of lag during the stroke,
  exact on the next frame", or an explicit live preview evaluated on the CPU
  without rebuilding the whole map. Start with the one-frame lag and measure;
  the influence build's Release cost (8.6-10.8 ms on `2.anie`) is the budget.
- Undo restores the before map and the command's listener rebuilds; redo the
  after. A no-op stroke pushes nothing.

### 5.5 UI and the visual state

- The panel shows the selected bone's state and the Bake/Revert/Rebake/Clear
  actions; the shared brush section with an Add/Subtract/Replace selector.
- The viewport distinguishes Envelope bones from Baked bones. The exact ink is
  a UI decision (the user's bone-visual note is UNDECIDED in `notes.md`); a
  state icon in the panel and a different bone stroke/fill is enough for the
  first version. The heatmap carries a state label ("Envelope" / "Baked") so the
  field being drawn is never ambiguous.
- The heatmap toggle already exists; on for this mode.
- A readout of the value under the cursor (baked authored value and the final
  normalised weight) is useful; bone names do not exist, so the readout names
  the joint by its index/path or the selected highlight.
- On an Envelope bone the brush controls are visible but disabled with the Bake
  action beside them; a disabled control that silently does nothing is worse
  than one that explains why.

## 6. Points, curves and interpolation

This is the user's line 143: edit weights at a few hand-placed points/curves,
interpolate the rest; curves resampled first; additive blending; pinning; more
than linear interpolation. Constraints write a **baked** bone's map; an Envelope
bone is baked first with the same explicit action as the brush (§5.1), so the
constraint tool never writes a live envelope.

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
- Additive blending applies to the constraints before the solve; after the solve
  the values are clamped and written into the **baked** map (missing = 0). The
  data model remains one baked value per (bone, vertex); an Envelope bone is
  baked first (§5.1), so the constraint tool never writes a live envelope.

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

- Confirm §3.2 (owner-node storage; the measured 3/35 multi-key cases), §2
  (per-bone baked state; explicit vs Bake-and-paint), §5.1 (the new-layer Mixed
  rule) and §3.3 (the shared-rig refusal). These are the decisions that change
  the data model; do not code past them.
- Build fixtures: a two-bone chain with a small quad mesh (a baked 0/1 field is
  visually unambiguous), a grid mesh, and a 33-bone/dense-map fixture for the
  cap. These can be constructed in tests, like `tests/bone_cap.cpp`'s chain.
- A two-key fixture whose keys share the rig (the bake must succeed) and one
  whose keys differ (the bake must refuse), modelled on the measured 2-key
  projects.
- Record a 0.21 project fixture for the compatibility gate.

### Phase 1 - ids, baked storage, format, build, bake command (no UI)

- `Bone2` id + `reserveBoneId`, serialise/deserialise, copy/clone semantics.
- `core::SkinWeights` on `ObjectNode`; `BakedBone` presence is the state; an
  empty baked map round-trips as Baked; life-linked node records; value
  semantics.
- `BoneInfluenceMap::writeAsync` baked branch; no-baked fast path; build
  invariants.
- `ObjectTree` 0.22 weight block; `orderIDData`; `FormatVersion.h` 0.22; prune
  policy; `SkinWeightsModified` event and cache reset for all keys of the owner.
- The headless bake command: evaluate the selected bone's envelope at the
  current key/mesh, write the nonzero entries, check the multi-key shared rig,
  one command with undo/redo.
- Gates: `bone_ids`, `skin_weights`, `weight_roundtrip`, `weight_bake`, extended
  `bone_influence_storage` and `roundtrip_main`.
- Acceptance: every 0.21 project loads and renders bit-identically (no baked
  records -> the current path); a bake is **bit-identical** to the envelope
  control at the bake frame (influence hashes and pixels); an empty baked map
  remains Baked after save/load; a 0.22 synthetic map round-trips; a divergent
  second key refuses the bake with its frame; normalisation and top-eight are
  pinned; a 0.21 file with no block still loads; corrupt blocks fail with named
  errors.

### Phase 2 - the bake action and the weight brush

- New `PaintWeights` mode with the state panel: Envelope / Baked / Mixed, the
  Bake/Revert/Rebake/Clear actions, the shared brush and Add/Subtract/Replace.
- Bake and brush commands; per-vertex stamp on a baked bone; one command per
  stroke; deferred rebuild; heatmap state label; the distinct bone ink; the
  envelope brush on a baked bone and the Rebake relation.
- A GUI probe: enter the mode, bake, see the render unchanged, paint a stroke,
  see the heatmap/deformation change, undo, revert, save/reopen; assert the
  brush is disabled/explained on an envelope bone.
- Acceptance: baked state survives save/load; the bake frame is bit-identical
  to the envelope control; undo restores the before map exactly; Revert returns
  the live envelope; Rebake replaces the map; painting an envelope bone is
  impossible without the explicit bake action (or the chosen Bake-and-paint
  alternative); the envelope-only path is unchanged; a stroke's rebuild is
  coalesced (measured); the heatmap shows the final field and its state.

### Phase 3 - multi-selection

- The paint target set from the tree selection; one command across layers; the
  primary owner rule; messages for out-of-rig selections.
- Acceptance: two selected layers both receive the stroke, one undo; unselected
  influenced layers are unchanged; the heatmap covers the set; the FFD target
  set is at least designed, not necessarily implemented.

### Phase 4 - constraints and interpolation

- Constraint points/curves, arc-length resampling, radius, pin, additive;
  vertex box selection; nearest/falloff first, the mesh-graph solve second;
  preview/commit; one command. The tool writes the baked map: if the selected
  bone is still Envelope, it offers the same explicit bake first (or a "Bake and
  apply" variant), because a constraint solve is the same kind of authoring
  transition as a brush stroke.
- Acceptance: a two-pin fixture (0 outside, 1 on a line) produces the expected
  field on the mesh (a numeric check plus a heatmap/pixel check); additive
  overlaps clamp; pins are exact; undo one apply; the bake-before-apply state
  transition is explicit and undoable.

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
| `skin_weights` | baked presence vs Envelope, an empty baked map staying Baked, per-node records, top-eight and normalisation, node life-link, orphan policy, mesh-signature handling |
| `bone_influence_storage` (extended) | the baked merge in every slot layout (missing = 0, no envelope fallback), the no-baked fast path |
| `weight_roundtrip` | 0.21 no-block load, 0.22 save/load, baked-with-no-entries, corrupt counts/NaN, unresolved node, dormant vertex |
| `weight_bake` | bake equals the envelope at the bake frame (hashes), empty baked, divergent second key refusal, undo/redo of the bake |
| `weight_paint_commands` | one gesture per stroke, separate bake/stroke/revert/rebake commands, undo/redo replay, multi-layer command, no-op |
| `weight_constraints` | resampling, falloff, additive clamping, pins, mesh-graph solve exactness, bake-before-apply |
| `weight_mesh_edits` | split interpolation, delete/drop, remap policy |

### Pixel and rendering

- **The bake is bit-identical** to the envelope-only control at the bake frame:
  the `--dump-influence` hashes and the rendered PNGs match, because the baked
  values are the same floats the envelope produced. This is the gate that makes
  the state transition safe.
- A painted stroke on a baked bone changes the field/deformation in the expected
  region; undo restores the pre-stroke frame exactly; Revert restores the
  envelope-only frame; Rebake reproduces the bake frame from the current
  envelope.
- Envelope-only projects: `--dump-influence` hashes and 10 sampled frames remain
  bit-identical to the current build (no baked records; the build path is
  unchanged).
- The heatmap's own gate remains `overlay_engine_primitives` + the manual GUI
  check from `influence-display.md`, extended with the state label.

### Performance and memory

- `--bench-influence` on `2.anie` with no baked records must match the current
  Release 8.6-10.8 ms (no new work).
- The baked merge is measured separately from the polygon containment; a baked
  project's map bytes and entry count are reported.
- The bake itself (evaluating the selected bone's envelope over the target
  meshes) is a one-off cost, measured on the largest dataset project.
- A stroke's rebuild latency (deferred to the frame or release) is measured; the
  heatmap's 120k-sample cap bounds the view.
- Save size grows by the baked entries plus the small per-node block.

### Probes and compatibility

- GUI probe: bake a bone (render unchanged), paint a stroke, undo, revert,
  rebake, save/reopen, then multi-select two layers and bake/paint both. Assert
  the brush is disabled/explained on an Envelope bone.
- All 36 loadable dataset projects open, render envelope-only, and save/reload
  with no baked records; their influence hashes are unchanged. The three
  measured projects with two keys on a node must round-trip; their bake
  behaviour is the shared-rig gate.
- A 0.20 file still discards its legacy cache; a 0.21 file loads; a 0.22 file
  is rejected by the old binary, as every newer minor is.

## 12. Risks and open questions

1. **Explicit vs implicit bake - the UX question.** The plan recommends an
   explicit, named "Bake bone weights" command: baking changes the source of
   truth and the file, so it should be one named undo step, not a side effect of
   a stroke; the envelope stays inspectable until the user commits; a stray
   click cannot convert a bone. The cost is one click. The alternative is
   "Bake and paint": the first stroke performs the same command and the stroke
   in one gesture, which is smoother but hides the conversion. A third option is
   a per-bone toggle (Envelope/Baked) in a bone list, with painting allowed only
   in Baked. The recommendation is explicit; this is the decision to make first.
2. **Storage location.** Owner node (proposed) is the rig anchor. The document
   model allows several BoneKeys per node; measured 3 of 35 projects use two on
   one node, never more. Owner-node storage makes one paint serve every key,
   which is why it beats BoneKey-local storage (per-key duplication, blank new
   keys) and mesh-local storage (no skeleton). If the product later enforces
   one rig per node, the same data fits; that is a document/UI policy, not a
   storage reason.
3. **Per-bone vs per-(bone, node) state, and new layers.** A bone can influence
   several layers; the data is per node. The UI state aggregates. A new
   influenced layer has no baked record; the proposal is a "Mixed" state with
   the new layer live until baked. Whether a bone-level Bake should
   auto-rasterize new layers, and whether the user should see a per-layer state,
   is open.
4. **Multiple keys and the shared-rig rule.** A baked field is evaluated from
   one key's envelope. The proposal refuses a bake if another key has a same-id
   bone with different authored geometry/range/shape (the three measured
   two-key projects are the test). Alternatives: per-key baked maps (reintroduces
   duplication), a "make the rig shared" action, or treating all BoneKeys as
   pose keys over one rig (a larger model change, aligned with the user's
   intuition that there is "basically one"). This needs a decision before
   Phase 1.
5. **Bone identity.** A global int id with `reserveBoneId` matches `TimeKey`.
   A project-local id is enough for this app; reusable rigs need an explicit
   mapping. The duplicate-bone rule and shadow-id rule must be decided with the
   command that duplicates bones (if one is added).
6. **Normalisation visibility.** The heatmap shows final weights, but the baked
   map stores the bone's authored weight. The UI must say which number it is
   editing; the plan recommends showing the baked value and the final field,
   with the final as the heatmap and a state label.
7. **The top-eight cap.** A vertex with more contributions than eight loses the
   smallest. The heatmap of the selected bone may not show a dropped influence;
   decide whether the paint tool warns "this bone is not in the final eight".
8. **Mesh topology changes.** Index-based storage is exact until a structural
   edit. The warning/dormant policy prevents silent corruption but leaves work
   to redo. Stable vertex ids or a UV remap are the long-term fixes; neither is
   free.
9. **Multi-selection architecture.** The tree has multi-selection, the Driver
   does not. The proposed paint target set is the smallest change; a multi-rig
   editor is a bigger one. The primary-owner rule when the selection moves is
   a UI decision.
10. **Constraint solver.** Screen-space falloff is simple and predictable;
    a mesh-graph harmonic solve is the "not just linear" answer but needs a
    solver, boundary conditions and performance work. The plan stages them.
11. **Revert and manual edits.** Reverting to Envelope discards the baked map;
    undo can restore it, but a save after the revert makes the loss permanent.
    The UI must confirm when entries exist, and the undo path must be exact.
12. **Format and release.** No release has shipped; 0.22 is a dev-only bump, but
    the 0.21 reader must still open old projects. The weight block is authored,
    so it is read, not skipped; a corrupt block fails the load. The dormant
    record policy is a data-retention decision.
13. **Mesh attributes and weights.** The notes' "new vertices keep attributes
    interpolated until commit" is a mesh-editor change that this plan depends on
    for Phase 5. It should be designed with the mesh editor's other attributes,
    not as a weight-only special case.
14. **Translation.** New mode, state, action and hint strings need the usual
    catalog pass; the repo's new-string rule (English until the next lupdate)
    applies.
15. **Performance of the heatmap and build during a stroke.** The display is
    already capped; the build is not incremental. Phase 2 measures the stroke
    latency before adding the curve solver, which is heavier.

## 13. Handoff checklist

- [ ] Confirm explicit vs implicit bake; per-bone/new-layer state; the
      shared-rig rule for the three two-key projects (§5.1, §3.3, §12)
- [ ] Phase 0 fixtures: two-bone quad, grid, dense-map, 0.21 compatibility,
      two-key shared/divergent
- [ ] Phase 1: bone ids, `SkinWeights` with BakedBone presence, 0.22 block,
      baked build branch, bake command, event
- [ ] `bone_ids`, `skin_weights`, `weight_bake` gates
- [ ] 0.21 compatibility: all loadable projects render bit-identically
- [ ] Phase 2: `PaintWeights` mode, Bake/Revert/Rebake/Clear, brush, heatmap
      state label, distinct bone ink
- [ ] GUI probe for bake/paint/undo/revert/save/reopen
- [ ] Phase 3: paint target set and multi-layer bake/paint command
- [ ] Phase 4: constraints, resampling, falloff, mesh-graph solve, bake-first
- [ ] Phase 5: mesh edit policy and grid remap
- [ ] Performance numbers recorded: build, bake, stroke, save size, entry count
- [ ] Full Debug suite green (the documented pre-existing failure excepted)
- [ ] `AGENTS.md` updated with the landed rules
- [ ] Revisit reusable rig and pseudo-3D as separate plans
