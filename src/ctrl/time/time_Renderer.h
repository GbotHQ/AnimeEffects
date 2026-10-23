#ifndef CTRL_TIME_RENDERER_H
#define CTRL_TIME_RENDERER_H

#include <QVector>
#include <QPainter>
#include <QSettings>
#include "util/Range.h"
#include "core/CameraInfo.h"
#include "core/ObjectNode.h"
#include "core/TimeFormat.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/TimeLineRow.h"
#include "ctrl/time/time_Scaler.h"
#include "gui/theme/TimeLine.h"

namespace ctrl {
    constexpr float radiiFactor = 1.25;
    constexpr float childRadiiFactor = 1.0;
    constexpr double keyRadius = 3.0 * radiiFactor;
    constexpr double keyRadiusSlim = 2.0 * radiiFactor;
    constexpr double keyRadiusChild = 3.0 * childRadiiFactor;
    constexpr double keyRadiusChildPoly = 5.0 * childRadiiFactor;
namespace time {

    // THE SELECTED KEY'S RING - one rule for BOTH views, so it is named here and
    // read by the graph's marker pass too (gui::drawPanel):
    //   * ink   - the key's OWN channel hue at kSelectRingAlpha (0.85), 1px wide;
    //   * place - an ellipse centred on the marker whose radius is the marker's own
    //             radius PLUS kSelectRingOffset, drawn OUTSIDE the marker so the
    //             fill keeps its size. The graph's marker radius is
    //             ctrl::graph::kKeyDotRadius, pinned to keyRadius, so the two
    //             engines put the same band at the same distance.
    // The timeline's pen is centred on the radius (QPainter); the graph's ImGui
    // circle strokes INWARD, so it adds half a pixel (see its marker pass). That
    // half pixel is the only difference, and it is the engines' stroke convention,
    // not the rule's.
    constexpr int kSelectRingAlpha = 217;      // 0.85 of the channel hue
    constexpr double kSelectRingOffset = 2.6;  // px past the marker's own radius

    // THE PROPORTIONAL KEY'S FILL RULE: a key whose frame/value follow its absolute
    // neighbours is painted in the same shape and hue as any other key, but its fill
    // is drawn at 45% opacity (115/255) so the stored, derived position reads as a
    // softer body. Nothing else changes - not the radius, not the hue, not the edge,
    // and not the selection ring (which stays at kSelectRingAlpha). The graph panel's
    // marker pass applies the same factor to its own marker fill (its
    // kKeyProportionalFillAlpha, 0.45), so one proportional key cannot be inked two
    // ways in the two views.
    constexpr int kProportionalFillAlpha = 115; // 0.45 of the channel hue

    class Renderer {
    public:
        Renderer(QPainter& aPainter, const core::CameraInfo& aCamera, const theme::TimeLine& aTheme);

        void setMargin(int aMargin) { mMargin = aMargin; }
        void setRange(const util::Range& aRange) { mRange = aRange; }
        void setTimeScale(const Scaler& aScale) { mScale = &aScale; }
        // The project frame range: the range markers sit at its first and
        // last frame's ticks. The temporary window dims the markers while it
        // overrides; it does not move them.
        void setProjectRange(const util::Range& aRange) { mProjectRange = aRange; }
        // The temporary playback window (see ctrl::TimeLineEditor::PlayWindow);
        // the range markers dim while it is active.
        void setPlayWindow(const ctrl::TimeLineEditor::PlayWindow& aWindow) { mPlayWindow = aWindow; }

        // The track background: fills, lane separators, type labels and the
        // row seams. Keys are a separate pass (renderKeys) so background
        // layers like the range markers can paint between them.
        void renderLines(const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect);
        void renderRangeMarkers(const QRect& aCameraRect);
        // Key graphics for every row: child-key summaries on closed folders
        // and the per-lane key shapes.
        void renderKeys(const QVector<TimeLineRow>& aRows, const QRect& aCullRect);
        void renderSelectionRange(const QRect& aRect);

    private:
        void drawTimeCurrent();
        void drawKeys(const core::ObjectNode* aNode, const TimeLineRow& aRow);
        void drawChildKeys(const core::ObjectNode* aNode, const QPoint& aPos);

        QPainter& mPainter;
        const core::CameraInfo& mCamera;
        const theme::TimeLine& mTheme;
        int mMargin;
        util::Range mRange;        // the visible frame range (culling)
        util::Range mProjectRange; // the project frame range (range markers)
        const Scaler* mScale;
        ctrl::TimeLineEditor::PlayWindow mPlayWindow;
    };

} // namespace time
} // namespace ctrl

#endif // CTRL_TIME_RENDERER_H
