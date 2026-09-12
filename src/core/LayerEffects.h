#ifndef CORE_LAYEREFFECTS_H
#define CORE_LAYEREFFECTS_H

#include <cstdint>
#include <vector>
#include <QString>
#include "img/BlendMode.h"
#include "img/PSDDescriptor.h"

namespace gl {
class EasyShaderProgram;
}

namespace core {

// Photoshop layer effects (the PSD "lfx2" descriptor), reduced to what the renderer can draw
// today.
//
// Applied:
//  - the two effects that REPLACE the layer's fill colour and need no silhouette pass —
//    colour overlay and gradient overlay (linear);
//  - the silhouette family — drop shadow, outer glow and chrome — which blur a copy of the
//    layer's SHAPE, tint it with the effect colour, offset it by distance/angle and composite
//    it under the layer (shadow, glow) or over it (chrome) with the effect's own blend mode.
//    See core/LayerNode.cpp's fPresentSilhouettes and FilterFrame::Kind_Tint;
//  - the STROKE — the band of `size` pixels inside the layer's outline, painted with a solid
//    colour or a gradient and blended with the layer's pixels (see Stroke below). It is a
//    function of the layer's outline distance, which travels with the resource as an A8 plane
//    (core/DistanceField.h) and is sampled in the content shader. A stroke whose band cannot be
//    measured (no field on the resource, an outside/centre position, a pattern paint) is
//    reported through isDrawnAsConfigured() instead of being drawn with a wrong band.
//
// Recognised but NOT drawn (parseLayerEffects still records them and isDrawnAsConfigured()
// returns false while any of them is enabled, so a caller can say the picture is not the file's;
// ctrl::undrawnEffectNotes is the sentence, and the import and reload paths both push it to the
// app log): bevel/emboss, satin, inner glow, inner shadow.
//
// The reason is per effect, not one shared gap - an earlier revision of this comment claimed a
// missing "distance transform of the layer's alpha", which was true when it was written and is
// not any more: core/DistanceField.h exists and the stroke already samples it. What is actually
// missing:
//  - BEVEL/EMBOSS needs a LIGHTING pass that no other effect here needs: a height field from
//    the softened alpha, its gradient as a surface normal, and a highlight and a shadow shaded
//    from the style's angle/altitude/depth, each with its own blend mode. Nothing in the
//    renderer shades a normal today, which is why this is the one gap that is architectural.
//  - SATIN is a pair of blurred, offset copies of the alpha combined with each other (one
//    inverted) and then blended into the layer's own pixels with the style's mode: a
//    fill-modulating pass like the overlays, but built from two blurs rather than one shape.
//  - INNER GLOW and INNER SHADOW are the silhouette family read from the INSIDE: the same
//    blur/tint/offset as the chrome path, clipped to the layer's own shape. The mechanism
//    exists (Silhouette::isInner, FilterFrame's inner presentation), so these are the cheapest
//    of the four.
//
// Why they are still not written: no file available here exercises any of them. Of the 183 PSDs
// on this machine, 4 carry a layer-effects block at all; across them satin, inner glow and inner
// shadow appear ZERO times with their enable flag set, bevel 3 times (all in the 114 MB test
// file, whose whole remaining >32-level residual sits inside those three layers), and strokes 25
// times - 22 of which are outside/centre bands, which this build does not draw either (an outside
// band lies beyond the layer's own support, so it is a dilation of the shape rather than a
// distance band inside it; those 22 are reported by name, with their position and paint type).
// An implementation with nothing to verify it against is a guess, and this build does not guess -
// so they are reported, not approximated. Bevel is the one with real evidence to check against,
// and it is the honest next piece of work.
//
// Blend modes: the overlays are applied as straight colour replacements, i.e. Normal, so an
// overlay whose own mode is something else is reported through isDrawnAsConfigured(). A
// silhouette is presented with its own mode and therefore accepts any mode this renderer can
// name (see effectBlendMode); an unreadable key is reported rather than silently Normal.
// the runtime blend code the layer shader's fxBlend() switches on, for the styles whose
// blend mode has to be a uniform value (the stroke's own mode). -1 = a mode this renderer
// cannot express, which the caller reports instead of drawing.
int effectBlendCode(img::BlendMode aMode);

// Photoshop's layer-effects blend mode enum ("Md  " items of type "BlnM"). These are
// CAPITALIZED 4CCs of the layer-record keys ('Nrml' = normal, 'Scrn' = screen, 'CBrn' =
// color burn, ...), which is why the raw layer-record table (img::getBlendModeFromPSD)
// cannot read them. Both files in this repository's corpus that carry effects (the
// 114 MB PSD uses 'Scrn' and 'Nrml') write this encoding, and 'CBrn' = color burn is
// documented by psd-tools' own pattern-fill descriptor example. The layer-record keys are
// accepted as well, so a caller never has to know which encoding a file used.
// aOutKnown (optional) reports whether the key named a mode this renderer can APPLY: an
// unknown key (or Dissolve, which has no per-pixel formula) must not silently become
// Normal, or a wrong picture would look like a right one. The returned mode is still
// Normal in that case, so drawing can continue with a reported approximation.
img::BlendMode effectBlendMode(const std::string& aKey, bool* aOutKnown = nullptr);

class LayerEffects {
public:
    // the host of colour stops the shader can take (see LayerDrawingFrag's stop arrays)
    enum { kMaxStops = 8 };

    enum Type {
        Type_ColorOverlay,
        Type_GradientOverlay,
        Type_Bevel,
        Type_Stroke,
        Type_Chrome,
        Type_Satin,
        Type_InnerGlow,
        Type_OuterGlow,
        Type_DropShadow,
        Type_InnerShadow,
        Type_TERM
    };

    struct Stop {
        float location = 0.0f; // 0..1
        float r = 0.0f, g = 0.0f, b = 0.0f; // 0..1
        float opacity = 1.0f;  // 0..1
    };

    // colour overlay: a solid fill replacement
    struct ColorOverlay {
        bool enabled = false;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        float opacity = 1.0f;
        std::string blendMode; // the file's key, for reporting
        // the renderer applies the fill replacements as Normal only; a different mode is
        // parsed and reported through isDrawnAsConfigured()
        img::BlendMode mode = img::BlendMode_Normal;
        bool blendModeKnown = true;
    };

    // gradient overlay (linear only in the renderer; other Photoshop styles are parsed and
    // reported so the import log says so instead of silently drawing the wrong thing)
    struct GradientOverlay {
        bool enabled = false;
        float opacity = 1.0f;
        float angleDeg = 90.0f;   // Photoshop's gradient angle; 90 puts the first stop at the bottom
        float scale = 100.0f;     // percent of the layer extent
        float offsetX = 0.0f;     // fractions of the layer size
        float offsetY = 0.0f;
        bool reverse = false;
        std::string style;        // the PSD enum's value key, e.g. "Lnr " (linear)
        std::string blendMode;    // the file's key, for reporting
        img::BlendMode mode = img::BlendMode_Normal;
        bool blendModeKnown = true;
        std::vector<Stop> stops;  // merged colour + transparency stops, sorted by location

        bool isLinear() const { return style == "Lnr "; }
        bool isValid() const { return enabled && stops.size() >= 2; }
    };

    // An effect built from a blurred copy of the layer's silhouette: Photoshop's drop shadow,
    // outer glow and chrome all blur the layer's alpha, optionally offset it, tint it and
    // composite it (behind the layer for shadows/glows, over it for chrome). The renderer
    // draws these with a real Gaussian pass through core/FilterFrame, so they follow the
    // layer like the other effects rather than being baked into its pixels.
    struct Silhouette {
        enum Kind { Kind_DropShadow, Kind_OuterGlow, Kind_Chrome, Kind_TERM };

        bool enabled = false;
        Kind kind = Kind_OuterGlow;
        float blur = 0.0f;       // pixels
        float distance = 0.0f;   // pixels
        float angleDeg = 0.0f;   // Photoshop's angle (counter-clockwise, 0 points right)
        float r = 0.0f, g = 0.0f, b = 0.0f; // tint
        float opacity = 1.0f;
        std::string blendMode;   // the file's key ("Scrn", "Nrml", ...), for reporting
        img::BlendMode mode = img::BlendMode_Normal; // presented against the backdrop
        bool blendModeKnown = true;

        bool isUnder() const { return kind == Kind_DropShadow || kind == Kind_OuterGlow; }
        // Chrome is INNER: Photoshop's "ChFX" is the chrome layer of Bevel & Emboss, so it
        // paints inside the layer's own shape and never spills a halo outside it. A drop shadow
        // and an outer glow are outer effects and do extend beyond the shape. Measured on the
        // 114 MB test file: treating the chrome as an outer silhouette made our render 19-24
        // levels brighter than the file's own composite in the ~120 k px ring around the glyphs,
        // exactly where the unclipped blurred copy spills.
        bool isInner() const { return kind == Kind_Chrome; }
        bool isUsable() const { return enabled && blur > 0.0f && opacity > 0.0f; }
    };

    // Photoshop's STROKE ("FrFX"): the band of `size` pixels inside the layer's outline,
    // painted with a solid colour or a gradient and blended with the layer's own pixels
    // above the fill-replacing styles (that is Photoshop's stacking: ... overlay, stroke).
    // It is a function of the layer's outline distance (core/DistanceField.h), so a layer
    // with a stroke carries that field with its resource; a layer without one is reported
    // rather than drawn with a wrong band.
    struct Stroke {
        bool enabled = false;
        float size = 0.0f;      // px, the band's width INSIDE the outline
        std::string style;      // "InsF" inside (drawn), "OutF"/"CtrF" (reported)
        // "SClr" solid, "GrFl" gradient, "Ptrn" pattern (reported). The solid value is
        // Photoshop's: the "PntT" enum of a stroke is (type "FrFl", value "SClr") in every
        // stroke of every file examined, and "SClr" is what the corpus's 32 solid strokes
        // carry. A build of this file tested "SCol" instead, a key no producer writes, so
        // every solid-colour stroke was misfiled as neither solid nor gradient and dropped -
        // silently, because the harness's own stroke helper wrote the same wrong key.
        std::string paintType;
        float r = 0.0f, g = 0.0f, b = 0.0f; // the solid colour (Photoshop keeps it for GrFl too)
        float opacity = 1.0f;
        std::string blendMode;
        img::BlendMode mode = img::BlendMode_Normal;
        bool blendModeKnown = true;
        // gradient paint, with the same anchoring parameters as the gradient overlay
        float angleDeg = 90.0f;
        float scale = 100.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        bool reverse = false;
        std::vector<Stop> stops;

        bool isInside() const { return style.empty() || style == "InsF"; }
        bool isSolid() const { return paintType.empty() || paintType == "SClr"; }
        bool isGradient() const { return paintType == "GrFl"; }
        bool isUsable() const { return enabled && size > 0.0f && opacity > 0.0f; }
        // what the renderer can draw as configured: an inside band, painted with a solid
        // colour or a gradient it has, with a blend mode it can name
        bool isDrawable() const;
    };

    ColorOverlay colorOverlay;
    GradientOverlay gradientOverlay;
    Stroke stroke;
    std::vector<Silhouette> silhouettes;

    bool hasSilhouettes() const {
        for (const auto& s : silhouettes) {
            if (s.isUsable()) {
                return true;
            }
        }
        return false;
    }

    // every effect key the block contained, enabled or not (for the UI / import log)
    bool seen[Type_TERM] = {};
    // the subset that is actually switched on in the file
    bool active[Type_TERM] = {};

    bool isActive() const {
        return colorOverlay.enabled || gradientOverlay.isValid() || hasSilhouettes() || stroke.isUsable();
    }
    bool isDrawnAsConfigured() const; // false when an active effect is outside what we draw

    std::vector<Type> presentTypes() const;
    std::vector<Type> activeTypes() const; // enabled effects this build does not draw
    static QString typeName(Type aType);
    static QString silhouetteName(Silhouette::Kind aKind);

    bool operator==(const LayerEffects& aRhs) const;
    bool operator!=(const LayerEffects& aRhs) const { return !(*this == aRhs); }
};

// Reads the effects this model covers out of an lfx2 descriptor. Returns false when the
// descriptor has no effect keys at all (an empty/absent block).
bool parseLayerEffects(const img::PSDDescriptor& aDescriptor, LayerEffects& aOut);

// Uploads the effects the renderer draws to the layer shader (uFxEnable and friends). Like
// the Blend If uniforms these belong to the linked program, so every draw states them: an
// inactive LayerEffects disables them.
void applyEffectsUniforms(gl::EasyShaderProgram& aShader, const LayerEffects& aEffects);

} // namespace core

#endif // CORE_LAYEREFFECTS_H
