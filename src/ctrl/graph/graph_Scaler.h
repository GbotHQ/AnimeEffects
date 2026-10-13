#ifndef CTRL_GRAPH_SCALER_H
#define CTRL_GRAPH_SCALER_H

#include <QRect>
#include "core/Frame.h"
#include "ctrl/time/time_Scaler.h"

namespace ctrl {
namespace graph {

// The graph panel's two axes: frames horizontally, values vertically.
//
// The frame axis DELEGATES to ctrl::time::Scaler, so the graph's frame grid, its
// zoom ladder and its tick classification are the timeline's - one convention
// for "how far apart are frames", not two. It is the graph's OWN instance
// (mFrameScale), seeded from the project's fps and frame range but never shared
// with the timeline's scaler, so fitting or zooming the graph's frame axis moves
// the graph's frame window and leaves the timeline's zoom exactly where it was.
// What this class adds is the panel's own origin (a free pixel offset, which is
// what lets a wheel zoom stay anchored on the cursor) and the value window.
//
// Frame 0 sits at plotRect().left() + frameOffset() by default; the offset is
// free, so panning can put any frame anywhere.
class Scaler {
public:
    // Panel layout. Public so the renderer and the hit-test share one geometry
    // instead of each carrying its own copy of the insets.
    static constexpr int kMargin = 4;          // inset of the plot inside the panel
    static constexpr int kAxisLabelWidth = 46; // left column carrying value labels

    Scaler();

    // The panel rect, in viewport pixels.
    void setRect(const QRect& aRect);
    const QRect& rect() const { return mRect; }
    // Where curves are drawn: the rect minus the margins and the label column.
    QRect plotRect() const;

    // --- frame axis (ctrl::time::Scaler owns the ladder) ---
    ctrl::time::Scaler& frameScale() { return mFrameScale; }
    const ctrl::time::Scaler& frameScale() const { return mFrameScale; }
    int pixelX(core::Frame aFrame) const;
    core::Frame frameAt(int aPixelX) const;
    double frameAtPixel(double aPixelX) const;
    double pixelXOfFrame(double aFrame) const;
    // Zooms the frame axis by a wheel delta so the frame under aAnchorPixelX
    // stays under it (the timeline's anchoring rule). Returns false when the
    // zoom is already clamped at the floor or ceiling and nothing moved.
    bool updateFrameZoom(int aWheelDelta, int aAnchorPixelX);
    // Pans the frame axis by a pixel delta.
    void panFrame(int aPixelDeltaX);
    // Fits the frame window so every frame in [aLo,aHi] lands inside
    // plotRect(), with a small pad at both ends. This is the graph's OWN frame
    // axis - its own ctrl::time::Scaler instance - so the fit moves the graph's
    // frame zoom rung and origin and leaves the timeline untouched. The rung is
    // the largest the axis can show the span at; a span wider than the axis'
    // floor (1 px/frame) allows is centred at that floor rather than refused.
    // Does nothing when the plot has no width.
    void fitFrameAxis(core::Frame aLo, core::Frame aHi);

    // --- value axis ---
    void setValueWindow(float aMin, float aMax);
    float valueMin() const { return mValueMin; }
    float valueMax() const { return mValueMax; }
    // Fits [aMin,aMax] with padding on both sides, and widens a degenerate span
    // so an all-constant channel still has a usable scale.
    void fitValueWindow(float aMin, float aMax);
    int pixelY(float aValue) const;
    float valueAt(int aPixelY) const;
    double valueAtPixel(double aPixelY) const;
    // Zooms the value axis by aFactor about aAnchorPixelY (aFactor > 1 zooms in).
    // Returns false when the resulting window would be degenerate.
    bool updateValueZoom(float aFactor, int aAnchorPixelY);
    // Pans the value axis by a pixel delta (positive moves the window down).
    void panValue(int aPixelDeltaY);

private:
    double plotTop() const;
    double plotHeight() const;

    ctrl::time::Scaler mFrameScale;
    QRect mRect;
    double mFrameOffset; // content pixel of frame 0, relative to plotRect().left()
    float mValueMin;
    float mValueMax;
};

} // namespace graph
} // namespace ctrl

#endif // CTRL_GRAPH_SCALER_H
