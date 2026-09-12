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

- **M10 - where the sampler reads.** The texture-coordinate convention, measured rather than
  assumed. A source whose value alternates every texel, rendered 1:1 onto a canvas of exactly its
  size, must come back as that pattern: a bilinear filter returns a texel unchanged only when the
  sample sits on its centre, so any misalignment shows up as a collapse of contrast. Measured
  255 of 255 and 0.000 texels off the nearest centre, with the sub-texel sweep matching the
  predicted 255/127/0/127 profile.
- **M11 - 2x minification.** A 2x2-block pattern (whose exact 2x answer is the block itself) and a
  1-texel checkerboard (whose exact 2x answer is uniformly 127.5). Pins both the block the level-1
  read lands in (delta 0.0 vs the box) and the position inside it (contrast 0, where a
  quarter-texel phase error would give 127).
- **M12 - the shape of `glGenerateMipmap`'s filter.** An impulse response, because a checkerboard
  is the one pattern that cannot tell two symmetric filters apart: every aligned 2x2 block of it
  averages to 127.5, and so does every 4x4 block, so a driver filtering with a 4x4 box would pass a
  2x2-box comparison exactly. Measured: one non-zero texel per level at 255/4^l - a plain box -
  and M12b confirms it on a broadband pseudo-random image (max delta 1).
- **M13 - the texture margin parity.** The 2x over-blur isolated to a single variable: the same
  artwork, sampler and zoom, with only the width of the transparent margin around the artwork
  changing. Includes a direct assertion that the shipped margin is even, which is the invariant
  that keeps level 1 aligned with the artwork.

## Figures

Every figure is generated from renders the probe itself produces, never drawn by hand, and there is
one figure per artwork: cramming several artworks into one sheet makes it impossible to judge any of
them, and a 30-pixel crop of "the window that differs most" shows nothing recognisable.

`tools/verify_mipmap/make_artwork_figures.py` writes, for each file, one figure containing both
measured zooms:

- the whole artwork as **three panels at actual resolution** - one render pixel is one image pixel,
  never resampled, so the panel is exactly what a screen pixel shows at that zoom and figures are
  comparable with each other;
- below each panel, **crops at one constant factor, x4 nearest-neighbour**, of the window where the
  old path is furthest from the truth (red box) and the window where the shipped path is furthest
  from it (blue box). The factor is the same in every figure on purpose: a magnification that
  changes per figure makes two figures impossible to compare. Both windows are found by scanning
  the file's own difference maps, not chosen by hand;
- the mean and max error against the supersampled truth for the shipped chain and for level-0
  minification, and which of the two is closer.

They live outside the repository because at actual resolution they are 96 MB for 24 files, which has
no business in git:

```sh
python3 tools/verify_mipmap/make_artwork_figures.py <renderDir> <csv> <outDir> <file...>
ls ~/Downloads/mipmap-review/artwork/          # 24 figures, one per artwork
```

The other scripts are `make_edge_figures.py` (the same idea restricted to alpha edges, where a halo
would be) and the corpus CSV the probe appends to, which is the breadth evidence: 116 files.

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

## The before/after comparison

Superseded by the per-artwork figures above, which show the same comparison on 24 files with the
ground truth beside it instead of on one file with a magnified crop.

## The corpus verdict

The change is judged on the supersampled truth over a sample of the corpus, and the sample is every
fourth file of the 116-file corpus: 24 files (18 PSD, 6 ANIE), each at 2x and 4x minification.
`--eval` writes the numbers; one figure per file shows them.

| | better than level 0 | no change | worse |
| --- | --- | --- | --- |
| 2x minification | 7 | 4 | 13 |
| 4x minification | 8 | 11 | 5 |

Read together with the errors rather than instead of them. At 4x the worst-case error against the
truth roughly halves, which is the point of the change: 000060.psd 255 -> 127, 006751.psd 151 -> 57,
000063.psd 94 -> 49, 000145.psd 133 -> 95, 000169.psd 139 -> 91, seal.anie 96 -> 67. Those are the
pixels where level-0 sampling aliases. At 2x it costs mean error on 13 of 24 files, by 0.1 to 0.25 of
255 - level 1 is a wider kernel than level 0 at exactly 2x when the footprint is not centred on a
texel boundary, so it smooths where level 0 was already the exact box. Every one of those
differences is well under one 8-bit level except 000060.psd, whose canvas is clamped to ss=2 and
whose reference is therefore weak.

So: an upgrade where level-0 sampling aliases, and a small, measured cost at exactly 2x. Anything
stronger than that is not supported by the 24 files, and every figure carries its own numbers so it
can be checked file by file.

## Independent ground truth: supersampling

Everything above compares the render against something derived from the same machinery - an
analytic value for a synthetic pattern, or the naive implementation. Neither answers "is the
minified render CORRECT on real artwork": the analytic cases are constant-colour patterns, and a
fixed-vs-naive differential cannot see an error both implementations share.

`--supersample <dir> <tag> <anie> <ss>` renders the scene at ss x the target resolution, so the
sampler is magnifying and uses no mip level at all, then box-averages ss x ss blocks down. That
converges on the true integral of the scene over each destination pixel, which is what a minified
render is supposed to approximate. Agreement, over real artwork:

| artwork | canvas | zoom | mean | p99 | max | px > 16 |
|---|---|---|---|---|---|---|
| pagedoll.anie | 1200x1500 | 1:1 | 0.259 | 8 | 70 | 6330 |
| pagedoll.anie | | 2x | 0.169 | 5 | 89 | 5178 |
| pagedoll.anie | | 4x | 0.086 | 0 | 101 | 3010 |
| tart.anie | 1000x1000 | 1:1 | 0.710 | 16 | 55 | 8942 |
| tart.anie | | 2x | 0.246 | 9 | 58 | 4520 |
| tart.anie | | 4x | 0.175 | 1 | 97 | 4020 |
| test.anie | 1100x790 | 1:1 | 2.147 | 32 | 60 | 36579 |
| test.anie | | 2x | 0.511 | 17 | 73 | 8806 |
| test.anie | | 4x | 0.214 | 5 | 73 | 3704 |

`--supersample` takes any project type: a `.anie` goes through `ProjectLoader`, a `.psd`/`.ora`/image
through `ImageFileLoader` (with the `.ora` prompt forced off, since a harness has nobody to answer
it). PSD results, same measurement:

| source | canvas | zoom | mean | p99 | max | px > 16 |
|---|---|---|---|---|---|---|
| AUTUMN.psd | 1938x2531 | 1:1 | 0.602 | 20 | 103 | 64152 |
| AUTUMN.psd | | 4x | 0.095 | 0 | 97 | 11054 |
| black pearl cookie.psd | 1924x2500 | 1:1 | 0.096 | 2 | 58 | 4737 |
| black pearl cookie.psd | | 4x | 0.020 | 0 | 77 | 1068 |

The first PSD run aborted with an XC_ASSERT deep
inside `gl::Texture::create`, which read as a mipmapping defect: a 1938x2531 canvas at ss=8 wants a
15504x20248 target, past the 16384 device limit. `--supersample` now clamps ss to what the device
will allocate and SAYS SO, rather than failing or silently under-sampling.

The two renders are visually indistinguishable in the per-artwork figures; the residual against the
truth is edge-localised and shrinks as minification increases. The residual is not zero and should not be: at finite ss the
supersample is itself an approximation (the level-0 lookup is bilinear, not an exact area
integral), and a mip chain is a box filter of the TEXTURE rather than of the rendered scene, so
one-pixel-wide strokes are where the two legitimately disagree most. `test.anie` at 1:1 is the
worst case precisely because it is a line drawing.

A first attempt at this measured a mean difference of 29.7 at 1:1 and looked like a serious defect.
It was the camera: `CameraInfo` maps world units to screen pixels by `mScale` alone and anchors
content on `centerOffset() = imageSize/2`, so passing the supersampled size as `imageSize` shrank
the artwork into a corner at 1/ss scale. The fix is to keep `imageSize` at the canvas, offset
`leftTopPos` to the centre of the larger frame, and scale by `zoom * ss`. The tell was that the two
bounding boxes disagreed; after the fix they match exactly.

## Cost, measured against a control

Same workload (a full `--figures` run on pagedoll.anie) built with the chain and with mip
generation switched off, Release, two runs each:

| build | wall | peak RSS |
|---|---|---|
| mipmapped (shipped) | 4.10 s / 4.20 s | 181 MB / 183 MB |
| no mip chain (control) | 6.28 s / 6.23 s | 181 MB / 181 MB |

The chain is FASTER for minified rendering, not slower - sampling a minified layer from a large
level 0 thrashes the texture cache, while the small mip levels stay resident. Peak RSS does not
isolate the chain's memory because the harness's own supersampled framebuffers dominate it; the
analytic cost of a chain is +1/3 of the base texture bytes (a geometric series summing to 4/3), so
about +2.4 MB per 1200x1500 RGBA layer, ~+46 MB across that project's 19 layers. That figure is
analytic, not measured.

## ASan

The whole probe (98 checks) runs clean under AddressSanitizer against an ASan build of the
libraries (`-DAE_ENABLE_ASAN=ON` in `build-asan/`), exit 0, no reports:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 QT_QPA_PLATFORM=offscreen \
    ./build-asan/tests/Debug/verify_mipmap
```

That covers the new CPU premultiply loop in `ImageKey::resetTextureCache`, which is the only new
code here that indexes a heap buffer.

## Deformed meshes: measured, not assumed

This was the one place the change could plausibly make things worse. UVs interpolate linearly
within a triangle, so the derivative the sampler sees is constant per triangle and trilinear
filtering picks one blend per triangle - if adjacent triangles land on different levels, the
boundary between them is a blurriness step, and before mipmapping every layer sampled level 0
where such steps cannot exist.

`--lodscan <anie> <frame>` computes the same rho GL derives, per triangle, from the real deformed
mesh the render just used (`LayerNode::currentMesh()`, a read-only accessor added for this), and
reports the spread across each mesh and the largest delta across edge-adjacent triangle pairs.

The computation is validated against theory: a 1:1 layer rendered at camera scale 0.25 reports
exactly **2.000** for every triangle, which is `log2(4)`. A constant reported by a broken
computation would look identical to a constant that is correct, which is why the absolute values
are printed rather than only the spread.

Across **36 real projects** (about 2500 meshes) at frame 12:

- Nearly all meshes: spread 0.00, adjacent delta 0.00.
- `ballerina.anie` 0.02, `ek61ul.anie` 0.01, `qitqmd.anie` 0.01.
- `F001.anie` `Copy of Paint Layer 964`: 714 triangles, LOD **0.000..2.001**, mean 1.980 - a
  handful of triangles at level 0 inside a level-2 region, i.e. the seam scenario really occurs.

Rendering that layer at 4x minification and comparing against the supersampled reference shows
agreement in line with every other project (mean 0.089, p99 0) and, in the worst 220-pixel window
**no triangle-shaped discontinuity**: the residual follows the artwork's
thin strokes, not the mesh topology. The affected triangles are a ~1% minority and contribute
nothing visible.

Honest limits of this measurement: adjacency is detected by shared vertex index, so a seam across a
duplicated-vertex block boundary would not be counted - "adjacent delta 0.00" is weaker evidence
than the rendered comparison. One project, one frame, one window was examined visually. So the
conclusion is that no seam was found in the cases that exist here, not that seams are impossible.

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
