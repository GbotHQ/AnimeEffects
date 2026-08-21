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

    class Renderer {
    public:
        Renderer(QPainter& aPainter, const core::CameraInfo& aCamera, const theme::TimeLine& aTheme,
            const core::TimeFormatType& timeFormat);

        void setMargin(int aMargin) { mMargin = aMargin; }
        void setRange(const util::Range& aRange) { mRange = aRange; }
        void setTimeScale(const Scaler& aScale) { mScale = &aScale; }
        // The effective playback range (the project frame range, or the
        // temporary window while it overrides): the ruler fades everything
        // outside it, and the range markers recede while the window is
        // active. The window's own visuals come from setPlayWindow.
        void setActiveRange(const util::Range& aRange) { mActiveRange = aRange; }
        // The temporary playback window (see ctrl::TimeLineEditor::PlayWindow);
        // the header draws its band and brackets, the range markers dim.
        void setPlayWindow(const ctrl::TimeLineEditor::PlayWindow& aWindow) { mPlayWindow = aWindow; }

        void renderLines(const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect);
        void renderRangeMarkers(const QRect& aCameraRect);
        void renderHeader(int aHeight, int aFps);
        void renderPlayWindow(const QRect& aCameraRect, int aHeight, int aFps);
        void renderHandle(const QPoint& aPoint, int aRange);
        void renderSelectionRange(const QRect& aRect);

    private:
        void drawTimeCurrent();
        void drawKeys(const core::ObjectNode* aNode, const TimeLineRow& aRow);
        void drawChildKeys(const core::ObjectNode* aNode, const QPoint& aPos);
        void drawPillText(const QRect& aPill, const QString& aText, const QColor& aColor, int aTop);

        QPainter& mPainter;
        const core::CameraInfo& mCamera;
        const theme::TimeLine& mTheme;
        int mMargin;
        util::Range mRange;       // the visible frame range (culling)
        util::Range mActiveRange; // the effective playback range (fade basis)
        const Scaler* mScale;
        ctrl::TimeLineEditor::PlayWindow mPlayWindow;
        const core::TimeFormatType timeFormatVar;
    };

} // namespace time
} // namespace ctrl

#endif // CTRL_TIME_RENDERER_H
