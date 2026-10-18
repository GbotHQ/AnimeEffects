#ifndef UTIL_EASING
#define UTIL_EASING

#include <QString>
#include <QSettings>

namespace util {

class Easing {
public:
    enum Type {
        // Stepped holds: no interpolation between keys. Floor keeps the
        // previous key's value up to the next key's frame; Round flips to
        // the next key's value at the interval midpoint (rounded frame).
        Type_Floor,
        Type_Linear,
        Type_Sine,
        Type_Quad,
        Type_Cubic,
        Type_Quart,
        Type_Quint,
        Type_Expo,
        Type_Circ,
        Type_Back,
        Type_Elastic,
        Type_Bounce,
        Type_Custom,
        // Appended after Custom: existing ids 0-12 are serialized as-is and
        // must never shift.
        Type_Round,
        // Blender-style "auto clamped" bezier handles: the segment's control
        // points are not stored, they are DERIVED from the neighbouring keys of
        // the same channel every time the rate is evaluated (see AutoContext and
        // autoHandles below). A key whose neighbours change therefore re-shapes
        // itself, which is the point of an automatic handle.
        Type_Auto,
        Type_TERM,
    };

    enum Range { Range_In, Range_Out, Range_InOut, Range_TERM};

    struct CubicBezier {
        // Classic cubic ease-in-out (smoothstep): controls crossed at the
        // thirds — (2/3, 0) and (1/3, 1) — giving Y(t) = 3t^2 - 2t^3. Both
        // controls ON the diagonal would degenerate the curve to a straight
        // line, so the Ys sit at the extremes.
        float x1 = 2.0f / 3.0f;
        float y1 = 0.0f;
        float x2 = 1.0f / 3.0f;
        float y2 = 1.0f;
        // Control points are compared by value (set/read via the editor or
        // serialization), so a plain float compare is the right test.
        bool operator==(const CubicBezier& aRhs) const {
            return x1 == aRhs.x1 && y1 == aRhs.y1 && x2 == aRhs.x2 && y2 == aRhs.y2;
        }
    };

    struct Param {
        Param()
            :
            type(Type_Linear), range(Range_InOut), weight(1.0f) {}
        bool isValidParam() const;
        bool operator==(const Param& aRhs) const;

        bool operator!=(const Param& aRhs) const { return !(*this == aRhs); }
        mutable Type type;
        Range range;
        float weight;
        mutable CubicBezier cubicBezier;
    };

    // ---- Type_Auto's derived handles -------------------------------------
    // One neighbouring key of an Auto segment. `value` is the shape's value
    // expressed in the SEGMENT's own change - 0 at the segment's `from` key and
    // 1 at its `to` key - so the handle math is unit-free: it reads the same
    // whether the channel is a position in pixels, an angle in radians or an
    // opacity in 0..1, and it never needs the easing to know what it is shaping.
    struct AutoKey {
        bool valid = false;
        float frame = 0.0f; // frame offset from the segment's from key
        float value = 0.0f; // (v - vFrom) / (vTo - vFrom)
    };

    // The keys around an Auto segment's two ends. A segment of a two-key channel
    // has neither; the first key of a channel has no `prev`, the last no `next`.
    struct AutoContext {
        AutoKey prev; // the key before the segment's from key
        AutoKey next; // the key after the segment's to key
    };

    // The control points of the segment (0,0) -> (aSpan,1) in the normalized
    // space aCtx's neighbours are expressed in, at x = 1/3 and x = 2/3 (Blender
    // puts an automatic handle a third of the way across its interval, so only
    // the y values are ever derived). y is the neighbouring keys' slope at each
    // end, clamped so neither handle leaves the value range its own neighbouring
    // keys span - Blender's "auto clamped", which is what stops an automatic
    // handle from easing PAST a key it sits between.
    static void autoHandles(const AutoContext& aCtx, float aSpan, float& aY1, float& aY2);

    static QString getTypeName(Type aType);
    static QString getRangeName(Range aRange);
    // Picker display order: the stepped holds lead, then the analytic curves,
    // then the bezier "Custom" entry. Distinct from the enum ordinal (which is
    // serialization order and must not shift), so pickers build their layouts
    // from these helpers instead of assuming index == enum ordinal.
    static int displayIndexOf(Type aType);      // -1 when aType is not listed
    static Type typeAtDisplayIndex(int aIndex); // Type_Linear fallback
    static int displayCount();                  // number of pickable entries

    static float calculate(Type, Range, float t, float b, float c, float d);
    static float calculate(const Param&, float t, float b, float c, float d);
    // The Auto form. aCtx carries the neighbouring keys (an empty context - what
    // a caller with no document hands in - yields the straight identity curve).
    // aSpan must be the segment's length in the same frame unit as aCtx's
    // frames, exactly as `d` is for calculate() above.
    static float calculate(const Param&, const AutoContext& aCtx, float t, float b, float c, float d);

    static float sineIn(float t, float b, float c, float d);
    static float sineOut(float t, float b, float c, float d);
    static float sineInOut(float t, float b, float c, float d);

    static float quadIn(float t, float b, float c, float d);
    static float quadOut(float t, float b, float c, float d);
    static float quadInOut(float t, float b, float c, float d);

    static float cubicIn(float t, float b, float c, float d);
    static float cubicOut(float t, float b, float c, float d);
    static float cubicInOut(float t, float b, float c, float d);

    static float quartIn(float t, float b, float c, float d);
    static float quartOut(float t, float b, float c, float d);
    static float quartInOut(float t, float b, float c, float d);

    static float quintIn(float t, float b, float c, float d);
    static float quintOut(float t, float b, float c, float d);
    static float quintInOut(float t, float b, float c, float d);

    static float expoIn(float t, float b, float c, float d);
    static float expoOut(float t, float b, float c, float d);
    static float expoInOut(float t, float b, float c, float d);

    static float circIn(float t, float b, float c, float d);
    static float circOut(float t, float b, float c, float d);
    static float circInOut(float t, float b, float c, float d);

    static float backIn(float t, float b, float c, float d);
    static float backOut(float t, float b, float c, float d);
    static float backInOut(float t, float b, float c, float d);

    static float elasticIn(float t, float b, float c, float d);
    static float elasticOut(float t, float b, float c, float d);
    static float elasticInOut(float t, float b, float c, float d);

    static float bounceIn(float t, float b, float c, float d);
    static float bounceOut(float t, float b, float c, float d);
    static float bounceInOut(float t, float b, float c, float d);

    static Easing::Type easingToEnum(QString easing);
    static Easing::Range rangeToEnum(QString range);

private:
    Easing() {}
};

} // namespace util

#endif // UTIL_EASING
