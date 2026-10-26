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

## Order of work

1. `TimeKeyType_Mask` + `MaskKey` (field, easings, serialization) — the type the rest hangs on.
2. Expans + blender: `lerp` of two fields on equal grids, nearer key otherwise (no scaling in the
   per-frame path).
3. Present multiply for a layer and for a folder, with a pixel gate (masked half unmoved, unmasked half
   at full strength).
4. The inspector card + tree chip + lane (the lane is free; the card and chip are QML/inspector work).
5. The Mask tool target + rubylith.
6. PSD import writes the mask.
