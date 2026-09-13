# verify_mipmap — mipmap verification probe

Verifies that layer artwork textures are mipmapped **correctly**: that the generated mip
chain keeps the silhouette's own color instead of dragging it toward the color stored under
alpha 0, which is the classic mipmap dark halo (and its mirror image, the light halo).

## Run

Note on the bias: the fixtures in the suites pin `GL_TEXTURE_LOD_BIAS` to 0, because what they assert
is which level a given footprint reads and what kernel that level applies - "this is level l" would
otherwise be reading level l-0.5. The shipped -0.5 is a policy applied on top, measured by M17, by
`--bias`, `--bias1` and the corpus run.

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
  artwork, sampler and zoom, with only the width of a transparent margin padded around the artwork
  inside its texture changing. A margin of 1 (what the code shipped before this work) gives worst
  delta 112 of 255; no padding at all, which is what ships now, gives 0.5.
- **M14 - the margin at every level, not just level 1.** Level *l* is a box of 2^*l* base texels
  whose grid is anchored to multiples of 2^*l* in the texture, so the artwork's origin has to be a
  multiple of 2^*l* - being even is not enough. M14 measures levels 0 to 7 (1x to 128x minification)
  at margins 0, 8, 16 and 32 against the exact 2^*l* x 2^*l* box of the artwork. Level 0 is the
  control that validates the artwork-to-screen mapping independently of any mip behaviour (delta
  0.0 at every margin). Result: a margin of 8 was aligned through level 3 and **misaligned from
  level 4 up**, worst delta 111.0 and mean 6.99 of 255 on random artwork at 16x, 51.0/7.49 at 64x;
  16 was wrong from level 5, 32 from level 6. No padding is exact at every level (worst 1.3, mean
  0.66), which is why `img::Util::prepareForSampling` pads by nothing: the transparent neighbour a
  bilinear read needs at the artwork's edge comes from the sampler's `GL_CLAMP_TO_BORDER` border
  colour, which the hardware applies to every mip level.
- **M15 - level selection across a continuous zoom sweep.** M6/M11 pin the integer zooms, where the
  level is unambiguous; the interesting case is between them, where trilinear blends two levels. The
  instrument is a hard edge: a pixel's value is the kernel convolved with the step, sampled at the
  pixel centre, so the profile's steepest step measures the kernel width - and the kernel width IS
  2^lambda base texels. The measured step is compared against the same statistic computed on the
  ideal profile for lambda = -log2(zoom), a blend of the two neighbouring boxes, which removes the
  statistic's own bias between levels instead of hiding it in a tolerance. 15 zooms from 0.9 to
  0.0625, i.e. lambda 0.152 to 4.000: measured and predicted agree within 1 of 255 at every one.
- **`--decompose` - what the remaining error is made of.** See the section below the corpus verdict:
  one real layer at 16x, four quantities (render, truth, the ideal box chain, the tent integral) and
  the inequality that separates the filter from the implementation.
- **M17 - the shipped LOD bias.** Reads the bias off the live layer texture and compares it with
  `core::ImageKey::layerLodBias()`, then checks that a negative bias makes the render sharper at a
  fractional phase. See the section below for what it can and cannot assert.
- **M16 - deep-level coverage of the artwork's last rows and columns.** Level l has
  max(1, floor(size/2^l)) texels, so unless the texture size is a multiple of 2^l the chain drops the
  last (size mod 2^l) base texels of that axis and the renderer shows that strip of the artwork as
  the neighbouring block. Checked as arithmetic on the size the app uploads, and once on the GPU by
  reading the chain back for a 513-texel artwork (odd, and 1 modulo every 2^l - the worst case for
  the floor rule) and comparing level l against the exact box of the artwork plus its padding: levels
  1..3 match to 2 of 255. Every level 1..7 now covers the artwork whole; before the size was aligned,
  a 500-texel artwork would have lost its last 116 rows and columns at level 7.
- **M18 - the levels past the alignment cap (256x and beyond).** The levels whose grid does not divide
  the side are the ones `glGenerateMipmap` does not fill with the average they promise; M18 measures
  what each one actually weights, by sweeping a step through the side and reading the level's own texel
  out of the driver, one row at a time. It asserts the box for nine size/level combinations including
  the realistic 1920 and 3840 sides, the support spanning the whole footprint, flat content staying
  flat, the padded upload landing on the mass-weighted truth, and that the row weights do not depend on
  the texture's width. See "the grid must not be stretched either" below.
- **M19 - anisotropic minification.** Trilinear picks its level from the larger of the two
  screen-space derivatives, so a non-uniformly scaled layer is over-blurred along its short axis by
  the ratio. M19 measures what an elongated footprint resolves against the exact area integral of
  that footprint (4:1, 8:1 and 16:1, per requested anisotropy), the square-footprint control that
  says the ordinary case is untouched, the level shift the driver's anisotropic path introduces at
  fractional levels, and the cost against the fragment count. See "Anisotropic minification" below.

## Figures

Every figure is generated from renders the probe itself produces, never drawn by hand, and there is
one figure per artwork: cramming several artworks into one sheet makes it impossible to judge any of
them, and a 30-pixel crop of "the window that differs most" shows nothing recognisable.

`tools/verify_mipmap/make_artwork_figures.py` writes, for each file, one figure containing all four
measured zooms (2x, 4x, 8x and 16x):

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

The panel is the artwork as it appears at that zoom, so its size follows the zoom - half the canvas
at 2x, a sixteenth at 16x - which is the point of the deep zooms: at 16x a canvas-sized panel would be
almost all backdrop. The crops stay 128 render pixels at x4 wherever the panel is big enough to hold
them (at 16x the panel is itself smaller than 128 pixels, so the crop is the whole panel at x4); the
magnification factor is the constant one, and it is the same in every figure.

They live outside the repository because at actual resolution they are 133 MB for 29 files, which has
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

The change is judged against the supersampled truth over a sample of the corpus: every fourth file of
the 116-file one, 29 files, at 2x, 4x, 8x and 16x minification. `--eval` writes the numbers.

Each row compares the central 64x64 pixels of the canvas-sized render against a supersampled
reference of exactly that window (`ss` = two samples per base texel, rounded up to odd when a canvas
dimension is odd so that the two pixel grids line up exactly; the `ref_align255` column reports the
residual, and is 0 on all 116 rows). The window reference is cross-checked against the whole-canvas
reference wherever that one still fits the 16384 device limit - `winref_delta` is 0 on 21 rows and 1
on 10, so the two agree.

| minification | mip closer than level 0 | tied | level-0 closer | mean error: level 0 -> shipped | worst pixel: level 0 -> shipped | pixels off > 16 |
| --- | --- | --- | --- | --- | --- | --- |
| 2x | 8 | 6 | 15 | 0.905 -> 1.074 | 20.1 -> 21.9 | 861 -> 1122 |
| 4x | 16 | 1 | 12 | 2.543 -> 2.095 | 54.5 -> 36.7 | 4911 -> 3880 |
| 8x | 27 | 0 | 2 | 4.969 -> 3.089 | 107.1 -> 57.0 | 10444 -> 6290 |
| 16x | 29 | 0 | 0 | 7.902 -> 4.296 | 142.5 -> 63.5 | 16332 -> 8966 |

So: an upgrade wherever level-0 sampling aliases (everything from 8x, and 27 of 29 files at 8x),
a wash to slightly better at 4x, and a small measured cost at exactly 2x - level 1 is a wider kernel
than level 0 there when the footprint is not centred on a texel boundary, so it smooths where level 0
was already the exact box. Most of those 2x differences are under one 8-bit level.

### What the two texture-geometry fixes did, measured the same way

Both fixes are in the upload, not in the sampling, and both show up only where the level grids are
wrong. Three configurations of the same probe over the same 29 files against the same truth:

| minification | level 0 | 8-texel ring (before) | no ring | no ring + size-aligned (shipped) |
| --- | --- | --- | --- | --- |
| 2x mean / worst pixel | 0.905 / 20.1 | 1.186 / 25.8 | 1.185 / 26.1 | **1.074 / 21.9** |
| 4x mean / worst pixel | 2.543 / 54.5 | 2.362 / 44.3 | 2.356 / 44.3 | **2.095 / 36.7** |
| 8x mean / worst pixel | 4.969 / 107.1 | 3.382 / 65.6 | 3.383 / 65.5 | **3.089 / 57.0** |
| 16x mean / worst pixel | 7.902 / 142.5 | 4.429 / 70.4 | 4.577 / 75.2 | **4.296 / 63.5** |

Against level 0 the shipped chain is closer on 8 of 29 files at 2x, 16 at 4x, 27 at 8x and 29 at 16x.

Two things are worth reading off that table rather than off a summary. Removing the ring alone was a
wash at 16x - 9 files better, 12 worse - because the size stretch it left behind costs the same
order, so the two defects partly cancelled file by file. Aligning the size as well improves on the
ring at every zoom, 2x included, where the small cost of the change is halved (1.186 -> 1.074): part
of what looked like "level 1 is a wider kernel than level 0 at exactly 2x" was the level-1 grid being
stretched on artwork whose side is odd. What remains at 16x - about 4.3 of 255 mean - is mostly the
difference between a box filter and the tent-weighted integral the truth represents, which no mip
chain reproduces, plus whatever stretch is left beyond the alignment cap.

### The third condition: the grid must not be stretched either (fixed, with a documented cap)

Alignment is not the only condition on these grids. Level *l* has `floor(size / 2^l)` texels and
normalized texture coordinates map [0,1] onto them, so unless the texture size is a multiple of 2^*l*
the level-*l* grid is stretched over the artwork - each of its texels covers slightly more than 2^*l*
base texels, the displacement growing linearly to `size mod 2^l` base texels at the far edge - and the
chain drops the last `size mod 2^l` base texels of that axis entirely, so the artwork's own last rows
and columns are represented by the neighbouring block.

Both are fixed in the upload: `img::Util::mipAlignedTextureSize` pads the texture to the right and
bottom, up to a multiple of the largest power of two that fits in that side (capped at 128), and the
artwork stays at the origin, so alignment is untouched. It costs no geometry: the mesh and the layer's
rect come from the image, while the shader divides texture coordinates by the texture's size, so the
artwork's coordinates land on [0, size/padded] by themselves. Each axis is aligned separately, so a
1-pixel-wide image is not padded out to a cap-sized texture; a 1000-pixel side goes to 1024, a
2049-pixel side to 2112.

M14 measures the result against the whole expected chain rather than a hand-picked box: the render
must equal `boxLevel()` of the texture the app would upload, sampled bilinearly at the footprint
centre. Sizes 512, 504, 500 and 513, levels 2 to 5, with and without a ring: worst delta 1.5 to 2.5 of
255, mean at most 1.24 - 8-bit rounding - and the same at every size, which is the part that used to
fail.

Past level 7 the padding can no longer help, and the level past it is NOT an average of its own
footprint: the GL spec fixes the level sizes, not the filter, and what this driver does with a source
that does not halve is weight only a band of it. Measured with a step swept across the side (M18),
level 8 of a 384-texel side is the average of rows [128, 256) - the middle third, the outer two thirds
carrying no weight at all - and the deepest level of a 1920-texel side weights 7% of it. The deviation
from the grid's own box reaches **118 of 255** at those deepest levels, on a layer under two screen
pixels, and **17 of 255** at the first level past the cap, where the layer can still be 7 px across. It
is not a displacement: a kernel in the wrong place blurs, a truncated one makes the layer the wrong
COLOUR - minified past the cap a 300-texel artwork collapsed to alpha 255 and rgb 200 of 255 where the
mass-weighted truth is 156 and 122, 78 of 255 too bright. (The midpoint of the profile, which is the
quantity "under one screen pixel" is a claim about, was within 0.12 px on every size measured - which
is why measuring displacement alone was not enough.)

Those levels are now rebuilt by `gl::Texture::create` from the deepest level that DOES divide, with the
area weights the level grid implies (texel *j* of level *l* covers normalized [j/T, (j+1)/T] of the
axis), each rebuilt level taken from the exact one rather than chained through the level above it.
Every level of every chain is then the box its grid promises, for any size: M18 asserts it with the
same sweep on nine size/level combinations - 256, 384, 512, 640 and 768 squares, and 1920 and 3840 at
both their first non-dividing level and their deepest - plus flat content staying flat (no halo), the
support spanning the whole footprint, the padded 300-in-384 upload landing on the truth (alpha 155
against 156), and the separability of the row weights from the texture's width. Reverting the rebuild
turns 11 of those checks red with exactly the numbers above, which is how the assertions were shown to
have power.

The rebuild's cost is bounded by the size of that deepest exact level, which the alignment makes
small: a side that is a multiple of 128 has at least seven trailing zero bits, so the readback is at
most 128 texels across for any texture 128 texels or more, and the rebuilt levels are smaller still.
A power-of-two side has nothing to rebuild at all. An unaligned size handed to the same code would
read back the whole texture instead - which is one more reason the layer upload pads.

What is left past the cap is the level grid's own arithmetic: with `floor(size / 2^l)` texels over the
whole side, the deepest level's texel can cover up to twice the 2^*l* base texels its own number
suggests. At 256x on a 384-texel side it covers 384 where the footprint is 256, i.e. 1.5x softer than
the ideal box - on a layer 1.5 px across, and the shipped -0.5 bias blends level 7 (exactly the 256-texel
footprint) into half of it. Bounded, and sub-pixel; it is not the same class of error as dropping two
thirds of the source. The cap itself stays because it bounds the padding's memory - up to 127 wasted
texels an axis - where padding each side up to a power of two would cost up to 4x the texture memory
for a 2049-pixel side.

## What the remaining error IS: the decomposition

The corpus numbers say how far the shipped render is from the supersampled truth. They do not say
what that distance is made of, and "it is the difference between a box filter and the integral the
truth represents" is exactly the kind of claim that is easy to assert and hard to check. `--decompose
<csv> <file>` measures it on a real layer, rendered ALONE at exactly 16x (lambda = 4), by computing
four quantities on the same pixels:

| quantity | what it is |
| --- | --- |
| `render` | the shipped render: the GPU's level-4 read of the texture the app uploads |
| `truth` | the supersampled render of the same scene, box-averaged: the integral |
| `box` | a CPU box pyramid over those exact uploaded bytes, read bilinearly at the footprint centres - the ideal chain |
| `tent` | the integral of the TENT reconstruction of the same artwork over the same footprints |

`--eval` also writes `box4_*` columns: the same scene rendered with level 4 forced (see the corpus
section), which is the ideal box chain for that footprint and the panel that makes "the sampling is
not what is left" visible rather than argued.

`|render - truth| <= |render - box| + |box - tent| + |tent - truth|`, so the three terms say which
part is the filter and which part is the implementation. Measured on six real layers (the largest
layer of six corpus files), channel 0, interior footprints, in 1/255:

| file | layer | render-vs-truth | box-vs-truth | render-vs-box | tent-vs-truth | box-vs-tent |
| --- | --- | --- | --- | --- | --- | --- |
| 000020.psd | 1127x2120 | 3.374 | 3.592 | 0.960 | 0.142 | 3.582 |
| 000042.psd | 750x1500 | 2.279 | 2.285 | 0.984 | 0.254 | 2.268 |
| 000077.psd | 2265x3000 | 2.707 | 2.853 | 0.609 | 0.131 | 2.835 |
| 000145.psd | 1024x2048 | 0.094 | 0.094 | 0.000 | 0.001 | 0.093 |
| AUTUMN.psd | 660x1204 | 3.718 | 3.780 | 0.203 | 0.040 | 3.780 |
| 000063.psd | 2480x3393 | 2.303 | 2.222 | 1.111 | 0.250 | 2.189 |

Three things this shows, none of them by construction:

- **The supersampled truth IS the tent integral**, on real content: `tent-vs-truth` is 0.04 to 0.25
  of 255. The ground truth is not defined as that integral anywhere in the harness; the CPU model of
  the integral is computed from the artwork and agrees with the rendered truth to a quarter of an
  8-bit level.
- **The error is the filter, not the chain**: `render-vs-truth` and `box-vs-truth` and `box-vs-tent`
  are the same number to within their own noise (e.g. 3.374 / 3.592 / 3.582), while `render-vs-box` -
  the part the implementation contributes - is 0.2 to 1.1. The chain is doing exactly what a box
  pyramid does; the box pyramid is what disagrees with the integral.
- **The shipped render IS the ideal box chain at 16x.** `--eval` gained a fourth panel per zoom
  that renders the same scene with level 4 FORCED (`GL_TEXTURE_MIN_LOD = GL_TEXTURE_MAX_LOD = 4`),
  i.e. the box pyramid's ideal output regardless of what the zoom would have picked. At 16x the
  shipped render and that forced render are the same pixels - mean difference 0.000, worst 0 on the
  first file measured, and across the 29-file corpus a mean of 0.09 of 255 (the residual is the
  0.001 trilinear blend with level 5 that `--lodscan` reports) - while at 2x, 4x and 8x forcing
  level 4 is plainly the wrong level (10.5, 9.8 and 7.6 of 255 from the shipped render), which is
  how the forced column is known to do what it says. So the distance from the truth at 16x is not
  the sampling: it is what the box pyramid gives, and the figures show it as a fourth column
  (third and fourth identical at 16x, visibly different below it).
- **What the remaining difference actually is: the phase, not the filter's shape.** The forced-level-4
  column measures 4.264 of 255 from the supersampled truth against the shipped render's 4.296 (corpus
  means at 16x), so the whole of the remaining error is in what the chain gives. `--decompose` takes
  the same layer twice, once where it happens to fall and once shifted by a fraction of a texel so
  that every footprint centre lands on a level-4 texel centre, and the two passes separate the two
  candidate mechanisms:

  | file | pass | render-vs-truth | box-vs-truth | box-vs-tent |
  | --- | --- | --- | --- | --- |
  | 000020.psd | as it falls | 3.374 | 3.592 | 3.582 |
  | 000020.psd | footprint aligned | **0.308** | **0.240** | 0.250 |
  | AUTUMN.psd | as it falls | 3.718 | 3.780 | 3.780 |
  | AUTUMN.psd | footprint aligned | **0.048** | **0.049** | 0.057 |
  | 000077.psd | as it falls | 2.707 | 2.853 | 2.835 |
  | 000077.psd | footprint aligned | **0.209** | **0.208** | 0.224 |

  At exactly 16x one screen pixel IS one level-4 texel, so a footprint that lands on a texel centre is
  read as a plain box and nothing else. There the whole difference from the truth is 0.05 to 0.31 of
  255 - the box's hard ends against the truth's tapered ones cost that, and no more. Where the
  footprint falls between two texels, which is the normal case, the value is a bilinear blend of two
  neighbouring boxes: a kernel of support 2^(l+1) where the footprint is 2^l, twice as wide as it
  should be. That is the 2.7 to 3.7 of 255, and it is a property of reading a mip chain with bilinear
  interpolation at a fractional phase, not of the reduction filter's shape. An earlier version of
  this file attributed the difference to the box's ends; that was wrong by an order of magnitude and
  this table is how it was caught.
- **The axis imbalance in a figure is the content's, not the render's.** On portrait artwork the
  render keeps more per-pixel detail along one axis than the other, which looks like anisotropic
  blur. Measured on 000020.psd (1127x2120): truth gradients 13.863 along x and 10.284 along y, render
  87.0% and 84.1%, and the ideal box model 86.7% and 83.7% - the box model has the same imbalance. On
  000063.psd it points the other way (render 78.5% x and 86.6% y, box model 78.7% and 87.3%). An
  anisotropic kernel would show as the render disagreeing with the box model per axis, and it does
  not: the two agree to within 0.3 percentage points.
- **Where `render-vs-box` is large it is not the filter.** Its worst pixels reach 50 to 63 of 255 on
  three files, at footprints whose box texel has partial alpha (e.g. alpha 84 of 255) - the layer's
  silhouette, where the pixel's value is decided by the MESH's geometric coverage, which neither CPU
  model reproduces. Restricted to footprints whose box texel is fully opaque, the same measurement's
  worst pixel drops to **2 of 255** on every one of those files (mean 0.1 to 0.9). A position sweep
  of the model over +/-0.5 base texel confirms it is not a sub-texel sample-position difference: the
  best offset is (0, 0) on every file.

## What "truth" means here, and how much the definition matters

The ground truth above is the integral over the destination pixel's footprint of the BILINEAR
reconstruction of the artwork. Two things are bundled in that sentence and only one of them is
arguable:

- **The footprint integration is not a choice.** Rendering at ss x and box-averaging ss x ss blocks
  is the integral over the footprint as ss grows; the box is the correct operation, not a filter that
  was picked. How far from the limit the harness actually is is measurable: the same scene rendered
  at ss and at ss/2 differs by 0.004 to 0.031 of 255, so the quadrature is two orders of magnitude
  below the effects being discussed.
- **The reconstruction model is a choice.** GL_LINEAR is what the renderer shows when magnifying, so
  bilinear is the self-consistent model, but someone could equally declare the artwork to be a
  piecewise-CONSTANT array of texels - in which case the right answer is the coverage-weighted
  average of those texels, i.e. area downsampling in the image-processing sense, which is also what a
  mip chain approximates. `--decompose` now computes both, plus a CPU model of each:

  | file | pass | area model vs truth | tent model vs truth | the two definitions differ by | chain vs area | chain vs tent |
  | --- | --- | --- | --- | --- | --- | --- |
  | 000020.psd | as it falls | 0.144 | 0.142 | 0.015 | 3.593 | 3.582 |
  | 000020.psd | aligned | 0.151 | 0.142 | 0.037 | 0.243 | 0.250 |
  | AUTUMN.psd | as it falls | 0.047 | 0.040 | 0.025 | 3.800 | 3.780 |
  | AUTUMN.psd | aligned | 0.045 | 0.040 | 0.023 | 0.052 | 0.057 |

  So the choice of truth is worth 0.02 to 0.04 of 255, and the chain is off from BOTH definitions by
  the same amount (3.59 vs area and 3.58 vs tent as it falls; 0.24 and 0.25 aligned). Being sceptical
  about the truth does not rescue the chain at a fractional phase: under either definition the level-l
  texel, which only records its block's average, cannot say where inside the block the detail was.

## The LOD bias that ships: -0.5 levels, and an open contradiction about it

`ImageKey` now sets `GL_TEXTURE_LOD_BIAS` to -0.5 on every layer texture (the constant is
`core::ImageKey::layerLodBias()`, the reasoning is on `gl::Texture::setLodBias`). The reason is
arithmetic: bilinear filtering at level m takes two taps that are each a whole level-m texel, so the
kernel it integrates is 2*2^m base texels wide, and a footprint of 2^lambda is therefore matched by
m = lambda - 1, one level finer than the derivatives pick.

The evidence for the value is real content, and it is consistent:

| source | bias 0 | bias -0.5 |
| --- | --- | --- |
| 29-file corpus at 16x, mean error | 4.198 | 2.533 |
| 29-file corpus at 4x | 1.631 | 0.968 |
| 000020.psd, one layer at 16x (`--bias1`) | 3.374 | 1.816 |
| AUTUMN.psd, one layer at 16x | 3.718 | 1.792 |

What is NOT consistent is a synthetic fixture. M17 builds line-art-like strokes and compares the
render against a CPU integral of the bilinear reconstruction, the same kind of comparison as
`--bias1`: it reports the bias as slightly WORSE, 16.656 against 15.066, while `--bias1` on the same
kind of comparison over real layers reports it as clearly better. The two have not been reconciled.
M17 therefore asserts only the direction the setting is defined by - a negative bias reads a finer
level, so the render must be sharper, measured as 27.2653 against 17.0293 of mean per-pixel gradient -
and prints both error numbers so the contradiction stays visible instead of being tuned away. The
quality claim rests on the real-content rows above.

## What can be done about it beyond a constant: the LOD bias, measured

`--bias <csv> <file>` sweeps GL_TEXTURE_LOD_BIAS against the supersampled truth. The truth is an
antialiased integral, so it penalises blurring and aliasing alike: whichever bias has the smallest
error there has the smallest total error by this measure, not merely the sharpest picture. Mean error
over eight files (64x64 central window, channel max):

| zoom | -1.5 | -1.0 | -0.5 | 0.0 (shipped) | +0.5 | +1.0 |
| --- | --- | --- | --- | --- | --- | --- |
| 4x | 1.694 | 1.132 | **0.968** | 1.631 | 2.864 | 4.338 |
| 16x | 3.299 | **2.465** | 2.533 | 4.198 | 5.995 | 8.171 |

Per file the best bias is -0.5 on five of eight and -1.0 on two at 4x, and -1.0 on five and -0.5 on
three at 16x. So the shipped lambda is systematically on the blurry side of the optimum: about half
the error is recoverable by sampling a partly finer level, and past about -1.5 the under-filtering
starts to cost more than it gains. Nothing in the shipped code sets a bias; this is where the setting
would be, and it is a fidelity trade, so it is measured and left alone rather than changed quietly.

If the reference for tuning ought to be a high-quality resample rather than the supersampled
render, the optimum could move - so `--bias1 <csv> <file>` scores one real layer at 16x against all
four references at once: the supersampled render, and CPU models of area (coverage-weighted texels),
tent (integral of the bilinear reconstruction) and Lanczos (windowed sinc, a = 3):

| layer | bias | vs ssaa | vs area | vs tent | vs lanczos |
| --- | --- | --- | --- | --- | --- |
| 000020.psd | -0.5 | 1.816 | 1.837 | 1.833 | 1.926 |
| 000020.psd | 0.0 | 3.374 | 3.396 | 3.388 | 3.428 |
| AUTUMN.psd | -1.0 | 1.074 | 1.071 | 1.075 | 1.442 |
| AUTUMN.psd | 0.0 | 3.718 | 3.737 | 3.717 | 3.964 |
| 000063.psd | -1.0 | 0.418 | 0.487 | 0.488 | 0.435 |
| 000063.psd | 0.0 | 2.303 | 2.357 | 2.347 | 2.149 |

The four references sit within 0.02 to 0.3 of each other, and the bias that minimises each of them is
the same bias on every layer. So: area/Lanczos is the better reference to argue about quality with,
and it does not change what the tuning says.

The two other routes, for the record, both out of the mip chain rather than in it: resample each
layer once per display scale with an area/Lanczos filter and cache it (exact by the measurements
above - the CPU area model matches the truth to 0.05 - and affordable precisely because layer content
is static between edits), or supersample the composite for export.

## Anisotropic minification: the elongated footprint

Trilinear minification has to pick ONE level and picks it from the larger of the two screen-space
derivatives, so a layer squashed on one axis is filtered at the level its LONG axis requires and is
over-blurred along the short one by the ratio. That is not a mipmapping defect - the level is right for
the footprint it was chosen for - but it is the one thing a non-uniformly scaled layer loses, and this
app has non-uniform scale as a first-class keyframe (ScaleKey takes a QVector2D). Layer textures now
ask for anisotropy 16 (`core::ImageKey::layerMaxAnisotropy`), clamped to what the device reports, and a
no-op where `EXT_texture_filter_anisotropic` is absent - the harness gives its stubbed device the REAL
limit for exactly that reason, since a stubbed limit would hide a device that cannot do it.

Measured on a synthetic broadband image, error against the exact area integral of the footprint
(`areaIntegralRect`, 1/255, 256 pixels, shipped bias):

| footprint (texels) | anisotropy 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 4:1 - 64x16 | 4.443 | 2.244 | **1.117** | 1.117 | 1.117 |
| 8:1 - 128x16 | 6.971 | 4.012 | 1.838 | **0.741** | 0.741 |
| 16:1 - 256x16 | 12.683 | 6.846 | 3.803 | 1.736 | **0.539** |
| **1:1 (control)** | **0.4817** | 0.4817 | 0.4817 | 0.4817 | 0.4817 |

The gain saturates at the footprint's OWN ratio, because that is how many samples the driver takes
(ceil of the ratio, capped by the requested maximum): 4 wastes nothing but gains nothing either, and
16 is an upper bound rather than a cost. The 1:1 row is the control that matters - an isotropic
footprint is byte-identical at every value, because the driver takes one sample.

On real layers (`--aniso <csv> <file> [ratio]`, 4:1, shipped bias, mean error against the supersampled
truth):

| layer | anisotropy 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| `qitqmd.anie` 3656x2786 | 1.239 | 0.444 | 0.191 | 0.191 | 0.191 |
| `amongusych2.anie` 962x542 | 8.661 | 2.482 | 0.600 | 0.600 | 0.600 |
| `ballerina.anie` 3286x4811 | 6.121 | 3.775 | 3.697 | 3.697 | 3.697 |
| `2.anie` Linework17 (flat background) | 0.007 | 0.003 | 0.001 | 0.001 | 0.001 |

`ballerina.anie`'s residual floor is the box-vs-integral difference measured in "What the remaining
error IS", not an anisotropy one: it stops falling once the footprint is resolved.

What it costs: measured by rendering the same scene 20 times per setting, so the fragment count does not
move, on a 256x256 canvas where a layer of 131k fragments takes 0.4-0.6 ms - small enough that the
sampling is a visible share of it rather than hidden behind the per-frame work:

| footprint | anisotropy | run 1 | run 2 | run 3 |
|---|---|---|---|---|
| 4:1 (8x2 texels) | 1 | 0.425 ms | 0.550 ms | 0.498 ms |
| 4:1 | 4 | 0.475 ms | 0.511 ms | 0.579 ms |
| 4:1 | 16 | 0.496 ms | 0.526 ms | 0.540 ms |
| 16:1 (32x2 texels) | 1 | 0.402 ms | 0.647 ms | 0.460 ms |
| 16:1 | 4 | 0.479 ms | 0.417 ms | 0.664 ms |
| 16:1 | 16 | 0.485 ms | 0.377 ms | 0.457 ms |

Read that honestly: the run-to-run spread (+-0.1 ms) is the same size as any difference between the
settings and the ORDER is random between runs, so this does not show the anisotropy to be free - it puts
a floor under what it can be: a cost below ~0.1 ms per render of a 131k-fragment layer is not resolvable
in this harness, i.e. below ~20% of that render's total and far below it on a real frame, where the
canvas-sized destination texture and the clipping frame dominate (the same measurement on a 1024x1024
canvas is 4.1-4.4 ms for 262k fragments, with the differences inside +-0.3 ms).

And what it does NOT do, measured because it is the one way the policy could have changed the shipped
path: the driver's anisotropic path selects its level differently, which shows up on a hard edge as a
NARROWER kernel. At bias 0, on an isotropic footprint, the implied level moves by up to **0.155** of a
level at fractional zooms (0.9, 0.42, 0.2) and by exactly 0.000 at integer ones (0.5, 0.125). M19
bounds that at a third of a level. The shipped corpus numbers were then re-measured with the policy in
place: 2x/4x/8x/16x over the same files reproduce the published tables, eleven of twelve rows to four
decimals and one (000036.psd at 16x) by 0.0003 in the better direction, which is what a fractional
effective level - the -0.5 bias means the level is never an integer - makes of that 0.155. The LOD bias
scan itself (`--bias1` at anisotropy 1 versus 16, four files, six biases) is bit-identical, so -0.5
stays as shipped.

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

The whole probe (301 checks) runs under AddressSanitizer against an ASan build of the libraries
(`-DAE_ENABLE_ASAN=ON` in `build-asan/`) with **no memory errors** - never a use-after-free, an
overflow or a double free:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 QT_QPA_PLATFORM=offscreen \
    ./build-asan/tests/Debug/verify_mipmap
```

That covers the new CPU code that indexes heap buffers: the premultiply-and-pad loop in
`img::Util::makeTextureUpload`.

Leak detection is worth running too, and it has now caught three real ones:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 QT_QPA_PLATFORM=offscreen \
    ./build-asan/tests/Debug/verify_mipmap     # 301 checks, 0 failures, exit 0, no leaks
```

1. `XCMemBlock` is a plain struct with a raw `uint8*` and no destructor, so a helper that allocates and
   returns one has no owner; the first version of the padding helper did exactly that and leaked one
   texture-sized buffer per layer upload (464,095,874 bytes in 1,976 allocations before, 210,897,574 in
   1,730 after). The helper returns a `std::vector<uint8>` now.
2. The harness's own `makeResource` allocated an `img::ResourceNode` and its image copy and handed the
   node to nobody. That was **267,730,480 of the 267,855,450 bytes** the report held - 99.95% of it, and
   the earlier note here called it "the harness's own, deliberately never freed", which was wrong: a node
   handed to a layer is only BORROWED (the layer takes an `img::ResourceHandle`, a shared_ptr), so the
   harness can and now does own what it allocates, releasing it in `clearRenderFixtures()` - which marks
   exactly the point where the trees that borrowed those handles are gone.
3. Fixing that was worth doing for its own sake, because it is what the gate was hiding: the report then
   read 80 bytes, two allocations, both from `core::Project`'s constructor. `pConf` (a
   `std::vector<audioConfig>`) and `mediaPlayer` (a `mediaState`) are raw `new` members and nothing ever
   deleted them, so every `Project` leaked them - one per document and per export in the app, not just in
   the harness. `~Project` deletes both now.

The probe leaks nothing at all as of this writing: with the two above fixed the run exits 0 with no leak
summary, which is what makes the number meaningful - a regression of a few KB would now be visible
instead of hiding inside hundreds of megabytes of the harness's own.

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

Across the corpus - 35 of the 36 `.anie` files (1885 meshes; `gif COPY2 proj.anie` does not load, in
every run here), frame 12, at three zooms - the numbers are:

| | whole mesh | triangles that can light >= 2 px^2 |
| --- | --- | --- |
| worst spread inside one mesh | 2.00 (4x), 4.01 (16x), 6.00 (64x) | **0.00** at every zoom |
| worst adjacent-pair delta | 0.015 (4x), 0.077 (16x), 0.283 (64x) | **0.000** at every zoom |

The two columns are the point, and the second is the one a user could see. The spread of up to 6 levels
belongs to ONE mesh (`F001.anie`, `Copy of Paint Layer 964`, 714 triangles, LOD 0.000..6.003 at 64x,
mean 5.941) and it is not a seam: its own adjacent-pair delta is 0.004, i.e. its level-0 triangles are
isolated - they are the sub-pixel slivers the mesh creator's edge-vertex reduction leaves behind, and at
16x and 64x *all* of its 714 triangles are under 2 px^2, as is the whole mesh. A blurriness step needs
two triangles that both cover pixels on either side of their shared edge, so the statistic that matters
is the restricted one: at 4x, where 6.27M of the corpus' 6.84M triangles clear that threshold and there
are 9.25M adjacent pairs among them, the spread and the adjacent delta are exactly 0.00 and 0.000. At
16x and 64x only 122 and 106 triangles in the whole corpus are big enough for a step across an internal
edge to be resolvable at all - and theirs are 0.00 too.

Rendering the F001 layer at 4x minification and comparing against the supersampled reference shows
agreement in line with every other project (mean 0.089, p99 0) and, in the worst 220-pixel window, no
triangle-shaped discontinuity: the residual follows the artwork's thin strokes, not the mesh topology.

Honest limits of this measurement: adjacency is detected by shared vertex index, so a seam across a
duplicated-vertex block boundary would not be counted; the "can light a pixel" test is a screen-area
threshold (2 px^2) plus an on-screen bounds test, not a count of rasterized pixels; and the visual
comparison was one project, one frame, one window. So the conclusion is that no seam was found in the
cases that exist here, not that seams are impossible.

The mechanism is real and still there: `LayerMesh` keeps static `texCoords` while `positions` are
deformed (bones/FFD/SRT), so UV derivatives - and therefore LOD - vary from triangle to triangle, and
adjacent triangles can in principle select different mip levels. Before mipmapping every layer sampled
level 0, where such steps cannot exist, so this was the one place the change could plausibly make the
product worse.

It is measured now, on real deformed meshes: `--lodscan` derives the same rho the sampler does, per
triangle, from the mesh a real render just used, and the corpus table above is the verdict - the LOD is
uniform to the last decimal across every triangle that can show a step, at three zooms. M7 independently
pins the UNDEFORMED case: on a uniformly scaled layer the level is constant across the whole mesh (zero
spatial spread at LOD 1, 2 and 3), so neither the grid topology nor the mesh creator's edge-vertex
reduction ("burr reduction") introduces LOD variation where there is no deformation to begin with.

Worth noting the risk window is narrower than it first looks: a layer drawn at or above 1:1 never
samples a mip level at all, so only MINIFIED layers with a deformed mesh are exposed, and a visible
seam needs adjacent triangles to differ by roughly a factor of two in local compression. If it does
prove visible, the standard fix for a 2D tool is one LOD for the whole draw rather than per-fragment
derivatives.

Anisotropic minification is measured and enabled for layer textures now (see the section above);
nothing else in the pipeline samples a texture anisotropically - the filter frames and the clipping
frame are not sampled minified in a way that needs it, and the editor overlay's Qt textures are the
path that is "closed by construction rather than by measurement" below.

## Still to cover

Verified so far: mip chain generation, the production `ImageKey` texture and its parameters, the
rendered composite for a normal layer, all 28 blend modes, the layer HSV branch, the folder composite
path, the grid/mesh overlay, and every mip level of every texture size (M18).

Not yet verified: the export path END TO END. M6 and M8 cover what the exporter changes about the render
that matters for mipmapping - its output-resolution camera (hence a different mip level) and its
`aUseWorkingCache=true` render mode - but the exporter itself is not driven, so its own framebuffer
readback and PNG write are untested here. Driving it is tractable: `core::Project` takes a filename and
its attribute setters (`setImageSize`/`setFps`/`setFrameRange`) allow a project to be populated
programmatically, and `tests/media_exporter_e2e.cpp` shows the `CommonParam`/`ImageParam`/`execute`
shape. It was left undone for budget, not because anything blocked it. (`media_exporter_e2e` itself
fails in this environment for reasons that predate this branch.)

Also outstanding, closed by construction rather than by measurement: the editor overlay path
(`ctrl::Painter`) uploads QImages as `QOpenGLTexture`s with `LinearMipMapLinear`, so Qt builds a chain
for whatever the paint engine hands it. It now converts to `QImage::Format_ARGB32_Premultiplied` before
the upload, the representation a chain is correct for, but there is no pixel-level measurement of that
path here - it only runs inside a widget with a QPainter, which this harness does not drive.

Also outstanding: the glyph-cache decision, and the load-time/memory cost of building the chain (the
rebuild in `gl::Texture::create` adds one readback plus a few thousand averages per non-power-of-two
texture; the ASan run shows it allocates nothing that outlives the call, but the wall-clock cost on the
load path has not been measured).

A reminder from writing M6: a fixed interior crop silently becomes wrong when the layer's scale changes.
At 4x minification the quad is smaller than the crop, so the backdrop averaged in and the "correct"
value read 141.6 instead of 164.0 - a test bug that looks exactly like a product bug. The crop now
follows the quad.

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
