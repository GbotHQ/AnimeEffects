#ifndef CTRL_BRUSH_H
#define CTRL_BRUSH_H

#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <functional>
#include "util/NonCopyable.h"

namespace ctrl {

// THE one brush model. Every painting context in the application shares it, so
// "size 40, hardness 60%" means the same thing whether it is painting a raster
// mask, warping FFD vertices, widening bone influence or erasing a pose - and a
// fix to the stroke (spacing, pressure ramps, the falloff) lands everywhere at
// once instead of in one tool and not the others.
//
// WHAT IS SHARED AND WHAT IS NOT. The stroke (where the stamps go), the falloff
// (what one stamp weighs at a distance) and the parameter names are shared. The
// EFFECT of a stamp is the target's: a raster target deposits alpha, the FFD
// target displaces vertices, an influence target adds weight. That is why the
// stamp carries an AMOUNT and a distance falloff and nothing else - a target that
// mixed those up would be the only thing that could disagree.
struct BrushParam {
    float radius = 100.0f;   // content (world) pixels
    float hardness = 0.5f;   // 0..1: the fraction of the radius at full strength
    float opacity = 1.0f;    // 0..1: the cap ONE stroke may reach (build-up is off)
    float flow = 1.0f;       // 0..1: what one stamp deposits
    bool pressureSize = false; // a tablet pen's pressure scales the radius
    bool pressureFlow = true;  // a tablet pen's pressure scales the amount
    float spacing = 0.125f;    // distance between stamps, as a fraction of the radius

    // the brush's erase mode: the same stamp, deposited the other way
    bool erase = false;

    // The step actually walked between stamps, in content pixels. Floored so a
    // hairline brush cannot ask for thousands of stamps per pixel of travel.
    float stampStep() const { return std::max(radius * spacing, 0.5f); }
};

// One stamp: the target applies brushFalloff() per element it covers and
// multiplies by `amount`; `erase` says which way to deposit it.
struct BrushStamp {
    QVector2D center;   // content (world) pixels
    QVector2D delta;    // travel since the previous stamp: what a WARPING target moves
                        // a point by, so a spaced stroke conserves its displacement
    float radius = 0.0f; // content pixels, pressure already applied
    float hardness = 0.0f;
    float amount = 0.0f; // flow * pressure, 0..1 (the falloff is NOT folded in)
    bool erase = false;
};

// The shared falloff, and the ONLY definition of what hardness means:
// Photoshop's model - a flat CORE of radius*hardness at full strength and a
// smoothstep band from there to the rim. hardness 1 leaves a hard edge (any
// antialiasing is the target's own filtering), hardness 0 a fully feathered disc.
// A distance at or past the rim weighs nothing.
inline float brushFalloff(float aDistance, float aRadius, float aHardness) {
    if (aRadius <= 0.0f || aDistance >= aRadius)
        return 0.0f;
    const float hardness = std::min(std::max(aHardness, 0.0f), 1.0f);
    const float core = aRadius * hardness;
    if (aDistance <= core)
        return 1.0f;
    // core < distance < radius, and core < radius here, so the divisor is positive
    const float t = (aRadius - aDistance) / (aRadius - core); // 1 at the core, 0 at the rim
    return t * t * (3.0f - 2.0f * t);
}

// Turns cursor samples into a spaced stream of stamps. Without this every tool
// stamped once per drag EVENT, so a fast stroke came out as dots - the defect the
// FFD brush has carried since it was written.
//
// begin() stamps once at the press point; move() walks the segment from the last
// stamp position in BrushParam::stampStep() steps, interpolating the pen pressure
// between the two samples so a pressure ramp is a ramp and not two steps; end()
// closes the stroke and emits nothing more.
class BrushStroke: private util::NonCopyable {
public:
    using Sink = std::function<void(const BrushStamp&)>;

    void begin(const BrushParam& aParam, const QVector2D& aPos, float aPressure, Sink aSink);
    void move(const QVector2D& aPos, float aPressure);
    void end();

    bool isActive() const { return mActive; }
    // how many stamps the current stroke has emitted (the gates read it)
    int stampCount() const { return mStampCount; }

private:
    // (`emit` is Qt's keyword macro, so the internal stamp emitter is named out)
    void stampAt(const QVector2D& aPos, float aPressure);

    BrushParam mParam;
    Sink mSink;
    QVector2D mLastPos;
    QVector2D mLastStampPos;
    float mLastPressure = 1.0f;
    // distance from mLastPos to the next stamp that is due
    float mNextStamp = 0.0f;
    bool mActive = false;
    int mStampCount = 0;
};

} // namespace ctrl

#endif // CTRL_BRUSH_H
