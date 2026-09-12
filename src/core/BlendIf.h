#ifndef CORE_BLENDIF_H
#define CORE_BLENDIF_H

#include <cstdint>
#include <QVector4D>

namespace gl {
class EasyShaderProgram;
}

namespace core {

// Photoshop "Blend If" (the layer record's blending-ranges section).
//
// Two split-slider ranges restrict a layer per pixel on the value of the layer's own
// pixels ("This Layer", src) and of the backdrop ("Underlying Layer", dst). Each range is
// the 4 bytes Photoshop stores, in file order: blackLo, blackHi, whiteLo, whiteHi, where
// blackLo -> blackHi is the rising edge and whiteLo -> whiteHi the falling one, so the
// layer reaches full strength on [blackHi, whiteLo]. A default range (0, 0, 255, 255)
// applies the layer everywhere; the degenerate crossed range (255, 255 / 0, 0) suppresses it
// entirely, because no value satisfies both edges at once.
//
// The two edges of ONE slider combine with min(), NOT by multiplying: the two rules agree on
// every ordinary range (the ramps never dip in the same place) and differ only when the
// handles cross (blackHi > whiteLo), where the product under-weights the band. Measured on
// this repository's 114 MB PSD, whose 7 layers with a non-default range include 3 that set a
// genuinely crossed destination range (and one degenerate (255,255,0,0) that is not a crossing):
// min() reproduces that file's own stored composite at mean 0.8461 max-channel on effect-free
// pixels, against 3.2933 for the product, and on the worst crossing the product's ceiling (0.243)
// is below the weight the composite requires (0.44). See core/BlendIf.cpp and
// BLEND_IF_ASSESSMENT.md §14.
//
// The byte order and the trapezoid shape are not guesses: an independent compositor using
// exactly this interpretation reproduces a Photoshop-authored 57-layer file's own stored
// composite at mean 8.4/255, while swapping the white pair (the only plausible alternative
// reading) degrades that to 19.9, and applying no weights at all gives 42.4. The gray
// (composite) range is what real color-grading layers use; Photoshop's per-channel groups
// are not modelled here (the PSD reader keeps them available in PSDFormat if needed).
//
// The range values compare against 8-bit sRGB-encoded channel values evaluated with
// Rec.601 luma, which is the space the renderer's framebuffers already hold (nothing
// enables GL_FRAMEBUFFER_SRGB) and matches the shader's BlendLum used by the
// non-separable blends.
class BlendIf {
public:
    struct Range {
        uint8_t blackLo = 0;
        uint8_t blackHi = 0;
        uint8_t whiteLo = 255;
        uint8_t whiteHi = 255;

        // true when the range restricts nothing (the PSD default)
        bool isDefault() const {
            return blackLo == 0 && blackHi == 0 && whiteLo == 255 && whiteHi == 255;
        }

        // the project format stores one range per 32-bit word (Serializer has no byte
        // overload); the byte order inside the word is the PSD's
        int packed() const {
            return (int)blackLo | ((int)blackHi << 8) | ((int)whiteLo << 16) | ((int)whiteHi << 24);
        }
        bool operator==(const Range& aRhs) const {
            return blackLo == aRhs.blackLo && blackHi == aRhs.blackHi && whiteLo == aRhs.whiteLo
                && whiteHi == aRhs.whiteHi;
        }
        bool operator!=(const Range& aRhs) const { return !(*this == aRhs); }

        static Range unpacked(int aPacked) {
            Range r;
            r.blackLo = (uint8_t)(aPacked & 0xff);
            r.blackHi = (uint8_t)((aPacked >> 8) & 0xff);
            r.whiteLo = (uint8_t)((aPacked >> 16) & 0xff);
            r.whiteHi = (uint8_t)((aPacked >> 24) & 0xff);
            return r;
        }
    };

    // "Underlying Layer" (the backdrop) and "This Layer" (the layer's own pixels)
    Range dst;
    Range src;

    bool isActive() const { return !dst.isDefault() || !src.isDefault(); }

    bool operator==(const BlendIf& aRhs) const {
        return dst.blackLo == aRhs.dst.blackLo && dst.blackHi == aRhs.dst.blackHi
            && dst.whiteLo == aRhs.dst.whiteLo && dst.whiteHi == aRhs.dst.whiteHi
            && src.blackLo == aRhs.src.blackLo && src.blackHi == aRhs.src.blackHi
            && src.whiteLo == aRhs.src.whiteLo && src.whiteHi == aRhs.src.whiteHi;
    }
    bool operator!=(const BlendIf& aRhs) const { return !(*this == aRhs); }
};

// CPU side of the shader's trapezoid: the weight (0..1) one range gives an 8-bit channel
// value. Kept beside the GLSL version so authoring commands (see LayerMaskBake) use the same
// semantics; the verification harness has its own independent replica on purpose.
float blendIfRangeWeight(float aValue255, const BlendIf::Range& aRange);

// Weight of one straight 8-bit RGB pixel under the source range (Rec.601 luma).
float blendIfSourceWeight(const BlendIf& aBlendIf, uint8_t aR, uint8_t aG, uint8_t aB);

// Uploads the ranges to the layer drawing shader: uBlendIfDst / uBlendIfSrc carry the
// thresholds normalized to 0..1 and uBlendIfEnable masks which of the two are active
// (bit 0 = dst, bit 1 = src). The uniforms belong to the linked program, so this must be
// called for every draw of that program - an inactive BlendIf disables the weight.
void applyBlendIfUniforms(gl::EasyShaderProgram& aShader, const BlendIf& aBlendIf);

} // namespace core

#endif // CORE_BLENDIF_H
