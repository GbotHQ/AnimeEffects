#include "Easing.h"
#include <QtMath>
#include <algorithm>
#include "util/EasingName.h"
#include "util/UnitBezier.h"

namespace {
// Picker display order, mirrored by the Easing::display* helpers. The enum
// ordinal is serialization order; this is the order pickers show.
//
// The head is the three NON-CURVE entries - the two stepped holds and the
// straight line, whose shape is not one of the family's parameters - then the
// analytic curve family (Cubic, Sine, Quad, ...), then the drawn Custom curve.
// Automatic handles are a per-side MODE of the Custom bezier now (see
// HandleMode), not an entry here: the picker's Auto preset writes both modes.
//
// Type_Custom stays LAST: the default-easing combo indexes this table directly
// and excludes the final entry (a custom curve is not a preset it can copy).
constexpr util::Easing::Type kEasingDisplayOrder[] = {
    util::Easing::Type_Linear, util::Easing::Type_Floor, util::Easing::Type_Round,
    util::Easing::Type_Cubic, util::Easing::Type_Sine, util::Easing::Type_Quad, util::Easing::Type_Quart,
    util::Easing::Type_Quint, util::Easing::Type_Expo, util::Easing::Type_Circ, util::Easing::Type_Back,
    util::Easing::Type_Elastic, util::Easing::Type_Bounce, util::Easing::Type_Custom,
};
constexpr int kEasingDisplayOrderCount = int(sizeof(kEasingDisplayOrder) / sizeof(kEasingDisplayOrder[0]));
} // namespace

namespace util {

//-------------------------------------------------------------------------------------------------
bool Easing::Param::isValidParam() const {
    if (type < 0 || Type_TERM <= type)
        return false;
    if (range < 0 || Range_TERM <= range)
        return false;
    if (weight < 0.0f || 1.0f < weight)
        return false;
    // Type 14 IS Type_TERM now, so the range test above rejects the retired
    // Type_Auto id: it names no easing and a 0.15 file must never carry it.
    if (handleIn < 0 || HandleMode_TERM <= handleIn)
        return false;
    if (handleOut < 0 || HandleMode_TERM <= handleOut)
        return false;
    return true;
}

bool Easing::Param::operator==(const Param& aRhs) const {
    return type == aRhs.type && range == aRhs.range && weight == aRhs.weight &&
        cubicBezier == aRhs.cubicBezier && handleIn == aRhs.handleIn && handleOut == aRhs.handleOut;
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
    default:
        return "";
    }
}

QString Easing::getHandleModeName(const HandleMode aMode) {
    // The names are Blender's, through the same EasingName catalog as the type
    // names, so every picker/combo shows one vocabulary.
    switch (aMode) {
    case HandleMode_Free:
        return EasingName::tr("Free");
    case HandleMode_Aligned:
        return EasingName::tr("Aligned");
    case HandleMode_Vector:
        return EasingName::tr("Vector");
    case HandleMode_Auto:
        return EasingName::tr("Auto");
    case HandleMode_AutoClamped:
        return EasingName::tr("Auto Clamped");
    default:
        return "";
    }
}

QString Easing::getParamName(const Param& aParam) {
    if (aParam.type != Type_Custom)
        return getTypeName(aParam.type);

    // A Custom curve is its handle pair. Both sides equal are the five named
    // presets Blender offers; a mixed pair names both sides, incoming first, so
    // the chip can never claim a shape the two modes do not describe.
    if (aParam.handleIn == aParam.handleOut) {
        switch (aParam.handleIn) {
        case HandleMode_Free:
        case HandleMode_Aligned:
        case HandleMode_Vector:
        case HandleMode_Auto:
        case HandleMode_AutoClamped:
            return getHandleModeName(aParam.handleIn);
        default:
            break;
        }
    }
    return getHandleModeName(aParam.handleIn) + QStringLiteral(" / ")
        + getHandleModeName(aParam.handleOut);
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
        // The retired name: a project file's lane JSON or a stored default can
        // still say it. There is no Auto TYPE any more - the callers that read a
        // saved easing (ctrl::TimeLineEditor::easingFromJson, defaultParam) turn it into a Custom param
        // whose two sides are kDefaultAutomaticHandleMode, which is what it meant.
        return Type_Custom;
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

Easing::Param Easing::defaultParam() {
    Param param;
    param.range = rangeToEnum(QString());

    QSettings settings;
    const QVariant stored = settings.value("generalsettings/easing");
    bool isOrdinal = false;
    const int ordinal = stored.toInt(&isOrdinal);
    if (isOrdinal) {
        // 14 was Type_Auto before Type_Auto was retired and the id was left
        // unused (now Type_TERM). A settings file written before 0.15 can still
        // carry it; it meant the automatic (CLAMPED) handle pair, not unclamped
        // Auto and not Linear.
        if (ordinal == int(Type_TERM)) {
            param.type = Type_Custom;
            param.handleIn = kDefaultAutomaticHandleMode;
            param.handleOut = kDefaultAutomaticHandleMode;
            return param;
        }
        // The stored ordinal names an interpolation type directly (0.14+); the
        // range test keeps an out-of-range byte from becoming an invalid param.
        if (0 <= ordinal && ordinal < int(Type_TERM)) {
            param.type = static_cast<Type>(ordinal);
            return param;
        }
        param.type = Type_Linear;
        return param;
    }

    const QString name = stored.toString();
    if (name == QStringLiteral("Auto")) {
        param.type = Type_Custom;
        param.handleIn = kDefaultAutomaticHandleMode;
        param.handleOut = kDefaultAutomaticHandleMode;
        return param;
    }
    param.type = easingToEnum(name);
    return param;
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
    // replaces the normal one); the stepped holds do show one whose weight this
    // math ignores. Keeping weight out of the maths means a stale weight on a
    // Custom key (a value left over from another easing type, or one read from a
    // saved file) can never silently skew a curve.
    return b + c * static_cast<float>(bezier.solve(x, UnitBezier::kBezierEpsilon));
}

float Easing::calculate(const Param& aParam, const float t, const float b, const float c, const float d) {
    // Param, relative frame, 0, 1, current frame, is bezier?
    if (aParam.type == Type_Custom) {
        return calculateBezier(aParam, t, b, c, d);
    }
    const float result = calculate(aParam.type, aParam.range, t, b, c, d);
    if (Type_Sine <= aParam.type && aParam.type < Type_Custom) {
        return result * aParam.weight + (c * (t / d) + b) * (1.0f - aParam.weight);
    }
    return result;
}

float Easing::calculate(
    const Param& aParam, const AutoContext& aCtx, const float t, const float b, const float c, const float d) {
    if (aParam.type != Type_Custom) {
        return calculate(aParam, t, b, c, d);
    }
    // Same solve a stored Custom pair gets, over control points that each side
    // derives by its own mode (see controlPoint). aCtx.inMode is the TO key's
    // incoming mode: the arriving side belongs to the TO key even though its
    // stored value lives in aParam.
    const QPointF first = controlPoint(aParam, aCtx.inMode, aCtx, d, 0);
    const QPointF second = controlPoint(aParam, aCtx.inMode, aCtx, d, 1);
    const UnitBezier bezier(
        static_cast<float>(first.x()), static_cast<float>(first.y()),
        static_cast<float>(second.x()), static_cast<float>(second.y()));
    return b + c * static_cast<float>(bezier.solve(t / d, UnitBezier::kBezierEpsilon));
}

//-------------------------------------------------------------------------------------------------
// PER-SIDE HANDLE DERIVATION.
//
// Each side of a Custom segment carries its own HandleMode and derives its own
// control point; one derivation feeds playback (core::TimeKeyBlender), the graph
// (ctrl::graph::Curve) and the picker's previews, so the drawn curve and the
// played curve cannot disagree.
//
// The segment is normalized to (0,0) -> (1,1): x is progress across the frame
// span, y is the value in units of the segment's own change. The neighbours in
// AutoContext use exactly that space, so the arithmetic is independent of the
// channel's units (pixels, radians, 0..1 opacity).
//
// Auto reads the neighbour on ITS OWN outside:
//   * the OUTGOING side (at the from key) reads `prev`, the key before the from
//     key: its slope is the secant arriving at the key, (0 - py) / (0 - px),
//     so the leaving tangent continues the incoming curve - the one-sided
//     central difference that makes a key smooth;
//   * the INCOMING side (at the to key) reads `next`, the key after the to key:
//     its slope is (ny - 1) / (nx - 1), the secant leaving the key.
//
// This is the whole answer to "when the key is auto and I drag any of the
// handles, it changes both tangents to manual": the two sides read DIFFERENT
// neighbours and are derived independently, so dragging one side (which writes
// that side's mode Free) leaves the other side Auto and still deriving from its
// own neighbour.
//
// A side with no such neighbour falls back to the segment's own chord, i.e. its
// identity third point, which is what a two-key channel (or an endpoint) has to
// derive from. The handle x stays at the third (Blender's automatic handle), so
// only y is ever derived.
QPointF Easing::controlPoint(
    const Param& aFrom, const HandleMode aInMode, const AutoContext& aCtx, const float aSpan, const int aSide) {
    const bool outSide = (aSide == 0);
    const HandleMode mode = outSide ? aFrom.handleOut : aInMode;
    const CubicBezier& stored = aFrom.cubicBezier;

    if (!isDerived(mode)) {
        // Free and Aligned read the stored pair: Aligned's collinearity is
        // enforced by the gestures that write it, never inferred here.
        return outSide ? QPointF(stored.x1, stored.y1) : QPointF(stored.x2, stored.y2);
    }

    if (mode == HandleMode_Vector) {
        // Blender's Vector: on the chord to the segment's other key, at the
        // third points - a straight segment. The point stays inside the cubic's
        // own domain, which is why the outside chord (which would put x < 0 or
        // x > 1) is not what a vector handle means.
        return outSide ? QPointF(1.0 / 3.0, 1.0 / 3.0) : QPointF(2.0 / 3.0, 2.0 / 3.0);
    }

    // Auto / AutoClamped: the one-sided secant from this side's neighbour.
    // The identity fallbacks are the thirds of the diagonal.
    double y = outSide ? 1.0 / 3.0 : 2.0 / 3.0;
    if (aSpan > 0.0f) {
        if (outSide) {
            if (aCtx.prev.valid) {
                const double px = static_cast<double>(aCtx.prev.frame) / aSpan; // < 0
                const double py = aCtx.prev.value;
                const double slope = px != 0.0 ? py / px : 1.0;
                y = slope / 3.0;
                if (mode == HandleMode_AutoClamped) {
                    // Keep the handle inside the value range the key and its
                    // neighbours span. The segment's own end (y = 1) is one of
                    // those values, so a monotone rise keeps its slope while a
                    // local extremum is pulled flat.
                    const double lo = std::min(std::min(py, 0.0), 1.0);
                    const double hi = std::max(std::max(py, 0.0), 1.0);
                    y = std::min(std::max(y, lo), hi);
                }
            }
        } else {
            if (aCtx.next.valid) {
                const double nx = static_cast<double>(aCtx.next.frame) / aSpan; // > 1
                const double ny = aCtx.next.value;
                const double slope = nx != 1.0 ? (ny - 1.0) / (nx - 1.0) : 1.0;
                y = 1.0 - slope / 3.0;
                if (mode == HandleMode_AutoClamped) {
                    const double lo = std::min(std::min(0.0, 1.0), ny);
                    const double hi = std::max(std::max(0.0, 1.0), ny);
                    y = std::min(std::max(y, lo), hi);
                }
            }
        }
    }
    return outSide ? QPointF(1.0 / 3.0, y) : QPointF(2.0 / 3.0, y);
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
