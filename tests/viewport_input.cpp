// THE VIEWPORT'S INPUT ROUTER, DRIVEN WITHOUT A WINDOW.
//
// Two defects were reported against the live application and neither had a gate:
//
//   "the brush overlay only follows the mouse when some mouse buttons are pressed"
//       -> a move with NO gesture returned false and the tool never saw the sample
//   "when drawing it only does one tap instead of keeping drawing while lmb is down"
//       -> the drag's buttons came from the QMouseEvent's `buttons()`, which is only populated for
//          the widget Qt routed the press to
//
// This test builds a fake ViewportHost, so every call the router makes - how many samples reached the
// tool, where they were, whether a button was reported - is observable. It uses a REAL
// core::AbstractCursor, because that is what the router reads the pressed state from.
#include <QCoreApplication>
#include <QMouseEvent>
#include <Qt>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/AbstractCursor.h"
#include "core/CameraInfo.h"
#include "core/RenderInfo.h"
#include "gui/ViewportInput.h"
#include "XCAssert.h"

// libcore's Project.cpp calls the AUDIO widget's project serializer. It is irrelevant to pointer
// routing, and compiling that widget would drag its whole tool-panel dependency into a headless test,
// so the one symbol is stubbed here instead.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

// A failing XC_ASSERT or XC_FATAL_ERROR is a test failure, not a crash log (the stanza every test
// that links libcore needs).
namespace {
class TestAssertHandler: public XCAssertHandler {
public:
    void failure() const override {
        std::printf("FAIL: XC_ASSERT\n");
        std::exit(1);
    }
};

class TestErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString& aInfo, const QString&) const override {
        std::printf("FAIL: XC_FATAL_ERROR %s\n", aInfo.toUtf8().data());
        std::exit(1);
    }
};

TestAssertHandler gAssertHandler;
TestErrorHandler gErrorHandler;
} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

namespace {

int gFailures = 0;

void check(bool aCondition, const char* aWhat) {
    if (!aCondition) {
        std::printf("FAIL %d: %s\n", __LINE__, aWhat);
        ++gFailures;
    }
}

struct Fed {
    QPointF pos;
    bool leftDown = false;
    bool tablet = false;
};

// The host: records what the router fed the tool, and nothing else.
struct FakeHost: gui::ViewportHost {
    core::RenderInfo info;
    core::AbstractCursor mCursor;
    core::CameraInfo cameraInfo;
    int updates = 0;
    int frames = 0;
    int modifierForwards = 0;
    std::vector<Fed> fed;

    core::RenderInfo& renderInfo() override { return info; }
    core::CameraInfo& camera() override { return cameraInfo; }
    core::AbstractCursor& cursor() override { return mCursor; }
    ctrl::MaskEditor* maskEditor() override { return nullptr; }
    void forwardModifiers(Qt::KeyboardModifiers) override { ++modifierForwards; }
    ctrl::ToolType toolType() const override { return mTool; }
    const ctrl::BrushParam* activeBrush() const override { return nullptr; }
    void updateCursorNow() override {
        ++updates;
        fed.push_back(Fed{mCursor.screenPos().toPointF(), mCursor.isPressedLeft(), false});
    }
    void requestFrame() override { ++frames; }
    void sampleMaskValue(float) override {}
    void beginBrushDial(const QPointF&) override {}
    void stepBrushDial(const QPointF&) override {}
    bool brushDialActive() const override { return false; }
    void endBrushDial() override {}
    QPoint mapToGlobal(const QPoint& aPos) override { return aPos; }
    void setGrabCursor(bool) override {}
    void addDirty(const QRect&) override {}
    QRect viewportRect() const override { return QRect(0, 0, 640, 480); }

    ctrl::ToolType mTool = ctrl::ToolType_SRT;
};

gui::PointerSample sample(QPointF aPos, Qt::MouseButton aButton, Qt::MouseButtons aButtons) {
    gui::PointerSample s;
    s.pos = aPos;
    s.button = aButton;
    s.buttons = aButtons;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- 1. HOVER: a move with no gesture still reaches the tool -----------------------------
    {
        FakeHost host;
        host.mTool = ctrl::ToolType_Mask;
        gui::ViewportInput input(host);

        const bool taken = input.move(sample(QPointF(120, 90), Qt::NoButton, Qt::NoButton));
        check(taken, "a hover move is handled by the router");
        check(host.fed.size() == 1, "the tool was fed for a hover (the brush ring follows the pointer)");
        check(!host.fed.empty() && !host.fed.back().leftDown, "the hover reported no button");
        check(host.frames >= 1, "the hover asked for a frame");
    }

    // ---- 2. A BRUSH STROKE IS EVERY SAMPLE, WHILE THE BUTTON IS DOWN ------------------------
    {
        FakeHost host;
        host.mTool = ctrl::ToolType_Mask;
        host.cameraInfo.setScreenSize(QSize(640, 480));
        host.cameraInfo.setImageSize(QSize(640, 480));
        host.cameraInfo.setScale(1.0f);
        gui::ViewportInput input(host);

        // the router presses the cursor itself (the test must not do it twice)
        input.press(sample(QPointF(100, 100), Qt::LeftButton, Qt::LeftButton));

        const size_t afterPress = host.fed.size();
        check(afterPress == 1, "the press fed the tool once");

        // five moves: EVERY one must reach the tool, and the cursor must report the left button -
        // even though the samples carry `buttons` empty, which is what the event looked like when
        // the press had been consumed elsewhere (the reported "one tap" defect)
        for (int i = 1; i <= 5; ++i)
            input.move(sample(QPointF(100.0 + i * 7.0, 100.0), Qt::NoButton, Qt::NoButton));

        check(host.fed.size() == afterPress + 5, "each drag sample reached the tool (not one tap)");
        check(!host.fed.empty() && host.fed.back().leftDown,
              "the drag was fed with the cursor's left button down");
        check(!host.fed.empty() && host.fed.back().pos == QPointF(135.0, 100.0),
              "the last fed sample is the last pointer position");

        input.release(sample(QPointF(135, 100), Qt::LeftButton, Qt::NoButton));
        check(input.isBusy() == false, "the release ended the gesture");
    }

    // ---- 3. A STATE-ONLY GESTURE IS ONE SAMPLE PER FRAME ------------------------------------
    {
        FakeHost host;
        host.mTool = ctrl::ToolType_SRT;
        host.cameraInfo.setScreenSize(QSize(640, 480));
        host.cameraInfo.setImageSize(QSize(640, 480));
        host.cameraInfo.setScale(1.0f);
        gui::ViewportInput input(host);

        input.press(sample(QPointF(50, 50), Qt::LeftButton, Qt::LeftButton));
        const size_t afterPress = host.fed.size();

        for (int i = 1; i <= 5; ++i)
            input.move(sample(QPointF(50.0 + i * 10.0, 50.0), Qt::NoButton, Qt::LeftButton));
        check(host.fed.size() == afterPress, "an SRT drag is HELD, not applied per sample");

        check(input.frameTick(), "the frame fed the held sample");
        check(host.fed.size() == afterPress + 1, "exactly one sample per frame");
        check(!host.fed.empty() && host.fed.back().pos == QPointF(100.0, 50.0),
              "the frame applies the LATEST pointer position");
    }

    // ---- 4. A PEN IS ALWAYS `Every` ---------------------------------------------------------
    {
        FakeHost host;
        host.mTool = ctrl::ToolType_Mask;
        gui::ViewportInput input(host);
        gui::PointerSample pen = sample(QPointF(10, 10), Qt::LeftButton, Qt::LeftButton);
        pen.precise = true;
        input.press(pen);
        check(input.sampling() == gui::Sampling::Every, "a pen press is Every, whatever the tool");
    }

    if (gFailures == 0)
        std::printf("viewport_input: hover feeds, a stroke is every sample, a drag is one per frame\n");
    return gFailures == 0 ? 0 : 1;
}
