// Automated test for the property panel's Photoshop "Blend If" surface
// (gui/prop/prop_ConstantPanel), which is the widget the app builds for a selected node.
//
// The blend ranges are the one part of this feature that is not visible in a render when it is
// wrong: the panel can show stale values, or write an edit back as several undo steps, and the
// picture still looks right. This drives the real panel - not a replica - with a real project
// and checks:
//   1. the eight split-slider items (four per side) show the node's ranges, and a folder's too,
//   2. editing a spin box writes back ONE undoable command named "update blend if ranges",
//      and undo/redo move the model back and forth,
//   3. the reset button returns the side to Photoshop's unrestricted default, undoably,
//   4. with no target the items are disabled rather than editable.
//
// Output is one line per check; exit code 0 = all passed. Requires a GL context (building a
// LayerNode compiles its transform shader), so it bootstraps an offscreen one and exits 2 with
// a clear message if that is unavailable, which lets the runner retry on a real display.
#include <cstdio>
#include <functional>
#include <memory>

#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QJsonObject>
#include <QSpinBox>

#include "core/Animator.h"
#include "core/BlendIf.h"
#include "core/Frame.h"
#include "core/FolderNode.h"
#include "core/LayerNode.h"
#include "core/ObjectTree.h"
#include "core/Project.h"
#include "core/TimeLine.h"
#include "gl/DeviceInfo.h"
#include "gl/Global.h"
#include "AudioPlaybackWidget.h"
#include "gui/GUIResources.h"
#include "gui/ViaPoint.h"
#include "gui/prop/prop_ConstantPanel.h"
#include "gui/prop/prop_Items.h"

// XC's global handlers live in the app's Main.cpp (the harness defines its own pair the
// same way); a test binary has to define them or the XC macros have nothing to call.
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;

// The two gui pieces this test stubs instead of compiling (see the target in
// tests/CMakeLists.txt): the panel never calls either, and compiling them would drag in the
// resource dialog, the resource tree and MainWindow.
namespace gui {
ViaPoint::ViaPoint(QWidget* aParent):
    mParent(aParent), mProject(nullptr), mMainMenuBar(nullptr), mResDialog(nullptr), mLogView(nullptr),
    mGLDeviceInfo(), mKeyCommandMap(nullptr), mKeyCommandInvoker(nullptr), mMainViewSetting(nullptr),
    mMouseSetting(nullptr) {}
void ViaPoint::setProject(core::Project* aProject) { mProject = aProject; }
img::ResourceNode* ViaPoint::requireOneResource() { return nullptr; }
void ViaPoint::pushLog(const QString&, ctrl::UILogType) {}
void ViaPoint::pushUndoneLog(const QString&) {}
void ViaPoint::pushRedoneLog(const QString&) {}
} // namespace gui

bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

int gChecks = 0;
int gFails = 0;

void expect(bool aOk, const QString& aMessage) {
    ++gChecks;
    if (aOk) {
        std::printf("  [ ok ] %s\n", aMessage.toUtf8().constData());
    } else {
        ++gFails;
        std::printf("  [FAIL] %s\n", aMessage.toUtf8().constData());
    }
}

// core::Project needs an Animator; the panel never plays anything, so a stub frame is enough
class StubAnimator: public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(0); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

// An offscreen GL 4.0 core context, because constructing a LayerNode compiles its mesh
// transform shader (core/MeshTransformerResource::setup) through gl::Global.
class GLBootstrap {
public:
    bool init() {
        QSurfaceFormat format;
        format.setVersion(4, 0);
        format.setProfile(QSurfaceFormat::CoreProfile);
        mSurface.setFormat(format);
        mSurface.create();
        if (!mSurface.isValid()) {
            std::fprintf(stderr, "no offscreen surface\n");
            return false;
        }
        mContext.setFormat(format);
        if (!mContext.create()) {
            std::fprintf(stderr, "no GL context\n");
            return false;
        }
        if (!mContext.makeCurrent(&mSurface)) {
            std::fprintf(stderr, "cannot make the GL context current\n");
            return false;
        }
        auto* functions = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(&mContext);
        if (!functions || !functions->initializeOpenGLFunctions()) {
            std::fprintf(stderr, "no OpenGL 4.0 core functions\n");
            return false;
        }
        gl::Global::setFunctions(*functions);
        // DeviceInfo::load() would call Global::makeCurrent(), which needs the global
        // QOpenGLWidget the app's canvas registers; this context is already current, so read the
        // same values directly (DeviceInfo's fields are public).
        auto& ggl = gl::Global::functions();
        const auto stringOf = [&](GLenum aName) {
            const char* text = (const char*)ggl.glGetString(aName);
            return std::string(text ? text : "");
        };
        mDeviceInfo.vender = stringOf(GL_VENDOR);
        mDeviceInfo.renderer = stringOf(GL_RENDERER);
        mDeviceInfo.version = stringOf(GL_VERSION);
        ggl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mDeviceInfo.maxTextureSize);
        ggl.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &mDeviceInfo.maxRenderBufferSize);
        gl::DeviceInfo::setInstance(&mDeviceInfo);
        std::printf("OpenGL: %s\n", mDeviceInfo.version.c_str());
        return true;
    }

private:
    QOffscreenSurface mSurface;
    QOpenGLContext mContext;
    gl::DeviceInfo mDeviceInfo;
};

// what the panel shows for one side, in black-low/black-high/white-low/white-high order
QVector<int> shownRange(gui::prop::ConstantPanel& aPanel, bool aDst) {
    QVector<int> values;
    for (int i = 0; i < 4; ++i) {
        values.push_back(aPanel.blendIfItem(aDst, i)->value());
    }
    return values;
}

QVector<int> rangeOf(const core::BlendIf::Range& aRange) {
    return QVector<int>{aRange.blackLo, aRange.blackHi, aRange.whiteLo, aRange.whiteHi};
}

// drives an edit the way a user does: type into the spin box, then leave the field. The panel
// only writes back on editingFinished, so this is the real path.
void editSpinBox(gui::prop::IntegerItem& aItem, int aValue) {
    aItem.box().setValue(aValue);
    QMetaObject::invokeMethod(&aItem.box(), "editingFinished", Qt::DirectConnection);
}

} // namespace

// Test-local XC handlers: without them a failed assert or a fatal report aborts silently, and
// the point of this binary is to say what went wrong.
class TestAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::fprintf(stderr, "panel_blendif: XC assert failed\n"); }
};

class TestErrorHandler: public XCErrorHandler {
public:
    void critical(const QString& aText, const QString& aInfo, const QString& aDetail) const override {
        std::fprintf(stderr, "panel_blendif: fatal: %s | %s | %s\n", aText.toUtf8().constData(),
            aInfo.toUtf8().constData(), aDetail.toUtf8().constData());
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    static TestAssertHandler assertHandler;
    static TestErrorHandler errorHandler;
    gXCAssertHandler = &assertHandler;
    gXCErrorHandler = &errorHandler;

    GLBootstrap gl;
    if (!gl.init()) {
        std::fprintf(stderr, "panel_blendif: no usable OpenGL context; retry with a display\n");
        return 2;
    }

    StubAnimator animator;
    core::Project project(QString(), animator, nullptr);

    core::ObjectTree& tree = project.objectTree();
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);

    auto* layer = new core::LayerNode("layer", tree.shaderHolder());
    root->children().pushBack(layer);
    auto* folder = new core::FolderNode("folder");
    root->children().pushBack(folder);

    core::BlendIf layerRanges;
    layerRanges.dst = core::BlendIf::Range{10, 20, 30, 40};
    layerRanges.src = core::BlendIf::Range{200, 210, 220, 230};
    layer->setBlendIf(layerRanges);
    core::BlendIf folderRanges;
    folderRanges.dst = core::BlendIf::Range{5, 6, 7, 8};
    folder->setBlendIf(folderRanges);

    gui::ViaPoint viaPoint(nullptr);
    viaPoint.setProject(&project);
    QScopedPointer<gui::GUIResources> resources(new gui::GUIResources(QStringLiteral(":")));
    gui::prop::ConstantPanel panel(viaPoint, project, nullptr, resources.data());

    // (1) the panel shows the selected node's ranges, both sides
    panel.setTarget(layer);
    expect(shownRange(panel, true) == rangeOf(layerRanges.dst), "the panel shows the destination range");
    expect(shownRange(panel, false) == rangeOf(layerRanges.src), "the panel shows the source range");
    expect(
        panel.blendIfItem(true, 0)->box().minimum() == 0 && panel.blendIfItem(true, 0)->box().maximum() == 255,
        "the split sliders span 0..255");

    // it follows a re-selection (a folder carries ranges too, and the same widgets must show them)
    panel.setTarget(folder);
    expect(shownRange(panel, true) == rangeOf(folderRanges.dst), "the panel follows the selection (folder)");
    expect(shownRange(panel, false) == rangeOf(core::BlendIf::Range()), "an unrestricted side reads as the default");
    panel.setTarget(layer);

    // (2) an edit is ONE undoable command, and undo/redo move the model
    {
        const int before = (int)layer->renderer()->blendIf().dst.whiteHi;
        editSpinBox(*panel.blendIfItem(true, 3), 150);
        expect(
            layer->renderer()->blendIf().dst.whiteHi == 150,
            QString("editing the white-high slider writes the model (was %1)").arg(before));
        expect(
            shownRange(panel, true) == QVector<int>({10, 20, 30, 150}),
            "the other three sliders keep their values");

        bool undone = false;
        const QString name = project.commandStack().undo(&undone);
        expect(
            undone && layer->renderer()->blendIf().dst.whiteHi == before,
            QString("one undo restores the range (command '%1')").arg(name));
        expect(name == QStringLiteral("update blend if ranges"), "the command carries its own name");

        bool redone = false;
        project.commandStack().redo(&redone);
        expect(
            redone && layer->renderer()->blendIf().dst.whiteHi == 150,
            "redo re-applies the edit");
    }

    // (3) the reset button returns that side to Photoshop's default, undoably
    {
        panel.blendIfReset(true)->onPressed();
        expect(
            layer->renderer()->blendIf().dst == core::BlendIf::Range(),
            "reset restores the unrestricted destination range");
        expect(
            layer->renderer()->blendIf().src == layerRanges.src,
            "reset touches only the side it belongs to");
        panel.updateAttribute();
        expect(shownRange(panel, true) == QVector<int>({0, 0, 255, 255}), "the panel shows the reset values");

        // the reset is one command on top of the earlier edit, so the first undo restores the
        // EDITED range (white-high 150) and the second one the imported range
        project.commandStack().undo();
        const core::BlendIf::Range edited{10, 20, 30, 150};
        expect(
            layer->renderer()->blendIf().dst == edited,
            "undo brings the pre-reset range back");
        project.commandStack().undo();
        expect(
            layer->renderer()->blendIf().dst == layerRanges.dst,
            "a second undo returns to the imported range");
        project.commandStack().redo();
        project.commandStack().redo();
    }

    // (4) with nothing selected the items are disabled, not editable
    {
        panel.setTarget(nullptr);
        expect(!panel.blendIfItem(true, 0)->box().isEnabled(), "the sliders are disabled without a target");
        expect(!panel.blendIfReset(true)->button().isEnabled(), "the reset button is disabled without a target");
        panel.setTarget(layer);
        expect(panel.blendIfItem(true, 0)->box().isEnabled(), "the sliders are enabled again for a target");
    }

    std::printf("%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
