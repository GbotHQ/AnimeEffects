#ifndef UTIL_EASING
#define UTIL_EASING

#include <QString>
#include <QSettings>
#include <QPointF>

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
        // ID 14 IS RETIRED. It was Type_Auto, and it is deliberately NOT
        // reused: a 0.14 file that holds it loads as Type_Custom with BOTH
        // handle sides Auto (see Deserializer::read), which is what that value
        // meant. Type_TERM deliberately keeps the value 14 so nothing above it
        // can quietly take the retired slot.
        Type_TERM,
    };

    // Blender's five per-side bezier handle modes. A key owns TWO of them -
    // handleIn (its incoming tangent, stored as the PREVIOUS segment's x2/y2)
    // and handleOut (its outgoing one, stored as the NEXT segment's x1/y1) -
    // and each side derives its control point by its OWN mode:
    //
    //   Free         the stored value is the truth, untouched;
    //   Aligned      the stored value is the truth TOO - it is collinear with
    //                the other side by construction, because a drag keeps it so
    //                (the mode is a promise the gestures enforce, never an
    //                inference from the pixel angle);
    //   Vector       on the chord to the segment's other key, at the third
    //                points: the segment is a straight line (Blender's Vector);
    //   Auto         DERIVED from that side's neighbour: the outgoing side of a
    //                key reads the PREVIOUS key, the incoming side the NEXT one
    //                (a one-sided secant, so each side keeps deriving while
    //                the other is dragged);
    //   AutoClamped  the same derivation, with the handle clamped into the
    //                value range of the key and its two neighbours, so a local
    //                extremum is flat instead of overshooting.
    //
    // The ordering is serialization order (Blender's own enum order) and 0.14
    // did not have it; see FormatVersion.h.
    enum HandleMode {
        HandleMode_Free,
        HandleMode_Aligned,
        HandleMode_Vector,
        HandleMode_Auto,
        HandleMode_AutoClamped,
        HandleMode_TERM,
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
        // The two handle modes of the KEY that owns this param: its incoming
        // tangent (handleIn, stored as the previous segment's x2/y2) and its
        // outgoing one (handleOut, stored as this param's x1/y1). A new key
        // starts Aligned on both sides - the old `aligned = true` default,
        // split into the two sides Blender edits. A gesture that drags one side
        // writes THAT side's mode only; the other side keeps deriving if it is
        // Auto (see GraphEditor::updateDrag).
        HandleMode handleIn = HandleMode_Aligned;
        HandleMode handleOut = HandleMode_Aligned;
    };

    // ---- A segment's derived handles -------------------------------------
    // One neighbouring key of a segment. `value` is the shape's value expressed
    // in the SEGMENT's own change - 0 at the segment's `from` key and 1 at its
    // `to` key - so the handle math is unit-free: it reads the same whether the
    // channel is a position in pixels, an angle in radians or an opacity in
    // 0..1, and it never needs the easing to know what it is shaping.
    struct AutoKey {
        bool valid = false;
        float frame = 0.0f; // frame offset from the segment's from key
        float value = 0.0f; // (v - vFrom) / (vTo - vFrom)
    };

    // The keys around a segment's two ends. A segment of a two-key channel has
    // neither; the first key of a channel has no `prev`, the last no `next`.
    // `inMode` is the TO key's incoming mode: the arriving side of the segment
    // belongs to the TO key (its stored value lives in the FROM key's param),
    // so a caller that has a Param in hand is holding only one of the two modes.
    struct AutoContext {
        AutoKey prev; // the key before the segment's from key
        AutoKey next; // the key after the segment's to key
        HandleMode inMode = HandleMode_Aligned;
    };

    // Whether a handle mode derives its control point instead of reading the
    // stored one.
    static bool isDerived(HandleMode aMode) {
        return aMode == HandleMode_Vector || aMode == HandleMode_Auto
            || aMode == HandleMode_AutoClamped;
    }

    // The control point of ONE side of the segment aFrom -> its successor, in
    // normalized segment space (x 0..1 across the frame span, y in units of the
    // segment's own value change). aSide 0 is the outgoing point (the FROM
    // key's handleOut), 1 the incoming one (the TO key's mode, passed as
    // aInMode). aSpan is the segment's frame span, the unit autoEasingContext's
    // neighbour frames use.
    //
    //   * a stored mode (Free/Aligned) returns the stored (x,y) untouched;
    //   * Vector returns the chord's third point - (1/3,1/3) outgoing,
    //     (2/3,2/3) incoming - which is a straight segment;
    //   * Auto derives the side's slope from ITS OWN outside neighbour: the
    //     outgoing side reads `prev` (the secant arriving at the key), the
    //     incoming side reads `next` (the secant leaving it). A side with no
    //     such neighbour falls back to the segment's own chord, i.e. the
    //     identity third point. The handle x stays at the third (Blender's
    //     automatic handle), so only y is derived;
    //   * AutoClamped clamps that y into the value range the key and its
    //     neighbours span, which flattens a local extremum.
    static QPointF controlPoint(
        const Param& aFrom, HandleMode aInMode, const AutoContext& aCtx, float aSpan, int aSide);

    static QString getTypeName(Type aType);
    static QString getRangeName(Range aRange);
    // The translated name of one handle mode, from the same EasingName catalog
    // the type names come from.
    static QString getHandleModeName(HandleMode aMode);
    // Picker display order: Linear, Floor and Round lead (the entries whose
    // shape is not one of the family's parameters - a straight line and the two
    // stepped holds), then the analytic curve family, then the bezier "Custom"
    // entry. Distinct from the enum ordinal (which is serialization order and
    // must not shift), so pickers build their layouts from these helpers
    // instead of assuming index == enum ordinal. Automatic handles are a MODE
    // now, not a type, so they are not in this table; the picker's Auto preset
    // writes both modes on a Custom param.
    static int displayIndexOf(Type aType);      // -1 when aType is not listed
    static Type typeAtDisplayIndex(int aIndex); // Type_Linear fallback
    static int displayCount();                  // number of pickable entries

    static float calculate(Type, Range, float t, float b, float c, float d);
    static float calculate(const Param&, float t, float b, float c, float d);
    // The Custom form with its two control points derived per side: aFrom is the
    // FROM key's param (its handleOut drives the leaving side and holds the
    // stored pair) and aCtx carries the neighbours plus the TO key's incoming
    // mode. An empty context (what a caller with no document hands in) makes
    // both derived sides fall back to the identity thirds; a non-Custom type
    // ignores the context entirely. aSpan must be the segment's length in the
    // same frame unit as aCtx's frames, exactly as `d` is for calculate().
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
