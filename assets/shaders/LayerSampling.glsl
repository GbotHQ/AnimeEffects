// THE LAYER SAMPLER - shared by the layer draw and the clipper's singulation pass.
//
// ONE READ, BOTH PASSES. The clipper used to sample its own alpha with texture() - the hardware's
// implicit-LOD, anisotropic read - while the layer it clipped drew through this reconstruction.
// The two filters disagreed at every clipped edge, and a paged clipper could not reproduce the
// hardware read at all. So the clipper pass takes the same sampler; with VIRTUAL_TEXTURE it takes
// the same pages and coarse fallback, which is what lets clipper layers be paged.
//
// The sampler's own rationale (the two-tap, one-level reconstruction) is the comment below.

#if VIRTUAL_TEXTURE
uniform sampler2D uCoarseTexture;      // unit 4: the always-resident levels
uniform usampler2D uPageTable;         // unit 5: (pageX, levelRow + pageY) -> slot
uniform int uPageLevelRow[16];         // the first row of each page level in that table
uniform vec2 uLayerSize;               // the CONTENT's size in level-0 texels
uniform float uPageTexels;             // the page size, 256
uniform int uCoarseLevel;              // the first level the coarse chain holds
uniform float uPageAtlasSize;          // 4096
uniform float uPagePitch;              // page + gutter, 260
uniform float uPageGutter;             // 2
uniform int uPageSlotsPerAxis;         // 4096 / 260 = 15
uniform int uAtlasCount;               // how many of the four below are real
uniform sampler2D uAtlas0;
uniform sampler2D uAtlas1;
uniform sampler2D uAtlas2;
uniform sampler2D uAtlas3;
#endif

// THE LAYER SAMPLE: a two-tap, one-level reconstruction of the pixel's footprint.
//
// THE GEOMETRY. The footprint comes from the draw itself: J = [dFdx dFdy] of the texel-space uv
// (aUv * textureSize(aTexture, 0)) is the 2x2 Jacobian of the draw, and its singular values are the
// footprint's axes. J^T J is symmetric 2x2, so its eigenvalues are the squared singular values
// (major >= minor); its major eigenvector is a SCREEN-space direction, and the footprint's major
// axis in TEXEL space - the space the taps offset - is J times that vector. A derivative is exact
// for an affine draw - phase-independently, measured identical to the transform's footprint over a
// 24-step sub-pixel sweep - and it is the only thing that follows a mesh/FFD/bone deformation, which
// the transform cannot see, so nothing else is needed.
//
// THE LEVEL comes from the MINOR axis, because the taps integrate the MAJOR one: a footprint of 8 x 1
// texels is one texel thick across, so floor(log2(minor)) is the level whose texels the taps match
// across, and the major axis is covered by the taps' extent rather than by the level's blur.
//
// THE TAPS: TWO taps on ONE level, each a 2x2 block of filtered reads spanning major/2 along the
// major axis and minor across it, placed at t = +-0.25 * major along the major axis. The per-level
// span is deconvolved by 0.8 for levels above 0, because a level texel is already a box average of
// the level below. That is eight texel fetches - the same count as the hardware trilinear read's two
// levels of one 2x2 tap - with no mix between levels, so none of the level blend's blur enters.
//
// WHY THIS SHAPE: the square two-level read over-integrates a footprint's minor axis and
// under-integrates its major one. Measured against a 4x supersampled reference over 352 anisotropic
// cells (2:1 to 8:1) this reads error mean 0.485 of 255 against 3.735 (better in 12/12 cells), at
// 1.3:1 better in 23/24 cells (median -0.1496), and at exactly 1:1 a wash (13/24, interval through
// zero) - round footprints lose nothing. Magnification (LOD <= 0) is one explicit level-0 filtered
// read: textureLod(..., 0) on the full texture, the level-0 page on the paged one. Under true
// magnification that is the hardware's bilinear read, and it is bit-identical to it.
uniform float uLayerLodBias;    // the layer's LOD bias, in levels
uniform float uLayerMaxLod;     // the texture's deepest level

#if VIRTUAL_TEXTURE
// THE PAGED READ. A level is no longer one texture: page (px, py) of it lives at the slot the
// table names, inside one of up to four atlases, with a 2-texel gutter of the level's
// neighbouring texels around it. So a filtered read at a level texel is: which page owns it, is
// that page resident, where is its slot, and what are the atlas coordinates of the texel.
//
// EVERY TAP RESOLVES ITS OWN PAGE, not the pixel's: a tap is offset by up to the footprint's
// anisotropy ratio in level texels, which can cross a page boundary. The gutter is what makes
// the read INSIDE a page seamless; the per-tap lookup is what makes the read across one correct.
//
// A MISS SAMPLES THE COARSE CHAIN: the always-resident levels from uCoarseLevel down. Its level
// k is the full chain's level uCoarseLevel + k, so the relative level is the same content at
// the same texel grid - a blur, never a hole. The transition at a page boundary is hard by
// design (the two tiers are separate reads, not a level blend), and a page arriving is a
// quality increase rather than a pop.
vec4 sampleCoarse(vec2 aUv, float aLevel)
{
    return textureLod(uCoarseTexture, aUv, max(0.0, aLevel - float(uCoarseLevel)));
}

vec4 sampleAtlas(int aAtlas, vec2 aAtlasTexel)
{
    vec2 uv = aAtlasTexel / uPageAtlasSize;
    if (aAtlas == 0) return textureLod(uAtlas0, uv, 0.0);
    if (aAtlas == 1) return textureLod(uAtlas1, uv, 0.0);
    if (aAtlas == 2) return textureLod(uAtlas2, uv, 0.0);
    return textureLod(uAtlas3, uv, 0.0);
}

vec4 samplePage(vec2 aUv, float aLevel)
{
    vec2 sizeL = max(vec2(1.0), floor(uLayerSize / exp2(aLevel)));
    vec2 p = aUv * sizeL;
    // THE CONTENT'S BORDER. The full texture's clamp-to-border returns 0 outside the content, but
    // its bilinear read still blends the edge texel when the coordinate is within ONE texel of
    // it - and a reconstruction tap placed just outside a layer's edge is exactly that case. The
    // page's gutter holds the level's neighbouring texels and its zeros beyond them, so a read
    // within one level texel is left to the atlas; only a coordinate whose whole 2x2 footprint is
    // outside returns 0. Zeroing every tap centre outside [0,1] instead measured as a one-pixel
    // band at each layer's top/bottom edge (max 40-44 of 255 on test.anie): the taps that should
    // have blended the edge texel contributed nothing.
    if (p.x < -1.0 || sizeL.x + 1.0 < p.x || p.y < -1.0 || sizeL.y + 1.0 < p.y)
        return vec4(0.0);

    vec2 grid = ceil(sizeL / uPageTexels);
    // p == sizeL can land one past the last page when the level divides evenly; the page's
    // out-of-content texels are zero, so clamping the PAGE and leaving `local` at the content
    // edge reproduces the border read
    vec2 page = clamp(floor(p / uPageTexels), vec2(0.0), grid - vec2(1.0));
    vec2 local = p - page * uPageTexels;
    int level = int(aLevel);

    uint entry = texelFetch(uPageTable, ivec2(page) + ivec2(0, uPageLevelRow[level]), 0).r;
    if (entry == 0u)
        return sampleCoarse(aUv, aLevel);

    int atlas = int(entry >> 8);
    int slot = int(entry & 255u) - 1;
    if (slot < 0 || atlas >= uAtlasCount)
        return sampleCoarse(aUv, aLevel);   // a stale entry: never a hole

    // the slot's CONTENT origin, in atlas texels, plus the gutter the tap reads from
    vec2 origin = vec2(float(slot % uPageSlotsPerAxis), float(slot / uPageSlotsPerAxis))
        * uPagePitch + vec2(uPageGutter);
    return sampleAtlas(atlas, origin + local);
}
#endif

vec4 sampleLayer(sampler2D aTexture, vec2 aUv)
{
#if VIRTUAL_TEXTURE
    vec2 size0 = uLayerSize;
#else
    vec2 size0 = vec2(textureSize(aTexture, 0));
#endif
    vec2 tuv = aUv * size0;
    vec2 dx = dFdx(tuv);
    vec2 dy = dFdy(tuv);
    // J^T J is symmetric 2x2: its eigenvalues are the squared singular values of J
    float jxx = dot(dx, dx);
    float jxy = dot(dx, dy);
    float jyy = dot(dy, dy);
    float halfTrace = (jxx + jyy) * 0.5;
    float disc = sqrt(max(halfTrace * halfTrace - (jxx * jyy - jxy * jxy), 0.0));
    float major = sqrt(max(halfTrace + disc, 0.0));
    float minor = sqrt(max(halfTrace - disc, 0.0));
    // THE MAJOR AXIS, IN TEXEL SPACE. J^T J acts on SCREEN offsets (J is the screen->texel map), so
    // its major eigenvector is the footprint's long axis as a SCREEN direction. The taps below
    // offset aUv, which is TEXEL space, so that screen vector cannot be used as an offset directly:
    // the texel-space axis is the left singular vector, J times the right one (normalised). The two
    // coincide only when J is symmetric - for an anisotropic draw that is rotated relative to the
    // texture, the screen eigenvector turns with the draw while the footprint's texel-space axis
    // stays put, so using it directly rotates the taps away from the major axis by exactly the
    // draw's rotation (at 90 degrees the taps ran ACROSS the footprint, which is the measured
    // rotation-dependent error). An eigenvector's scale and sign are arbitrary; both cancel.
    vec2 eig = vec2(jxy, halfTrace + disc - jxx);
    if (dot(eig, eig) < 1e-20)
        eig = vec2(halfTrace + disc - jyy, jxy);
    vec2 majorDir = dx * eig.x + dy * eig.y;    // J * eig: the screen axis in texel space
    if (dot(majorDir, majorDir) < 1e-20)
        majorDir = vec2(1.0, 0.0);              // isotropic (J ~ c * rotation): any axis will do
    majorDir = normalize(majorDir);

    // The bias is applied to the UNCLAMPED level and magnification returns before any tap: clamping
    // into [0, maxLod] first would take taps where a bilinear read is exact, and the bias is
    // negative, so that case is common.
    float lod = log2(max(minor, 1e-6)) + uLayerLodBias;
    if (lod <= 0.0)
#if VIRTUAL_TEXTURE
        return samplePage(aUv, 0.0);      // magnified: level 0 bilinear is exact and free
#else
        // LEVEL 0 BY POLICY, not by the hardware's own LOD. With the negative layer bias this
        // branch is entered while the unbiased footprint can still be minifying, and the
        // hardware's texture() would then blend the next mip level (and may filter
        // anisotropically) - a read no page stack can reproduce. textureLod(..., 0) is exactly
        // the same read as texture() under true magnification, and is what the page path takes:
        // measured on test.anie as a mean of 0.149 of 255 (max 1 here; max 43 there) until the
        // two branches agreed.
        return textureLod(aTexture, aUv, 0.0);
#endif

    // THE LEVEL IS THE NEAREST one, not the one below: the taps reconstruct the MAJOR axis and the
    // level's own texel has to supply the MINOR one, so the level whose texel matches `minor` is the
    // one to take. `floor` biases that by up to a full level downward (the level is then narrower
    // than the footprint), which leaves the minor axis under-filtered - measured as the residual
    // error tail this read otherwise still has on thin features.
    float level = clamp(floor(lod + 0.5), 0.0, uLayerMaxLod);
    float scale = exp2(level);              // level-0 texels -> this level's texels
    float majorL = major / scale;
    float minorL = minor / scale;
#if VIRTUAL_TEXTURE
    vec2 sizeLf = max(vec2(1.0), floor(size0 / scale));
#else
    vec2 sizeLf = vec2(textureSize(aTexture, clamp(int(level), 0, int(uLayerMaxLod))));
#endif
    vec2 acrossDir = vec2(-majorDir.y, majorDir.x);
    // A level texel is already a box average of the level below, so a tap's span is deconvolved by
    // 0.8 above level 0. One tap covers major/2 along the major axis and minor across it.
    float levelSpan = (level < 0.5) ? 1.0 : 0.8;
    vec2 stepMaj = majorL * levelSpan / 2.0 / sizeLf;
    vec2 stepMin = minorL * levelSpan * 0.5 / sizeLf;

    // TWO taps at +-major/4 along the major axis, each a 2x2 block of FILTERED reads: a filtered
    // read reconstructs inside its texel, which is what the block's coverage needs. The across
    // index runs OUTER and the major index INNER, so a block's four accumulations - and the
    // rounding of their sum - happen in a fixed order.
    vec4 acc = vec4(0.0);
    for (int t = 0; t < 2; ++t) {
        float along = ((float(t) + 0.5) / 2.0 - 0.5) * majorL;
        vec2 centre = aUv + (majorDir * along) / sizeLf;
        for (int k = 0; k < 2; ++k)
            for (int j = 0; j < 2; ++j) {
                vec2 off = majorDir * ((float(j) + 0.5) * stepMaj - stepMaj)
                    + acrossDir * ((float(k) + 0.5) * stepMin - stepMin);
#if VIRTUAL_TEXTURE
                // THE COARSE TIER IS SAMPLED LIKE THE CONTROL'S, reconstruction included. A level
                // the coarse chain owns (index >= uCoarseLevel) is resident in full, so there is
                // no page to resolve -
                // but returning one bilinear read there ran a DIFFERENT filter from the full
                // texture's two taps, and that difference (not residency, not the table) measured
                // 816 px over 8/255, max 115, on nene's Layer 6 while every page was resident.
                // The tap offsets are level texels either way; only the level index shifts.
                acc += (level >= float(uCoarseLevel))
                    ? sampleCoarse(centre + off, level)
                    : samplePage(centre + off, level);
#else
                acc += textureLod(aTexture, centre + off, level);
#endif
            }
    }
    return acc / 8.0;
}
