// The Transform tool's key policy, headless.
//
// The tool used to own a key for every channel missing at the playhead and push it
// on the first drag: a static object grew a Move key for no reason, and unchecking
// all three "Create ... key" boxes changed nothing. The policy now matches the
// inspector's model-2: a channel with an explicit key elsewhere auto-keys at the
// playhead, a static channel edits the base value, and a checked box forces an
// explicit key. This gate drives the real SRTEditor/MoveMode with a synthetic
// cursor, so it pins the actual key maps and the undo command.
#include <QApplication>
#include <QMouseEvent>
#include <QTextStream>
#include <cstdio>
#include <memory>

#include "core/AbstractCursor.h"
#include "core/CameraInfo.h"
#include "core/FolderNode.h"
#include "core/MoveKey.h"
#include "core/Project.h"
#include "core/TimeKeyExpans.h"
#include "core/RotateKey.h"
#include "core/Animator.h"
#include "ctrl/SRTEditor.h"
#include "ctrl/TimeLineUtil.h"
#include "ctrl/UILogger.h"
#include "XCAssert.h"
#include "XCReport.h"

// Project.cpp references this GUI symbol only for the optional .aemus sidecar.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

class HarnessAssertHandler : public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler : public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;
XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

namespace {

int failures = 0;
void check(bool aOk, const char* aWhat) {
    std::printf("%s  %s\n", aOk ? "PASS" : "FAIL", aWhat);
    if (!aOk) ++failures;
}

class TestAnimator : public core::Animator {
public:
    core::Frame currentFrame() const override { return mFrame; }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
    void setFrame(int aFrame) { mFrame = core::Frame(aFrame); }

private:
    core::Frame mFrame;
};

class TestLogger : public ctrl::UILogger {
public:
    void pushLog(const QString&, ctrl::UILogType) override {}
};

struct Fixture {
    TestAnimator animator;
    core::Project project;
    core::FolderNode* folder = nullptr;
    ctrl::SRTEditor* editor = nullptr;
    TestLogger logger;
    core::CameraInfo camera;
    core::AbstractCursor cursor;

    explicit Fixture(int aFrame = 10): project(QStringLiteral("srt_keys.anie"), animator, nullptr) {
        animator.setFrame(aFrame);
        camera.reset(QSize(800, 600), 1.0, QSize(800, 600), QPoint(0, 0));
        folder = new core::FolderNode(QStringLiteral("folder"));
        folder->setDefaultPosture(QVector2D(100.0f, 200.0f));
        editor = new ctrl::SRTEditor(project, logger);
    }
    ~Fixture() {
        delete editor;
        delete folder;
    }

    ctrl::SRTParam param(bool aMove = false, bool aRotate = false, bool aScale = false) {
        ctrl::SRTParam p;
        p.necessarilyMove = aMove;
        p.necessarilyRotate = aRotate;
        p.necessarilyScale = aScale;
        return p;
    }

    void target(const ctrl::SRTParam& aParam) {
        editor->updateParam(aParam);
        editor->setTarget(folder);
    }

    // The symbol's translation handle is the node's own origin. A press on it
    // focuses Translation; the move supplies a world velocity.
    void dragTranslation(float aDx, const QVector2D& aWorldCenter = QVector2D(100.0f, 200.0f)) {
        const QPointF c = camera.toScreenPos(aWorldCenter).toPointF();
        const QPointF moved = c + QPointF(aDx, 0.0);
        QMouseEvent press(QEvent::MouseButtonPress, c, c, c, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        cursor.setMousePress(&press, camera);
        editor->updateCursor(camera, cursor);

        QMouseEvent move(QEvent::MouseMove, moved, moved, moved, Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        cursor.setMouseMove(&move, camera);
        editor->updateCursor(camera, cursor);

        QMouseEvent release(QEvent::MouseButtonRelease, moved, moved, moved, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        cursor.setMouseRelease(&release, camera);
        editor->updateCursor(camera, cursor);
    }

    core::TimeLine& line() { return *folder->timeLine(); }
};

void testStaticEditsDefault() {
    Fixture f;
    f.target(f.param());
    const QVector2D before = static_cast<const core::MoveKey*>(
        f.line().baseValue(core::TimeKeyType_Move))->pos();
    f.dragTranslation(40.0f);
    check(f.line().map(core::TimeKeyType_Move).isEmpty(), "static: drag creates no explicit Move key");
    const QVector2D after = static_cast<const core::MoveKey*>(
        f.line().baseValue(core::TimeKeyType_Move))->pos();
    check(after != before, "static: the base Move value is what the drag edits");
    f.project.commandStack().undo();
    const QVector2D undone = static_cast<const core::MoveKey*>(
        f.line().baseValue(core::TimeKeyType_Move))->pos();
    check(undone == before, "static: one undo restores the base value");
    f.project.commandStack().redo();
}

void testExistingKeyEditsInPlace() {
    Fixture f;
    auto* key = new core::MoveKey();
    key->setPos(QVector2D(100.0f, 200.0f));
    key->setCentroid(QVector2D(0.0f, 0.0f));
    ctrl::TimeLineUtil::pushNewMoveKey(f.project, *f.folder, 10, key);
    const int sizeBefore = f.line().map(core::TimeKeyType_Move).size();
    f.editor->updateParam(f.param());
    f.editor->setTarget(f.folder);
    const QVector2D before = static_cast<const core::MoveKey*>(
        f.line().timeKey(core::TimeKeyType_Move, 10))->pos();
    f.dragTranslation(30.0f);
    check(f.line().map(core::TimeKeyType_Move).size() == sizeBefore,
          "existing key: drag does not add a second key");
    const QVector2D after = static_cast<const core::MoveKey*>(
        f.line().timeKey(core::TimeKeyType_Move, 10))->pos();
    check(after != before, "existing key: the key at the playhead is edited");
}

void testAnimatedChannelAutoKeys() {
    Fixture f(10);
    auto* other = new core::MoveKey();
    other->setPos(QVector2D(300.0f, 200.0f));
    ctrl::TimeLineUtil::pushNewMoveKey(f.project, *f.folder, 20, other);
    f.editor->updateParam(f.param());
    f.editor->setTarget(f.folder);
    f.dragTranslation(30.0f, f.line().current().srt().pos());
    check(f.line().map(core::TimeKeyType_Move).size() == 2,
          "animated: a drag at another frame creates a key there");
    check(f.line().timeKey(core::TimeKeyType_Move, 10) != nullptr,
          "animated: and the new key is at the playhead");
}

void testCheckboxForcesAKey() {
    Fixture f;
    f.target(f.param(true));
    f.dragTranslation(30.0f, f.line().current().srt().pos());
    check(f.line().map(core::TimeKeyType_Move).size() == 1,
          "checked Move: the drag creates the explicit key");
    check(f.line().timeKey(core::TimeKeyType_Move, 10) != nullptr,
          "checked Move: at the playhead");
}

void testOtherChannelCheckboxCreatesOnlyThatKey() {
    Fixture f;
    f.target(f.param(false, true));
    f.dragTranslation(30.0f);
    check(f.line().map(core::TimeKeyType_Move).isEmpty(),
          "checked Rotate: the move stays static");
    check(f.line().map(core::TimeKeyType_Rotate).size() == 1,
          "checked Rotate: a Rotate key is created");
    check(f.line().timeKey(core::TimeKeyType_Rotate, 10) != nullptr,
          "checked Rotate: at the playhead");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testStaticEditsDefault();
    testExistingKeyEditsInPlace();
    testAnimatedChannelAutoKeys();
    testCheckboxForcesAKey();
    testOtherChannelCheckboxCreatesOnlyThatKey();
    if (failures) {
        std::printf("FAILED %d check(s)\n", failures);
        return 1;
    }
    std::printf("OK: transform keys follow static/animated/forced policy\n");
    return 0;
}
