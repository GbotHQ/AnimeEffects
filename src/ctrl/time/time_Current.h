#ifndef CTRL_TIME_CURRENT_H
#define CTRL_TIME_CURRENT_H

#include <QPoint>
#include "core/Frame.h"
#include "util/Range.h"

namespace ctrl {
namespace time {

    class Scaler;

    // The playhead. It is bounded by the effective playback range (the
    // project frame range, or the temporary window while it overrides): both
    // setFrame (transport, keyboard, the playback tick) and setHandlePos
    // (dragging, scrubbing) clamp into the range passed per call, so every
    // playhead movement respects the active range through this one point.
    // The scale itself never clamps — keys and the view may live anywhere.
    class Current {
    public:
        Current(int aLeftMargin);
        void setFrame(const Scaler& aScale, const util::Range& aRange, core::Frame aFrame);
        void setHandlePos(const Scaler& aScale, const util::Range& aRange, const QPoint& aPos);
        void update(const Scaler& aScale);
        core::Frame frame() const { return mFrame; }
        QPoint handlePos() const { return mPos; }
        int handleRange() const { return 5; }

    private:
        const int mLeftMargin;
        core::Frame mFrame;
        QPoint mPos;
    };

} // namespace time
} // namespace ctrl

#endif // CTRL_TIME_CURRENT_H
