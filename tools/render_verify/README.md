# render_verify — directional blur verification harness

Verifies the directional blur feature (`BlurKey` blurX/blurY/angle, `core/WorldBlurMath.h`,
`core/FilterFrame` separable Gaussian + downsample ladder) by rendering real scenes through
the production pipeline and comparing against an independent CPU reference, under ASan.

## Run

```sh
tools/render_verify/build_and_run.sh        # rebuild libs + harness, then run
tools/render_verify/build_and_run.sh --no-build
```

Must run from the repo root (the script cds there). Shaders come from the embedded Qt
resources, so the script compiles `assets/assets.qrc` into the harness binary itself (the GUI
target normally supplies it). `AE_BUILD_DIR` selects the build tree to link against (default
`build-asan`; point it at `build` when no ASan tree exists — the suite then runs without
sanitizer coverage). Tries `QT_QPA_PLATFORM=offscreen` first, falls back to X11 (`DISPLAY=:0`)
when offscreen GL is unavailable. Exit code: 0 = all pass, 1 = test failures, 2 = GL bootstrap
failure (triggers the X11 fallback).

## Suites

- **S1 math** — `worldBlurEllipse` over real node chains (via the production
  `TimeCacheAccessor`) vs an independent double-precision Jacobi SVD: directed edge cases
  (zero/negative scales, degenerate radii, angle wrap, nesting) and 800 randomized chains,
  plus invariants (orthonormality, `|det M| == s1*s2`) and a brute-force max-stretch check.
- **S2 ladder** — `FilterFrame::blurLadderLevel` vs an independent reference over a
  boundary grid (radii around 16*2^k, tiny/odd buffer sizes), with level invariants.
- **S3 blending** — `TimeKeyBlender` blur segments: radii lerp, accumulated angle (multi-turn spins keep their turns, like the rotate key)
  easing, exact-keyframe/after-last/no-key cases.
- **S4 render** — full production renders vs the CPU reference blur (premultiplied,
  GL bilinear, CLAMP_TO_EDGE, ladder replica): plain/transformed layers, the edge case of
  a **layer inside a folder with transforms on both**, blur on transformed folders, three
  levels of nesting, ladder radii, camera zoom, **camera rotation and x-flip**, odd/tiny
  canvases, zero minor axis.
- **S5 orientation** — GPU ground truth: second-moment angle of a blurred dot (including
  folder+layer rotations and **camera rotation/flip** — the measured screen angle must
  equal the world angle mapped by `R(camRot) * diag(flip ? -1 : 1, 1)`), the
  `(rx,ry,a) == (ry,rx,a+90)` identity, isotropic `amount` vs directional equal-radii
  byte identity, layer-blur vs folder-blur byte identity.
- **S6 gating** — blended radius at/below the epsilon (0.001) must render byte-identical
  to no key (layer and folder); small radii are active with an identity-like kernel
  (sigma floor lowered so interpolations ramp in smoothly, no step at the gate); keys
  activate at their frame.
- **S7 interactions** — blur crossed with other features: a clippee pair inside a blurred
  folder and a blurred layer that is itself clipped (clipping propagates into the
  composite), **folder blur nested inside folder blur** (inner ladder/direct sequencing),
  Multiply/Screen blend modes over a captured background (`DestinationTexturizer` +
  `blendColor` + the `SRC_ALPHA` framebuffer blend replicated), and HSV ordering: a
  layer's HSV is baked before the blur (`ref = blur(plain)`), a folder's HSV is applied
  at presentation after the blur (`ref = hsv(blur(plain))`). NOTE: the clip cells push
  the **clippee first, clipper second** — children are walked back-to-front
  (`collectRenderUnits` iterates `rbegin`→`rend`), so the first-pushed sibling renders
  on top, and `isClipper` requires the clippee to be the clipper's `prevSib()`. Getting
  this backwards silently skips the clippee (no node is a clipper), which made the
  folder case pass vacuously until the order was fixed.
- **S8 serialization** — blur keys through the binary project format:
  `TimeLine::serialize`/`deserialize` round-trip at the current version (radii, angle,
  directional flag, easing per key, alongside another key type), plus the flat-layout
  check (same bytes read back identically under the pre-blur 0.8 version label; blur
  shipped as a single feature with one bump, 8 -> 9, so no historical layout exists).
  The clipboard JSON path is file-local
  (`gui/TimeLineEditorWidget.cpp` copy, `ctrl/TimeLineEditor.cpp` paste) and was
  verified by inspection: both sides use Amount/BlurX/BlurY/Angle; paste falls back to
  Amount when BlurX is absent (the directional flag is inferred, not serialized).
- **S9 export** — the real `ctrl::Exporter` image-sequence path (no ffmpeg) on a
  `core::Project` with a blurred clippee, a blurred transformed folder and a
  frame-varying blur: exported PNGs must equal the interactive-path render at the same
  frames (PNG is straight-alpha, the scene is premultiplied — premultiply back before
  comparing), and the two frames must differ across the key change.
- **S10 performance** — informational snapshot: filtered vs unfiltered ms/frame at
  1920x1080 (direct-path and ladder radii), 10 timed renders each including one
  readback.

(The suites also cover PSD/ORA import (S11/S13), folder blend modes (S12), golden images
against reference exports (S14) and the shader-variant matrix (S15); they are documented at
their definitions in `render_verify.cpp`.)

- **S18 layer effects** — Photoshop's `lfx2` block: the descriptor reader
  (`img/PSDDescriptor`, pinned against a hand-written block including the two-word
  `DescriptorBlock2` header, truncated input and an unknown OSType), the parsed model
  (`core/LayerEffects`: what is drawn vs what is only recognised — an enabled effect with
  nothing this build draws, such as a bevel or a blur-less chrome, must mark the result as
  not fully faithful), the gradient overlay's placement against an independently written
  reference at two angles, the carry-through that keeps effects from changing coverage (a
  fully transparent gradient is byte-identical to no effect), the import leg (a PSD written
  with an `lfx2` block, a chrome silhouette and an `iOpa` fill opacity) and the project
  round-trip of the gradient, the silhouette's geometry/tint/blend mode and the fill opacity.
- **S16 Blend If, GPU vs CPU reference** — including one case that deliberately does *not*
  compare against the CPU reference, because that reference shares the semantics and agreeing
  with it would prove nothing: a backdrop of exactly luma 128 under a **crossed** destination
  range (`blackLo 64, blackHi 192, whiteLo 96, whiteHi 224`) with an opaque `(200,40,40)` top
  layer must land on the hand-computed `(164,84,84)` — the black edge is 0.5, the white edge
  0.75, and Photoshop's weight is `min(0.5, 0.75)`. The product reading would give
  `(155,95,95)`. The two rules coincide on every uncrossed range, which is why the earlier
  cases could not see the difference; see BLEND_IF_ASSESSMENT.md §14.
- **S22 layer mesh coverage** — the deform mesh must not change what a layer draws. Every layer
  is drawn through a triangular mesh generated from its own alpha (`img::GridMeshCreator`), so
  whatever that mesh calls empty is never rasterized: the pixels it calls content have to be
  exactly the layer's support. The suite draws one layer twice — through the generated mesh and
  through an exact bounding quad (a cell size at or above the bitmap size makes
  `GridMesh::createFromImage` build a quad) — and requires the two renders to be **byte
  identical**, then requires both to reproduce the layer's own bitmap drawn 1:1 at an integer
  offset. Its fixture contains an alpha-6 haze block and a ramp down to alpha 1, i.e. content the
  old `alpha > 10` content test called empty, so it fails on the previous code. (Regenerating a
  mesh is only legal before the layer is first rendered: the cached FFD vertex data in
  `TimeKeyExpans` is a copy of the mesh, and `LayerNode::transformShape` asserts on a mismatch.)
- **S21 resource identity** — the rule a source reload uses to answer "which layer in the
  project is this layer in the file?". It pins the policy in the model
  (`img::ResourceData::matchesIdentity`): a Photoshop `lyid` decides when both sides have one
  (so a rename keeps correspondence and two same-named layers stay apart), the layer name is
  the fallback when it does not (the migration path for a project written before ids), the
  layer/folder kind is always checked, same-named siblings with distinct ids are not duplicates
  while an id-less same-named sibling remains ambiguous, the importer reads `lyid` for layers
  *and* groups, and the ids survive a project round-trip.
- **S20 layer stroke** — Photoshop's Stroke (`FrFX`) and the outline distance field it needs
  (`core/DistanceField.h`): the exact separable Euclidean transform against a brute-force
  reference on four shapes (plus the degenerate all-empty/all-full planes, which must produce
  no field at all), then the band itself — a 32x32 square with a 3 px inside stroke must land
  exactly on the pixels the *analytic* rectangle distance puts in the band, in the stroke's
  colour, and must leave the layer's pixels alone (this passes with a max channel difference of
  0). It also pins the fallbacks (a stroke with no field on the resource is byte-identical to
  no stroke; an outside or pattern stroke is reported, not drawn), the descriptor (size,
  position, paint type, blend mode, gradient stops, angle and scale), and the import plus
  project round-trip: a PSD whose layer carries an `FrFX` block bakes a field that peaks at the
  shape's inradius and survives ProjectSaver -> ProjectLoader byte-identically. The paint type
  is pinned to Photoshop's own value (`PntT` = `SClr` for a solid stroke; the helper used to be
  called with `SCol`, a key no producer writes, which is why a solid stroke reading as neither
  solid nor gradient went unnoticed - restoring `SCol` makes six checks here fail). It also
  pins the reporting: a PSD whose effects are all drawn reports nothing, a PSD carrying a bevel
  reports it by layer and by effect name, and a bevel the file switches *off* is not reported.
- **S19 layer effects (fill opacity and silhouettes)** — Photoshop's FILL opacity (`iOpa`)
  as its own value rather than a factor on the layer opacity, and the blurred-silhouette
  effects (drop shadow, outer glow, chrome): a 0 % fill with no style draws nothing (alpha 0
  everywhere) while the same layer at 100 % does; a 50 % fill halves the coverage without
  moving it; an opaque style survives a 0 % fill byte-identically (the glow-only layer);
  the silhouette lands exactly one `distance` from the shape at the effect's angle
  (centroid measured for 0/90/180 degrees, in GL-readback rows so the direction is pinned);
  an outer glow reproduces the independent Gaussian reference at both a direct and a
  laddered radius; and a `Scrn` silhouette really lightens its backdrop, which a Normal
  present could not.
- **S17 layer mask** — PSD layer masks (`img/ResourceData`'s mask plane + the shader's
  GL_R8 fetch) against the CPU reference, plus the import and persistence legs: coverage
  multiply per blend mode, inverted, disabled (byte-identical to no mask), composition with
  layer opacity and Blend If, the composite/blur path (an all-black mask must erase even a
  blurred layer), a PSD written with a user mask imported with its plane and flags, and the
  mask surviving ProjectSaver -> ProjectLoader byte-identically. It also checks the
  production CPU range weight (`core::blendIfRangeWeight`) against the reference over all 256
  values (both now combine a slider's two edges with `min`; that pair agrees by construction, so
  the rule itself is pinned by S16's hand-computed crossed case, not here), and covers the
  authoring command: baking a layer's Blend If SOURCE range into its
  mask (`core::LayerMaskBake`) must be render-equivalent to leaving the range live, and its
  undo must restore both the ranges and the previous mask pixels. The suite's first case pins
  the mask row order empirically (image row 0 lands at the canvas top) instead of assuming
  it, and the reference builds its mask in the readbacks' row order.
- **S16 blend if** — Photoshop "Blend If" (`core/BlendIf.h`) against the CPU reference,
  on flat canvas-sized identity layers (texel-exact, border band excluded like the golden
  suite): the destination range over a luma ramp, the source range, both multiplied, an
  emptied range drawing nothing, the default-range no-op gate, the weight composed with
  layer opacity and alpha, a folder present, and the composite/blur present path (an active
  range must weight the presented composite; the blurred content itself is S4's subject).
  The reference is `ref::blendPresent(..., &ranges)`: a Rec.601 trapezoid over the 8-bit
  channel values, applied as an alpha scale.

## Layout

- `render_verify.cpp` — main + all suites.
- `scene.h/.cpp` — offscreen GL fixture (mirrors `MainDisplayWidget::paintGL`) and tree
  builders (transforms via default SRT keys, blur keys via `TimeLine::createPusher`).
- `cpu_ref.h` — the independent CPU reference (Gaussian pass, resample/ladder replicas,
  Jacobi SVD, second-moment measurement, HSV adjust and blend-present replicas).
  Written from the documented semantics, not from the production code, so production
  bugs surface as mismatches.

Coordinates: readbacks are GL-ordered (row 0 = canvas bottom). The composite slot holds
the scene as rendered through the camera — view linear part `R(rotate) * diag(flip ? -1
: 1, 1)` — y-flipped in texture rows. So a world-space direction `(dx, dy)` maps to the
slot as `flipY(R(rot) * (flip ? -dx : dx, dy))`, which reduces to `(dx, -dy)` for an
untransformed camera. The S4/S7 replicas apply this mapping independently of production
(via `scene::screenDirFromWorld`), so a sign/convention bug in production fails S4/S5.
