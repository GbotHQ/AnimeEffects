#ifndef CTRL_GRAPH_GEOMETRY_H
#define CTRL_GRAPH_GEOMETRY_H

#include <QPoint>
#include <QPointF>
#include <QRect>
#include "core/Frame.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Scaler.h"

namespace ctrl {
namespace graph {

// THE ONE HOME OF THE PANEL'S SHARED GEOMETRY.
//
// The animation graph panel is DRAWN by one layer (gui::drawPanel, fed by
// gui::GraphPanelModel) and PICKED by another (ctrl::GraphEditor::hitTest), and
// the two have to agree to the pixel: a drawing layer that computes its own
// rects silently desyncs from the pick, so a user grabs something invisible or
// the visible thing does not respond. That is the whole reason this header
// exists, and it is why the geometry lives here rather than beside either
// consumer:
//   * the editor's hit-test reads kKeyHitRadius / kHandleHitRadius and the
//     keyRect() / handlePoint() mappings below;
//   * the model publishes the drawing geometry FROM these same functions
//     (keyRect() for a key square, handlePoint() for a tangent handle's dot);
//   * the drawing layer draws only what the model published, so the drawn item
//     and the clickable item are one geometry by construction, not by a
//     coincidence two copies of the arithmetic would have to keep.
//
// Nothing here holds state and nothing reads the document: every entry is a
// pure mapping through the panel's Scaler, or a radius. Curve::handle() - the
// NORMALIZED handle space these functions map through - stays on graph::Curve,
// where the easing that defines it lives; this header maps that space to pixels.
//
// A key is a square of side 2 * kKeyRadius centred on the mapped point, and its
// grab radius is generous (like the timeline's keys): kKeyHitRadius. A Custom
// easing's tangent handle is a dot of radius kHandleRadius, grabbed within
// kHandleHitRadius - the easing picker's own handles are grabbed generously too.

// The drawn half-side of a key square, in pixels.
constexpr int kKeyRadius = 5;
// The grab radius of a key, in pixels. The hit-test compares a press against
// this with `<=`, so a press EXACTLY this far from a key's centre still hits.
constexpr int kKeyHitRadius = 10;
// The drawn radius of a tangent handle's dot, in pixels.
constexpr int kHandleRadius = 5;
// The grab radius of a tangent handle, in pixels. Deliberately larger than a
// key's own kKeyHitRadius, because a handle is the more specific target of the
// two and the hit-test resolves a handle first.
constexpr int kHandleHitRadius = 12;

// The pixel rect of a key at (frame, value): a square of side 2 * kKeyRadius
// centred on the mapped point. The hit-test and the model both use this, so the
// clickable key is exactly the drawn key.
inline QRect keyRect(const Scaler& aScale, core::Frame aFrame, float aValue) {
    const QPoint center(aScale.pixelX(aFrame), aScale.pixelY(aValue));
    return QRect(center.x() - kKeyRadius, center.y() - kKeyRadius, 2 * kKeyRadius, 2 * kKeyRadius);
}

// Where a Custom easing's tangent handle lands in panel pixels. x normalizes
// along the segment's frame span and y across its value span - the space
// Curve::handle() stores handles in, because it is the space playback consumes -
// so the pixel is that normalized point pushed through the two axes.
//
// aHandleIndex 0 leaves the segment's `from` key and 1 arrives at its `to` key.
// The editor's hit-test grabs a handle here and the model publishes its dot
// here, so a grab at the dot is the drag the editor performs.
inline QPointF handlePoint(const Scaler& aScale, const Curve::Segment& aSegment, int aHandleIndex) {
    const QPointF normalized = Curve::handle(aSegment, aHandleIndex);
    const double frameSpan = static_cast<double>(aSegment.to->frame() - aSegment.from->frame());
    const double valueSpan = static_cast<double>(aSegment.toValue - aSegment.fromValue);

    const double frame = static_cast<double>(aSegment.from->frame()) + normalized.x() * frameSpan;
    const float value = aSegment.fromValue + static_cast<float>(normalized.y() * valueSpan);
    return QPointF(aScale.pixelXOfFrame(frame), static_cast<double>(aScale.pixelY(value)));
}

} // namespace graph
} // namespace ctrl

#endif // CTRL_GRAPH_GEOMETRY_H
