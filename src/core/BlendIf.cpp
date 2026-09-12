#include <algorithm>
#include "gl/EasyShaderProgram.h"
#include "core/BlendIf.h"

namespace core {

// The two edges of one split slider. Photoshop combines them with min(), NOT by multiplying
// them.
//
// The rules differ ONLY when the handles cross (blackHi > whiteLo, both edges live): on an
// ordinary range the ramps never dip in the same place, so min() and the product are identical
// there. This repository's 114 MB test PSD has 7 layers with a non-default range and 3 of them
// genuinely cross, all on the destination side, e.g. Layer 49 dst = (blackLo 29, blackHi 179,
// whiteLo 68, whiteHi 137) - the black side's high handle sits past the white side's low one.
// A range is only a counter-example if both its edges are LIVE: a degenerate (255, 255, 0, 0)
// (which this file also carries) has zero-width ramps, where both rules give 0.
//
// What the crossing settles is a BOUND, not a curve fit. On a crossed range the product has a
// low ceiling and min() does not: for Layer 49 the largest weight the product can ever produce
// is 0.243 (at the crossing luma 103), while min() reaches 0.493. Recovering, per pixel, the
// weight that reproduces the file's own stored composite gives 0.44 at backdrop luma 95, and
// 37.3% of the usable samples need more weight than the product's ceiling (max recovered 0.540,
// i.e. more than twice the ceiling) - the product is impossible there, whatever the ramp shape.
// Binned by the backdrop luma the range is keyed on, the recovered weight tracks min(lo, hi) to
// a mean absolute error of 0.0083 over the whole 0..255 sweep, against 0.0466 for the product;
// for the second crossing layer (Layer 24, dst 71/128/32/183, opacity 125) 0.0100 against
// 0.0212, with 22.9% of samples above the product's ceiling. The third crossing (Layer 46, dst
// 82/114/102/136) crosses so shallowly (ceilings 0.286 vs 0.234 after opacity) that it cannot
// tell the rules apart, and indeed prefers neither (0.0092 vs 0.0090) - it is recorded here as
// the control that shows the method reports "no preference" when there is nothing to see.
// Scored over every range layer at once on effect-free pixels, min() reproduces the composite
// at mean 0.8461 (max-channel, 0-255) against 3.2933 for the product; 0.85 is the 8-bit floor
// for a stack this deep (see tools/psd_composite_probe/BLEND_IF_ASSESSMENT.md 14).
//
// Limits of this evidence, stated so nobody has to guess how far it reaches: it is ONE file.
// Only 1 of the 183 PSDs available on this machine has a crossed range at all (the other 182
// have no non-default range, or none at all), and the implementations that could corroborate it
// from outside refuse the case rather than model it - so this is Photoshop's own output measured,
// not documentation read.
float blendIfRangeWeight(float aValue255, const BlendIf::Range& aRange) {
    const float lo = (aRange.blackHi > aRange.blackLo)
        ? std::min(1.0f, std::max(0.0f,
              (aValue255 - aRange.blackLo) / (float)(aRange.blackHi - aRange.blackLo)))
        : (aValue255 >= (float)aRange.blackLo ? 1.0f : 0.0f);
    const float hi = (aRange.whiteHi > aRange.whiteLo)
        ? std::min(1.0f, std::max(0.0f,
              (aRange.whiteHi - aValue255) / (float)(aRange.whiteHi - aRange.whiteLo)))
        : (aValue255 <= (float)aRange.whiteHi ? 1.0f : 0.0f);
    return std::min(lo, hi);
}

float blendIfSourceWeight(const BlendIf& aBlendIf, uint8_t aR, uint8_t aG, uint8_t aB) {
    if (aBlendIf.src.isDefault()) {
        return 1.0f;
    }
    // Rec.601, the same luma the shader's BlendLum uses
    const float luma = 0.3f * aR + 0.59f * aG + 0.11f * aB;
    return blendIfRangeWeight(luma, aBlendIf.src);
}

namespace {

QVector4D toUniform(const BlendIf::Range& aRange) {
    const float s = 1.0f / 255.0f;
    return QVector4D(
        aRange.blackLo * s, aRange.blackHi * s, aRange.whiteLo * s, aRange.whiteHi * s);
}

} // namespace

void applyBlendIfUniforms(gl::EasyShaderProgram& aShader, const BlendIf& aBlendIf) {
    int enable = 0;
    if (!aBlendIf.dst.isDefault()) {
        enable |= 1;
    }
    if (!aBlendIf.src.isDefault()) {
        enable |= 2;
    }

    aShader.setUniformValue("uBlendIfDst", toUniform(aBlendIf.dst));
    aShader.setUniformValue("uBlendIfSrc", toUniform(aBlendIf.src));
    aShader.setUniformValue("uBlendIfEnable", enable);
}

} // namespace core
