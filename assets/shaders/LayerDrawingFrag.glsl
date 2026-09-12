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
#variation PREMULTIPLIED_INPUT 0

uniform vec4 uColor;
uniform sampler2D uTexture;
uniform sampler2D uDestTexture;

// Photoshop "Blend If" (see core/BlendIf.h): thresholds normalized to 0..1 as
// (blackLo, blackHi, whiteLo, whiteHi); uBlendIfEnable has bit 0 = destination range
// ("Underlying Layer"), bit 1 = source range ("This Layer"). These uniforms belong to the
// linked program, so every draw sets them - a layer without ranges passes enable = 0.
uniform vec4 uBlendIfDst;
uniform vec4 uBlendIfSrc;
uniform int uBlendIfEnable;

// Layer mask (see img/ResourceData.h): a single-channel plane in the layer's own texture
// coordinates. uMaskEnable is 0 (no mask / disabled), 1 (mask) or 2 (inverted mask); the
// branch is uniform-conditional, so unmasked layers pay no fetch.
uniform sampler2D uMaskTexture;
uniform int uMaskEnable;

// Layer effects that replace the fill colour (core/LayerEffects.h): a colour overlay and a
// linear gradient overlay, both applied as Normal-blend replacements. uFxEnable has bit 0 =
// colour overlay, bit 1 = gradient overlay. The gradient's stops arrive as an array (colour +
// location in .xyz/.w, opacity in the parallel float array), sampled in the LAYER's own image
// coordinates, which is the space Photoshop anchors the overlay to.
uniform int uFxEnable;
// Photoshop's STROKE ("FrFX", core/LayerEffects.h): a band of `uStrokeWidth` pixels inside
// the layer's outline, painted with a solid colour or a gradient and blended with the
// layer's own pixels (Photoshop composites a style against the layer's content, and the
// layer - fill plus styles - against the scene). The band comes from the layer's outline
// distance field (core/DistanceField.h) on the next free texture unit.
uniform int uStrokeEnable;
uniform float uStrokeWidth;
uniform int uStrokeBlend;      // fxBlend code of the stroke's own mode
uniform int uStrokeSolid;      // 1 = uStrokeColor, 0 = the stroke's gradient
uniform vec4 uStrokeColor;     // rgb + opacity
uniform sampler2D uDistanceTexture;
uniform vec4 uStrokeGradParams; // x = angle (radians), y = scale (fraction), z = reverse
uniform vec2 uStrokeGradOffset; // fractions of the image size
uniform vec4 uStrokeStopColor[8];
uniform float uStrokeStopAlpha[8];
uniform int uStrokeStopCount;
// Photoshop's FILL opacity (the PSD "iOpa" key, 0..1): it scales the layer's own pixels
// WITHOUT touching the layer effects, so a 0 % fill still shows the styles at full strength
// (the classic glow-only layer). The layer's own opacity key stays in uColor.a.
uniform float uFillOpacity;
uniform vec4 uFxColor;          // rgb + opacity
uniform vec4 uFxGradParams;     // x = angle (radians), y = scale (fraction of 100), z = reverse
uniform vec2 uFxGradOffset;     // fractions of the image size
// the layer's texture size: shared with the vertex stage's uniform of the same name (GLSL
// uniforms are per program, not per stage), which is where the draw sites already set it
uniform vec2 uImageSize;
uniform vec4 uFxStopColor[8];   // rgb + location
uniform float uFxStopAlpha[8];
uniform int uFxStopCount;

#if USE_HSV
uniform bool setColor;
uniform float hue;
uniform float saturation;
uniform float value;
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

// Gradient overlay sampling: the gradient runs along the given angle across the layer's
// image-space bounds, centred, over `scale` of their extent (Photoshop's 90 degrees puts the
// first stop at the bottom, i.e. the axis is -y in image rows).
vec4 fxGradientSample(vec2 aImagePos, vec2 aImageSize, vec4 aParams, vec2 aOffset,
                      vec4 aStops[8], float aAlphas[8], int aStopCount) {
    vec2 centered = aImagePos - aImageSize * 0.5 - aOffset * aImageSize;
    vec2 axis = vec2(cos(aParams.x), -sin(aParams.x));
    float extent = abs(axis.x) * aImageSize.x + abs(axis.y) * aImageSize.y;
    float length = max(extent * aParams.y, 0.001);
    float t = 0.5 + dot(centered, axis) / length;
    t = clamp(aParams.z > 0.5 ? 1.0 - t : t, 0.0, 1.0);

    int count = max(aStopCount, 1);
    vec4 result = aStops[0];
    float alpha = aAlphas[0];
    for (int i = 1; i < 8; ++i) {
        if (i >= count) {
            break;
        }
        float a = aStops[i - 1].w;
        float b = aStops[i].w;
        float span = max(b - a, 1e-6);
        float k = clamp((t - a) / span, 0.0, 1.0);
        if (t >= a && t <= b) {
            result = mix(aStops[i - 1], aStops[i], k);
            alpha = mix(aAlphas[i - 1], aAlphas[i], k);
        } else if (t > b) {
            result = aStops[i];
            alpha = aAlphas[i];
        }
    }
    return vec4(result.rgb, alpha);
}

vec4 fxGradientColor(vec2 aImagePos, vec2 aImageSize) {
    return fxGradientSample(
        aImagePos, aImageSize, uFxGradParams, uFxGradOffset, uFxStopColor, uFxStopAlpha, uFxStopCount);
}

// The stroke's own blend mode, applied against the layer's pixels (A = the layer's colour,
// B = the stroke colour). Photoshop's style blend modes are the same formulas as the layer
// blend modes; the codes come from effectBlendCode() (core/LayerEffects.h), and any mode
// without a branch here is reported rather than approximated.
// (GLSL has no vector-condition ternary, so the split formulas select with step()/mix();
// the divisions are guarded the way the scalar BLEND_* macros guard theirs)
vec3 fxBlend(int aMode, vec3 A, vec3 B) {
    const float kEps = 0.00390625; // 1/256: at or above the top level, dodge is white
    if (aMode == 1) return A * B;                                    // multiply
    if (aMode == 2) return 1.0 - (1.0 - A) * (1.0 - B);              // screen
    if (aMode == 3) {                                                // overlay
        return mix(2.0 * A * B, 1.0 - 2.0 * (1.0 - A) * (1.0 - B), step(0.5, A));
    }
    if (aMode == 4) return min(A, B);                                // darken
    if (aMode == 5) return max(A, B);                                // lighten
    if (aMode == 6) {                                                // colour dodge
        vec3 dodge = min(vec3(1.0), A / max(1.0 - B, kEps));
        return mix(dodge, vec3(1.0), step(1.0 - kEps, B));
    }
    if (aMode == 7) {                                                // colour burn
        vec3 burn = max(vec3(0.0), 1.0 - (1.0 - A) / max(B, kEps));
        return mix(burn, vec3(0.0), step(B, vec3(kEps)));
    }
    if (aMode == 8) return min(vec3(1.0), A + B);                    // linear dodge
    if (aMode == 9) return max(vec3(0.0), A + B - 1.0);              // linear burn
    if (aMode == 10) {                                               // soft light
        return mix(2.0 * A * B + A * A * (1.0 - 2.0 * B),
            sqrt(A) * (2.0 * B - 1.0) + (2.0 * A) * (1.0 - B), step(0.5, A));
    }
    if (aMode == 11) {                                               // hard light
        return mix(2.0 * A * B, 1.0 - 2.0 * (1.0 - A) * (1.0 - B), step(0.5, B));
    }
    if (aMode == 12) return abs(A - B);                              // difference
    if (aMode == 13) return A + B - 2.0 * A * B;                     // exclusion
    return B;                                                        // normal
}

// Blend If: one split-slider range over a 0..1 value. Equal thresholds collapse to a hard step,
// which makes the default (0, 0, 255, 255) a no-op: the rising edge is already satisfied at 0
// and the falling edge only ends past 1. The two edges of one slider combine with MIN, not by
// multiplying: on an ordinary range the ramps never dip in the same place (so min and product
// agree there), but on a CROSSED one - the black side's high handle past the white side's low
// handle, which 3 layers of this repository's 114 MB PSD do - the product under-weights the
// band, and on those ranges the product's ceiling (0.243 against min's 0.493 for the worst one)
// is below the weight the file's own composite requires. Measured against that composite, min()
// reproduces it at mean 0.8461 (max-channel, effect-free pixels) against 3.2933 for the product;
// see core/BlendIf.cpp for the per-luma recovery and for what the evidence does and does not
// cover.
float BlendIfRange(float v, vec4 t) {
    float lo = (t.y > t.x) ? clamp((v - t.x) / (t.y - t.x), 0.0, 1.0) : ((v >= t.x) ? 1.0 : 0.0);
    float hi = (t.w > t.z) ? clamp((t.w - v) / (t.w - t.z), 0.0, 1.0) : ((v <= t.w) ? 1.0 : 0.0);
    return min(lo, hi);
}

// Per-pixel contribution weight. The destination range reads the captured backdrop (stored
// premultiplied, un-multiplied here exactly like the non-separable branch below); the
// source range reads the layer's own color after the HSV adjustment, which is the analogue
// of Photoshop's "This Layer" pixels. Both use Rec.601 luma - the same BlendLum the
// non-separable blends use, and the space the 8-bit framebuffers hold.
float BlendIfWeight(vec3 srcColor, vec4 dstColor) {
    float w = 1.0;
    if ((uBlendIfEnable & 2) != 0) {
        w *= BlendIfRange(BlendLum(srcColor), uBlendIfSrc);
    }
    if ((uBlendIfEnable & 1) != 0) {
        vec3 dstStraight = (dstColor.a > 0.0) ? dstColor.rgb / dstColor.a : vec3(0.0);
        w *= BlendIfRange(BlendLum(dstStraight), uBlendIfDst);
    }
    return w;
}

vec4 blendColor(const vec4 src, const vec4 dst, const float weight)
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
    // this branch replaces the backdrop outright, so the Blend If weight rides the alpha
    // that the framebuffer pass uses to mix the replacement with the untouched scene
    result.a = weight;
#elif BLEND_NONSEPARABLE
    // whole-color blends are defined on straight colors, but the destination is
    // stored premultiplied: recover Cb before evaluating the formula. A fully
    // transparent dest pixel has no defined straight color and contributes
    // nothing (its weight dst.a is 0), so a neutral value is fine there.
    vec3 destStraight = (dst.a > 0.0) ? dst.rgb / dst.a : vec3(0.0);
    vec3 blended = BLEND_FUNC(destStraight, src.rgb);
    result.a = src.a * weight;
    float idstA = 1.0 - dst.a;
    result.r = dst.a * blended.r + idstA * src.r;
    result.g = dst.a * blended.g + idstA * src.g;
    result.b = dst.a * blended.b + idstA * src.b;
#else
    result.a = src.a * weight;
    float idstA = 1.0 - dst.a;
    result.r = dst.a * BLEND_FUNC(dst.r, src.r) + idstA * src.r;
    result.g = dst.a * BLEND_FUNC(dst.g, src.g) + idstA * src.g;
    result.b = dst.a * BLEND_FUNC(dst.b, src.b) + idstA * src.b;
#endif
    return result;
}

void main(void)
{
    vec4 color = texture(uTexture, vTexCoord);
#if PREMULTIPLIED_INPUT
    color.rgb /= max(color.a, 0.001);
#endif
    color *= uColor;

    // The layer's own pixels and the effects that replace them composite WITH EACH OTHER
    // inside the layer's coverage, each with its own opacity: the layer opacity (uColor.a,
    // already folded into color.a) scales the whole result, while the fill opacity scales
    // only the pixels. A 0 % fill therefore leaves the styles untouched, and a fully opaque
    // style hides the pixels underneath it. Uniforms are per linked program and unset
    // uniforms read as 0, so every draw site states uFillOpacity (1.0 when the layer has
    // none): see LayerNode::renderLayer and FilterFrame::drawQuad.
    // (plain locals, not const: GLSL requires a constant expression to initialise const)
    float coverage = color.a;    // shape x layer opacity
    vec3 pixels = color.rgb;     // straight color of the layer's own pixels
    float fillCoverage = clamp(uFillOpacity, 0.0, 1.0);
    vec3 overRgb = pixels;
    float overCoverage = 0.0;
    if ((uFxEnable & 1) != 0) {
        overRgb = mix(overRgb, uFxColor.rgb, uFxColor.a);
        overCoverage = max(overCoverage, uFxColor.a);
    }
    if ((uFxEnable & 2) != 0) {
        vec4 gradient = fxGradientColor(vTexCoord * uImageSize, uImageSize);
        overRgb = mix(overRgb, gradient.rgb, gradient.a);
        overCoverage = max(overCoverage, gradient.a);
    }
    {
        // style over pixels inside the layer: out.a = f + o*(1-f) for the two coverages
        float innerCoverage = fillCoverage + overCoverage * (1.0 - fillCoverage);
        vec3 innerStraight = (overRgb * overCoverage + pixels * fillCoverage * (1.0 - overCoverage))
            / max(innerCoverage, 0.0001);
        color.rgb = innerStraight;
        color.a = coverage * innerCoverage;
    }

    // Photoshop's Stroke sits above the fill-replacing styles: the band within uStrokeWidth
    // pixels of the outline is painted with the stroke's colour and blended with the pixels
    // underneath it. An inside stroke cannot be more opaque than the shape it sits in, so the
    // band is capped by the layer's coverage - which is also what antialiases its outer edge
    // together with the 1-pixel ramp at the band's inner edge.
    if (uStrokeEnable != 0) {
        float outlineDistance = texture(uDistanceTexture, vTexCoord).r * 255.0;
        float band = min(clamp(uStrokeWidth + 0.5 - outlineDistance, 0.0, 1.0), color.a);
        float strokeAlpha = band * uStrokeColor.a;
        if (strokeAlpha > 0.0) {
            vec3 strokeRgb = (uStrokeSolid != 0)
                ? uStrokeColor.rgb
                : fxGradientSample(vTexCoord * uImageSize, uImageSize, uStrokeGradParams,
                      uStrokeGradOffset, uStrokeStopColor, uStrokeStopAlpha, uStrokeStopCount).rgb;
            color.rgb = mix(color.rgb, fxBlend(uStrokeBlend, color.rgb, strokeRgb), strokeAlpha);
        }
    }

    // the mask scales the layer's coverage; it composes multiplicatively with the Blend If
    // weight below (both are alpha factors on the same fragment)
    if (uMaskEnable != 0) {
        float maskValue = texture(uMaskTexture, vTexCoord).r; // GLSL const needs a constant expression
        color.a *= (uMaskEnable == 2) ? (1.0 - maskValue) : maskValue;
    }

    ivec2 destCoord = ivec2(vDestCoord);
    vec4 destColor = texelFetch(uDestTexture, destCoord, 0);

#if USE_HSV
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
#endif

    // Blend If weight from the adjusted layer color and the captured backdrop. The
    // clipping coverage below multiplies the same alpha, so the two compose.
    float blendIfWeight = ((uBlendIfEnable & 3) != 0) ? BlendIfWeight(color.rgb, destColor) : 1.0;

#if IS_CLIPPEE
    uvec2 clippingData = texelFetch(uClippingTexture, destCoord, 0).xy;
    if (uClippingId == int(clippingData.x))
    {
        color.a *= float(clippingData.y) / 255.0;
        oFragColor = blendColor(color, destColor, blendIfWeight);
    }
    else
    {
        //oFragColor = destColor;
        discard;
    }
#else
    oFragColor = blendColor(color, destColor, blendIfWeight);
#endif
}
