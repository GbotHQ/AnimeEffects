# Layer masks — the UX

A **mask** is an alpha field in a node's content space: the matte that says how much of the node is
visible. It is a `TimeKeyType` channel, not a tree node and not a filter-stack entry, so it inherits
everything this application already has: a timeline lane, keys with easing, proportional keys,
the key menu, the marquee, the graph's channel list (no curve — a field has no scalar), undo through
value-replayable commands, and the brush.

The picture: [`mask-surfaces.svg`](mask-surfaces.svg).

## The five surfaces

| surface | what it carries |
|---|---|
| **Inspector** — a `Mask` card beside Filters | thumbnail of the live matte, eye (is the mask applied), the three-state key dot, `Add mask` / `Invert` / `Clear`, and — between keys — a `Key at frame N` action |
| **Object tree** — a chip on the layer's own row | a 14×14 preview of the matte, dimmed when it is fully opaque, plus a dot when it is keyed. Pressing it selects the mask channel |
| **Timeline** — a `Mask` lane | keys, easing, proportional rings, the shared gestures. Label `Mask` |
| **Canvas** — a rubylith overlay | the masked-out region in red at 45 %, shown while the Mask tool targets this mask, or while the card's eye is hovered |
| **Toolbox** — the Mask tool's `Target` list | `head · Mask` alongside the existing `· amount` / `· delay` filter entries. One tool, one brush, one Value |

## States

| state | card | tree chip | canvas | lane |
|---|---|---|---|---|
| **no mask** | `Add mask` | absent | nothing | absent |
| **static** | thumbnail, hollow dot, `Invert` / `Clear` | preview, no dot | rubylith while painting | absent |
| **on a key** | filled dot, thumbnail is the key's field | preview + dot | rubylith | key under the playhead |
| **between keys** | thumbnail is the *blend*, amber `read only`, primary `Key at frame N` | preview + dot | rubylith of the blend | no key at the playhead |
| **mask off** (eye) | eye off, card dimmed | chip dimmed | no effect in the render | lane unchanged |

## Gestures

- **Paint** — the existing Mask tool: stroke → deposit toward `Value` (0 = hidden, 100 = visible),
  `[` / `]` size, Shift+`[`/`]` hardness, Alt+right-drag size/hardness, Alt+click samples the matte.
- **Add mask** — one press; the field starts at reveal-all, so the picture does not change.
- **Invert** — one undoable command; `Clear` removes the mask entirely (undoable), not a blank field.
- **The key dot** — hollow → a key at the playhead; filled → no key here. Same as every other channel.
- **Between keys** — the brush does **not** write a silent key. The card shows `Key at frame N` as its
  primary action; the stroke paints the moment that key exists.
- **Eye** — disables the mask's effect without touching its field or its keys.

## Settled by the design

1. **One mask per node.** A stack (multiply / add / subtract, Krita-style) would make lanes per *entry*
   rather than per type, which is the lane model of the whole timeline. Revisit with real projects.
2. **Neutral is reveal-all**, unlike a filter mask's per-channel neutral: adding one changes nothing.
3. **`mask × present alpha`**, after the node's own filters — a blurred layer cannot bleed past its matte.
4. **Raster only.** No vector paths.
5. **PSD masks import** as one default key on the node they belong to, document-space rect. This is
   also what retires the "clipping as a mask stand-in" workaround for import fidelity.
6. **Folders too** — a folder mask rides the composite path that folder opacity already needs.

## The one open question

**Painting between keys.** Drawn here as *refuse + `Key at frame N`* (consistent with filters and FFD,
and no strokes can vanish into an unnamed key). The alternative is an auto-key on the first stroke,
which is friendlier for masking but inconsistent with every other tool in the application.

## Order of work, and where it stands

DONE and gated (`5f06df23`, `77b34405`):

1. `TimeKeyType_Mask` + `MaskKey` — the field, one shared easing, the content rect, the texel grid and
   the PNG payload, at format 0.20. `timekey_roundtrip` saves and reloads a gradient matte (0 of 3072
   texels differ).
2. Expans + blender — one integer `lerp` per texel on equal grids, the nearer key otherwise, and the
   field's texture shared through a copy so a static mask uploads once.
3. The layer draw — `LAYER_MASK` as a fourth variant of the shipped table, sampled through the same
   `uScreenToLayer` the HSV amount mask uses, multiplied into the present alpha after every filter.
   `layer_mask_pixels` measures ten things on radeonsi (a half field masks its half, a soft edge is
   monotonic partial alpha, a corner field hides 1024 px and leaves the rest visible, and halving the
   content mapping doubles the field's screen extent).
4. The four edits every surface calls — `addMask`, `removeMask`, `invertMask`, `toggleMaskKey`, one
   undo step each, with a reveal-all neutral and a keyed hold that copies the frame it was keyed on.
   `layer_mask_commands` pins them.

DONE as well (`0470aac0`, `c5f419f2`, `62866b9a`):

5. The inspector card, its four derived states, its thumbnail (served through
   `image://maskthumb/mask?<revision>`, showing WHAT IS HIDDEN in the canvas overlay's own ink) and
   the Mask tool's `Target` entry. Measured live on a leaf layer: canvas unchanged by 0 px when the
   mask is added, 26223 px when a stroke is painted, back to the brush ring's 1521 after ONE undo.
6. A folder's matte, on the composite path, through `FilterFrame::maskTexture` - measured 14623 px
   for one stroke over a folder subtree, and 0 px on add (a neutral matte costs nothing).

REMAINING:

7. The tree's chip (a row must show that it carries a matte without being selected) and the canvas
   RUBYLITH while the Mask tool targets the matte. Painting already shows its result live - the
   hidden area disappears - so the rubylith is pre-commitment feedback, not feedback.
8. PSD import writes the mask instead of standing clipping in for one.
