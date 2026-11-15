# PSD "Blend If" in AnimeEffects: renderer compatibility assessment

Measured on `ALL FATHER F3 color fixes.psd` (3840x1240 RGB/8, 57 layer records, 114 MB) with the
tool in this directory. Everything below marked **verified** was read out of the code or measured;
**unknown** items are called out explicitly rather than assumed.

## 1. What the feature is

Photoshop's per-layer "Blend If" is the blending-ranges section of a layer record: two slider
pairs, "This Layer" (source) and "Underlying Layer" (destination), each stored as 4 bytes
(black-lo, black-hi, white-lo, white-hi). The layer's effective contribution is scaled per pixel
by a trapezoid over those thresholds; the default `(0,0,255,255)` means no restriction. Photoshop
additionally exposes per-channel (R/G/B) slider sets.

Measured inventory of this file (`meta.py`):

| layer | blend / opacity | destination range | note |
|---|---|---|---|
| Layer 17 | Overlay 107 | 136-217 / 255-255 | acts on ~7.7 % mean weight |
| Layer 16 | Overlay 255 | 35-149 / 255-255 | ~42 % |
| Layer 53 | Overlay 163 | 0-0 / 35-94 | ~42 % |
| Layer 24 | Color 125 | 71-128 / 32-183 | ~10 % |
| Layer 46 | Color 89 | 82-114 / 102-136 | ~9 % |
| Layer 49 | Color 255 | 29-68 / 179-137 | ~51 %, 43.9 % of the canvas fully suppressed |
| title copy 7 | Normal 255 | source 255-255 / 0-0 (empty) | fill opacity 0 too |

Per-channel pairs are default on every layer of this file.

Effect size (independent numpy compositor, `komp.py`): reproducing the PSD's own stored composite

| variant | mean per-pixel diff vs the stored composite |
|---|---|
| blend-if + masks + fill opacity | **8.36** (3.45 outside the title band) |
| blend-if disabled | 42.39 |
| AnimeEffects today | 40.36 |

## 2. Why the current renderer takes it well (verified)

1. **The weight is an alpha scale in every presentation path.** All three paths draw with
   `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`
   (`LayerNode::renderLayer`, `FilterFrame::drawQuad` for `Kind_HSV`), so scaling the fragment's
   alpha scales the layer's contribution exactly like Photoshop's weight — this is precisely how
   the existing layer opacity (`uColor.a`) already works.
2. **The backdrop pixels are already in the shader.** `LayerDrawingFrag.glsl` unconditionally does
   `texelFetch(uDestTexture, ivec2(vDestCoord), 0)`, where `vDestCoord` is computed in
   `LayerDrawingVert.glsl` from the same view transform used for `gl_Position` — i.e. the fragment's
   own screen pixel. That is the "Underlying Layer" value the destination range needs. In the
   direct path unit 1 is bound to the capture unconditionally (`LayerNode::renderLayer`), even for
   `BlendMode_Normal`, so no binding change is needed for Normal + Blend If.
3. **One shader serves all presentation paths**, so the weight is implemented once:
   `LayerNode::renderLayer` → `ShaderHolder::shader(...)`, and `LayerNode::renderComposite` /
   `FolderNode::renderComposite` → `FilterFrame::drawBlendPresent` / `drawQuad` →
   `FilterFrame::presentShader(...)`, which builds from the same
   `:/shaders/LayerDrawingFrag.glsl` (`fBuildLayerDrawingVariant`).
4. **Frame setup is already right**: the capture texture is cleared once per frame
   (`MainDisplayWidget::paintGL:287`, harness `Fixture::render`), so no stale capture can leak in.
5. **Uniforms, not shader permutations.** S15 pins the variant matrix at
   `BlendMode_TERM * 2 * 2`; a uniform-driven weight keeps every existing variant valid (no extra
   compile time, no `#variation` blowup). Consequence: the uniforms belong to the program, so they
   must be set on every draw of that program (a node without ranges must disable the weight).
6. **No color-space work.** Nothing enables `GL_FRAMEBUFFER_SRGB` (grep over `src/`), so the
   framebuffer holds raw 8-bit sRGB-encoded values — the values Photoshop's sliders act on.
   Empirically confirmed: a Rec.601 8-bit reference reproduces the file's composite at 8.36 while
   Rec.709 gives 9.73 (and 57 % vs 35 % of pixels off by >2), and the shipped shader already uses
   Rec.601 for its non-separable luma (`BlendLum`).
7. **The data is already parsed and the format has a precedent.** `PSDReader` fills
   `layer->compositeBlendingRange` and `channel->blendingRange`; nothing consumes them. For
   persistence, `FolderNode`'s blend mode was appended to the object block and read under
   `aIn.version().minorVersion() >= 10` with a format bump — the same shape works here, and both
   node types serialize through `ObjectNodeUtil::writeObjectBlock/readObjectBlock`, so there is a
   single place to extend.
8. `PSDWriter` can write the ranges (round-trip), but the app has no PSD *export* path
   (no `PSDWriter` call site in `src/`), so there is no exporter work.

## 3. What has to be built

| # | change | site |
|---|---|---|
| 1 | `core::BlendIf` value type: `dst`/`src` ranges (4×uint8 each), `isActive()`, `isDefault()`; per-channel ranges left as a documented extension | new `src/core/BlendIf.h` |
| 2 | node property + `Renderer` accessors (`virtual const BlendIf& blendIf()`, `setBlendIf`), members on the two renderers | `core/Renderer.h`, `LayerNode`, `FolderNode` |
| 3 | trapezoid weight in the fragment shader (Rec.601 on straight colors, guards for inverted/empty ranges), applied as `color.a *= w`, and as `result.a = w` in the `BLEND_PREMULTIPLIED_SRC` replace branch | `assets/shaders/LayerDrawingFrag.glsl` |
| 4 | set the uniforms (`uBlendIfSrc`, `uBlendIfDst`, enable mask) on every draw of the layer shader | `LayerNode::renderLayer`, `FilterFrame::drawQuad` present branch |
| 5 | capture the backdrop when a layer has ranges even at `BlendMode_Normal` (currently only non-Normal modes capture) | `LayerNode::renderLayer`, `LayerNode::renderComposite`, `FolderNode::renderComposite` |
| 6 | pass the ranges through the present path | `FilterFrame::drawBlendPresent` / `drawQuad` signatures |
| 7 | import: map `compositeBlendingRange` (+ per-channel) onto the created nodes, layers and folders | `ctrl/ImageFileLoader.cpp` |
| 8 | persist: append the ranges to the object block, read under `minorVersion() >= 12`, bump the constant 11 → 12 | `core/ObjectNodeUtil.cpp`, `core/FormatVersion.h` |
| 9 | (follow-up) property-panel sliders | `gui/prop/prop_*` |

Cost note: item 5 adds one region copy of the framebuffer per frame for every layer that carries
ranges (the same `DestinationTexturizer::update` the non-separable blends already pay).

## 4. Unknowns and risks (not assumptions)

- **Per-channel R/G/B ordering** relative to the layer's channel list is not verifiable with this
  file (its per-channel pairs are all default). Implement the gray pair first, keep the structure
  extensible, and validate only when a Photoshop file with per-channel sliders is available.
- **Pass-through group records with ranges.** What Photoshop does for a group that has no composite
  of its own is not established. AE renders a Normal folder's children straight into the parent
  framebuffer (no composite buffer — `FolderNode::isCompositeFolder`), so a group-level weight has
  no single result to scale. Plan: implement per-node weighting on the presentation paths, and treat
  pass-through group records as an explicit follow-up.
- **Semi-transparent backdrop precision.** The capture is premultiplied RGBA8, so the shader must
  un-premultiply before comparing channel values (the non-separable branch already does). With an
  opaque canvas this is exact; with `dst.a < 255` it carries on the order of ±1-2 LSB error. AE's
  separable blends already treat the premultiplied capture as the blend operand, so this is
  consistent with existing behavior, not a new class of error.
- **"This Layer" pixel definition.** Decision: compare the fragment's own color *after* the HSV key
  (a per-layer content adjustment) and *before* opacity, since Photoshop's slider acts on the
  layer's pixels.
- **CSP premultiplied-src glow modes** with ranges: weighting that branch means writing
  `result.a = w` so the GL pass mixes the replacement with the untouched backdrop. Mathematically
  right for an opaque backdrop, untested against a reference.
- **Inverted/empty ranges** (`title copy 7`: source 255-255 / 0-0) must not divide by zero; an empty
  range naturally yields weight 0.
- **Not covered by Blend If** but part of the same import gap for this file: layer masks (6 layers),
  Photoshop layer effects (`lfx2`, 10 layers) and fill opacity (`iOpa`, 9 layers). The independent
  compositor's residual is 39.4 mean inside the title band versus 3.45 outside it — that band is
  exactly where those features live.

## 5. Verification plan

1. **New harness suite** (`tools/render_verify`, S16) with an independent CPU reference in the style of
   `cpu_ref.h`: trapezoid weight over Rec.601 luma of straight colors, applied as an alpha scale.
   Cases: destination-only, source-only, both, inverted/empty (weight 0), default (byte-identical to
   no Blend If), weight × fractional opacity, layer inside a Normal folder (pass-through), layer
   under a non-separable Color blend, blurred composite layer (present path), folder present path.
2. **No third-party golden is possible — measured, not assumed**: GIMP 3.2 ignores PSD Blend If (its
   flatten of the real file is closer to a no-blend-if composite, 21.3, than to a blend-if-aware one,
   56.8), and Krita 5.3.2.1 fails to open the file at all. The only external ground truth for Blend
   If is therefore a real PSD's own stored composite, which is what `tools/psd_composite_probe`
   diffs against.
3. **Real-file acceptance for the import work**: AE's frame 0 versus this file's stored composite
   should fall from mean 40.36 to below ~15 (the remaining error is the title band's masks, effects
   and fill opacity). A drop inside the title band would indicate something else changed.

## 6. Layer masks (landed after the Blend If work)

Layer masks were implemented next, as the assessment's follow-up list called for. What
changed: `img/ResourceData` carries the mask (A8 plane in the layer's image rect, plus
enabled/inverted flags), the importer decodes the PSD user-mask channel (its own rect and
default color are folded into the layer rect, mask flags bit 1/bit 2 map to
disabled/inverted), `ImageKey::Cache` holds a GL_R8 texture uploaded with a pinned unpack
alignment (mask widths are not 4-aligned), `LayerDrawingFrag.glsl` multiplies the fragment
alpha by the mask behind a uniform-conditional branch (unmasked layers pay no fetch, and no
new shader permutation is created), and `ResourceHolder` persists the plane in the resource
block under a new format minor (13).

Measured on the same real file (frame 0 vs the PSD's own stored composite):

| build | whole canvas | outside the title band | title band |
|---|---|---|---|
| before any work | 40.36 (51.7 % >32) | 40.85 | 37.28 |
| Blend If only | 9.46 (3.82 % >32) | 6.05 | 31.09 |
| **Blend If + masks** | **8.75 (3.44 % >32)** | **3.94** | 39.24 |
| independent compositor (same features) | 8.36 | 3.45 | 39.44 |

Outside the title band AE is now at the independent compositor's floor. The title band is at
39.2 in *both* — that region's remaining error is the Photoshop layer effects (`lfx2`) and
fill opacity (`iOpa`) that neither implementation renders, and the earlier 31.09 was two
errors cancelling (missing masks hiding layers whose missing effects should have drawn them).

**On the title-band number as a fidelity metric.** It is not monotone: adding masks moved it
from 31.09 up to 39.24 while the whole-canvas error fell from 9.46 to 8.75 and the
outside-band error from 6.05 to 3.94. The band is ~13 % of the canvas and is dominated by
layers whose Photoshop effects decide their appearance, so a build that hides their fills
correctly (masks) but cannot draw their effects scores worse there than one that draws the
fills by accident. The right acceptance criterion is therefore the whole-canvas /
outside-band error plus agreement with an independent implementation in the band: AE 39.24 vs
the compositor's 39.44 means AE is at the floor of what the reachable feature set can do
there. Closing the band itself needs `lfx2` (layer effects) and `iOpa` (fill opacity), listed
as follow-ups below.

Verification: `tools/render_verify` suite S17 (coverage per blend mode, inverted, disabled,
opacity/Blend If composition, composite/blur path, written-PSD import leg with flags and
pixel values, project save/load byte-identical) plus the existing 3412 checks - all green;
real pre-13 projects still load and round-trip (`build/tests/Debug/roundtrip` on three
`.anie` files, 0 load failures).

## 7. Authoring: the property panel and the source-range bake

The layer/folder property panel (`gui/prop/prop_ConstantPanel`, which serves both node types)
now shows what the importer brings in, with every edit pushed as one undoable command:

- **Blend If: Underlying Layer** and **Blend If: This Layer** groups, four split-slider values
  each (black low/high, white low/high; 0..255, the PSD default 0/0/255/255 = no restriction)
  plus a "Reset (no restriction)" button. The values come from
  `Renderer::blendIf()`; edits go through `assignBlendIfSide`, which writes the whole struct
  inside a `cmnd::ScopedMacro`.
- **Mask** group: "Apply" (enabled), "Invert", and **"Bake This Layer range"**.

"Bake This Layer range" is the source-range conversion: `core::LayerMaskBake` captures the
previous mask and ranges, computes the baked mask (source weight per pixel, composed
multiplicatively with any existing mask) and clears the source range. The destination range
is kept deliberately — it depends on the backdrop, so it cannot be baked. Mask edits are
picked up by the render path (`ImageKey::syncMaskTexture` rebuilds the GL_R8 copy when the
resource's mask revision moved), so the command needs no GL context of its own, which is what
makes it safe to run from a panel.

**The panel is covered by an automated widget test** (`tests/panel_blendif.cpp`, run through
`tests/panel_blendif.sh`): it drives the real `ConstantPanel` — not a replica — with a real
project and checks eighteen things: that the eight split-slider items show the selected node's
destination and source ranges (and a folder's, when the selection changes), that an edit typed
into a spin box becomes exactly **one** command named "update blend if ranges" whose undo
restores the previous range and whose redo re-applies it, that the reset button returns only the
side it belongs to Photoshop's unrestricted default and that the two undos step back through the
reset and then the edit, and that with no target the items are disabled rather than editable.

Two notes on how it is built, because the gui is an executable rather than a library: the target
compiles the panel and the widget classes it builds (the same curated-subset approach
`easing_picker_layout` uses), and it stubs the two collaborators the panel never touches —
`gui::ViaPoint`'s logger methods (whose only real body opens the resource dialog) and
`AudioPlaybackWidget::deserialize`, which `core::Project`'s constructor references. Without
those stubs the panel's dependency cone reaches `ResourceDialog`, the resource tree and
`MainWindow`. If the gui ever becomes a library, both stubs can go.

## 8. Implementation status (Blend If)

Blend If is implemented and verified. What changed:

| file | change |
|---|---|
| `src/core/BlendIf.h/.cpp` | the value type (two ranges, `isActive()`, `isDefault()`, packed round-trip) + `applyBlendIfUniforms` |
| `src/core/Renderer.h`, `LayerNode`, `FolderNode` | `blendIf()`/`setBlendIf()` on the renderer interface and the two node types |
| `assets/shaders/LayerDrawingFrag.glsl` | `BlendIfRange`/`BlendIfWeight` (Rec.601 trapezoid on straight colors, step form for equal thresholds) and the weight as an alpha scale in all three blend branches, including the premultiplied-src replace branch |
| `LayerNode::renderLayer` | captures the backdrop when ranges are active even at `BlendMode_Normal`; sets the uniforms on every draw; `aApplyBlendIf=false` for the isolated composite/blur content pass (the bug S16 caught: the slot has no backdrop, so weighting there zeroed the blurred content) |
| `LayerNode::renderComposite`, `FolderNode::renderComposite`, `FilterFrame` | the present paths carry the ranges and capture the backdrop when needed |
| `src/ctrl/ImageFileLoader.cpp` | maps the PSD composite pair onto layers and folders |
| `src/core/LayerNode.cpp`, `FolderNode.cpp`, `FormatVersion.h` | ranges appended after the object block (minor 11 -> 12), read under `minorVersion() >= 12` |
| `tools/render_verify` | S16 (16 checks) with the CPU reference `ref::blendPresent(..., &ranges)`, plus the CPU reference extension in `cpu_ref.h` |

Measured (real file, `/home/gambot/Downloads/ALL FATHER F3 color fixes.psd`, frame 0 vs the PSD's own
stored composite, `tools/psd_composite_probe`):

| build | whole canvas | title band | outside the title band |
|---|---|---|---|
| before (mean / >32 share) | 40.36 / 51.66 % | 37.28 | 40.85 |
| **with Blend If** | **9.46 / 3.82 %** | 31.09 | **6.05 (0.51 % >32)** |
| independent compositor (same features) | 8.36 | 39.44 | 3.45 |

Harness: `AE_BUILD_DIR=build tools/render_verify/build_and_run.sh` -> **3412 checks, 0 failures**
(S16 included; S14 goldens green).

Deliberate follow-ups, none of them Blend If:

- **Per-channel R/G/B ranges** are still not modelled (this file uses only the gray pair; the
  byte-order question for the per-channel set remains unverified).
- **Pass-through group records with ranges** have no composite to weight; they need a decision
  about Photoshop's own semantics there.
- **Layer masks (6), Photoshop layer effects `lfx2` (10), fill opacity `iOpa` (9)** are the whole
  remaining title-band error (31.09 mean there versus 6.05 elsewhere) and are separate features:
  masks need a per-pixel coverage channel, effects need a layer-style renderer, fill opacity only
  needs the importer to fold `iOpa` into the layer's opacity for the fill.
- **UI**: the ranges are imported and persisted but not editable; the layer property panel would
  add the four-slider pairs.
- Saving with this build writes format minor 12, which older builds refuse to open (the same
  policy as the 0.9/0.10/0.11 bumps).

## 9. Layer effects (lfx2): what the title was missing

The remaining title-band error turned out to be Photoshop layer effects, so this increment
implemented the ones that need no silhouette pass. Measured on the real file:

| build | whole canvas | title band | outside the title band |
|---|---|---|---|
| Blend If + masks | 8.75 (3.44 % >32) | 39.24 | 3.94 |
| **+ gradient overlay & fill opacity** | **5.55 (1.76 % >32)** | **15.73** | 3.94 (0.03 % >32) |
| independent compositor (no effects) | 8.36 | 39.44 | 3.45 |

What the title layers actually carry (`/tmp` dump + psd-tools cross-check): only four layers
have effects and two of them are hidden. Visible: `title copy` has Bevel (`ebbl`), Stroke
(`FrFX`, inset gradient, 4 px, Screen), Chrome (`ChFX`, Screen, blur 14, distance 11) and a
**Gradient Overlay** (`GrFl`, Normal, 100 %, linear, stops (228,161,30) → (248,213,79));
`title copy 7` has an Outer Glow. The gradient overlay is what turns the white letter fill
gold, which is exactly what the earlier renders were missing.

Implemented: `img/PSDDescriptor` (a descriptor reader — keys are length-prefixed or 4CC
terms, `lfx2` is a `DescriptorBlock2` with version + data_version = 16 before the descriptor;
verified against psd-tools on the real block: identical keys, enums and stop values),
`core/LayerEffects` (colour overlay + gradient overlay with merged colour/transparency stops,
angle/scale/reverse/offset, plus the keys of every effect present so the UI and the import log
can report the ones that are not drawn), the shader's fill-replacement pass (Normal blend,
linear gradients, stop arrays), the importer wiring, `iOpa` fill opacity folded into the layer
opacity, and persistence under format minor 14.

Still not drawn at that point, and the reason the title band sat at 15.7 rather than ~3.9: the
effects that need a dilated or blurred copy of the layer's silhouette — the 4 px inset gradient
stroke, the Screen-mode chrome (blur 14, offset 11), the bevel (which also carries a texture)
and `title copy 7`'s outer glow. `LayerEffects::isDrawnAsConfigured()` reports false whenever
one of those is enabled, so the import is never silently misrepresented.

## 10. Blurred silhouettes and the fill-opacity correction (this increment)

Three things landed after §9, all measured on the same file and canvas:

1. **The silhouette family is drawn.** `core/LayerNode.cpp`'s `fPresentSilhouettes` blurs a
   copy of the layer's *shape* through the production Gaussian (`core/FilterFrame`), tints it
   with the effect colour through a new `FilterFrame::Kind_Tint` pass, offsets it by
   `distance` at the effect's angle (Photoshop's counter-clockwise, 0 = right) and composites
   it with the effect's own blend mode — under the layer for drop shadow and outer glow, over
   it for chrome. That covers `title copy`'s chrome and `title copy 7`'s outer glow.
2. **`iOpa` is FILL opacity, not layer opacity.** Folding it into the layer opacity was wrong
   and visibly so: this file's `title copy 7` has `iOpa = 0` *and* an outer glow, i.e. the
   classic glow-only layer whose pixels are hidden but whose effect is not. The value is now a
   separate node property (`LayerNode::fillOpacity`, `uFillOpacity` in the shader), where the
   layer's pixels and the fill-replacing styles composite with each other *inside* the layer's
   coverage: a 0 % fill still shows an opaque style, a 0 % fill with no style draws nothing,
   and the silhouette is taken from a dedicated shape pass so a low fill cannot thin it.
3. **Blend If's per-channel pairs are verified unused, not assumed.** All 57 layer records of
   the file were dumped through the production parser: the 7 layers that edit the composite
   range (6 edit `dst`, `title copy 7` empties `src` to `[255,255,0,0]`) leave every one of
   their channel pairs at the default `[0,0,255,255]`, so applying the composite range to all
   channels is faithful here. A file that does set a per-channel range is now reported at
   import instead of silently drawn with the composite range.

| build | whole canvas | title band | outside the title band |
|---|---|---|---|
| Blend If + masks + overlays (§9, previous increment) | 5.546 (1.76 % >32) | 15.727 | 3.940 |
| + silhouettes, `iOpa` still folded into the layer opacity | 5.4838 (>32 90217, >64 30747, max 241) | not measured | not measured |
| **+ fill-opacity correction and the shape pass** | **5.4434 (1.86 % >32, >64 28972, max 205)** | **7.711 (3.77 % >32)** | **3.289 (0.05 % >32)** |

The title band's 15.727 → 7.711 therefore spans both of the last two rows; the split was not
measured, because the title-band script was only rerun for the final state. What *is* measured
per row is the whole-canvas mean and the >32/>64/max counts, and every step improved them
(>32 90217 → 88635, >64 30747 → 28972, max 241 → 205 from the silhouette row to the final one;
the >32 *percentage* went 1.76 % → 1.86 % across those rows because >32 counts stopped being
dominated by the title's gross error while faint glow pixels crossed the threshold — the
absolute count fell). Metric honesty: >2 covers 48.7 % of the canvas, so "48.7 % of pixels
differ" is not a signal — it is the noise floor of the file's own stored composite.

Independent evidence for the two conventions that could have been guessed wrong:

- **Offset direction.** Flipping the silhouette offset to the opposite side of the angle makes
  the match worse (title band 7.711 → 7.983, whole canvas 5.4434 → 5.5759, >64 pixels 28972 →
  33221), so the file's own composite supports "the silhouette lands at the effect's angle".
  For a drop shadow the importer negates `distance`, i.e. the shadow falls opposite the stored
  (light) angle — that reading is *not* validated here: this file contains no drop shadow.
- **Blur profile.** The harness' S19 compares the rendered glow against the independent CPU
  Gaussian reference and passes within one 8-bit level at both a direct radius (4) and a
  laddered one (20).

Still not drawn, and the remaining title-band error: the bevel (`ebbl`, highlight + texture)
and the 4 px inset gradient stroke (`FrFX`). Both need a distance transform of the layer's
alpha (a dilation), which this renderer does not compute; `isDrawnAsConfigured()` still reports
them, so the result stays honest. The worst leave-one-out node is now `more tint/Layer 46`
(a full-canvas Color-blend layer at 35 %: hiding it improves the mean by 0.152), i.e. the
remaining error is dominated by blend/quantization details rather than a missing feature.

## 11. The Stroke, and the outline distance field

The remaining edge error had a very specific signature, measured before any code was written:
across a letter stem of the file's `title copy` layer (row 400, canvas x 1711-1754) our render
was a smooth gold ramp, while the stored composite carries a ~4 px rim whose pixels are
(249, 246, 153), (250, 246, 144), (241, 215, 96) before settling onto exactly our values. Those
rim pixels *are* the file's `FrFX` stroke: 4 px, position inside, gradient paint
(251, 250, 158) -> (250, 230, 83) at -78 degrees scaled to 81 %, blend Screen at 100 % — the
first gradient stop reproduces the measured rim almost exactly. The deficit inside the glyphs
was +46 in blue within 4 px of the outline and *zero* deeper than 8 px, so this was an edge
feature, not a fill or brightness problem.

What landed:

1. **`core/DistanceField`** — the exact separable Euclidean distance transform (Felzenszwalb &
   Huttenlocher's lower-envelope construction) from the layer's alpha to the nearest outside
   pixel, as an 8-bit plane in the layer's own rect. `tools/render_verify` S20 checks it against
   brute force over every pixel of four shapes and pins the degenerate cases (a plane with no
   outline produces no field).
2. **The plane travels with the resource** (`ResourceData::grabDistance`, an A8 plane like the
   mask, uploaded lazily as GL_R8 on texture unit 4, persisted in the `ResNode_` block under
   project format **0.15**); it is baked at import, where the CPU pixels exist.
3. **The band is painted in the content shader**: an inside stroke cannot be more opaque than
   the shape it sits in, so `band = min(clamp(size + 0.5 - distance), coverage)` (a 1 px ramp at
   the band's inner edge, the shape's own alpha at the outer one), painted with the stroke's
   colour or gradient and blended with the layer's pixels through a runtime blend switch
   (`fxBlend`, the modes the shader can express; anything else is reported, never silently
   Normal). Photoshop stacks a stroke *above* the fill-replacing styles, and this does too.

| build | whole canvas | title band | outside the title band |
|---|---|---|---|
| §10 (silhouettes + fill opacity) | 5.4434 (>32 88635, >64 28972, max 205) | 7.711 | 3.289 |
| + stroke | 5.0731 (>32 52610, >64 12160, max 202) | 6.951 | 3.289 (unchanged) |
| **+ the chrome clipped to the layer's shape (see below)** | **4.5790 (>32 21441, >64 6246, max 206)** | **5.937** | 3.289 |

### The chrome was spilling a halo (largest single residual found after the stroke)

With the stroke in place the worst remaining error was not the bevel: it was a ring of ~120 k
pixels around the title glyphs where our render sat **19-24 levels brighter** than the file's own
composite, vanishing beyond 15 px from the outline. Two measurements pinned it:

- the excess correlated with the *predicted spill* of our chrome model (a blurred copy of the
  shape Screened outside it): predicted brightness 21.1, measured excess 19.0 in the ring;
- disabling the silhouette family moved the ring excess to ~0, and disabling **only** the chrome
  did the same, while disabling only the outer glow did not.

Photoshop's `ChFX` is the chrome layer of Bevel & Emboss, i.e. an **inner** effect: it paints
inside the layer's alpha and never spills. Two things had to change:

1. the tint pass can now cap its coverage by the unblurred shape (`uClipTexture`/`uClipEnable`,
   `FilterFrame::drawQuad`'s `aClipTexture`), and `LayerEffects::Silhouette::isInner()` says which
   effects are inner (chrome) and which are outer (drop shadow, outer glow);
2. the effect's distance/angle offset had to move from the tint pass to the copy that feeds it.
   A source offset shifts *every* sample of a pass — the clip included — so the clip was being
   shifted along with the silhouette and clipped nothing. Copying through the offset and blurring
   the shifted copy is equivalent (the Gaussian is symmetric) and leaves the tint pass unshifted.

Measured, with the chrome disabled entirely as the control:

| build | whole canvas | >32 | >64 |
|---|---|---|---|
| chrome as an unclipped silhouette | 5.0731 | 52610 | 12160 |
| chrome clipped, offset still in the tint pass (the bug) | 4.9335 | 50552 | 12752 |
| chrome disabled (control) | 4.9734 | 39423 | 9351 |
| **chrome clipped and offset on the copy** | **4.5790** | **21441** | **6246** |

So a correctly clipped chrome is worth far more than its absence (that control is the point: the
effect belongs there, our model of it was wrong), and S19 now pins the invariant — an inner
silhouette must not change a single pixel outside the shape, an outer one must.

One finding is worth recording because it was measured, not assumed: the field must be built
from the **128** level of the alpha, not from "any non-zero alpha". With a threshold of 1 the
antialiased edge pixels count as shape, which pushes the outline one pixel outward and, with it,
the band: that version scored 5.1850 / title 7.181, against 5.0731 / 6.951 for the 128
threshold. The composite's own rim sits where the 128 threshold puts it.

Independent evidence for the whole path: S20's analytic case (a square's exact rectangle
distance, the same rounding and band rule) matches the GPU render with a **max channel
difference of 0**, and the field round-trips byte-identically through the project format.

Still not drawn: the bevel (`ebbl`), satin, the inner glow/shadow, and a stroke whose position
is outside/centre or whose paint is a pattern. `isDrawnAsConfigured()` reports all of them.

### The distance field as derived state (and what it costs)

Two properties of this design deserve to be written down, because they are the parts a reader
would otherwise have to take on trust:

- **It is derived, not authored.** The plane is only meaningful while the pixels it was measured
  from are current, and this app has exactly one path that replaces a layer's pixels after
  import: a resource reload (`gui/res/res_ImageSetter` -> `core/ImageKeyUpdater` ->
  `ResourceData::grabImage`). `ResourceData` therefore records the image revision a field was
  measured from, and `ImageKey::syncDistanceTexture` re-bakes it once per pixel change, in the
  render path where a GL context is current - or refuses to upload a field measured from other
  pixels, so a stale band is never drawn. S20 renders a stroke, replaces the pixels (moving the
  shape 20 px) and checks that the band moved with them.
- **Cost, measured at -O2** (`DistanceField::build`): 25 ms for the 1365x478 title layer, 220 ms
  for a full 3840x1240 layer, ~0.95 s for 8K, with one int32 of scratch per pixel (4 B/px,
  halved from the first implementation, which used doubles) plus the persisted plane at 1 B/px.
  The transform is integer arithmetic, so the field is exactly reproducible; the harness checks
  it against brute force over every pixel and the analytic band case renders with a max channel
  difference of 0.

The alternatives, for the record: a GPU jump-flood would avoid the CPU cost, the extra plane and
the format bump (and would still work when a slimmed project has no CPU pixels), at the price of
~2*log2(N) full-screen passes per rebuild and its own invalidation cache; a GPU morphological
band (shape minus an erosion) is cheaper still but yields no distance gradient - so no bevel -
and its cost grows with the square of the stroke width. The CPU field was chosen because it is
exact, serves both consumers, and matches the grain of the mask plane already in this codebase;
the price is the persisted plane and the import-time work above.

### What the bevel would be worth (measured, before writing any bevel code)

The file's `title copy` also carries a soft bevel: highlight (253, 229, 150) at 100 %, shadow
(11, 11, 11) at 66 %, angle -149, altitude 0, size 14, soften 1, and a five-point contour. Two
measurements bound what drawing it could buy, and both were run against the file's own
composite:

- Painting the highlight over the glyph shape with a *per-pixel optimal* weight (an oracle no
  implementation can reach) moves the title band from 7.711 to 6.945, and a weight fitted from
  the distance field plus the local edge direction — i.e. a realistic bevel model — reaches
  7.37. A uniform highlight over the whole shape makes it *worse* (7.82 at 10 %, monotonically
  worse above that), so the highlight is not a flat brightening: it is confined to the outer
  ~6 px, and the interior of the glyphs already matches (the signed deficit deeper than 8 px is
  -0.5 in every channel).
- After the stroke landed, the same annulus analysis leaves a residual of about +15 in blue
  between 4 and 6 px inside the outline (18 k pixels), i.e. roughly 0.1 of the band mean.

So the bevel's own headroom is ~0.3-0.8 of title-band mean in the best case, and the rest of the
band's error is not a missing effect at all: measured per region, the band's non-glyph pixels
carry 6.6 (they are the busiest artwork on the canvas — the ornament, the stone, the bird —
where any small difference shows), while the same measurement outside the title folder is 3.6.
That is the honest state: the remaining band error is dominated by content complexity and blend
detail, not by an effect that is still missing.

## 12. Derived state and resource identity (the architecture, not the workaround)

Two things in this feature were doing the right thing for the wrong reason. Both are now
structural.

### The outline distance field is a declared DERIVED CHANNEL

Before: the field was inferred from its own existence. `ImageKey::syncDistanceTexture` re-baked it
only `if (resource.hasDistance())`, so "a consumer wants this and it is not here" could not be
expressed at all - a project that arrived without a plane (a file written by an older build, or a
derivation that failed) could never acquire one, and the stroke silently did not draw. And the
computation ran inside a draw: a 25-220 ms CPU bake under the frame, once per image revision.

Now (`img::ResourceData`):

- a **registry** of derived channels (`Derivation_Distance`, extensible), each with `wanted`,
  the plane, the image revision it was measured from, the **recipe** it was built with and a
  revision for the GPU cache;
- `isDerivationStale()` = wanted && (absent || source revision differs || recipe differs), so
  *wanted-but-absent*, *measured from other pixels* and *built by an older recipe* are three
  distinguishable, reportable states rather than one silent assumption;
- the computation is a **deriver** the owner of the pixels installs
  (`DistanceField::deriveResourceChannels`), because `img` cannot call into `core`. The resource
  runs it after every pixel change - and every pixel change goes through `ResourceData::grabImage`
  (import, project load, a reload's `ImageSetter` -> `ResourceNode::updateHandle`) - and when a
  need is first declared. The render path now **only uploads**, exactly as it does for the mask;
  nothing computes inside a draw.
- **consumers declare the need**: the importer (the only caller of `setLayerEffects`) sets
  `Derivation_Distance` from the layer's stroke, and the project loader installs the deriver and
  derives from the state the file recorded, so a wanted channel with no plane or an old-recipe
  plane is rebuilt at load, from the pixels that were just read.
- a failed derivation is reported **once, where it is attempted** (`deriveResourceChannels`),
  not per frame - which is what makes the honesty gap closable without spamming the log.

S20 checks the contract end to end: a resource that starts with no channel, declaring the need
derives it at once; replacing the pixels re-derives it in the same call; the band follows the
replaced image. On the real file the refactor is behaviour-preserving - whole canvas 4.5790,
>32 21441, >64 6246, identical to before it.

### Identity is a stable id, with the name as the fallback

Before: a source reload matched layers by **name**. `findCorrespondingNode` compared
`ResourceData::identifier()` (Photoshop's `luni`) plus a layer/folder flag and required a unique
match among siblings, so a rename in the source broke correspondence (the GUI told the user to
rename "from within AnimeEffects") and two same-named layers were unresolvable.

The `.anie` format stored **only** that name - the `ResNode_` block carried identifier, child
count, kind, rect, blend mode, image, mask and distance, and the layer -> resource link was a
save-scoped reference id regenerated on every save, not a persistent identity. So an id had to be
added, not merely used:

- `img::ResourceData::stableId()`, filled from Photoshop's **`lyid`** (`img::psdLayerStableId`,
  layers and groups alike) and persisted in the `ResNode_` block under minor **16**;
- `img::ResourceData::matchesIdentity()` owns the rule - kind must agree, the **id decides when
  both sides have one**, otherwise the name - and both callers that used to test names on their
  own (`gui::res::findCorrespondingNode` and `ResourceNode::getCountOfSameSiblings`) now ask it,
  so the reload and the duplicate check cannot drift apart;
- the fallback is the point: a project written before ids has none on either side, and a file
  whose producer writes none still reloads exactly as it did before.

On the real file every one of its **57 records carries a distinct id** (`lyid:109` ... `lyid:351`),
which the probe now prints - so on this file a reload matches on ids throughout.

Both of those were addressed afterwards, in the pipeline work rather than here: the reload now
reconciles against a format-neutral description of the file (`ctrl::SourceDocument`), matching by
identity and applying a plan of keep / update / append / remove, and the full-plane comparison
(`hasSameLayerDataWith`) was replaced by `ctrl::diffResource` / `diffObject`, which say *what*
differs - including the parts (mask, Blend If, layer effects, fill opacity, visibility, blend) the
old reload detected but never applied. See section 15.

## 13. The remaining error, measured instead of dismissed

The residual after §11 was reported as "mean 4.5790, and >2 covers 46.9% of the canvas, so that
is the file's own noise floor". That last clause was an **assertion with no measurement behind
it**, and it was wrong. This section is the measurement, in the order it was done, including the
hypotheses that died.

### 13.1 What the reference actually is (the comparison is fair)

- The PSD's header declares **3 channels**: its stored composite is a **flattened, fully opaque
  RGB** image, and the render is opaque too (every readback pixel has alpha 255). So the
  comparison is RGB against RGB - no transparency or matting artifact, and no alpha weighting to
  argue about. (The first thing checked, because "the render is unpremultiplied against a
  straight reference" is where such a residual usually hides.)
- **Not a global geometry problem**: shifting the render by ±0.25, ±0.5 or ±1 px in x or y makes
  the match worse for *both* the render and the independent compositor (4.5790 at zero offset,
  5.05/5.00 at ±0.25, 5.70/5.63 at ±0.5, 7.19/7.09 at ±1). Zero shift is the optimum.
- **Not a global tone or colour curve**: binning the model's value per channel and mapping each
  bin to the composite's mean leaves a within-bin spread (4.78 / 2.69 / 3.30) as large as the raw
  residual (2.83 / 1.58 / 3.33) - the composite is not the model put through a curve.

### 13.2 The residual is a boundary phenomenon, and in the flat interior it *is* the rounding floor

Distance to the nearest composite gradient (|grad| > 2), effect-free pixels, mean max-channel
difference:

| distance | before the fix | after |
|---|---|---|
| 0-1 px | 4.1889 | 3.4417 |
| 1-2 px | 4.4753 | 3.6562 |
| 2-3 px | 3.7583 | 3.0447 |
| 3-4 px | 2.8759 | 2.3274 |
| 4-6 px | 1.7358 | 1.3585 |
| 6-8 px | 0.8092 | 0.5841 |
| >= 8 px | 0.5299 | 0.4141 |

With a stricter flatness definition (6-8 px from *any* gradient) the difference is 0.87, and
**>= 8 px out it is 0.50** - which is exactly one 8-bit rounding step, the floor for a float
compositor rounded once. And where exactness is *provable* it is stronger than that: at every
sampled pixel whose topmost contributor is an opaque Normal layer at opacity 255, unmasked, with
no Blend If, no fill opacity and no effects, the composite equals that layer's stored colour with
a **difference of 0 on all 99 sampled pixels**, and so does the render. So on flat content the
answer to "is this precision?" is yes, and it is proven, not assumed.

Where the residual *does* live: **99.2% of the whole-canvas difference mass is within 4 px of a
composite gradient**, and the extreme tail (>64, ~6.2k px) is the title band's `lfx2` effects -
profiles there show the composite carrying bright highlights (255,193,93) where the render has
(59,72,48), i.e. gradient-fill/stroke/chrome/inner-glow/contour detail this build draws only in
part.

### 13.3 The bug this found: the layer mesh was refusing to draw part of every layer

`img::GridMeshCreator` builds the triangular mesh every layer is drawn through, and its notion of
"is there content here" was **alpha > 10**. That single threshold decided which cells exist and
where the burr reduction stops - and whatever the mesh calls empty is never rasterized, so it was
silently discarding the faint end of every antialiased edge and dropping whole cells whose content
was fainter than 4 % opacity. Verified by an A/B on the production render path
(`PROBE_CELL=30000` makes every image key fall back to an exact bounding quad mesh, i.e. identity
mapping, no cell culling, no reduction):

| build | whole canvas | >2 | >32 | >64 |
|---|---|---|---|---|
| generated mesh, content = alpha > 10 | 4.5790 | 2232782 | 21441 | 6246 |
| exact quad mesh (control) | 3.9942 | 1647873 | 20945 | 6188 |
| **generated mesh, content = alpha != 0** | **3.9943** | **1647876** | 20947 | 6190 |

13 % of the whole-canvas difference and **585k pixels of >2 difference were content the mesh
refused to draw**. After the fix the generated mesh and the exact quad agree to within **6 pixels**
of the 4.76 M (triangle-boundary rasterisation), i.e. the mesh no longer changes what a layer
draws. Outside the effect rects the error fell 3.9400 -> 3.2215 and every distance band improved;
inside the title band's effect rects it rose slightly (8.6296 -> 8.8935), because content that was
previously not drawn now interacts with the title's only-partly-modelled effects. Reported, not
smoothed over.

Making the mesh honest about faint content also exposed a latent **out-of-bounds cell write** in
the reduction loop: for a content pixel no cell covers, `cellIds` is still -1 and the code indexed
the table with `(-1 / tableWidth, -1 - that * tableWidth)` = (0, -1). It is now recovered
geometrically (`CellTable::cellAtPixel`) instead of read through a bad pointer.

### 13.4 The semantics are not the problem (each one tested)

Against the file's own composite, on effect-free pixels, the independent numpy compositor
(`komp.py`, own container parser, own blend math) scores 3.998 and the production render 4.238.
Every variant that changes one convention makes it worse: Rec.709 luma (4.365), no ClipColor
(4.406), Color's operands swapped (26.99), Overlay branching on the source instead of the backdrop
(33.14), ignoring Blend If (39.37), ignoring masks (6.92), ignoring layer opacity (15.66). Byte
truncation of the 8-bit accumulation is worse (8.32); **rounding** the accumulated colour to
8-bit after each layer is *better* than full float (3.998 -> 3.737), which is consistent with both
Photoshop and this renderer compositing in 8-bit premultiplied buffers.

### 13.5 What is left, attributed

Outside the title band's effect rects, comparing the render, the independent compositor and the
composite pixel by pixel (after the fix):

| class | px | render vs composite | model vs composite |
|---|---|---|---|
| all three agree (<=2) | 2736991 | 0.622 | 1.058 |
| render and model agree, **both** miss the composite | 1236027 | 8.824 | 8.578 |
| model closer to the composite | 49329 | 7.583 | 5.717 |
| render closer to the composite | 72287 | 2.193 | 4.326 |

**This class was resolved by section 14** - it was the Blend If edge rule, and after that fix the
same measurement reads 0.74 outside the effect rects with 257 pixels of >8 in 4.1 M. The numbers
below are the state *before* that fix, kept because they are what the attribution in §14.1 was
solved against.

So of what remained at this point: **7.4 M of the 19.0 M difference mass was a disagreement
between the file's stored composite and *any* exact per-pixel compositing of its stored layer
pixels** - two
independent implementations (this renderer and a numpy one sharing no code) land on the same value
there, ~8.8 away from the composite, with a consistent chroma signature (R -4.5, B +7.6, luma
within 0.4). 87% of those pixels are within 2 px of a gradient, i.e. at partial-coverage
boundaries. What that is *not*: a sub-pixel shift (measured, §13.1), a blur of the whole image
(blurring the render makes the match worse: 3.99 -> 4.22/4.53/4.76 for 1/8-3/4-1/8, 1/4-1/2-1/4 and
1/3-1/3-1/3 kernels), or an effect (these pixels are outside every effect layer's rect). The
remaining ~120k pixels where the two implementations disagree at all are this renderer's own
residue.

### 13.6 The test that should have existed

`tools/render_verify` **S22** pins the invariant that needed no oracle: the same layer, drawn
through the generated mesh and through an exact bounding quad, must produce the same pixels, and
both must reproduce the layer's own bitmap drawn 1:1. Its fixture has an alpha-6 haze block and a
ramp down to alpha 1 - content the old threshold called empty - so it fails on the old code. The
existing golden cases could not have caught this: they deliberately use hard-edged, canvas-sized
content, which pushes the mesh boundary to the canvas border.

## 14. The Blend If edge rule was wrong: the two edges combine with min(), not a product

Section 13 left one class unexplained: ~1.24 M pixels (outside the title band's effect rects)
where the production render **and** an independent numpy compositor agree with each other and
both sit ~8.8 (max-channel) from the file's own composite, 87 % of them within 2 px of a
gradient, with a chroma signature (R -4.5, B +7.6, luma inside 0.4). Section 13 excluded the
obvious families one at a time - sub-pixel shift, whole-image blur, a tone curve, layer effects,
per-channel Blend If bytes, masks, opacity, every blend-mode convention, and (by then) the layer
mesh. This is what it was.

### 14.1 Asking the inverse question instead of guessing forward

Two things made the question askable at all:

1. modelling the accumulation as **8-bit with rounding per layer** (the framebuffers are RGBA8)
   brought the independent model to **0.37** of the production render on effect-free samples
   (0.3684 vs 0.2933 for the composite) - i.e. the model became a trustworthy proxy, so any
   remaining difference from the composite is *Photoshop's*, not the renderer's;
2. the residual could then be written as a linear inverse problem: for every layer, the
   numerical derivative d(model)/d(weight) against (composite - model) over the pixels in
   question. A sparse answer names a mechanism; a diffuse one rules the family out.

The solve was not diffuse. Top of the list, where a +100 % relative weight change would explain
100 % of the class residual:

| layer | mode | opacity | best-fit relative weight change | explains |
|---|---|---|---|---|
| `Layer 49` | Color | 255 | **+0.601** | **42.9 %** |
| `Layer 24` | Color | 125 | +0.682 | 14.3 % |
| `Layer 50` | Overlay | 255 | -0.359 | 11.0 % |
| `Layer 51` | Color | 89 | -0.741 | 10.6 % |

One layer - a full-canvas **Color** layer at 100 % opacity - carries 43 % of it, and every layer
in the top of that list has a destination range. So: recover that layer's weight from the
composite itself. Forcing `Layer 49`'s weight to a scanned value, re-running the whole stack
above it and keeping the value that minimises the difference, gives **0.44 at a backdrop luma of
95** where the model's own weight is 0.27.

### 14.2 What the two candidate rules predict there

`Layer 49`'s destination range is `blackLo 29, blackHi 179, whiteLo 68, whiteHi 137` - and note
`179 > 68`: **the handles cross**, as they do in 3 of this file's 7 range layers (the 4th
crossed-looking entry is degenerate, `255,255 / 0,0`, where both rules give 0). At luma 95:

- rising edge `(95-29)/(179-29)` = 0.440
- falling edge `(137-95)/(137-68)` = 0.609
- product (what this repository did, and what the independent compositor did) = **0.268**
- `min` = **0.440**  <- what the composite implies

Scored over every range layer at once, on effect-free grid samples, against the file's composite:

| edge combination | vs composite | vs render |
|---|---|---|
| product | 3.2933 | 0.3684 |
| **min** | **0.8461** | 2.8625 |
| max | 31.8535 | 34.2482 |

**0.85 is the 8-bit floor** for a stack this deep (204 layer steps on the effect-free grid), and
`>32` falls from 8 samples to 1. The separation is 4x, in the direction of the file's own
composite, on pixels where the render is the outlier.

### 14.3 Why min is also the right reading

The two sliders describe the *same* coverage: the black side says "fully visible at or above
`blackHi`, ramping up from `blackLo`", the white side says "fully visible at or below `whiteLo`,
ramping down to `whiteHi`". A pixel is visible only where **both** hold, and the coverage is the
less permissive of the two — a minimum. A product would mean two independent attenuations, which
is a different (and, as measured, wrong) statement. On an **uncrossed** range the two rules are
*identical* - the ramps never dip in the same place, so `min == product` everywhere - which is
exactly why this survived every earlier check: the harness's own blend-if cases used ordinary
ranges, and the crossed one they did use (`255,255 / 0,0`) is degenerate under both readings.
It is also why the two implementations agreed: both were written from the same misreading.

The same function serves the source range ("This Layer"), so the fix applies there too. This
file cannot separate the two there - its only source range is `255,255 / 0,0`, degenerate under
either rule - so the source case is fixed by the same argument, not by this measurement, and is
labelled as such.

### 14.4 Where it landed

- `core/BlendIf.cpp` (`blendIfRangeWeight`) and
  `assets/shaders/LayerDrawingFrag.glsl` (`BlendIfRange`) - the two production implementations -
  now combine the edges with `min`.
- `tools/render_verify/cpu_ref.h` carries the same rule, and S16 has a new case that does **not**
  compare against that reference (which shares the semantics, so agreeing with it would prove
  nothing): a backdrop of exactly luma 128 under a crossed range `64..192 / 96..224` and an
  opaque `(200,40,40)` top layer must land on the **hand-computed** `(164,84,84)` -
  `min(0.5, 0.75) = 0.5` - where the product reading would give `(155,95,95)`.
- The probe re-run on the 114 MB file is the end-to-end check (see below).

### 14.5 The numbers after the fix

| build | whole canvas | title band | outside it | >2 | >8 | >32 | >64 |
|---|---|---|---|---|---|---|---|
| §11 (before this investigation) | 4.5790 | 8.6296 | 3.9400 | 2232782 (46.89 %) | 852647 | 21441 | 6246 |
| + the mesh content test (alpha != 0) | 3.9943 | 8.8935 | 3.2215 | 1647876 (34.61 %) | 817145 | 20947 | 6190 |
| **+ min() for the two range edges** | **1.3024** | 4.8383 | **0.7446** | **278990 (5.86 %)** | **88294** | 19419 | 6001 |

Split by region, after the fix: outside the ten `lfx2` effect rects (86.4 % of the canvas) the
difference is **0.7446 mean with 257 pixels of >8 and 59 of >32** - the 8-bit rounding floor. The
remaining 99.6 % of the >32 pixels sit inside those rects (13.6 % of the canvas, mean 4.8383),
which is the title band's effects: gradient fill, stroke, chrome, inner glow, contour and bevel,
models this build knows are incomplete and *reports* through `isDrawnAsConfigured()`.

Answered independently, by the numpy compositor that shares no code with the renderer, outside
the effect rects:

| comparison | mean | >2 | >8 | >32 |
|---|---|---|---|---|
| compositor with the PRODUCT rule vs the file's composite | 3.4530 | 1252022 | 556489 | 1034 |
| **compositor with min() vs the file's composite** | **1.1678** | 145789 | **301** | **74** |
| render vs the file's composite | 0.7446 | 105344 | 257 | 59 |
| compositor with min() vs the render | 1.2090 | 152795 | 59 | 2 |

301 pixels of 4.1 M differing by more than 8 levels, from an implementation that parses the PSD
itself and implements the blend math itself, against the reading it was written from. Before the
rule was corrected the same comparison was 556 489 pixels.

### 14.6 How the rule was verified, and how far that evidence reaches

The 0.44-vs-0.268 point in 14.2 is one sample. The rule was then verified two ways that do not
depend on a fit.

**A bound, not a curve fit.** Each crossed range has a ceiling: the largest weight its rule can
produce anywhere. For `Layer 49` (`dst 29/179/68/137`) the product peaks at **0.243** (at the
crossing luma 103) while `min` reaches **0.493** — and with the layer at opacity 255 those are
the total weights. Recovering, per pixel, the weight that reproduces the file's own composite
puts **37.3 % of the usable samples above the product's ceiling**, with a maximum of **0.540**
(more than twice the ceiling). No ramp shape can rescue the product there.

**The recovered curve, binned by the luma the range is keyed on.** The weight that best explains
the composite was recovered per pixel, over the whole 0..255 backdrop sweep, and compared with
each rule's prediction:

| layer | range | crossing luma | ceiling max/min | ceiling product/min | n samples | mean abs err `min` | mean abs err product | above product ceiling |
|---|---|---|---|---|---|---|---|---|
| Layer 49 | dst 29/179/68/137, opa 255 | 103.0 | 0.493 | 0.243 | 10430 | **0.0083** | 0.0466 | 37.3 % |
| Layer 24 | dst 71/128/32/183, opa 125 | 101.7 | 0.264 | 0.142 | 11649 | **0.0100** | 0.0212 | 22.9 % |
| Layer 46 | dst 82/114/102/136, opa 89 | 108.2 | 0.286 | 0.234 | 3350 | 0.0092 | 0.0090 | 0.9 % |

Bin by bin, the recovered weight matches `min(lo, hi)` wherever the two rules differ (e.g. at
backdrop luma 96-112: recovered 0.444 against `min` 0.447 and product 0.239) and matches both
where they agree — the fingerprint of `min`, not a curve that merely fits better. The third
crossing is kept in the table as a **control**: its handles cross so shallowly (ceilings 0.286
vs 0.234 after opacity) that it cannot separate the rules, and the method duly reports no
preference — which is what a measurement should do when there is nothing to see. One sample of
10430 also exceeds even `min`'s ceiling (0.540 against 0.493); it is a single point, reported
rather than hidden.

**What this evidence does not cover.** It is one file. All 183 PSDs on this machine were scanned
for non-default blend ranges: **1 has a crossed range** (this one); the other 182 either leave
the ranges at the default or carry none at all (the 104-file art corpus has `blendIf=0`
everywhere). External corroboration was sought and is thin: the open-source implementation that
addresses split sliders at all *refuses* the crossed case (`shadows_hi >= highlights_lo` raises),
and the rest document the uncrossed behaviour only. So the rule here is Photoshop's own output
measured on this file, not documentation read — which is the strongest evidence available and
also a single point of failure, stated as such. The source range ("This Layer") cannot be
separated by this file at all (its only source range is degenerate), so the same `min` is applied
there by the argument in 14.3, not by measurement — and 14.3's argument is the one thing that
does not rest on the file.

## 15. The import pipeline: one description, reconciliation, detect == apply

The work above was about what the renderer draws. Section 12 ended with the reload path still
described as crude - a second tree walker with a different population, raw pointers into a parse
result that had to outlive every node built from it, an abort when the structure changed, and a
comparison that detected things it never applied. That is now closed, and the pieces are:

- **`ctrl::SourceDocument`** (`src/ctrl/SourceDocument.h`): one format-neutral description per
  container format - `describePsd`, `describeOra`, `describeImage` - carrying name, stable id,
  kind, rect, pixels, mask, blend, visibility, clipping, opacity, fill opacity, Blend If ranges
  and layer effects. `ctrl::buildResourceTree` and `ctrl::buildObjectTree` consume it, and the
  importer and the reload both go through them, so a resource a reload builds is the resource the
  import would have built (masks, derivation hook and declared derived channels included).
- The two divergent walkers are gone (`img::Util::createResourceNodes(PSDFormat&, …)` and its ORA
  sibling, plus `img::Util::createResourceNode(QImage…)`), the raw-pointer lazy image loaders with
  them, `ResourceUpdater::mPSDFormat` with them, and `img` no longer depends on `gui`.
- **The reload reconciles**: `gui::res::ResourceUpdater` reads the file once into a description,
  matches the project against it by identity (`ctrl::matchesIdentity`), and applies a plan of
  keep / update / append / remove. No GUI-item/model index mirroring (items are found by node), no
  transient abandon flags, no abort when the structure changed - a layer the file gained is
  appended and one it dropped is removed (or kept and reported when the timeline still draws it).
  The commands are collected before any are pushed, so a reload that finds nothing to do leaves
  the project unedited.
- **Detect == apply** in both directions, per `ctrl::diffResource` / `ctrl::diffObject`: pixels
  and rect, mask and its flags, the resource's blend and visibility, and on the object side the
  blend mode (the image key's - not the resource's, which the renderer ignores), Blend If, layer
  effects, fill opacity, the opacity default key, visibility and clipping. What is deliberately
  not taken from the file (a rename, an ambiguous sibling, a removal that is kept) is written to
  the app's log, never dropped silently. A reader that cannot describe a property says so
  (`Entry::hasObjectData`), so its silence is not read as "the file has none" and cannot wipe what
  the artist set.
- **`tests/import_reload.cpp`** drives all of it headlessly - import, unchanged reload, changed
  pixels, rename, opacity/visibility/blend, an added mask, an appended layer, a removed layer, and
  a format whose reader describes no Blend If - with expectations defined by the fixtures the test
  writes itself. 35 checks; flipping the `hasObjectData` gate makes it fail.

## 16. The solid-colour stroke was never drawn (a key that does not exist)

`Stroke::isSolid()` tested `paintType == "SCol"`. Photoshop writes **`SClr`**: the `PntT` item of
a stroke's `FrFX` block is an enum whose *type* key is `FrFl` and whose *value* key is `SClr`
(solid), `GrFl` (gradient) or `Ptrn` (pattern). Measured across every PSD on this machine:

| file | strokes | `PntT` value | `SCol` occurrences |
|---|---|---|---|
| `000033.psd` | 17 | `SClr` x17 | 0 |
| `000034.psd` | 15 | `SClr` x15 | 0 |
| ALL FATHER (114 MB) | 8 | `GrFl` x8 | 0 |
| 180 others | 0 with effects | — | 0 |

So a solid stroke read as *neither solid nor gradient*, `isDrawable()` returned false, and the
stroke was dropped — silently, because nothing in the application reported undrawn effects
(section 17). It survived every test because the harness's own stroke helper was called with
`"SCol"`: the test wrote the same wrong key the code expected, so 41 S20 checks confirmed the
bug rather than the format. The one file that does exercise strokes uses gradients, which is why
the picture never showed it.

Fixed in `core/LayerEffects.h` (`isSolid()` accepts `SClr`), and the two harness sites now carry
the real key plus an assertion that the *parsed* paint type is `SClr` and that the imported layer
still carries its stroke. Falsified: restoring `"SCol"` makes **6 checks fail** — the 3 px band
is missing entirely, the modelled stroke is reported as not drawable, and the import wrongly
claims an undrawn effect.

## 17. A predicate nobody called: the effects report

`LayerEffects::isDrawnAsConfigured()` and `activeTypes()` had **no caller outside the tests and
the probe**. The boundary the header describes was real, but the application never said it: an
imported PSD with a bevel simply rendered without it. The import log says `success` either way.

What the files here actually use (all 183 PSDs, per-effect attribution by mirroring the
production predicate):

| effect | active | of those not drawn |
|---|---|---|
| Bevel and Emboss | 3 | **3** (all in the 114 MB file) |
| Stroke | 25 | 22 (outside/centre bands) |
| Drop Shadow | 13 | 0 |
| Gradient Overlay / Chrome / Outer Glow | 3 / 3 / 1 | 0 |
| Satin, Inner Glow, Inner Shadow, Colour Overlay | **0** | — |

Photoshops writes all seven style keys into every `lfx2` with an `enab` flag, so the report must
read the flag: the file's "Satin" and "Inner Glow" are *present but switched off*, and a report
that listed present keys would accuse it of effects it does not use (pinned by an S20 check).

Fixed: `ctrl::undrawnEffectNotes(document)` is the one place that turns the predicate into a
sentence, and both paths that bring a file into the project push it to the app's log — the import
(`ResourceUpdater::createResourceTree`) and every reload (`reconcile`) — with the loader's own log
carrying it too. The S20 suite now asserts both directions: a PSD whose effects are all drawn
reports nothing, and one carrying a bevel reports it by layer and by name.

**A related hole, measured but not closed.** `describeFolder` reads no `lfx2`, so a layer style
on a *group* is neither applied nor reported. Photoshop allows that, and this build would be
silent about it in exactly the way section 17 is about layers. Measured across the same 183 PSDs:
1648 folder dividers, **0 of them carrying an `lfx2`** (46 non-folder records do). So the hole is
real in the code and unexercised by every file available here - which is why it is written down
here rather than guessed at, and why closing it would need a file to check against, not just a
parser change.

That assertion immediately earned its place: the first version called `appendUndrawnEffects`
*before* setting `mLog = "success"`, so the notes were overwritten by the very line they were
appended to. The suite failed; the order is now stated in the function.

## 18. What a reload costs, and what it should cost

Section 15 describes the reload's *semantics*. Its *cost* was never measured, and the answer to
"why is change detection a byte compare per layer?" is that the compare is not the expensive
part — decoding everything to have something to compare is. Measured on the two largest files
available (parse = read + records, no decode; digest = FNV-1a over each layer's *compressed*
channel bytes):

| file | parse (records, compressed bytes in memory) | structure-only description | full description (every layer decoded) | digest of compressed bytes |
|---|---|---|---|---|
| ALL FATHER, 114 MB, 51 layers, 92 MB compressed | 78 ms | 4 ms | **6169 ms** | 129 ms |
| `000033.psd`, 135 layers, 182 MB compressed | 143 ms | 11 ms | **27882 ms** | 258 ms |

These are reproducible: `tools/psd_composite_probe/build_and_run.sh --cost [<psd>]`
(`reload_cost.cpp`) times all four stages on any PSD, with no GL and no project. The digest
itself is deterministic - repeated runs on the 114 MB file give the same value
(`a5734fcc51aab1d7`) and a digest of the same bytes cannot be faked by a cache. The parse figure
is the one that moves with the page cache: 78 ms warm against 352 ms cold on this machine, which
does not change any conclusion (the decode it is compared against is 6.2 s), but is the reason
the numbers are quoted per run rather than as constants.

`ResourceUpdater::reconcile` calls `describeSource(filePath, /* wantPixels = */ true)`: a reload
that finds nothing to do still pays the full decode. The architecture that fixes it, in the order
the tiers pay off:

1. **The file cannot have changed.** Record `(path, size, mtime)` when the file is read; if all
   three match, a reload is a `stat` and a message. Cheap, but only removes the accidental case.
2. **Compare the compressed bytes, not the pixels.** Every layer's channel bytes are in memory
   after the parse; lossless compression means **equal compressed bytes imply equal pixels**, so
   a digest match is a *sound* "unchanged" and a mismatch can only be a false positive (a
   re-encode that decodes the same), which costs one decode and ends in a no-op. This is the tier
   that matters: 210 ms instead of 6.2 s on the 114 MB file, ~400 ms instead of 27.9 s on
   `000033.psd`.
3. **Decode only what the digest says changed** - and byte-compare those decoded pixels before
   applying, so the plan still says exactly what it does.
4. The structure-only description (4 ms) is what makes a metadata-only change cheap too; it must
   compute the *padded* rect (the importer pads when it builds the texture) or a structure-only
   comparison would report every layer as moved.

Storage is the one open decision, and it is a product decision rather than a technical one:
a digest per layer has to be remembered between reloads, which means either a field in the
resource (survives reopening a project - fast from the first reload - and needs a project-format
version bump, so older builds cannot open the new files) or a session-only cache (no format
change, but the first reload after opening a project pays the full decode once and is fast
after). The mtime tier above needs no storage either way.

## 19. The mesh content test: a rule that did not survive the corpus

Section 13.3 changed `GridMeshCreator`'s content test from `alpha > 10` to `alpha != 0`. That test
is both the fidelity rule - a cell exists only where it is true, and the mesh is what draws the
layer, so whatever it calls empty is not drawn at all - and the geometry rule. The objection to
dropping the threshold is concrete and names the two cases any rule has to separate:

- **erase residue**: an opaque shape erased with Flow < 100 % leaves pixels at alpha 1..10 that
  are invisible in the artwork but still in the file. These should not be drawn and should not
  buy mesh geometry.
- **soft lighting**: a glow painted as a smooth ramp has its whole low-alpha skirt in the same
  range, and that skirt is intended content. Cutting it at 10 is wrong.

### 19.1 No per-pixel LEVEL separates them (a per-region MEAN does - see 19.5)

Both cases live in the same alpha range, so any single cut ON A PIXEL'S LEVEL damages one of
them: the two populations are told apart by a width statistic over a region, not by any value a
single pixel can be compared against (19.5 gets there - the region's mean alpha - after 19.2
disposes of the first attempt). Measured
(`build_and_run.sh --mesh [<psd>] [--seed T]`; "vertices" is the production `GridMeshCreator`'s own
count, each policy realised by zeroing the alpha of what it calls empty):

| T | faint px (1..T-1) | in a component with a solid core | in a core-less island | islands | vertices `> T` | vertices seed+growth |
|---|---|---|---|---|---|---|
| 2 | 1 525 568 | 98.92 % | 16 474 | 6 364 | −2.28 % | −0.12 % |
| 4 | 3 910 559 | 99.53 % | 18 454 | 6 584 | −3.94 % | −0.13 % |
| 10 | 9 044 100 | 99.74 % | 23 253 | 6 683 | −7.66 % | −0.16 % |
| 32 | 21 257 002 | 99.88 % | 25 411 | 6 728 | −16.93 % | −0.17 % |

The split barely moves across a 16x range of `T`, so the threshold could not have been tuned into
correctness - at `T = 2` it already cut 1.5 M pixels belonging to solid features. And the geometry
it bought was mostly paid for with content: at `T = 10` the old rule saved 7.66 % of vertices, of
which the residue accounted for 0.16 % (6 683 islands averaging 3.5 px) and the other 7.5 % was
the skirt of real features.

### 19.2 The first rule proposed here, and why the corpus killed it

The rule proposed was: keep every connected component of `alpha > 0` that contains a pixel `>= T`,
drop the rest - i.e. use the level as a *seed* rather than a cut, so no feature with a solid core
is ever cut and a core-less island is dropped whole. It looked good on the one file above,
because there the residue is 0.26 % of the faint pixels.

**The vocabulary, which is this document's own and not Photoshop's.** A *region* is a connected
component (8-connected) of pixels with `alpha > 0` within one layer. A region is *core-less* when
no pixel in it reaches `T` (10 in these runs) - core-less regions are exactly the ones rule 19.2
would delete, and every number below is about that population. Two statistics of a region's own
alpha histogram then split it:

- **modal share** = (pixels at the region's most frequent alpha) / (region pixels). 1.00 means
  every pixel has the same value.
- **peak** = the region's maximum alpha.
- **uniform veil** = modal share >= 0.75 **and** peak <= 3, i.e. nearly all pixels at one very low
  level. **soft gradient** = modal share < 0.60, i.e. no level dominates - the alpha varies across
  the region. **in between** = neither (0.60..0.75, or a flat region peaking at 4..9).

The cutoffs 0.75 / 0.60 / 3 are **chosen, not measured** - they were the operational form of a
hypothesis about intent ("a flat leftover is not painted, a ramp is"), and 19.5 supersedes the
whole classification with a quantity that needs no cutoffs at all.

It does not hold. The corpus says so, in numbers: audited with
`build_and_run.sh --audit [<psd>]` over 15 artwork PSDs (1 704 498 core-less pixels in 82 231
components), splitting the dropped set by its **own distribution**:

| the rule would drop | components | pixels | share of components | share of pixels |
|---|---|---|---|---|
| uniform veils (modal share >= 0.75, peak <= 3) | 73 685 | 848 635 | 89.6 % | 49.8 % |
| **soft gradients (modal share < 0.60)** | **6 270** | **712 121** | 7.6 % | **41.8 %** |
| in between | 2 276 | 143 742 | 2.8 % | 8.4 % |

**41.8 % of the pixels the rule deletes are soft gradients** - ramped, fading regions, i.e. exactly
the painted soft content the rule exists to protect. Whole files are dominated by them: on
`000025.psd` the rule would delete 221 484 px of soft gradient against 3 530 px of veil, and on
`006764.psd` 4 191 gradient components covering 100 860 px. The one file used to design the rule
was the unrepresentative one.

Looking at the dropped regions rather than only counting them says the same thing: the largest
core-less component of the 114 MB file (1 776 px, 45x112, peak alpha 4) is a coherent
crescent-shaped mark, not speckle - see the `_marked.png` / `_alpha_x8.png` crops the audit writes.

### 19.3 What actually separates the two populations

Two independent properties of a region's own alpha field, and they agree:

| file | boundary alpha / peak, veils | ... soft gradients | ... in between |
|---|---|---|---|
| ALL FATHER | 0.97 | 0.44 | 0.60 |
| `000025.psd` | 0.92 | 0.50 | 0.56 |
| `000061.psd` | 0.93 | 0.30 | 0.38 |
| `000033.psd` | 0.91 | 0.53 | 0.64 |

A veil has nearly all its pixels at one very low level and its boundary already at **~0.95 of its
own peak** - it stops abruptly, which is what a leftover of an erase/rounding operation looks
like. A soft feature's boundary sits at **~0.30-0.53 of its peak** - it fades out, which is what
painted soft content looks like. Together with the modal share, that is the "distribution +
spatial" signature the two cases actually have, and connectivity is not part of it: the falsified
rule used connectivity as a *proxy* for "is this painted", and on real artwork that proxy is
wrong about 41.8 % of the mass.

### 19.4 What to do instead

The refined rule, stated so it can be argued with. It cuts on the region's **mean** alpha -
its average coverage over its `alpha > 0` pixels - not on its peak, and it puts that cut at the
format's floor (section 19.5 measures why):

- a pixel with `alpha >= 2` is content;
- an `alpha == 1` pixel is content when it belongs to a region that contains an `alpha >= 2`
  pixel, i.e. when it is part of something that is itself visible;
- so what is dropped is exactly **a region in which every pixel is alpha 1** - no attached
  visible content, no internal variation, nothing above the format's floor (19.5). No modal-share
  or peak cutoff appears in the rule; those were the scaffolding of 19.2's hypothesis, not part of
  the answer.

The asymmetry of the two possible mistakes decides the default. Keeping a veil costs geometry
measured at well under the 3.85 % that dropping *all* core-less components saves on the worst file
here, and the picture is right; deleting painted soft content is up to 41.8 % of the dropped mass
and the picture is silently wrong. So the default stays `alpha != 0` (draw everything), the veil
rule is an explicit per-layer choice, and whichever way it is set the importer **reports** what it
dropped - how many regions, how many pixels, and the largest one's rectangle - because that is
what turns a heuristic about intent into something the artist can see and overrule.

### 19.5 Mean alpha is the discriminator the corpus does support

Ask for the **mean** alpha over a region's `alpha > 0` pixels - the region's average coverage -
and the two populations separate, on every file measured (pixel-weighted, 15 corpus PSDs plus the
114 MB file):

| file | uniform veils | soft gradients |
|---|---|---|
| `000033.psd` | 1.04 | 2.26 |
| `000061.psd` | 1.08 | 2.12 |
| `006764.psd` | 1.12 | 3.09 |
| `000144.psd` | 1.06 | 3.16 |
| ALL FATHER | 1.00 - 1.12 (all layers) | 2.1 - 4.0 |
| **range over all 16 files** | **1.00 - 1.12** | **2.12 - 4.00** |

The ranges do not overlap. What a cutoff on that quantity would remove, pooled over the 15 corpus
files (`build_and_run.sh --audit` prints it):

| cutoff | uniform veils | soft gradients | in between | soft-gradient share of the removal |
|---|---|---|---|---|
| mean <= 1 | 517 553 px | **0 px** | 0 px | **0.0 %** |
| mean <= 2 | 848 488 px | 251 693 px | 141 965 px | 20.3 % |
| mean <= 3 | 848 635 px | 583 967 px | 143 073 px | 37.1 % |
| mean <= 4 | 848 635 px | 704 304 px | 143 295 px | 41.5 % |

So the operating point is the 8-bit floor itself, and it is sharp: `mean <= 1` removes 61 % of the
veil pixels and **exactly zero** soft-gradient pixels, while `mean <= 2` already drags in 251 693
px of painted soft content.

One of those sentences needs taking apart, because it is close to arithmetic rather than evidence.
Every pixel of a region has `alpha >= 1`, so a region's mean is 1 **iff every one of its pixels is
exactly 1** - a region with any internal variation cannot have mean 1, whatever it is. So "the
floor cut removes no soft-gradient region" is not a discovery about soft gradients; it is the
definition of the cut. The empirical content is elsewhere, and it is class-free:
`mean <= 1` is exactly the predicate **"every pixel of the region is alpha 1"**, which needs no
modal-share cutoffs, and what has to be asked about it is *what those regions actually are*.
Measured (`--audit`, `--min-size 1`):

| file | regions of pure alpha 1 | pixels | size distribution |
|---|---|---|---|
| ALL FATHER | 6 364 | 16 474 | 3 763 single-pixel; 1 143 of 2-3 px; 13 of >= 64 px; largest >= 1 024 px |
| `000033.psd` | 380 959 | 817 900 | long tail up to >= 2 048 px |
| `000061.psd` | 18 863 | 70 564 | long tail up to >= 1 024 px |
| `000057.psd` | 11 093 | 82 962 | - |
| 15-file corpus total | ~434 972 | ~1 032 729 | mostly single pixels, with a tail of areas |

That distribution is the honest picture: the floor cut mostly removes specks (a single alpha-1
pixel is 1/255 of coverage over one pixel - unrepresentable as anything but noise), but it also
removes *areas* - a >= 1 024 px region of pure alpha 1 exists in the test file, and `000033.psd`
has hundreds of thousands of them. Whether a large floor-level area should go is a separate
question from whether a single-pixel speck should, and it is answered by an area or mass ceiling,
not by moving the alpha cut. And the cut is principled rather than fitted: the mean alpha over a
region's `alpha > 0` pixels *is* its average per-pixel contribution, so a region whose mean sits at
1/255 cannot move any output pixel by more than about one level on average. That is a **visibility**
statement - the kind of threshold that can be justified - unlike "was this painted", which no local
measurement can answer.

It also explains 19.2's failure exactly: that rule cut on the **peak** (T = 10), and a soft
gradient island peaks at 3-9 while averaging 2.1-4.0, so a peak cut at 10 deleted it, while a mean
cut at 1 does not. A soft feature is by definition mostly skirt; its mean is therefore low, and a
rule that asks the mean to be high will always delete soft paint. Asking it to be at the floor
asks only whether the region contributes anything above the format's own resolution.

### 19.6 What is still not settled

- A deliberately painted **flat** low-opacity area whose mean sits at the floor is the one thing
  the rule cannot tell from residue - though at 1/255 a "deliberate" 0.4 % flat tint is a strange
  thing to paint, and the format can barely represent it. This is why the rule is reported and
  per-layer rather than silent.
- The mean is a per-region statistic, so a 2-pixel veil and a 5 000-pixel veil are treated alike:
  both are at the floor. They are not equally visible - a 1-level shift over a large area can read
  as a faint veil where two specks cannot - so a *large* floor-level region is the one case worth
  sparing by an **area or mass ceiling**, never by raising the alpha cut (19.5 shows what raising
  it costs).
- The measurements are 16 files. Both metrics separate the populations on all of them, but a
  larger corpus could show a style where they do not; the per-layer setting exists so that such a
  file can be handled without a code change.
- Where the coverage decision should live is unchanged from the earlier analysis: **derived state
  on the resource**, next to the outline distance field, computed once when the layer's image is
  set - so the mesh, the distance field and the importer's report all read one answer, and the
  mesh stops rebuilding the decision on every deform reset.

## 20. Comparing content cutoffs, the only way that works: render off vs on

Sections 16-19 established that the mesh content test decides the mesh's silhouette (which the
artist deforms), that accidents at low alpha must not shape it, and that no comparison against the
PSD's stored composite can answer anything - the composite contains the residue, so removing
anything can only move a render away from it. The comparison that does work is **off vs on**: the
same file rendered twice, with and without the cutoff, everything else identical. The instrument
is `PROBE_CUTOFF=<policy>[,<anchor>]` in the probe:

- the cutoff is applied to the **mesh only**: a layer's pixels are copied, alpha `<= T` is zeroed
  in the copy, and `GridMesh::createFromImage` is called with that copy, while the texture keeps
  the original pixels. That is what production does - the test decides CELL EXISTENCE, and a cell
  that exists draws its own texels, faint ones included. Zeroing the resource itself would delete
  faint pixels inside surviving cells, which production never does.
- it runs between the import and the first render, which is safe: re-meshing a layer that has
  already been rendered trips `LayerNode::transformShape`'s vertex-count assert (the expans holds
  a copy of the mesh), and `core/FFDKeyUpdater` is what the app uses to re-mesh later.
- **verified**: `PROBE_CUTOFF=constant` (T = 10) reproduces the unmodified import **byte
  identically** on `000145.psd` (0 changed pixels of 2 097 152), so the masked re-mesh reproduces
  the production test exactly and any difference between two runs is the cutoff alone.

The three formulas, per layer, and the one parameter:

```
constant:  T = 10
mean:      T = clamp( round(0.05 * mean(alpha : alpha > 2)), 1, 10 )
ratio:     T = clamp( round(10 * (1 - R / anchor)), 1, 10 ),  R = count(2 < a <= 10) / count(a > 10)
```

The ratio's `anchor` (the R at which T collapses to 1) was **fitted, not guessed**: over the 51
layers of the 114 MB file, the anchor that maximises exact agreement with the mean variant is
**0.60** (63 % of layers agree, mean |dT| 1.37). The two variants are deliberately different, so
agreement is not the goal; the anchor exists only so the ratio variant sits on a comparable scale.
Its residual is systematic - the ratio is the less protective variant on soft layers (fog: mean
38.3 -> T=2, R 0.300 -> T=7; glow: mean 46.6 -> T=2, R 0.190 -> T=8) - and 0.375 is run alongside
to show the sensitivity.

First off-vs-on result, 114 MB file, mean variant against the constant:

| | changed px | % of canvas | mean | max | >8 levels |
|---|---|---|---|---|---|
| mean vs constant | 10 288 | 0.22 % | 0.006 | 66 | 221 |

The changes cluster on the right of the canvas (x 2816-3584, y 0-768) and a 1:1 crop shows them as
a **thin trace along an arc edge plus specks** - the faint skirt the constant cuts. The batch
(`tools/psd_composite_probe/cutoff_batch.sh` over `corpus30.txt`, `cutoff_report.py` for the
crops) runs the same comparison over 30 corpus files, four renders each.

## 21. The cutoff comparison over a corpus sample: what it found

Sample: 30 PSDs spread across the corpus by size (`tools/psd_composite_probe/corpus30.txt`, 3.2 GB),
each rendered through the real pipeline with the cutoff off and each variant on (section 20's
mechanism; the "off" run IS the constant T=10, verified byte-identical). Per-layer statistics were
collected in a separate decode-only pass (`threshold_audit`'s `CUT` lines).

**Per layer, 2 386 layers over the 30 files** (T clamped to [1, 10] as the renderer does):

| variant | T distribution over the 2 386 layers | differs from the constant | pixels the statistic removes |
|---|---|---|---|
| constant | 10 on every layer | — | 179 914 849 |
| mean `clamp(0.05*mean(a>2))` | 1:63 2:139 3:146 4:148 5:178 6:199 7:160 8:183 9:153 10:1017 | 57 % | 104 229 890 (58 %) |
| ratio@0.60 | 1:74 2:22 3:31 4:31 5:65 6:123 7:198 8:315 9:535 10:992 | 58 % | 131 505 677 (73 %) |

The mean and ratio variants differ **from each other on 53 % of layers** (1 269 of 2 386), and the
disagreement is systematic: the mean is the conservative variant (58 % of the constant's removals
against the ratio's 73 %), and on soft layers it cuts T = 1-3 where the ratio cuts T = 3-8:

| file | layer | mean(a>2) | R | mean variant | ratio@0.60 |
|---|---|---|---|---|---|
| 000025 | (name in Shift-JIS) 51 | 44.7 | 0.296 | T 2 -> 814 350 px | T 5 -> 1 655 263 px |
| 000026 | 57 | 34.4 | 0.307 | T 2 -> 645 940 px | T 5 -> 1 386 753 px |
| 000035 | 40 | 68.9 | 0.126 | T 3 -> 588 070 px | T 8 -> 1 125 637 px |
| 000052 | red | 39.7 | 0.232 | T 2 -> 519 976 px | T 6 -> 1 096 338 px |

This corrects the single-file impression recorded earlier: on the 114 MB file the two variants
differed on 37 % of layers, over the sample it is 53 %, and the sample is the number that counts.

**On screen, the two variants cut in opposite directions depending on the file:**

- `006757`, ratio@0.60: a yellow-and-green dotted trim is visibly BROKEN with the cutoff off and
  **more complete** with it on - the lower T restored dashes the constant was cutting (11 127 px
  changed, 1 165 of them by more than 8 levels). The cutoff recovering genuine faint paint.
- `000143`, ratio@0.60: a scatter of coloured dashes appears over an otherwise smooth gradient
  (16 000 px changed, 14 319 by more than 8, mean difference 2.03) while ratio@0.375 changes 2 px
  on the same file. Either faint content restored or stray pixel data let back in - the picture is
  what decides, which is why renders are the artifact and statistics are not.

**A structural finding, from joining the two halves:** the per-layer pixel count does not predict
the render. `000080`: the statistic removes 14 190 px and the render changes by **0 px**; `000070`:
21 423 px change identically for all three variants. The mechanism explains it - the cutoff decides
CELL EXISTENCE, and a faint pixel inside a cell that still exists is still drawn, because the
texture is untouched. So "pixels removed" and "ink removed" overstate the effect by a large factor
and the honest unit for any cutoff statistic is **cells lost**, not pixels.

**Operational note.** The batch needs bounded concurrency: the largest files hold ~4.6 GB resident
per probe, and running four at once OOMed this machine and destroyed the batch output (only the
repo scripts and these numbers survive). Reruns must cap concurrency and check free memory before
each file.

### 21.1 The sample, re-picked for memory

The first 30-file sample was chosen by file size, which put seven files in it that cost 4.7-11 GB to
import (the cost is layer AREA, not file size: `000061` is 404 MB but 395 layers and 963 Mpx of
layer area). Rendering those OOMed this machine twice, so the sample was re-picked from the 85 of
104 corpus files that predict under 4 GB (`psd_layer_area.py`, 12 B per layer pixel, calibrated
against the observed 11.66 GB for 963 Mpx):

- kept: the 23 files of the original sample that are under 4 GB;
- replaced: `000061` (11.0 GB), `000026` (7.3), `000052` (6.9), `000059` (6.6), `000038` (5.2),
  `006749` (5.0), `000053` (4.7);
- added, spread across the safe range rather than the smallest: `000047` (4.0), `000029` (2.8),
  `006752` (1.7), `000022` (0.8), `000023` (0.4), `000072` (0.2), `000049` (0.0).

The list is `tools/psd_composite_probe/corpus30_safe.txt`, 30 files, all under 4 GB predicted. The
renders run ONE probe at a time from now on (`cutoff_batch.sh` carries a memory guard that waits for
8 GB of MemAvailable before each file).
