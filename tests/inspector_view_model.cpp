// The QML inspector's model contract: values, key states, model-2 writes, the
// filter stack, and undo. The scene itself is verified live; this gate keeps the
// document decisions from depending on whether a delegate happened to load.
//
// Run: QT_QPA_PLATFORM=offscreen ./inspector_view_model
#include <QApplication>
#include <cstdio>

#include "core/Animator.h"
#include "core/BlurKey.h"
#include "core/FolderNode.h"
#include "core/HsvKey.h"
#include "core/MoveKey.h"
#include "core/OpaKey.h"
#include "core/Project.h"
#include "core/RotateKey.h"
#include "gui/qml/FilterStackModel.h"
#include "gui/qml/InspectorRowModel.h"
#include "gui/qml/InspectorViewModel.h"
#include "gui/ViaPoint.h"
#include "XCAssert.h"
#include "XCReport.h"

#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;
XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

namespace gui {
// The model only calls this from the asset chooser; a headless gate never opens
// the dialog. Defining the one symbol keeps the test free of the GL/dialog stack.
img::ResourceNode* ViaPoint::requireOneResource() { return nullptr; }
} // namespace gui

namespace {

class TestAnimator: public core::Animator {
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



#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                                                   \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)

const core::MoveKey::Data& moveDefault(const core::TimeLine& line) {
    return static_cast<const core::MoveKey*>(line.defaultKey(core::TimeKeyType_Move))->data();
}

int run() {
    TestAnimator animator;
    animator.setFrame(10);
    core::Project project("inspector_view_model.anie", animator, nullptr);
    core::FolderNode folder(QStringLiteral("folder"));
    folder.setDefaultPosture(QVector2D(100.0f, 200.0f));
    folder.setDefaultDepth(-3.0f);
    folder.setDefaultOpacity(0.75f);
    core::TimeLine& line = *folder.timeLine();

    gui::InspectorViewModel inspector(nullptr);
    inspector.setProject(&project);
    inspector.setTarget(&folder);

    // A fresh layer: model 2 reads the defaults as its values.
    CHECK(inspector.move()->x() == 100.0);
    CHECK(inspector.move()->y() == 200.0);
    CHECK(inspector.depth()->depth() == -3.0);
    CHECK(inspector.opacity()->opacity() == 0.75);
    CHECK(inspector.move()->keyState() == QStringLiteral("static"));
    CHECK(inspector.move()->keyEnabled());

    // A static edit writes the default and creates no key.
    inspector.move()->setX(11.0);
    CHECK(line.map(core::TimeKeyType_Move).isEmpty());
    CHECK(moveDefault(line).pos() == QVector2D(11.0f, 200.0f));

    // A keyed property edits the key at the playhead, and undo restores it.
    inspector.move()->toggleKey();
    CHECK(inspector.move()->keyState() == QStringLiteral("keyed"));
    CHECK(line.hasTimeKey(core::TimeKeyType_Move, 10));
    inspector.move()->setX(11.0);
    inspector.move()->setY(33.0);
    const auto* key = static_cast<const core::MoveKey*>(line.timeKey(core::TimeKeyType_Move, 10));
    CHECK(key && key->data().pos() == QVector2D(11.0f, 33.0f));
    project.commandStack().undo();
    inspector.refresh();
    const auto* undone = static_cast<const core::MoveKey*>(line.timeKey(core::TimeKeyType_Move, 10));
    CHECK(undone && undone->data().pos() == QVector2D(11.0f, 0.0f));

    // The dot is a toggle; removing the key keeps the static base.
    inspector.move()->toggleKey();
    CHECK(inspector.move()->keyState() == QStringLiteral("static"));
    CHECK(!line.hasTimeKey(core::TimeKeyType_Move, 10));
    CHECK(moveDefault(line).pos() == QVector2D(11.0f, 200.0f));

    // Rotation is degrees in the model and radians in the document.
    inspector.rotate()->setAngle(45.0);
    CHECK(inspector.rotate()->angle() == 45.0);
    const auto* rotate = static_cast<const core::RotateKey*>(line.defaultKey(core::TimeKeyType_Rotate));
    CHECK(rotate && std::abs(rotate->data().rotate() - 0.7853982f) < 0.0001f);

    // Filters: add is static-first, the dot creates the explicit key, and the
    // row's fields edit the document.
    CHECK(inspector.filters()->rows().isEmpty());
    inspector.filters()->addFilter(core::TimeKeyType_HSV);
    CHECK(inspector.filters()->rows().size() == 1);
    CHECK(inspector.filters()->hasFilter(core::TimeKeyType_HSV));
    auto* hsv = inspector.filters()->rowAt(0);
    CHECK(hsv && hsv->kind() == QStringLiteral("hsv"));
    CHECK(hsv->keyState() == QStringLiteral("static"));
    hsv->setHue(42);
    CHECK(line.map(core::TimeKeyType_HSV).isEmpty());
    CHECK(static_cast<const core::HSVKey*>(line.defaultKey(core::TimeKeyType_HSV))->data().hsv().at(0) == 42);
    hsv->toggleKey();
    CHECK(hsv->keyState() == QStringLiteral("keyed"));

    // The first add expands; collapsing it is a user state that survives a
    // frame refresh (a playback tick must not reopen a card).
    CHECK(hsv->expanded());
    inspector.filters()->toggleExpanded(core::TimeKeyType_HSV);
    CHECK(!hsv->expanded());
    inspector.refreshFrame();
    CHECK(!hsv->expanded());

    inspector.filters()->addFilter(core::TimeKeyType_Blur);
    CHECK(inspector.filters()->rows().size() == 2);
    CHECK(inspector.filters()->rowAt(1)->kind() == QStringLiteral("blur"));

    // Reorder and the eye are stack state, both undoable.
    inspector.filters()->moveFilter(core::TimeKeyType_Blur, 0);
    CHECK(inspector.filters()->rowAt(0)->kind() == QStringLiteral("blur"));
    project.commandStack().undo();
    inspector.refresh();
    CHECK(inspector.filters()->rowAt(0)->kind() == QStringLiteral("hsv"));
    inspector.filters()->setFilterEnabled(core::TimeKeyType_Blur, false);
    CHECK(!line.filterEnabled(core::TimeKeyType_Blur));
    project.commandStack().undo();
    inspector.refresh();
    CHECK(line.filterEnabled(core::TimeKeyType_Blur));

    // Removing a filter also removes its lane's explicit keys, in one step.
    inspector.filters()->removeFilter(core::TimeKeyType_HSV);
    CHECK(!inspector.filters()->hasFilter(core::TimeKeyType_HSV));
    CHECK(!line.hasTimeKey(core::TimeKeyType_HSV, 10));
    project.commandStack().undo();
    inspector.refresh();
    CHECK(inspector.filters()->hasFilter(core::TimeKeyType_HSV));
    CHECK(line.hasTimeKey(core::TimeKeyType_HSV, 10));

    // Constants are renderer state; disabled on a node without a renderer is
    // covered by the widget path, and here the folder reports both.
    CHECK(inspector.blendEnabled());
    CHECK(inspector.clippedEnabled());
    inspector.setClipped(true);
    CHECK(folder.renderer()->isClipped());
    project.commandStack().undo();
    inspector.refresh();
    CHECK(!folder.renderer()->isClipped());

    std::printf("OK: inspector view model passes\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return run();
}
