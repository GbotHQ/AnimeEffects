#include <algorithm>
#include <cmath>
#include <QVector2D>
#include <QVector4D>
#include "gl/EasyShaderProgram.h"
#include "core/LayerEffects.h"

namespace core {

namespace {

using Value = img::PSDDescriptor::Value;

float numberOr(const Value* aOwner, const char* aKey, float aDefault) {
    const Value* v = aOwner ? aOwner->find(aKey) : nullptr;
    return v ? (float)v->asNumber(aDefault) : aDefault;
}

bool boolOr(const Value* aOwner, const char* aKey, bool aDefault) {
    const Value* v = aOwner ? aOwner->find(aKey) : nullptr;
    return v ? v->asBool() : aDefault;
}

std::string enumOr(const Value* aOwner, const char* aKey) {
    const Value* v = aOwner ? aOwner->find(aKey) : nullptr;
    return v ? v->asEnumValue() : std::string();
}

// "Clr " is {Rd, Grn, Bl} in 0..255
void readColor(const Value* aColor, float& aR, float& aG, float& aB) {
    aR = numberOr(aColor, "Rd  ", 0.0f) / 255.0f;
    aG = numberOr(aColor, "Grn ", 0.0f) / 255.0f;
    aB = numberOr(aColor, "Bl  ", 0.0f) / 255.0f;
}

// merges a gradient descriptor's colour and transparency stops into one sorted stop list
// (both the gradient overlay and a gradient stroke use it)
void readStops(const Value* aGrad, std::vector<LayerEffects::Stop>& aOut) {
    aOut.clear();
    if (!aGrad) {
        return;
    }

    struct ColorStop {
        float location;
        float r, g, b;
    };
    struct AlphaStop {
        float location;
        float opacity;
    };
    std::vector<ColorStop> colors;
    std::vector<AlphaStop> alphas;
    std::vector<float> locations;

    if (const Value* clrs = aGrad->find("Clrs")) {
        for (size_t i = 0; i < clrs->size(); ++i) {
            const Value* s = clrs->at(i);
            if (!s) {
                continue;
            }
            ColorStop stop{};
            stop.location = numberOr(s, "Lctn", 0.0f) / 4096.0f;
            readColor(s->find("Clr "), stop.r, stop.g, stop.b);
            colors.push_back(stop);
            locations.push_back(stop.location);
        }
    }
    if (const Value* trns = aGrad->find("Trns")) {
        for (size_t i = 0; i < trns->size(); ++i) {
            const Value* s = trns->at(i);
            if (!s) {
                continue;
            }
            AlphaStop stop{};
            stop.location = numberOr(s, "Lctn", 0.0f) / 4096.0f;
            stop.opacity = numberOr(s, "Opct", 100.0f) / 100.0f;
            alphas.push_back(stop);
            locations.push_back(stop.location);
        }
    }
    if (colors.empty()) {
        return;
    }

    std::sort(locations.begin(), locations.end());
    locations.erase(std::unique(locations.begin(), locations.end()), locations.end());
    std::sort(colors.begin(), colors.end(), [](const ColorStop& a, const ColorStop& b) { return a.location < b.location; });
    std::sort(alphas.begin(), alphas.end(), [](const AlphaStop& a, const AlphaStop& b) { return a.location < b.location; });

    const auto sampleColor = [&](float aLocation, float aOut[3]) {
        if (aLocation <= colors.front().location) {
            aOut[0] = colors.front().r;
            aOut[1] = colors.front().g;
            aOut[2] = colors.front().b;
            return;
        }
        if (aLocation >= colors.back().location) {
            aOut[0] = colors.back().r;
            aOut[1] = colors.back().g;
            aOut[2] = colors.back().b;
            return;
        }
        for (size_t i = 1; i < colors.size(); ++i) {
            if (aLocation <= colors[i].location) {
                const float span = colors[i].location - colors[i - 1].location;
                const float t = span > 0.0f ? (aLocation - colors[i - 1].location) / span : 0.0f;
                aOut[0] = colors[i - 1].r + (colors[i].r - colors[i - 1].r) * t;
                aOut[1] = colors[i - 1].g + (colors[i].g - colors[i - 1].g) * t;
                aOut[2] = colors[i - 1].b + (colors[i].b - colors[i - 1].b) * t;
                return;
            }
        }
    };
    const auto sampleAlpha = [&](float aLocation) {
        if (alphas.empty()) {
            return 1.0f;
        }
        if (aLocation <= alphas.front().location) {
            return alphas.front().opacity;
        }
        if (aLocation >= alphas.back().location) {
            return alphas.back().opacity;
        }
        for (size_t i = 1; i < alphas.size(); ++i) {
            if (aLocation <= alphas[i].location) {
                const float span = alphas[i].location - alphas[i - 1].location;
                const float t = span > 0.0f ? (aLocation - alphas[i - 1].location) / span : 0.0f;
                return alphas[i - 1].opacity + (alphas[i].opacity - alphas[i - 1].opacity) * t;
            }
        }
        return 1.0f;
    };

    for (const float location : locations) {
        LayerEffects::Stop stop;
        stop.location = location;
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        sampleColor(location, rgb);
        stop.r = rgb[0];
        stop.g = rgb[1];
        stop.b = rgb[2];
        stop.opacity = sampleAlpha(location);
        aOut.push_back(stop);
        if (aOut.size() >= LayerEffects::kMaxStops) {
            break; // the shader takes a fixed number of stops
        }
    }
    if (aOut.size() == 1) {
        aOut.push_back(aOut.front()); // a single stop is a solid fill
    }
}

// the gradient parameters (angle/scale/offset/reverse) are shared by the gradient overlay
// and a gradient stroke; both descriptors name them the same way
void readGradientCommon(const Value* aEffect, float& aAngleDeg, float& aScale, bool& aReverse, float& aOffsetX,
                        float& aOffsetY) {
    aAngleDeg = numberOr(aEffect, "Angl", 90.0f);
    aScale = numberOr(aEffect, "Scl ", 100.0f);
    aReverse = boolOr(aEffect, "Rvrs", false);
    if (const Value* ofst = aEffect->find("Ofst")) {
        // Photoshop stores these as fractions of the layer size (the sign convention is
        // not verified against a reference render; 0 in the file this targets)
        aOffsetX = numberOr(ofst, "Hrzn", 0.0f);
        aOffsetY = numberOr(ofst, "Vrtc", 0.0f);
    }
}

void readGradient(const Value* aGrad, LayerEffects::GradientOverlay& aOut) {
    readStops(aGrad, aOut.stops);
}

} // namespace

int effectBlendCode(img::BlendMode aMode) {
    switch (aMode) {
    case img::BlendMode_Normal: return 0;
    case img::BlendMode_Multiply: return 1;
    case img::BlendMode_Screen: return 2;
    case img::BlendMode_Overlay: return 3;
    case img::BlendMode_Darken: return 4;
    case img::BlendMode_Lighten: return 5;
    case img::BlendMode_ColorDodge: return 6;
    case img::BlendMode_ColorBurn: return 7;
    case img::BlendMode_LinearDodge: return 8;
    case img::BlendMode_LinearBurn: return 9;
    case img::BlendMode_SoftLight: return 10;
    case img::BlendMode_HardLight: return 11;
    case img::BlendMode_Difference: return 12;
    case img::BlendMode_Exclusion: return 13;
    default: return -1; // no fxBlend branch: reported, never silently drawn as Normal
    }
}

img::BlendMode effectBlendMode(const std::string& aKey, bool* aOutKnown) {
    struct Entry {
        const char* key;
        img::BlendMode mode;
    };
    // the capitalized effect enum keys, then the layer-record keys they come from
    static const Entry kTable[] = {
        {"Nrml", img::BlendMode_Normal},        // Dissolve is stochastic and has no
        // per-pixel formula, so it is reported rather than drawn (see kNotDrawable below)
        {"Dslv", img::BlendMode_Normal},
        {"Drkn", img::BlendMode_Darken},        {"Mltp", img::BlendMode_Multiply},
        {"CBrn", img::BlendMode_ColorBurn},     {"LBrn", img::BlendMode_LinearBurn},
        {"Drkr", img::BlendMode_DarkerColor},   {"Lghn", img::BlendMode_Lighten},
        {"Scrn", img::BlendMode_Screen},        {"CDdg", img::BlendMode_ColorDodge},
        {"LDdg", img::BlendMode_LinearDodge},   {"Lghr", img::BlendMode_LighterColor},
        {"Ovrl", img::BlendMode_Overlay},       {"SftL", img::BlendMode_SoftLight},
        {"HrdL", img::BlendMode_HardLight},     {"vLit", img::BlendMode_VividLight},
        {"lLit", img::BlendMode_LinearLight},   {"pLit", img::BlendMode_PinLight},
        {"hMix", img::BlendMode_HardMix},       {"diff", img::BlendMode_Difference},
        {"Xclu", img::BlendMode_Exclusion},     {"fsub", img::BlendMode_Subtract},
        {"fdiv", img::BlendMode_Divide},        {"hue ", img::BlendMode_Hue},
        {"sat ", img::BlendMode_Saturation},    {"colr", img::BlendMode_Color},
        {"lum ", img::BlendMode_Luminosity},
        // the layer-record spellings (img::getBlendModeFromPSD's table)
        {"norm", img::BlendMode_Normal},        {"diss", img::BlendMode_Normal}, // Dissolve: see the Dslv note above
        {"dark", img::BlendMode_Darken},        {"mul ", img::BlendMode_Multiply},
        {"idiv", img::BlendMode_ColorBurn},     {"lbrn", img::BlendMode_LinearBurn},
        {"dkCl", img::BlendMode_DarkerColor},   {"lite", img::BlendMode_Lighten},
        {"scrn", img::BlendMode_Screen},        {"div ", img::BlendMode_ColorDodge},
        {"lddg", img::BlendMode_LinearDodge},   {"lgCl", img::BlendMode_LighterColor},
        {"over", img::BlendMode_Overlay},       {"sLit", img::BlendMode_SoftLight},
        {"hLit", img::BlendMode_HardLight},     {"vLit", img::BlendMode_VividLight},
        {"lLit", img::BlendMode_LinearLight},   {"pLit", img::BlendMode_PinLight},
        {"hMix", img::BlendMode_HardMix},       {"diff", img::BlendMode_Difference},
        {"smud", img::BlendMode_Exclusion},
    };

    // keys that name a Photoshop mode this renderer cannot express (Dissolve is
    // stochastic): the caller gets Normal plus "not known", i.e. the picture is not the
    // file's, which is what isDrawnAsConfigured() reports
    static const char* kNotDrawable[] = {"Dslv", "diss"};

    if (aOutKnown) {
        *aOutKnown = true;
    }
    if (aKey.empty()) {
        return img::BlendMode_Normal; // an absent key is the documented default
    }
    for (const char* k : kNotDrawable) {
        if (aKey == k) {
            if (aOutKnown) {
                *aOutKnown = false;
            }
            return img::BlendMode_Normal;
        }
    }
    for (const auto& e : kTable) {
        if (aKey == e.key) {
            return e.mode;
        }
    }
    if (aOutKnown) {
        *aOutKnown = false;
    }
    return img::BlendMode_Normal;
}

bool LayerEffects::Stroke::isDrawable() const {
    if (!isUsable()) {
        return false;
    }
    if (!isInside() || !blendModeKnown || effectBlendCode(mode) < 0) {
        return false;
    }
    return isSolid() || (isGradient() && stops.size() >= 2);
}

QString LayerEffects::silhouetteName(Silhouette::Kind aKind) {
    switch (aKind) {
    case Silhouette::Kind_DropShadow: return QStringLiteral("drop shadow");
    case Silhouette::Kind_OuterGlow: return QStringLiteral("outer glow");
    case Silhouette::Kind_Chrome: return QStringLiteral("chrome");
    default: return QStringLiteral("?");
    }
}

QString LayerEffects::typeName(Type aType) {
    switch (aType) {
    case Type_ColorOverlay: return QStringLiteral("Color Overlay");
    case Type_GradientOverlay: return QStringLiteral("Gradient Overlay");
    case Type_Bevel: return QStringLiteral("Bevel and Emboss");
    case Type_Stroke: return QStringLiteral("Stroke");
    case Type_Chrome: return QStringLiteral("Chrome");
    case Type_Satin: return QStringLiteral("Satin");
    case Type_InnerGlow: return QStringLiteral("Inner Glow");
    case Type_OuterGlow: return QStringLiteral("Outer Glow");
    case Type_DropShadow: return QStringLiteral("Drop Shadow");
    case Type_InnerShadow: return QStringLiteral("Inner Shadow");
    default: return QString();
    }
}

std::vector<LayerEffects::Type> LayerEffects::presentTypes() const {
    std::vector<Type> out;
    for (int i = 0; i < Type_TERM; ++i) {
        if (seen[i]) {
            out.push_back((Type)i);
        }
    }
    return out;
}

std::vector<LayerEffects::Type> LayerEffects::activeTypes() const {
    std::vector<Type> out;
    for (int i = 0; i < Type_TERM; ++i) {
        if (active[i]) {
            out.push_back((Type)i);
        }
    }
    return out;
}

bool LayerEffects::isDrawnAsConfigured() const {
    const auto isPlainNormal = [](img::BlendMode aMode, bool aKnown) {
        return aKnown && aMode == img::BlendMode_Normal;
    };
    if (colorOverlay.enabled && !isPlainNormal(colorOverlay.mode, colorOverlay.blendModeKnown)) {
        return false;
    }
    if (gradientOverlay.enabled
        && (!gradientOverlay.isLinear() || !isPlainNormal(gradientOverlay.mode, gradientOverlay.blendModeKnown))) {
        return false;
    }
    // an enabled stroke this build cannot draw (outside/centre position, a pattern paint, an
    // unnameable blend mode, a gradient with fewer than two stops) is reported
    if (stroke.enabled && !stroke.isDrawable()) {
        return false;
    }
    for (const auto& s : silhouettes) {
        // a silhouette is presented with its own blend mode, so any mode we can name is
        // faithful; an unreadable key must be reported rather than silently Normal
        if (s.isUsable() && !s.blendModeKnown) {
            return false;
        }
    }
    // an enabled effect of the silhouette family with nothing to draw (the file carries no
    // blur for it) is not drawn either, and claiming otherwise would hide that
    const auto hasKind = [this](Silhouette::Kind aKind) {
        for (const auto& s : silhouettes) {
            if (s.isUsable() && s.kind == aKind) {
                return true;
            }
        }
        return false;
    };
    if (active[Type_Chrome] && !hasKind(Silhouette::Kind_Chrome)) {
        return false;
    }
    if (active[Type_OuterGlow] && !hasKind(Silhouette::Kind_OuterGlow)) {
        return false;
    }
    if (active[Type_DropShadow] && !hasKind(Silhouette::Kind_DropShadow)) {
        return false;
    }
    for (int i = 0; i < Type_TERM; ++i) {
        // the stroke is drawn when the checks above accepted it, so it counts as drawn here
        const bool drawn = i == Type_GradientOverlay || i == Type_ColorOverlay || i == Type_Stroke
            || i == Type_Chrome || i == Type_OuterGlow || i == Type_DropShadow;
        if (active[i] && !drawn) {
            return false; // an enabled effect this build does not draw
        }
    }
    return true;
}

bool LayerEffects::operator==(const LayerEffects& aRhs) const {
    if (colorOverlay.enabled != aRhs.colorOverlay.enabled || gradientOverlay.enabled != aRhs.gradientOverlay.enabled
        || stroke.enabled != aRhs.stroke.enabled) {
        return false;
    }
    if (stroke.enabled) {
        if (stroke.size != aRhs.stroke.size || stroke.style != aRhs.stroke.style
            || stroke.paintType != aRhs.stroke.paintType || stroke.mode != aRhs.stroke.mode
            || stroke.stops.size() != aRhs.stroke.stops.size()
            || std::abs(stroke.opacity - aRhs.stroke.opacity) > 1e-6f
            || std::abs(stroke.angleDeg - aRhs.stroke.angleDeg) > 1e-6f
            || std::abs(stroke.scale - aRhs.stroke.scale) > 1e-6f
            || std::abs(stroke.r - aRhs.stroke.r) > 1e-6f || std::abs(stroke.g - aRhs.stroke.g) > 1e-6f
            || std::abs(stroke.b - aRhs.stroke.b) > 1e-6f) {
            return false;
        }
        for (size_t i = 0; i < stroke.stops.size(); ++i) {
            if (std::abs(stroke.stops[i].location - aRhs.stroke.stops[i].location) > 1e-6f
                || std::abs(stroke.stops[i].r - aRhs.stroke.stops[i].r) > 1e-6f
                || std::abs(stroke.stops[i].g - aRhs.stroke.stops[i].g) > 1e-6f
                || std::abs(stroke.stops[i].b - aRhs.stroke.stops[i].b) > 1e-6f) {
                return false;
            }
        }
    }
    if (colorOverlay.enabled) {
        const float a[4] = {colorOverlay.r, colorOverlay.g, colorOverlay.b, colorOverlay.opacity};
        const float b[4] = {aRhs.colorOverlay.r, aRhs.colorOverlay.g, aRhs.colorOverlay.b, aRhs.colorOverlay.opacity};
        for (int i = 0; i < 4; ++i) {
            if (std::abs(a[i] - b[i]) > 1e-6f) {
                return false;
            }
        }
    }
    if (gradientOverlay.enabled) {
        if (gradientOverlay.stops.size() != aRhs.gradientOverlay.stops.size()
            || gradientOverlay.style != aRhs.gradientOverlay.style) {
            return false;
        }
        const float a[4] = {gradientOverlay.opacity, gradientOverlay.angleDeg, gradientOverlay.scale,
                            gradientOverlay.reverse ? 1.0f : 0.0f};
        const float b[4] = {aRhs.gradientOverlay.opacity, aRhs.gradientOverlay.angleDeg, aRhs.gradientOverlay.scale,
                            aRhs.gradientOverlay.reverse ? 1.0f : 0.0f};
        for (int i = 0; i < 4; ++i) {
            if (std::abs(a[i] - b[i]) > 1e-6f) {
                return false;
            }
        }
        for (size_t i = 0; i < gradientOverlay.stops.size(); ++i) {
            const auto& sa = gradientOverlay.stops[i];
            const auto& sb = aRhs.gradientOverlay.stops[i];
            if (std::abs(sa.location - sb.location) > 1e-6f || std::abs(sa.r - sb.r) > 1e-6f
                || std::abs(sa.g - sb.g) > 1e-6f || std::abs(sa.b - sb.b) > 1e-6f
                || std::abs(sa.opacity - sb.opacity) > 1e-6f) {
                return false;
            }
        }
    }
    return true;
}

bool parseLayerEffects(const img::PSDDescriptor& aDescriptor, LayerEffects& aOut) {
    if (!aDescriptor.isValid() || !aDescriptor.root().isDescriptor()) {
        return false;
    }
    const Value& root = aDescriptor.root();

    struct Mapping {
        const char* key;
        LayerEffects::Type type;
    };
    const Mapping mappings[] = {
        {"SoCo", LayerEffects::Type_ColorOverlay},
        {"GrFl", LayerEffects::Type_GradientOverlay},
        {"ebbl", LayerEffects::Type_Bevel},
        {"FrFX", LayerEffects::Type_Stroke},
        {"ChFX", LayerEffects::Type_Chrome},
        {"SoFi", LayerEffects::Type_Satin},
        {"IrGl", LayerEffects::Type_InnerGlow},
        {"OrGl", LayerEffects::Type_OuterGlow},
        {"DrSh", LayerEffects::Type_DropShadow},
        {"IrSh", LayerEffects::Type_InnerShadow},
    };

    bool any = false;
    for (const Mapping& mapping : mappings) {
        const Value* effect = root.find(mapping.key);
        if (!effect || !effect->isDescriptor()) {
            continue;
        }
        any = true;
        aOut.seen[mapping.type] = true;
        if (!boolOr(effect, "enab", false)) {
            continue;
        }
        aOut.active[mapping.type] = true;

        // the blurred-silhouette family: same idea, different tint/offset/blend
        const auto addSilhouette = [&](LayerEffects::Silhouette::Kind aKind) {
            LayerEffects::Silhouette s;
            s.enabled = true;
            s.kind = aKind;
            s.blendMode = enumOr(effect, "Md  ");
            s.mode = effectBlendMode(s.blendMode, &s.blendModeKnown);
            s.opacity = numberOr(effect, "Opct", 100.0f) / 100.0f;
            s.blur = numberOr(effect, "blur", 0.0f);
            s.distance = numberOr(effect, "Dstn", 0.0f);
            s.angleDeg = numberOr(effect, "lagl", 120.0f); // Photoshop's default/global light
            readColor(effect->find("Clr "), s.r, s.g, s.b);
            if (s.blur > 0.0f) {
                aOut.silhouettes.push_back(s);
            }
        };
        if (mapping.type == LayerEffects::Type_Stroke) {
            auto& st = aOut.stroke;
            st.enabled = true;
            st.style = enumOr(effect, "Styl");
            st.paintType = enumOr(effect, "PntT");
            st.size = numberOr(effect, "Sz  ", 0.0f);
            st.opacity = numberOr(effect, "Opct", 100.0f) / 100.0f;
            st.blendMode = enumOr(effect, "Md  ");
            st.mode = effectBlendMode(st.blendMode, &st.blendModeKnown);
            readColor(effect->find("Clr "), st.r, st.g, st.b);
            readGradientCommon(effect, st.angleDeg, st.scale, st.reverse, st.offsetX, st.offsetY);
            readStops(effect->find("Grad"), st.stops);
            continue;
        }
        if (mapping.type == LayerEffects::Type_Chrome) {
            addSilhouette(LayerEffects::Silhouette::Kind_Chrome);
            continue;
        }
        if (mapping.type == LayerEffects::Type_OuterGlow) {
            addSilhouette(LayerEffects::Silhouette::Kind_OuterGlow);
            continue;
        }
        if (mapping.type == LayerEffects::Type_DropShadow) {
            // a drop shadow is drawn behind the layer, so its offset is negative
            addSilhouette(LayerEffects::Silhouette::Kind_DropShadow);
            if (!aOut.silhouettes.empty()) {
                aOut.silhouettes.back().distance = -aOut.silhouettes.back().distance;
            }
            continue;
        }

        if (mapping.type == LayerEffects::Type_ColorOverlay) {
            aOut.colorOverlay.enabled = true;
            aOut.colorOverlay.blendMode = enumOr(effect, "Md  ");
            aOut.colorOverlay.mode = effectBlendMode(aOut.colorOverlay.blendMode, &aOut.colorOverlay.blendModeKnown);
            readColor(effect->find("Clr "), aOut.colorOverlay.r, aOut.colorOverlay.g, aOut.colorOverlay.b);
            aOut.colorOverlay.opacity = numberOr(effect, "Opct", 100.0f) / 100.0f;
        } else if (mapping.type == LayerEffects::Type_GradientOverlay) {
            aOut.gradientOverlay.enabled = true;
            aOut.gradientOverlay.blendMode = enumOr(effect, "Md  ");
            aOut.gradientOverlay.mode
                = effectBlendMode(aOut.gradientOverlay.blendMode, &aOut.gradientOverlay.blendModeKnown);
            aOut.gradientOverlay.opacity = numberOr(effect, "Opct", 100.0f) / 100.0f;
            readGradientCommon(effect, aOut.gradientOverlay.angleDeg, aOut.gradientOverlay.scale,
                aOut.gradientOverlay.reverse, aOut.gradientOverlay.offsetX, aOut.gradientOverlay.offsetY);
            aOut.gradientOverlay.style = enumOr(effect, "Type");
            readGradient(effect->find("Grad"), aOut.gradientOverlay);
        }
    }
    return any;
}

} // namespace core

namespace core {

void applyEffectsUniforms(gl::EasyShaderProgram& aShader, const LayerEffects& aEffects) {
    int enable = 0;
    if (aEffects.colorOverlay.enabled) {
        enable |= 1;
    }
    if (aEffects.gradientOverlay.isValid()) {
        enable |= 2;
    }
    aShader.setUniformValue("uFxEnable", enable);
    aShader.setUniformValue("uFxColor",
        QVector4D(aEffects.colorOverlay.r, aEffects.colorOverlay.g, aEffects.colorOverlay.b,
            aEffects.colorOverlay.opacity));

    const auto& grad = aEffects.gradientOverlay;
    const float angleRad = (float)(grad.angleDeg * 3.14159265358979323846 / 180.0);
    aShader.setUniformValue("uFxGradParams",
        QVector4D(angleRad, std::max(grad.scale, 0.0f) * 0.01f, grad.reverse ? 1.0f : 0.0f, 0.0f));
    aShader.setUniformValue("uFxGradOffset", QVector2D(grad.offsetX, grad.offsetY));

    const int count = (int)std::min<size_t>(grad.stops.size(), LayerEffects::kMaxStops);
    aShader.setUniformValue("uFxStopCount", count);
    // the arrays have a fixed size in the shader, so fill every element: unset entries would
    // hold whatever the previous draw left there
    for (int i = 0; i < LayerEffects::kMaxStops; ++i) {
        const LayerEffects::Stop stop = i < count ? grad.stops[i] : grad.stops.empty()
                ? LayerEffects::Stop()
                : grad.stops.back();
        const QString name = QStringLiteral("uFxStopColor[%1]").arg(i);
        const QString alphaName = QStringLiteral("uFxStopAlpha[%1]").arg(i);
        aShader.setUniformValue(name.toUtf8().constData(), QVector4D(stop.r, stop.g, stop.b, stop.location));
        aShader.setUniformValue(alphaName.toUtf8().constData(), stop.opacity);
    }

    // the stroke's own uniforms. A layer without a drawable stroke states enable = 0, so the
    // shader's band pass never runs (the uniforms belong to the linked program).
    const auto& stroke = aEffects.stroke;
    const bool strokeOn = stroke.isDrawable();
    aShader.setUniformValue("uStrokeEnable", strokeOn ? 1 : 0);
    aShader.setUniformValue("uStrokeWidth", stroke.size);
    aShader.setUniformValue("uStrokeBlend", strokeOn ? effectBlendCode(stroke.mode) : 0);
    aShader.setUniformValue("uStrokeSolid", stroke.isSolid() ? 1 : 0);
    aShader.setUniformValue("uStrokeColor", QVector4D(stroke.r, stroke.g, stroke.b, stroke.opacity));
    const float strokeAngleRad = (float)(stroke.angleDeg * 3.14159265358979323846 / 180.0);
    aShader.setUniformValue("uStrokeGradParams",
        QVector4D(strokeAngleRad, std::max(stroke.scale, 0.0f) * 0.01f, stroke.reverse ? 1.0f : 0.0f, 0.0f));
    aShader.setUniformValue("uStrokeGradOffset", QVector2D(stroke.offsetX, stroke.offsetY));
    const int strokeStopCount = (int)std::min<size_t>(stroke.stops.size(), LayerEffects::kMaxStops);
    aShader.setUniformValue("uStrokeStopCount", strokeStopCount);
    for (int i = 0; i < LayerEffects::kMaxStops; ++i) {
        const LayerEffects::Stop stop = stroke.stops.empty()
            ? LayerEffects::Stop()
            : (i < strokeStopCount ? stroke.stops[i] : stroke.stops.back());
        const QString name = QStringLiteral("uStrokeStopColor[%1]").arg(i);
        const QString alphaName = QStringLiteral("uStrokeStopAlpha[%1]").arg(i);
        aShader.setUniformValue(name.toUtf8().constData(), QVector4D(stop.r, stop.g, stop.b, stop.location));
        aShader.setUniformValue(alphaName.toUtf8().constData(), stop.opacity);
    }
}

} // namespace core
