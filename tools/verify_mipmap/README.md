# verify_mipmap — mipmap verification probe

Verifies that layer artwork textures are mipmapped **correctly**: that the generated mip
chain keeps the silhouette's own color instead of dragging it toward the color stored under
alpha 0, which is the classic mipmap dark halo (and its mirror image, the light halo).

## Run

```sh
tools/verify_mipmap/build_and_run.sh              # rebuild libs + probe, then run
tools/verify_mipmap/build_and_run.sh --no-build
AE_MIPMAP_ASAN=1 tools/verify_mipmap/build_and_run.sh   # against build-asan/ instead of build/
```

Runs from the repo root. Tries `QT_QPA_PLATFORM=offscreen` first and falls back to X11
(`DISPLAY=:0`) when offscreen GL is unavailable. Exit code: 0 = all pass, 1 = test failures,
2 = GL bootstrap failure (which is what triggers the X11 fallback).

The probe loads no shaders — it reads mip levels back with `glGetTexImage` and drives the
production `gl::Texture` / `core::ImageKey` directly — so it needs no `assets.qrc` and no
`rcc`, unlike the export harnesses.

## Suites

- **M1 — mip chain generation vs an analytic CPU reference.** Two patterns (a bright shape on
  transparent black, and a dark shape on transparent white — the two halo directions), each as
  a checkerboard and as an inset square. For every case the probe computes, independently on
  the CPU, what averaging *straight* data yields and what averaging *premultiplied* data
  yields, then asserts the GPU chain is the premultiplied one and that the composited result
  over an opaque black backdrop equals `source_color * coverage`.
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

| mutation to `ImageKey::resetTextureCache` | result |
|---|---|
| premultiply deleted, mipmaps kept | M2 fails: `level 1 must match the premultiplied reference (delta 128)`, `no halo (worst deviation 128.0)` |
| mipmaps disabled, premultiply kept | M2 fails: `7 levels expected, got 1`, `GL_TEXTURE_MAX_LEVEL must be 6 (got 0)` |
| none (shipped code) | 50 checks, 0 failures |

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

The probe currently verifies texture generation and the texture's parameters. It does **not**
yet verify the rendered composite through the production shader across blend modes, clipping,
folder composites, the blur ladder, deformed meshes, or the export path — those are the next
suites (see the objective's path list).
