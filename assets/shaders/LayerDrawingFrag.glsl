#version 330

// A is background and B is top layer
// Dissolve remains unsupported (stochastic, not expressible as a pure color function).
// Hue, saturation, color, luminosity, darker color and lighter color are "non RGB
// formulas": they need the whole RGB triple of both pixels (W3C compositing spec,
// Rec.601 luma), so they run through the BLEND_NONSEPARABLE whole-color path.

#define BlendNormal(A,B)     (B)
#define BlendLighten(A,B)    ((A > B) ? A : B)
#define BlendDarken(A,B)     ((A > B) ? B : A)
#define BlendMultiply(A,B)   (A * B)
#define BlendAverage(A,B)    ((A + B) / 2.0)
#define BlendAdd(A,B)        (min(1.0, A + B))
#define BlendSubtract(A,B)   (max(0.0, A - B))
#define BlendDifference(A,B) (abs(A - B))
#define BlendDivide(A,B)     ((B == 0.0) ? B : min(1.0, A / (B)))
#define BlendScreen(A,B)     (1.0 - (1.0 - A) * (1.0 - B))
#define BlendExclusion(A,B)  (A + B - 2 * A * B)
#define BlendOverlay(A,B)    ((A < 0.5) ? (2 * A * B) : (1.0 - 2 * (1.0 - A) * (1.0 - B)))
#define BlendSoftLight(A,B)  ((A < 0.5) ? (2.0 * A * B + A * A * (1.0 - 2.0 * B)) : (sqrt(A) * (2.0 * B - 1.0) + (2.0 * A) * (1.0 - B)))
#define BlendHardLight(A,B)  (BlendOverlay(B, A))
#define BlendColorDodge(A,B) ((B == 1.0) ? B : min(1.0, A / (1.0 - B)))
#define BlendColorBurn(A,B)  ((B == 0.0) ? B : max(0.0, 1.0 - (1.0 - A) / (B)))
#define BlendLinearDodge(A,B)(BlendAdd(A, B))
#define BlendLinearBurn(A,B) (max(0.0, A + B - 1.0))
// Clip Studio Paint modes (measured, see tools/csp_blend_re/RESULTS.md):
// the source color is premultiplied by alpha before these run (see
// BLEND_PREMULTIPLIED_SRC below), so B is already fg*alpha.
// Add (Glow): plain additive clamp, no HDR/normalization.
#define BlendAddGlow(A,B)      (min(A + B, 1.0))
// Glow Dodge: color dodge computed like CSP's 8-bit integer math
// (out = min(255, (bg*255) // max(255-fg, 1))): the division truncates, and
// fg=1 yields 255 only when bg>0, else 0 (0/0 -> 0, unlike Photoshop whose
// dodge special-cases fg=1 to white). floor(x + 0.001) guards float rounding
// (true values are >= 1/254 away from integers, so 0.001 is safe). The
// numerator A*65025.0 and the denominator max(255.0-B*255.0, 1.0) are both in
// 0..255-space (A, B normalized), so the quotient is a plain integer truncation.
#define BlendGlowDodge(A,B)    (min(floor(A * 65025.0 / max(255.0 - B * 255.0, 1.0) + 0.001) / 255.0, 1.0))
#define BlendLinearLight(A,B)((B < 0.5) ? BlendLinearBurn(A, 2.0 * B) : BlendLinearDodge(A, 2.0 * (B - 0.5)))
#define BlendVividLight(A,B) ((B < 0.5) ? BlendColorBurn(A, 2.0 * B) : BlendColorDodge(A, 2.0 * (B - 0.5)))
#define BlendPinLight(A,B)   ((B < 0.5) ? BlendDarken(A, 2.0 * B) : BlendLighten(A, 2.0 * (B - 0.5)))
#define BlendHardMix(A,B)    ((BlendVividLight(A,B)) < 0.5 ? 0.0 : 1.0)

// Non-separable blends (whole-RGB, used when BLEND_NONSEPARABLE=1).
// Formulas follow the W3C compositing spec / PDF reference (which model the
// Photoshop/Krita/GIMP behavior): luma is Rec.601 (0.3, 0.59, 0.11); SetSat
// rescales the chroma spread, SetLum + ClipColor shift luminosity while keeping
// colors in gamut. The formulas operate on STRAIGHT colors: the destination is
// stored premultiplied (composite slots / scene capture), so blendColor
// un-multiplies it before evaluating them (see the BLEND_NONSEPARABLE branch).
float BlendLum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 BlendClipColor(vec3 c) {
    float l = BlendLum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0)
        c = l + (c - l) * (l / (l - n));
    if (x > 1.0)
        c = l + (c - l) * ((1.0 - l) / (x - l));
    return c;
}
vec3 BlendSetLum(vec3 c, float l) { return BlendClipColor(c + (l - BlendLum(c))); }
vec3 BlendSetSat(vec3 c, float s) {
    float lo = min(c.r, min(c.g, c.b));
    float hi = max(c.r, max(c.g, c.b));
    return (hi > lo) ? (c - lo) * (s / (hi - lo)) : vec3(0.0);
}
float BlendSat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

#define BlendHue(A,B)         (BlendSetLum(BlendSetSat(B, BlendSat(A)), BlendLum(A)))
#define BlendSaturation(A,B)  (BlendSetLum(BlendSetSat(A, BlendSat(B)), BlendLum(A)))
#define BlendColor(A,B)       (BlendSetLum(B, BlendLum(A)))
#define BlendLuminosity(A,B)  (BlendSetLum(A, BlendLum(B)))
#define BlendDarkerColor(A,B)   ((BlendLum(B) <= BlendLum(A)) ? B : A)
#define BlendLighterColor(A,B)  ((BlendLum(B) > BlendLum(A)) ? B : A)

#variation BLEND_FUNC BlendNormal
#variation BLEND_NONSEPARABLE 0
#variation BLEND_PREMULTIPLIED_SRC 0
#variation IS_CLIPPEE 0
#variation USE_HSV 0
#variation USE_HSV_MASK 0
#variation LAYER_MASK 0
#variation PREMULTIPLIED_INPUT 0

uniform vec4 uColor;
uniform sampler2D uTexture;
uniform sampler2D uDestTexture;

#if USE_HSV
uniform bool setColor;
uniform float hue;
uniform float saturation;
uniform float value;
#endif

// Both content-space fields (the HSV amount mask and the layer's own matte) are
// sampled through ONE matrix: the inverse of the transforms this layer is drawn
// with, so a field sticks to the ARTWORK while the camera zooms or the layer moves.
#if USE_HSV_MASK || LAYER_MASK
uniform mat3 uScreenToLayer;
#endif

#if USE_HSV_MASK
// A painted AMOUNT mask for the HSV adjustment: the texel at this pixel decides how
// much of the adjustment applies (0 = the untouched colour, 1 = the whole
// adjustment).
uniform sampler2D uHsvMask;
uniform vec4 uHsvMaskRect; // content x, y, w, h
#endif

// THE NODE'S OWN MATTE. Its alpha field says how much of this layer is visible at
// this pixel: 0 hidden, 1 visible. Outside the field the layer is fully visible (a
// field RESTRICTS, it does not enable), and the multiply happens here - after every
// filter - so a blurred layer cannot bleed past its own matte.
#if LAYER_MASK
uniform sampler2D uLayerMask;
uniform vec4 uLayerMaskRect; // content x, y, w, h
#endif

#if IS_CLIPPEE
uniform int uClippingId;
uniform usampler2D uClippingTexture;
#endif

in vec2 vTexCoord;
in vec2 vDestCoord;

layout(location = 0, index = 0) out vec4 oFragColor;

#if USE_HSV
vec3 RGBtoHSV(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 HSVtoRGB(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
#endif

vec4 blendColor(const vec4 src, const vec4 dst)
{
    vec4 result;

#if BLEND_PREMULTIPLIED_SRC
    // CSP Add (Glow) / Glow Dodge: the blend input is the ALPHA-PREMULTIPLIED
    // source color (rounded to 8-bit, matching CSP's stored layer pixels),
    // out = F(bg, round(a*fg)) - not blend-then-composite like Photoshop.
    // The result already accounts for the source alpha, so it replaces the
    // backdrop outright; the framebuffer alpha pass is bypassed by writing an
    // opaque alpha. The destination is un-premultiplied first (the scene is
    // stored premultiplied); a fully transparent dest contributes nothing, so
    // a neutral value is fine there.
    vec3 destStraight = (dst.a > 0.0) ? dst.rgb / dst.a : vec3(0.0);
    vec3 psrc = floor(src.rgb * src.a * 255.0 + 0.5) / 255.0;
    vec3 blended = BLEND_FUNC(destStraight, psrc);
    result.r = blended.r;
    result.g = blended.g;
    result.b = blended.b;
    result.a = 1.0;
#elif BLEND_NONSEPARABLE
    // whole-color blends are defined on straight colors, but the destination is
    // stored premultiplied: recover Cb before evaluating the formula. A fully
    // transparent dest pixel has no defined straight color and contributes
    // nothing (its weight dst.a is 0), so a neutral value is fine there.
    vec3 destStraight = (dst.a > 0.0) ? dst.rgb / dst.a : vec3(0.0);
    vec3 blended = BLEND_FUNC(destStraight, src.rgb);
    result.a = src.a;
    float idstA = 1.0 - dst.a;
    result.r = dst.a * blended.r + idstA * src.r;
    result.g = dst.a * blended.g + idstA * src.g;
    result.b = dst.a * blended.b + idstA * src.b;
#else
    result.a = src.a;
    float idstA = 1.0 - dst.a;
    result.r = dst.a * BLEND_FUNC(dst.r, src.r) + idstA * src.r;
    result.g = dst.a * BLEND_FUNC(dst.g, src.g) + idstA * src.g;
    result.b = dst.a * BLEND_FUNC(dst.b, src.b) + idstA * src.b;
#endif
    return result;
}

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
// zero) - round footprints lose nothing. Magnification (LOD <= 0) keeps the hardware read, where
// bilinear is exact and free, and this stays bit-identical to it there.
uniform float uLayerLodBias;    // the layer's LOD bias, in levels
uniform float uLayerMaxLod;     // the texture's deepest level

vec4 sampleLayer(sampler2D aTexture, vec2 aUv)
{
    vec2 size0 = vec2(textureSize(aTexture, 0));
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
        return texture(aTexture, aUv);    // magnified: bilinear is exact and free

    // THE LEVEL IS THE NEAREST one, not the one below: the taps reconstruct the MAJOR axis and the
    // level's own texel has to supply the MINOR one, so the level whose texel matches `minor` is the
    // one to take. `floor` biases that by up to a full level downward (the level is then narrower
    // than the footprint), which leaves the minor axis under-filtered - measured as the residual
    // error tail this read otherwise still has on thin features.
    float level = clamp(floor(lod + 0.5), 0.0, uLayerMaxLod);
    float scale = exp2(level);              // level-0 texels -> this level's texels
    float majorL = major / scale;
    float minorL = minor / scale;
    vec2 sizeLf = vec2(textureSize(aTexture, clamp(int(level), 0, int(uLayerMaxLod))));
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
                acc += textureLod(aTexture, centre + off, level);
            }
    }
    return acc / 8.0;
}

void main(void)
{
    vec4 color = sampleLayer(uTexture, vTexCoord);
#if PREMULTIPLIED_INPUT
    color.rgb /= max(color.a, 0.001);
#endif
    color *= uColor;
    ivec2 destCoord = ivec2(vDestCoord);
    vec4 destColor = texelFetch(uDestTexture, destCoord, 0);

#if USE_HSV
    vec3 hsvOriginal = color.xyz;
    vec3 hsv = RGBtoHSV(color.xyz);
    if (setColor)
    {
        hsv.x = hue;
    }
    else
    {
        hsv.x += hue;
    }
    hsv.x = fract(hsv.x);
    hsv.y = min(1.0, hsv.y * saturation);
    hsv.z = min(1.0, hsv.z * value);
    color.xyz = HSVtoRGB(hsv);

#if USE_HSV_MASK
    {
        vec2 q = (uScreenToLayer * vec3(vDestCoord, 1.0)).xy;
        vec2 uv = (q - uHsvMaskRect.xy) / uHsvMaskRect.zw;
        float amount = (uv.x < 0.0 || 1.0 < uv.x || uv.y < 0.0 || 1.0 < uv.y) ? 1.0 : texture(uHsvMask, uv).r;
        // outside the field the adjustment applies in full: the field restricts, it
        // does not enable
        color.xyz = mix(hsvOriginal, color.xyz, amount);
    }
#endif
#endif

#if LAYER_MASK
    {
        vec2 q = (uScreenToLayer * vec3(vDestCoord, 1.0)).xy;
        vec2 uv = (q - uLayerMaskRect.xy) / uLayerMaskRect.zw;
        float visible = (uv.x < 0.0 || 1.0 < uv.x || uv.y < 0.0 || 1.0 < uv.y)
            ? 1.0
            : texture(uLayerMask, uv).r;
        color.a *= visible;
        if (color.a <= 0.0)
            discard;
    }
#endif

#if IS_CLIPPEE
    uvec2 clippingData = texelFetch(uClippingTexture, destCoord, 0).xy;
    if (uClippingId == int(clippingData.x))
    {
        color.a *= float(clippingData.y) / 255.0;
        oFragColor = blendColor(color, destColor);
    }
    else
    {
        //oFragColor = destColor;
        discard;
    }
#else
    oFragColor = blendColor(color, destColor);
#endif
}
