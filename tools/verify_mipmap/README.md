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
- **M2 — the production `core::ImageKey` texture.** Builds a real `img::ResourceNode` the way
  the loaders do, lets `ImageKey` construct its texture, and asserts the level count, the
  min/mag/`GL_TEXTURE_MAX_LEVEL`/wrap parameters, that level 1 is the premultiplied chain, and
  that the halo assertion holds on the shipped texture.

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
| `ShaderHolder`: `PREMULTIPLIED_INPUT` put back to `0` | **M3 fails: the minified layer renders `red=50.0` instead of `100.0`** — the halo reproduced in a real render (`source * coverage^2 = 200 * 0.25`) |
| none (shipped code) | 58 checks, 0 failures |

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

## Still to cover

Verified so far: mip chain generation, the production `ImageKey` texture and its parameters, the
rendered composite for a normal layer, and all 28 blend modes.

Not yet verified: clipping (the clippee/clipper sibling structure), folder composites, the
blur/filter ladder over mipmapped artwork, the grid/mesh overlay, deformed-mesh LOD behaviour
(`LayerMesh` keeps static `texCoords` while `positions` are deformed, so LOD varies per
triangle), the export path (`ctrl::Exporter`) against the interactive render, and an ASan run
over the new CPU premultiply pass.

M4's equivalence reference cannot catch a *global* shader error, because both sides of the
comparison run through the same shader. Only M3's analytic assertion has power against that; the
sweep is there for mode-specific differences.
