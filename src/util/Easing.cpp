#include "Easing.h"
#include <QtMath>
#include <algorithm>
#include "util/EasingName.h"
#include "util/UnitBezier.h"

namespace {
// Picker display order, mirrored by the Easing::display* helpers. The enum
// ordinal is serialization order; this is the order pickers show.
//
// The head is the four NON-CURVE entries - the two stepped holds, the straight
// line, and Automatic, whose shape is derived rather than picked - then the
// analytic curve family (Cubic, Sine, Quad, ...), then the drawn Custom curve.
// Auto used to sit last, just before Custom, which read as "the odd one out at
// the end" while it is really a sibling of Floor/Round/Linear: a type whose
// shape is not one of the family's parameters. The picker's tile catalog
// (EasingPickWidget.cpp's kTileOrder) carries the same head and puts its
// hairline separator right after Auto, so the first tile row is exactly these
// four and the family follows.
//
// Type_Custom stays LAST: the default-easing combo indexes this table directly
// and excludes the final entry (a custom curve is not a preset it can copy).
constexpr util::Easing::Type kEasingDisplayOrder[] = {
    util::Easing::Type_Linear, util::Easing::Type_Floor, util::Easing::Type_Round, util::Easing::Type_Auto,
    util::Easing::Type_Cubic, util::Easing::Type_Sine, util::Easing::Type_Quad, util::Easing::Type_Quart,
    util::Easing::Type_Quint, util::Easing::Type_Expo, util::Easing::Type_Circ, util::Easing::Type_Back,
    util::Easing::Type_Elastic, util::Easing::Type_Bounce, util::Easing::Type_Custom,
};
constexpr int kEasingDisplayOrderCount = int(sizeof(kEasingDisplayOrder) / sizeof(kEasingDisplayOrder[0]));
} // namespace

namespace util {

// Type_Auto's evaluation (defined next to the calculate() overloads below): the
// same solve a Custom bezier does, over handles DERIVED from the segment's
// neighbouring keys instead of read from the param.
float calculateAuto(const Easing::Param&, const Easing::AutoContext&, float t, float b, float c, float d);

//-------------------------------------------------------------------------------------------------
bool Easing::Param::isValidParam() const {
    if (type < 0 || Type_TERM <= type)
        return false;
    if (range < 0 || Range_TERM <= range)
        return false;
    if (weight < 0.0f || 1.0f < weight)
        return false;
    return true;
}

bool Easing::Param::operator==(const Param& aRhs) const {
    return type == aRhs.type && range == aRhs.range && weight == aRhs.weight &&
        cubicBezier == aRhs.cubicBezier && aligned == aRhs.aligned;
}
//-------------------------------------------------------------------------------------------------
int Easing::displayIndexOf(const Type aType) {
    for (int i = 0; i < kEasingDisplayOrderCount; ++i) {
        if (kEasingDisplayOrder[i] == aType)
            return i;
    }
    return -1;
}

Easing::Type Easing::typeAtDisplayIndex(const int aIndex) {
    if (0 <= aIndex && aIndex < kEasingDisplayOrderCount)
        return kEasingDisplayOrder[aIndex];
    return Type_Linear;
}

int Easing::displayCount() { return kEasingDisplayOrderCount; }

//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
QString Easing::getTypeName(const Type aType) {
    switch (aType) {
    case Type_Floor:
        return EasingName::tr("Floor");
    case Type_Linear:
        return EasingName::tr("Linear");
    case Type_Sine:
        return EasingName::tr("Sine");
    case Type_Quad:
        return EasingName::tr("Quad");
    case Type_Cubic:
        return EasingName::tr("Cubic");
    case Type_Quart:
        return EasingName::tr("Quart");
    case Type_Quint:
        return EasingName::tr("Quint");
    case Type_Expo:
        return EasingName::tr("Expo");
    case Type_Circ:
        return EasingName::tr("Circ");
    case Type_Back:
        return EasingName::tr("Back");
    case Type_Elastic:
        return EasingName::tr("Elastic");
    case Type_Bounce:
        return EasingName::tr("Bounce");
    case Type_Custom:
        return EasingName::tr("Custom");
    case Type_Round:
        return EasingName::tr("Round");
    case Type_Auto:
        return EasingName::tr("Auto");
    default:
        return "";
    }
}

QString Easing::getRangeName(const Range aRange) {
    switch (aRange) {
    case Range_In:
        return QString("In");
    case Range_Out:
        return QString("Out");
    case Range_InOut:
        return QString("All");
    default:
        return "";
    }
}

Easing::Type Easing::easingToEnum(QString easing) {
    QString aEasing;
    if (easing.isNull()) {
        QSettings settings;
        // The stored default is an ENUM ORDINAL (GeneralSettingDialog writes
        // int(typeAtDisplayIndex(...))) while a project file and the timeline
        // menu carry the NAME. Reading the setting as a string turned every
        // stored ordinal into "3" and answered Linear for every choice, so the
        // default-easing combo had no effect on any newly created key.
        const QVariant stored = settings.value("generalsettings/easing");
        bool isOrdinal = false;
        const int ordinal = stored.toInt(&isOrdinal);
        if (isOrdinal) {
            if (0 <= ordinal && ordinal < Type_TERM)
                return static_cast<Type>(ordinal);
            return Type_Linear;
        }
        aEasing = stored.toString();
    }
    else {
        aEasing = easing;
    }
    if (aEasing == "Floor" || aEasing == "None") // "None" is the legacy name
        return Type_Floor;
    if (aEasing == "Linear")
        return Type_Linear;
    if (aEasing == "Sine")
        return Type_Sine;
    if (aEasing == "Quad")
        return Type_Quad;
    if (aEasing == "Cubic")
        return Type_Cubic;
    if (aEasing == "Quart")
        return Type_Quart;
    if (aEasing == "Quint")
        return Type_Quint;
    if (aEasing == "Expo")
        return Type_Expo;
    if (aEasing == "Circ")
        return Type_Circ;
    if (aEasing == "Back")
        return Type_Back;
    if (aEasing == "Elastic")
        return Type_Elastic;
    if (aEasing == "Bounce")
        return Type_Bounce;
    if (aEasing == "Round")
        return Type_Round;
    if (aEasing == "Auto")
        return Type_Auto;
    if (aEasing == "Custom")
        return Type_Custom;
    return Type_Linear;
    // Default easing is Linear
}

Easing::Range Easing::rangeToEnum(QString range) {
    QString aRange;
    if (range.isNull()) {
        QSettings settings;
        aRange = settings.value("generalsettings/range").toString();
    } else {
        aRange = range;
    }
    if (aRange == "In")
        return Range_In;
    if (aRange == "Out")
        return Range_Out;
    if (aRange == "All")
        return Range_InOut;
    return Range_InOut;
    // Default range is InOut, defined as "All" by Hidefuku
}

//-------------------------------------------------------------------------------------------------
float Easing::calculate(
    const Type aType, const Range aRange, const float t, const float b, const float c, const float d) {
#define RETURN_BY_EASING_FUNCTION(func) \
    switch (aRange) { \
    case Range_In: \
        return func##In(t, b, c, d); \
    case Range_Out: \
        return func##Out(t, b, c, d); \
    case Range_InOut: \
        return func##InOut(t, b, c, d); \
    default: \
        return func##InOut(t, b, c, d); \
    }

    switch (aType) {
    case Type_Floor:
        return b;
    case Type_Round:
        // Stepped hold with a rounded frame: flip to the target value once
        // the normalized progress reaches the interval midpoint.
        return t / d < 0.5f ? b : b + c;
    case Type_Linear:
        return c * (t / d) + b;
    case Type_Auto:
        // Auto's handles are DERIVED from the neighbouring keys, which a bare
        // Type cannot carry - the context lives in the Param overload below. So
        // a context-free call gets the shape an Auto segment with no neighbours
        // has: the identity, i.e. Linear. Returning `b` (the default below)
        // would silently freeze a segment instead.
        return c * (t / d) + b;
    case Type_Sine:
        RETURN_BY_EASING_FUNCTION(sine);
    case Type_Quad:
        RETURN_BY_EASING_FUNCTION(quad);
    case Type_Cubic:
        RETURN_BY_EASING_FUNCTION(cubic);
    case Type_Quart:
        RETURN_BY_EASING_FUNCTION(quart);
    case Type_Quint:
        RETURN_BY_EASING_FUNCTION(quint);
    case Type_Expo:
        RETURN_BY_EASING_FUNCTION(expo);
    case Type_Circ:
        RETURN_BY_EASING_FUNCTION(circ);
    case Type_Back:
        RETURN_BY_EASING_FUNCTION(back);
    case Type_Elastic:
        RETURN_BY_EASING_FUNCTION(elastic);
    case Type_Bounce:
        RETURN_BY_EASING_FUNCTION(bounce);
    default:
        return b;
    }
#undef RETURN_BY_EASING_FUNCTION
}

float calculateBezier(const Easing::Param& aParam, const float t, const float b, const float c, const float d) {
    auto [x1, y1, x2, y2] = aParam.cubicBezier;
    // Normalized progress (frame offset / duration). This is the x fed to the
    // curve; it must NOT depend on weight.
    const float x = t / d;
    // The solver is built per evaluation: constructing a UnitBezier is 11 Horner
    // evals plus a handful of multiplies, and this keeps Easing stateless (no
    // cache, no shared mutable state on the frame path).
    const UnitBezier bezier(x1, y1, x2, y2);
    // The unit cubic solve returns the curve value; scale onto the key range.
    // Weight is deliberately NOT applied here: a custom bezier is exactly the
    // drawn curve, and its handles already span every shape weight could reach.
    // Custom mode has no blend control in the picker at all (its own panel
    // replaces the normal one); the stepped holds and Auto do show one whose
    // weight this math ignores. Keeping weight out of the math means a stale
    // weight on a Custom or Auto key (a value left over from another easing
    // type, or one read from a saved file) can never silently skew a curve.
    return b + c * static_cast<float>(bezier.solve(x, UnitBezier::kBezierEpsilon));
}

float Easing::calculate(const Param& aParam, const float t, const float b, const float c, const float d) {
    // Param, relative frame, 0, 1, current frame, is bezier?
    if (aParam.type == Type_Custom) {
        return calculateBezier(aParam, t, b, c, d);
    }
    if (aParam.type == Type_Auto) {
        return calculateAuto(aParam, AutoContext(), t, b, c, d);
    }
    const float result = calculate(aParam.type, aParam.range, t, b, c, d);
    if (Type_Sine <= aParam.type && aParam.type < Type_Custom) {
        return result * aParam.weight + (c * (t / d) + b) * (1.0f - aParam.weight);
    }
    return result;
}

float Easing::calculate(
    const Param& aParam, const AutoContext& aCtx, const float t, const float b, const float c, const float d) {
    if (aParam.type == Type_Auto) {
        return calculateAuto(aParam, aCtx, t, b, c, d);
    }
    return calculate(aParam, t, b, c, d);
}

//-------------------------------------------------------------------------------------------------
// Blender-style automatic handles, ported from the multi-key curve tool this
// feature came from (dev/curves/easing-demo.html: computeAllTangents /
// getBezierTangents / recalcHandles). That tool shapes a whole key sequence;
// this port keeps its arithmetic but runs it for ONE segment, because that is
// the unit an AnimeEffects easing shapes. Every rule of the original is here:
//
//  * an interior key's tangent is the central difference of its neighbours,
//    (y[i+1] - y[i-1]) / (x[i+1] - x[i-1]) - the "uniform" tangent mode, the one
//    Blender's own auto handles use;
//  * an endpoint has no such triple, so its tangent is derived from the handle
//    of the key it faces: 1.5 - 0.5 * m, i.e. the direction from the endpoint
//    toward that key's own bezier handle;
//  * either tangent is then CLAMPED so neither of its handles leaves the value
//    range its three keys span ("auto clamped");
//  * each control point sits a third of the way across its interval, so only y
//    is ever derived.
//
// In the ported tool the neighbours are absolute (frame, value) points; here
// they arrive normalized to the segment (0 at the from key, 1 at the to key),
// which is the space the whole Easing class speaks and makes the arithmetic
// independent of the channel's units.
void Easing::autoHandles(const AutoContext& aCtx, const float aSpan, float& aY1, float& aY2) {
    // The identity handles: a cubic with its controls at the thirds of the
    // diagonal IS the straight line, i.e. Linear.
    aY1 = 1.0f / 3.0f;
    aY2 = 2.0f / 3.0f;

    if (aSpan <= 0.0f) {
        return;
    }

    // The neighbours in the segment's own units: x in segment widths, y in
    // segment value changes. The from key is (0,0), the to key (1,1).
    const bool hasPrev = aCtx.prev.valid;
    const bool hasNext = aCtx.next.valid;
    const float px = hasPrev ? aCtx.prev.frame / aSpan : 0.0f; // < 0
    const float py = hasPrev ? aCtx.prev.value : 0.0f;
    const float nx = hasNext ? aCtx.next.frame / aSpan : 0.0f; // > 1
    const float ny = hasNext ? aCtx.next.value : 0.0f;

    // The tangent slope at each end of the segment. An end with a neighbour gets
    // the central difference over its own triple; the other end is filled in
    // below from the handle it faces.
    //
    // start: triple (prev, from, to): (py - 0) / (px - 1) is the same central
    // difference written as (1 - py) / (1 - px).
    float m0 = 0.0f;
    if (hasPrev) {
        const float dxPrev = 1.0f - px; // > 0
        m0 = (1.0f - py) / dxPrev;
    }
    // end: triple (from, to, next): (ny - 0) / (nx - 0).
    float m1 = 0.0f;
    if (hasNext) {
        m1 = nx != 0.0f ? ny / nx : 0.0f;
    }

    // Clamp each interior tangent so BOTH of its handles stay inside the value
    // range its three keys span. Each handle is measured from its own key: the
    // one leaving the key sits at +m*(dxRight/3), the one arriving at it at
    // -m*(dxLeft/3), and the two constraints are intersected. Both intervals are
    // well ordered here (every segment share below is positive), so a plain
    // clamp is the whole rule.
    if (hasPrev) {
        const float loY = std::min(std::min(py, 0.0f), 1.0f);
        const float hiY = std::max(std::max(py, 0.0f), 1.0f);
        const float left = -px / 3.0f; // previous segment's share of this key
        const float right = 1.0f / 3.0f; // this segment's share
        const float loM = std::max(loY / right, -hiY / left);
        const float hiM = std::min(hiY / right, -loY / left);
        m0 = std::min(std::max(m0, loM), hiM);
    }
    if (hasNext) {
        const float loY = std::min(std::min(0.0f, 1.0f), ny);
        const float hiY = std::max(std::max(0.0f, 1.0f), ny);
        const float left = 1.0f / 3.0f; // this segment's share
        const float right = (nx - 1.0f) / 3.0f; // next segment's share
        const float loM = std::max(right != 0.0f ? (loY - 1.0f) / right : 0.0f, -(hiY - 1.0f) / left);
        const float hiM = std::min(right != 0.0f ? (hiY - 1.0f) / right : 0.0f, -(loY - 1.0f) / left);
        if (right != 0.0f) {
            m1 = std::min(std::max(m1, loM), hiM);
        }
    }

    // Fill in the ends that have no neighbour, now that the tangent they are
    // derived from is final. Both missing means the segment says nothing about
    // its own shape, and the identity handles above are already the answer.
    if (hasPrev && !hasNext) {
        m1 = 1.5f - 0.5f * m0;
    } else if (!hasPrev && hasNext) {
        m0 = 1.5f - 0.5f * m1;
    }

    // A segment with NEITHER neighbour carries no tangent to derive, so the
    // identity handles written above must survive: m0 = m1 = 0 would overwrite
    // them with the step (0, 1), and a two-key channel (or a preview with no
    // document) would hold instead of easing.
    if (hasPrev || hasNext) {
        aY1 = m0 / 3.0f;
        aY2 = 1.0f - m1 / 3.0f;
    }
}

float calculateAuto(const Easing::Param& aParam, const Easing::AutoContext& aCtx, const float t,
                    const float b, const float c, const float d) {
    Q_UNUSED(aParam);
    // Same shape as calculateBezier: derive the drawn curve, then solve it and
    // scale onto the key range. This is the only difference - the control points
    // are computed rather than read, from the keys around the segment.
    float y1 = 0.0f, y2 = 0.0f;
    Easing::autoHandles(aCtx, d, y1, y2);
    const UnitBezier bezier(1.0f / 3.0f, y1, 2.0f / 3.0f, y2);
    return b + c * static_cast<float>(bezier.solve(t / d, UnitBezier::kBezierEpsilon));
}

//-------------------------------------------------------------------------------------------------
float Easing::sineIn(const float t, const float b, const float c, const float d) { return -c * qCos(t / d * (M_PI / 2)) + c + b; }

float Easing::sineOut(const float t, const float b, const float c, const float d) { return c * qSin(t / d * (M_PI / 2)) + b; }

float Easing::sineInOut(const float t, const float b, const float c, const float d) { return -c / 2 * (qCos(M_PI * t / d) - 1) + b; }

//-------------------------------------------------------------------------------------------------
float Easing::quadIn(float t, const float b, const float c, const float d) {
    t /= d;
    return c * t * t + b;
}

float Easing::quadOut(float t, const float b, const float c, const float d) {
    t /= d;
    return -c * t * (t - 2) + b;
}

float Easing::quadInOut(float t, const float b, const float c, const float d) {
    t /= d / 2;
    if (t < 1) {
        return ((c / 2) * (t * t)) + b;
    } else {
        --t;
        return -c / 2 * ((t - 2) * t - 1) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::cubicIn(float t, const float b, const float c, const float d) {
    t /= d;
    return c * t * t * t + b;
}

float Easing::cubicOut(float t, const float b, const float c, const float d) {
    t = t / d - 1;
    return c * (t * t * t + 1) + b;
}

float Easing::cubicInOut(float t, const float b, const float c, const float d) {
    t /= d / 2;
    if (t < 1) {
        return c / 2 * t * t * t + b;
    } else {
        t -= 2;
        return c / 2 * (t * t * t + 2) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::quartIn(float t, const float b, const float c, const float d) {
    t /= d;
    return c * t * t * t * t + b;
}

float Easing::quartOut(float t, const float b, const float c, const float d) {
    t = t / d - 1;
    return -c * (t * t * t * t - 1) + b;
}

float Easing::quartInOut(float t, const float b, const float c, const float d) {
    t /= d / 2;
    if (t < 1) {
        return c / 2 * t * t * t * t + b;
    } else {
        t -= 2;
        return -c / 2 * (t * t * t * t - 2) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::quintIn(float t, const float b, const float c, const float d) {
    t /= d;
    return c * t * t * t * t * t + b;
}

float Easing::quintOut(float t, const float b, const float c, const float d) {
    t = t / d - 1;
    return c * (t * t * t * t * t + 1) + b;
}

float Easing::quintInOut(float t, const float b, const float c, const float d) {
    t /= d / 2;
    if (t < 1) {
        return c / 2 * t * t * t * t * t + b;
    } else {
        t -= 2;
        return c / 2 * (t * t * t * t * t + 2) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::expoIn(const float t, const float b, const float c, const float d) { return (t == 0) ? b : c * qPow(2, 10 * (t / d - 1)) + b; }

float Easing::expoOut(const float t, const float b, const float c, const float d) {
    return (t == d) ? b + c : c * (-qPow(2, -10 * t / d) + 1) + b;
}

float Easing::expoInOut(float t, const float b, const float c, const float d) {
    if (t == 0)
        return b;
    if (t == d)
        return b + c;

    if ((t /= d / 2) < 1) {
        return c / 2 * qPow(2, 10 * (t - 1)) + b;
    } else {
        return c / 2 * (-qPow(2, -10 * --t) + 2) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::circIn(float t, const float b, const float c, const float d) {
    t /= d;
    return -c * (qSqrt(1 - t * t) - 1) + b;
}

float Easing::circOut(float t, const float b, const float c, const float d) {
    t = t / d - 1;
    return c * qSqrt(1 - t * t) + b;
}

float Easing::circInOut(float t, const float b, const float c, const float d) {
    t /= d / 2;
    if (t < 1) {
        return -c / 2 * (qSqrt(1 - t * t) - 1) + b;
    } else {
        t -= 2;
        return c / 2 * (qSqrt(1 - t * t) + 1) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::backIn(float t, const float b, const float c, const float d) {
    float s = 1.70158f;
    t /= d;
    return c * t * t * ((s + 1) * t - s) + b;
}

float Easing::backOut(float t, const float b, const float c, const float d) {
    float s = 1.70158f;
    t = t / d - 1;
    return c * (t * t * ((s + 1) * t + s) + 1) + b;
}

float Easing::backInOut(float t, const float b, const float c, const float d) {
    float s = 1.70158f;
    t /= d / 2;
    if (t < 1) {
        s *= 1.525f;
        return c / 2 * (t * t * ((s + 1) * t - s)) + b;
    } else {
        t -= 2;
        s *= 1.525f;
        return c / 2 * (t * t * ((s + 1) * t + s) + 2) + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::elasticIn(float t, const float b, const float c, const float d) {
    if (t == 0)
        return b;
    if ((t /= d) == 1)
        return b + c;

    float p = d * 0.3f;
    float a = c;
    float s = p / 4;
    float postFix = a * qPow(2, 10 * (t -= 1));
    return -(postFix * qSin((t * d - s) * (2 * M_PI) / p)) + b;
}

float Easing::elasticOut(float t, const float b, const float c, const float d) {
    if (t == 0)
        return b;
    if ((t /= d) == 1)
        return b + c;

    float p = d * 0.3f;
    float a = c;
    float s = p / 4;
    return (a * qPow(2, -10 * t) * qSin((t * d - s) * (2 * M_PI) / p) + c + b);
}

float Easing::elasticInOut(float t, const float b, const float c, const float d) {
    if (t == 0)
        return b;
    if ((t /= d / 2) == 2)
        return b + c;

    float p = d * (0.3f * 1.5f);
    float a = c;
    float s = p / 4;

    if (t < 1) {
        float postFix = a * qPow(2, 10 * (t -= 1));
        return -0.5f * (postFix * qSin((t * d - s) * (2 * M_PI) / p)) + b;
    } else {
        float postFix = a * qPow(2, -10 * (t -= 1));
        return postFix * qSin((t * d - s) * (2 * M_PI) / p) * 0.5f + c + b;
    }
}

//-------------------------------------------------------------------------------------------------
float Easing::bounceIn(const float t, const float b, const float c, const float d) { return c - bounceOut(d - t, 0, c, d) + b; }

float Easing::bounceOut(float t, const float b, const float c, const float d) {
    if ((t /= d) < (1.0f / 2.75f)) {
        return c * (7.5625f * t * t) + b;
    } else if (t < (2.0f / 2.75f)) {
        float postFix = t -= (1.5f / 2.75f);
        return c * (7.5625f * (postFix)*t + 0.75f) + b;
    } else if (t < (2.5f / 2.75f)) {
        float postFix = t -= (2.25f / 2.75f);
        return c * (7.5625f * (postFix)*t + 0.9375f) + b;
    } else {
        float postFix = t -= (2.625f / 2.75f);
        return c * (7.5625f * (postFix)*t + 0.984375f) + b;
    }
}

float Easing::bounceInOut(const float t, const float b, const float c, const float d) {
    if (t < d / 2) {
        return bounceIn(t * 2, 0, c, d) * 0.5f + b;
    } else {
        return bounceOut(t * 2 - d, 0, c, d) * 0.5f + c * 0.5f + b;
    }
}

} // namespace util
