# verify_mipmap — mipmap verification probe

Verifies that layer artwork textures are mipmapped **correctly**: that the generated mip
chain keeps the silhouette's own color instead of dragging it toward the color stored under
alpha 0, which is the classic mipmap dark halo (and its mirror image, the light halo).

## Run

Registered as a CMake target in `tests/CMakeLists.txt`, so it is part of the standard suite:

```sh
./scripts/run_suites.sh Debug                                  # includes verify_mipmap
QT_QPA_PLATFORM=offscreen ./build/tests/Debug/verify_mipmap    # on its own
```

Built by CMake rather than hand-rolled in `tools/` on purpose: `tools/verify_blur` rotted into
not compiling at all (it still references `Project::Attribute::setMaxFrame`,
`Exporter::CommonParam::frame` and `TimeInfo::frameMax`, all removed since) precisely because
nothing kept building it. A registered target cannot drift out of the build.

Offscreen GL is enough here. Exit code: 0 = all pass, 1 = test failures, 2 = GL bootstrap
failure. GL objects are freed by `clearRenderFixtures()` while the context is still current — a
static destructor runs after the context is gone and crashes in `glDeleteFramebuffers`, which
this harness originally did.

## Suites

- **M1 — mip chain generation vs an analytic CPU reference.** Two patterns (a bright shape on
  transparent black, and a dark shape on transparent white — the two halo directions), each as
  a checkerboard and as an inset square. For every case the probe computes, independently on
  the CPU, what averaging *straight* data yields and what averaging *premultiplied* data
  yields, then asserts the GPU chain is the premultiplied one and that the composited result
  over an opaque black backdrop equals `source_color * coverage`.
- **M3 — the production RENDER path.** Builds a real `core::ObjectTree` (a folder holding one
  layer with a real `ImageKey`) and renders it offscreen through the same wiring
  `MainDisplayWidget::paintGL` uses, over an opaque black backdrop, with the layer at half scale
  so the sampler reads mip level 1. Asserts the rendered silhouette equals `source * coverage`
  and is *not* the haloed value, and that it matches the same content prefiltered by hand.
- **M4 — every blend mode.** The same equivalence check for all 28 `img::BlendMode` values over a
  mid-grey backdrop, because the fix lives in the shader's handling of the sampled texel and each
  mode takes a different branch over it. The mid-grey backdrop is required: against opaque black
  most blend functions collapse to the same answer (4 distinct results of 28, which makes the
  sweep nearly vacuous); mid-grey gives 16.
- **M5 — feature paths.** The M4 equivalence check applied to the paths a layer can be drawn
  through: an HSV adjust on the layer, a folder composite (which renders the subtree into a
  `FilterFrame` slot and blends that slot against the scene), the two combined, one non-Normal
  layer blend mode, the grid/mesh overlay, clipping (a clippee/clipper pair, which runs the
  `IS_CLIPPEE` shader variant), and a layer blur (which routes the artwork through a
  `FilterFrame` composite slot and the downsample ladder). Each path must also provably have
  changed the result versus the plain baseline, or the equivalence assertion would be vacuous.
  The grid overlay is covered by inspection as well: `GridDrawingFrag` outputs a constant
  `uColor` and samples no texture, so mipmapping cannot affect it - the suite just pins that the
  path still runs. Clipping needs more than "the render changed": the clipper covers only the
  right half of the clippee, and the suite asserts the clippee's own color is ABSENT left of that
  boundary and PRESENT right of it. Without that, a clipper that drew but clipped nothing would
  leave the `IS_CLIPPEE` variant unexercised while equivalence still passed. Measured: 128
  (backdrop) outside against 227 inside - the exact predicted `0.5*200 + 0.5*255`.

The blur path needs an ABSOLUTE expectation, not only equivalence: blurring a uniform region
  leaves its value where it was, and that value is analytic (`coverage*source +
  (1-coverage)*backdrop` = 164 here). Equivalence alone passes under the `PREMULTIPLIED_INPUT`
  mutation, because both sides of it run through the same shader - the mutation takes the blur
  path to 114 instead of 164 and only the absolute assertion catches it.
- **M6 — every mip level.** All the tests above minify by exactly 2x, so they only ever exercise
  level 1. Export renders at its own resolution and the canvas can be zoomed, both of which select
  deeper levels, so M6 sweeps 2x, 4x and 8x and asserts the same analytic interior value at each.
  The source is chosen to make that value LEVEL-INDEPENDENT - a checkerboard of constant RGB
  averages to constant RGB at alpha = coverage at every level - so the assertion has power against
  the wrong level being selected, not just against a wrong chain.
- **M7 — is the mip level constant across the mesh?** This is the seam question. UVs interpolate
  linearly within a triangle, so the sampler's derivative is constant per triangle and trilinear
  filtering picks one blend per triangle - if adjacent triangles land on different levels, the
  boundary between them is a blurriness step. The detector uses a level-SENSITIVE source (an opaque
  white checkerboard on transparent black): level 0 is maximum contrast, every level at or above 1 is
  uniform 50% coverage, so any region sampling level 0 shows up as non-zero local contrast. Scale 1.0
  runs as a POSITIVE CONTROL - there LOD is 0 by construction and contrast MUST be high, which is what
  proves the detector can see level 0 at all. Measured: 127.5 (the maximum possible) at LOD 0, and
  exactly 0.00 with zero spatial spread at LOD 1, 2 and 3.
- **M8 — the configuration the exporter renders in.** NOT the exporter itself: it covers the two
  things `ctrl::Exporter` changes about the render call that matter for mipmapping. The camera - the
  exporter sets it from the project's image size to the OUTPUT size, so an export at a size other
  than the canvas minifies differently from the interactive view and lands on a different level -
  and `aUseWorkingCache`, which the exporter passes as `true` while the canvas passes `false`. The
  analytic interior value must hold at each camera scale (levels 1, 2, 3), and the two cache modes
  must render byte-identically (delta 0).
- **M2 — the production `core::ImageKey` texture.** Builds a real `img::ResourceNode` the way
  the loaders do, lets `ImageKey` construct its texture, and asserts the level count, the
  min/mag/`GL_TEXTURE_MAX_LEVEL`/wrap parameters, that level 1 is the premultiplied chain, and
  that the halo assertion holds on the shipped texture.

## Figures

| file | what it shows |
|---|---|
| `fig1_reference.png` | the project at 1:1, the healthy baseline |
| `fig2a_fixed_8x.png`, `fig2b_naive_8x.png` | the same frame at 8x minification, shipped vs naive |
| `fig3_diff_x8.png` | `|fixed - naive| x8` - the halo as an outline tracing every alpha edge |
| `fig4_halo_crop.png` | worst region magnified 7x, with the amplified difference beside it |
| `fig5_scale_ladder.png` | one region at 1:1, 2x, 4x, 8x - the filter behaviour |
| `fig6a_modes.png`, `fig6b_modes.png` | six blend modes on the same layer |
| `fig7_multiply_halo.png` | Multiply, shipped vs naive - the halo is not Normal-only |
| `fig8_export_vs_canvas.png` | canvas render vs the exported PNG (maxdiff 1) |
| `fig9_paths_sheet.png` | render paths: plain, mesh overlay, folder Multiply/Screen, blur, HSV, clipping |


`figures/` holds renders of real artwork (`~/Downloads/ANIE/pagedoll.anie`, 1200x1500, 19 layers)
through the production pipeline, over an opaque mid-grey backdrop so both halo directions read.
Regenerate with the probe's figure mode:

```sh
QT_QPA_PLATFORM=offscreen ./build/tests/Debug/verify_mipmap \
    --figures <outdir> <tag> ~/Downloads/ANIE/pagedoll.anie
```

The "naive" set is produced by running the same command with two mutations applied - the
premultiply removed from `ImageKey` and `PREMULTIPLIED_INPUT` set back to `0` - so the comparison
is the shipped code against the straight-alpha implementation it replaces, not against a
hand-drawn mock-up.

`figures/fig9_paths_sheet.png` is the render-path contact sheet: the same project rendered
normally, with the mesh overlay, through a composite folder (Multiply and Screen), with a layer
blur, with an HSV adjust on every layer, and clipped to a circular clipper. Each panel was checked
to differ from the baseline before being used as evidence.

Two traps found while building that sheet, both recorded because they silently produce a figure
that looks fine and proves nothing:

- Keys pushed on the tree's TOP node have no visible effect (measured maxdiff 0). The panel has to
  be produced by varying something the render actually reads.
- Pushing a key onto a tree that has ALREADY been rendered also has no visible effect (maxdiff 0
  again), because the time cache is not invalidated when a timeline is poked directly instead of
  going through the command stack. Every key-based figure therefore gets its own fresh project
  load. Applying the blur to all 19 layers did not help; loading fresh did.

Measured on this artwork, fixed against naive: max channel difference 53/255 at 1:1 and 51/255 at
8x minification, over roughly 500 edge pixels. The mean difference is tiny (0.014) because a halo
is a thin fringe on alpha edges, which is why the figures magnify and amplify rather than show
plain full frames.

## Why the ground truth is analytic here

There is no third-party author to diff against: Krita and GIMP (which author the
`tests/golden` PSD/ORA goldens) do not do GL mip selection, so they cannot say what mip level
1 of a given input should contain. The content of a mip level is instead *defined* — it is the
box average of the level above — so the probe computes it directly, in both representations,
and compares. The composited consequence is likewise closed-form: over an opaque black
backdrop a correct chain shows `source * coverage`, and a chain that keeps straight data shows
`source * coverage * coverage`.

## The test must be able to fail

A mipmap probe that only asserts "the GPU agrees with the premultiplied reference" passes just
as happily when mipmaps are never generated, when the requested policy is ignored, or when a
filter is quietly wrong. Every case therefore also asserts that the two references genuinely
differ, that the chain has more than one level, and that level 1 is not a copy of level 0. The
straight-alpha chain is rendered as a **positive control** and must *exhibit* the halo.

Both halves were mutation-tested against the product, not just reasoned about:

| mutation | result |
|---|---|
| `ImageKey`: premultiply deleted, mipmaps kept | M2 fails: `level 1 must match the premultiplied reference (delta 128)`, `no halo (worst deviation 128.0)` |
| `ImageKey`: mipmaps disabled, premultiply kept | M2 fails: `7 levels expected, got 1`, `GL_TEXTURE_MAX_LEVEL must be 6 (got 0)` |
| `ShaderHolder`: `PREMULTIPLIED_INPUT` put back to `0` | **M3 fails: the minified layer renders `red=50.0` instead of `100.0`** — the halo reproduced in a real render (`source * coverage^2 = 200 * 0.25`); M5's blur path also fails, `114.0` against the analytic `164.0` |
| none (shipped code) | 91 checks, 0 failures |

The `ShaderHolder` row is the important one: it proves M3 verifies the *consumption* of the
texture and not only its construction. M1/M2 cannot see a wrong `PREMULTIPLIED_INPUT`, because
they never run the shader.

## A trap worth keeping in mind

When the transparent region stores `RGB = 0` — which is what most writers emit — premultiplied
and straight averaging produce **numerically identical chains**: premultiplying an opaque texel
is the identity, and the transparent texel contributes 0 either way. A probe that only tests
"bright shape on transparent black" is therefore **blind to a missing premultiply**, and the
first version of M2 was exactly that: it stayed green when the premultiply was deliberately
deleted. The discriminating pattern is a bright region under `alpha = 0`, where the two chains
differ by 128 in the color channels at level 1. Keep both patterns.

`maxAnyChannelDelta` exists for the same reason: for a premultiplied chain of that pattern the
discriminating channel is **alpha** alone (both levels premultiply to `rgb = 0` while coverage
halves), so a color-only comparison reports "level 1 is identical to level 0".

## Known limitation: deformed meshes

M7 pins the UNDEFORMED case, and that is as far as measurement currently reaches. On a deformed
mesh the LOD is constant per triangle and can differ between adjacent triangles, which would show
as a blurriness seam, and before mipmapping every layer sampled level 0 where such seams cannot
exist. Measuring it needs the deformed vertex positions per triangle, and `LayerNode::mCurrentMesh`
is private with no accessor, so a test cannot reach them without changing production code. The
risk is bounded rather than eliminated: a layer drawn at or above 1:1 samples no mip level at all,
and a visible seam needs adjacent triangles to differ by roughly 2x in local compression. If it
proves visible, the standard fix for a 2D tool is one LOD per draw rather than per-fragment
derivatives.

## Still to cover

Verified so far: mip chain generation, the production `ImageKey` texture and its parameters, the
rendered composite for a normal layer, all 28 blend modes, the layer HSV branch, the folder
composite path, and the grid/mesh overlay.

Not yet verified: the export path END TO END. M6 and M8 cover what the exporter changes about the
render that matters for mipmapping - its output-resolution camera (hence a different mip level)
and its `aUseWorkingCache=true` render mode - but the exporter itself is not driven, so its own
framebuffer readback and PNG write are untested here. Driving it is tractable: `core::Project`
takes a filename and its attribute setters (`setImageSize`/`setFps`/`setFrameRange`) allow a
project to be populated programmatically, and `tests/media_exporter_e2e.cpp` shows the
`CommonParam`/`ImageParam`/`execute` shape. It was left undone for budget, not because anything
blocked it.

Also outstanding: an ASan run over the new CPU premultiply pass, the glyph-cache decision, and
measuring the load-time/memory cost of building the chain.

A reminder from writing M6: a fixed interior crop silently becomes wrong when the layer's scale
changes. At 4x minification the quad is smaller than the crop, so the backdrop averaged in and
the "correct" value read 141.6 instead of 164.0 - a test bug that looks exactly like a product
bug. The crop now follows the quad.

Known open risk, partly measured. `LayerMesh` keeps static `texCoords` while `positions` are
deformed (bones/FFD/SRT), so UV derivatives - and therefore LOD - vary from triangle to triangle,
and adjacent triangles can select different mip levels. Before mipmapping every layer sampled
level 0, where such steps cannot exist, so this is the one place the change could plausibly make
the product worse.

M7 pins the UNDEFORMED case: on a uniformly scaled layer the level is constant across the whole
mesh (zero spatial spread at LOD 1, 2 and 3), so neither the grid topology nor the mesh creator's
edge-vertex reduction ("burr reduction") introduces LOD variation. It does NOT measure a deformed
mesh. Constructing one needs the FFD machinery - `GridMesh` exposes no mutable positions, and
`HeightMap::writeHeightMap` is dead in this branch, while the live FFD path runs through the
editor's target/workspace (`ctrl/ffd/ffd_BrushMode`, `FFDKeyUpdater`) - so a probe cannot yet
deform a mesh cheaply.

Worth noting the risk window is narrower than it first looks: a layer drawn at or above 1:1 never
samples a mip level at all, so only MINIFIED layers with a deformed mesh are exposed, and a visible
seam needs adjacent triangles to differ by roughly a factor of two in local compression. If it does
prove visible, the standard fix for a 2D tool is one LOD for the whole draw rather than per-fragment
derivatives.

Also unmeasured: anisotropic minification (a non-uniform layer scale compresses the texture on
one axis, and trilinear LOD follows the largest derivative, so the other axis is over-blurred).
No anisotropic filtering is enabled anywhere in the pipeline.

M4's and M5's equivalence reference cannot catch a *global* shader error, because both sides of
the comparison run through the same shader. Only M3's analytic assertion has power against that
(which the `PREMULTIPLIED_INPUT` mutation above confirms); the sweeps are there for mode- and
path-specific differences, where they do have power because a checkerboard and a uniform image
respond differently to anything that mishandles coverage.

A trap worth remembering, hit while writing M5: `TimeLine::grabDefaultKey` files a key under
`mDefaultKeys`, but `LayerNode` decides whether to run its HSV branch with
`mTimeLine.isEmpty(TimeKeyType_HSV)`, which consults the key map. A *default* HSV key is
therefore invisible to that check and silently does nothing - the key has to be pushed at a
frame via `timeLine->createPusher(...)`. The non-vacuity assertion in M5 is what caught it.
