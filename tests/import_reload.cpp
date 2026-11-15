// Automated test for the SOURCE RELOAD path: import a PSD, then reload it from the file after
// the file changed, and check what the project ends up with.
//
// Why this test exists: the reload is the one path in this repository that reaches the source
// document a second time, and until now nothing exercised it. It is GUI code
// (gui/res/res_ResourceUpdater), so it is driven here for real - the production ResourceUpdater,
// a real QTreeWidget resource tree, a real project - with the GUI pieces it does not use stubbed
// (see the target in tests/CMakeLists.txt).
//
// The expectations are INDEPENDENT of the app's own math: every fixture is written by this test
// (img::PSDWriter), so what each layer contains, what its id is and what changed between the two
// files are all known here, not computed by the code under test.
//
// Output is one line per check; exit code 0 = all passed. Requires a GL context (import builds
// LayerNodes, which compile shaders), so it bootstraps an offscreen one and exits 2 if that is
// unavailable, which lets the runner retry on a real display.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QMainWindow>
#include <QTreeWidget>

#include "XCAssert.h"
#include "core/Animator.h"
#include "core/BlendIf.h"
#include "core/Frame.h"
#include "core/ImageKey.h"
#include "core/LayerEffects.h"
#include "core/OpaKey.h"
#include "core/TimeKey.h"
#include "core/FolderNode.h"
#include "core/LayerNode.h"
#include "core/ObjectTree.h"
#include "core/Project.h"
#include "core/TimeLine.h"
#include "ctrl/ImageFileLoader.h"
#include "gl/DeviceInfo.h"
#include "gl/Global.h"
#include "gl/VertexArrayObject.h"
#include "gui/ResourceDialog.h"
#include "gui/ViaPoint.h"
#include "gui/res/res_Item.h"
#include "gui/res/res_ResourceUpdater.h"
#include "img/PSDFormat.h"
#include "img/PSDWriter.h"
#include "img/ResourceNode.h"
#include "util/IProgressReporter.h"
#include "AudioPlaybackWidget.h"

// the PSD descriptor / layer-effects fixture writers, shared with tools/render_verify (see the
// header: one writer per format block, so two harnesses cannot disagree about the format)
#include "PsdFixture.h"
using namespace psdfx;

// XC's globals live in the app's Main.cpp; a test binary defines its own pair.
class TestAssertHandler: public XCAssertHandler {
public:
    void failure() const override {
        std::fprintf(stderr, "XC_ASSERT failure\n");
        ++gAssertFailures;
    }
    static int gAssertFailures;
};
int TestAssertHandler::gAssertFailures = 0;
class TestErrorHandler: public XCErrorHandler {
public:
    void critical(const QString& aText, const QString& aInfo, const QString& aDetail) const override {
        std::fprintf(stderr, "XC_FATAL_ERROR: %s | %s | %s\n", qPrintable(aText), qPrintable(aInfo), qPrintable(aDetail));
        std::abort();
    }
};
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;

// The GUI pieces this test does not compile (see tests/CMakeLists.txt): ViaPoint is the GUI's
// service locator and the reload path only calls the few members below out of it.
namespace gui {
ViaPoint::ViaPoint(QWidget* aParent):
    mParent(aParent), mProject(nullptr), mMainMenuBar(nullptr), mResDialog(nullptr), mLogView(nullptr),
    mGLDeviceInfo(), mKeyCommandMap(nullptr), mKeyCommandInvoker(nullptr), mMainViewSetting(nullptr),
    mMouseSetting(nullptr) {}
void ViaPoint::setProject(core::Project* aProject) { mProject = aProject; }
img::ResourceNode* ViaPoint::requireOneResource() { return nullptr; }
QStringList gLog;
void ViaPoint::pushLog(const QString& aText, ctrl::UILogType) { gLog << aText; }
extern QStringList gLog;
} // namespace gui

// gui/res/res_Notifier.cpp references this through the resource dialog, which the reload path
// only touches when ViaPoint::resourceDialog() is non-null (it is null in this test), so the
// symbol only has to exist. Loud if that ever changes.
namespace gui {
void ResourceDialog::updateResourcePath() {
    std::fprintf(stderr, "unexpected: ResourceDialog::updateResourcePath in the reload test\n");
    std::abort();
}
} // namespace gui

bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

// ---------------------------------------------------------------------------------------------
// A byte counter for ARRAY allocations, which is what every pixel and mask block in this codebase
// is (img::Buffer::alloc, img::Util::createTextureImage, PSDUtil: `new uint8[]`).
//
// Why the test counts bytes: a reload reads the whole file into a ctrl::SourceDocument and applies
// only what differs, so the memory of everything it does NOT apply is the document's to free. It
// used to be nothing's: the entries held raw XCMemBlock fields, the document was a stack local in
// ResourceUpdater::reconcile, and only the entries that were applied had their blocks taken - so
// every reload leaked the pixels AND the mask of every unchanged layer, for the life of the
// process (a 2000x2000 layer: 20 MB per reload). Counting bytes across a reload that changes
// nothing measures exactly that: it must come back to where it started.
//
// The replacement is global, so it must sit at file scope (the standard forbids it in a namespace).
std::atomic<long long> gLiveArrayBytes{0};
constexpr size_t kArrayHeader = 16; // keeps the alignment malloc gave us

void* operator new[](size_t aSize) {
    void* raw = std::malloc(aSize + kArrayHeader);
    if (!raw)
        throw std::bad_alloc();
    *static_cast<size_t*>(raw) = aSize;
    gLiveArrayBytes += (long long)aSize;
    return static_cast<char*>(raw) + kArrayHeader;
}

void operator delete[](void* aPtr) noexcept {
    if (!aPtr)
        return;
    char* raw = static_cast<char*>(aPtr) - kArrayHeader;
    gLiveArrayBytes -= (long long)*reinterpret_cast<size_t*>(raw);
    std::free(raw);
}

void operator delete[](void* aPtr, size_t) noexcept { operator delete[](aPtr); }

namespace {

int gChecks = 0;
int gFails = 0;

void expect(bool aOk, const QString& aWhat) {
    ++gChecks;
    if (!aOk)
        ++gFails;
    std::printf("  [%s] %s\n", aOk ? "PASS" : "FAIL", aWhat.toUtf8().constData());
}

// The app's canvas widget: LayerNode construction reaches core::LayerMesh's GLBinder, which
// calls gl::Global::makeCurrent(), and that needs the widget the app registers (the same reason
// tools/render_verify and the roundtrip test register one).
class TestGLWidget: public QOpenGLWidget {
public:
    gl::DeviceInfo mDeviceInfo;
    bool mGLReady = false;

protected:
    void initializeGL() override {
        auto* functions = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(context());
        if (!functions || !functions->initializeOpenGLFunctions()) {
            std::fprintf(stderr, "no OpenGL 4.0 core functions\n");
            std::exit(2);
        }
        if (!mGLReady) {
            gl::Global::setContext(*this);
            gl::Global::setFunctions(*functions);
            mDeviceInfo.load();
            gl::DeviceInfo::setInstance(&mDeviceInfo);
        } else {
            gl::Global::clearFunctions();
            gl::Global::setFunctions(*functions);
        }
        mGLReady = true;
    }
};

class NullAnimator: public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

class NullReporter: public util::IProgressReporter {
public:
    void setSection(const QString&) override {}
    void setMaximum(int) override {}
    void setProgress(int) override {}
    bool wasCanceled() const override { return false; }
};

// ---------------------------------------------------------------------------------------------
// The fixture: a PSD written by this test, so every expectation below is defined here.
//
// A group ("grp") holding two layers ("A", "B"), plus a third layer ("C") beside it. Each layer
// is canvas-sized and flat, so "what is this layer's pixel" needs no arithmetic.
struct LayerSpec {
    std::string name;
    uint32 stableId = 0;
    bool isFolder = false;
    const char* blend = "norm";
    uint8 rgb[3] = {255, 255, 255};
    uint8 alpha = 255;
    uint8 opacity = 255;
    bool visible = true;
    bool hasMask = false;
    uint8 maskValue = 255;   // flat mask
    // the layer's effects block ("lfx2"), bytes as Photoshop writes them; empty = no effects.
    // The writer is tests/support/PsdFixture.h, shared with tools/render_verify.
    std::vector<uint8> lfx2;
};

const int kSide = 32;

std::vector<LayerSpec> baselineSpecs() {
    LayerSpec a;
    a.name = "A";
    a.stableId = 101;
    a.rgb[0] = 200; a.rgb[1] = 40; a.rgb[2] = 40;

    LayerSpec b;
    b.name = "B";
    b.stableId = 102;
    b.rgb[0] = 40; b.rgb[1] = 200; b.rgb[2] = 40;

    LayerSpec grp;
    grp.name = "grp";
    grp.stableId = 103;
    grp.isFolder = true;
    grp.blend = "pass";

    LayerSpec c;
    c.name = "C";
    c.stableId = 104;
    c.rgb[0] = 40; c.rgb[1] = 40; c.rgb[2] = 200;

    // the writer emits the list bottom-first, like Photoshop
    return {c, grp, b, a};
}

// writes the given records as one PSD. `aSpecs` is bottom-first.
bool writePsd(const QString& aPath, const std::vector<LayerSpec>& aSpecs) {
    img::PSDFormat format;
    format.header().version = 1;
    format.header().channels = 4;
    format.header().width = kSide;
    format.header().height = kSide;
    format.header().depth = 8;
    format.header().mode = img::PSDFormat::ColorMode_RGB;
    format.imageData().compressionId = 0;

    for (const LayerSpec& spec : aSpecs) {
        auto* layer = new img::PSDFormat::Layer();
        layer->name = spec.name;
        layer->rect.edge[0] = 0;
        layer->rect.edge[1] = 0;
        layer->rect.edge[2] = kSide;
        layer->rect.edge[3] = kSide;
        layer->blendMode = spec.blend;
        layer->opacity = spec.opacity;
        layer->flags = (uint8)((spec.visible ? 0x08 : 0x0a) | (spec.isFolder ? 0x10 : 0x00));
        layer->entryType = spec.isFolder ? img::PSDFormat::LayerEntryType_OpenFolder
                                         : img::PSDFormat::LayerEntryType_Layer;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            // a group record carries the four channel slots with no data
            chan->dataLength = spec.isFolder ? 0 : kSide * kSide;
            if (!spec.isFolder) {
                chan->data.reset(new uint8[kSide * kSide]());
                if (id == -1) {
                    std::memset(chan->data.get(), spec.alpha, kSide * kSide);
                } else {
                    std::memset(chan->data.get(), spec.rgb[id], kSide * kSide);
                }
            }
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        if (spec.hasMask) {
            // the user mask: a record block plus the -2 channel it points at
            layer->mask.reset(new img::PSDFormat::LayerMask());
            layer->mask->rect.edge[0] = 0;
            layer->mask->rect.edge[1] = 0;
            layer->mask->rect.edge[2] = kSide;
            layer->mask->rect.edge[3] = kSide;
            layer->mask->defaultColor = 0;
            layer->mask->flags = 0;
            auto* chan = new img::PSDFormat::Channel();
            chan->id = -2;
            chan->compressionId = 0;
            chan->dataLength = kSide * kSide;
            chan->data.reset(new uint8[kSide * kSide]());
            std::memset(chan->data.get(), spec.maskValue, kSide * kSide);
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        {
            // "lyid": the layer's stable id, a big-endian uint32
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "lyid";
            info->dataLength = 4;
            info->data.reset(new uint8[4]());
            info->data[0] = (uint8)(spec.stableId >> 24);
            info->data[1] = (uint8)(spec.stableId >> 16);
            info->data[2] = (uint8)(spec.stableId >> 8);
            info->data[3] = (uint8)spec.stableId;
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        {
            // "luni": the display name
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "luni";
            info->dataLength = 4 + (uint32)spec.name.size() * 2;
            info->data.reset(new uint8[info->dataLength]());
            info->data[0] = (uint8)(spec.name.size() >> 24);
            info->data[1] = (uint8)(spec.name.size() >> 16);
            info->data[2] = (uint8)(spec.name.size() >> 8);
            info->data[3] = (uint8)spec.name.size();
            for (size_t i = 0; i < spec.name.size(); ++i)
                info->data[4 + i * 2 + 1] = (uint8)spec.name[i];
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        if (!spec.lfx2.empty()) {
            // "lfx2": the layer effects block. Its data ARE the descriptor block (version pair +
            // descriptor), so the fixture's bytes go in as they are.
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "lfx2";
            info->dataLength = (uint32)spec.lfx2.size();
            info->data.reset(new uint8[spec.lfx2.size()]);
            std::memcpy(info->data.get(), spec.lfx2.data(), spec.lfx2.size());
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));

        if (spec.isFolder) {
            // the bounding record that closes the group
            auto* bound = new img::PSDFormat::Layer();
            bound->name = "</Layer group>";
            bound->rect.edge[0] = 0;
            bound->rect.edge[1] = 0;
            bound->rect.edge[2] = kSide;
            bound->rect.edge[3] = kSide;
            bound->blendMode = "norm";
            bound->opacity = 255;
            bound->flags = 0x08;
            bound->entryType = img::PSDFormat::LayerEntryType_Bounding;
            for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
                auto* chan = new img::PSDFormat::Channel();
                chan->id = id;
                chan->compressionId = 0;
                chan->dataLength = 0;
                bound->channels.push_back(img::PSDFormat::ChannelPtr(chan));
            }
            // the section divider block: what makes the reader see this record as a group. An
            // open folder carries type 1 and Photoshop's "8BIMpass" signature (a pass-through
            // group), the closing record type 3.
            {
                auto* info = new img::PSDFormat::AdditionalLayerInfo();
                info->key = "lsct";
                info->dataLength = 12;
                info->data.reset(new uint8[12]());
                info->data[3] = 1; // LayerEntryType_OpenFolder
                std::memcpy(info->data.get() + 4, "8BIMpass", 8);
                layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
            }
            {
                auto* info = new img::PSDFormat::AdditionalLayerInfo();
                info->key = "lsct";
                info->dataLength = 4;
                info->data.reset(new uint8[4]());
                info->data[3] = 3; // LayerEntryType_Bounding
                bound->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
            }
            format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(bound));
        }
    }

    // the merged image data section (section 5): the writer always emits it, and the reader
    // expects it, so a fixture without it is a truncated file
    for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
        auto* chan = new img::PSDFormat::Channel();
        chan->id = id;
        chan->compressionId = 0;
        chan->dataLength = kSide * kSide;
        chan->data.reset(new uint8[kSide * kSide]());
        format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
    }

    std::ofstream out(aPath.toLocal8Bit(), std::ios::binary);
    if (out.fail())
        return false;
    img::PSDWriter writer(out, format);
    return writer.resultCode() == img::PSDWriter::ResultCode_Success;
}

// ---------------------------------------------------------------------------------------------
// helpers over the imported project

core::LayerNode* findLayer(core::ObjectNode& aRoot, const QString& aName) {
    core::ObjectNode::Iterator itr(&aRoot);
    while (itr.hasNext()) {
        auto node = itr.next();
        if (node->name() == aName)
            return node->type() == core::ObjectType_Layer ? (core::LayerNode*)node : nullptr;
    }
    return nullptr;
}

// the resources the project holds, by name, in the tree the reload reconciles
QStringList resourceNames(const img::ResourceNode& aTree) {
    QStringList names;
    img::ResourceNode::ConstIterator itr(&aTree);
    while (itr.hasNext()) {
        auto node = itr.next();
        if (node->parent()) {
            names << node->data().identifier();
        }
    }
    names.sort();
    return names;
}

img::ResourceNode* findResource(img::ResourceNode& aTree, const QString& aName) {
    img::ResourceNode::Iterator itr(&aTree);
    while (itr.hasNext()) {
        auto node = itr.next();
        if (node->data().identifier() == aName) {
            return node;
        }
    }
    return nullptr;
}

// the layer's mask as the resource holds it (A8), empty when it has none
bool maskPixel(const img::ResourceNode& aNode, uint8& aOut) {
    if (!aNode.data().hasMask() || !aNode.data().mask().data()) {
        return false;
    }
    // the plane lives in the layer's padded rect, so the first INTERIOR pixel is the content
    const int w = aNode.data().mask().pixelSize().width();
    aOut = aNode.data().mask().data()[(size_t)1 * w + 1];
    return true;
}

// the layer's first pixel as it will be drawn: the resource its image key points at
bool firstPixel(const core::LayerNode& aLayer, uint8 aOut[4]) {
    auto* key = dynamic_cast<const core::ImageKey*>(
        aLayer.timeLine()->defaultKey(core::TimeKeyType_Image));
    if (!key || !key->hasImage())
        return false;
    const img::ResourceHandle& handle = key->data().resource();
    if (!handle || !handle->image().data())
        return false;
    const uint8* data = handle->image().data();
    // the importer pads by one transparent pixel, so the content starts at (1,1)
    const int w = handle->image().pixelSize().width();
    const size_t index = ((size_t)1 * w + 1) * 4;
    for (int i = 0; i < 4; ++i)
        aOut[i] = data[index + i];
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the checks
    gXCAssertHandler = new TestAssertHandler();
    gXCErrorHandler = new TestErrorHandler();

    {
        QSurfaceFormat format;
        format.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc, argv);

    // the layer nodes compile shaders and build GL mesh buffers, so the import needs the
    // registered, current GL context of a real widget (the same bootstrap as the probe and
    // tools/render_verify)
    QMainWindow window;
    auto* glWidget = new TestGLWidget();
    window.setCentralWidget(glWidget);
    window.resize(32, 32);
    window.show();
    app.processEvents();
    if (!glWidget->mGLReady) {
        glWidget->update();
        app.processEvents();
    }
    if (!glWidget->mGLReady) {
        std::fprintf(stderr, "the GL widget did not initialize\n");
        return 2;
    }
    glWidget->makeCurrent();
    {
        gl::VertexArrayObject vao; // drains any pending GL error the widget left behind
        vao.bind();
    }
    while (gl::Global::functions().glGetError() != GL_NO_ERROR) {
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "no temporary directory\n");
        return 2;
    }
    const QString psdPath = dir.filePath("source.psd");

    // ---------------------------------------------------------------- import
    std::printf("== import: a written PSD through the production loader ==\n");
    NullAnimator animator;
    core::Project project("", animator, nullptr);
    gui::ViaPoint viaPoint(nullptr);
    viaPoint.setProject(&project);

    ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
    loader.setOraImportMode(ctrl::ImageFileLoader::OraImportMode::Layered);
    NullReporter reporter;

    const std::vector<LayerSpec> baseline = baselineSpecs();
    expect(writePsd(psdPath, baseline), "the fixture PSD was written");
    expect(loader.load(psdPath, project, reporter), QString("the fixture PSD imports (%1)").arg(loader.log()));

    auto* topNode = project.objectTree().topNode();
    expect(topNode != nullptr, "the import created an object tree");

    // the GUI resource tree mirrors the holder's, exactly as the resource dialog builds it
    QTreeWidget tree;
    expect(project.resourceHolder().imageTrees().size() == 1, "one image tree was created");
    if (project.resourceHolder().imageTrees().empty()) {
        std::printf("\n%d checks, %d failures (no image tree: stopping)\n", gChecks, gFails);
        return 1;
    }
    auto* guiItem = gui::res::ResourceUpdater::createGUITree(
        tree, *project.resourceHolder().imageTrees().front().topNode, "source.psd");
    tree.addTopLevelItem(guiItem);

    auto* layerA = topNode ? findLayer(*topNode, "A") : nullptr;
    expect(layerA != nullptr, "layer A is in the object tree");
    if (layerA) {
        uint8 px[4] = {0, 0, 0, 0};
        expect(firstPixel(*layerA, px) && px[0] == 200 && px[1] == 40 && px[2] == 40 && px[3] == 255,
               QString("layer A has the colour the fixture gave it (got %1,%2,%3,%4)")
                   .arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]));
    }

    // ---------------------------------------------------------------- reload, nothing changed
    std::printf("\n== reload: the file is unchanged ==\n");
    gui::res::ResourceUpdater updater(viaPoint, project);
    const bool editedBefore = project.commandStack().isEdited();
    updater.reload(*guiItem);
    if (layerA) {
        uint8 px[4] = {0, 0, 0, 0};
        expect(firstPixel(*layerA, px) && px[0] == 200,
               "the unchanged reload left the layer alone");
    }
    expect(project.commandStack().isEdited() == editedBefore,
           "an unchanged reload left the command stack's edit state alone");

    // ---------------------------------------------------------------- reload, pixels changed
    std::printf("\n== reload: one layer's pixels changed in the file ==\n");
    std::vector<LayerSpec> changed = baseline;
    for (auto& spec : changed) {
        if (spec.name == "A") {
            spec.rgb[0] = 10; spec.rgb[1] = 20; spec.rgb[2] = 30;
        }
    }
    expect(writePsd(psdPath, changed), "the changed fixture PSD was written");
    updater.reload(*guiItem);
    if (layerA) {
        uint8 px[4] = {0, 0, 0, 0};
        const bool got = firstPixel(*layerA, px);
        expect(got && px[0] == 10 && px[1] == 20 && px[2] == 30,
               QString("the reload applied the changed pixels to layer A (got %1,%2,%3), and the layer object is the same one")
                   .arg(px[0]).arg(px[1]).arg(px[2]));
        expect(findLayer(*topNode, "A") == layerA, "the reload kept the same object node for A");
    }


    // ---------------------------------------------------------------- reload: a rename
    // The stable id decides, so the layer is still the same layer even though its name changed;
    // the project keeps its own name for the resource and NOTHING is appended or removed.
    std::printf("\n== reload: the file renamed a layer ==\n");
    {
        std::vector<LayerSpec> renamedSpecs = baselineSpecs();
        for (auto& spec : renamedSpecs) {
            if (spec.name == "A") {
                spec.name = "A renamed";
            }
        }
        expect(writePsd(psdPath, renamedSpecs), "the renamed fixture PSD was written");
        const QStringList before = resourceNames(*project.resourceHolder().imageTrees().front().topNode);
        gui::gLog.clear();
        updater.reload(*guiItem);
        const QStringList after = resourceNames(*project.resourceHolder().imageTrees().front().topNode);
        expect(before == after, "a rename neither appends nor removes a resource");
        expect(findLayer(*topNode, "A") == layerA, "the object node is still the same one");
        bool reported = false;
        for (const QString& line : gui::gLog) {
            if (line.contains("A renamed")) {
                reported = true;
            }
        }
        expect(reported, "the rename is reported rather than applied silently");
    }

    // ---------------------------------------------------------------- reload: a property change
    // Everything a reload detects on the OBJECT side has to reach the node the renderer reads:
    // the layer's blend mode lives on its image key, its opacity on the default OpaKey, its
    // visibility on the node, and the file's fill opacity on the layer.
    std::printf("\n== reload: opacity, visibility, blend mode and fill opacity changed ==\n");
    {
        std::vector<LayerSpec> propertySpecs = baselineSpecs();
        for (auto& spec : propertySpecs) {
            if (spec.name == "A") {
                spec.name = "A"; // back to the imported name
            }
            if (spec.name == "B") {
                spec.opacity = 128;
                spec.visible = false;
                spec.blend = "mul ";
            }
        }
        expect(writePsd(psdPath, propertySpecs), "the property fixture PSD was written");
        updater.reload(*guiItem);

        auto* layerB = topNode ? findLayer(*topNode, "B") : nullptr;
        expect(layerB != nullptr, "layer B is still in the object tree");
        if (layerB) {
            // the fixture says B is invisible at 50 % opacity in Multiply
            const bool visible = layerB->isVisible();
            const float opacity = [&]() {
                auto* key = layerB->timeLine()->defaultKey(core::TimeKeyType_Opa);
                return key ? static_cast<const core::OpaKey*>(key)->opacity() : -1.0f;
            }();
            const img::BlendMode blend = layerB->renderer()->blendMode();
            expect(!visible, "the file's visibility reached the node");
            expect(std::abs(opacity - 128.0f / 255.0f) < 0.002f,
                   QString("the file's opacity reached the default key (%1)").arg(opacity));
            expect(blend == img::BlendMode_Multiply,
                   QString("the file's blend mode reached the node (%1)").arg((int)blend));
        }
    }

    // ---------------------------------------------------------------- reload: a mask the file gained
    std::printf("\n== reload: the file gave a layer a mask ==\n");
    {
        std::vector<LayerSpec> maskSpecs = baselineSpecs();
        for (auto& spec : maskSpecs) {
            if (spec.name == "B") {
                spec.hasMask = true;
                spec.maskValue = 40;
            }
        }
        expect(writePsd(psdPath, maskSpecs), "the masked fixture PSD was written");
        updater.reload(*guiItem);
        auto* resB = findResource(*project.resourceHolder().imageTrees().front().topNode, "B");
        uint8 value = 0;
        const bool got = resB && maskPixel(*resB, value);
        expect(got && value == 40,
               QString("the mask the file added was applied (got %1)").arg(got ? (int)value : -1));
    }

    // ---------------------------------------------------------------- reload: a layer effect the
    // file added, changed and dropped
    // The blurred-silhouette family (drop shadow, outer glow, chrome) is the family this model
    // exists for, and it is the one the reload's change test could not see: the comparison that
    // decides whether to apply listed the overlays and the stroke field by field and never looked
    // at the silhouette list at all, so a shadow the file ADDED was detected as "nothing changed"
    // and the layer kept drawing without it - silently, on every reload, for the whole feature.
    // The three cases below are the three shapes of that bug (added, changed, dropped); the
    // fixture writes the block with the shared writer (tests/support/PsdFixture.h).
    std::printf("\n== reload: a layer effect the file added, changed and dropped ==\n");
    {
        const auto silhouettesOf = [](core::LayerNode* aLayer) {
            return aLayer ? aLayer->renderer()->layerEffects().silhouettes
                          : std::vector<core::LayerEffects::Silhouette>();
        };
        expect(layerA && silhouettesOf(layerA).empty(), "the imported layer A starts with no effects");

        const auto withShadow = [](double aBlur, double aOpacity) {
            std::vector<LayerSpec> specs = baselineSpecs();
            for (auto& spec : specs) {
                if (spec.name == "A") {
                    spec.lfx2 = makeLfx2DropShadow(aBlur, aOpacity);
                }
            }
            return specs;
        };

        // (1) added
        expect(writePsd(psdPath, withShadow(7.0, 80.0)), "the fixture with a drop shadow on A was written");
        updater.reload(*guiItem);
        {
            const auto shadows = silhouettesOf(layerA);
            const bool ok = shadows.size() == 1 && shadows[0].kind == core::LayerEffects::Silhouette::Kind_DropShadow
                && std::abs(shadows[0].blur - 7.0f) < 0.01f && std::abs(shadows[0].opacity - 0.8f) < 0.01f;
            expect(ok,
                   QString("the drop shadow the file added reached the node (%1 silhouette(s))")
                       .arg((int)shadows.size()));
        }

        // (2) changed - the case a field-by-field comparison misses even when it covers the field
        expect(writePsd(psdPath, withShadow(19.0, 40.0)), "the fixture with a changed drop shadow was written");
        updater.reload(*guiItem);
        {
            const auto shadows = silhouettesOf(layerA);
            const bool ok = shadows.size() == 1 && std::abs(shadows[0].blur - 19.0f) < 0.01f
                && std::abs(shadows[0].opacity - 0.4f) < 0.01f;
            expect(ok,
                   QString("the file's changed drop shadow replaced the old one (blur %1)")
                       .arg(shadows.empty() ? -1.0f : shadows[0].blur));
        }

        // (3) dropped
        expect(writePsd(psdPath, baselineSpecs()), "the fixture without effects was written");
        updater.reload(*guiItem);
        expect(silhouettesOf(layerA).empty(), "the effect the file dropped went away with it");
    }

    // ---------------------------------------------------------------- the reload's change test
    // EXHAUSTIVENESS, field by field
    // ctrl::diffObject decides what a reload applies from LayerEffects::operator==, so a member
    // that operator does not compare is a member a reload cannot see. It is written as each type's
    // members() beside its declarations for that reason, and this table is what holds it there:
    // one entry per member, each of which must make the two objects DIFFER. A member added to a
    // struct and not to its members() will not be caught here (nothing enumerates a struct), but a
    // member dropped from members() - the way this last broke - fails a named check.
    std::printf("\n== the reload's change test compares every member of a layer's effects ==\n");
    {
        using Effects = core::LayerEffects;
        Effects base;
        base.colorOverlay.enabled = true;
        base.gradientOverlay.enabled = true;
        base.stroke.enabled = true;
        base.silhouettes.push_back(Effects::Silhouette());
        base.seen[Effects::Type_Chrome] = true;
        base.active[Effects::Type_Chrome] = true;
        base.colorOverlay.blendMode = "Nrml";
        base.gradientOverlay.blendMode = "Nrml";
        base.stroke.blendMode = "Nrml";
        base.silhouettes[0].blendMode = "Nrml";

        struct Case {
            const char* name;
            std::function<void(Effects&)> mutate;
        };
        const std::vector<Case> cases = {
            {"colorOverlay.enabled", [](Effects& e) { e.colorOverlay.enabled = !e.colorOverlay.enabled; }},
            {"colorOverlay.r", [](Effects& e) { e.colorOverlay.r += 0.01f; }},
            {"colorOverlay.g", [](Effects& e) { e.colorOverlay.g += 0.01f; }},
            {"colorOverlay.b", [](Effects& e) { e.colorOverlay.b += 0.01f; }},
            {"colorOverlay.opacity", [](Effects& e) { e.colorOverlay.opacity -= 0.01f; }},
            {"colorOverlay.blendMode", [](Effects& e) { e.colorOverlay.blendMode = "Scrn"; }},
            {"colorOverlay.mode",
             [](Effects& e) { e.colorOverlay.mode = img::BlendMode_Multiply; }},
            {"colorOverlay.blendModeKnown",
             [](Effects& e) { e.colorOverlay.blendModeKnown = !e.colorOverlay.blendModeKnown; }},
            {"gradientOverlay.enabled",
             [](Effects& e) { e.gradientOverlay.enabled = !e.gradientOverlay.enabled; }},
            {"gradientOverlay.opacity", [](Effects& e) { e.gradientOverlay.opacity -= 0.01f; }},
            {"gradientOverlay.angleDeg", [](Effects& e) { e.gradientOverlay.angleDeg += 1.0f; }},
            {"gradientOverlay.scale", [](Effects& e) { e.gradientOverlay.scale += 1.0f; }},
            {"gradientOverlay.offsetX", [](Effects& e) { e.gradientOverlay.offsetX += 0.01f; }},
            {"gradientOverlay.offsetY", [](Effects& e) { e.gradientOverlay.offsetY += 0.01f; }},
            {"gradientOverlay.reverse", [](Effects& e) { e.gradientOverlay.reverse = !e.gradientOverlay.reverse; }},
            {"gradientOverlay.style", [](Effects& e) { e.gradientOverlay.style = "Lnr "; }},
            {"gradientOverlay.blendMode", [](Effects& e) { e.gradientOverlay.blendMode = "Scrn"; }},
            {"gradientOverlay.mode", [](Effects& e) { e.gradientOverlay.mode = img::BlendMode_Screen; }},
            {"gradientOverlay.blendModeKnown",
             [](Effects& e) { e.gradientOverlay.blendModeKnown = !e.gradientOverlay.blendModeKnown; }},
            {"gradientOverlay.stops",
             [](Effects& e) {
                 Effects::Stop s;
                 s.location = 0.5f;
                 e.gradientOverlay.stops.push_back(s);
             }},
            {"stroke.enabled", [](Effects& e) { e.stroke.enabled = !e.stroke.enabled; }},
            {"stroke.size", [](Effects& e) { e.stroke.size += 1.0f; }},
            {"stroke.style", [](Effects& e) { e.stroke.style = "OutF"; }},
            {"stroke.paintType", [](Effects& e) { e.stroke.paintType = "GrFl"; }},
            {"stroke.r", [](Effects& e) { e.stroke.r += 0.01f; }},
            {"stroke.g", [](Effects& e) { e.stroke.g += 0.01f; }},
            {"stroke.b", [](Effects& e) { e.stroke.b += 0.01f; }},
            {"stroke.opacity", [](Effects& e) { e.stroke.opacity -= 0.01f; }},
            {"stroke.blendMode", [](Effects& e) { e.stroke.blendMode = "Scrn"; }},
            {"stroke.mode", [](Effects& e) { e.stroke.mode = img::BlendMode_Screen; }},
            {"stroke.blendModeKnown", [](Effects& e) { e.stroke.blendModeKnown = !e.stroke.blendModeKnown; }},
            {"stroke.angleDeg", [](Effects& e) { e.stroke.angleDeg += 1.0f; }},
            {"stroke.scale", [](Effects& e) { e.stroke.scale += 1.0f; }},
            {"stroke.offsetX", [](Effects& e) { e.stroke.offsetX += 0.01f; }},
            {"stroke.offsetY", [](Effects& e) { e.stroke.offsetY += 0.01f; }},
            {"stroke.reverse", [](Effects& e) { e.stroke.reverse = !e.stroke.reverse; }},
            {"stroke.stops",
             [](Effects& e) {
                 Effects::Stop s;
                 s.r = 1.0f;
                 e.stroke.stops.push_back(s);
             }},
            {"silhouettes.size",
             [](Effects& e) { e.silhouettes.push_back(Effects::Silhouette()); }},
            {"silhouettes[0].enabled", [](Effects& e) { e.silhouettes[0].enabled = !e.silhouettes[0].enabled; }},
            {"silhouettes[0].kind",
             [](Effects& e) { e.silhouettes[0].kind = Effects::Silhouette::Kind_DropShadow; }},
            {"silhouettes[0].blur", [](Effects& e) { e.silhouettes[0].blur += 1.0f; }},
            {"silhouettes[0].distance", [](Effects& e) { e.silhouettes[0].distance += 1.0f; }},
            {"silhouettes[0].angleDeg", [](Effects& e) { e.silhouettes[0].angleDeg += 1.0f; }},
            {"silhouettes[0].r", [](Effects& e) { e.silhouettes[0].r += 0.01f; }},
            {"silhouettes[0].g", [](Effects& e) { e.silhouettes[0].g += 0.01f; }},
            {"silhouettes[0].b", [](Effects& e) { e.silhouettes[0].b += 0.01f; }},
            {"silhouettes[0].opacity", [](Effects& e) { e.silhouettes[0].opacity -= 0.01f; }},
            {"silhouettes[0].blendMode", [](Effects& e) { e.silhouettes[0].blendMode = "Scrn"; }},
            {"silhouettes[0].mode", [](Effects& e) { e.silhouettes[0].mode = img::BlendMode_Screen; }},
            {"silhouettes[0].blendModeKnown",
             [](Effects& e) { e.silhouettes[0].blendModeKnown = !e.silhouettes[0].blendModeKnown; }},
            {"seen", [](Effects& e) { e.seen[Effects::Type_Bevel] = !e.seen[Effects::Type_Bevel]; }},
            {"active", [](Effects& e) { e.active[Effects::Type_Bevel] = !e.active[Effects::Type_Bevel]; }},
        };
        expect(base == base, "an effects block equals itself");
        int seen = 0;
        for (const Case& c : cases) {
            Effects mut = base;
            c.mutate(mut);
            ++seen;
            expect(mut != base, QString("a changed %1 is a change to the reload").arg(c.name));
        }
        std::printf("  (%d member cases)\n", seen);
    }

    // ---------------------------------------------------------------- reload: the file gained a layer
    std::printf("\n== reload: the file gained a layer ==\n");
    {
        std::vector<LayerSpec> addedSpecs = baselineSpecs();
        LayerSpec d;
        d.name = "D";
        d.stableId = 105;
        d.rgb[0] = 90; d.rgb[1] = 90; d.rgb[2] = 90;
        addedSpecs.push_back(d); // bottom-first: D lands at the bottom
        expect(writePsd(psdPath, addedSpecs), "the fixture with an extra layer was written");
        updater.reload(*guiItem);
        auto* resD = findResource(*project.resourceHolder().imageTrees().front().topNode, "D");
        expect(resD != nullptr, "the layer the file gained is in the resource tree");
        if (resD) {
            uint8 px[4] = {0, 0, 0, 0};
            const bool got = resD->data().hasImage() && resD->data().image().pixelSize().width() == kSide + 2;
            expect(got, "with its pixels, ready to be used");
        }
        // the timeline is the artist's composition, so a layer the FILE gained must not appear in
        // it by itself - that is a deliberate decision, not a dropped difference
        expect(topNode && findLayer(*topNode, "D") == nullptr,
               "and the timeline is left alone (no object node was invented)");
    }

    // ---------------------------------------------------------------- reload: the file dropped a layer
    std::printf("\n== reload: the file dropped a layer ==\n");
    {
        // C is still drawn by the timeline, so the reload keeps it and says so
        std::vector<LayerSpec> withoutC;
        for (const auto& spec : baselineSpecs()) {
            if (spec.name != "C") {
                withoutC.push_back(spec);
            }
        }
        expect(writePsd(psdPath, withoutC), "the fixture without C was written");
        gui::gLog.clear();
        updater.reload(*guiItem);
        auto* resC = findResource(*project.resourceHolder().imageTrees().front().topNode, "C");
        expect(resC != nullptr, "a layer the timeline still draws is kept");
        bool reported = false;
        for (const QString& line : gui::gLog) {
            if (line.contains("kept")) {
                reported = true;
            }
        }
        expect(reported, "and the reload says it kept it");

        // with its object node gone (the artist deleted it from the timeline) it is removed
        auto* layerC = topNode ? findLayer(*topNode, "C") : nullptr;
        expect(layerC != nullptr, "layer C is in the object tree");
        if (layerC) {
            auto* parent = layerC->parent();
            auto itr = parent->children().begin();
            while (itr != parent->children().end() && *itr != layerC) {
                ++itr;
            }
            if (itr != parent->children().end()) {
                parent->children().erase(itr);
            }
            delete layerC;
        }
        updater.reload(*guiItem);
        resC = findResource(*project.resourceHolder().imageTrees().front().topNode, "C");
        expect(resC == nullptr, "a layer the file dropped and nothing draws is removed");
    }


    // ---------------------------------------------------------------- reload: a format that
    // does not describe everything
    // The plain-bitmap reader cannot say anything about Blend If or layer effects, so a reload of
    // a .png must leave the project's own values alone instead of reading "the file has none" and
    // wiping them. This is the "detect == apply" contract held to what the description KNOWS.
    std::printf("\n== reload: a bitmap, whose reader describes no Blend If ==\n");
    {
        const QString pngPath = dir.filePath("source.png");
        QImage image(kSide, kSide, QImage::Format_RGBA8888);
        image.fill(QColor(70, 80, 90, 255));
        expect(image.save(pngPath), "the PNG fixture was written");

        core::Project pngProject("", animator, nullptr);
        gui::ViaPoint pngVia(nullptr);
        pngVia.setProject(&pngProject);
        NullReporter pngReporter;
        ctrl::ImageFileLoader pngLoader(gl::DeviceInfo::instance());
        expect(pngLoader.load(pngPath, pngProject, pngReporter), "the PNG imports");

        auto* pngTop = pngProject.objectTree().topNode();
        auto* pngLayer = pngTop ? dynamic_cast<core::LayerNode*>(pngTop->children().front()) : nullptr;
        expect(pngLayer != nullptr, "the PNG layer exists");
        core::BlendIf ranges;
        ranges.dst = core::BlendIf::Range{64, 128, 160, 224};
        if (pngLayer) {
            pngLayer->setBlendIf(ranges);
        }

        QTreeWidget pngTree;
        auto* pngItem = gui::res::ResourceUpdater::createGUITree(
            pngTree, *pngProject.resourceHolder().imageTrees().front().topNode, "source.png");
        pngTree.addTopLevelItem(pngItem);
        gui::res::ResourceUpdater pngUpdater(pngVia, pngProject);
        pngUpdater.reload(*pngItem);

        const bool kept = pngLayer && pngLayer->renderer()->blendIf() == ranges;
        expect(kept, "the project's Blend If survives a reload of a format that cannot describe it");

        // ------------------------------------------------------------ and the mask, which the
        // reload used to take away from it
        // The mask channel is described by the PSD reader and by nobody else, but the apply step
        // asked for it unconditionally ("the file is the truth about the mask, present or not"),
        // so a reload of ANY file without a mask channel - every ORA, every bitmap, and every
        // structure-only description - deleted the mask the project had. That is destructive, and
        // it is wrong for a reader that never looked: the mask is applied, inverted and CREATED by
        // an explicit command in this app (core/LayerMaskBake, the panel's "Bake This Layer
        // range"), so it is the artist's data rather than a stale copy of the file's.
        auto* pngRes = pngProject.resourceHolder().imageTrees().empty()
            ? nullptr
            : pngProject.resourceHolder().imageTrees().front().topNode->children().front();
        expect(pngRes != nullptr, "the PNG's resource is the only child of its tree");
        if (pngRes) {
            // what the app's own bake writes: an A8 plane in the image's rect
            const QSize size = pngRes->data().image().pixelSize();
            const size_t count = (size_t)size.width() * size.height();
            uint8* data = new uint8[count];
            std::memset(data, 77, count);
            pngRes->data().grabMask(XCMemBlock(data, count), size);
            expect(pngRes->data().hasMask(), "the fixture put a mask on the plain bitmap's resource");

            // The file's PIXELS change, so the reload has something to apply: that is what the
            // bug needed to bite. A reload with nothing to apply never reaches the content update
            // at all (an unchanged reload pushes no command), so a mask test on an unchanged file
            // would pass even with the mask logic wrong - this one drives the apply path.
            QImage changed(kSide, kSide, QImage::Format_RGBA8888);
            changed.fill(QColor(160, 20, 200, 255));
            expect(changed.save(pngPath), "the PNG fixture's pixels were changed");
            pngUpdater.reload(*pngItem);

            uint8 px[4] = {0, 0, 0, 0};
            auto* pngKey = pngLayer
                ? dynamic_cast<const core::ImageKey*>(pngLayer->timeLine()->defaultKey(core::TimeKeyType_Image))
                : nullptr;
            bool pixelsChanged = false;
            if (pngKey && pngKey->hasImage()) {
                const uint8* d = pngKey->data().resource()->image().data();
                const int w = pngKey->data().resource()->image().pixelSize().width();
                pixelsChanged = d[((size_t)1 * w + 1) * 4] == 160;
            }
            expect(pixelsChanged, "the reload did apply the changed pixels (the content update ran)");

            uint8 after = 0;
            const bool survives = maskPixel(*pngRes, after);
            expect(survives && after == 77,
                   QString("a plain bitmap's reload leaves the project's mask alone (got %1)")
                       .arg(survives ? (int)after : -1));
        }
    }

    // ---------------------------------------------------------------- the other half of the same
    // rule: a reader that DOES describe the mask channel
    // For a PSD the file is the truth about the user-mask channel, absence included - and that is
    // not a taste, it is what keeps the picture right: the app's bake command turns the layer's
    // Blend If SOURCE range into mask pixels and resets the range (core/LayerMaskBake), so a
    // reload that restored the file's ranges (which it must: the node's Blend If is compared and
    // applied) while KEEPING the baked mask would apply the same restriction twice. The mask goes
    // with the range it came from, and the file's version of both wins.
    std::printf("\n== reload: a PSD, whose reader does describe the mask channel ==\n");
    {
        auto* resB = findResource(*project.resourceHolder().imageTrees().front().topNode, "B");
        expect(resB != nullptr, "layer B's resource is in the tree");

        std::vector<LayerSpec> masked = baselineSpecs();
        for (auto& spec : masked) {
            if (spec.name == "B") {
                spec.hasMask = true;
                spec.maskValue = 90;
            }
        }
        expect(writePsd(psdPath, masked), "the fixture with a mask on B was written");
        updater.reload(*guiItem);
        uint8 value = 0;
        const bool applied = resB && maskPixel(*resB, value);
        expect(applied && value == 90,
               QString("the file's mask is applied to a PSD layer (got %1)").arg(applied ? (int)value : -1));

        // and taken away again by the file, which is the half the other readers must not do
        expect(writePsd(psdPath, baselineSpecs()), "the fixture without it was written");
        updater.reload(*guiItem);
        expect(resB && !resB->data().hasMask(),
               "the mask the file dropped is gone with it (the file is the truth here)");
    }

    // ---------------------------------------------------------------- reload: the memory it
    // borrows and must give back
    // An UNCHANGED reload applies nothing, so everything the description allocated - the pixels
    // and the mask of every layer - has to be freed when the description dies (see OwnedBlock).
    // This is the direct measurement of that: array bytes live before and after.
    std::printf("\n== reload: the byte blocks the description allocated ==\n");
    {
        updater.reload(*guiItem); // warm up: the first reload of this state may still grow a cache
        const long long before = gLiveArrayBytes.load();
        updater.reload(*guiItem);
        const long long after = gLiveArrayBytes.load();
        expect(
            after == before,
            QString("an unchanged reload returns every array byte it took (%1 -> %2, %3 bytes leaked)")
                .arg(before)
                .arg(after)
                .arg(after - before));
    }

    std::printf("\n%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
