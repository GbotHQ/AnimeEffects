#ifndef CTRL_GRAPH_CURVE_H
#define CTRL_GRAPH_CURVE_H

#include <QPointF>
#include <QVector>
#include "core/TimeKey.h"
#include "core/TimeLine.h"
#include "util/Range.h"
#include "util/Easing.h"
#include "ctrl/graph/graph_Channel.h"

namespace ctrl {
namespace graph {

// One channel's curve over one layer: the keys, the segments between them, and
// the bezier tangent handles a Custom easing exposes.
//
// Sampling mirrors core::TimeKeyBlender EXACTLY, and that is the whole point of
// this class: the graph has to draw what playback plays. The rate therefore
// always comes from util::Easing::calculate with the arguments the blender
// passes (t = frame - p0.frame, b = 0, c = 1, d = p1.frame - p0.frame taken from
// the `from` key's easing), and the Move channel goes through the same
// util::FergusonCoonsSpline with the same Catmull-Rom velocities, so a spatial
// move path keeps its curvature in the graph.
//
// The curve does not own the timeline: it reads the key map on demand, so it
// stays correct while the document is edited underneath it.
class Curve {
public:
    Curve();
    Curve(const core::TimeLine& aTimeLine, const Channel& aChannel);

    const Channel& channel() const { return mChannel; }
    // The layer this curve belongs to.
    const core::TimeLine& timeLine() const { return *mTimeLine; }
    bool isEmpty() const { return keys().isEmpty(); }

    // The channel's keys in frame order (empty when the map holds none).
    QVector<const core::TimeKey*> keys() const;
    // The value stored on aKey for this channel.
    float valueOf(const core::TimeKey& aKey) const { return mChannel.value(aKey); }

    // The value the document holds at aFrame - the same number playback
    // evaluates, including the no-key, single-key and step-held cases.
    float sample(core::Frame aFrame) const;

    // One drawable segment: the two bounding keys and the easing that shapes the
    // curve between them (always the `from` key's easing, as in playback).
    struct Segment {
        const core::TimeKey* from;
        const core::TimeKey* to;
        float fromValue;
        float toValue;
        util::Easing::Param easing;
    };
    int segmentCount() const;
    // The drawable segment at aIndex (0 <= aIndex < segmentCount()).
    Segment segment(int aIndex) const;

    // A Custom easing's two tangent handles, in NORMALIZED easing space: x in
    // [0,1] along the segment (its unit of time) and y free to overshoot the
    // segment's value span. That normalization is what makes a handle mean the
    // same thing at any zoom, and it is the space core::TimeKeyBlender consumes.
    // aHandleIndex 0 is the handle leaving `from`, 1 the handle arriving at `to`.
    bool isCustom(const Segment& aSegment) const;
    static QPointF handle(const Segment& aSegment, int aHandleIndex);
    // Writes a handle back into an easing param: x is clamped into [0,1] (a
    // cubic bezier requires it) while y stays free, so overshoot remains
    // expressible.
    static void setHandle(util::Easing::Param& aEasing, int aHandleIndex, const QPointF& aNormalized);

    // A Type_Auto segment's control points, which are DERIVED rather than stored
    // (util::Easing::autoHandles): x is always the third of the segment's span
    // (1/3 and 2/3, Blender's automatic handle) and only the two y values come
    // from the keys AROUND the segment, so the pair re-shapes itself when a
    // neighbour changes - which is why it cannot live in the key's param. These
    // are the GHOST handles: real control points the panel can map with
    // graph_Geometry's handlePointAt and draw as a SOLID dot at the smaller
    // radius (kHandleRadius - kGhostHandleShrink, 3.5px) filled with the
    // channel's reduced-chroma hue, next to a Custom pair's larger (5px) dot
    // filled with the raised surface. The pair's radius and fill are the tell,
    // not a hollow centre.
    //
    // The pair is returned in the SAME normalized easing space handle() reads a
    // Custom pair from, handle 0 first, and it is the pair the DRAWING rate uses:
    // the same derivation sample() evaluates with (core::autoEasingContext +
    // autoHandles over the same neighbours), so the ghost dot sits on the control
    // point of the curve the panel draws.
    //
    // aFrom is the segment's FROM key - the key whose easing shapes the segment,
    // exactly as handle() reads. EMPTY when there is nothing to derive: a segment
    // whose easing is not Type_Auto, a key with no successor in its channel (the
    // last key), and a channel of a type that carries no easing at all.
    QVector<QPointF> ghostHandles(const core::TimeKey& aFrom) const;

    // Polyline of the channel across [aFrom,aTo] in (frame, value) space; the caller maps it through
    // the Scaler. aSamplesPerSegment bounds the error of the analytic curves (one sample per frame is
    // NOT enough: Expo and Elastic are steep inside a single frame at high zoom) and is spent on the
    // part of each segment the window SHOWS, plus each touched segment's own endpoints for the caller
    // to clip against. The frames outside the keys follow setLoop below.
    void buildPolyline(core::Frame aFrom, core::Frame aTo, int aSamplesPerSegment, QVector<QPointF>& aOut) const;

    // How the frames OUTSIDE the keys are evaluated, which is the project's own playback rule
    // (core::TimeKeyGatherer reads core::TimeInfo the same way):
    //   * inside the frame range, loop ON  - the value is the eased segment from the LAST key back
    //     to the FIRST, wrapped by the range's span (the animation repeats over the range, so the
    //     first key sits one span ahead of the last);
    //   * inside it, loop OFF - the value HOLDS: the first key before it, the last key after it;
    //   * OUTSIDE the range the frame is CLAMPED into it, so the curve is flat at the range's edge
    //     value - the animation is defined over the range and a repeated period is not something a
    //     curve should extrapolate to infinity.
    // A single key, or a range with no span, means no wrap.
    void setLoop(bool aLoop, const util::Range& aRange);
    bool isLooped() const;


    // The easing param that shapes the segment starting at one key, for a
    // graphable type. Null for a type that carries no easing.
    static const util::Easing::Param* easingOf(const core::TimeKey& aKey, core::TimeKeyType aType);
    static util::Easing::Param* easingOf(core::TimeKey& aKey, core::TimeKeyType aType);

private:
    // The key whose segment starts at or before aFrame, and its successor.
    // Mirrors the gatherer's no-key / single / sandwiched cases.
    const core::TimeKey* findLower(core::Frame aFrame) const;
    // The keys either side of the segment aFrom -> aTo in this channel's map:
    // the blender gatherer's point(-1) / point(2). Either is null at an end of
    // the key list. They are what a Type_Auto easing derives its handles from,
    // and what the Move spline takes its Catmull-Rom velocities from.
    void neighborsOf(const core::TimeKey& aFrom, const core::TimeKey& aTo,
                     const core::TimeKey*& aPrev, const core::TimeKey*& aNext) const;
    float sampleBetween(const core::TimeKey& aFrom, const core::TimeKey& aTo, core::Frame aFrame) const;
    // The value aAt (a float frame) inside the pair aFrom/aTo over a length of aD frames: the easing
    // belongs to aFrom, and aD is the pair's effective length - the frames' own difference, or the
    // loop span for a wrapped pair.
    float sampleOver(const core::TimeKey& aFrom, const core::TimeKey& aTo, float aAt, float aD) const;

    const core::TimeLine* mTimeLine;
    Channel mChannel;
    bool mLoop;
    util::Range mLoopRange;
};

} // namespace graph
} // namespace ctrl

#endif // CTRL_GRAPH_CURVE_H
