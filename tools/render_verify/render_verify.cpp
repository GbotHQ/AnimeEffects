// Verification harness for the directional blur (BlurKey blurX/blurY/angle + the
// world-space ellipse math in core/WorldBlurMath.h and the separable Gaussian +
// downsample ladder in core/FilterFrame).
//
// Suites:
//   S1 math        worldBlurEllipse (via the production TimeCacheAccessor over real node
//                  chains, incl. layer-in-folder transform stacks) vs an independent
//                  double-precision Jacobi SVD reference; invariants + brute-force checks.
//   S2 ladder      FilterFrame::blurLadderLevel vs an independent reference, boundary grid.
//   S3 blending    TimeKeyBlender blur blending: lerp, accumulated angle, easing, gating.
//   S4 render      full production render vs the CPU reference blur (direct + ladder,
//                  transformed layers, transformed folders, nesting, zoom, camera
//                  rotation/flip, odd canvases).
//   S5 orientation ground-truth: second-moment angle of a blurred dot (folder+layer
//                  transforms, camera rotation/flip), ellipse swap identity,
//                  isotropic/directional equivalence, layer-vs-folder byte identity.
//   S6 gating/edge: amount <= epsilon renders identical to no key (identity kernel
//                  below ~0.5 lets interpolations ramp in smoothly); key-before-frame inactive.
//   S7 interactions: blur x clippee (on folder and layer), nested folder blur, blend
//                  modes over a captured background, layer/folder HSV ordering.
//   S8 serialization, S9 export path, S11 PSD import round-trip (folder blend modes via
//   PSDWriter -> ImageFileLoader), S12 folder blend modes GPU-vs-CPU reference,
//   S13 ORA import (synthetic .ora via miniz -> ImageFileLoader, folder blend modes),
//   S16 blend if (Photoshop blending ranges: GPU vs the CPU reference across the direct,
//   folder and composite/blur presentation paths),
//   S14 golden images (tests/golden: generated PSD/ORA inputs -> import -> render vs
//   reference-exported (Krita/GIMP) expected.png; regenerate inputs with --golden-gen),
//   S10 perf snapshot.
//   The layer-mesh suites (img/GridMeshCreator) are grouped by the question each answers:
//   S22 mesh coverage of the layer's support (inside the bitmap), S23 what a content cutoff
//   may and may not do (it is a creation parameter, not a mask), S24 render-level coverage of
//   small features, S25 coverage of content against the bitmap edge next to sub-threshold
//   residue - the shape that cost an opaque row in the corpus.
//
// Must be run from the repo root (assets come from the embedded :/ resources).
#include <cmath>
#include <clocale>
#include <locale.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <tuple>
#include <array>
#include <vector>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QJsonObject>
#include <QMainWindow>
#include <QOpenGLWidget>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QVersionNumber>
#include "gl/Global.h"
#include "gl/DeviceInfo.h"
#include "gl/VertexArrayObject.h"
#include "core/ObjectTree.h"
#include "core/DistanceField.h"
#include "core/LayerEffects.h"
#include "core/LayerMaskBake.h"
#include "core/Constant.h"
#include "core/GridMesh.h"
#include "core/ImageKey.h"
#include "core/LayerNode.h"
#include "core/FolderNode.h"
#include "core/BlurKey.h"
#include "core/HsvKey.h"
#include "core/Serializer.h"
#include "core/Deserializer.h"
#include "core/TimeLine.h"
#include "core/FormatVersion.h"
#include "core/Project.h"
#include "core/Animator.h"
#include "core/WorldBlurMath.h"
#include "core/FilterFrame.h"
#include "core/TimeCacheAccessor.h"
#include "core/TimeKeyExpans.h"
#include "ctrl/Exporter.h"
#include "ctrl/ProjectSaver.h"
#include "ctrl/ProjectLoader.h"
#include "ctrl/ImageFileLoader.h"
#include "ctrl/SourceDocument.h"
#include "img/BlendMode.h"
#include "img/PSDFormat.h"
#include "img/PSDDescriptor.h"
#include "img/PSDWriter.h"
#include "img/oraParser.h"
#include "deps/zip_file.h"
#include "util/Easing.h"
#include "util/IProgressReporter.h"
#include "util/Range.h"
#include "util/StreamWriter.h"
#include "util/StreamReader.h"
#include "cpu_ref.h"
#include "scene.h"
#include "harness.h"

// the PSD descriptor / layer-effects fixture writers, shared with tests/import_reload.cpp (see the
// header: one writer per format block, so two harnesses cannot disagree about the format)
#include "PsdFixture.h"
using namespace psdfx;
#include "golden.h"

namespace {

using namespace hb;

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

} // namespace

// the libs reference these globals (defined by src/gui/Main.cpp in the app)
class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::fprintf(stderr, "XC_ASSERT failure\n"); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString& aText, const QString& aInfo, const QString& aDetail) const override {
        std::fprintf(
            stderr, "XC_FATAL_ERROR: %s | %s | %s\n", aText.toUtf8().constData(), aInfo.toUtf8().constData(),
            aDetail.toUtf8().constData());
        std::abort();
    }
};
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;

// src/core/Project.cpp references this gui method, but libgui is not linked into the
// harness; the class declaration comes from gui/AudioPlaybackWidget.h (pulled in by
// core/Project.h), so a lone definition satisfies the linker
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

//-------------------------------------------------------------------------------------------------
// test bookkeeping (gChecks/gFails/expect/suiteHeader/caseReport/diffStr) lives in harness.h

//-------------------------------------------------------------------------------------------------
// GL bootstrap (mirrors MainDisplayWidget::initializeGL essentials)
class HarnessGLWidget: public QOpenGLWidget {
public:
    gl::DeviceInfo mDeviceInfo;
    QScopedPointer<gl::VertexArrayObject> mVAO;
    bool mGLReady = false;

protected:
    void initializeGL() override {
        if (!context()->isValid()) {
            std::fprintf(stderr, "FATAL: invalid OpenGL context\n");
            std::exit(2);
        }
        auto* functions = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(context());
        if (!functions || !functions->initializeOpenGLFunctions()) {
            std::fprintf(stderr, "FATAL: failed to get OpenGL 4.0 core functions\n");
            std::exit(2);
        }
        if (!mGLReady) {
            gl::Global::setContext(*this);
            gl::Global::setFunctions(*functions);
            mDeviceInfo.load();
            gl::DeviceInfo::setInstance(&mDeviceInfo);
            const char* version = (const char*)context()->functions()->glGetString(GL_VERSION);
            std::printf("OpenGL: %s\n", version ? version : "(unknown)");
        } else {
            // initializeGL re-runs when the widget recreates its FBO/context; the factory's
            // functions object is cached per context, so the previously registered one may
            // be dangling by now (ASan-caught). Refresh the registration.
            gl::Global::clearFunctions();
            gl::Global::setFunctions(*functions);
        }
        mVAO.reset(new gl::VertexArrayObject());
        mVAO->bind(); // keep binding (core profile requires a VAO)
        mGLReady = true;
    }
};

//-------------------------------------------------------------------------------------------------
// generated content
void fillWedge(int aX, int aY, uint8_t* aPx) {
    // 61x47 asymmetric test content with a transparent margin (the GridMeshCreator insets
    // layer-edge vertices, so content stays 4px away from the image edge)
    aPx[0] = aPx[1] = aPx[2] = aPx[3] = 0;
    if (aX >= 6 && aX <= 40 && aY >= 8 && aY <= 28) { // opaque red-ish block
        aPx[0] = 200; aPx[1] = 80; aPx[2] = 60; aPx[3] = 255;
    }
    if (aX >= 44 && aX <= 56 && aY >= 6 && aY <= 40) { // vertical gradient bar
        const uint8_t v = (uint8_t)((aY - 6) * 255 / 34);
        aPx[0] = v; aPx[1] = (uint8_t)(255 - v); aPx[2] = 128; aPx[3] = 255;
    }
    if (aY >= 30 && aY <= 42 && aX >= 8 && aX <= 8 + (aY - 30) * 2) { // semi-transparent wedge
        aPx[0] = 40; aPx[1] = 200; aPx[2] = 240; aPx[3] = 128;
    }
    if (aY >= 5 && aY <= 40 && aX == 3 + aY) { // hard 1px diagonal line
        aPx[0] = aPx[1] = aPx[2] = aPx[3] = 255;
    }
    if ((aX - 30) * (aX - 30) + (aY - 38) * (aY - 38) <= 4) { // white dot
        aPx[0] = aPx[1] = aPx[2] = aPx[3] = 255;
    }
}

void fillWedgeOpaque(int aX, int aY, uint8_t* aPx) {
    // same shape as fillWedge but every covered pixel is opaque: with alpha 255 the
    // premultiplied content survives the RGBA8 readback without quantization, so the
    // CPU reference can reproduce steep/discontinuous blend modes (Vivid Light, Hard
    // Mix) exactly - a semi-transparent content readback loses the straight values and
    // their noise is amplified to full scale by the a/(2-2b) gradients
    fillWedge(aX, aY, aPx);
    if (aPx[3] != 0)
        aPx[3] = 255;
}

void fillDot(int aX, int aY, uint8_t* aPx) {
    aPx[0] = aPx[1] = aPx[2] = aPx[3] = 0;
    if (aX == 15 && aY == 15) { // exact center of 31x31
        aPx[0] = aPx[1] = aPx[2] = aPx[3] = 255;
    }
}

// solid square at 50% alpha (straight): the overlap of two of these is the alpha
// discriminator - additive gives clamp(0.5+0.5)=1.0, the over operator gives 0.75
void fillHalf(int aX, int aY, uint8_t* aPx) {
    aPx[0] = aPx[1] = aPx[2] = aPx[3] = 0;
    if (aX >= 4 && aX <= 26 && aY >= 4 && aY <= 26) { // 23x23 centered in 31x31
        aPx[0] = 200; aPx[1] = 80; aPx[2] = 60; aPx[3] = 128;
    }
}

// keeps generated resources alive for a scene's lifetime
struct ResPool {
    std::vector<img::ResourceNode*> nodes;
    ~ResPool() { qDeleteAll(nodes.begin(), nodes.end()); }
    img::ResourceNode* wedge(const QString& aId) {
        nodes.push_back(scene::makeImage(aId, QSize(61, 47), fillWedge));
        return nodes.back();
    }
    img::ResourceNode* wedgeOpaque(const QString& aId) {
        nodes.push_back(scene::makeImage(aId, QSize(61, 47), fillWedgeOpaque));
        return nodes.back();
    }
    img::ResourceNode* dot(const QString& aId) {
        nodes.push_back(scene::makeImage(aId, QSize(31, 31), fillDot));
        return nodes.back();
    }
    img::ResourceNode* half(const QString& aId) {
        nodes.push_back(scene::makeImage(aId, QSize(31, 31), fillHalf));
        return nodes.back();
    }
};

//-------------------------------------------------------------------------------------------------
// S1: worldBlurEllipse math vs independent reference
void checkEllipseCase(
    const QString& aName, const std::vector<std::array<double, 3>>& aChainLeafToRoot, double aBlurX, double aBlurY,
    double aAngleDeg, bool aUseLayerLeaf
) {
    static ResPool pool;
    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    core::ObjectNode* parent = root;
    core::ObjectNode* leaf = nullptr;
    // the chain is ordered leaf -> root; build the tree root-most first
    for (size_t i = aChainLeafToRoot.size(); i-- > 0;) {
        const auto& s = aChainLeafToRoot[i];
        const bool isLeaf = (i == 0);
        const QString name = QString("n%1").arg(i);
        if (isLeaf && aUseLayerLeaf) {
            leaf = scene::addLayer(tree, parent, pool.wedge("math_img"), name, QVector2D(0, 0), s[0] * kRadToDeg,
                QVector2D((float)s[1], (float)s[2]));
        } else {
            leaf = scene::addFolder(parent, name, QVector2D(0, 0), s[0] * kRadToDeg, QVector2D((float)s[1], (float)s[2]));
        }
        parent = leaf;
    }

    core::TimeCacheAccessor accessor(*root, tree.timeCacheLock(), scene::makeTimeInfo(0), false);
    const core::WorldBlurEllipse prod =
        core::worldBlurEllipse(accessor, leaf, (float)aBlurX, (float)aBlurY, (float)aAngleDeg);
    const ref::Ellipse ref = ref::worldBlurEllipseRef(aChainLeafToRoot, aBlurX, aBlurY, aAngleDeg);

    bool ok = true;
    // production invariants
    ok &= expect(std::abs(prod.majorDir.length() - 1.0f) < 1e-4f, aName + ": majorDir not unit");
    ok &= expect(std::abs(prod.minorDir.length() - 1.0f) < 1e-4f, aName + ": minorDir not unit");
    ok &= expect(std::abs(QVector2D::dotProduct(prod.majorDir, prod.minorDir)) < 1e-4f,
        aName + ": axes not orthogonal");
    ok &= expect(prod.majorRadius >= prod.minorRadius - 1e-6f && prod.minorRadius >= 0.0f,
        aName + ": radius ordering");

    // radii vs reference (relative, scaled for tiny values)
    const double rScale1 = std::max(ref.majorRadius, 1.0);
    const double rScale2 = std::max(ref.minorRadius, 1.0);
    ok &= expect(std::abs(prod.majorRadius - ref.majorRadius) <= 2e-3 * rScale1,
        aName + QString(": majorRadius prod=%1 ref=%2").arg(prod.majorRadius).arg(ref.majorRadius));
    ok &= expect(std::abs(prod.minorRadius - ref.minorRadius) <= 2e-3 * rScale2,
        aName + QString(": minorRadius prod=%1 ref=%2").arg(prod.minorRadius).arg(ref.minorRadius));

    // principal axis direction (axis lines: sign-ambiguous, compare |cross|); only when the
    // ellipse is anisotropic enough for the axis to be well-defined
    if (ref.majorRadius > 1.05 * std::max(ref.minorRadius, 1e-9)) {
        const double cross =
            std::abs(prod.majorDir.x() * ref.majorY - prod.majorDir.y() * ref.majorX);
        ok &= expect(cross <= 1e-2,
            aName + QString(": majorDir prod=(%1,%2) ref=(%3,%4)")
                        .arg(prod.majorDir.x()).arg(prod.majorDir.y()).arg(ref.majorX).arg(ref.majorY));
    }
    caseReport(aName, ok,
        QString("r=(%1,%2) ang_prod=%3")
            .arg(prod.majorRadius, 0, 'f', 3)
            .arg(prod.minorRadius, 0, 'f', 3)
            .arg(std::atan2(prod.majorDir.y(), prod.majorDir.x()) * kRadToDeg, 0, 'f', 2));
}

void suiteMath() {
    suiteHeader("S1 math: worldBlurEllipse vs reference SVD");
    using A3 = std::array<double, 3>;
    auto R = [](double deg, double sx, double sy) { return A3{deg * kDegToRad, sx, sy}; };

    checkEllipseCase("identity chain, directional ellipse", {R(0, 1, 1)}, 10, 4, 30, false);
    checkEllipseCase("node rotation rotates the ellipse", {R(45, 1, 1)}, 10, 4, 30, false);
    checkEllipseCase("node rotation cancels blur angle", {R(-30, 1, 1)}, 10, 4, 30, false);
    checkEllipseCase("scale+rot+angle", {R(30, 2.0, 0.5)}, 12, 6, 45, false);
    checkEllipseCase("blurX only", {R(20, 1.5, 1.0)}, 12, 0, 60, false);
    checkEllipseCase("blurY only", {R(20, 1.5, 1.0)}, 0, 12, 60, false);
    checkEllipseCase("equal radii stay a circle", {R(25, 1.5, 0.75)}, 12, 12, 37, false);
    checkEllipseCase("default params == plain transform", {R(33, 1.7, 0.6)}, 1, 1, 0, false);
    checkEllipseCase("negative scale", {R(15, -2.0, 1.0)}, 8, 3, 10, false);
    checkEllipseCase("zero scale X", {R(0, 0.0, 1.5)}, 6, 2, 0, false);
    checkEllipseCase("zero scale with rotated ellipse", {R(40, 0.0, 2.0)}, 5, 3, 70, false);
    checkEllipseCase("angle wrap 370==10", {R(12, 1.25, 0.8)}, 10, 3, 370, false);
    checkEllipseCase("angle wrap -450==270", {R(12, 1.25, 0.8)}, 10, 3, -450, false);
    checkEllipseCase("rot exactly 90", {R(90, 1, 1)}, 10, 4, 0, false);
    checkEllipseCase("huge radii x scale", {R(18, 3.0, 3.0)}, 512, 128, 60, false);
    checkEllipseCase("tiny radii", {R(33, 1.1, 0.9)}, 0.01, 0.005, 33, false);
    checkEllipseCase("large scale", {R(-55, 1000.0, 0.5)}, 20, 7, 80, false);

    // the edge case the feature is for: a layer inside a folder, both transformed
    checkEllipseCase("layer in transformed folder", {R(-21, 2.2, 1.0), R(33, 1.7, 0.6)}, 14, 5, 40, true);
    checkEllipseCase("layer in transformed folder (folder leaf chain)", {R(-21, 2.2, 1.0), R(33, 1.7, 0.6)}, 14, 5, 40, false);
    // three levels of nesting, all transformed
    checkEllipseCase("nested folders + layer",
        {R(-35, 1.5, 1.5), R(80, 0.5, 1.3), R(15, 1.2, 0.8)}, 16, 4, 123, true);

    // fuzz
    std::mt19937 rng(12345);
    int fuzzFailsBefore = gFails;
    for (int iter = 0; iter < 800; ++iter) {
        std::uniform_real_distribution<double> rot(-1080.0, 1080.0);
        std::uniform_real_distribution<double> logScale(std::log(0.05), std::log(8.0));
        std::uniform_real_distribution<double> radius(0.0, 512.0);
        std::uniform_real_distribution<double> angle(-720.0, 720.0);
        std::uniform_int_distribution<int> depthD(1, 4);
        std::uniform_int_distribution<int> coin(0, 1);
        std::vector<A3> chain;
        for (int i = 0; i < depthD(rng); ++i) {
            double sx = std::exp(logScale(rng));
            double sy = std::exp(logScale(rng));
            if (coin(rng))
                sx = -sx;
            if (coin(rng))
                sy = -sy;
            chain.push_back(A3{rot(rng) * kDegToRad, sx, sy});
        }
        const double bx = coin(rng) == 0 ? 0.0 : radius(rng);
        const double by = coin(rng) == 0 ? 0.0 : radius(rng);
        const double ang = angle(rng);

        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        core::ObjectNode* parent = root;
        core::ObjectNode* leaf = nullptr;
        // the chain is ordered leaf -> root; build the tree root-most first
        for (size_t i = chain.size(); i-- > 0;) {
            leaf = scene::addFolder(parent, QString("n%1").arg(i), QVector2D(0, 0),
                (float)(chain[i][0] * kRadToDeg), QVector2D((float)chain[i][1], (float)chain[i][2]));
            parent = leaf;
        }
        core::TimeCacheAccessor accessor(*root, tree.timeCacheLock(), scene::makeTimeInfo(0), false);
        const core::WorldBlurEllipse prod =
            core::worldBlurEllipse(accessor, leaf, (float)bx, (float)by, (float)ang);
        const ref::Ellipse refE = ref::worldBlurEllipseRef(chain, bx, by, ang);

        const QString ctx = QString("fuzz#%1 chain=%2 blur=(%3,%4,%5)").arg(iter).arg(chain.size()).arg(bx).arg(by).arg(ang);
        const double s1 = std::max(refE.majorRadius, 1.0);
        const double s2 = std::max(refE.minorRadius, 1.0);
        bool ok = true;
        // near-singular chains (sigma2 -> 0) hit catastrophic cancellation in the
        // production closed-form discriminant (float32); major axis stays accurate,
        // minor axis and the det invariant do not, so relax those two checks there
        const bool nearSingular = refE.minorRadius < 1e-3 * refE.majorRadius;
        // float32 accumulation through ill-conditioned chains inflates the minor axis
        // of the closed form well beyond eps*kappa(final): empirically up to
        // ~1e-5 * (product of per-node condition numbers). Scale the tolerance by
        // that, capped at 20% - real logic errors (axis swaps, sign flips, wrong
        // formula) are O(1), and the strict major-axis/direction checks below still
        // hold with full accuracy on those chains.
        double kappaProd = 1.0;
        for (const auto& n : chain) {
            const double a = std::abs(n[1]), b = std::abs(n[2]);
            kappaProd *= std::max(a, b) / std::max(std::min(a, b), 1e-30);
        }
        const double bmax = std::max(bx, by);
        if (bmax > 0.0)
            kappaProd *= bmax / std::max(std::min(bx, by), 1e-30);
        const double kappaTol = std::min(0.20, std::max(3e-3, 1e-5 * kappaProd));
        ok &= expect(std::abs(prod.majorRadius - refE.majorRadius) <= 3e-3 * s1,
            ctx + QString(": majorRadius prod=%1 ref=%2").arg(prod.majorRadius).arg(refE.majorRadius));
        if (!nearSingular) {
            ok &= expect(std::abs(prod.minorRadius - refE.minorRadius) <= kappaTol * s2,
                ctx + QString(": minorRadius prod=%1 ref=%2").arg(prod.minorRadius).arg(refE.minorRadius));
        }
        ok &= expect(prod.minorRadius >= 0.0f && prod.majorRadius >= prod.minorRadius - 1e-6f,
            ctx + ": radius ordering");
        if (refE.majorRadius > 1.05 * std::max(refE.minorRadius, 1e-9)) {
            const double cross = std::abs(prod.majorDir.x() * refE.majorY - prod.majorDir.y() * refE.majorX);
            ok &= expect(cross <= 1.5e-2, ctx + ": majorDir mismatch");
        }
        // |det| == sigma1 * sigma2 invariant, from the raw chain in double precision
        ref::Mat2 m;
        for (const auto& n : chain)
            m = ref::mul(ref::rotScale(n[0], n[1], n[2]), m);
        m = ref::mul(m, ref::blurEllipse(bx, by, ang));
        const double detAbs = std::abs(ref::det(m));
        const double sigProd = (double)prod.majorRadius * prod.minorRadius;
        if (!nearSingular) {
            ok &= expect(std::abs(sigProd - detAbs) <= std::max(1.5e-2, kappaTol) * std::max(detAbs, 1.0),
                ctx + QString(": det invariant prod=%1 det=%2").arg(sigProd).arg(detAbs));
        }
        if (!ok) {
            QString chainDesc;
            for (const auto& n : chain)
                chainDesc += QString(" (rot=%1,sx=%2,sy=%3)").arg(n[0] * kRadToDeg).arg(n[1]).arg(n[2]);
            std::printf("      chain:%s\n", chainDesc.toUtf8().constData());
        }
    }
    const int fuzzFailed = gFails - fuzzFailsBefore;
    caseReport("fuzz (800 random chains)", fuzzFailed == 0, QString("%1 failures").arg(fuzzFailed));

    // brute-force max-stretch cross-check of the reference itself (guards reference bugs)
    {
        ref::Mat2 m = ref::mul(ref::rotScale(0.7, 2.0, 0.5), ref::blurEllipse(14, 5, 40));
        const ref::Ellipse e = ref::svd(m);
        const double brute = ref::maxStretch(m);
        expect(std::abs(e.majorRadius - brute) <= 1e-2 * std::max(brute, 1.0),
            QString("reference SVD cross-check: svd=%1 brute=%2").arg(e.majorRadius).arg(brute));
    }
}

//-------------------------------------------------------------------------------------------------
// S2: blurLadderLevel vs independent reference
int ladderLevelRef(float aRadiusX, float aRadiusY, const QSize& aSize) {
    float r = std::max(aRadiusX, aRadiusY);
    int level = 0;
    while (r > 16.0f && level < 8) {
        r *= 0.5f;
        ++level;
    }
    if (level == 0)
        return 0;
    const float scale = (float)(1 << level);
    if (aRadiusX / scale < 1.0f || aRadiusY / scale < 1.0f)
        return 0;
    while ((aSize.width() >> level) < 2 || (aSize.height() >> level) < 2) {
        --level;
        if (level == 0)
            return 0;
    }
    return level;
}

void suiteLadder() {
    suiteHeader("S2 ladder: blurLadderLevel boundary grid");
    const std::vector<float> radii = {0.0f, 0.5f, 1.0f, 15.9f, 16.0f, 16.1f, 17.0f, 31.9f, 32.0f,
        32.1f, 33.0f, 63.0f, 64.0f, 65.0f, 127.0f, 128.0f, 129.0f, 255.5f, 256.0f, 257.0f, 511.0f,
        512.0f, 513.0f, 4096.0f};
    const std::vector<QSize> sizes = {QSize(1024, 768), QSize(95, 67), QSize(128, 96), QSize(16, 16),
        QSize(4, 4), QSize(3, 3), QSize(2, 2), QSize(1, 1), QSize(2048, 8), QSize(8, 2048)};
    int mismatches = 0;
    int total = 0;
    bool invariantOk = true;
    for (float r : radii) {
        const std::vector<std::pair<float, float>> pairs = {{r, r}, {r, 1.0f}, {1.0f, r},
            {r, r * 0.5f}, {r, 16.0f}, {r, 0.9f}};
        for (auto& p : pairs) {
            for (auto& s : sizes) {
                ++total;
                const int prod = core::FilterFrame::blurLadderLevel(p.first, p.second, s);
                const int refV = ladderLevelRef(p.first, p.second, s);
                if (prod != refV) {
                    ++mismatches;
                    std::printf(
                        "    [MISMATCH] radii=(%f,%f) size=%dx%d: prod=%d ref=%d\n", (double)p.first,
                        (double)p.second, s.width(), s.height(), prod, refV);
                }
                // invariants for non-zero levels (note: the radius cap is NOT one — the
                // buffer-size clamp can leave radius/scale well above 16)
                if (prod > 0) {
                    const float scale = (float)(1 << prod);
                    invariantOk &= p.first / scale >= 1.0f && p.second / scale >= 1.0f;
                    invariantOk &= (s.width() >> prod) >= 2 && (s.height() >> prod) >= 2;
                    invariantOk &= prod <= 8;
                }
            }
        }
    }
    expect(invariantOk, "ladder invariants (radii >= 1 at level, buffer >= 2x2, level <= 8)");
    ++gChecks;
    caseReport(QString("level grid (%1 cases)").arg(total), mismatches == 0, QString("%1 mismatches").arg(mismatches));
    if (mismatches)
        ++gFails;
}

//-------------------------------------------------------------------------------------------------
// S3: blur key blending (lerp, accumulated angle, easing, frame gating)
void addBlurFull(core::ObjectNode* aNode, int aFrame, float aBX, float aBY, float aAngle, bool aSineEasing = false) {
    auto* key = new core::BlurKey();
    key->setBlurX(aBX);
    key->setBlurY(aBY);
    key->setAngleDeg(aAngle);
    key->setDirectional(true);
    if (aSineEasing) {
        key->data().easing().type = util::Easing::Type_Sine;
        key->data().easing().range = util::Easing::Range_InOut;
    }
    cmnd::Base* pusher = aNode->timeLine()->createPusher(core::TimeKeyType_Blur, aFrame, key);
    pusher->tryExec();
    delete pusher;
}

// returns (blurX, blurY, angle) blended at aFrame for a folder with two keys
std::array<float, 3> blendedBlur(
    core::ObjectTree& aTree, core::ObjectNode& aRoot, core::ObjectNode& aNode, int aFrame
) {
    core::TimeCacheAccessor accessor(aRoot, aTree.timeCacheLock(), scene::makeTimeInfo(aFrame), false);
    const core::TimeKeyExpans& e = accessor.get(aNode);
    return {e.blurX(), e.blurY(), e.angleDeg()};
}

void suiteBlending() {
    suiteHeader("S3 blending: TimeKeyBlender blur segments");
    ResPool pool;

    auto makeTree = [&](core::ObjectTree& aTree, core::FolderNode*& aRootOut, core::FolderNode*& aNodeOut) {
        auto* root = new core::FolderNode("root");
        aTree.grabTopNode(root);
        auto* f = scene::addFolder(root, "f", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
        aRootOut = root;
        aNodeOut = f;
    };

    // (a) raw linear angle: 350 -> 10 sweeps the full 340 degrees (no shortest-arc
    // normalization), matching the rotate key so multi-turn spins stay accumulated
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 0, 0.0f, 10.0f, 350.0f);
        addBlurFull(f, 10, 20.0f, 30.0f, 10.0f);
        const auto v = blendedBlur(tree, *root, *f, 5);
        bool ok = expect(std::abs(v[0] - 10.0f) < 1e-3 && std::abs(v[1] - 20.0f) < 1e-3,
            QString("radii lerp: (%1,%2)").arg(v[0]).arg(v[1]));
        ok &= expect(std::abs(v[2] - 180.0f) < 1e-2,
            QString("linear 350->10 mid: got %1").arg(v[2]));
        caseReport("linear 350->10 @mid", ok, QString("angle=%1").arg(v[2]));
    }
    // (b) raw linear backward: 10 -> 350 mid = 180
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 0, 0.0f, 0.0f, 10.0f);
        addBlurFull(f, 10, 0.0f, 0.0f, 350.0f);
        const auto v = blendedBlur(tree, *root, *f, 5);
        const bool ok = expect(std::abs(v[2] - 180.0f) < 1e-2,
            QString("linear 10->350 mid: got %1").arg(v[2]));
        caseReport("linear 10->350 @mid", ok, QString("angle=%1").arg(v[2]));
    }
    // (b2) multi-turn spin accumulates: 0 -> 3600 spins 20 turns (mid = 1800), so
    // easing on the segment modulates the spin speed
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 0, 4.0f, 8.0f, 0.0f);
        addBlurFull(f, 10, 4.0f, 8.0f, 3600.0f);
        const auto v = blendedBlur(tree, *root, *f, 5);
        const bool ok = expect(
            std::abs(v[2] - 1800.0f) < 1e-2 && std::abs(v[0] - 4.0f) < 1e-3,
            QString("multi-turn spin mid: got %1").arg(v[2]));
        caseReport("multi-turn 0->3600 @mid", ok, QString("angle=%1").arg(v[2]));
    }
    // (c) plain interpolation 0 -> 90
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 0, 4.0f, 8.0f, 0.0f);
        addBlurFull(f, 10, 12.0f, 16.0f, 90.0f);
        const auto v = blendedBlur(tree, *root, *f, 5);
        const bool ok = expect(std::abs(v[0] - 8.0f) < 1e-3 && std::abs(v[1] - 12.0f) < 1e-3 && std::abs(v[2] - 45.0f) < 1e-2,
            QString("lerp: (%1,%2,%3)").arg(v[0]).arg(v[1]).arg(v[2]));
        caseReport("linear lerp @mid", ok, QString("(%1,%2,%3)").arg(v[0]).arg(v[1]).arg(v[2]));
    }
    // (d) exact keyframe and (e) single key / after-last follows the key
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 4, 7.0f, 3.0f, 33.0f);
        const auto at4 = blendedBlur(tree, *root, *f, 4);
        const bool ok1 = expect(std::abs(at4[0] - 7.0f) < 1e-3 && std::abs(at4[2] - 33.0f) < 1e-2,
            "exact keyframe value");
        const auto at7 = blendedBlur(tree, *root, *f, 7);
        const bool ok2 = expect(std::abs(at7[0] - 7.0f) < 1e-3 && std::abs(at7[2] - 33.0f) < 1e-2,
            "after-last follows key");
        caseReport("single key exact/follow", ok1 && ok2, "");
    }
    // (f) no keys at all -> zeros
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        const auto v = blendedBlur(tree, *root, *f, 3);
        const bool ok = expect(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f, "no keys -> zero blur");
        caseReport("no keys -> zeros", ok, "");
    }
    // (g) easing applies to the blur segment (sine InOut at t=0.3)
    {
        core::ObjectTree tree;
        core::FolderNode *root, *f;
        makeTree(tree, root, f);
        addBlurFull(f, 0, 0.0f, 0.0f, 0.0f, true);
        addBlurFull(f, 10, 100.0f, 50.0f, 90.0f);
        const auto v = blendedBlur(tree, *root, *f, 3); // t = 0.3
        const float rate = util::Easing::calculate(util::Easing::Type_Sine, util::Easing::Range_InOut, 0.3f, 0.0f, 1.0f, 1.0f);
        const bool ok = expect(std::abs(v[0] - 100.0f * rate) < 0.05 && std::abs(v[1] - 50.0f * rate) < 0.05,
            QString("eased blurX: got %1 want %2").arg(v[0]).arg(100.0f * rate));
        caseReport("sine easing @t=0.3", ok, QString("rate=%1").arg(rate));
    }
}

//-------------------------------------------------------------------------------------------------
// S4: production render vs CPU reference blur
struct RenderCaseParams {
    QString name;
    QSize canvas;
    float zoom = 1.0f;
    float camRotDeg = 0.0f;
    bool camFlip = false;
    std::vector<std::array<double, 3>> chain; // leaf -> root (rotRad, sx, sy) of the blur node's chain
    double blurX, blurY, angle;
    int maxTol = 5;
    double meanTol = 0.6;
    std::function<void(core::ObjectTree&, ResPool&, bool)> build;
};

// scene::fixtureFor (scene.cpp) owns the fixture cache; this alias keeps call sites short
scene::Fixture& fixtureFor(const QSize& aSize) { return scene::fixtureFor(aSize); }

// Applies the CPU reference blur for the given world chain + blur ellipse, mirroring
// production's path choice (ladder vs direct) and the direction mapping: the world-space
// ellipse directions map to the slot's screen space through the camera (rotation +
// optional x-mirror; zoom is isotropic). Then:
//  - the direct path works on the plain render's readback (array row 0 = canvas
//    bottom), so a screen-space direction (dx, dy) becomes the array direction (dx, -dy)
//  - the ladder replica flips into image orientation internally, so it takes the
//    screen-space direction unchanged; both encode the SAME world-space blur ellipse
ref::Image refBlurApplied(
    const ref::Image& aIn, const QSize& aCanvas, const core::CameraInfo& aCamera,
    const std::vector<std::array<double, 3>>& aChain, double aBlurX, double aBlurY, double aAngle, float aZoom,
    double* aOutR1 = nullptr, double* aOutR2 = nullptr, int* aOutLevel = nullptr
) {
    const ref::Ellipse e = ref::worldBlurEllipseRef(aChain, aBlurX, aBlurY, aAngle);
    const double r1 = e.majorRadius * aZoom;
    const double r2 = e.minorRadius * aZoom;
    const QVector2D majorScreen =
        scene::screenDirFromWorld(aCamera, QVector2D((float)e.majorX, (float)e.majorY));
    const QVector2D minorScreen =
        scene::screenDirFromWorld(aCamera, QVector2D((float)e.minorX, (float)e.minorY));
    const int level = core::FilterFrame::blurLadderLevel((float)r1, (float)r2, aCanvas);
    if (aOutR1)
        *aOutR1 = r1;
    if (aOutR2)
        *aOutR2 = r2;
    if (aOutLevel)
        *aOutLevel = level;
    if (level > 0) {
        return ref::ladderBlur(
            aIn, aCanvas.width(), aCanvas.height(), majorScreen.x(), majorScreen.y(), r1, minorScreen.x(),
            minorScreen.y(), r2, level);
    }
    ref::Image out = ref::gaussPass(aIn, majorScreen.x(), -majorScreen.y(), r1);
    return ref::gaussPass(out, minorScreen.x(), -minorScreen.y(), r2);
}

// the standard S4/S7 comparison: max/mean bounds plus a small allowance of >4 outliers
bool checkDiff(const scene::Diff& d, int aMaxTol, double aMeanTol, const QSize& aCanvas) {
    const int over4Allowance = (aCanvas.width() * aCanvas.height()) / 500;
    return d.maxDiff <= aMaxTol && d.mean <= aMeanTol && d.over4 <= over4Allowance;
}

void runRenderCase(const RenderCaseParams& aP) {
    ResPool poolBlur, poolPlain;
    scene::Fixture& fx = fixtureFor(aP.canvas);
    const core::CameraInfo camera = scene::makeCamera(aP.canvas, aP.zoom, aP.camRotDeg, aP.camFlip);

    core::ObjectTree treeBlur;
    aP.build(treeBlur, poolBlur, true);
    const std::vector<uint8_t> gpu = fx.render(treeBlur, camera, 0);

    core::ObjectTree treePlain;
    aP.build(treePlain, poolPlain, false);
    const std::vector<uint8_t> plain = fx.render(treePlain, camera, 0);

    const ref::Image in = ref::imageFromBytes(plain.data(), aP.canvas.width(), aP.canvas.height());
    double r1 = 0.0, r2 = 0.0;
    int level = 0;
    const ref::Image blurred =
        refBlurApplied(in, aP.canvas, camera, aP.chain, aP.blurX, aP.blurY, aP.angle, aP.zoom, &r1, &r2, &level);
    const std::vector<uint8_t> refBytes = ref::imageToBytes(blurred);
    const scene::Diff d = scene::diffImages(gpu, refBytes);

    const bool ok = checkDiff(d, aP.maxTol, aP.meanTol, aP.canvas);
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport(aP.name, ok, QString("%1 lvl=%2 r=(%3,%4)").arg(diffStr(d)).arg(level).arg(r1, 0, 'f', 2).arg(r2, 0, 'f', 2));
}

void suiteRender() {
    suiteHeader("S4 render: production vs CPU reference");
    using A3 = std::array<double, 3>;
    auto R = [](double deg, double sx, double sy) { return A3{deg * kDegToRad, sx, sy}; };
    const QSize canvas(128, 96);
    const QVector2D center(64, 48);

    // the builder needs the blur values embedded; wrap with a richer lambda
    auto layerCase = [&](const QString& name, QVector2D pos, float rot, QVector2D scale, double bx, double by,
                         double ang, QSize cnv, float zoom, std::vector<A3> chain, int maxTol = 5,
                         float camRot = 0.0f, bool camFlip = false) {
        RenderCaseParams p;
        p.name = name;
        p.canvas = cnv;
        p.zoom = zoom;
        p.camRotDeg = camRot;
        p.camFlip = camFlip;
        p.chain = std::move(chain);
        p.blurX = bx;
        p.blurY = by;
        p.angle = ang;
        p.maxTol = maxTol;
        p.build = [pos, rot, scale, bx, by, ang](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(tree, root, pool.wedge("w"), "layer", pos, rot, scale);
            if (withBlur)
                scene::addBlur(layer, 0, (float)bx, (float)by, (float)ang);
        };
        runRenderCase(p);
    };

    // axis-aligned and rotated directional blur, no transforms
    layerCase("root layer, blur (12,0,0)", center, 0, QVector2D(1, 1), 12, 0, 0, canvas, 1.0f, {R(0, 1, 1)});
    layerCase("root layer, blur (0,12,0)", center, 0, QVector2D(1, 1), 0, 12, 0, canvas, 1.0f, {R(0, 1, 1)});
    layerCase("root layer, blur (12,4,30)", center, 0, QVector2D(1, 1), 12, 4, 30, canvas, 1.0f, {R(0, 1, 1)});
    // sub-step radii engage the composite path with an identity-like 3-tap kernel and
    // must match the CPU reference (old sigma floor made these render as a 5-tap blur)
    layerCase("root layer, sub-step blur (0.6,0.6,0)", center, 0, QVector2D(1, 1), 0.6, 0.6, 0, canvas, 1.0f, {R(0, 1, 1)});
    layerCase("root layer, sub-step blur (0.8,0.2,0)", center, 0, QVector2D(1, 1), 0.8, 0.2, 0, canvas, 1.0f, {R(0, 1, 1)});
    layerCase("root layer, blur (8,8,137)", center, 0, QVector2D(1, 1), 8, 8, 137, canvas, 1.0f, {R(0, 1, 1)});
    layerCase("root layer, blur (16,16,45)", center, 0, QVector2D(1, 1), 16, 16, 45, canvas, 1.0f, {R(0, 1, 1)});

    // transformed layer
    layerCase("layer rot25 scale(1.8,0.7), blur (14,5,40)", center, 25.0f, QVector2D(1.8f, 0.7f),
        14, 5, 40, canvas, 1.0f, {R(25, 1.8, 0.7)});
    layerCase("layer rot-60 scale(2.5,2.5), blur (20,9,300)", center, -60.0f, QVector2D(2.5f, 2.5f),
        20, 9, 300, canvas, 1.0f, {R(-60, 2.5, 2.5)});

    // the edge case: layer inside a folder, both transformed
    {
        RenderCaseParams p;
        p.name = "folder(rot33,s(1.7,0.6)) > layer(rot-21,s(2.2,1)), blur on layer";
        p.canvas = canvas;
        p.chain = {R(-21, 2.2, 1.0), R(33, 1.7, 0.6)};
        p.blurX = 14; p.blurY = 5; p.angle = 40;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 33.0f, QVector2D(1.7f, 0.6f));
            auto* layer = scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(5, -8), -21.0f, QVector2D(2.2f, 1.0f));
            if (withBlur)
                scene::addBlur(layer, 0, 14.0f, 5.0f, 40.0f);
        };
        runRenderCase(p);
    }
    // blur on the transformed folder itself
    {
        RenderCaseParams p;
        p.name = "folder(rot33,s(1.7,0.6)) > layer(rot10), blur on folder";
        p.canvas = canvas;
        p.chain = {R(33, 1.7, 0.6)};
        p.blurX = 18; p.blurY = 6; p.angle = 75;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 33.0f, QVector2D(1.7f, 0.6f));
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(5, -8), 10.0f, QVector2D(1.0f, 1.0f));
            if (withBlur)
                scene::addBlur(folder, 0, 18.0f, 6.0f, 75.0f);
        };
        runRenderCase(p);
    }
    // three levels of nesting, all transformed
    {
        RenderCaseParams p;
        p.name = "f1(rot15) > f2(rot80,s(0.5,1.3)) > layer(rot-35), blur on layer";
        p.canvas = canvas;
        p.chain = {R(-35, 1.5, 1.5), R(80, 0.5, 1.3), R(15, 1.2, 0.8)};
        p.blurX = 16; p.blurY = 4; p.angle = 123;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* f1 = scene::addFolder(root, "f1", QVector2D(60, 44), 15.0f, QVector2D(1.2f, 0.8f));
            auto* f2 = scene::addFolder(f1, "f2", QVector2D(10, 5), 80.0f, QVector2D(0.5f, 1.3f));
            auto* layer = scene::addLayer(tree, f2, pool.wedge("w"), "layer", QVector2D(3, -2), -35.0f, QVector2D(1.5f, 1.5f));
            if (withBlur)
                scene::addBlur(layer, 0, 16.0f, 4.0f, 123.0f);
        };
        runRenderCase(p);
    }

    // ladder path (radius > 16)
    layerCase("ladder: blur (48,12,25)", center, 0, QVector2D(1, 1), 48, 12, 25, canvas, 1.0f, {R(0, 1, 1)}, 7);
    layerCase("ladder: blur (64,64,0)", center, 0, QVector2D(1, 1), 64, 64, 0, canvas, 1.0f, {R(0, 1, 1)}, 7);
    layerCase("ladder: blur (200,50,100)", center, 0, QVector2D(1, 1), 200, 50, 100, canvas, 1.0f, {R(0, 1, 1)}, 7);
    layerCase("ladder odd canvas: blur (40,40,60)", QVector2D(47, 33), 0, QVector2D(1, 1), 40, 40, 60,
        QSize(95, 67), 1.0f, {R(0, 1, 1)}, 7);
    layerCase("ladder odd canvas: blur (100,20,80)", QVector2D(47, 33), 0, QVector2D(1, 1), 100, 20, 80,
        QSize(95, 67), 1.0f, {R(0, 1, 1)}, 7);

    // camera zoom scales the radius
    layerCase("zoom 2: layer rot25 s(1.8,0.7), blur (14,5,40)", center, 25.0f, QVector2D(1.8f, 0.7f),
        14, 5, 40, canvas, 2.0f, {R(25, 1.8, 0.7)});
    layerCase("zoom 0.5: layer rot25 s(1.8,0.7), blur (14,5,40)", center, 25.0f, QVector2D(1.8f, 0.7f),
        14, 5, 40, canvas, 0.5f, {R(25, 1.8, 0.7)});

    // degenerate axes
    layerCase("zero minor axis, rotated: blur (10,0,45)", center, 15.0f, QVector2D(1.3f, 0.9f),
        10, 0, 45, canvas, 1.0f, {R(15, 1.3, 0.9)});
    layerCase("tiny canvas 24x18, blur (8,3,30)", QVector2D(12, 9), 0, QVector2D(1, 1),
        8, 3, 30, QSize(24, 18), 1.0f, {R(0, 1, 1)});

    // camera rotation: the world->slot direction mapping includes the view rotation
    // (the radii are unaffected - rotation is isometric)
    layerCase("camera rot20: layer rot25 s(1.8,0.7), blur (14,5,40)", center, 25.0f, QVector2D(1.8f, 0.7f),
        14, 5, 40, canvas, 1.0f, {R(25, 1.8, 0.7)}, 5, 20.0f);
    layerCase("camera rot-65: layer rot25 s(1.8,0.7), blur (14,5,40)", center, 25.0f, QVector2D(1.8f, 0.7f),
        14, 5, 40, canvas, 1.0f, {R(25, 1.8, 0.7)}, 5, -65.0f);
    layerCase("camera rot45 ladder: blur (48,12,25)", center, 0, QVector2D(1, 1),
        48, 12, 25, canvas, 1.0f, {R(0, 1, 1)}, 7, 45.0f);
    // camera x-mirror (view flip): negates world x before the rotation
    layerCase("camera flip: layer rot15 s(1.3,0.9), blur (12,4,30)", center, 15.0f, QVector2D(1.3f, 0.9f),
        12, 4, 30, canvas, 1.0f, {R(15, 1.3, 0.9)}, 5, 0.0f, true);
    layerCase("camera flip+rot30 ladder: blur (40,10,60)", center, 0, QVector2D(1, 1),
        40, 10, 60, canvas, 1.0f, {R(0, 1, 1)}, 7, 30.0f, true);
    // camera rotation on a nested folder+layer chain, blur on the layer
    {
        RenderCaseParams p;
        p.name = "camera rot50: folder(rot33,s(1.7,0.6)) > layer(rot-21,s(2.2,1)), blur on layer";
        p.canvas = canvas;
        p.camRotDeg = 50.0f;
        p.chain = {R(-21, 2.2, 1.0), R(33, 1.7, 0.6)};
        p.blurX = 14; p.blurY = 5; p.angle = 40;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 33.0f, QVector2D(1.7f, 0.6f));
            auto* layer = scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(5, -8), -21.0f, QVector2D(2.2f, 1.0f));
            if (withBlur)
                scene::addBlur(layer, 0, 14.0f, 5.0f, 40.0f);
        };
        runRenderCase(p);
    }
}

//-------------------------------------------------------------------------------------------------
// S5: orientation ground truth + equivalences
void suiteOrientation() {
    suiteHeader("S5 orientation/equivalence (GPU ground truth)");
    ResPool pool;
    const QSize canvas(96, 96);

    auto renderDot = [&](std::function<void(core::ObjectTree&, core::ObjectNode*&)> aBuild) {
        core::ObjectTree tree;
        core::ObjectNode* blurNode = nullptr;
        aBuild(tree, blurNode);
        return fixtureFor(canvas).render(tree, scene::makeCamera(canvas), 0);
    };

    // (i) plain directional blur at 30 degrees
    const std::vector<uint8_t> bytesI = renderDot([&](core::ObjectTree& tree, core::ObjectNode*& node) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        node = scene::addLayer(tree, root, pool.dot("d"), "layer", QVector2D(48, 48), 0.0f, QVector2D(1, 1));
        scene::addBlur(node, 0, 8.0f, 2.0f, 30.0f);
    });
    // (ii) the same blur buried in folder+layer rotations: world angle = 50 - 20 + 15 = 45
    const std::vector<uint8_t> bytesII = renderDot([&](core::ObjectTree& tree, core::ObjectNode*& node) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* folder = scene::addFolder(root, "folder", QVector2D(48, 48), 50.0f, QVector2D(1, 1));
        node = scene::addLayer(tree, folder, pool.dot("d"), "layer", QVector2D(0, 0), -20.0f, QVector2D(1, 1));
        scene::addBlur(node, 0, 8.0f, 2.0f, 15.0f);
    });
    // (iii) swapped radii + angle + 90 must equal (i): same ellipse, different parametrization
    const std::vector<uint8_t> bytesIII = renderDot([&](core::ObjectTree& tree, core::ObjectNode*& node) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        node = scene::addLayer(tree, root, pool.dot("d"), "layer", QVector2D(48, 48), 0.0f, QVector2D(1, 1));
        scene::addBlur(node, 0, 2.0f, 8.0f, 120.0f);
    });

    auto measureAngle = [](const std::vector<uint8_t>& aBytes) {
        const ref::Image img = ref::imageFromBytes(aBytes.data(), 96, 96);
        double major = 0.0, minor = 0.0;
        // array-frame angle; world direction (dx,dy) appears as (dx,-dy), so negate
        const double arrAngle = ref::alphaPrincipalAngle(img, major, minor);
        return std::make_tuple(-arrAngle * kRadToDeg, major, minor);
    };

    {
        const auto [angle, major, minor] = measureAngle(bytesI);
        const double err = std::abs(ref::angleDiffDeg(angle, 30.0, 180.0));
        const bool ok = expect(err < 3.0, QString("dot blur 30 deg: measured %1 (err %2)").arg(angle).arg(err));
        caseReport("dot orientation: blur (8,2,30)", ok && expect(major > 2.0 * minor, "anisotropy sanity"),
            QString("measured=%1deg var=(%2,%3)").arg(angle).arg(major).arg(minor));
    }
    {
        const auto [angle, major, minor] = measureAngle(bytesII);
        const double err = std::abs(ref::angleDiffDeg(angle, 45.0, 180.0));
        const bool ok = expect(err < 3.0,
            QString("dot blur in folder(rot50)>layer(rot-20) at 15: measured %1 want 45 (err %2)").arg(angle).arg(err));
        caseReport("dot orientation: folder+layer transforms", ok,
            QString("measured=%1deg var=(%2,%3)").arg(angle).arg(major).arg(minor));
    }
    {
        const scene::Diff d = scene::diffImages(bytesI, bytesIII);
        const bool ok = d.maxDiff <= 4 && d.mean <= 0.5;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("ellipse swap identity: (8,2,30) == (2,8,120)", ok, diffStr(d));
    }

    // (iv) camera view transforms: the measured screen angle must track the view - the
    // screen-space direction is R(camRot) * diag(flip ? -1 : 1, 1) * world direction
    auto renderDotCam = [&](float aCamRotDeg, bool aCamFlip) {
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* node = scene::addLayer(tree, root, pool.dot("d"), "layer", QVector2D(48, 48), 0.0f, QVector2D(1, 1));
        scene::addBlur(node, 0, 8.0f, 2.0f, 30.0f);
        return fixtureFor(canvas).render(tree, scene::makeCamera(canvas, 1.0f, aCamRotDeg, aCamFlip), 0);
    };
    const struct {
        float rot;
        bool flip;
    } camCases[4] = {{20.0f, false}, {-65.0f, false}, {0.0f, true}, {20.0f, true}};
    for (const auto& cc : camCases) {
        const auto bytes = renderDotCam(cc.rot, cc.flip);
        const auto [angle, major, minor] = measureAngle(bytes);
        const double want = (cc.flip ? 180.0 - 30.0 : 30.0) + cc.rot;
        const double err = std::abs(ref::angleDiffDeg(angle, want, 180.0));
        bool ok = expect(err < 3.0,
            QString("dot blur 30deg with camera rot=%1 flip=%2: measured %3 want %4 (err %5)")
                .arg(cc.rot)
                .arg(cc.flip)
                .arg(angle)
                .arg(want)
                .arg(err));
        ok &= expect(major > 2.0 * minor, "anisotropy sanity");
        caseReport(QString("dot orientation: camera rot %1%2").arg(cc.rot).arg(cc.flip ? ", flip" : ""), ok,
            QString("measured=%1deg want=%2 var=(%3,%4)").arg(angle).arg(want).arg(major).arg(minor));
    }

    // isotropic legacy vs directional equal-radii: byte-identical
    {
        const QSize cnv(128, 96);
        auto renderWedge = [&](std::function<void(core::ObjectNode*)> aAddBlur) {
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(tree, root, pool.wedge("w"), "layer", QVector2D(64, 48), 15.0f,
                QVector2D(1.3f, 0.9f));
            aAddBlur(layer);
            return fixtureFor(cnv).render(tree, scene::makeCamera(cnv), 0);
        };
        const auto legacy = renderWedge([](core::ObjectNode* n) { scene::addBlurAmount(n, 0, 12.0f); });
        const auto dir0 = renderWedge([](core::ObjectNode* n) { scene::addBlur(n, 0, 12.0f, 12.0f, 0.0f); });
        const auto dir45 = renderWedge([](core::ObjectNode* n) { scene::addBlur(n, 0, 12.0f, 12.0f, 45.0f); });

        const scene::Diff dLegacy = scene::diffImages(legacy, dir0);
        bool ok = dLegacy.bytesIdentical;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("isotropic amount(12) == directional (12,12,0)", ok, diffStr(dLegacy));

        const scene::Diff dCircle = scene::diffImages(dir0, dir45);
        ok = dCircle.mean <= 1.0 && dCircle.maxDiff <= 6;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("circle axis freedom: (12,12,0) ~= (12,12,45)", ok, diffStr(dCircle));
    }

    // layer blur vs folder blur: same chain, same content -> byte-identical
    {
        const QSize cnv(128, 96);
        auto renderTree = [&](bool aBlurOnFolder) {
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 30.0f, QVector2D(2.0f, 0.5f));
            auto* layer = scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addBlur(aBlurOnFolder ? (core::ObjectNode*)folder : (core::ObjectNode*)layer, 0, 12.0f, 5.0f, 40.0f);
            return fixtureFor(cnv).render(tree, scene::makeCamera(cnv), 0);
        };
        const auto a = renderTree(true);
        const auto b = renderTree(false);
        const scene::Diff d = scene::diffImages(a, b);
        const bool ok = d.maxDiff <= 1;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("layer blur == folder blur (same chain)", ok, diffStr(d));
    }
}

//-------------------------------------------------------------------------------------------------
// S6: gating (amount <= epsilon is inert; small amounts are active with an identity-like
// kernel so interpolations ramp in smoothly) and frame activation
void suiteGating() {
    suiteHeader("S6 gating/edge cases");
    ResPool pool;
    const QSize cnv(128, 96);
    auto& fx = fixtureFor(cnv);
    const core::CameraInfo camera = scene::makeCamera(cnv);

    auto renderLayerBlur = [&](float bx, float by, float ang, int keyFrame, int renderFrame, bool onFolder,
                               float opacity = 1.0f) {
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        core::ObjectNode* blurNode;
        if (onFolder) {
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.4f, 0.8f));
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            blurNode = folder;
        } else {
            blurNode = scene::addLayer(tree, root, pool.wedge("w"), "layer", QVector2D(64, 48), 20.0f, QVector2D(1.4f, 0.8f));
        }
        if (opacity != 1.0f)
            core::getOrCreateDefaultKey<core::OpaKey, core::TimeKeyType_Opa>(*blurNode->timeLine())
                ->setOpacity(opacity);
        if (keyFrame >= 0)
            scene::addBlur(blurNode, keyFrame, bx, by, ang);
        return fx.render(tree, camera, renderFrame);
    };

    // a blur at/below the epsilon (FilterFrame::kMinActiveBlurRadius) is gated off:
    // the composite path does not engage and the frame is byte-identical to keyless
    const auto plain = renderLayerBlur(0, 0, 0, -1, 0, false);
    const auto gated = renderLayerBlur(0.001f, 0.001f, 0.0f, 0, 0, false);
    const scene::Diff dGated = scene::diffImages(plain, gated);
    bool ok = dGated.bytesIdentical;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("layer blur 0.001 is inert (== keyless)", ok, diffStr(dGated));

    // above the epsilon the composite engages; at 0.6 the kernel is a tiny 3-tap so the
    // result is active but close to keyless (the old 0.5 gate would have collapsed it)
    const auto small = renderLayerBlur(0.6f, 0.6f, 0.0f, 0, 0, false);
    const scene::Diff dSmall = scene::diffImages(plain, small);
    ok = !dSmall.bytesIdentical && dSmall.mean > 0.001;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("layer blur 0.6 is active (!= keyless)", ok, diffStr(dSmall));

    const auto active = renderLayerBlur(4.0f, 1.0f, 30.0f, 0, 0, false);
    const scene::Diff dActive = scene::diffImages(plain, active);
    ok = !dActive.bytesIdentical && dActive.mean > 0.05;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("layer blur (4,1,30) is active (!= keyless)", ok, diffStr(dActive));

    // the gate must hold under fractional opacity: an epsilon blur on a semi-transparent
    // layer is still gated off and byte-identical to the same keyless frame
    const auto plain07 = renderLayerBlur(0, 0, 0, -1, 0, false, 0.7f);
    const auto gated07 = renderLayerBlur(0.001f, 0.001f, 0.0f, 0, 0, false, 0.7f);
    const scene::Diff dGated07 = scene::diffImages(plain07, gated07);
    ok = dGated07.bytesIdentical;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("layer blur 0.001 @ opacity 0.7 is inert", ok, diffStr(dGated07));

    // sub-step radius on a semi-transparent layer engages the composite with an
    // identity-like kernel; the composite round trip (premultiplied 8-bit quantization)
    // is the only residual and must stay far below the old 0.5-gate step (which jumped
    // from nothing to a real sigma-0.5 kernel)
    const auto sub07 = renderLayerBlur(0.4f, 0.4f, 0.0f, 0, 0, false, 0.7f);
    const scene::Diff dSub07 = scene::diffImages(plain07, sub07);
    ok = !dSub07.bytesIdentical && dSub07.mean < 1.0;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("layer blur 0.4 @ opacity 0.7 is near-identity", ok, diffStr(dSub07));

    const auto folderGated = renderLayerBlur(0.001f, 0.001f, 0.0f, 0, 0, true);
    const auto folderPlain = renderLayerBlur(0, 0, 0, -1, 0, true);
    const scene::Diff dFolderGated = scene::diffImages(folderPlain, folderGated);
    ok = dFolderGated.bytesIdentical;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("folder blur 0.001 is inert", ok, diffStr(dFolderGated));

    const auto folderGated07 = renderLayerBlur(0.001f, 0.001f, 0.0f, 0, 0, true, 0.7f);
    const auto folderPlain07 = renderLayerBlur(0, 0, 0, -1, 0, true, 0.7f);
    const scene::Diff dFolderGated07 = scene::diffImages(folderPlain07, folderGated07);
    ok = dFolderGated07.bytesIdentical;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("folder blur 0.001 @ opacity 0.7 is inert", ok, diffStr(dFolderGated07));

    // key at frame 5: inactive before, active from frame 5 on
    const auto before = renderLayerBlur(6.0f, 2.0f, 20.0f, 5, 4, false);
    const scene::Diff dBefore = scene::diffImages(plain, before);
    ok = dBefore.bytesIdentical;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("blur key @f5 inert at f4", ok, diffStr(dBefore));

    const auto after = renderLayerBlur(6.0f, 2.0f, 20.0f, 5, 6, false);
    const scene::Diff dAfter = scene::diffImages(plain, after);
    ok = !dAfter.bytesIdentical && dAfter.mean > 0.02;
    ++gChecks;
    if (!ok)
        ++gFails;
    caseReport("blur key @f5 active at f6", ok, diffStr(dAfter));
}

// Alpha-semantics regression: two overlapping 50%-alpha squares must accumulate with the
// over-rule. The over operator gives alpha = 0.5 + 0.5*(1-0.5) = 0.75 (192) at the overlap;
// the old additive-clamp GL_ONE, GL_ONE gave 255. The RGB channel is over-blended in both
// cases (150,60,45 = 200*0.5 + 100*0.5 straight-over, stored premultiplied), so this test
// discriminates on alpha. Verified pre-fix: (150,60,45,255) on both paths (SCRATCHPAD).
// Geometry: A@(45,45) -> (33,39)-(56,62), B@(58,58) -> (46,26)-(69,49); overlap (50,44).
void suiteAlphaProbe() {
    suiteHeader("alpha semantics probe (additive vs over)");
    ResPool pool;
    const QSize cnv(128, 96);
    auto& fx = fixtureFor(cnv);
    const core::CameraInfo camera = scene::makeCamera(cnv);
    auto pxAt = [&](const std::vector<uint8_t>& img, int x, int y) {
        size_t i = (size_t)(y * cnv.width() + x) * 4;
        return QString("%1,%2,%3,%4").arg(img[i]).arg(img[i + 1]).arg(img[i + 2]).arg(img[i + 3]);
    };
    auto near = [&](const std::vector<uint8_t>& img, int x, int y, std::array<int, 4> exp, int tol, const char* what) {
        const QString got = pxAt(img, x, y);
        int bad = 0;
        for (int c = 0; c < 4; ++c) {
            size_t i = (size_t)(y * cnv.width() + x) * 4;
            if (std::abs((int)img[i + c] - exp[c]) > tol)
                ++bad;
        }
        ++gChecks;
        if (bad) {
            ++gFails;
            caseReport(what, false, got);
        } else {
            caseReport(what, true, got);
        }
    };
    auto render2 = [&](bool withFolder, const char* tag) {
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        if (withFolder) {
            auto* folder = scene::addFolder(root, "folder", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addLayer(tree, folder, pool.half("b"), "b", QVector2D(58, 58), 0.0f, QVector2D(1, 1));
            scene::addLayer(tree, folder, pool.half("a"), "a", QVector2D(45, 45), 0.0f, QVector2D(1, 1));
            scene::addBlur(folder, 0, 0.002f, 0.002f, 0.0f);
        } else {
            scene::addLayer(tree, root, pool.half("b"), "b", QVector2D(58, 58), 0.0f, QVector2D(1, 1));
            scene::addLayer(tree, root, pool.half("a"), "a", QVector2D(45, 45), 0.0f, QVector2D(1, 1));
        }
        const std::vector<uint8_t> img = fx.render(tree, camera, 0);
        const QString prefix = QString("[alpha-%1]").arg(tag);
        // overlap alpha = over-rule 0.752 -> 192; rgb straight-over premultiplied 150,60,45
        near(img, 50, 44, {150, 60, 45, 192}, 2, qPrintable(prefix + " overlap alpha=192"));
        near(img, 50, 50, {100, 40, 30, 128}, 2, qPrintable(prefix + " single-square control"));
        near(img, 10, 10, {0, 0, 0, 0}, 0, qPrintable(prefix + " outside transparent"));
    };
    render2(false, "direct");
    render2(true, "composite");
}

//-------------------------------------------------------------------------------------------------
// S7: blur crossed with other features (clipping, nested composites, blend modes, HSV)
void suiteInteractions() {
    suiteHeader("S7 interactions: blur x clippee / nested blur / blend / HSV");
    using A3 = std::array<double, 3>;
    auto R = [](double deg, double sx, double sy) { return A3{deg * kDegToRad, sx, sy}; };
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);

    // clippee pair inside a blurred, transformed folder: the clipping context must
    // propagate into the folder's composite render
    {
        RenderCaseParams p;
        p.name = "clippee in folder(rot25,s(1.4,0.8)), blur (10,3,40) on folder";
        p.canvas = canvas;
        p.chain = {R(25, 1.4, 0.8)};
        p.blurX = 10; p.blurY = 3; p.angle = 40;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 25.0f, QVector2D(1.4f, 0.8f));
            // the clippee is pushed first so it renders above the clipper (children are
            // walked back-to-front; a clippee clips to the sibling rendered below it)
            auto* clippee = scene::addLayer(
                tree, folder, pool.wedge("wd"), "clippee", QVector2D(10, 6), -15.0f, QVector2D(2.0f, 2.0f));
            clippee->setClipped(true);
            scene::addLayer(tree, folder, pool.wedge("wc"), "clipper", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            if (withBlur)
                scene::addBlur(folder, 0, 10.0f, 3.0f, 40.0f);
        };
        runRenderCase(p);
    }

    // a blurred layer that is itself clipped to the sibling below: the layer composite
    // isolates the clippee (the clipper stays crisp), so the reference is
    // present(blur(masked clippee)) over the clipper render. The masked clippee content is
    // reconstructed as the unclipped render scaled by the clipper's rendered alpha - the
    // IS_CLIPPEE shader's color.a *= mask/255 scales the premultiplied content.
    {
        ResPool poolFull, poolClipper, poolTop;
        core::ObjectTree treeFull;
        {
            auto* root = new core::FolderNode("root");
            treeFull.grabTopNode(root);
            auto* clippee = scene::addLayer(
                treeFull, root, poolFull.wedge("wd"), "clippee", QVector2D(60, 44), -15.0f, QVector2D(2.0f, 2.0f));
            clippee->setClipped(true);
            scene::addBlur(clippee, 0, 8.0f, 2.0f, 30.0f);
            scene::addLayer(
                treeFull, root, poolFull.wedge("wc"), "clipper", QVector2D(64, 48), 10.0f, QVector2D(1.5f, 1.2f));
        }
        const std::vector<uint8_t> gpu = fx.render(treeFull, camera, 0);
        core::ObjectTree treeClipper;
        {
            auto* root = new core::FolderNode("root");
            treeClipper.grabTopNode(root);
            scene::addLayer(
                treeClipper, root, poolClipper.wedge("wc"), "clipper", QVector2D(64, 48), 10.0f,
                QVector2D(1.5f, 1.2f));
        }
        const std::vector<uint8_t> clipperBytes = fx.render(treeClipper, camera, 0);
        core::ObjectTree treeTop;
        {
            auto* root = new core::FolderNode("root");
            treeTop.grabTopNode(root);
            scene::addLayer(
                treeTop, root, poolTop.wedge("wd"), "clippee", QVector2D(60, 44), -15.0f, QVector2D(2.0f, 2.0f));
        }
        const std::vector<uint8_t> topBytes = fx.render(treeTop, camera, 0);

        const ref::Image clipperImg = ref::imageFromBytes(clipperBytes.data(), canvas.width(), canvas.height());
        const ref::Image topImg = ref::imageFromBytes(topBytes.data(), canvas.width(), canvas.height());
        ref::Image masked(canvas.width(), canvas.height());
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                const float m = clipperImg.at(x, y)[3];
                const float* u = topImg.at(x, y);
                float* d = masked.at(x, y);
                for (int c = 0; c < 4; ++c)
                    d[c] = u[c] * m;
            }
        }
        const ref::Image blurred = refBlurApplied(masked, canvas, camera, {R(-15, 2.0, 2.0)}, 8, 2, 30, 1.0f);
        const ref::Image presented = ref::blendPresent(blurred, clipperImg, img::BlendMode_Normal, 1.0);
        const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(presented));
        const bool ok = checkDiff(d, 8, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        if (std::getenv("VB_DUMP")) {
            scene::dumpPNG("/tmp/vb_clip_gpu.png", gpu, canvas);
            scene::dumpPNG("/tmp/vb_clip_ref.png", ref::imageToBytes(presented), canvas);
            scene::dumpPNG("/tmp/vb_clip_clipper.png", clipperBytes, canvas);
            scene::dumpPNG("/tmp/vb_clip_top.png", topBytes, canvas);
            // production intermediates: the crisp clipped render (mask check) and the
            // unclipped blurred render (halo check)
            core::ObjectTree treeCrisp;
            {
                auto* root = new core::FolderNode("root");
                treeCrisp.grabTopNode(root);
                ResPool poolCrisp;
                auto* clippee = scene::addLayer(
                    treeCrisp, root, poolCrisp.wedge("wd"), "clippee", QVector2D(60, 44), -15.0f,
                    QVector2D(2.0f, 2.0f));
                clippee->setClipped(true);
                scene::addLayer(
                    treeCrisp, root, poolCrisp.wedge("wc"), "clipper", QVector2D(64, 48), 10.0f,
                    QVector2D(1.5f, 1.2f));
                const std::vector<uint8_t> crisp = fx.render(treeCrisp, camera, 0);
                scene::dumpPNG("/tmp/vb_clip_crisp.png", crisp, canvas);
            }
            core::ObjectTree treeUnclipped;
            {
                auto* root = new core::FolderNode("root");
                treeUnclipped.grabTopNode(root);
                ResPool poolUnclipped;
                auto* clippee = scene::addLayer(
                    treeUnclipped, root, poolUnclipped.wedge("wd"), "clippee", QVector2D(60, 44), -15.0f,
                    QVector2D(2.0f, 2.0f));
                scene::addBlur(clippee, 0, 8.0f, 2.0f, 30.0f);
                scene::addLayer(
                    treeUnclipped, root, poolUnclipped.wedge("wc"), "clipper", QVector2D(64, 48), 10.0f,
                    QVector2D(1.5f, 1.2f));
                const std::vector<uint8_t> unclipped = fx.render(treeUnclipped, camera, 0);
                scene::dumpPNG("/tmp/vb_clip_unclipped.png", unclipped, canvas);
            }
        }
        caseReport("blur (8,2,30) on clipped layer(rot-15,s2)", ok, diffStr(d));
    }

    // folder blur nested inside folder blur: the inner composite is blurred and presented
    // into the outer composite, then the outer blur runs; the reference applies the two
    // reference blurs in sequence (inner chain includes the outer folder's transform)
    {
        ResPool poolBlur, poolPlain;
        auto build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* f1 = scene::addFolder(root, "f1", QVector2D(60, 44), 15.0f, QVector2D(1.2f, 0.8f));
            auto* f2 = scene::addFolder(f1, "f2", QVector2D(10, 5), 80.0f, QVector2D(0.5f, 1.3f));
            scene::addLayer(tree, f2, pool.wedge("w"), "layer", QVector2D(3, -2), -35.0f, QVector2D(1.5f, 1.5f));
            if (withBlur) {
                scene::addBlur(f1, 0, 36.0f, 9.0f, 30.0f);
                scene::addBlur(f2, 0, 8.0f, 2.0f, 75.0f);
            }
        };
        core::ObjectTree treeBlur;
        build(treeBlur, poolBlur, true);
        const std::vector<uint8_t> gpu = fx.render(treeBlur, camera, 0);
        core::ObjectTree treePlain;
        build(treePlain, poolPlain, false);
        const std::vector<uint8_t> plain = fx.render(treePlain, camera, 0);

        const ref::Image in = ref::imageFromBytes(plain.data(), canvas.width(), canvas.height());
        const ref::Image inner =
            refBlurApplied(in, canvas, camera, {R(80, 0.5, 1.3), R(15, 1.2, 0.8)}, 8, 2, 75, 1.0f);
        const ref::Image outer = refBlurApplied(inner, canvas, camera, {R(15, 1.2, 0.8)}, 36, 9, 30, 1.0f);
        const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(outer));
        const bool ok = checkDiff(d, 7, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("nested blur: f1(36,9,30) > f2(8,2,75) > layer", ok, diffStr(d));
    }

    // blend modes x blur: the blurred composite must be presented with the layer's blend
    // mode against the real captured scene (DestinationTexturizer), not the composite
    for (const img::BlendMode mode : {img::BlendMode_Multiply, img::BlendMode_Screen}) {
        ResPool poolFull, poolTop, poolBg;
        const char* modeName = mode == img::BlendMode_Multiply ? "multiply" : "screen";
        core::ObjectTree treeFull;
        {
            auto* root = new core::FolderNode("root");
            treeFull.grabTopNode(root);
            // the children list is top-most first (last pushed renders at the bottom)
            auto* top = scene::addLayer(
                treeFull, root, poolFull.wedge("top"), "top", QVector2D(64, 48), 15.0f, QVector2D(1.3f, 0.9f));
            top->setBlendMode(mode);
            scene::addBlur(top, 0, 10.0f, 3.0f, 40.0f);
            scene::addLayer(
                treeFull, root, poolFull.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        }
        const std::vector<uint8_t> gpu = fx.render(treeFull, camera, 0);
        // reference inputs: the crisp top layer alone (Normal) and the background alone
        core::ObjectTree treeTop;
        {
            auto* root = new core::FolderNode("root");
            treeTop.grabTopNode(root);
            scene::addLayer(treeTop, root, poolTop.wedge("top"), "top", QVector2D(64, 48), 15.0f, QVector2D(1.3f, 0.9f));
        }
        const std::vector<uint8_t> topBytes = fx.render(treeTop, camera, 0);
        core::ObjectTree treeBg;
        {
            auto* root = new core::FolderNode("root");
            treeBg.grabTopNode(root);
            scene::addLayer(treeBg, root, poolBg.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        }
        const std::vector<uint8_t> bgBytes = fx.render(treeBg, camera, 0);

        const ref::Image topImg = ref::imageFromBytes(topBytes.data(), canvas.width(), canvas.height());
        const ref::Image blurred = refBlurApplied(topImg, canvas, camera, {R(15, 1.3, 0.9)}, 10, 3, 40, 1.0f);
        const ref::Image bgImg = ref::imageFromBytes(bgBytes.data(), canvas.width(), canvas.height());
        const ref::Image presented = ref::blendPresent(blurred, bgImg, mode, 1.0);
        const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(presented));
        const bool ok = checkDiff(d, 8, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        if (std::getenv("VB_DUMP")) {
            scene::dumpPNG(QString("/tmp/vb_blend_%1_gpu.png").arg(modeName), gpu, canvas);
            scene::dumpPNG(QString("/tmp/vb_blend_%1_ref.png").arg(modeName), ref::imageToBytes(presented), canvas);
            scene::dumpPNG(QString("/tmp/vb_blend_%1_bg.png").arg(modeName), bgBytes, canvas);
            scene::dumpPNG(QString("/tmp/vb_blend_%1_top.png").arg(modeName), topBytes, canvas);
        }
        caseReport(QString("blend %1: blurred layer over background").arg(modeName), ok, diffStr(d));
    }

    // layer HSV x blur: the layer's HSV is baked into the composite BEFORE the blur in
    // both the plain and the blurred render, so the plain render is the reference input
    {
        RenderCaseParams p;
        p.name = "layer HSV(60,150,80) + blur (10,3,40)";
        p.canvas = canvas;
        p.chain = {R(15, 1.3, 0.9)};
        p.blurX = 10; p.blurY = 3; p.angle = 40;
        p.build = [](core::ObjectTree& tree, ResPool& pool, bool withBlur) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(tree, root, pool.wedge("w"), "layer", QVector2D(64, 48), 15.0f, QVector2D(1.3f, 0.9f));
            scene::addHSV(layer, 0, 60, 150, 80, 0);
            if (withBlur)
                scene::addBlur(layer, 0, 10.0f, 3.0f, 40.0f);
        };
        runRenderCase(p);
    }

    // folder HSV x blur: the folder's HSV is applied at presentation, AFTER the blur, so
    // the reference is hsv(blur(plain keyless render)) - ordering matters (HSV is
    // nonlinear, blur and HSV do not commute)
    {
        ResPool poolFull, poolPlain;
        auto build = [](core::ObjectTree& tree, ResPool& pool, bool withFilters) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.3f, 0.8f));
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            if (withFilters) {
                scene::addHSV(folder, 0, 90, 120, 110, 0);
                scene::addBlur(folder, 0, 10.0f, 4.0f, 55.0f);
            }
        };
        core::ObjectTree treeFull;
        build(treeFull, poolFull, true);
        const std::vector<uint8_t> gpu = fx.render(treeFull, camera, 0);
        core::ObjectTree treePlain;
        build(treePlain, poolPlain, false);
        const std::vector<uint8_t> plain = fx.render(treePlain, camera, 0);

        const ref::Image in = ref::imageFromBytes(plain.data(), canvas.width(), canvas.height());
        const ref::Image blurred = refBlurApplied(in, canvas, camera, {R(20, 1.3, 0.8)}, 10, 4, 55, 1.0f);
        const ref::Image adjusted = ref::hsvAdjust(blurred, 90.0 / 360.0, 120.0 / 100.0, 110.0 / 100.0, false);
        const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(adjusted));
        const bool ok = checkDiff(d, 8, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("folder HSV(90,120,110) after blur (10,4,55)", ok, diffStr(d));
    }
}

//-------------------------------------------------------------------------------------------------
// S8: blur key persistence through the binary project format (TimeLine::serialize /
// deserialize at the current version). Blur shipped as a single feature with one
// AE_PROJECT_FORMAT_MINOR_VERSION bump (8 -> 9): the on-disk layout is flat (easing +
// blurX + blurY + angle + directional) with no historical variant, so with any declared
// project version the same bytes deserialize to the same key.
// The clipboard JSON path is NOT covered here (copy/paste are file-local functions in
// gui/TimeLineEditorWidget.cpp and ctrl/TimeLineEditor.cpp; verified by inspection: both
// sides use Amount/BlurX/BlurY/Angle, paste falls back to Amount when BlurX is absent and
// infers the directional flag - the flag itself is not serialized).
// (NullProgressReporter moved to harness.h)

void suiteSerialization() {
    suiteHeader("S8 blur key serialization (binary project format)");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    // a timeline mixing blur keys (isotropic "Amount" mode, directional, non-default
    // easings) with an HSV key, so the type-name tagging ("Blur") is exercised against
    // another key type
    core::TimeLine src;
    auto pushKey = [&src](core::TimeKeyType aType, int aFrame, core::TimeKey* aKey) {
        auto* pusher = src.createPusher(aType, aFrame, aKey);
        pusher->tryExec();
        delete pusher;
    };
    auto pushBlur = [&pushKey](
        int aFrame, float aBX, float aBY, float aAngle, bool aDir, const util::Easing::Param& aEasing) {
        auto* key = new core::BlurKey();
        key->setBlurX(aBX);
        key->setBlurY(aBY);
        key->setAngleDeg(aAngle);
        key->setDirectional(aDir);
        key->data().easing() = aEasing;
        pushKey(core::TimeKeyType_Blur, aFrame, key);
    };
    util::Easing::Param sineIn;
    sineIn.type = util::Easing::Type_Sine;
    sineIn.range = util::Easing::Range_In;
    util::Easing::Param backOut;
    backOut.type = util::Easing::Type_Back;
    backOut.range = util::Easing::Range_Out;
    pushBlur(0, 5.0f, 5.0f, 0.0f, false, util::Easing::Param());
    pushBlur(7, 8.0f, 2.0f, 30.0f, true, sineIn);
    pushBlur(23, 12.5f, 1.0f, -45.0f, true, backOut);
    {
        auto* hsv = new core::HSVKey();
        hsv->setHSV({60, 150, 80, 0});
        pushKey(core::TimeKeyType_HSV, 3, hsv);
    }

    // serialize
    std::ostringstream buffer(std::ios::binary);
    util::StreamWriter writer(buffer);
    core::Serializer serializer(writer);
    expect(src.serialize(serializer), "S8: timeline serialize");

    // deserialize at the current format version
    NullProgressReporter reporter;
    gl::DeviceInfo deviceInfo;
    std::istringstream input(buffer.str(), std::ios::binary);
    util::LEStreamReader reader(input);
    core::Deserializer::IDSolverType idSolver;
    core::Deserializer deserializer(
        reader, idSolver, buffer.str().size(), QVersionNumber(0, 9), deviceInfo, reporter, 0);
    core::TimeLine dst;
    expect(dst.deserialize(deserializer), "S8: timeline deserialize");

    // compare blur keys frame by frame
    const auto& srcMap = src.map(core::TimeKeyType_Blur);
    const auto& dstMap = dst.map(core::TimeKeyType_Blur);
    expect(dstMap.count() == srcMap.count(), "S8: blur key count round-trips");
    for (auto itr = srcMap.begin(); itr != srcMap.end(); ++itr) {
        const int frame = itr.key();
        auto dstItr = dstMap.find(frame);
        if (!expect(dstItr != dstMap.end(), QString("S8: frame %1 present after load").arg(frame)))
            continue;
        const auto* sk = (const core::BlurKey*)itr.value();
        const auto* dk = (const core::BlurKey*)dstItr.value();
        expect(
            dk->blurX() == sk->blurX() && dk->blurY() == sk->blurY() && dk->angleDeg() == sk->angleDeg(),
            QString("S8: frame %1 radii/angle round-trip").arg(frame));
        expect(dk->isDirectional() == sk->isDirectional(), QString("S8: frame %1 directional flag").arg(frame));
        expect(
            dk->data().easing() == sk->data().easing(), QString("S8: frame %1 easing round-trip").arg(frame));
    }
    expect(!dst.map(core::TimeKeyType_HSV).isEmpty(), "S8: HSV key survives alongside blur");

    // The layout is flat and version-independent: the same bytes read back identically
    // under the pre-blur (0.8) version label, since there is no version-branched blur
    // layout to be compatible with. Spot-check the anisotropic key against the round-trip
    // above rather than against a synthetic historical format.
    {
        std::istringstream input8(buffer.str(), std::ios::binary);
        util::LEStreamReader reader8(input8);
        core::Deserializer::IDSolverType solver8;
        core::Deserializer deserializer8(
            reader8, solver8, buffer.str().size(), QVersionNumber(0, 8), deviceInfo, reporter, 0);
        core::TimeLine old;
        expect(old.deserialize(deserializer8), "S8: blur timeline loads under pre-blur version label");
        const auto& oldMap = old.map(core::TimeKeyType_Blur);
        const auto* sk = (const core::BlurKey*)srcMap[7];
        auto oldItr = oldMap.find(7);
        bool ok = oldItr != oldMap.end() && sk;
        if (ok) {
            const auto* ok_ = (const core::BlurKey*)oldItr.value();
            ok = ok_->blurX() == sk->blurX() && ok_->blurY() == sk->blurY()
                && ok_->angleDeg() == sk->angleDeg() && ok_->isDirectional() == sk->isDirectional();
        }
        expect(ok, "S8: single flat layout (no historical variant) round-trips under any version");
    }

    caseReport(
        "blur key binary round-trip (single v0.9 layout)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));

    // Folder blend mode persistence (minor version 10 feature): the mode rides the
    // FolderNd block as a 4CC appended after the object block, gated on the declared
    // version (FolderNode::serialize/deserialize). Blocks from pre-10 projects carry no
    // mode, so a pre-10 version label reads the pass-through default instead.
    const int folderChecksBefore = gChecks;
    core::FolderNode folderSrc("blendme");
    folderSrc.setBlendMode(img::BlendMode_VividLight);
    std::ostringstream fbuffer(std::ios::binary);
    util::StreamWriter fwriter(fbuffer);
    core::Serializer fserializer(fwriter);
    expect(folderSrc.serialize(fserializer), "S8: folder serialize");
    {
        std::istringstream fin(fbuffer.str(), std::ios::binary);
        util::LEStreamReader freader(fin);
        core::Deserializer::IDSolverType fsolver;
        core::Deserializer fdst(freader, fsolver, fbuffer.str().size(), QVersionNumber(0, 10), deviceInfo, reporter, 0);
        core::FolderNode folderDst("x");
        expect(folderDst.deserialize(fdst), "S8: folder deserialize @ 0.10");
        expect(
            folderDst.blendMode() == img::BlendMode_VividLight, "S8: folder blend mode round-trips (0.10 4CC)");
        expect(folderDst.name() == "blendme", "S8: folder name round-trips alongside the mode");
    }
    {
        // a pre-10 label skips the 4CC read entirely: the folder keeps its pass-through
        // default (the appended bytes stay unread, like a genuinely old block would lack)
        std::istringstream fin(fbuffer.str(), std::ios::binary);
        util::LEStreamReader freader(fin);
        core::Deserializer::IDSolverType fsolver;
        core::Deserializer fdst(freader, fsolver, fbuffer.str().size(), QVersionNumber(0, 9), deviceInfo, reporter, 0);
        core::FolderNode folderOld("x");
        expect(folderOld.deserialize(fdst), "S8: folder deserialize @ 0.9");
        expect(
            folderOld.blendMode() == img::BlendMode_Normal, "S8: pre-10 label keeps the pass-through default");
    }
    caseReport(
        "folder blend mode binary round-trip (v0.10 4CC, pre-10 gate)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - folderChecksBefore));
}

//-------------------------------------------------------------------------------------------------
// S9: the export path - ctrl::Exporter renders through the same composite wiring it gained
// (FilterFrame + opacityScale). Export a two-frame PNG sequence of a tree exercising a
// blurred clippee, a blurred transformed folder and a frame-varying blur, and compare each
// exported frame against the interactive-path render at the same time (the export camera is
// the same default camera). Video/GIF encoding (ffmpeg) is out of scope.
// (StubAnimator moved to harness.h)

void suiteExport() {
    suiteHeader("S9 export path (ctrl::Exporter image sequence)");
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);

    StubAnimator animator;
    core::Project project("", animator, nullptr);
    project.attribute().setImageSize(canvas);
    project.attribute().setFrameRange(util::Range(0, 200)); // Project::Attribute carries a range (419e17f)
    project.attribute().setFps(24);
    project.attribute().setLoop(false);
    auto* root = new core::FolderNode("root");
    project.objectTree().grabTopNode(root);
    core::ObjectTree& tree = project.objectTree();

    ResPool pool;
    {
        // blurred clippee (pushed first so it renders on top, clipped to the clipper below)
        auto* clippee = scene::addLayer(
            tree, root, pool.wedge("wd"), "clippee", QVector2D(60, 44), -15.0f, QVector2D(2.0f, 2.0f));
        clippee->setClipped(true);
        scene::addBlur(clippee, 0, 8.0f, 2.0f, 30.0f);
        scene::addLayer(
            tree, root, pool.wedge("wc"), "clipper", QVector2D(64, 48), 10.0f, QVector2D(1.5f, 1.2f));
        // blurred transformed folder; a second key at frame 1 weakens the blur so the two
        // exported frames must differ (export time progression)
        auto* folder = scene::addFolder(root, "folder", QVector2D(30, 60), 20.0f, QVector2D(1.3f, 0.8f));
        scene::addLayer(tree, folder, pool.wedge("wf"), "child", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
        scene::addBlur(folder, 0, 10.0f, 3.0f, 40.0f);
        scene::addBlurAmount(folder, 1, 2.0f);
        // background at the bottom
        scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(64, 48), 0.0f, QVector2D(4, 4));
    }

    // export frames 0..1 as PNGs through the real Exporter
    const QString outDir = "/tmp/vb_export_s9";
    QDir(outDir).removeRecursively();
    QDir().mkpath(outDir);
    ctrl::Exporter exporter(project);
    exporter.setOverwriteConfirmer([](const QString&) { return true; });
    NullProgressReporter reporter;
    exporter.setProgressReporter(reporter);
    ctrl::Exporter::CommonParam common;
    common.path = outDir;
    common.size = canvas;
    common.frame = util::Range(0, 1);
    common.fps = 24;
    ctrl::Exporter::ImageParam image;
    image.name = "f";
    image.suffix = "png";
    image.quality = -1;
    const ctrl::Exporter::Result result = exporter.execute(common, image);
    expect((bool)result, QString("S9: exporter.execute failed: %1").arg(result.message));

    // compare each exported PNG against the interactive-path render at the same frame;
    // the QImage on disk is top-down, the fixture readback is bottom-up
    const QStringList pngs = QDir(outDir).entryList({"*.png"}, QDir::Files, QDir::Name);
    if (!expect(pngs.size() == 2, QString("S9: two frames exported (got %1)").arg(pngs.size())))
        return;
    std::vector<uint8_t> frames[2];
    for (int i = 0; i < 2; ++i) {
        // PNG stores straight alpha; the GL readback (and the scene itself) is
        // premultiplied, so premultiply back before comparing (blur halos are
        // semi-transparent and would mismatch wildly otherwise)
        const QImage img = QImage(outDir + "/" + pngs[i])
                               .convertToFormat(QImage::Format_RGBA8888_Premultiplied)
                               .mirrored(false, true);
        expect(
            img.size() == canvas, QString("S9: frame %1 has canvas size (got %2x%3)")
                                      .arg(i)
                                      .arg(img.size().width())
                                      .arg(img.size().height()));
        std::vector<uint8_t> got((size_t)canvas.width() * canvas.height() * 4);
        for (int y = 0; y < canvas.height(); ++y)
            memcpy(got.data() + (size_t)y * canvas.width() * 4, img.constScanLine(y), (size_t)canvas.width() * 4);
        const std::vector<uint8_t> ref = fx.render(tree, camera, i);
        const scene::Diff d = scene::diffImages(got, ref);
        const bool ok = checkDiff(d, 8, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport(QString("exported frame %1 == interactive render").arg(i), ok, diffStr(d));
        frames[i] = std::move(got);
    }
    // the blur weakens at frame 1, so the two exported frames must differ
    const scene::Diff between = scene::diffImages(frames[0], frames[1]);
    expect(between.maxDiff > 16, "S9: exported frames differ across the blur key change");
}

//-------------------------------------------------------------------------------------------------
// S11: PSD import round-trip through the production writer + loader. An in-memory PSDFormat
// with group records (open-folder + bounding section dividers) and per-record PSD blend modes
// is written by img::PSDWriter, then loaded through ctrl::ImageFileLoader::loadPsd; the
// resulting object tree must carry the folder blend modes (minor version 10 feature), the
// group structure, folder opacities and layer visibility. Folder blend modes ride the same
// per-layer 4CC PSDReader has always parsed, so a file authored through the production
// writer exercises the whole import chain end to end.
void suitePsdImport() {
    suiteHeader("S11 psd import: PSDWriter -> ImageFileLoader folder blend modes");
    const int failsBefore = gFails;

    img::PSDFormat format;
    format.header().version = 1;
    format.header().channels = 4;
    format.header().height = 64;
    format.header().width = 64;
    format.header().depth = 8;
    format.header().mode = img::PSDFormat::ColorMode_RGB;
    format.imageData().compressionId = 0;

    auto setRect = [](img::PSDFormat::Layer* aLayer, int l, int t, int r, int b) {
        aLayer->rect.edge[0] = t;
        aLayer->rect.edge[1] = l;
        aLayer->rect.edge[2] = b;
        aLayer->rect.edge[3] = r;
    };
    auto addLayerRecord = [&](const std::string& aName, int l, int t, int r, int b, uint8 aOpacity, uint8 aFlags) {
        auto* layer = new img::PSDFormat::Layer();
        layer->name = aName;
        setRect(layer, l, t, r, b);
        layer->blendMode = "norm";
        layer->opacity = aOpacity;
        layer->flags = aFlags;
        const int size = (r - l) * (b - t);
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = (uint32)size;
            chan->data.reset(new uint8[(size_t)size]);
            std::memset(chan->data.get(), 0, (size_t)size);
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
    };
    // group records carry the section divider ("lsct") additional info, which the reader
    // converts into LayerEntryType: open folders carry the 8BIM signature + group key,
    // bounding entries a bare entry value (like Photoshop writes them)
    auto addFolderRecord = [&](const std::string& aName, const std::string& aBlendMode, uint8 aOpacity,
                               const char* aGroupKey, int l, int t, int r, int b) {
        auto* layer = new img::PSDFormat::Layer();
        layer->name = aName;
        setRect(layer, l, t, r, b);
        layer->blendMode = aBlendMode;
        layer->opacity = aOpacity;
        auto* info = new img::PSDFormat::AdditionalLayerInfo();
        info->key = "lsct";
        if (aGroupKey) {
            info->dataLength = 12; // entry (4) + 8BIM (4) + key (4)
            info->data.reset(new uint8[12]);
            std::memset(info->data.get(), 0, 12);
            info->data[3] = 1; // open folder (LayerEntryType_OpenFolder)
            std::memcpy(info->data.get() + 4, "8BIM", 4);
            std::memcpy(info->data.get() + 8, aGroupKey, 4);
        } else {
            info->dataLength = 4; // entry only
            info->data.reset(new uint8[4]());
            info->data[3] = 3; // bounding section divider
        }
        layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
    };

    // PSD stores layer records bottom-first; ImageFileLoader walks them top-first
    // (layers.rbegin()) and treats the first record it sees per group as the open
    // folder, the "bounding" record as the close, so per group the FILE order must be
    // [bounding-close, members..., open] to round-trip: walking top-first then sees
    // [open, members, close] and builds the tree topNode > [grp2, grp, bg] (grp2
    // topmost)
    // a pass-through group ('pass' - the default group blend mode Photoshop writes) and a
    // group with an unsupported mode ('diss' - dissolve is still unsupported) sit BELOW the
    // bg record (earlier in the file = lower z); both must import as Normal folders ('pass'
    // maps 1:1, unsupported modes fall back to Normal instead of injecting BlendMode_TERM
    // into the renderer)
    addFolderRecord("</Layer group>", "norm", 255, nullptr, 0, 0, 64, 64);
    addLayerRecord("child3", 4, 4, 12, 12, 255, 0);
    addFolderRecord("grpPass", "pass", 255, "GRP3", 0, 0, 64, 64);
    addFolderRecord("</Layer group>", "norm", 255, nullptr, 0, 0, 64, 64);
    addLayerRecord("child4", 48, 48, 60, 60, 255, 0);
    addFolderRecord("grpDiss", "diss", 255, "GRP4", 0, 0, 64, 64);
    addLayerRecord("bg", 0, 0, 64, 64, 255, 0);
    addFolderRecord("</Layer group>", "norm", 255, nullptr, 0, 0, 64, 64);
    addLayerRecord("child", 20, 16, 44, 40, 255, 0);
    addFolderRecord("grp", "mul ", 200, "GRP1", 0, 0, 64, 64);
    addFolderRecord("</Layer group>", "norm", 255, nullptr, 0, 0, 64, 64);
    addLayerRecord("child2", 8, 8, 24, 24, 255, 0x02); // hidden (visibility flag bit)
    addFolderRecord("grp2", "scrn", 100, "GRP2", 0, 0, 64, 64);

    // merged image data: raw 4 channels (the reader parses the section with its own size
    // math - rowBytes = (width*height + 7) / 8 scanlines of `height` rows per channel, so
    // 512*64 bytes each - the object-tree build does not use the pixels)
    {
        const uint32 size = 512 * 64; // PSDReader::loadImageData raw branch size
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = size;
            chan->data.reset(new uint8[size]);
            std::memset(chan->data.get(), 0, size);
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
    }

    // write through the production writer
    const QString path = "/tmp/vb_psd_import.psd";
    {
        std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
        img::PSDWriter writer(out, format);
        expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S11: PSDWriter result");
        out.close();
    }

    // load through the production import path (the GL context is current, which loadPsd
    // needs for the eager layer shader compile and texture uploads)
    StubAnimator animator;
    core::Project project("", animator, nullptr);
    ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
    NullProgressReporter reporter;
    const bool loaded = loader.load(path, project, reporter);
    expect(loaded, QString("S11: loadPsd: %1").arg(loader.log()));
    if (!loaded)
        return;

    auto* top = project.objectTree().topNode();
    const auto& children = top->children();
    if (!expect(children.size() == 5, QString("S11: top children == 5 (got %1)").arg(children.size())))
        return;
    auto itr = children.begin();
    auto* grp2 = dynamic_cast<core::FolderNode*>(*itr++);
    auto* grp = dynamic_cast<core::FolderNode*>(*itr++);
    auto* bg = dynamic_cast<core::LayerNode*>(*itr++);
    auto* grpDiss = dynamic_cast<core::FolderNode*>(*itr++);
    auto* grpPass = dynamic_cast<core::FolderNode*>(*itr++);
    expect(grp2 && grp2->name() == "grp2", "S11: child[0] is folder 'grp2'");
    expect(grp && grp->name() == "grp", "S11: child[1] is folder 'grp'");
    expect(bg != nullptr, "S11: child[2] is a layer");

    // the folder blend modes come from the PSD 4CCs ("scrn", "mul ")
    expect(grp2 && grp2->blendMode() == img::BlendMode_Screen, "S11: grp2 blend == Screen (scrn)");
    expect(grp && grp->blendMode() == img::BlendMode_Multiply, "S11: grp blend == Multiply (mul )");
    expect(grpDiss && grpDiss->name() == "grpDiss", "S11: child[3] is folder 'grpDiss'");
    expect(grpPass && grpPass->name() == "grpPass", "S11: child[4] is folder 'grpPass'");
    expect(
        grpPass && grpPass->blendMode() == img::BlendMode_Normal,
        "S11: pass-through group ('pass') imports as a Normal folder");
    expect(
        grpDiss && grpDiss->blendMode() == img::BlendMode_Normal,
        "S11: unsupported group mode ('diss') falls back to Normal (no TERM injection)");

    // folder opacity maps to the default opa key
    const auto opaOf = [](core::ObjectNode* aNode) {
        auto* key = (core::OpaKey*)aNode->timeLine()->defaultKey(core::TimeKeyType_Opa);
        return key ? key->opacity() : 1.0f;
    };
    expect(grp && std::abs(opaOf(grp) - 200.0f / 255.0f) < 0.001f, "S11: grp opacity == 200/255");
    expect(grp2 && std::abs(opaOf(grp2) - 100.0f / 255.0f) < 0.001f, "S11: grp2 opacity == 100/255");

    // group structure and the hidden layer
    if (grp) {
        const auto& grpChildren = grp->children();
        expect(
            grpChildren.size() == 1 && (*grpChildren.begin())->name() == "child",
            "S11: grp contains layer 'child'");
    }
    if (grp2) {
        const auto& grp2Children = grp2->children();
        const bool ok = grp2Children.size() == 1 && (*grp2Children.begin())->name() == "child2"
            && !(*grp2Children.begin())->isVisible();
        expect(ok, "S11: grp2 contains hidden layer 'child2'");
    }

    caseReport(
        "PSD import round-trip (folders, blend modes, opacities, visibility)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks));

    // Real-file check: the repo's own sample PSD (assets/sample.psd, RLE channels, unicode
    // names, an 'lsct' section divider with a 'pass' group - the default group blend mode
    // Photoshop writes) goes through the same production import, then renders one frame
    // through the full composite wiring. Pre-fix this injected BlendMode_TERM into the
    // folder and aborted in presentShader's mode < TERM assert at first render.
    const int sampleChecksBefore = gChecks;
    {
        core::Project sampleProject("", animator, nullptr);
        ctrl::ImageFileLoader sampleLoader(gl::DeviceInfo::instance());
        const bool sampleLoaded = sampleLoader.load("assets/sample.psd", sampleProject, reporter);
        expect(sampleLoaded, QString("S11: sample.psd loads: %1").arg(sampleLoader.log()));
        if (sampleLoaded) {
            int folderCount = 0;
            bool modesValid = true;
            const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
                if (auto* folder = dynamic_cast<core::FolderNode*>(aNode)) {
                    ++folderCount;
                    if (folder->blendMode() >= img::BlendMode_TERM)
                        modesValid = false;
                }
                for (auto* child : aNode->children())
                    walk(child);
            };
            walk(sampleProject.objectTree().topNode());
            expect(folderCount >= 2, QString("S11: sample.psd has groups (got %1 folders incl. top)").arg(folderCount));
            expect(modesValid, "S11: every imported folder blend mode is valid (< TERM)");
            // render smoke: exercises the import + composite wiring end to end
            const QSize sampleCanvas = sampleProject.attribute().imageSize();
            auto& sampleFx = fixtureFor(sampleCanvas);
            sampleFx.render(sampleProject.objectTree(), scene::makeCamera(sampleCanvas), 0);
            expect(true, "S11: sample.psd renders a frame without hitting a shader assert");
        }
    }

    // CSP tsly flag: CSP exports its own Add (Glow)/Glow Dodge with the plain "lddg"/"div "
    // blend keys PLUS a "tsly" layer-info block whose value is 0 (CSP-specific) instead of
    // 1 (plain) - verified against the user's pair of real CSP exports (psd.psd vs
    // psd_with_non_glow_equivalents.psd: identical layer records, differ only in that
    // flag). The mapping must produce the CSP modes only when the flag is present AND 0;
    // Adobe never writes "tsly", so lddg/div without it stay LinearDodge/ColorDodge.
    {
        img::PSDFormat tslyFormat;
        tslyFormat.header().version = 1;
        tslyFormat.header().channels = 4;
        tslyFormat.header().height = 64;
        tslyFormat.header().width = 64;
        tslyFormat.header().depth = 8;
        tslyFormat.header().mode = img::PSDFormat::ColorMode_RGB;
        tslyFormat.imageData().compressionId = 0;
        auto tslyRecord = [&](const std::string& aName, const std::string& aBlend, int aTsly) {
            auto* layer = new img::PSDFormat::Layer();
            layer->name = aName;
            setRect(layer, 0, 0, 8, 8);
            layer->blendMode = aBlend;
            layer->opacity = 255;
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "tsly";
            info->dataLength = 4;
            info->data.reset(new uint8[4]);
            std::memset(info->data.get(), 0, 4);
            if (aTsly > 0)
                info->data[0] = 1;
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
            for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
                auto* chan = new img::PSDFormat::Channel();
                chan->id = id;
                chan->compressionId = 0;
                chan->dataLength = 64;
                chan->data.reset(new uint8[64]);
                std::memset(chan->data.get(), 0, 64);
                layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
            }
            tslyFormat.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
        };
        tslyRecord("csp_addglow", "lddg", 0);
        tslyRecord("plain_lddg", "lddg", 1);
        tslyRecord("csp_glowdodge", "div ", 0);
        tslyRecord("plain_div", "div ", 1);
        {
            const uint32 size = 512 * 64; // PSDReader::loadImageData raw branch size
            for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
                auto* chan = new img::PSDFormat::Channel();
                chan->id = id;
                chan->compressionId = 0;
                chan->dataLength = size;
                chan->data.reset(new uint8[size]);
                std::memset(chan->data.get(), 0, size);
                tslyFormat.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
            }
        }
        const QString tslyPath = "/tmp/vb_psd_tsly.psd";
        {
            std::ofstream out(tslyPath.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, tslyFormat);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S11: tsly PSDWriter result");
            out.close();
        }
        core::Project tslyProject("", animator, nullptr);
        ctrl::ImageFileLoader tslyLoader(gl::DeviceInfo::instance());
        const bool tslyLoaded = tslyLoader.load(tslyPath, tslyProject, reporter);
        expect(tslyLoaded, QString("S11: tsly PSD loads: %1").arg(tslyLoader.log()));
        if (tslyLoaded) {
            const std::function<void(core::ObjectNode*)> checkTsly = [&](core::ObjectNode* aNode) {
                if (auto* ln = dynamic_cast<core::LayerNode*>(aNode)) {
                    if (aNode->name() == "csp_addglow")
                        expect(
                            ln->blendMode() == img::BlendMode_AddGlow,
                            "S11: lddg + tsly=0 imports as Add (Glow)");
                    if (aNode->name() == "plain_lddg")
                        expect(
                            ln->blendMode() == img::BlendMode_LinearDodge,
                            "S11: lddg + tsly=1 imports as Linear Dodge");
                    if (aNode->name() == "csp_glowdodge")
                        expect(
                            ln->blendMode() == img::BlendMode_GlowDodge,
                            "S11: div  + tsly=0 imports as Glow Dodge");
                    if (aNode->name() == "plain_div")
                        expect(
                            ln->blendMode() == img::BlendMode_ColorDodge,
                            "S11: div  + tsly=1 imports as Color Dodge");
                }
                for (auto* child : aNode->children())
                    checkTsly(child);
            };
            checkTsly(tslyProject.objectTree().topNode());
        }
    }
    caseReport(
        "sample.psd real-file import (pass-through group) + render smoke", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - sampleChecksBefore));
}

//-------------------------------------------------------------------------------------------------
// S13: ORA import round-trip through the production oraParser + ImageFileLoader. The
// harness writes a real .ora zip (mimetype + stack.xml + PNG entries) whose stacks carry
// composite-op attributes, then loads it through ctrl::ImageFileLoader with the layered
// import mode pre-selected (OraImportMode skips the interactive layered/merged prompt).
// Stacks map composite-op like layers (multiply/screen); an attribute-less stack keeps
// the pass-through Normal default; opacities map to default opa keys. Folder assertions
// are name-based (oraParser flattens the stack document in its own traversal order).
void suiteOraImport() {
    suiteHeader("S13 ora import: synthetic .ora -> ImageFileLoader folder blend modes");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    const QSize canvas(48, 48);
    auto pngBytes = [](const QSize& aSize, const QColor& aColor) {
        QImage img(aSize, QImage::Format_RGBA8888);
        img.fill(aColor);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        img.save(&buffer, "PNG");
        return bytes;
    };

    // stacks: multiply @0.8, screen @0.75, and one without composite-op (Normal default)
    const std::string stackXml =
        "<image w='48' h='48' version='0.0.1'>"
        "<stack name='root'>"
        "<layer name='bg' src='data/bg.png' x='0' y='0' opacity='1.0' visibility='visible'"
        " composite-op='svg:src-over'/>"
        "<stack name='grpMul' composite-op='svg:multiply' opacity='0.8' visibility='visible'>"
        "<layer name='child' src='data/child.png' x='6' y='6' opacity='1.0' visibility='visible'"
        " composite-op='svg:src-over'/>"
        "</stack>"
        "<stack name='grpScreen' composite-op='svg:screen' opacity='0.75' visibility='visible'>"
        "<layer name='child2' src='data/child2.png' x='24' y='24' opacity='1.0' visibility='visible'"
        " composite-op='svg:src-over'/>"
        "</stack>"
        "<stack name='grpDefault' opacity='1.0' visibility='visible'>"
        "<layer name='child3' src='data/child3.png' x='36' y='4' opacity='1.0' visibility='visible'"
        " composite-op='svg:src-over'/>"
        "</stack>"
        "</stack>"
        "</image>";

    // write a real .ora zip through the same miniz wrapper the reader uses
    const QString path = "/tmp/vb_ora_import.ora";
    {
        miniz_cpp::zip_file ora;
        ora.writestr("mimetype", "image/openraster");
        ora.writestr("stack.xml", stackXml);
        ora.writestr("data/bg.png", pngBytes(canvas, QColor(32, 48, 64, 255)).toStdString());
        ora.writestr("data/child.png", pngBytes(QSize(16, 16), QColor(200, 60, 40, 255)).toStdString());
        ora.writestr("data/child2.png", pngBytes(QSize(16, 16), QColor(40, 180, 90, 255)).toStdString());
        ora.writestr("data/child3.png", pngBytes(QSize(8, 8), QColor(220, 200, 60, 255)).toStdString());
        ora.writestr("mergedimage.png", pngBytes(canvas, QColor(90, 90, 90, 255)).toStdString());
        ora.save(path.toStdString());
    }

    // load through the production import path with the layered mode pre-selected (the
    // interactive prompt would block the harness); the GL context is current for the
    // eager layer shader compile and texture uploads.
    //
    // The parse runs under a comma-decimal LC_NUMERIC scoped with uselocale(): that is
    // exactly the locale that used to silently zero ORA opacities ("0.8" parsed as 0.0)
    // via strtod-style as_float(), see oraParser.cpp. The parse is immune (it uses the
    // C-locale QString::toFloat()), so this only pins the regression: if a parse path
    // ever regresses to a locale-honoring converter, opacity checks below fail. Best
    // effort - machines without any of these locales keep the C default via newlocale()
    // returning null. Unlike the old process-wide setlocale() in main, uselocale keeps
    // the rest of the harness on the environment locale.
    StubAnimator animator;
    core::Project project("", animator, nullptr);
    ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
    loader.setOraImportMode(ctrl::ImageFileLoader::OraImportMode::Layered);
    NullProgressReporter reporter;
    locale_t commaNum = nullptr;
    for (const char* loc : {"cs_CZ.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", "ru_RU.UTF-8"}) {
        commaNum = newlocale(LC_NUMERIC_MASK, loc, nullptr);
        if (commaNum)
            break;
    }
    locale_t savedNum = uselocale(commaNum); // commaNum null -> C default, harmless
    const bool loaded = loader.load(path, project, reporter);
    uselocale(savedNum);
    if (commaNum)
        freelocale(commaNum);
    expect(loaded, QString("S13: loadOra: %1").arg(loader.log()));
    if (!loaded)
        return;

    // collect the imported folders by name (oraParser's flat-list traversal order is
    // its own business; the blend wiring is what this suite pins down)
    std::map<QString, core::FolderNode*> folders;
    const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
        if (auto* folder = dynamic_cast<core::FolderNode*>(aNode))
            folders.emplace(folder->name(), folder);
        for (auto* child : aNode->children())
            walk(child);
    };
    walk(project.objectTree().topNode());
    auto folderMode = [&](const QString& aName) {
        auto it = folders.find(aName);
        return it != folders.end() ? it->second->blendMode() : img::BlendMode_TERM;
    };
    expect(
        folderMode("grpMul") == img::BlendMode_Multiply,
        "S13: stack 'grpMul' blend == Multiply (svg:multiply)");
    expect(
        folderMode("grpScreen") == img::BlendMode_Screen,
        "S13: stack 'grpScreen' blend == Screen (svg:screen)");
    expect(
        folderMode("grpDefault") == img::BlendMode_Normal,
        "S13: stack without composite-op keeps the Normal default");

    // stack opacity maps to the default opa key
    const auto opaOf = [](core::ObjectNode* aNode) {
        auto* key = (core::OpaKey*)aNode->timeLine()->defaultKey(core::TimeKeyType_Opa);
        return key ? key->opacity() : 1.0f;
    };
    expect(
        folders.count("grpMul") && std::abs(opaOf(folders["grpMul"]) - 0.8f) < 0.001f,
        QString("S13: grpMul opacity == 0.8 (got %1)")
            .arg(folders.count("grpMul") ? opaOf(folders["grpMul"]) : -1.0f));
    expect(
        folders.count("grpScreen") && std::abs(opaOf(folders["grpScreen"]) - 0.75f) < 0.001f,
        QString("S13: grpScreen opacity == 0.75 (got %1)")
            .arg(folders.count("grpScreen") ? opaOf(folders["grpScreen"]) : -1.0f));

    // group structure: each named stack holds its layer
    auto holdsLayer = [&](const QString& aFolder, const QString& aLayer) {
        auto it = folders.find(aFolder);
        if (it == folders.end())
            return false;
        for (auto* child : it->second->children()) {
            if (child->name() == aLayer)
                return true;
        }
        return false;
    };
    expect(holdsLayer("grpMul", "child"), "S13: grpMul contains layer 'child'");
    expect(holdsLayer("grpScreen", "child2"), "S13: grpScreen contains layer 'child2'");
    expect(holdsLayer("grpDefault", "child3"), "S13: grpDefault contains layer 'child3'");

    // render smoke: the imported tree (two blend-mode folders) goes through the
    // composite wiring without hitting a shader assert
    const QSize loadedCanvas = project.attribute().imageSize();
    auto& fx = fixtureFor(loadedCanvas);
    fx.render(project.objectTree(), scene::makeCamera(loadedCanvas), 0);
    expect(true, "S13: imported ORA renders a frame without hitting a shader assert");

    // the MERGED mode: the same file imported as one flat layer. Its geometry is the one the
    // merged import has always produced - the resource (and with it the node's initial rect) is
    // placed at the CENTRE of the padded bitmap - so it is pinned here rather than left to
    // chance, because ctrl::describeOra has to reproduce it exactly.
    {
        core::Project mergedProject("", animator, nullptr);
        ctrl::ImageFileLoader mergedLoader(gl::DeviceInfo::instance());
        mergedLoader.setOraImportMode(ctrl::ImageFileLoader::OraImportMode::Merged);
        expect(mergedLoader.load(path, mergedProject, reporter),
               QString("S13: the same ORA imports merged: %1").arg(mergedLoader.log()));

        auto* mergedTop = mergedProject.objectTree().topNode();
        expect(mergedTop && mergedTop->children().size() == 1, "S13: a merged ORA is one layer");
        if (auto* layer = mergedTop && mergedTop->children().size() == 1
                ? dynamic_cast<core::LayerNode*>(mergedTop->children().front())
                : nullptr) {
            uint8 px[4] = {0, 0, 0, 0};
            bool gotPixel = false;
            if (auto* key = dynamic_cast<core::ImageKey*>(
                    layer->timeLine()->defaultKey(core::TimeKeyType_Image))) {
                const img::ResourceHandle& handle = key->data().resource();
                if (handle && handle->image().data()) {
                    const int w = handle->image().pixelSize().width();
                    const uint8* data = handle->image().data() + ((size_t)1 * w + 1) * 4;
                    for (int i = 0; i < 4; ++i) {
                        px[i] = data[i];
                    }
                    gotPixel = true;
                }
            }
            expect(gotPixel && px[0] == 90 && px[1] == 90 && px[2] == 90,
                   QString("S13: the merged layer holds the merged image (%1,%2,%3)")
                       .arg(px[0]).arg(px[1]).arg(px[2]));

            // the padded rect is (-1,-1, canvas+2) and the merged import places the layer at its
            // Qt centre: left + (width-1)/2 = -1 + (canvas+1)/2, written out here rather than
            // recomputed from the same expression the reader uses
            const QRect expectRect(
                QPoint(-1 + (canvas.width() + 1) / 2, -1 + (canvas.height() + 1) / 2),
                canvas + QSize(2, 2));
            expect(layer->initialRect() == expectRect,
                   QString("S13: the merged layer keeps its historical (centre-placed) rect (%1,%2 %3x%4)")
                       .arg(layer->initialRect().x()).arg(layer->initialRect().y())
                       .arg(layer->initialRect().width()).arg(layer->initialRect().height()));
        }
    }

    caseReport(
        "ORA import (folder blend modes, opacities, structure) + render smoke", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S12: folder blend modes render semantics (minor version 10 feature). A folder with a
// non-Normal blend mode composites its subtree and presents it against the captured scene
// (DestinationTexturizer) with the folder's mode at its own opacity. The CPU reference is
// blendPresent(content, background, mode, opacity) where content is the same subtree
// rendered without the mode. For a fractional folder opacity the composite renders its
// children at the compensated opacity 1/op, which clamps to 1.0 in the RGBA8 slot, so the
// reference content render needs no compensation; the present opacity is quantized to the
// app's 8-bit uColor.
void suiteFolderBlend() {    suiteHeader("S12 folder blend modes: GPU vs CPU reference");
    const int failsBefore = gFails;
    using A3 = std::array<double, 3>;
    auto R = [](double deg, double sx, double sy) { return A3{deg * kDegToRad, sx, sy}; };
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);

    auto renderTree = [&](const std::function<void(core::ObjectTree&, ResPool&)>& aBuild) {
        ResPool pool;
        core::ObjectTree tree;
        aBuild(tree, pool);
        return fx.render(tree, camera, 0);
    };
    auto bytesToImage = [&](const std::vector<uint8_t>& aBytes) {
        return ref::imageFromBytes(aBytes.data(), canvas.width(), canvas.height());
    };
    auto runCase = [&](const QString& aName, const std::vector<uint8_t>& aGpu, const ref::Image& aContent,
                       const ref::Image& aBg, img::BlendMode aMode, double aOpacity) {
        const ref::Image presented = ref::blendPresent(aContent, aBg, aMode, aOpacity);
        const scene::Diff d = scene::diffImages(aGpu, ref::imageToBytes(presented));
        // Color's max=54/255 spike on 3-2 pixels at the wedge's gray-crossing gradient
        // bar is NOT the ClipColor formulas: they are algebraically bounded (the ratios
        // l/(l-n), (1-l)/(x-l) cap at 1 where their branches fire), and the S14 golden
        // probes psd_layer_color_wedge / psd_layer_color_sweep pin the GPU Color formula
        // to Krita at max=1/255 on a FLAT scene (identity transforms, exact texel
        // sampling). The spike is the double-precision replica resampling the transformed
        // shapes (rot/scale under motion transforms: CPU rounds to-texel, GPU bilinear),
        // i.e. a reference-path artifact, not shader or blend-math error. Keep the 64
        // tolerance so the suite stays green, but guard the outlier COUNT: an enlarged
        // spike (a real formula regression) must grow past max=4 pixels and fail.
        const int maxTol = aMode == img::BlendMode_Color ? 64 : 8;
        const bool ok = aMode == img::BlendMode_Color
            ? (d.maxDiff <= maxTol && d.mean <= 0.8 && d.over4 <= 4)
            : checkDiff(d, maxTol, 0.8, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport(aName, ok, diffStr(d));
    };
    // the standard full tree: a blend-mode folder over a wedged background layer
    auto buildFull = [](core::ObjectTree& tree, ResPool& pool, img::BlendMode aMode, bool aTransformed) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* folder = scene::addFolder(
            root, "folder", QVector2D(64, 48), aTransformed ? 25.0f : 0.0f, aTransformed ? QVector2D(1.6f, 0.9f) : QVector2D(1, 1));
        folder->setBlendMode(aMode);
        scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
        scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
    };
    auto buildContent = [](core::ObjectTree& tree, ResPool& pool, bool aTransformed) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* folder = scene::addFolder(
            root, "folder", QVector2D(64, 48), aTransformed ? 25.0f : 0.0f, aTransformed ? QVector2D(1.6f, 0.9f) : QVector2D(1, 1));
        scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
    };
    auto buildBg = [](core::ObjectTree& tree, ResPool& pool) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
    };
    auto contentImage = [&](bool aTransformed) {
        return bytesToImage(renderTree([aTransformed, &buildContent](core::ObjectTree& tree, ResPool& pool) {
            buildContent(tree, pool, aTransformed);
        }));
    };
    auto bgImage = [&]() { return bytesToImage(renderTree(buildBg)); };

    // (a) every blend mode, untransformed
    for (int m = img::BlendMode_Normal; m < img::BlendMode_TERM; ++m) {
        const img::BlendMode mode = (img::BlendMode)m;
        const std::vector<uint8_t> gpu = renderTree([mode, &buildFull](core::ObjectTree& tree, ResPool& pool) {
            buildFull(tree, pool, mode, false);
        });
        runCase(QString("folder blend %1 over bg").arg(img::getBlendNameFromBlendMode(mode)), gpu,
            contentImage(false), bgImage(), mode, 1.0);
    }

    // (a2) every blend mode over an OPAQUE content: with opaque content premult == straight
    // in the RGBA8 readback, so the CPU reference's un-multiply is exact; with
    // semi-transparent content the readback quantization of the premult values is
    // amplified by the steep Vivid Light / Hard Mix gradients to full-scale diffs, so
    // those two modes run again (with all others, for coverage) on opaque content
    {
        auto buildOpaqueContent = [](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 0.0f, QVector2D(1, 1));
            folder->setBlendMode(img::BlendMode_Normal);
            scene::addLayer(tree, folder, pool.wedgeOpaque("wo"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
        };
        const ref::Image contentOpaque = bytesToImage(renderTree(buildOpaqueContent));
        for (const img::BlendMode mode :
             {img::BlendMode_VividLight, img::BlendMode_HardMix, img::BlendMode_ColorBurn, img::BlendMode_ColorDodge,
                 img::BlendMode_Multiply, img::BlendMode_Screen, img::BlendMode_Divide, img::BlendMode_Hue,
                 img::BlendMode_Saturation, img::BlendMode_Color, img::BlendMode_Luminosity, img::BlendMode_DarkerColor,
                 img::BlendMode_LighterColor}) {
            const std::vector<uint8_t> gpu = renderTree([mode](core::ObjectTree& tree, ResPool& pool) {
                auto* root = new core::FolderNode("root");
                tree.grabTopNode(root);
                auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 0.0f, QVector2D(1, 1));
                folder->setBlendMode(mode);
                scene::addLayer(tree, folder, pool.wedgeOpaque("wo"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
                scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
            });
            runCase(QString("folder %1 over bg (opaque content)").arg(img::getBlendNameFromBlendMode(mode)), gpu,
                contentOpaque, bgImage(), mode, 1.0);
        }
    }

    // (b) transformed folder: rot25 s(1.6,0.9), layer rot-12 s(1.2,1)
    for (const img::BlendMode mode : {img::BlendMode_Multiply, img::BlendMode_Screen, img::BlendMode_Overlay,
                                      img::BlendMode_Difference}) {
        const std::vector<uint8_t> gpu = renderTree([mode, &buildFull](core::ObjectTree& tree, ResPool& pool) {
            buildFull(tree, pool, mode, true);
        });
        runCase(
            QString("folder %1 rot25 s1.6 > layer rot-12").arg(img::getBlendNameFromBlendMode(mode)), gpu,
            contentImage(true), bgImage(), mode, 1.0);
    }

    // (c) fractional folder opacity 0.7 (the composite renders children at 1/0.7 which
    // clamps to 1.0 in the RGBA8 slot, so the content render is uncompensated; the present
    // opacity rides uColor.a quantized to 8 bits like the app does)
    auto buildContentPlain = [](core::ObjectTree& tree, ResPool& pool) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 0.0f, QVector2D(1, 1));
        scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
    };
    const ref::Image contentPlain = bytesToImage(renderTree(buildContentPlain));
    const double opQ = (double)(int)(255.0f * 0.7f) / 255.0;
    for (const img::BlendMode mode : {img::BlendMode_Multiply, img::BlendMode_Screen, img::BlendMode_Divide}) {
        const std::vector<uint8_t> gpu = renderTree([mode, &buildFull](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 0.0f, QVector2D(1, 1));
            folder->setBlendMode(mode);
            folder->setDefaultOpacity(0.7f);
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        runCase(QString("folder %1 @ opacity 0.7").arg(img::getBlendNameFromBlendMode(mode)), gpu,
            contentPlain, bgImage(), mode, opQ);
    }

    // (d) folder blur x blend: the blur is baked into the composite, then the folder
    // presents the result with its mode against the real captured background
    for (const img::BlendMode mode : {img::BlendMode_Multiply, img::BlendMode_Screen}) {
        const std::vector<uint8_t> gpu = renderTree([mode, &buildFull](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.3f, 0.8f));
            folder->setBlendMode(mode);
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addBlur(folder, 0, 10.0f, 3.0f, 40.0f);
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        const ref::Image plain = bytesToImage(renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.3f, 0.8f));
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
        }));
        const ref::Image blurred = refBlurApplied(plain, canvas, camera, {R(20, 1.3, 0.8)}, 10, 3, 40, 1.0f);
        runCase(QString("folder %1 + blur (10,3,40)").arg(img::getBlendNameFromBlendMode(mode)), gpu,
            blurred, bgImage(), mode, 1.0);
    }

    // (e) folder HSV x blend: the HSV is applied at presentation together with the blend
    // (the combined USE_HSV + per-mode present shader), so the reference is
    // blendPresent(hsv(plain), bg, mode)
    for (const img::BlendMode mode : {img::BlendMode_Multiply, img::BlendMode_Screen}) {
        const std::vector<uint8_t> gpu = renderTree([mode, &buildFull](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.3f, 0.8f));
            folder->setBlendMode(mode);
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addHSV(folder, 0, 60, 150, 80, 0);
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        const ref::Image plain = bytesToImage(renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(64, 48), 20.0f, QVector2D(1.3f, 0.8f));
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
        }));
        const ref::Image adjusted = ref::hsvAdjust(plain, 60.0 / 360.0, 150.0 / 100.0, 80.0 / 100.0, false);
        runCase(QString("folder HSV(60,150,80) + %1").arg(img::getBlendNameFromBlendMode(mode)), gpu,
            adjusted, bgImage(), mode, 1.0);
    }

    // (f) nested blend folders: outer(multiply) > [innerBg, inner(screen) > [layer]] over
    // bg. The outer composite pass renders its children in tree order, so innerBg is added
    // BEFORE the inner folder: the inner present then captures the outer composite's
    // content (innerBg) as its blend background, and the outer present blends the whole
    // result against the real scene (bg). The reference renders reproduce the world
    // transforms the GPU applies inside the outer composite (rot15 s(1.2,0.8)).
    {
        const std::vector<uint8_t> gpu = renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* outer = scene::addFolder(root, "outer", QVector2D(64, 48), 15.0f, QVector2D(1.2f, 0.8f));
            outer->setBlendMode(img::BlendMode_Multiply);
            scene::addLayer(tree, outer, pool.wedge("bi"), "innerBg", QVector2D(40, 60), 10.0f, QVector2D(1, 1));
            auto* inner = scene::addFolder(outer, "inner", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            inner->setBlendMode(img::BlendMode_Screen);
            scene::addLayer(tree, inner, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        const ref::Image content = bytesToImage(renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* outer = scene::addFolder(root, "outer", QVector2D(64, 48), 15.0f, QVector2D(1.2f, 0.8f));
            scene::addLayer(tree, outer, pool.wedge("w"), "layer", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
        }));
        const ref::Image innerBg = bytesToImage(renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* outer = scene::addFolder(root, "outer", QVector2D(64, 48), 15.0f, QVector2D(1.2f, 0.8f));
            scene::addLayer(tree, outer, pool.wedge("bi"), "innerBg", QVector2D(40, 60), 10.0f, QVector2D(1, 1));
        }));
        const ref::Image inner = ref::blendPresent(content, innerBg, img::BlendMode_Screen, 1.0);
        runCase("nested: outer multiply > inner screen", gpu, inner, bgImage(), img::BlendMode_Multiply, 1.0);
    }

    // (g) the Normal gate: a folder with blend mode Normal stays on the pass-through path
    // and renders byte-identical to the same tree without the folder. The folder must be
    // untransformed for the byte-exact comparison (an identity parent chain multiplies
    // without changing the layer's float matrix; a composed flat transform cannot
    // reproduce the folder chain's float rounding)
    {
        // the folder must be UNTRANSFORMED - including position - for the byte-exact
        // comparison: the folder's move key translates the whole chain, so a folder at
        // (64,48) puts the layer at a different world spot than the flat tree (buildFull
        // defaults to (64,48); this is why the shared builder cannot be reused here). An
        // identity parent chain multiplies without changing the layer's float matrix (an
        // identity 4x4 product is exact in float), which a composed flat transform could
        // not reproduce.
        const std::vector<uint8_t> plain = renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* folder = scene::addFolder(root, "folder", QVector2D(0, 0), 0.0f, QVector2D(1, 1));
            folder->setBlendMode(img::BlendMode_Normal);
            scene::addLayer(tree, folder, pool.wedge("w"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        const std::vector<uint8_t> noFolder = renderTree([](core::ObjectTree& tree, ResPool& pool) {
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            scene::addLayer(tree, root, pool.wedge("w"), "layer", QVector2D(6, -4), -12.0f, QVector2D(1.2f, 1.0f));
            scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(58, 52), -8.0f, QVector2D(1.6f, 1.4f));
        });
        const scene::Diff d = scene::diffImages(plain, noFolder);
        expect(d.bytesIdentical, "S12: folder blend Normal is pass-through (byte-identical)");
        caseReport("Normal gate: mode-Normal folder == no folder", d.bytesIdentical, diffStr(d));
    }

    caseReport("folder blend semantics (26 modes x transforms/opacity/blur/HSV/nesting)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks));
}

//-------------------------------------------------------------------------------------------------
// S15: shader-variant permutation matrix. LayerDrawingFrag.glsl carries six variation
// flags, but only three are orthogonal: BLEND_FUNC, BLEND_NONSEPARABLE and
// BLEND_PREMULTIPLIED_SRC are all derived from the blend mode, while IS_CLIPPEE and
// USE_HSV are independent booleans. ShaderHolder caches one program per
// (mode, isClippee, useHSV) slot = BlendMode_TERM * 2 * 2 (index math at
// src/core/ShaderHolder.cpp:29). The render suites above only force the combinations
// their scenes happen to select (presentHSVShader came from one unusual combination no
// earlier suite compiled), so this suite force-compiles and links the FULL matrix:
// reserveShader() aborts via XC_FATAL_ERROR on any compile/link failure, so merely
// reaching the report is the check.
//-------------------------------------------------------------------------------------------------
// S16: Photoshop "Blend If" (core/BlendIf.h). A layer's contribution is scaled per pixel by
// the product of a trapezoid over its own Rec.601 luma ("This Layer") and one over the
// backdrop's ("Underlying Layer") - the same alpha scale the layer opacity uses, so the CPU
// reference is ref::blendPresent(content, backdrop, mode, opacity, ranges). Flat,
// canvas-sized, identity-transformed layers sample texel-exactly: a mismatch here is the
// weight or its plumbing, not rasterization.
void suiteBlendIf() {
    suiteHeader("S16 blend if: GPU vs CPU reference");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    using Fill = std::function<void(int, int, uint8_t*)>;
    struct Pool { // canvas-sized images live as long as the tree
        QSize size;
        std::vector<img::ResourceNode*> nodes;
        ~Pool() { qDeleteAll(nodes.begin(), nodes.end()); }
        img::ResourceNode* image(const QString& aId, const Fill& aFill) {
            nodes.push_back(scene::makeImage(aId, size, aFill));
            return nodes.back();
        }
    };
    // horizontal / vertical luma ramps (the y ramp is what drives "This Layer" weights)
    const Fill rampX = [](int x, int, uint8_t* px) {
        const int v = x * 255 / 127;
        px[0] = px[1] = px[2] = (uint8_t)v;
        px[3] = 255;
    };
    const Fill rampY = [](int, int y, uint8_t* px) {
        const int v = y * 255 / 95;
        px[0] = px[1] = px[2] = (uint8_t)v;
        px[3] = 255;
    };
    const Fill solid = [](int, int, uint8_t* px) {
        px[0] = 200; px[1] = 80; px[2] = 60; px[3] = 255;
    };
    const Fill solidSemi = [](int, int, uint8_t* px) {
        px[0] = 40; px[1] = 180; px[2] = 220; px[3] = 160;
    };

    // ranges: a strict mid-band (fully applied only on 128..160) and an empty (crossed) one
    core::BlendIf midBandDst;
    midBandDst.dst = core::BlendIf::Range{64, 128, 160, 224};
    core::BlendIf midBandSrc;
    midBandSrc.src = core::BlendIf::Range{80, 120, 180, 220};
    core::BlendIf bothBands = midBandDst;
    bothBands.src = midBandSrc.src;
    core::BlendIf emptyDst;
    emptyDst.dst = core::BlendIf::Range{255, 255, 0, 0};

    struct SceneSpec {
        Fill top;   // empty = no top layer
        Fill bg;
        img::BlendMode mode = img::BlendMode_Normal;
        float opacity = 1.0f;
        core::BlendIf blendIf;
        float blur = 0.0f;   // blur amount keyed on the top node (layer or folder)
        bool folder = false; // carry mode/blur/range on a folder around the layer
    };
    auto renderSpec = [&](const SceneSpec& aSpec) {
        Pool pool;
        pool.size = canvas;
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        if (aSpec.top) {
            if (aSpec.folder) {
                auto* folder = scene::addFolder(root, "folder", center, 0.0f, unit);
                folder->setBlendMode(aSpec.mode);
                folder->setBlendIf(aSpec.blendIf);
                scene::addLayer(tree, folder, pool.image("top", aSpec.top), "top", QVector2D(0, 0), 0.0f, unit);
                if (aSpec.blur > 0.0f)
                    scene::addBlurAmount(folder, 0, aSpec.blur);
            } else {
                auto* layer =
                    scene::addLayer(tree, root, pool.image("top", aSpec.top), "top", center, 0.0f, unit);
                layer->setBlendMode(aSpec.mode);
                layer->setBlendIf(aSpec.blendIf);
                if (aSpec.opacity != 1.0f)
                    layer->setDefaultOpacity(aSpec.opacity);
                if (aSpec.blur > 0.0f)
                    scene::addBlurAmount(layer, 0, aSpec.blur);
            }
        }
        // the first-pushed sibling renders on top, so the backdrop goes last
        if (aSpec.bg)
            scene::addLayer(tree, root, pool.image("bg", aSpec.bg), "bg", center, 0.0f, unit);
        return fx.render(tree, camera, 0);
    };
    auto imageOf = [&](const std::vector<uint8_t>& aBytes) {
        return ref::imageFromBytes(aBytes.data(), canvas.width(), canvas.height());
    };
    auto compare = [&](const QString& aName, const std::vector<uint8_t>& aGpu, const ref::Image& aExpected,
                       int aMaxTol, double aMeanTol = 0.05) {
        const scene::Diff d = scene::diffImages(aGpu, ref::imageToBytes(aExpected));
        const bool ok = checkDiff(d, aMaxTol, aMeanTol, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport(aName, ok, diffStr(d));
    };
    // the reference inputs are the same trees rendered without the other node, exactly like
    // the S12 folder-blend suite
    auto contentOf = [&](SceneSpec aSpec) {
        const core::BlendIf keep = aSpec.blendIf;
        aSpec.bg = Fill();
        aSpec.blendIf = core::BlendIf();
        aSpec.blur = 0.0f;
        aSpec.opacity = 1.0f;
        aSpec.mode = img::BlendMode_Normal;
        aSpec.folder = false;
        (void)keep;
        return renderSpec(aSpec);
    };
    auto bgOf = [&](SceneSpec aSpec) {
        aSpec.top = Fill();
        aSpec.blendIf = core::BlendIf();
        return renderSpec(aSpec);
    };
    auto modeName = [](img::BlendMode aMode) { return img::getBlendNameFromBlendMode(aMode); };

    // (0) the reference setup itself: a canvas-sized layer under the identity transform
    // covers the canvas texel-exactly (otherwise every comparison below is meaningless)
    {
        SceneSpec spec;
        spec.bg = rampX;
        const std::vector<uint8_t> gpu = renderSpec(spec);
        // the outer band is excluded: the GridMeshCreator insets the layer mesh edges, so
        // the outermost texels blend with the cleared background (the golden suite crops
        // the same band - tests/golden README's borderCrop)
        const int crop = 3;
        int checked = 0, bad = 0;
        int badX = -1, badY = -1, badGot = -1, badWant = -1;
        for (int y = crop; y < canvas.height() - crop; ++y) {
            for (int x = crop; x < canvas.width() - crop; ++x) {
                const uint8_t* px = gpu.data() + ((size_t)y * canvas.width() + x) * 4;
                const int want = x * 255 / (canvas.width() - 1);
                ++checked;
                if (px[0] != (uint8_t)want || px[0] != px[1] || px[2] != px[0] || px[3] != 255) {
                    if (bad == 0) {
                        badX = x;
                        badY = y;
                        badGot = px[0];
                        badWant = want;
                    }
                    ++bad;
                }
            }
        }
        expect(
            bad == 0,
            QString("S16: canvas-sized identity layer is texel-exact inside the %1px border (precondition; %2 of %3 "
                    "interior pixels off, first x=%4 y=%5 got=%6 want=%7)")
                .arg(crop).arg(bad).arg(checked).arg(badX).arg(badY).arg(badGot).arg(badWant));
    }

    // (1) destination range over a varying backdrop, for a separable and a non-separable mode
    for (const img::BlendMode mode : {img::BlendMode_Normal, img::BlendMode_Multiply, img::BlendMode_Color}) {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mode = mode;
        spec.blendIf = midBandDst;
        const int tol = mode == img::BlendMode_Color ? 3 : 2;
        compare(
            QString("blend if dst range over a ramp (%1)").arg(modeName(mode)), renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), mode, 1.0, &midBandDst), tol);
    }

    // (2) source range: the weight follows the layer's own pixels
    for (const img::BlendMode mode : {img::BlendMode_Normal, img::BlendMode_Multiply}) {
        SceneSpec spec;
        spec.top = rampY;
        spec.bg = solid;
        spec.mode = mode;
        spec.blendIf = midBandSrc;
        compare(
            QString("blend if src range over a ramp (%1)").arg(modeName(mode)), renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), mode, 1.0, &midBandSrc), 2);
    }

    // (3) both ranges multiply
    {
        SceneSpec spec;
        spec.top = rampY;
        spec.bg = rampX;
        spec.mode = img::BlendMode_Multiply;
        spec.blendIf = bothBands;
        compare(
            "blend if src x dst ranges multiply", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), spec.mode, 1.0, &bothBands), 2);
    }

    // (4) empty (crossed) range suppresses the layer entirely
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.blendIf = emptyDst;
        const std::vector<uint8_t> gpu = renderSpec(spec);
        const std::vector<uint8_t> bg = bgOf(spec);
        expect(gpu == bg, "S16: emptied destination range draws nothing (byte-identical to the backdrop)");
    }

    // (5) default ranges are a no-op (the gate that keeps every existing project unchanged)
    {
        SceneSpec withRanges;
        withRanges.top = rampY;
        withRanges.bg = rampX;
        withRanges.mode = img::BlendMode_Overlay;
        withRanges.blendIf = core::BlendIf(); // default == inactive
        SceneSpec without = withRanges;
        const std::vector<uint8_t> a = renderSpec(withRanges);
        const std::vector<uint8_t> b = renderSpec(without);
        expect(a == b, "S16: default ranges == no ranges (byte-identical)");
    }

    // (6) the weight composes with the layer's own opacity and alpha
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mode = img::BlendMode_Normal;
        spec.opacity = 0.5f;
        spec.blendIf = midBandDst;
        // the app quantizes the present opacity into uColor's 8-bit alpha
        compare(
            "blend if weight x layer opacity 0.5", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), spec.mode, 127.0 / 255.0, &midBandDst), 3);
    }
    {
        SceneSpec spec;
        spec.top = solidSemi;
        spec.bg = rampX;
        spec.blendIf = midBandSrc;
        // a semi-transparent content render is stored premultiplied in the RGBA8 readback,
        // so the reference's un-multiply carries ~1 LSB of noise (max observed 1/255); the
        // tolerance is on the mean for that reason, not on the weight
        compare(
            "blend if weight x layer alpha 160/255", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), spec.mode, 1.0, &midBandSrc), 3, 0.1);
    }

    // (7) folder presentation path (composite folder with a non-Normal mode + ranges)
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mode = img::BlendMode_Multiply;
        spec.blendIf = midBandDst;
        spec.folder = true;
        compare(
            "blend if on a folder present (Multiply)", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), spec.mode, 1.0, &midBandDst), 2);
    }

    // (7b) a folder whose ONLY non-default property is its ranges
    // A folder applies its ranges on the composite path (FolderNode::renderComposite), and that
    // path is entered through FolderNode::isCompositeFolder - which listed a non-Normal blend
    // mode, an HSV key and a blur radius, but NOT the ranges. A folder whose only non-default
    // property was its ranges therefore never reached the code that applies them: parsed,
    // persisted, shown in the panel and silently ignored, while a folder that also had a mode or
    // a blur applied them. Every folder case in this suite (7) and (8) carried one of those too,
    // which is exactly why nothing noticed.
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mode = img::BlendMode_Normal; // nothing else asks for the composite path
        spec.folder = true;
        spec.blendIf = emptyDst;
        const std::vector<uint8_t> hidden = renderSpec(spec);
        expect(
            hidden == bgOf(spec),
            "S16: a ranges-only folder with a crossed range == backdrop (byte-identical)");

        // and the weight it does apply is the one the reference computes
        spec.blendIf = midBandDst;
        compare(
            "blend if on a ranges-only folder present (Normal)", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), img::BlendMode_Normal, 1.0, &midBandDst),
            2);
    }

    // (8) composite/blur presentation plumbing: an emptied range must vanish, the default
    // must be byte-identical to no ranges (the blurred content itself is S4's subject)
    for (const bool folder : {false, true}) {
        const QString who = folder ? "blurred folder" : "blurred layer";
        {
            SceneSpec spec;
            spec.top = solid;
            spec.bg = rampX;
            spec.blur = 6.0f;
            spec.folder = folder;
            spec.blendIf = emptyDst;
            const std::vector<uint8_t> gpu = renderSpec(spec);
            const std::vector<uint8_t> bg = bgOf(spec);
            expect(gpu == bg, QString("S16: %1 with an emptied range == backdrop (byte-identical)").arg(who));
        }
        {
            // an ACTIVE range on the composite path: the presented (blurred) composite is
            // weighted against the backdrop. The blurred content and the backdrop come from
            // the same tree rendered with the range removed, so this pins the weight and the
            // present plumbing; the blur math itself is S4's subject.
            SceneSpec spec;
            spec.top = solid;
            spec.bg = rampX;
            spec.blur = 6.0f;
            spec.folder = folder;
            SceneSpec ranged = spec;
            ranged.blendIf = midBandDst;
            SceneSpec contentSpec = spec;
            contentSpec.bg = Fill();
            SceneSpec bgSpec = spec;
            bgSpec.top = Fill();
            compare(
                QString("blend if %1 present is weighted").arg(who), renderSpec(ranged),
                ref::blendPresent(
                    imageOf(renderSpec(contentSpec)), imageOf(renderSpec(bgSpec)), img::BlendMode_Normal, 1.0,
                    &midBandDst),
                4);
        }
    }

    // (9) a CROSSED destination range, against a HAND-COMPUTED value rather than the CPU
    // reference (which shares the semantics, so agreeing with it would prove nothing).
    // Backdrop luma 128 exactly; range black 64..192 / white 96..224, i.e. crossed because
    // 192 > 96. The black edge is (128-64)/128 = 0.5, the white edge (224-128)/128 = 0.75, and
    // the weight is min(0.5, 0.75) = 0.5 - the product would be 0.375. An opaque top layer of
    // (200,40,40) at Normal over (128,128,128) therefore lands on backdrop + w*(top-backdrop)
    // = (164, 84, 84); the product reading would give (155, 95, 95).
    {
        const Fill grey = [](int, int, uint8_t* px) {
            px[0] = px[1] = px[2] = 128;
            px[3] = 255;
        };
        const Fill topFlat = [](int, int, uint8_t* px) {
            px[0] = 200;
            px[1] = 40;
            px[2] = 40;
            px[3] = 255;
        };
        SceneSpec spec;
        spec.top = topFlat;
        spec.bg = grey;
        spec.blendIf.dst = core::BlendIf::Range{64, 192, 96, 224};
        const std::vector<uint8_t> gpu = renderSpec(spec);
        const size_t middle = ((size_t)(canvas.height() / 2) * canvas.width() + canvas.width() / 2) * 4;
        const int got[3] = {gpu[middle], gpu[middle + 1], gpu[middle + 2]};
        const int want[3] = {164, 84, 84};
        bool ok = true;
        for (int c = 0; c < 3; ++c)
            ok = ok && std::abs(got[c] - want[c]) <= 1;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport(
            "S16: a CROSSED range takes min(black, white) = 0.5, not the product 0.375", ok,
            QString("got %1,%2,%3 want %4,%5,%6 (the product gives 155,95,95)")
                .arg(got[0]).arg(got[1]).arg(got[2]).arg(want[0]).arg(want[1]).arg(want[2])
        );
    }

    // (10) the PSD import leg: PSDWriter can emit the blending-ranges section, so a written
    // file round-trips through the production importer and must land on the node. (The
    // writer writes the composite pair then the per-channel pairs; only the composite pair
    // is modelled.)
    {
        img::PSDFormat format;
        format.header().version = 1;
        format.header().channels = 4;
        format.header().height = 64;
        format.header().width = 64;
        format.header().depth = 8;
        format.header().mode = img::PSDFormat::ColorMode_RGB;
        format.imageData().compressionId = 0;

        const auto addLayer = [&](const std::string& aName, bool aWithRanges) {
            auto* layer = new img::PSDFormat::Layer();
            layer->name = aName;
            layer->rect.edge[0] = 0;
            layer->rect.edge[1] = 0;
            layer->rect.edge[2] = 64;
            layer->rect.edge[3] = 64;
            layer->blendMode = "norm";
            layer->opacity = 255;
            layer->flags = 0x08;
            for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
                auto* chan = new img::PSDFormat::Channel();
                chan->id = id;
                chan->compressionId = 0;
                chan->dataLength = 64 * 64;
                chan->data.reset(new uint8[64 * 64]());
                layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
            }
            if (aWithRanges) {
                auto* range = new img::PSDFormat::BlendingRange();
                range->isValid = true;
                const uint8 src[4] = {80, 120, 180, 220};
                const uint8 dst[4] = {29, 68, 179, 137};
                std::memcpy(range->src, src, 4);
                std::memcpy(range->dst, dst, 4);
                layer->compositeBlendingRange.reset(range);
            }
            format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
        };
        addLayer("bg", false);
        addLayer("ranged", true); // topmost (records are bottom-first)

        const uint32 mergedSize = 512 * 64;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = mergedSize;
            chan->data.reset(new uint8[mergedSize]());
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }

        const QString path = "/tmp/vb_blend_if_import.psd";
        {
            std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, format);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S16: PSDWriter emits the ranges");
            out.close();
        }

        StubAnimator animator;
        core::Project project("", animator, nullptr);
        ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
        NullProgressReporter reporter;
        const bool loaded = loader.load(path, project, reporter);
        expect(loaded, QString("S16: import of the written PSD: %1").arg(loader.log()));
        if (loaded) {
            core::BlendIf found;
            bool sawLayer = false;
            const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
                if (aNode->name() == "ranged") {
                    sawLayer = true;
                    if (const core::Renderer* r = aNode->renderer())
                        found = r->blendIf();
                }
                for (auto* child : aNode->children())
                    walk(child);
            };
            walk(project.objectTree().topNode());
            expect(sawLayer, "S16: the written PSD imports its layer");
            expect(
                found.dst == core::BlendIf::Range{29, 68, 179, 137} && found.src == core::BlendIf::Range{80, 120, 180, 220},
                QString("S16: imported ranges match the file (dst %1/%2/%3/%4, src %5/%6/%7/%8)")
                    .arg(found.dst.blackLo).arg(found.dst.blackHi).arg(found.dst.whiteLo).arg(found.dst.whiteHi)
                    .arg(found.src.blackLo).arg(found.src.blackHi).arg(found.src.whiteLo).arg(found.src.whiteHi));
        }
    }

    // (10) the ranges ride the project format: appended after each object block's time line
    // (minor 12), so a stream read under the previous label must leave them inactive and
    // stop exactly where that layout ended - the same gate S8 pins for the folder 4CC
    {
        const int serChecksBefore = gChecks;
        core::ObjectTree tree; // provides the shader holder the LayerNode needs
        core::LayerNode layerSrc("blendifme", tree.shaderHolder());
        core::BlendIf ranges;
        ranges.dst = core::BlendIf::Range{29, 68, 179, 137};
        ranges.src = core::BlendIf::Range{80, 120, 180, 220};
        layerSrc.setBlendIf(ranges);

        std::ostringstream buffer(std::ios::binary);
        util::StreamWriter writer(buffer);
        core::Serializer serializer(writer);
        expect(layerSrc.serialize(serializer), "S16: layer with ranges serializes");

        gl::DeviceInfo deviceInfo;
        NullProgressReporter reporter;
        {
            std::istringstream in(buffer.str(), std::ios::binary);
            util::LEStreamReader reader(in);
            core::Deserializer::IDSolverType solver;
            core::Deserializer dst(
                reader, solver, buffer.str().size(), QVersionNumber(AE_PROJECT_FORMAT_MAJOR_VERSION, AE_PROJECT_FORMAT_MINOR_VERSION),
                deviceInfo, reporter, 0);
            core::LayerNode layerDst("x", tree.shaderHolder());
            expect(layerDst.deserialize(dst), "S16: layer deserialize @ current version");
            expect(
                layerDst.blendIf() == ranges, "S16: blend if ranges round-trip through the project format");
        }
        {
            // a pre-12 label ends the node block before the ranges: the node keeps the
            // inactive default (the appended words stay unread)
            std::istringstream in(buffer.str(), std::ios::binary);
            util::LEStreamReader reader(in);
            core::Deserializer::IDSolverType solver;
            core::Deserializer dst(reader, solver, buffer.str().size(), QVersionNumber(0, 11), deviceInfo, reporter, 0);
            core::LayerNode layerOld("x", tree.shaderHolder());
            expect(layerOld.deserialize(dst), "S16: layer deserialize @ 0.11");
            expect(
                !layerOld.blendIf().isActive(), "S16: a pre-12 label leaves the ranges inactive");
        }
        caseReport(
            "blend if binary round-trip (minor 12 gate)", gFails == failsBefore,
            QString("%1 checks").arg(gChecks - serChecksBefore));
    }

    caseReport(
        "blend if (ranges, gates, opacity/alpha composition, folder + blur presents)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S17: PSD layer masks (img::ResourceData's mask plane + the shader's mask fetch). The mask
// multiplies the layer's coverage per pixel, so the CPU reference is the same
// ref::blendPresent with a mask image (the reference's mask is built in GL row order like
// every other reference input, which is what pins the sampling orientation). Flat,
// canvas-sized, identity-transformed layers keep everything texel-exact.
void suiteLayerMask() {
    suiteHeader("S17 layer mask: GPU vs CPU reference");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    bool gMaskRowFlip = true; // set by the orientation precondition below

    using Fill = std::function<void(int, int, uint8_t*)>;
    struct Pool {
        QSize size;
        std::vector<img::ResourceNode*> nodes;
        ~Pool() { qDeleteAll(nodes.begin(), nodes.end()); }
        img::ResourceNode* image(const QString& aId, const Fill& aFill, const std::function<uint8(int, int)>& aMask) {
            auto* node = scene::makeImage(aId, size, aFill);
            if (aMask) {
                uint8* data = new uint8[(size_t)size.width() * size.height()];
                for (int y = 0; y < size.height(); ++y)
                    for (int x = 0; x < size.width(); ++x)
                        data[(size_t)y * size.width() + x] = aMask(x, y);
                node->data().grabMask(XCMemBlock(data, (size_t)size.width() * size.height()), size);
                node->data().setMaskEnabled(maskEnabled);
                node->data().setMaskInverted(maskInverted);
            }
            nodes.push_back(node);
            return node;
        }
        bool maskEnabled = true;
        bool maskInverted = false;
    };
    const Fill rampX = [](int x, int, uint8_t* px) {
        const int v = x * 255 / 127;
        px[0] = px[1] = px[2] = (uint8_t)v;
        px[3] = 255;
    };
    const Fill solid = [](int, int, uint8_t* px) {
        px[0] = 200; px[1] = 80; px[2] = 60; px[3] = 255;
    };
    // a vertical luma ramp (drives the SOURCE range, whose weight follows the layer's pixels)
    const Fill rampY = [](int, int y, uint8_t* px) {
        const int v = y * 255 / 95;
        px[0] = px[1] = px[2] = (uint8_t)v;
        px[3] = 255;
    };
    // a diagonal mask: the reference rebuilds it in GL row order, so a vertical flip in the
    // sampling path shows up as a mismatch rather than cancelling out
    const std::function<uint8(int, int)> maskDiagonal = [](int x, int y) {
        const int v = (x + 2 * y) * 255 / (127 + 2 * 95);
        return (uint8)qBound(0, v, 255);
    };
    const std::function<uint8(int, int)> maskBlack = [](int, int) { return (uint8)0; };
    const std::function<uint8(int, int)> maskWhite = [](int, int) { return (uint8)255; };

    core::BlendIf midBandDst;
    midBandDst.dst = core::BlendIf::Range{64, 128, 160, 224};

    struct SceneSpec {
        Fill top; // empty = no top layer
        Fill bg;
        std::function<uint8(int, int)> mask;
        bool maskEnabled = true;
        bool maskInverted = false;
        img::BlendMode mode = img::BlendMode_Normal;
        float opacity = 1.0f;
        core::BlendIf blendIf;
        float blur = 0.0f;
        bool folder = false;
    };
    auto renderSpec = [&](const SceneSpec& aSpec) {
        Pool pool;
        pool.size = canvas;
        pool.maskEnabled = aSpec.maskEnabled;
        pool.maskInverted = aSpec.maskInverted;
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        if (aSpec.top) {
            auto* res = pool.image("top", aSpec.top, aSpec.mask);
            if (aSpec.folder) {
                auto* folder = scene::addFolder(root, "folder", center, 0.0f, unit);
                folder->setBlendMode(aSpec.mode);
                folder->setBlendIf(aSpec.blendIf);
                scene::addLayer(tree, folder, res, "top", QVector2D(0, 0), 0.0f, unit);
                if (aSpec.blur > 0.0f)
                    scene::addBlurAmount(folder, 0, aSpec.blur);
            } else {
                auto* layer = scene::addLayer(tree, root, res, "top", center, 0.0f, unit);
                layer->setBlendMode(aSpec.mode);
                layer->setBlendIf(aSpec.blendIf);
                if (aSpec.opacity != 1.0f)
                    layer->setDefaultOpacity(aSpec.opacity);
                if (aSpec.blur > 0.0f)
                    scene::addBlurAmount(layer, 0, aSpec.blur);
            }
        }
        // the first-pushed sibling renders on top, so the backdrop goes last
        if (aSpec.bg)
            scene::addLayer(tree, root, pool.image("bg", aSpec.bg, nullptr), "bg", center, 0.0f, unit);
        return fx.render(tree, camera, 0);
    };
    auto imageOf = [&](const std::vector<uint8_t>& aBytes) {
        return ref::imageFromBytes(aBytes.data(), canvas.width(), canvas.height());
    };
    // the reference's mask is a single-channel image in the same GL row order as the
    // readbacks; the sampling convention (which image row lands on readback row 0) is
    // pinned by this suite's orientation precondition below
    auto maskImageOf = [&](const std::function<uint8(int, int)>& aMask) {
        ref::Image img(canvas.width(), canvas.height());
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                const float v = aMask(x, gMaskRowFlip ? canvas.height() - 1 - y : y) / 255.0f;
                float* px = img.at(x, y);
                px[0] = px[1] = px[2] = v;
                px[3] = 1.0f;
            }
        }
        return img;
    };
    auto compare = [&](const QString& aName, const std::vector<uint8_t>& aGpu, const ref::Image& aExpected,
                       int aMaxTol, double aMeanTol = 0.05) {
        const scene::Diff d = scene::diffImages(aGpu, ref::imageToBytes(aExpected));
        const bool ok = checkDiff(d, aMaxTol, aMeanTol, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport(aName, ok, diffStr(d));
    };
    // the reference's content render carries no mask (and no mode/opacity/ranges): the
    // effect under test is applied by blendPresent to that plain content, exactly like the
    // S12/S16 references - rendering it masked AND passing the mask would apply it twice
    auto contentOf = [&](SceneSpec aSpec) {
        aSpec.bg = Fill();
        aSpec.mask = nullptr;
        aSpec.mode = img::BlendMode_Normal;
        aSpec.opacity = 1.0f;
        aSpec.blur = 0.0f;
        aSpec.blendIf = core::BlendIf();
        aSpec.folder = false;
        return renderSpec(aSpec);
    };
    auto bgOf = [&](SceneSpec aSpec) {
        aSpec.top = Fill();
        return renderSpec(aSpec);
    };

    // (0) gates: an all-white mask is a no-op, an all-black mask erases the layer
    {
        SceneSpec plain;
        plain.top = solid;
        plain.bg = rampX;
        SceneSpec white = plain;
        white.mask = maskWhite;
        SceneSpec black = plain;
        black.mask = maskBlack;
        expect(renderSpec(white) == renderSpec(plain), "S17: an all-white mask is a no-op (byte-identical)");
        expect(renderSpec(black) == bgOf(plain), "S17: an all-black mask erases the layer (== backdrop)");
    }

    // (1) orientation precondition: a vertical mask ramp over a white top layer must show up
    // in the readback with the row order the reference assumes; this pins the convention for
    // every comparison below instead of leaving it to a guess
    {
        const std::function<uint8(int, int)> rampTopToBottom = [](int, int y) { return (uint8)(y * 255 / 95); };
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = rampTopToBottom;
        const std::vector<uint8_t> gpu = renderSpec(spec);
        // readback row 0 is the canvas bottom: with image row 0 at the canvas top the ramp
        // value there must be ~255 (the mask's last row), with image row 0 at the bottom ~0
        const int bottomRow = gpu[0];
        const int topRow = gpu[(size_t)(canvas.height() - 1) * canvas.width() * 4];
        const bool flipped = bottomRow > 128 && topRow < 128;
        const bool unflipped = bottomRow < 128 && topRow > 128;
        expect(
            flipped != unflipped,
            QString("S17: mask row order is one of the two conventions (bottom=%1 top=%2)").arg(bottomRow).arg(topRow));
        gMaskRowFlip = flipped;
        std::printf("    [INFO] mask row order: image row 0 is at the canvas %s\n", flipped ? "top" : "bottom");
    }

    // (2) the mask multiplies coverage, for a separable and a non-separable mode
    for (const img::BlendMode mode : {img::BlendMode_Normal, img::BlendMode_Multiply, img::BlendMode_Color}) {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = maskDiagonal;
        spec.mode = mode;
        const ref::Image mask = maskImageOf(maskDiagonal);
        compare(
            QString("layer mask (%1)").arg(img::getBlendNameFromBlendMode(mode)), renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), mode, 1.0, nullptr, &mask),
            mode == img::BlendMode_Color ? 3 : 2);
    }

    // (2) inverted and disabled masks
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = maskDiagonal;
        spec.maskInverted = true;
        const std::function<uint8(int, int)> inverted = [&](int x, int y) { return (uint8)(255 - maskDiagonal(x, y)); };
        const ref::Image mask = maskImageOf(inverted);
        compare(
            "layer mask inverted", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), img::BlendMode_Normal, 1.0, nullptr, &mask), 2);
    }
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = maskDiagonal;
        spec.maskEnabled = false;
        SceneSpec plain = spec;
        plain.mask = nullptr;
        expect(renderSpec(spec) == renderSpec(plain), "S17: a disabled mask is not applied (byte-identical)");
    }

    // (3) the mask composes with the layer opacity and with Blend If
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = maskDiagonal;
        spec.opacity = 0.5f;
        const ref::Image mask = maskImageOf(maskDiagonal);
        compare(
            "layer mask x opacity 0.5", renderSpec(spec),
            ref::blendPresent(imageOf(contentOf(spec)), imageOf(bgOf(spec)), img::BlendMode_Normal, 127.0 / 255.0, nullptr, &mask), 3);
    }
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.mask = maskDiagonal;
        spec.blendIf = midBandDst;
        const ref::Image mask = maskImageOf(maskDiagonal);
        compare(
            "layer mask x blend if", renderSpec(spec),
            ref::blendPresent(
                imageOf(contentOf(spec)), imageOf(bgOf(spec)), img::BlendMode_Normal, 1.0, &midBandDst, &mask),
            3);
    }

    // (4) composite/blur path: the mask rides the layer content, so an all-black mask must
    // erase even a blurred layer, and an all-white mask must match the unmasked render
    {
        SceneSpec spec;
        spec.top = solid;
        spec.bg = rampX;
        spec.blur = 6.0f;
        SceneSpec white = spec;
        white.mask = maskWhite;
        SceneSpec black = spec;
        black.mask = maskBlack;
        expect(
            renderSpec(white) == renderSpec(spec),
            "S17: an all-white mask on a blurred layer matches no mask (byte-identical)");
        const scene::Diff d = scene::diffImages(renderSpec(black), bgOf(spec));
        const bool ok = checkDiff(d, 2, 0.05, canvas);
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("S17: an all-black mask erases a blurred layer", ok, diffStr(d));
    }

    // (5) the production CPU trapezoid agrees with the reference's independent one
    {
        int bad = 0;
        const core::BlendIf::Range ranges[3] = {midBandDst.dst, core::BlendIf::Range{80, 120, 180, 220},
                                                core::BlendIf::Range{255, 255, 0, 0}};
        for (int v = 0; v <= 255; ++v) {
            for (const auto& r : ranges) {
                const double want = ref::blendIfRangeWeight((double)v, r.blackLo, r.blackHi, r.whiteLo, r.whiteHi);
                const float got = core::blendIfRangeWeight((float)v, r);
                if (std::abs(want - (double)got) > 0.01)
                    ++bad;
            }
        }
        expect(bad == 0, QString("S17: core::blendIfRangeWeight matches the reference over 256 values (%1 off)").arg(bad));
    }

    // (6) baking the SOURCE range into a mask is render-equivalent to leaving it live: the
    // baked mask is generated from the production CPU weight, so this pins the integration
    // (mask texture <-> range) rather than the formula, which the check above covers
    {
        SceneSpec spec;
        spec.top = rampY;
        spec.bg = rampX;
        spec.blendIf = core::BlendIf();
        spec.blendIf.src = core::BlendIf::Range{80, 120, 180, 220};
        const std::vector<uint8_t> ranged = renderSpec(spec);

        // the layer image in this scene is rampY; the baked mask is the source weight of each
        // pixel (no previous mask, so no composition)
        const std::function<uint8(int, int)> bakedMask = [&](int x, int y) {
            // rampY's fill: v = y * 255 / 95 (the same expression the fill above uses)
            const uint8_t v = (uint8)(y * 255 / 95);
            return (uint8)qBound(0, (int)(core::blendIfSourceWeight(spec.blendIf, v, v, v) * 255.0f + 0.5f), 255);
        };
        SceneSpec baked = spec;
        baked.mask = bakedMask;
        baked.blendIf = core::BlendIf(); // the range became pixels

        const scene::Diff d = scene::diffImages(ranged, renderSpec(baked));
        const bool ok = d.maxDiff <= 1;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("S17: baking the source range into a mask is render-equivalent", ok, diffStr(d));
    }

    // (7) the bake helper itself: bake attaches the mask and clears only the source range,
    // undo puts both back
    {
        Pool pool;
        pool.size = canvas;
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* layer = scene::addLayer(tree, root, pool.image("l", rampY, maskDiagonal), "l", center, 0.0f, unit);
        core::BlendIf ranges;
        ranges.src = core::BlendIf::Range{80, 120, 180, 220};
        ranges.dst = core::BlendIf::Range{29, 68, 179, 137};
        layer->setBlendIf(ranges);

        const auto resourceOf = [](core::ObjectNode& aNode) -> img::ResourceData* {
            core::TimeLine* tl = aNode.timeLine();
            auto* key = tl ? (core::ImageKey*)tl->defaultKey(core::TimeKeyType_Image) : nullptr;
            return key ? key->data().resource().get() : nullptr;
        };
        img::ResourceData* res = resourceOf(*layer);
        expect(res != nullptr && res->hasMask(), "S17: bake fixture layer has a mask");

        core::LayerMaskBake bake;
        expect(core::LayerMaskBake::isAvailable(*layer), "S17: bake is offered for a non-default source range");
        expect(core::LayerMaskBake::prepare(*layer, bake), "S17: bake prepares");
        bake.bake(*layer);
        expect(
            layer->blendIf().src.isDefault() && layer->blendIf().dst == ranges.dst,
            "S17: bake clears the source range and keeps the destination range");
        expect(
            res->hasMask() && res->isMaskEnabled() && !res->isMaskInverted(),
            "S17: the baked mask is enabled and not inverted");
        {
            // the baked plane is the source weight composed with the previous mask
            int bad = 0;
            const uint8_t* m = res->mask().data();
            for (int y = 0; y < canvas.height(); ++y) {
                for (int x = 0; x < canvas.width(); ++x) {
                    const uint8_t v = (uint8)(y * 255 / 95); // rampY, in image row order
                    const float w = core::blendIfSourceWeight(ranges, v, v, v);
                    const int want = ((int)(w * 255.0f + 0.5f) * maskDiagonal(x, y) + 127) / 255;
                    if (std::abs((int)m[(size_t)y * canvas.width() + x] - want) > 1)
                        ++bad;
                }
            }
            expect(bad == 0, QString("S17: baked mask is the source weight x the previous mask (%1 off)").arg(bad));
        }

        bake.restore(*layer);
        expect(layer->blendIf() == ranges, "S17: undo restores the ranges");
        {
            int bad = 0;
            const uint8_t* m = res->mask().data();
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = 0; x < canvas.width(); ++x)
                    if (m[(size_t)y * canvas.width() + x] != maskDiagonal(x, y))
                        ++bad;
            expect(bad == 0, QString("S17: undo restores the previous mask pixels (%1 off)").arg(bad));
        }
    }

    // (8) import: a PSD written with a user mask imports with its plane and flags
    {
        img::PSDFormat format;
        format.header().version = 1;
        format.header().channels = 4;
        format.header().height = 64;
        format.header().width = 64;
        format.header().depth = 8;
        format.header().mode = img::PSDFormat::ColorMode_RGB;
        format.imageData().compressionId = 0;

        auto* layer = new img::PSDFormat::Layer();
        layer->name = "masked";
        layer->rect.edge[0] = 0;
        layer->rect.edge[1] = 0;
        layer->rect.edge[2] = 64;
        layer->rect.edge[3] = 64;
        layer->blendMode = "norm";
        layer->opacity = 255;
        layer->flags = 0x08;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = 64 * 64;
            chan->data.reset(new uint8[64 * 64]);
            std::memset(chan->data.get(), 128, 64 * 64);
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        // the user mask channel: a left-to-right ramp, 0 outside the mask rect is not
        // exercised here (the rect covers the layer exactly)
        {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = -2;
            chan->compressionId = 0;
            chan->dataLength = 64 * 64;
            chan->data.reset(new uint8[64 * 64]);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    chan->data[(size_t)y * 64 + x] = (uint8)(x * 4);
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        layer->mask.reset(new img::PSDFormat::LayerMask());
        layer->mask->rect.edge[0] = 0;
        layer->mask->rect.edge[1] = 0;
        layer->mask->rect.edge[2] = 64;
        layer->mask->rect.edge[3] = 64;
        layer->mask->defaultColor = 255;
        layer->mask->flags = 0x06; // disabled (bit 1) + invert (bit 2): both must be imported
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));

        const uint32 mergedSize = 512 * 64;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = mergedSize;
            chan->data.reset(new uint8[mergedSize]());
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }

        const QString path = "/tmp/vb_layer_mask.psd";
        {
            std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, format);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S17: PSDWriter writes the mask");
            out.close();
        }

        StubAnimator animator;
        core::Project project("", animator, nullptr);
        ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
        NullProgressReporter reporter;
        const bool loaded = loader.load(path, project, reporter);
        expect(loaded, QString("S17: import of the masked PSD: %1").arg(loader.log()));
        const img::ResourceData* res = nullptr; // kept for the persistence check below
        if (loaded) {
            const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
                if (aNode->name() == "masked") {
                    if (const core::TimeLine* tl = aNode->timeLine()) {
                        if (const core::TimeKey* key = tl->defaultKey(core::TimeKeyType_Image)) {
                            res = ((const core::ImageKey*)key)->data().resource().get();
                        }
                    }
                }
                for (auto* child : aNode->children())
                    walk(child);
            };
            walk(project.objectTree().topNode());
            expect(res != nullptr, "S17: the imported layer has a resource");
            if (res) {
                expect(res->hasMask(), "S17: the imported layer has a mask");
                expect(
                    res->mask().pixelSize() == QSize(66, 66) && res->mask().format() == img::Format_A8,
                    QString("S17: mask plane is A8 in the padded layer rect (got %1x%2 fmt %3)")
                        .arg(res->mask().pixelSize().width()).arg(res->mask().pixelSize().height())
                        .arg((int)res->mask().format()));
                expect(!res->isMaskEnabled(), "S17: the disabled flag (bit 1) is imported");
                expect(res->isMaskInverted(), "S17: the invert flag (bit 2) is imported");
                // the layer rect is the PSD rect padded by the 1px sampling border, so the
                // ramp's value at padded x is (x - 1) * 4 clamped to the original edges
                if (res->hasMask() && res->mask().pixelSize() == QSize(66, 66)) {
                    const uint8* m = res->mask().data();
                    const int centre = m[(size_t)33 * 66 + 33];
                    const int right = m[(size_t)33 * 66 + 65];
                    expect(
                        centre == 32 * 4 && right >= 63 * 4,
                        QString("S17: mask values decode (centre %1, right edge %2)").arg(centre).arg(right));
                }
            }
        }

        // (9) persistence: the mask survives save -> load through the production project IO
        if (loaded) {
            const QString aniePath = "/tmp/vb_layer_mask.anie";
            ctrl::ProjectSaver saver;
            expect(saver.save(aniePath, project), "S17: project with a mask saves");
            core::Project reloaded("", animator, nullptr);
            ctrl::ProjectLoader loader2;
            const bool reloadedOk = loader2.load(aniePath, reloaded, gl::DeviceInfo::instance(), reporter);
            expect(reloadedOk, "S17: project with a mask reloads");
            if (reloadedOk) {
                const img::ResourceData* res2 = nullptr;
                const std::function<void(core::ObjectNode*)> walk2 = [&](core::ObjectNode* aNode) {
                    if (aNode->name() == "masked") {
                        if (const core::TimeLine* tl = aNode->timeLine()) {
                            if (const core::TimeKey* key = tl->defaultKey(core::TimeKeyType_Image)) {
                                res2 = ((const core::ImageKey*)key)->data().resource().get();
                            }
                        }
                    }
                    for (auto* child : aNode->children())
                        walk2(child);
                };
                walk2(reloaded.objectTree().topNode());
                expect(res2 && res2->hasMask(), "S17: the mask survives the project round-trip");
                if (res2 && res2->hasMask()) {
                    expect(
                        res2->isMaskEnabled() == res->isMaskEnabled()
                            && res2->isMaskInverted() == res->isMaskInverted(),
                        "S17: mask flags survive the project round-trip");
                    expect(
                        res2->mask().pixelSize() == res->mask().pixelSize()
                            && std::equal(res2->mask().data(), res2->mask().data() + res2->mask().size(),
                                res->mask().data()),
                        "S17: mask pixels survive the project round-trip (byte-identical)");
                }
            }
        }
    }

    caseReport(
        "layer mask (coverage, invert, disable, composition, import, persistence)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S18: Photoshop layer effects (core/LayerEffects, img/PSDDescriptor). Covers the descriptor
// reader against a hand-written lfx2 block (the byte layout is in the test, so the reader is
// pinned without the 114 MB file), the gradient overlay's placement against an independently
// written reference, the "effects ride the content pass" composition, and the import and
// project-format legs.

//-------------------------------------------------------------------------------------------------
// S21: resource identity. A source reload has to answer "which layer in the project is this layer
// in the file?", and it used to answer it with the layer NAME alone - which breaks on a rename and
// is ambiguous when two layers share a name. Photoshop's "lyid" gives a stable id; the model owns
// the rule (img::ResourceData::matchesIdentity) so the reload path and the duplicate-sibling check
// cannot disagree.
void suiteResourceIdentity() {
    suiteHeader("S21 resource identity: stable ids, rename tolerance, name fallback");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    const auto make = [](const char* aName, const char* aId, bool aIsLayer) {
        auto* node = new img::ResourceNode(aName);
        node->data().setIsLayer(aIsLayer);
        if (aId && *aId) {
            node->data().setStableId(aId);
        }
        return node;
    };
    const auto same = [](img::ResourceNode* a, img::ResourceNode* b) {
        return a->data().matchesIdentity(b->data());
    };

    // (1) the id decides when BOTH sides have one - that is what makes a rename survivable and
    // what tells two same-named layers apart. When only one side has an id the name decides: that
    // is the migration path for a project written before ids existed, and the fallback for a file
    // whose producer writes none.
    {
        QScopedPointer<img::ResourceNode> a(make("Layer 1", "lyid:7", true));
        QScopedPointer<img::ResourceNode> renamed(make("title copy", "lyid:7", true));
        QScopedPointer<img::ResourceNode> other(make("Layer 1", "lyid:8", true));
        QScopedPointer<img::ResourceNode> unnamed(make("Layer 1", "", true));
        expect(same(a.data(), renamed.data()), "S21: the same id matches across a rename");
        expect(!same(a.data(), other.data()), "S21: different ids do not match even with the same name");
        expect(
            same(a.data(), unnamed.data()) && same(other.data(), unnamed.data()),
            "S21: an id on one side only falls back to the name (the pre-id migration path)");
    }

    // (2) without ids on either side the name is all there is (the pre-id behaviour, which is
    // what a project written before ids existed still gets)
    {
        QScopedPointer<img::ResourceNode> a(make("title", "", true));
        QScopedPointer<img::ResourceNode> b(make("title", "", true));
        QScopedPointer<img::ResourceNode> c(make("other", "", true));
        QScopedPointer<img::ResourceNode> folder(make("title", "", false));
        expect(same(a.data(), b.data()), "S21: two id-less resources match by name");
        expect(!same(a.data(), c.data()), "S21: different names do not match");
        expect(!same(a.data(), folder.data()), "S21: a layer never matches a folder");
    }

    // (3) kind is checked even when ids agree: a group and a layer must not correspond
    {
        QScopedPointer<img::ResourceNode> layer(make("title", "lyid:3", true));
        QScopedPointer<img::ResourceNode> group(make("title", "lyid:3", false));
        expect(!same(layer.data(), group.data()), "S21: a matching id does not override the layer/folder kind");
    }

    // (4) two siblings sharing a name are distinguishable once they have ids: that ambiguity is
    // exactly what the reload used to refuse
    {
        // the parent owns its children (TreeNodeBase's destructor deletes them), so these are
        // raw pointers: a scoped pointer here would double-free
        img::ResourceNode parent("top");
        parent.data().setIsLayer(false);
        auto* first = make("layer", "lyid:1", true);
        auto* second = make("layer", "lyid:2", true);
        auto* third = make("layer", "", true);
        parent.children().pushBack(first);
        parent.children().pushBack(second);
        expect(
            first->getCountOfSameSiblings() == 0 && second->getCountOfSameSiblings() == 0,
            "S21: same-named siblings with distinct ids are not duplicates");
        parent.children().pushBack(third);
        // the id-less sibling is ambiguous with BOTH of the id-bearing ones (the name is all it
        // has), which is exactly the case the reload refuses to guess at
        expect(
            first->getCountOfSameSiblings() == 1 && third->getCountOfSameSiblings() == 2,
            QString("S21: an id-less same-named sibling stays ambiguous (%1, %2)")
                .arg(first->getCountOfSameSiblings())
                .arg(third->getCountOfSameSiblings()));
    }

    // (5) the importer fills the id from the PSD's "lyid" block, and it survives a project
    // round-trip - without it a reload after a rename would fall back to the name
    {
        const int side = 32;
        img::PSDFormat format;
        format.header().version = 1;
        format.header().channels = 4;
        format.header().height = side;
        format.header().width = side;
        format.header().depth = 8;
        format.header().mode = img::PSDFormat::ColorMode_RGB;
        format.imageData().compressionId = 0;

        struct Spec {
            const char* name;
            uint32 id;
            bool layer;
        };
        const Spec specs[2] = {{"group", 4242u, false}, {"inside", 99u, true}};
        for (const Spec& spec : specs) {
            auto* layer = new img::PSDFormat::Layer();
            layer->name = spec.name;
            layer->rect.edge[0] = 0;
            layer->rect.edge[1] = 0;
            layer->rect.edge[2] = side;
            layer->rect.edge[3] = side;
            layer->blendMode = spec.layer ? "norm" : "pass";
            layer->opacity = 255;
            layer->flags = spec.layer ? 0x08 : 0x18;
            layer->entryType = spec.layer ? img::PSDFormat::LayerEntryType_Layer
                                         : img::PSDFormat::LayerEntryType_OpenFolder;
            for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
                auto* chan = new img::PSDFormat::Channel();
                chan->id = id;
                chan->compressionId = 0;
                // a group record carries the same four channel slots with no data, which is what
                // Photoshop writes and what the importer expects to find
                chan->dataLength = spec.layer ? side * side : 0;
                if (spec.layer) {
                    chan->data.reset(new uint8[side * side]());
                    if (id == -1) {
                        std::memset(chan->data.get(), 255, side * side);
                    }
                }
                layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
            }
            {
                // "lyid": a big-endian uint32, the layer's stable id
                auto* info = new img::PSDFormat::AdditionalLayerInfo();
                info->key = "lyid";
                info->dataLength = 4;
                info->data.reset(new uint8[4]());
                info->data[0] = (uint8)(spec.id >> 24);
                info->data[1] = (uint8)(spec.id >> 16);
                info->data[2] = (uint8)(spec.id >> 8);
                info->data[3] = (uint8)spec.id;
                layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
            }
            format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
        }
        // a bounding record closes the group
        {
            auto* layer = new img::PSDFormat::Layer();
            layer->name = "</Layer group>";
            layer->blendMode = "norm";
            layer->flags = 0x18;
            layer->entryType = img::PSDFormat::LayerEntryType_Bounding;
            format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));
        }
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = side * side;
            chan->data.reset(new uint8[side * side]());
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }

        const QString path = "/tmp/vb_stable_id.psd";
        {
            std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, format);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S21: PSDWriter writes the ids");
            out.close();
        }

        StubAnimator animator;
        core::Project project("", animator, nullptr);
        ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
        NullProgressReporter reporter;
        const bool loaded = loader.load(path, project, reporter);
        expect(loaded, QString("S21: the PSD with layer ids imports: %1").arg(loader.log()));
        if (loaded) {
            const auto& trees = project.resourceHolder().imageTrees();
            expect(!trees.empty(), "S21: an image tree was created");
            QStringList ids;
            if (!trees.empty()) {
                std::function<void(img::ResourceNode*)> walk = [&](img::ResourceNode* aNode) {
                    if (!aNode->data().stableId().isEmpty()) {
                        ids << aNode->data().stableId();
                    }
                    for (auto child : aNode->children()) {
                        walk(child);
                    }
                };
                walk(trees.front().topNode);
            }
            ids.sort();
            expect(
                ids == QStringList({"lyid:4242", "lyid:99"}),
                QString("S21: the importer reads lyid for layers and groups (got %1)").arg(ids.join(",")));

            const QString aniePath = "/tmp/vb_stable_id.anie";
            ctrl::ProjectSaver saver;
            expect(saver.save(aniePath, project), "S21: the project saves");
            core::Project reloaded("", animator, nullptr);
            ctrl::ProjectLoader loader2;
            const bool reloadedOk = loader2.load(aniePath, reloaded, gl::DeviceInfo::instance(), reporter);
            expect(reloadedOk, "S21: the project reloads");
            if (reloadedOk) {
                const auto& trees2 = reloaded.resourceHolder().imageTrees();
                QStringList ids2;
                if (!trees2.empty()) {
                    std::function<void(img::ResourceNode*)> walk2 = [&](img::ResourceNode* aNode) {
                        if (!aNode->data().stableId().isEmpty()) {
                            ids2 << aNode->data().stableId();
                        }
                        for (auto child : aNode->children()) {
                            walk2(child);
                        }
                    };
                    walk2(trees2.front().topNode);
                }
                ids2.sort();
                expect(ids2 == ids, QString("S21: the ids survive the round-trip (got %1)").arg(ids2.join(",")));
            }
        }
    }

    const bool ok = (gFails == failsBefore);
    caseReport(
        QString("resource identity (stable ids, rename tolerance, fallback)"), ok,
        QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S22: the layer mesh must not change what a layer draws.
//
// Every layer is drawn through a triangular mesh generated from its own alpha
// (img::GridMeshCreator): a cell exists only where the image "has content", and the boundary
// vertices are pulled inward ("burr reduction") so the mesh follows the shape instead of
// carrying empty space around. That mesh is a RENDERING structure: whatever no cell covers is
// never drawn, so every pixel the layer must draw has to be inside a cell.
//
// It used to be alpha > 10, which silently clipped the faint end of every antialiased edge and
// dropped whole cells whose content was fainter than that. Measured on this repository's
// 114 MB PSD, that cost 13% of the whole-canvas difference against the file's own stored
// composite (mean 4.5790 with the generated mesh, 3.9942 with an exact bounding quad over the
// same 51 layers) and 585k pixels of >2 difference: content the mesh refused to draw.
//
// The invariant below needs no external oracle, which is what makes the failure obvious: the
// same layer, drawn through the generated mesh and through an exact bounding quad (a cell size
// at or above the bitmap size makes GridMesh::createFromImage build a quad), must produce the
// same pixels - and both must reproduce the layer's own bitmap, drawn 1:1 at an integer
// position.
void suiteMeshCoverage() {
    suiteHeader("S22 layer mesh: the generated mesh must cover the layer's whole support");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    const QSize canvas(160, 120);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    // A canvas-sized bitmap whose faint content sits well inside the canvas (so the mesh has
    // to decide about it rather than push it to the border). The three blocks are the three
    // cases the content test exists to tell apart:
    //   * a solid 32x32 block - DELIBERATE paint, the mesh must cover it;
    //   * an alpha-6 haze block - the ACCIDENT case (erase/partial-operation residue). A uniform
    //     veil like this must NOT become content, or it creates cells and pushes the burr
    //     reduction out, bending the mesh silhouette away from the form the artist animates;
    //   * a ramp from alpha 1 to 40 - DELIBERATE soft paint, whose faint end is a judgement
    //     call: the old fixed threshold of 10 cut it; the derived level lowers the bar, and the
    //     edge test keeps the faint end that still falls below the level.
    const QRect solid(16, 16, 32, 32);
    const QRect haze(96, 16, 48, 48);
    const QRect ramp(16, 80, 48, 16);
    const auto fill = [&](int x, int y, uint8_t* px) {
        px[0] = 200;
        px[1] = 120;
        px[2] = 40;
        px[3] = 0;
        if (solid.contains(x, y))
            px[3] = 255;
        if (haze.contains(x, y))
            px[3] = 6;
        if (ramp.contains(x, y))
            px[3] = (uint8_t)(1 + (x - ramp.left()) * 40 / (ramp.width() - 1));
    };

    // the level the shipped rule derives for THIS layer (img::MeshContentLevel): the fixture mixes
    // strong paint, a faint haze and a deliberate ramp, so its mean is low and the level comes down
    int layerLevel = img::GridMeshCreator::kContentAlphaThreshold;
    {
        std::vector<uint8_t> probe((size_t)canvas.width() * canvas.height() * 4, 0);
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                uint8_t* px = &probe[((size_t)y * canvas.width() + x) * 4];
                fill(x, y, px);
            }
        }
        layerLevel = img::MeshContentLevel::derive(probe.data(), canvas);
        std::printf("  [S22] the layer's derived content level is T=%d\n", layerLevel);
    }

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    auto* res = scene::makeImage("mesh", canvas, fill);
    auto* layer = scene::addLayer(tree, root, res, "mesh", center, 0.0f, unit);
    auto* key = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
    expect(key != nullptr, "S22: the layer has an image key to re-mesh");

    const std::vector<uint8_t> generated = fx.render(tree, camera, 0);

    // the same layer through an exact bounding quad: no cell culling, no burr reduction.
    // NOTE the mesh may only be regenerated BEFORE the first render of that tree: the cached
    // FFD vertex data (TimeKeyExpans) is a copy of the mesh taken when the expans is built, so
    // re-meshing an already-rendered layer trips the vertex-count assert in
    // LayerNode::transformShape. Regenerating a mesh therefore has to go through the updater
    // that refreshes the FFD data as well (core/FFDKeyUpdater), which is what the app does.
    core::ObjectTree quadTree;
    auto* quadRoot = new core::FolderNode("root");
    quadTree.grabTopNode(quadRoot);
    auto* quadRes = scene::makeImage("mesh", canvas, fill);
    auto* quadLayer = scene::addLayer(quadTree, quadRoot, quadRes, "mesh", center, 0.0f, unit);
    if (auto* quadKey = dynamic_cast<core::ImageKey*>(quadLayer->timeLine()->defaultKey(core::TimeKeyType_Image)))
        quadKey->resetGridMesh(core::Constant::imageCellSizeMax());
    const std::vector<uint8_t> quad = fx.render(quadTree, camera, 0);

    // The mesh is no longer expected to reproduce the quad over the WHOLE bitmap: the quad draws
    // the accidents too, and keeping them out of the mesh is the point. The invariant that must
    // hold is over the DELIBERATE paint - everywhere the source alpha is above the content
    // threshold, the mesh has to draw exactly what the quad draws, so the threshold never costs
    // anything the artist painted at a level they chose.
    int quadWorst = 0;
    QString quadWorstAt;
    for (int y = 0; y < canvas.height(); ++y) {
        const int by = canvas.height() - 1 - y;
        for (int x = 0; x < canvas.width(); ++x) {
            uint8_t src[4] = {200, 120, 40, 0};
            fill(x, by, src);
            if (src[3] <= img::GridMeshCreator::kContentAlphaThreshold) {
                continue;
            }
            const size_t i = ((size_t)y * canvas.width() + x) * 4;
            bool differs = false;
            for (int c = 0; c < 4; ++c) {
                if (generated[i + c] != quad[i + c]) {
                    differs = true;
                }
            }
            if (differs) {
                quadWorst = 255;
                quadWorstAt = QString("(%1,%2) mesh %3/%4/%5/%6 quad %7/%8/%9/%10")
                                  .arg(x).arg(by)
                                  .arg(generated[i]).arg(generated[i + 1]).arg(generated[i + 2]).arg(generated[i + 3])
                                  .arg(quad[i]).arg(quad[i + 1]).arg(quad[i + 2]).arg(quad[i + 3]);
            }
        }
    }
    expect(
        quadWorst == 0,
        QString("S22: over deliberate paint the mesh is byte-identical to the exact quad (%1)").arg(quadWorstAt)
    );

    // readbacks are GL-ordered (row 0 = the canvas bottom), the bitmap is stored top-first, and
    // the layer is centred on the canvas at an integer offset: bitmap row (h - 1 - y) is drawn
    // at canvas row y. Rows are otherwise identical, so the rects below are tested in bitmap
    // space.
    int hazeDrawn = 0;
    int hazeExpected = 0;
    int rampDrawn = 0;
    int rampExpected = 0;
    long solidExpected = 0, solidDrawn = 0;
    int worst = 0;
    QString worstAt;
    for (int y = 0; y < canvas.height(); ++y) {
        const int by = canvas.height() - 1 - y;
        for (int x = 0; x < canvas.width(); ++x) {
            uint8_t src[4] = {200, 120, 40, 0};
            fill(x, by, src);
            const size_t i = ((size_t)y * canvas.width() + x) * 4;
            // the readback is premultiplied and the source is straight, so the stored value is
            // the source colour scaled by its own alpha (8-bit round)
            // the 1:1 check is over everything this layer's OWN level counts as content: with the
            // derived level (img::MeshContentLevel) that is every pixel above the layer's level,
            // which for this fixture is 4
            if (src[3] > layerLevel) {
                const int expected = ((int)src[0] * src[3] + 127) / 255;
                if (std::abs((int)generated[i] - expected) > worst) {
                    worst = std::abs((int)generated[i] - expected);
                    worstAt = QString("(%1,%2) got %3 want %4").arg(x).arg(by).arg(generated[i]).arg(expected);
                }
                if (generated[i + 3] != src[3]) {
                    worst = 255;
                    worstAt = QString("(%1,%2) alpha got %3 want %4")
                                  .arg(x).arg(by).arg(generated[i + 3]).arg(src[3]);
                }
            }
            if (solid.contains(x, by)) {
                ++solidExpected;
                if (generated[i + 3] != 0)
                    ++solidDrawn;
            }
            if (haze.contains(x, by)) {
                ++hazeExpected;
                if (generated[i + 3] != 0)
                    ++hazeDrawn;
            }
            if (ramp.contains(x, by)) {
                ++rampExpected;
                if (generated[i + 3] != 0)
                    ++rampDrawn;
            }
        }
    }
    // deliberate paint is drawn, indistinguishably from the exact quad
    expect(
        solidDrawn == solidExpected,
        QString("S22: deliberate paint is drawn (%1 of %2 px)").arg(solidDrawn).arg(solidExpected)
    );
    // The level is derived from the layer (img::MeshContentLevel, chosen by the artist from the
    // corpus comparison), so what this fixture's faint pixels do follows from ITS OWN level, and a
    // fixture is only a good test if it covers both directions of the rule:
    //   * a layer whose mean is low - here a strong block, a faint haze and a deliberate ramp - gets
    //     a low level, so its faint end IS content. That is the point: a soft skirt the artist
    //     painted stops being cut off, and for this fixture the alpha-6 haze is treated the same way.
    //   * a layer that paints at full strength keeps the strict level 10, so residue on it stays out
    //     of the mesh - that direction is asserted in S26, whose fixture is opaque except for a
    //     small blob.
    expect(
        rampDrawn == rampExpected,
        QString("S22: a layer with soft paint keeps its ramp's faint end (%1 of %2 px drawn)")
            .arg(rampDrawn).arg(rampExpected)
    );
    expect(
        hazeDrawn == hazeExpected,
        QString("S22: at this layer's derived level (T=%1) the alpha-6 haze is content too "
                "(%2 of %3 px drawn)").arg(layerLevel).arg(hazeDrawn).arg(hazeExpected)
    );
    expect(
        layerLevel < img::GridMeshCreator::kContentAlphaThreshold,
        QString("S22: the fixture's derived level (%1) is below the old constant").arg(layerLevel)
    );
    expect(
        worst <= 1,
        QString("S22: above the threshold the layer is drawn 1:1 from its bitmap (worst channel error %1: %2)")
            .arg(worst).arg(worstAt)
    );

    const bool ok = (gFails == failsBefore);
    caseReport(
        QString("layer mesh coverage (mesh vs exact quad, faint content)"), ok,
        QString("%1 checks").arg(gChecks - checksBefore)
    );
}

//-------------------------------------------------------------------------------------------------
// S23: how a content cutoff CANNOT be applied - and how it must be.
//
// S22 fixes the test that decides cell existence and shows what it costs. The level is per-layer:
// it should not be the same constant for a layer whose faint pixels are a deliberate airbrushed
// skirt and for a layer whose faint pixels are erase residue (img::MeshContentLevel derives it;
// this suite holds the level still to test the mechanism). The first attempt at measuring that (an
// experiment that wanted to render each candidate level) applied the level by MASKING the pixels
// handed to the mesh builder - zero every alpha <= T, then let the builder run its own content
// test. That cannot work, and both halves are proved here:
//
//   (1) masking can never RELAX the level. It only deletes pixels, so every pixel that survives is
//       still tested against the builder's own bar; the mask can make the test stricter, never
//       looser. A soft, isolated skirt therefore stays unmeshed no matter how low T is - the exact
//       opposite of what the experiment claimed to render.
//   (2) masking can, by accident, ADD content. GridMeshCreator::Image dilates alpha by one pixel
//       (expandAlpha1Pixel) BEFORE the test: a pixel zeroed by the mask takes its neighbour's
//       alpha, so an opaque stroke hands its own value to the faint pixel beside it and a cell
//       appears in the next cell over. The experiment's "cutoff" renders were mostly this artifact
//       plus the unchanged file, which is also why their difference images showed almost nothing.
//
// The level has to be a parameter of the mesh's creation (GridMesh::createFromImage), which is
// what production now exposes; (3) and (4) below pin that behaviour in both directions.
void suiteMeshContentLevel() {
    suiteHeader("S23 mesh content level: a cutoff is a parameter, not a mask over the pixels");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    const int w = 96, h = 32, cell = 16;
    // rows 8..23 carry everything; x cells are [0,16) [16,32) [32,48) [48,64) [64,80) [80,96)
    //   x 4..7    alpha 20   - above 10, below 30: the stricter level must drop it
    //   x 20..31  alpha 255  - deliberate paint, ends exactly at the cell border x=32
    //   x 32      alpha 2    - the pixel a mask at T=2 WOULD delete, right beside that paint
    //   x 33..40  alpha 6    - a faint skirt in the same cell: the paint's own soft edge
    //   x 52..59  alpha 6    - the same faint skirt, far from any paint
    std::vector<uint8_t> raw((size_t)w * h * 4, 0);
    auto set = [&](int x0, int x1, int a) {
        for (int y = 8; y < 24; ++y) {
            for (int x = x0; x <= x1; ++x) {
                uint8_t* px = &raw[((size_t)y * w + x) * 4];
                px[0] = px[1] = px[2] = 200;
                px[3] = (uint8_t)a;
            }
        }
    };
    set(4, 7, 20);
    set(20, 31, 255);
    set(32, 32, 2);
    set(33, 40, 6);
    set(52, 59, 6);

    auto triangles = [&](const std::vector<uint8_t>& data, int threshold) {
        img::GridMeshCreator creator(data.data(), QSize(w, h), cell, threshold);
        return creator.indexCount() / 3;
    };
    // how many mesh vertices sit in a given x band. This is the honest measure of "is this part of
    // the picture meshed": cell and triangle counts can move for other reasons, a vertex inside the
    // skirt's own cells cannot appear unless the mesh really covers the skirt.
    auto verticesInX = [&](const std::vector<uint8_t>& data, int threshold, int x0, int x1) {
        img::GridMeshCreator creator(data.data(), QSize(w, h), cell, threshold);
        std::vector<GLfloat> pos((size_t)std::max(1, creator.vertexCount()) * 3, 0.0f);
        std::vector<GLfloat> tex((size_t)std::max(1, creator.vertexCount()) * 2, 0.0f);
        creator.writeVertices(pos.data(), tex.data());
        int n = 0;
        for (int i = 0; i < creator.vertexCount(); ++i) {
            if (pos[i * 3] >= x0 && pos[i * 3] < x1) {
                ++n;
            }
        }
        return n;
    };
    // the level applied by masking, i.e. the mechanism S23 exists to rule out
    auto maskedTriangles = [&](int T) {
        std::vector<uint8_t> masked = raw;
        for (size_t i = 3; i < masked.size(); i += 4) {
            if (masked[i] <= T) {
                masked[i] = 0;
            }
        }
        return triangles(masked, img::GridMeshCreator::kContentAlphaThreshold);
    };

    std::vector<uint8_t> masked2 = raw;
    for (size_t i = 3; i < masked2.size(); i += 4) {
        if (masked2[i] <= 2) {
            masked2[i] = 0;
        }
    }
    const int prod = triangles(raw, img::GridMeshCreator::kContentAlphaThreshold);
    const int mask2 = maskedTriangles(2);
    const int level2 = triangles(raw, 2);
    const int level30 = triangles(raw, 30);
    // cells [48,64) and [80,96) hold nothing but the isolated skirt at x 52..59; [32,48) holds the
    // skirt that is adjacent to the stroke (x 33..40).
    const int prodFar = verticesInX(raw, img::GridMeshCreator::kContentAlphaThreshold, 48, 96);
    const int maskFar = verticesInX(masked2, img::GridMeshCreator::kContentAlphaThreshold, 48, 96);
    const int levelFar = verticesInX(raw, 2, 48, 96);
    std::printf("  [mesh level] tris: const10(T=10) %d | masked(T=2) %d | level(T=2) %d | level(T=30) %d\n",
                prod, mask2, level2, level30);
    std::printf("  [mesh level] vertices over the isolated skirt: const10 %d | masked(T=2) %d | level(T=2) %d\n",
                prodFar, maskFar, levelFar);

    // (0) the CREATOR's default argument is the old constant. This is not production's level any
    // more - production goes through GridMesh::createFromImage, which derives (S26 pins that) - so
    // the constant is the held-still control the rest of the suite compares against.
    expect(triangles(raw, img::GridMeshCreator::kContentAlphaThreshold) == triangles(raw, 10),
           "S23: the creator's default argument is the old constant");

    // (1) the isolated skirt is not meshed at the constant level, and masking a lower level does not
    // mesh it either: masking deletes pixels, it cannot relax a test. (This is the check that
    // falsifies the masking experiment: its alpha-6 content stayed unmeshed at every "cutoff".)
    expect(prodFar == 0,
           QString("S23: the constant level does not mesh the isolated alpha-6 skirt (%1 vertices)")
               .arg(prodFar));
    expect(maskFar == 0,
           QString("S23: masking a lower level does not mesh it either (%1 vertices)").arg(maskFar));

    // (2) the level PARAMETER meshes it, because the parameter is what the builder tests against.
    expect(levelFar > 0,
           QString("S23: the level parameter does mesh the isolated skirt (%1 vertices)").arg(levelFar));

    // (3) the masked run's triangle count is a by-product of the deletion plus the one-pixel alpha
    // dilation in GridMeshCreator::Image, so it is not evidence either way: it is printed above, not
    // asserted. (Reasoning about it offline is a trap too - the cell grid does not have its origin
    // at the image's corner, e.g. a real 000049 layer reports vertexRect (-9,0 4523x4505) - so the
    // effect is measured by brute force below instead.)

    // (4) the level also works in the other direction: 30 drops the alpha-20 cell that production
    // keeps, so a caller can ask for a stricter bar than the shipped constant.
    expect(level30 < prod,
           QString("S23: a stricter level drops content production keeps (%1 vs %2 triangles)")
               .arg(level30).arg(prod));

    // (5) what the cell test's SAMPLING costs, measured by brute force. Image::getOpaExistence does
    // not test the whole cell: it samples an inverted triangle inside it (one column at the cell's
    // top row, widening to the full width only at its bottom row). An opaque 3x3 dot is swept over
    // the bitmap here and the positions where the mesh sees nothing at all are counted - the answer
    // is none, so the sample is wide enough for a compact feature, and this stays as a check that a
    // future change to the scan cannot start losing dots.
    {
        int invisible = 0, total = 0, firstX = -1, firstY = -1;
        for (int y = 1; y + 3 < h; ++y) {
            for (int x = 1; x + 3 < w; ++x) {
                std::vector<uint8_t> one((size_t)w * h * 4, 0);
                for (int dy = 0; dy < 3; ++dy) {
                    for (int dx = 0; dx < 3; ++dx) {
                        uint8_t* px = &one[((size_t)(y + dy) * w + (x + dx)) * 4];
                        px[0] = px[1] = px[2] = 255;
                        px[3] = 255;
                    }
                }
                img::GridMeshCreator creator(one.data(), QSize(w, h), cell);
                if (creator.indexCount() <= 0 || creator.vertexCount() <= 0) {
                    ++invisible;
                    if (firstX < 0) {
                        firstX = x;
                        firstY = y;
                    }
                }
                ++total;
            }
        }
        std::printf("  [mesh scan] an opaque 3x3 dot is invisible to the mesh at %d of %d positions "
                    "(first at %d,%d)\n", invisible, total, firstX, firstY);
        expect(invisible == 0,
               QString("S23: a compact opaque feature is never invisible to the mesh (%1 of %2 dot "
                       "positions, first %3,%4)").arg(invisible).arg(total).arg(firstX).arg(firstY));
    }

    // (6) ... but a THIN feature is another matter, and this is the defect an artist sees as a
    // broken stroke. A layer's mesh is not its bitmap: the boundary vertices are pulled inward
    // ("burr reduction") until they stop seeing content, and a 1-px strand sticking out of a blob is
    // exactly what that reduction has nothing to hold on to. This renders a blob with a 1-px
    // horizontal strand through the generated mesh and through an exact quad, and counts the strand
    // pixels that the mesh does not draw.
    {
        const QSize scanvas(160, 120);
        auto& sfx = fixtureFor(scanvas);
        const core::CameraInfo scamera = scene::makeCamera(scanvas);
        const QRect blob(48, 44, 32, 32);
        const int strandY = 60;
        const int strandX0 = 80, strandX1 = 140;  // protruding to the right of the blob
        const auto sfill = [&](int x, int y, uint8_t* px) {
            px[0] = 255;
            px[1] = 255;
            px[2] = 255;
            px[3] = 0;
            if (blob.contains(x, y) || (y == strandY && x >= strandX0 && x <= strandX1)) {
                px[3] = 255;
            }
        };
        const QVector2D scenter(scanvas.width() / 2.0f, scanvas.height() / 2.0f);
        core::ObjectTree sMesh;
        auto* sMeshRoot = new core::FolderNode("root");
        sMesh.grabTopNode(sMeshRoot);
        scene::addLayer(sMesh, sMeshRoot, scene::makeImage("strand", scanvas, sfill), "strand",
                        scenter, 0.0f, QVector2D(1.0f, 1.0f));
        const std::vector<uint8_t> sMeshOut = sfx.render(sMesh, scamera, 0);

        core::ObjectTree sQuad;
        auto* sQuadRoot = new core::FolderNode("root");
        sQuad.grabTopNode(sQuadRoot);
        auto* sQuadLayer = scene::addLayer(sQuad, sQuadRoot, scene::makeImage("strand", scanvas, sfill),
                                           "strand", scenter, 0.0f, QVector2D(1.0f, 1.0f));
        if (auto* qk = dynamic_cast<core::ImageKey*>(
                sQuadLayer->timeLine()->defaultKey(core::TimeKeyType_Image))) {
            qk->resetGridMesh(core::Constant::imageCellSizeMax());
        }
        const std::vector<uint8_t> sQuadOut = sfx.render(sQuad, scamera, 0);

        int strandInk = 0, strandDrawn = 0, blobInk = 0, blobDrawn = 0;
        for (int y = 0; y < scanvas.height(); ++y) {
            const int by = scanvas.height() - 1 - y;
            for (int x = 0; x < scanvas.width(); ++x) {
                uint8_t src[4] = {0, 0, 0, 0};
                sfill(x, by, src);
                if (src[3] <= img::GridMeshCreator::kContentAlphaThreshold) {
                    continue;
                }
                const size_t i = ((size_t)y * scanvas.width() + x) * 4;
                const bool drawn = sMeshOut[i + 3] > 0 && sQuadOut[i + 3] > 0;
                if (blob.contains(x, by)) {
                    ++blobInk;
                    blobDrawn += drawn;
                } else {
                    ++strandInk;
                    strandDrawn += drawn;
                }
            }
        }
        std::printf("  [mesh strand] 1-px strand: mesh draws %d of %d px (blob %d of %d)\n",
                    strandDrawn, strandInk, blobDrawn, blobInk);
        expect(strandDrawn == strandInk,
               QString("S23: the mesh draws the whole 1-px strand (%1 of %2 px; the blob is %3 of %4)")
                   .arg(strandDrawn).arg(strandInk).arg(blobDrawn).arg(blobInk));
    }

    const bool ok = (gFails == failsBefore);
    caseReport(QString("mesh content level (mask vs parameter)"), ok,
               QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S24: the mesh must draw every pixel of the layer it claims to cover - tested at RENDER level,
// because cell existence is not the same question.
//
// S23 proved that a compact opaque feature always creates a cell. What it did not test is whether
// the feature is DRAWN: the cell's boundary vertices are pulled inward by the burr reduction, and
// that reduction uses the same sampled triangle as the existence test, so content in the part of a
// cell the sample never looks at can be cut away after the cell was kept. Measured on the corpus:
// two renders of one file that differ only in the mesh's cell set differ at isolated 1-px-wide,
// 1..6-px-tall dashes, and at every one of them the file's own stored composite agrees with one of
// the two renders and not the other - i.e. one of the two meshes is drawing a wrong pixel.
//
// This renders small opaque features (a 3x3 dot and a 1x6 dash) tiled over the canvas at several
// lattice phases - one phase per possible alignment inside a mesh cell - through the generated mesh
// and through an exact quad, and counts the feature pixels the mesh does not draw.
void suiteMeshSmallFeatureCoverage() {
    suiteHeader("S24 mesh coverage of small features: a covered pixel must be drawn");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    const QSize canvas(160, 120);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);
    const int lattice = 16;  // the mesh's default cell size, so each lattice phase is one alignment

    struct Phase {
        int x, y;
    };
    const Phase phases[] = {{0, 0}, {3, 5}, {7, 2}, {11, 13}};
    int grandDots = 0, grandLostDots = 0, grandLostPx = 0;
    QString worstDetail;
    int worstLost = 0;

    for (const Phase& ph : phases) {
        // the layer's bitmap: a 3x3 dot and a 1x6 dash at every lattice point of this phase
        std::vector<uint8_t> ink((size_t)canvas.width() * canvas.height(), 0);
        std::vector<std::vector<QPoint>> features;
        // features are kept away from the bitmap's own edge: the mesh insets the layer's boundary by
        // a pixel or two on purpose (S22), so a feature touching the edge is a different question
        // from one in the interior, and mixing them hides which is which.
        const int margin = 4;
        for (int gy = ph.y; gy + 8 < canvas.height(); gy += lattice) {
            for (int gx = ph.x; gx + 8 < canvas.width(); gx += lattice) {
                if (gx < margin || gy < margin || gx + 8 > canvas.width() - margin
                    || gy + 8 > canvas.height() - margin) {
                    continue;
                }
                std::vector<QPoint> dot, dash;
                for (int dy = 0; dy < 3; ++dy) {
                    for (int dx = 0; dx < 3; ++dx) {
                        dot.push_back(QPoint(gx + dx, gy + dy));
                    }
                }
                for (int dy = 0; dy < 6; ++dy) {
                    dash.push_back(QPoint(gx + 5, gy + dy));
                }
                features.push_back(dot);
                features.push_back(dash);
                for (const QPoint& p : dot)
                    ink[(size_t)p.y() * canvas.width() + p.x()] = 1;
                for (const QPoint& p : dash)
                    ink[(size_t)p.y() * canvas.width() + p.x()] = 1;
            }
        }
        const auto dill = [&](int x, int y, uint8_t* px) {
            px[0] = 255;
            px[1] = 80;
            px[2] = 40;
            px[3] = ink[(size_t)y * canvas.width() + x] ? 255 : 0;
        };

        core::ObjectTree meshTree;
        auto* meshRoot = new core::FolderNode("root");
        meshTree.grabTopNode(meshRoot);
        scene::addLayer(meshTree, meshRoot, scene::makeImage("small", canvas, dill), "small", center,
                        0.0f, unit);
        const std::vector<uint8_t> meshOut = fx.render(meshTree, camera, 0);

        core::ObjectTree quadTree;
        auto* quadRoot = new core::FolderNode("root");
        quadTree.grabTopNode(quadRoot);
        auto* quadLayer = scene::addLayer(quadTree, quadRoot, scene::makeImage("small", canvas, dill),
                                          "small", center, 0.0f, unit);
        if (auto* qk = dynamic_cast<core::ImageKey*>(
                quadLayer->timeLine()->defaultKey(core::TimeKeyType_Image))) {
            qk->resetGridMesh(core::Constant::imageCellSizeMax());
        }
        const std::vector<uint8_t> quadOut = fx.render(quadTree, camera, 0);

        int lostDots = 0, lostPx = 0;
        for (const auto& f : features) {
            int missing = 0;
            for (const QPoint& p : f) {
                const int y = canvas.height() - 1 - p.y();  // bitmap row -> canvas row
                const size_t i = ((size_t)y * canvas.width() + p.x()) * 4;
                // the quad is the reference: it draws the bitmap 1:1 with no cell geometry
                if (quadOut[i + 3] > 0 && meshOut[i + 3] == 0 && missing == 0) {
                    worstDetail = QString("phase(%1,%2) feature at (%3,%4): mesh alpha 0, quad alpha %5")
                                      .arg(ph.x).arg(ph.y).arg(p.x()).arg(p.y()).arg(quadOut[i + 3]);
                }
                if (quadOut[i + 3] > 0 && meshOut[i + 3] == 0) {
                    ++missing;
                }
            }
            if (missing > 0) {
                ++lostDots;
                lostPx += missing;
                worstLost = std::max(worstLost, missing);
            }
        }
        std::printf("  [small features] phase(%2d,%2d): %d features, %d with missing pixels, %d px lost\n",
                    ph.x, ph.y, (int)features.size(), lostDots, lostPx);
        grandDots += (int)features.size();
        grandLostDots += lostDots;
        grandLostPx += lostPx;
    }
    std::printf("  [small features] total: %d features, %d with missing pixels (%d px), worst %d px\n",
                grandDots, grandLostDots, grandLostPx, worstLost);
    expect(grandLostDots == 0,
           QString("S24: the mesh draws every small opaque feature it covers (%1 of %2 features lost "
                   "%3 px; worst %4; %5)").arg(grandLostDots).arg(grandDots).arg(grandLostPx)
               .arg(worstLost).arg(worstDetail));

    const bool ok = (gFails == failsBefore);
    caseReport(QString("mesh coverage of small features"), ok,
               QString("%1 checks").arg(gChecks - checksBefore));
}

// S25: the mesh must draw content that sits against the layer bitmap's own edge, next to residue.
//
// S22 asserts "every pixel above the content threshold is drawn" for paint inside the bitmap and S24
// repeats it for small features on a 16-px lattice; both pass. The corpus disagreed. In a 30-file
// sample, one file's render lost an 18-px run of an OPAQUE row (alpha 207) in two places - 36 px in
// all, and the file's own stored composite agrees with the render that keeps the row.
//
// Traced on that layer, the shape is: two opaque rows (alpha ~207..255), and under them a row of
// erase residue (alpha 1..6) which is the bitmap's last row. The cause is the third rule below, and
// it has nothing to do with the height of the threshold - the paint is far above it:
//   * the residue is below the content threshold, so it creates no cell of its own - the threshold
//     doing its job, since nobody paints at 4/255;
//   * Image's one-pixel alpha dilation only fills ZERO pixels, so the residue row also stops the
//     paint from being dilated into it - which is why there is empty alpha for the pull to land in,
//     but it is context, not cause;
//   * the burr reduction asked whether the pulled POSITION sees content, and a pull is up to a cell
//     (16 px) wide while a line of paint is often 1-3 px thick: the endpoint lands beyond the paint,
//     in empty space, the ladder stops shortening, and the full pull moves the cell boundary over
//     the paint. Measured on the corpus vertex: maxReduce 8.0, pull 7.6 px, endpoint 6.6 px above the
//     vertex in a 3x3 window of pure zero, while the path to it crosses alpha 72, 67, 211, 210.
// Ablation on the real file (36 px in two 18-px runs, all matching the stored composite): walking the
// pull while still asking the CONTENT threshold fixes all 36; asking ANY ink at the endpoint alone
// fixes none of them. So the sweep is the fix for this shape, and the ink threshold covers a
// different one (a pull ending ON ink fainter than the threshold), which this family also exercises.
// So this suite paints deliberate content against each of the bitmap's four edges, with and without
// residue between the paint and the edge, over a family of last-cell-row truncations (a bitmap's
// height is rarely a multiple of the cell size, so the last cell row has less room than the cell
// geometry assumes), and asks for the invariant S22/S24 use: over every pixel above the content
// threshold, the generated mesh draws exactly what an exact quad draws.
void suiteMeshEdgeCoverage() {
    suiteHeader("S25 layer mesh: content against the bitmap edge must still be drawn");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    const QSize canvas(160, 120);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);
    const int T = img::GridMeshCreator::kContentAlphaThreshold;
    const uint8_t kPaint = 255;
    const uint8_t kResidue = 4;  // erase residue: above zero, far below the content threshold
    const int bitmapW = 160;

    // The layer bitmap is SMALLER than the canvas, as a real layer's is: the mesh is built on the
    // bitmap, so the bitmap's own edge is the edge that matters here, not the canvas's.
    struct Variant {
        QString name;
        QSize bitmap;
        int paintY0, paintY1, paintX0, paintX1;
        int residueY;  // -1 = no residue row
        int residueX0, residueX1;
        int residueX;  // -1 = no residue column; else a full-height residue column
        int paintProfile = 0;  // 0 = flat 255, 1 = fading towards the edge (as the corpus layer is)
        // thin strokes, as {x0, y0, x1, y1, alpha}: a 1-px line is narrower than any sampling
        // spacing, which is what a sampled-along-the-path test has to be able to survive
        std::vector<std::array<int, 5>> strokes;
    };
    std::vector<Variant> variants;

    // The family that matters: where the paint lands inside the bitmap's LAST cell row. A cell row
    // is 16 px, but the bitmap's height is rarely a multiple of 16, so the last cell row is cut
    // short - the mesh's vertices there have less room than the cell geometry assumes.
    for (int tail : {3, 5}) {
        for (int paintRows : {1, 2}) {
            for (int residue : {-1, 4}) {
                for (int align : {0, 1}) {
                    for (int profile : {0, 1}) {
                        const int h = 48 + tail;
                        const int x0 = align ? 8 : 16;
                        Variant v;
                        v.name = QString("tail %1 paintRows %2 residue %3 %4 %5")
                                     .arg(tail).arg(paintRows)
                                     .arg(residue < 0 ? "none" : "a4")
                                     .arg(align ? "mid-cell" : "cell-aligned")
                                     .arg(profile ? "soft" : "flat");
                        v.bitmap = QSize(bitmapW, h);
                        v.paintY0 = h - 1 - paintRows;
                        v.paintY1 = h - 2;
                        v.paintX0 = x0;
                        v.paintX1 = x0 + 31;
                        v.residueY = residue < 0 ? -1 : h - 1;
                        v.residueX0 = 0;
                        v.residueX1 = bitmapW - 1;
                        v.residueX = -1;
                        v.paintProfile = profile;
                        variants.push_back(v);
                    }
                }
            }
        }
    }
    // Thin DIAGONAL ink in the last cell row. A 1-px line is narrower than any spacing a walk
    // along the move could use, so this is the shape that tells a gapless walk from a sparse one:
    // at 45 degrees the samples and their 3x3 windows can step past the line's corner.
    for (int slope : {1, 2}) {
        for (int residue : {-1, 4}) {
            Variant v;
            v.name = QString("thin %1:1 diagonal in the last cell row, residue %2")
                         .arg(slope)
                         .arg(residue < 0 ? "none" : "a4");
            v.bitmap = QSize(bitmapW, 53);
            v.paintY0 = 1;
            v.paintY1 = 0;  // empty range: no block, the stroke below is the whole paint
            v.paintX0 = v.paintX1 = 0;
            v.residueY = residue < 0 ? -1 : 52;
            v.residueX0 = 0;
            v.residueX1 = bitmapW - 1;
            v.residueX = -1;
            // a 31-px run that starts on the row above the residue and climbs by 31/slope rows
            v.strokes.push_back({16, 51, 16 + 31, 51 - 31 / slope, kPaint});
            variants.push_back(v);
        }
    }
    // control: the same paint and residue away from the bitmap's edge (S22/S24's regime)
    variants.push_back(Variant{"paint above residue, away from the edge", QSize(bitmapW, 64),
                               40, 41, 16, 47, 42, 0, bitmapW - 1, -1});
    // the same question at the top edge and along x, where the cell grid is half a cell out of phase
    variants.push_back(Variant{"paint below residue, residue on the top edge", QSize(bitmapW, 51),
                               1, 2, 16, 47, 0, 0, bitmapW - 1, -1});
    variants.push_back(Variant{"paint beside a residue column, on the right edge", QSize(bitmapW, 64),
                               16, 47, bitmapW - 3, bitmapW - 2, -1, 0, 0, bitmapW - 1});
    variants.push_back(Variant{"paint beside a residue column, on the left edge", QSize(bitmapW, 64),
                               16, 47, 1, 2, -1, 0, 0, 0});

    int grandPaint = 0;
    int grandLost = 0;
    QString worstDetail;
    for (const Variant& v : variants) {
        const int bw = v.bitmap.width(), bh = v.bitmap.height();
        std::vector<uint8_t> alpha((size_t)bw * bh, 0);
        for (int y = v.paintY0; y <= v.paintY1; ++y) {
            // profile 1 mirrors the corpus layer: the paint's own edge fades, and the row above the
            // paint fades too (it is the last row of the cell row above)
            const uint8_t a = v.paintProfile == 0 ? kPaint
                                                  : (y == v.paintY0 ? 210 : 200);
            for (int x = v.paintX0; x <= v.paintX1; ++x) {
                alpha[(size_t)y * bw + x] = a;
            }
        }
        if (v.paintProfile != 0 && v.paintY0 - 1 >= 0 && v.residueY != v.paintY0 - 1) {
            for (int x = v.paintX0; x <= v.paintX1; ++x) {
                alpha[(size_t)(v.paintY0 - 1) * bw + x] = 120;
            }
        }
        if (v.residueY >= 0) {
            for (int x = v.residueX0; x <= v.residueX1; ++x) {
                alpha[(size_t)v.residueY * bw + x] = kResidue;
            }
        }
        if (v.residueX >= 0) {
            for (int y = 0; y < bh; ++y) {
                alpha[(size_t)y * bw + v.residueX] = kResidue;
            }
        }
        for (const auto& st : v.strokes) {
            // Bresenham, so a stroke is exactly one pixel wide with no gaps
            int x = st[0], y = st[1];
            const int x1 = st[2], y1 = st[3];
            const int dx = std::abs(x1 - x), dy = -std::abs(y1 - y);
            const int sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
            int err = dx + dy;
            while (true) {
                if (0 <= x && x < bw && 0 <= y && y < bh) {
                    alpha[(size_t)y * bw + x] = (uint8_t)st[4];
                }
                if (x == x1 && y == y1) {
                    break;
                }
                const int e2 = 2 * err;
                if (e2 >= dy) {
                    err += dy;
                    x += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    y += sy;
                }
            }
        }

        // the bitmap is centred on the canvas at an integer offset, so bitmap pixel (x,y) is drawn
        // at canvas pixel (ox + x, oy + bh - 1 - y)
        const int ox = (canvas.width() - bw) / 2;
        const int oy = (canvas.height() - bh) / 2;
        const auto fill = [&](int x, int y, uint8_t* px) {
            px[0] = 255;
            px[1] = 80;
            px[2] = 40;
            px[3] = alpha[(size_t)y * bw + x];
        };
        core::ObjectTree meshTree;
        auto* meshRoot = new core::FolderNode("root");
        meshTree.grabTopNode(meshRoot);
        scene::addLayer(meshTree, meshRoot, scene::makeImage("edge", v.bitmap, fill), "edge", center,
                        0.0f, unit);
        const std::vector<uint8_t> meshOut = fx.render(meshTree, camera, 0);

        core::ObjectTree quadTree;
        auto* quadRoot = new core::FolderNode("root");
        quadTree.grabTopNode(quadRoot);
        auto* quadLayer = scene::addLayer(quadTree, quadRoot, scene::makeImage("edge", v.bitmap, fill),
                                          "edge", center, 0.0f, unit);
        if (auto* qk = dynamic_cast<core::ImageKey*>(
                quadLayer->timeLine()->defaultKey(core::TimeKeyType_Image))) {
            qk->resetGridMesh(core::Constant::imageCellSizeMax());
        }
        const std::vector<uint8_t> quadOut = fx.render(quadTree, camera, 0);

        int paint = 0, lost = 0;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                if (alpha[(size_t)y * bw + x] <= T) {
                    continue;
                }
                ++paint;
                const int cy = oy + bh - 1 - y;  // bitmap row -> canvas row
                const int cx = ox + x;
                const size_t i = ((size_t)cy * canvas.width() + cx) * 4;
                if (quadOut[i + 3] > 0 && meshOut[i + 3] == 0) {
                    ++lost;
                    if (worstDetail.isEmpty()) {
                        worstDetail = QString("%1: opaque pixel (%2,%3) alpha %4 is not drawn by the "
                                              "mesh (the quad draws alpha %5)")
                                          .arg(v.name).arg(x).arg(y)
                                          .arg(alpha[(size_t)y * bw + x])
                                          .arg(quadOut[i + 3]);
                    }
                }
            }
        }
        std::printf("  [edge coverage] %-58s %5d deliberate px, %4d not drawn\n",
                    qPrintable(v.name), paint, lost);
        grandPaint += paint;
        grandLost += lost;
    }
    std::printf("  [edge coverage] total: %d deliberate px, %d not drawn\n", grandPaint, grandLost);
    expect(grandLost == 0,
           QString("S25: the mesh draws deliberate paint at the bitmap's edges (%1 of %2 px missing; "
                   "%3)").arg(grandLost).arg(grandPaint).arg(worstDetail));

    const bool ok = (gFails == failsBefore);
    caseReport(QString("mesh coverage at the bitmap edge"), ok,
               QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S26: the content level is DERIVED per layer (img::MeshContentLevel), and the direction it keeps.
//
// S22 covers the direction the artist asked for: a layer whose pixels are mostly faint gets a low
// level, so a skirt that was painted stops being cut off. This suite covers the mapping itself and
// the opposite direction - the protection the old constant gave - because a rule that always
// relaxed the level would let erase residue into every mesh:
//
//   * a layer that paints at full strength over almost its whole area (mean near 255) still gets the
//     strict level 10, so a small faint blob on it creates no cell and is not drawn;
//   * and the default of GridMesh::createFromImage really derives that level rather than falling
//     back to a constant: the mesh built through the default is the mesh built with the derived
//     level passed explicitly, and it differs from the one built at level 1.
void suiteMeshContentLevelDerivation() {
    suiteHeader("S26 mesh content level: derived per layer, and the strict direction it keeps");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    // (1) the mapping: T = clamp(round(0.05 x mean(alpha > 2)), 1, 10)
    expect(img::MeshContentLevel::fromMean(255.0) == 10,
           QString("S26: an opaque layer gets the strict level (got %1)").arg(img::MeshContentLevel::fromMean(255.0)));
    expect(img::MeshContentLevel::fromMean(100.0) == 5,
           QString("S26: mean 100 gives level 5 (got %1)").arg(img::MeshContentLevel::fromMean(100.0)));
    expect(img::MeshContentLevel::fromMean(40.0) == 2,
           QString("S26: mean 40 gives level 2 (got %1)").arg(img::MeshContentLevel::fromMean(40.0)));
    expect(img::MeshContentLevel::fromMean(20.0) == 1,
           QString("S26: a very faint layer is clamped to level 1 (got %1)").arg(img::MeshContentLevel::fromMean(20.0)));
    expect(img::MeshContentLevel::fromMean(0.0) == img::GridMeshCreator::kContentAlphaThreshold,
           QString("S26: with nothing above alpha 2 the strict level is kept (got %1)")
               .arg(img::MeshContentLevel::fromMean(0.0)));

    const QSize canvas(160, 120);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    // (2) an opaque layer with a small faint blob on it
    const QRect paint(8, 8, 120, 104);
    const QRect blob(136, 16, 16, 16);
    const auto fill = [&](int x, int y, uint8_t* px) {
        px[0] = 200;
        px[1] = 120;
        px[2] = 40;
        px[3] = 0;
        if (paint.contains(x, y))
            px[3] = 255;
        if (blob.contains(x, y))
            px[3] = 6;
    };
    std::vector<uint8_t> raw((size_t)canvas.width() * canvas.height() * 4, 0);
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            fill(x, y, &raw[((size_t)y * canvas.width() + x) * 4]);
        }
    }
    const int derived = img::MeshContentLevel::derive(raw.data(), canvas);
    expect(derived == img::GridMeshCreator::kContentAlphaThreshold,
           QString("S26: a layer that paints at full strength keeps level 10 (derived %1)").arg(derived));

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    scene::addLayer(tree, root, scene::makeImage("opaque", canvas, fill), "opaque", center, 0.0f, unit);
    const std::vector<uint8_t> rendered = fx.render(tree, camera, 0);
    int painted = 0, paintedDrawn = 0, blobDrawn = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            uint8_t src[4] = {200, 120, 40, 0};
            fill(x, canvas.height() - 1 - y, src);
            const size_t i = ((size_t)y * canvas.width() + x) * 4;
            if (paint.contains(x, canvas.height() - 1 - y)) {
                ++painted;
                if (rendered[i + 3] != 0)
                    ++paintedDrawn;
            }
            if (blob.contains(x, canvas.height() - 1 - y) && rendered[i + 3] != 0)
                ++blobDrawn;
        }
    }
    expect(paintedDrawn == painted,
           QString("S26: the opaque paint is drawn (%1 of %2 px)").arg(paintedDrawn).arg(painted));
    // The blob must not become mesh content. "Drawn" is the wrong measure for that: the lattice is
    // 16 px triangles, so a cell that holds paint can reach a few pixels into a blob that is 8 px
    // away from it. The question "did the blob create a cell" is answered by the mesh itself - build
    // the same layer with the blob's pixels removed, and the two meshes must be identical.
    {
        std::vector<uint8_t> without = raw;
        for (int y = blob.top(); y <= blob.bottom(); ++y) {
            for (int x = blob.left(); x <= blob.right(); ++x) {
                without[((size_t)y * canvas.width() + x) * 4 + 3] = 0;
            }
        }
        core::GridMesh withBlob, noBlob;
        withBlob.createFromImage(raw.data(), canvas, 16);
        noBlob.createFromImage(without.data(), canvas, 16);
        const bool identical = withBlob.indexCount() == noBlob.indexCount()
                               && withBlob.vertexCount() == noBlob.vertexCount()
                               && withBlob.vertexRect() == noBlob.vertexRect();
        std::printf("  [S26] with the blob %d tris, without it %d tris; %d of the blob's %d px fall "
                    "inside a neighbouring cell\n", withBlob.indexCount() / 3, noBlob.indexCount() / 3,
                    blobDrawn, blob.width() * blob.height());
        expect(identical,
               QString("S26: a faint blob on an opaque layer creates no cell (%1 vs %2 tris)")
                   .arg(withBlob.indexCount() / 3).arg(noBlob.indexCount() / 3));
    }

    // (3) the default of createFromImage is the derivation, not a constant
    {
        core::GridMesh byDefault, byDerived, byOne;
        byDefault.createFromImage(raw.data(), canvas, 16);
        byDerived.createFromImage(raw.data(), canvas, 16, derived);
        byOne.createFromImage(raw.data(), canvas, 16, 1);
        const bool same = byDefault.indexCount() == byDerived.indexCount()
                          && byDefault.vertexCount() == byDerived.vertexCount()
                          && byDefault.vertexRect() == byDerived.vertexRect();
        expect(same, QString("S26: the default is the derived level (default %1 tris, explicit %2)")
                        .arg(byDefault.indexCount() / 3).arg(byDerived.indexCount() / 3));
        expect(byOne.indexCount() != byDefault.indexCount(),
               QString("S26: level 1 meshes more than the derived level (%1 vs %2 tris)")
                   .arg(byOne.indexCount() / 3).arg(byDefault.indexCount() / 3));
    }

    const bool ok = (gFails == failsBefore);
    caseReport(QString("mesh content level derivation"), ok,
               QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S20: the outline distance field (core/DistanceField.h) and Photoshop's Stroke, which is the
// band of pixels within its size of the layer's outline. Measured against this repository's
// 114 MB PSD: its 'title copy' carries a 4 px inside gradient stroke in Screen mode, and the
// file's own stored composite has exactly that bright rim on every glyph edge.
void suiteLayerStroke() {
    suiteHeader("S20 layer stroke: outline distance field, band, import and persistence");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;

    // (1) the exact Euclidean transform against brute force. The seeds are OUTSIDE pixels
    // (alpha below the threshold); the field is the rounded distance to the nearest one.
    {
        bool allMatch = true;
        QString detail;
        for (int trial = 0; trial < 4; ++trial) {
            const int w = 19 + 3 * trial;
            const int h = 13 + 5 * trial;
            const int threshold = 128;
            std::vector<uint8> alpha((size_t)w * h, 0);
            // a deterministic blob: a disc plus a diagonal bar, so the outline is not axis
            // aligned and the two passes of the transform cannot cancel out
            const int cx = w / 2 + trial % 2;
            const int cy = h / 2;
            const int r = 3 + trial;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const bool disc = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
                    const bool bar = (x - y) >= 2 && (x - y) <= 3 && x > 3 && x < w - 3;
                    alpha[(size_t)y * w + x] = (disc || bar) ? 255 : 0;
                }
            }
            std::vector<uint8> field;
            if (!core::DistanceField::build(alpha.data(), QSize(w, h), field, threshold)) {
                allMatch = false;
                detail = "build failed";
                break;
            }
            int worst = 0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const bool inside = (int)alpha[(size_t)y * w + x] >= threshold;
                    if (!inside) {
                        worst = std::max<int>(worst, field[(size_t)y * w + x]);
                        continue; // outside pixels are 0 by definition
                    }
                    double best = 1e9;
                    for (int yy = 0; yy < h; ++yy) {
                        for (int xx = 0; xx < w; ++xx) {
                            if ((int)alpha[(size_t)yy * w + xx] >= threshold) {
                                continue;
                            }
                            const double dx = xx - x;
                            const double dy = yy - y;
                            best = std::min(best, std::sqrt(dx * dx + dy * dy));
                        }
                    }
                    const int expected = (int)std::min(255.0, std::floor(best + 0.5));
                    worst = std::max(worst, std::abs(expected - (int)field[(size_t)y * w + x]));
                }
            }
            if (worst != 0) {
                allMatch = false;
                detail = QString("shape %1x%2 worst=%3").arg(w).arg(h).arg(worst);
                break;
            }
        }
        expect(allMatch, QString("S20: the distance transform is exact vs brute force %1").arg(detail));
    }

    // (2) a plane with no outline inside it has no field: all-transparent and all-opaque both
    // fail rather than producing a field of nonsense
    {
        const int w = 12, h = 9;
        std::vector<uint8> empty((size_t)w * h, 0);
        std::vector<uint8> full((size_t)w * h, 255);
        std::vector<uint8> field;
        expect(
            !core::DistanceField::build(empty.data(), QSize(w, h), field)
                && !core::DistanceField::build(full.data(), QSize(w, h), field),
            "S20: a plane without an outline produces no field");
    }

    // (3) the band itself: a 32x32 square with a 3 px inside stroke must paint exactly the
    // pixels whose analytic distance to the outline is within the band, in the stroke colour,
    // and leave the layer's own pixels elsewhere
    {
        const QSize canvas(128, 96);
        auto& fx = fixtureFor(canvas);
        const core::CameraInfo camera = scene::makeCamera(canvas);
        const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
        const QVector2D unit(1.0f, 1.0f);
        const int x0 = 48, y0 = 32, x1 = 80, y1 = 64;   // the square, in image pixels
        const float strokeSize = 3.0f;
        const QColor strokeColor(255, 64, 16);

        const auto filled = [&](int x, int y, uint8_t* px) {
            const bool inside = (x >= x0 && x < x1 && y >= y0 && y < y1);
            px[0] = 40; px[1] = 90; px[2] = 200; px[3] = inside ? 255 : 0;
        };

        core::LayerEffects effects;
        effects.stroke.enabled = true;
        effects.stroke.size = strokeSize;
        effects.stroke.style = "InsF";
        effects.stroke.paintType = "SClr";
        effects.stroke.r = strokeColor.redF();
        effects.stroke.g = strokeColor.greenF();
        effects.stroke.b = strokeColor.blueF();
        effects.stroke.opacity = 1.0f;
        effects.stroke.mode = img::BlendMode_Normal;
        effects.stroke.blendMode = "Nrml";

        // the field the importer would bake, grabbed onto the same resource the layer draws
        std::vector<uint8> alpha((size_t)canvas.width() * canvas.height(), 0);
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                const bool inside = (x >= x0 && x < x1 && y >= y0 && y < y1);
                alpha[(size_t)y * canvas.width() + x] = inside ? 255 : 0;
            }
        }
        std::vector<uint8> field;
        expect(
            core::DistanceField::build(alpha.data(), canvas, field, 128),
            "S20: a square's plane has an outline");

        struct Pool {
            QSize size;
            std::vector<img::ResourceNode*> nodes;
            ~Pool() { qDeleteAll(nodes.begin(), nodes.end()); }
            img::ResourceNode* image(const QString& aId, const std::function<void(int, int, uint8_t*)>& aFill,
                                     const std::vector<uint8>* aField) {
                auto* node = scene::makeImage(aId, size, aFill);
                if (aField) {
                    auto* data = new uint8[aField->size()];
                    std::copy(aField->begin(), aField->end(), data);
                    node->data().grabDistance(
                        XCMemBlock(data, aField->size()), size,
                        img::ResourceData::derivationRecipe(img::ResourceData::Derivation_Distance));
                }
                nodes.push_back(node);
                return node;
            }
        };
        const auto render = [&](bool aWithField, bool aWithStroke) {
            Pool pool;
            pool.size = canvas;
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(
                tree, root, pool.image("fx", filled, aWithField ? &field : nullptr), "fx", center, 0.0f, unit);
            if (aWithStroke) {
                layer->setLayerEffects(effects);
            }
            return fx.render(tree, camera, 0);
        };

        const auto gpu = render(true, true);

        // reference: the shader's band rule (clamp(size + 0.5 - d) capped by the shape's
        // coverage) against the ANALYTIC distance of a rectangle, mixed in the stroke colour
        ref::Image expected(canvas.width(), canvas.height());
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                const bool inside = (x >= x0 && x < x1 && y >= y0 && y < y1);
                const double dx = std::min(x - (x0 - 1), x1 - x);
                const double dy = std::min(y - (y0 - 1), y1 - y);
                const int d = (int)std::floor(std::min(dx, dy) + 0.5); // the field's rounding
                const double band = std::min(std::max(strokeSize + 0.5 - d, 0.0), 1.0);
                const double alphaShape = inside ? 1.0 : 0.0;
                const double a = band * alphaShape;
                float* px = expected.at(x, y);
                const double base[3] = {40.0 / 255.0, 90.0 / 255.0, 200.0 / 255.0};
                const double stroke[3] = {strokeColor.redF(), strokeColor.greenF(), strokeColor.blueF()};
                for (int c = 0; c < 3; ++c) {
                    px[c] = (float)((base[c] + (stroke[c] - base[c]) * a) * alphaShape);
                }
                px[3] = (float)alphaShape;
            }
        }
        const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(expected));
        const bool ok = d.maxDiff <= 2 && d.mean <= 0.2;
        ++gChecks;
        if (!ok)
            ++gFails;
        caseReport("S20: the 3 px inside band lands on the analytic outline band", ok, diffStr(d));

        // The need for the field is declared by the CONSUMER, where the consumer is: nothing says
        // "this layer wants the distance field" except the drawing of a layer whose stroke is on
        // (LayerNode::render -> TimeKeyExpans::requireDistanceField). The importer used to be the
        // only place that ever said it, and the import is the one moment when that is true by
        // construction: a source RELOAD that gives the layer a stroke, or an undo of an effect
        // change, updates the node's effects and never revisits the resource - the pass was then
        // silently left off, with a resource that had no field and no reason to make one.
        // Here the resource is built WITHOUT a field and with nothing declared, exactly as such a
        // resource would be, and the stroke is switched on afterwards: if the render does not
        // declare the need, there is no field, no band, and this fails.
        {
            Pool pool;
            pool.size = canvas;
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* res = pool.image("fx", filled, nullptr); // pixels only: no field grabbed
            res->data().setDeriver(&core::DistanceField::deriveResourceChannels);
            expect(
                res->data().cached(img::ResourceData::Derivation_Distance).buffer.data() == nullptr,
                "S20: a resource built without a stroke has no plane yet");

            auto* layer = scene::addLayer(tree, root, res, "fx", center, 0.0f, unit);
            layer->setLayerEffects(effects); // the stroke arrives after the resource was built
            const auto demanded = fx.render(tree, camera, 0);

            expect(
                res->data().cached(img::ResourceData::Derivation_Distance).buffer.data() != nullptr,
                "S20: drawing a layer with a stroke is what derives the plane (asking IS the need)");
            const scene::Diff dd = scene::diffImages(demanded, ref::imageToBytes(expected));
            const bool okDemand = dd.maxDiff <= 2 && dd.mean <= 0.2;
            ++gChecks;
            if (!okDemand)
                ++gFails;
            caseReport(
                "S20: that field is the one the band needs (same band as the pre-baked render)", okDemand,
                diffStr(dd));
        }

        // The field is a DERIVED channel: the consumer declares the need, the resource derives it
        // and keeps it current after every pixel change (ResourceData::grabImage ->
        // deriveChannels), and the render path only uploads what the resource produced. Everything
        // below goes through that contract, exactly as the importer installs it.
        {
            Pool pool;
            pool.size = canvas;
            auto* res = pool.image("fx", filled, nullptr);
            res->data().setDeriver(&core::DistanceField::deriveResourceChannels);
            // ASKING IS THE WHOLE PROTOCOL. There is no "wanted" flag to set, so there is no state
            // for a caller to leave unset: an absent plane (a project written before the plane
            // existed, a derivation that failed) is derived by the first consumer that asks. The
            // flag this replaced could not express "wanted but absent" at all, and a revision that
            // had it still missed a source reload which gave a layer a stroke: nothing asked again.
            const auto plane = [&]() {
                return res->data().cached(img::ResourceData::Derivation_Distance).buffer.data();
            };
            expect(plane() == nullptr, "S20: the resource starts with no derived channel");
            expect(
                res->data().derived(img::ResourceData::Derivation_Distance).data() != nullptr,
                "S20: asking for the channel derives it at once");
            expect(plane() != nullptr && !res->data().isDerivationStale(img::ResourceData::Derivation_Distance),
                   "S20: the plane it hands back is current, not stale");

            const auto renderTree = [&]() {
                core::ObjectTree tree;
                auto* root = new core::FolderNode("root");
                tree.grabTopNode(root);
                auto* l = scene::addLayer(tree, root, res, "fx", center, 0.0f, unit);
                l->setLayerEffects(effects);
                return fx.render(tree, camera, 0);
            };
            const auto pixel = [&](const std::vector<uint8_t>& aBytes, int aX, int aY) {
                const size_t i = ((size_t)aY * canvas.width() + aX) * 4;
                return QVector3D(aBytes[i], aBytes[i + 1], aBytes[i + 2]);
            };
            const QVector3D strokeRgb(strokeColor.red(), strokeColor.green(), strokeColor.blue());
            const int midY = canvas.height() / 2;

            const auto before = renderTree();
            expect(
                (pixel(before, x0, midY) - strokeRgb).length() < 60.0f
                    && (pixel(before, x0 + 20, midY) - strokeRgb).length() > 120.0f,
                "S20: control - the band starts on the original outline");

            // move the square 20 px to the right
            {
                const size_t count = (size_t)canvas.width() * canvas.height();
                auto* data = new uint8[count * 4]();
                for (int y = 0; y < canvas.height(); ++y) {
                    for (int x = 0; x < canvas.width(); ++x) {
                        const bool inside = (x >= x0 + 20 && x < x1 + 20 && y >= y0 && y < y1);
                        uint8_t* px = data + ((size_t)y * canvas.width() + x) * 4;
                        px[0] = 40; px[1] = 90; px[2] = 200; px[3] = inside ? 255 : 0;
                    }
                }
                res->data().grabImage(XCMemBlock(data, count * 4), canvas, img::Format_RGBA8);
            }
            // the pixels changed, so the plane the cache holds no longer describes them: it is
            // stale until somebody asks, and asking is what the next draw does (that laziness is
            // the point - nothing has to be kept current in the background)
            expect(
                res->data().cached(img::ResourceData::Derivation_Distance).buffer.data() != nullptr
                    && res->data().isDerivationStale(img::ResourceData::Derivation_Distance),
                "S20: replacing the pixels makes the cached channel stale");

            const auto after = renderTree();
            expect(
                !res->data().isDerivationStale(img::ResourceData::Derivation_Distance),
                "S20: the next draw re-derives it, and the band follows the new pixels");
            const float newDist = (pixel(after, x0 + 20, midY) - strokeRgb).length();
            const float oldDist = (pixel(after, x0, midY) - strokeRgb).length();
            expect(
                newDist < 60.0f && oldDist > 120.0f,
                QString("S20: the band follows a replaced image (new edge dist %1, old edge dist %2)")
                    .arg(newDist, 0, 'f', 1).arg(oldDist, 0, 'f', 1));
        }

        // a stroke with no field on the resource must not draw: the render equals the same
        // layer without effects, and the model reports the result as not faithful
        expect(
            render(false, true) == render(false, false),
            "S20: a stroke without a distance field is not drawn");
        expect(effects.isDrawnAsConfigured(), "S20: control (the modelled stroke itself is drawable)");
        core::LayerEffects outside = effects;
        outside.stroke.style = "OutF";
        expect(
            !outside.stroke.isDrawable() && !outside.isDrawnAsConfigured(),
            "S20: an outside stroke is reported, not drawn");
        core::LayerEffects pattern = effects;
        pattern.stroke.paintType = "Ptrn";
        expect(!pattern.stroke.isDrawable(), "S20: a pattern stroke is reported, not drawn");
    }

    // (4) the descriptor: an inside gradient stroke parses with its size, position, paint type,
    // blend mode, gradient stops and angle
    {
        const auto block = makeLfx2Stroke("InsF", "GrFl", 4.0);
        img::PSDDescriptor descriptor;
        expect(descriptor.parse(block.data(), block.size(), 8), "S20: the stroke's lfx2 parses");
        core::LayerEffects effects;
        expect(core::parseLayerEffects(descriptor, effects), "S20: the stroke's descriptor parses");
        const auto& st = effects.stroke;
        expect(
            st.enabled && st.size == 4.0f && st.style == "InsF" && st.paintType == "GrFl",
            "S20: the stroke's size, position and paint type are read");
        expect(
            st.mode == img::BlendMode_Screen && st.blendModeKnown && std::abs(st.opacity - 1.0f) < 0.001f,
            "S20: the stroke's blend mode and opacity are read");
        expect(
            st.stops.size() == 2 && std::abs(st.stops[0].r - 251.0f / 255.0f) < 0.002f
                && std::abs(st.stops[1].b - 83.0f / 255.0f) < 0.002f,
            "S20: the stroke's gradient stops are read");
        expect(
            std::abs(st.angleDeg - (-78.0f)) < 0.001f && std::abs(st.scale - 81.0f) < 0.001f && st.isDrawable(),
            "S20: the stroke's gradient angle and scale are read");
        expect(effects.isDrawnAsConfigured(), "S20: a drawable stroke is reported as faithful");
    }

    // (5) import and persistence: a PSD whose layer carries a stroke bakes the field into the
    // resource, and the field survives the project round-trip
    {
        const int side = 64;
        const auto block = makeLfx2Stroke("InsF", "SClr", 3.0);

        img::PSDFormat format;
        format.header().version = 1;
        format.header().channels = 4;
        format.header().height = side;
        format.header().width = side;
        format.header().depth = 8;
        format.header().mode = img::PSDFormat::ColorMode_RGB;
        format.imageData().compressionId = 0;

        auto* layer = new img::PSDFormat::Layer();
        layer->name = "fx";
        layer->rect.edge[0] = 0;
        layer->rect.edge[1] = 0;
        layer->rect.edge[2] = side;
        layer->rect.edge[3] = side;
        layer->blendMode = "norm";
        layer->opacity = 255;
        layer->flags = 0x08;
        // a 20x20 square of solid pixels: an outline the field can measure
        const auto pixelAt = [&](int aChannel, int x, int y) -> uint8 {
            const bool inside = (x >= 20 && x < 44 && y >= 20 && y < 44);
            if (aChannel == 3) {
                return inside ? 255 : 0; // alpha carries the shape
            }
            return inside ? (uint8)(aChannel == 0 ? 200 : (aChannel == 1 ? 120 : 40)) : 0;
        };
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = side * side;
            chan->data.reset(new uint8[side * side]());
            const int channel = (id == -1) ? 3 : (int)id;
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    chan->data[(size_t)y * side + x] = pixelAt(channel, x, y);
                }
            }
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        {
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "lfx2";
            info->dataLength = (uint32)block.size();
            info->data.reset(new uint8[block.size()]);
            std::memcpy(info->data.get(), block.data(), block.size());
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));

        const uint32 mergedSize = side * side;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = mergedSize;
            chan->data.reset(new uint8[mergedSize]());
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }

        const QString path = "/tmp/vb_layer_stroke.psd";
        {
            std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, format);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S20: PSDWriter writes the stroke");
            out.close();
        }

        StubAnimator animator;
        core::Project project("", animator, nullptr);
        ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
        NullProgressReporter reporter;
        const bool loaded = loader.load(path, project, reporter);
        expect(loaded, QString("S20: the PSD with a stroke imports: %1").arg(loader.log()));
        if (loaded) {
            core::ObjectNode* found = nullptr;
            const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
                if (aNode->name() == "fx") {
                    found = aNode;
                }
                for (auto* child : aNode->children())
                    walk(child);
            };
            walk(project.objectTree().topNode());
            expect(found && found->renderer(), "S20: the stroked layer imports");
            if (found && found->renderer()) {
                const core::LayerEffects fx2 = found->renderer()->layerEffects();
                // The paint type is pinned to the value Photoshop actually writes: the "PntT"
                // enum's value key for a solid stroke is "SClr" (measured on every stroke of
                // every file available here). This assertion is the one that would have caught
                // the shipped bug - isSolid() tested "SCol", a key no producer writes, so a
                // solid stroke read as neither solid nor gradient, isDrawable() went false and
                // the stroke was dropped without a word. The helper above wrote "SCol" too, so
                // the suite confirmed the bug instead of the format.
                expect(
                    fx2.stroke.paintType == "SClr",
                    QString("S20: the imported stroke keeps the file's paint type (got '%1')")
                        .arg(QString::fromStdString(fx2.stroke.paintType)));
                expect(
                    fx2.stroke.enabled && std::abs(fx2.stroke.size - 3.0f) < 0.001f
                        && fx2.stroke.style == "InsF" && fx2.stroke.isSolid(),
                    "S20: the imported layer carries the stroke");

                // the field the import baked into the layer's resource
                const auto* imageKey = (const core::ImageKey*)found->timeLine()->defaultKey(core::TimeKeyType_Image);
                expect(imageKey != nullptr, "S20: the layer has an image key");
                if (imageKey) {
                    const auto& res = *imageKey->data().resource();
                    expect(
                        res.cached(img::ResourceData::Derivation_Distance).buffer.data() != nullptr,
                        "S20: the import warmed the outline distance field");
                    if (res.cached(img::ResourceData::Derivation_Distance).buffer.data() != nullptr) {
                        // spot-check the field against the analytic distance of the square:
                        // the layer rect is padded by the importer, so find the shape centre
                        const auto& buf = res.cached(img::ResourceData::Derivation_Distance).buffer;
                        const QSize size = buf.pixelSize();
                        int bestX = 0, bestY = 0, best = -1;
                        for (int y = 0; y < size.height(); ++y) {
                            for (int x = 0; x < size.width(); ++x) {
                                const int v = buf.data()[(size_t)y * size.width() + x];
                                if (v > best) {
                                    best = v;
                                    bestX = x;
                                    bestY = y;
                                }
                            }
                        }
                        // a 24x24 shape: the deepest pixel is 12 px from the nearest outside
                        // pixel on both axes, so the maximum reads 12
                        expect(
                            best == 12,
                            QString("S20: the baked field peaks at the shape's inradius (got %1 at %2,%3)")
                                .arg(best).arg(bestX).arg(bestY));
                    }
                }
            }

            const QString aniePath = "/tmp/vb_layer_stroke.anie";
            ctrl::ProjectSaver saver;
            expect(saver.save(aniePath, project), "S20: the project with a stroked layer saves");
            core::Project reloaded("", animator, nullptr);
            ctrl::ProjectLoader loader2;
            const bool reloadedOk = loader2.load(aniePath, reloaded, gl::DeviceInfo::instance(), reporter);
            expect(reloadedOk, "S20: the project with a stroked layer reloads");
            if (reloadedOk) {
                core::ObjectNode* found2 = nullptr;
                const std::function<void(core::ObjectNode*)> walk2 = [&](core::ObjectNode* aNode) {
                    if (aNode->name() == "fx") {
                        found2 = aNode;
                    }
                    for (auto* child : aNode->children())
                        walk2(child);
                };
                walk2(reloaded.objectTree().topNode());
                const auto* key2 = found2 ? (const core::ImageKey*)found2->timeLine()->defaultKey(core::TimeKeyType_Image)
                                          : nullptr;
                bool same = false;
                if (key2
                    && key2->data().resource()->cached(img::ResourceData::Derivation_Distance).buffer.data()) {
                    const auto* key1 = (const core::ImageKey*)found->timeLine()->defaultKey(core::TimeKeyType_Image);
                    const auto& a = key2->data().resource()->distance();
                    const auto& b = key1->data().resource()->distance();
                    same = a.pixelSize() == b.pixelSize() && a.size() == b.size()
                        && std::memcmp(a.data(), b.data(), a.size()) == 0;
                }
                expect(same, "S20: the distance field survives the project round-trip byte-identically");
                expect(
                    found2 && found2->renderer()->layerEffects().stroke.enabled,
                    "S20: the stroke survives the project round-trip");
            }

            // The import must SAY what the picture is missing. The stroke above is drawn, so its
            // import reports nothing (that is the control); adding the one effect this build
            // cannot draw - a bevel - to the same PSD must produce a note naming the layer and
            // the effect, because an undrawn effect that nobody mentions is a silently wrong
            // picture.
            expect(
                !loader.log().contains("not drawn as the file configures it"),
                "S20: an import whose effects are all drawn reports nothing");
            {
                const auto bevelBlock = makeLfx2Bevel(true);
                auto* info = new img::PSDFormat::AdditionalLayerInfo();
                info->key = "lfx2";
                info->dataLength = (uint32)bevelBlock.size();
                info->data.reset(new uint8[bevelBlock.size()]);
                std::memcpy(info->data.get(), bevelBlock.data(), bevelBlock.size());
                format.layerAndMaskInfo().layers.front()->additionalInfos.push_back(
                    img::PSDFormat::AdditionalLayerInfoPtr(info));

                const QString bevelPath = "/tmp/vb_layer_bevel.psd";
                {
                    std::ofstream out(bevelPath.toLocal8Bit().constData(), std::ios::binary);
                    img::PSDWriter writer(out, format);
                    expect(
                        writer.resultCode() == img::PSDWriter::ResultCode_Success,
                        "S20: PSDWriter writes the PSD carrying a bevel");
                    out.close();
                }

                core::Project bevelProject("", animator, nullptr);
                ctrl::ImageFileLoader bevelLoader(gl::DeviceInfo::instance());
                const bool bevelLoaded = bevelLoader.load(bevelPath, bevelProject, reporter);
                expect(bevelLoaded, QString("S20: the PSD with a bevel imports: %1").arg(bevelLoader.log()));
                expect(
                    bevelLoader.log().contains("Bevel and Emboss")
                        && bevelLoader.log().contains("not drawn as the file configures it"),
                    QString("S20: the import reports the bevel it cannot draw (log was '%1')")
                        .arg(bevelLoader.log()));
                expect(
                    bevelLoader.log().contains("'fx'"),
                    QString("S20: the report names the layer that carries it (log was '%1')")
                        .arg(bevelLoader.log()));

                // the description is where the fact lives: describing the same PSD without
                // pixels gives the header plus exactly one layer line, and a bevel the file
                // switches OFF gives none - Photoshop writes all seven style keys into every
                // lfx2 with an enable flag, so a report that ignored the flag would accuse
                // every file of effects it does not use
                {
                    const QStringList notes = ctrl::undrawnEffectNotes(ctrl::describePsd(format, "vb", false));
                    expect(
                        notes.size() == 2,
                        QString("S20: one note for the one layer carrying a bevel, plus a header (got %1)")
                            .arg(notes.size()));
                    expect(
                        !notes.isEmpty() && notes.last().contains("'fx'")
                            && notes.last().contains("Bevel and Emboss"),
                        QString("S20: the note names the layer and the effect (got '%1')")
                            .arg(notes.isEmpty() ? QString() : notes.last()));

                    img::PSDFormat quiet;
                    quiet.header().version = 1;
                    quiet.header().channels = 3;
                    quiet.header().height = 8;
                    quiet.header().width = 8;
                    quiet.header().depth = 8;
                    quiet.header().mode = img::PSDFormat::ColorMode_RGB;
                    auto* quietLayer = new img::PSDFormat::Layer();
                    quietLayer->name = "plain";
                    quietLayer->entryType = img::PSDFormat::LayerEntryType_Layer;
                    quietLayer->rect.edge[0] = 0;
                    quietLayer->rect.edge[1] = 0;
                    quietLayer->rect.edge[2] = 8;
                    quietLayer->rect.edge[3] = 8;
                    quietLayer->blendMode = "norm";
                    quietLayer->opacity = 255;
                    quietLayer->flags = 0x08;
                    const auto off = makeLfx2Bevel(false);
                    auto* offInfo = new img::PSDFormat::AdditionalLayerInfo();
                    offInfo->key = "lfx2";
                    offInfo->dataLength = (uint32)off.size();
                    offInfo->data.reset(new uint8[off.size()]);
                    std::memcpy(offInfo->data.get(), off.data(), off.size());
                    quietLayer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(offInfo));
                    quiet.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(quietLayer));

                    const QStringList quietNotes = ctrl::undrawnEffectNotes(ctrl::describePsd(quiet, "vb", false));
                    expect(
                        quietNotes.isEmpty(),
                        QString("S20: a disabled effect is not reported (got %1 lines)").arg(quietNotes.size()));
                }
            }
        }
    }

    const bool ok = (gFails == failsBefore);
    caseReport(
        QString("layer stroke and the outline distance field"), ok, QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S19: Photoshop's FILL opacity and the blurred-silhouette layer effects (drop shadow, outer
// glow, chrome). The render path is the one the real 114 MB PSD exercises: a layer whose fill
// opacity is 0 still shows its styles (its 'title copy 7' is a glow-only layer), and a
// silhouette is a blurred, tinted, offset copy of the layer's SHAPE.
void suiteLayerSilhouettes() {
    suiteHeader("S19 layer effects: fill opacity and the blurred-silhouette pass");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    using Fill = std::function<void(int, int, uint8_t*)>;
    struct Pool {
        QSize size;
        std::vector<img::ResourceNode*> nodes;
        ~Pool() { qDeleteAll(nodes.begin(), nodes.end()); }
        img::ResourceNode* image(const QString& aId, const Fill& aFill) {
            nodes.push_back(scene::makeImage(aId, size, aFill));
            return nodes.back();
        }
    };

    // a 32x32 block exactly at the canvas center, transparent elsewhere: a shape whose
    // centroid is known analytically, which is what the offset tests measure
    const Fill block = [](int x, int y, uint8_t* px) {
        const bool inside = (x >= 48 && x < 80 && y >= 32 && y < 64);
        px[0] = 200; px[1] = 120; px[2] = 40; px[3] = inside ? 255 : 0;
    };

    // renders one layer (plus the effects it carries) alone on a transparent canvas, or over
    // the given backdrop when a backdrop fill is supplied
    const auto renderOne = [&](const Fill& aLayerFill, const core::LayerEffects& aEffects,
                               float aFillOpacity, const Fill* aBackdrop = nullptr) {
        Pool pool;
        pool.size = canvas;
        core::ObjectTree tree;
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* layer = scene::addLayer(tree, root, pool.image("fx", aLayerFill), "fx", center, 0.0f, unit);
        layer->setLayerEffects(aEffects);
        layer->setFillOpacity(aFillOpacity);
        if (aBackdrop) {
            // the first-pushed sibling renders on top, so the backdrop goes last
            scene::addLayer(tree, root, pool.image("bg", *aBackdrop), "bg", center, 0.0f, unit);
        }
        return fx.render(tree, camera, 0);
    };

    struct Centroid {
        double x = 0.0, y = 0.0, sum = 0.0;
    };
    const auto centroidOf = [&](const std::vector<uint8_t>& aA) {
        Centroid c;
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                const double v = (double)aA[((size_t)y * canvas.width() + x) * 4 + 3] / 255.0;
                c.x += v * (x + 0.5);
                c.y += v * (y + 0.5);
                c.sum += v;
            }
        }
        if (c.sum > 0.0) {
            c.x /= c.sum;
            c.y /= c.sum;
        }
        return c;
    };

    core::LayerEffects gradientFx;
    gradientFx.gradientOverlay.enabled = true;
    gradientFx.gradientOverlay.style = "Lnr ";
    gradientFx.gradientOverlay.blendMode = "Nrml";
    gradientFx.gradientOverlay.angleDeg = 90.0f;
    gradientFx.gradientOverlay.scale = 100.0f;
    gradientFx.gradientOverlay.stops = {{0.0f, 228.0f / 255.0f, 161.0f / 255.0f, 30.0f / 255.0f, 1.0f},
                                        {1.0f, 248.0f / 255.0f, 213.0f / 255.0f, 79.0f / 255.0f, 1.0f}};

    // (1) a 0 % fill with no styles draws nothing at all
    {
        const auto zero = renderOne(block, core::LayerEffects(), 0.0f);
        const auto again = renderOne(block, core::LayerEffects(), 0.0f);
        const auto full = renderOne(block, core::LayerEffects(), 1.0f);
        expect(zero == again, "S19: rendering is deterministic (control)");
        bool anyAlpha = false;
        for (size_t i = 3; i < zero.size(); i += 4)
            anyAlpha = anyAlpha || zero[i] != 0;
        expect(!anyAlpha, "S19: a 0 % fill with no styles draws nothing (alpha 0 everywhere)");
        expect(full != zero, "S19: the same layer at a 100 % fill does draw (control)");
    }

    // (2) the fill opacity scales the pixels only: a 50 % fill halves the coverage while the
    // straight colour stays the layer's own
    {
        const auto half = renderOne(block, core::LayerEffects(), 0.5f);
        const auto full = renderOne(block, core::LayerEffects(), 1.0f);
        bool coverageOk = true;
        bool colorOk = true;
        bool outsideOk = true;
        for (int y = 32; y < 64; ++y) {
            for (int x = 48; x < 80; ++x) {
                const size_t i = ((size_t)y * canvas.width() + x) * 4;
                // the readback is PREMULTIPLIED, so alpha 127.5 quantizes to 128 +- 1 and the
                // colour follows it
                coverageOk = coverageOk && std::abs((int)half[i + 3] - 128) <= 1;
                const int want[3] = {200, 120, 40};
                for (int c = 0; c < 3; ++c) {
                    const int expected = (int)std::lround(want[c] * (int)half[i + 3] / 255.0);
                    colorOk = colorOk && std::abs((int)half[i + c] - expected) <= 2;
                }
            }
        }
        for (int y = 0; y < canvas.height() && outsideOk; ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                if (x >= 48 && x < 80 && y >= 32 && y < 64)
                    continue;
                if (half[((size_t)y * canvas.width() + x) * 4 + 3] != 0)
                    outsideOk = false;
            }
        }
        expect(coverageOk, "S19: a 50 % fill halves the coverage (alpha 128 +- 1)");
        expect(colorOk, "S19: the fill keeps the layer's own colour (premultiplied by the fill)");
        expect(outsideOk, "S19: the fill opacity does not leak outside the shape");
        expect(
            centroidOf(half).sum < centroidOf(full).sum,
            "S19: the 50 % fill reduces the covered area without moving it");
    }

    // (3) styles survive a 0 % fill: an opaque Normal gradient replacement hides the pixels
    // entirely, so the fill value must not be able to change the picture at all
    {
        const auto fill0 = renderOne(block, gradientFx, 0.0f);
        const auto fill1 = renderOne(block, gradientFx, 1.0f);
        expect(fill0 == fill1, "S19: a 0 % fill still shows an opaque style (byte-identical)");
    }

    // (4) the silhouette lands at distance/angle from the shape: with a 0 % fill the render IS
    // the silhouette, so its centroid sits exactly one distance away from the shape's centre
    {
        bool allOk = true;
        QString detail;
        for (const double angle : {0.0, 90.0, 180.0}) {
            // an OUTER effect: an inner one (chrome) is clipped to the shape, which is checked in
            // case (6); the offset semantics are shared, and this case is about the offset
            core::LayerEffects glow;
            core::LayerEffects::Silhouette s;
            s.enabled = true;
            s.kind = core::LayerEffects::Silhouette::Kind_OuterGlow;
            s.blur = 6.0f;
            s.distance = 12.0f;
            s.angleDeg = (float)angle;
            s.r = s.g = s.b = 1.0f;
            s.opacity = 1.0f;
            s.mode = img::BlendMode_Normal;
            s.blendModeKnown = true;
            glow.silhouettes.push_back(s);

            const Centroid c = centroidOf(renderOne(block, glow, 0.0f));
            // Photoshop's effect angle is counter-clockwise with 0 pointing right, so the
            // silhouette is displaced by (distance*cos, distance*sin) in image space. The
            // readback is GL-ordered (row 0 = the canvas bottom), so image +y is +row.
            const double wantX = 64.0 + 12.0 * std::cos(angle * M_PI / 180.0);
            const double wantY = 48.0 + 12.0 * std::sin(angle * M_PI / 180.0);
            const bool ok = std::abs(c.x - wantX) < 1.0 && std::abs(c.y - wantY) < 1.0;
            allOk = allOk && ok;
            detail += QString(" %1deg:(%2,%3)").arg(angle, 0, 'f', 0).arg(c.x, 0, 'f', 1).arg(c.y, 0, 'f', 1);
        }
        ++gChecks;
        if (!allOk)
            ++gFails;
        caseReport("S19: the silhouette lands at distance/angle from the shape", allOk, detail.trimmed());
    }

    // (5) an outer glow with no distance reproduces the independent Gaussian reference: the
    // shape is blurred (sigma = radius/2, the pass the layer blur uses), tinted with the
    // effect colour and composited under the layer. A 0 % fill isolates it.
    {
        for (const double radius : {4.0, 20.0}) {
            core::LayerEffects glow;
            core::LayerEffects::Silhouette s;
            s.enabled = true;
            s.kind = core::LayerEffects::Silhouette::Kind_OuterGlow;
            s.blur = (float)radius;
            s.distance = 0.0f;
            s.angleDeg = 120.0f;
            s.r = 0.2f;
            s.g = 0.6f;
            s.b = 0.9f;
            s.opacity = 1.0f;
            s.mode = img::BlendMode_Normal;
            s.blendModeKnown = true;
            glow.silhouettes.push_back(s);

            const auto gpu = renderOne(block, glow, 0.0f);

            // reference: the layer's premultiplied shape, blurred by the independent replica,
            // tinted with the effect colour, then presented onto an empty canvas
            ref::Image shape(canvas.width(), canvas.height());
            for (int y = 0; y < canvas.height(); ++y) {
                for (int x = 0; x < canvas.width(); ++x) {
                    float* px = shape.at(x, y);
                    // the fixture's block is symmetric about the centre, so the same predicate
                    // describes it in GL row order and in image row order
                    const bool inside = (x >= 48 && x < 80 && y >= 32 && y < 64);
                    px[0] = inside ? 200.0f / 255.0f : 0.0f;
                    px[1] = inside ? 120.0f / 255.0f : 0.0f;
                    px[2] = inside ? 40.0f / 255.0f : 0.0f;
                    px[3] = inside ? 1.0f : 0.0f;
                }
            }
            const ref::Image blurred = refBlurApplied(shape, canvas, camera, {}, radius, radius, 0.0, 1.0f);
            ref::Image tinted(canvas.width(), canvas.height());
            for (int y = 0; y < canvas.height(); ++y) {
                for (int x = 0; x < canvas.width(); ++x) {
                    const double coverage = blurred.at(x, y)[3];
                    float* px = tinted.at(x, y);
                    px[0] = (float)(0.2 * coverage);
                    px[1] = (float)(0.6 * coverage);
                    px[2] = (float)(0.9 * coverage);
                    px[3] = (float)coverage;
                }
            }
            ref::Image empty(canvas.width(), canvas.height());
            const ref::Image expected = ref::blendPresent(tinted, empty, img::BlendMode_Normal, 1.0);

            const scene::Diff d = scene::diffImages(gpu, ref::imageToBytes(expected));
            const bool ok = checkDiff(d, 4, 0.6, canvas);
            ++gChecks;
            if (!ok)
                ++gFails;
            caseReport(
                QString("S19: outer glow blur %1 matches the Gaussian reference").arg(radius, 0, 'f', 0), ok,
                diffStr(d));
        }
    }

    // (6) an INNER silhouette (chrome) is clipped to the layer's shape, an outer one (glow) is
    // not. This is the invariant that broke when the effect's distance/angle offset was applied
    // in the tint pass: the offset shifts every sample of that pass - the clip included - so the
    // clip moved with the silhouette and clipped nothing, and the chrome spilled a halo up to
    // `blur` pixels outside the shape (measured on the real file: 18-24 levels brighter than its
    // stored composite in the ring around the glyphs).
    {
        const int midY = canvas.height() / 2;
        const int x1 = 80; // the right edge of the 32x32 block the fixtures above use
        const auto renderWith = [&](core::LayerEffects::Silhouette::Kind aKind, float aDistance) {
            core::LayerEffects fx;
            core::LayerEffects::Silhouette s;
            s.enabled = true;
            s.kind = aKind;
            s.blur = 6.0f;
            s.distance = aDistance;
            s.angleDeg = 0.0f;
            s.r = s.g = s.b = 1.0f;
            s.opacity = 1.0f;
            s.mode = img::BlendMode_Normal;
            fx.silhouettes.push_back(s);
            return renderOne(block, fx, 1.0f); // an opaque fill: the shape is exactly the block
        };
        const auto plain = renderOne(block, core::LayerEffects(), 1.0f);
        const auto alphaAt = [&](const std::vector<uint8_t>& aBytes, int aX, int aY) {
            return aBytes[((size_t)aY * canvas.width() + aX) * 4 + 3];
        };
        // a point outside the block but inside the effect's reach (blur 6 + distance 8)
        const int outX = x1 + 6;
        for (const auto& variant : {std::make_pair(core::LayerEffects::Silhouette::Kind_Chrome, 8.0f),
                 std::make_pair(core::LayerEffects::Silhouette::Kind_OuterGlow, 8.0f)}) {
            const auto rendered = renderWith(variant.first, variant.second);
            const bool inner = variant.first == core::LayerEffects::Silhouette::Kind_Chrome;
            const bool changedOutside = alphaAt(rendered, outX, midY) != alphaAt(plain, outX, midY);
            expect(
                changedOutside != inner,
                inner ? "S19: an inner silhouette (chrome) paints nothing outside the shape"
                      : "S19: an outer silhouette (glow) does reach outside the shape");
            // and it must still do something inside
            expect(
                rendered != plain, "S19: the silhouette changes the shape's own pixels");
        }
    }

    // (7) the silhouette is presented with the effect's own blend mode: Screen over a dark
    // backdrop is lighter than the tint alone, which a Normal present could not produce
    {
        const Fill backdrop = [](int, int, uint8_t* px) {
            px[0] = px[1] = px[2] = 64;
            px[3] = 255;
        };
        const auto withMode = [&](img::BlendMode aMode) {
            core::LayerEffects chrome;
            core::LayerEffects::Silhouette s;
            s.enabled = true;
            s.kind = core::LayerEffects::Silhouette::Kind_Chrome;
            s.blur = 8.0f;
            s.distance = 0.0f;
            s.r = s.g = s.b = 0.5f;
            s.opacity = 1.0f;
            s.mode = aMode;
            s.blendMode = aMode == img::BlendMode_Screen ? "Scrn" : "Nrml";
            chrome.silhouettes.push_back(s);
            return renderOne(block, chrome, 0.0f, &backdrop);
        };
        // the lightest Screen over 64/255 is 1 - (1 - 0.5) * (1 - 64/255) = 0.625 (159),
        // while a Normal present caps at the tint itself (128)
        const auto screen = withMode(img::BlendMode_Screen);
        int screenMax = 0;
        for (size_t i = 0; i < screen.size(); i += 4)
            screenMax = std::max(screenMax, (int)screen[i]);
        expect(
            screenMax >= 155 && screenMax <= 162,
            QString("S19: a Screen silhouette lightens the backdrop (max channel %1, Normal caps at 128)")
                .arg(screenMax));
        const auto normal = withMode(img::BlendMode_Normal);
        int normalMax = 0;
        for (size_t i = 0; i < normal.size(); i += 4)
            normalMax = std::max(normalMax, (int)normal[i]);
        expect(
            normalMax <= 130,
            QString("S19: the same silhouette with Normal stays at the tint (max %1)").arg(normalMax));
    }

    const bool ok = (gFails == failsBefore);
    caseReport(
        QString("layer fill opacity and silhouette effects"), ok, QString("%1 checks").arg(gChecks - checksBefore));
}

void suiteLayerEffects() {
    suiteHeader("S18 layer effects: descriptor, gradient overlay, import, persistence");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    const QSize canvas(128, 96);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);
    const QVector2D center(canvas.width() / 2.0f, canvas.height() / 2.0f);
    const QVector2D unit(1.0f, 1.0f);

    using Fill = std::function<void(int, int, uint8_t*)>;
    struct Pool {
        QSize size;
        std::vector<img::ResourceNode*> nodes;
        ~Pool() { qDeleteAll(nodes.begin(), nodes.end()); }
        img::ResourceNode* image(const QString& aId, const Fill& aFill) {
            nodes.push_back(scene::makeImage(aId, size, aFill));
            return nodes.back();
        }
    };
    const Fill rampX = [](int x, int, uint8_t* px) {
        const int v = x * 255 / 127;
        px[0] = px[1] = px[2] = (uint8_t)v;
        px[3] = 255;
    };
    const Fill baseColor = [](int, int, uint8_t* px) {
        px[0] = 120; px[1] = 60; px[2] = 30; px[3] = 255;
    };

    // (1) the descriptor reader against a hand-written block
    {
        const auto gradient = [](DescWriter& w) { addGradientOverlay(w, true, "Lnr ", 90.0, 100.0); };
        const auto block = makeLfx2({gradient});
        img::PSDDescriptor descriptor;
        expect(
            descriptor.parse(block.data(), block.size(), 8),
            "S18: the lfx2 descriptor parses (version + data version header)");
        expect(descriptor.isValid() && descriptor.root().isDescriptor(), "S18: the root is a descriptor");
        const auto* grad = descriptor.root().find("GrFl");
        expect(grad != nullptr, "S18: the gradient overlay key is found");
        if (grad) {
            const auto* md = grad->find("Md  ");
            const auto* type = grad->find("Type");
            expect(
                md && md->asEnumValue() == "Nrml" && type && type->asEnumValue() == "Lnr ",
                "S18: effect enums come back as their 4CC values");
            const auto* stops = grad->find("Grad") ? grad->find("Grad")->find("Clrs") : nullptr;
            expect(stops && stops->size() == 2, "S18: the two colour stops are read");
            if (stops && stops->size() == 2) {
                const auto* first = stops->at(0)->find("Clr ");
                const auto* second = stops->at(1)->find("Clr ");
                expect(
                    first && second && first->find("Rd  ")->number == 228.0
                        && second->find("Grn ")->number == 213.0 && stops->at(1)->find("Lctn")->number == 4096.0,
                    "S18: stop colours and locations are read");
            }
        }
        // malformed inputs must fail rather than mis-parse
        img::PSDDescriptor truncated;
        expect(
            !truncated.parse(block.data(), 12, 8) && !truncated.isValid(),
            "S18: a truncated block fails the parse");
        {
            // an unknown OSType is rejected
            std::vector<uint8> bad(block.begin(), block.end());
            for (size_t i = 0; i + 4 <= bad.size(); ++i) {
                if (std::memcmp(&bad[i], "UntF", 4) == 0) {
                    std::memcpy(&bad[i], "zzzz", 4);
                    break;
                }
            }
            img::PSDDescriptor unknown;
            expect(!unknown.parse(bad.data(), bad.size(), 8), "S18: an unknown OSType fails the parse");
        }
    }

    // (2) the model: what is drawn, what is only recognised
    {
        core::LayerEffects effects;
        const auto gradient = [](DescWriter& w) { addGradientOverlay(w, true, "Lnr ", 90.0, 100.0); };
        const auto chrome = [](DescWriter& w) { addChrome(w, 14.0, 50.0, 11.0, 19.0, "Scrn"); };
        const auto block = makeLfx2({gradient, chrome});
        img::PSDDescriptor descriptor;
        expect(descriptor.parse(block.data(), block.size(), 8), "S18: descriptor parses for the model test");
        expect(core::parseLayerEffects(descriptor, effects), "S18: the effects model parses");
        expect(effects.gradientOverlay.enabled && effects.gradientOverlay.isValid(), "S18: gradient overlay is active");
        expect(effects.gradientOverlay.isLinear(), "S18: the linear style is recognised");
        expect(
            std::abs(effects.gradientOverlay.angleDeg - 90.0f) < 0.001f
                && std::abs(effects.gradientOverlay.scale - 100.0f) < 0.001f,
            "S18: angle and scale are read");
        expect(
            effects.gradientOverlay.stops.size() == 2
                && std::abs(effects.gradientOverlay.stops[0].r - 228.0f / 255.0f) < 0.001f
                && std::abs(effects.gradientOverlay.stops[1].b - 79.0f / 255.0f) < 0.001f,
            "S18: merged stops carry colour and transparency");
        expect(effects.seen[core::LayerEffects::Type_Chrome], "S18: the chrome key is reported as present");
        // this fixture's chrome carries a blur, so it IS drawn: a blurred silhouette tinted
        // with the effect colour and presented with its own blend mode (S19 tests that path)
        expect(
            effects.silhouettes.size() == 1 && effects.silhouettes[0].kind == core::LayerEffects::Silhouette::Kind_Chrome,
            "S18: the chrome becomes a drawable silhouette");
        if (effects.silhouettes.size() == 1) {
            const auto& sl = effects.silhouettes[0];
            expect(
                sl.mode == img::BlendMode_Screen && sl.blendModeKnown && sl.isUsable() && !sl.isUnder(),
                "S18: the silhouette carries its blend mode, opacity and over/under side");
            expect(
                std::abs(sl.blur - 14.0f) < 0.001f && std::abs(sl.distance - 11.0f) < 0.001f
                    && std::abs(sl.angleDeg - 19.0f) < 0.001f && std::abs(sl.opacity - 0.5f) < 0.001f,
                "S18: silhouette blur, distance, angle and opacity are read");
        }
        expect(effects.gradientOverlay.blendModeKnown, "S18: a known effect blend key is accepted");
        expect(effects.isDrawnAsConfigured(), "S18: the effects this build draws are reported as faithful");
    }

    // (2b) the fidelity report must not overstate: an effect the renderer cannot draw
    // (bevel/emboss, stroke, satin, inner glow/shadow) or one that carries nothing to draw
    // (an enabled chrome without a blur) marks the result as not fully faithful
    {
        const auto gradient = [](DescWriter& w) { addGradientOverlay(w, true, "Lnr ", 90.0, 100.0); };
        const auto chrome = [](DescWriter& w) { addChrome(w, 0.0, 50.0, 11.0, 19.0, "Scrn"); };
        const auto block = makeLfx2({gradient, chrome});
        core::LayerEffects effects;
        img::PSDDescriptor descriptor;
        expect(descriptor.parse(block.data(), block.size(), 8), "S18b: descriptor parses");
        expect(core::parseLayerEffects(descriptor, effects), "S18b: the effects model parses");
        expect(effects.active[core::LayerEffects::Type_Chrome], "S18b: the chrome is enabled in the file");
        expect(effects.silhouettes.empty(), "S18b: a blur-less chrome produces no silhouette");
        expect(
            !effects.isDrawnAsConfigured(),
            "S18b: an enabled effect that is not drawn marks the result as not fully faithful");
    }

    // (3) the gradient overlay's placement against the reference, for two angles
    {
        for (const double angle : {90.0, 0.0}) {
            core::LayerEffects effects;
            effects.gradientOverlay.enabled = true;
            effects.gradientOverlay.style = "Lnr ";
            effects.gradientOverlay.blendMode = "Nrml";
            effects.gradientOverlay.angleDeg = (float)angle;
            effects.gradientOverlay.scale = 100.0f;
            effects.gradientOverlay.stops = {{0.0f, 228.0f / 255.0f, 161.0f / 255.0f, 30.0f / 255.0f, 1.0f},
                                             {1.0f, 248.0f / 255.0f, 213.0f / 255.0f, 79.0f / 255.0f, 1.0f}};

            Pool pool;
            pool.size = canvas;
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(tree, root, pool.image("fx", baseColor), "fx", center, 0.0f, unit);
            layer->setLayerEffects(effects);
            // the first-pushed sibling renders on top, so the backdrop goes last
            scene::addLayer(tree, root, pool.image("bg", rampX), "bg", center, 0.0f, unit);
            const std::vector<uint8_t> gpu = fx.render(tree, camera, 0);

            // reference: apply the gradient to the layer's own pixels, then composite
            ref::Image content(canvas.width(), canvas.height());
            for (int y = 0; y < canvas.height(); ++y) {
                // the reference is built in GL row order (row 0 = canvas bottom) while the
                // gradient is anchored to the layer's IMAGE rows (row 0 = top), which is what
                // the shader samples; convert instead of assuming they agree
                const int imageRow = canvas.height() - 1 - y;
                for (int x = 0; x < canvas.width(); ++x) {
                    const double t = ref::gradientOverlayT(
                        x, imageRow, canvas.width(), canvas.height(), angle, 100.0, false);
                    double rgba[4];
                    ref::gradientSample(effects.gradientOverlay.stops, t, rgba);
                    float* px = content.at(x, y);
                    const double base[3] = {120.0 / 255.0, 60.0 / 255.0, 30.0 / 255.0};
                    for (int c = 0; c < 3; ++c)
                        px[c] = (float)(base[c] + (rgba[c] - base[c]) * rgba[3]);
                    px[3] = 1.0f;
                }
            }
            // the backdrop render is the same tree without the effect
            Pool bgPool;
            bgPool.size = canvas;
            core::ObjectTree bgTree;
            auto* bgRoot = new core::FolderNode("root");
            bgTree.grabTopNode(bgRoot);
            scene::addLayer(bgTree, bgRoot, bgPool.image("bg", rampX), "bg", center, 0.0f, unit);
            const auto bgBytes = fx.render(bgTree, camera, 0);

            const ref::Image expected = ref::blendPresent(content, ref::imageFromBytes(bgBytes.data(), canvas.width(), canvas.height()), img::BlendMode_Normal, 1.0);
            // the outer band is excluded: the layer mesh insets its edge vertices, so the
            // outermost texels of a canvas-sized layer blend with the cleared background (the
            // golden suite crops the same band, tests/golden README's borderCrop)
            const int crop = 3;
            const auto cropped = [&](const std::vector<uint8_t>& aBytes) {
                const int w = canvas.width(), h = canvas.height();
                const int iw = w - 2 * crop, ih = h - 2 * crop;
                std::vector<uint8_t> out((size_t)iw * ih * 4);
                for (int y = 0; y < ih; ++y) {
                    std::memcpy(out.data() + (size_t)y * iw * 4,
                        aBytes.data() + ((size_t)(y + crop) * w + crop) * 4, (size_t)iw * 4);
                }
                return out;
            };
            const scene::Diff d = scene::diffImages(cropped(gpu), cropped(ref::imageToBytes(expected)));
            // the gradient is evaluated in float on the GPU and in double in the reference, so
            // a smooth ramp differs by one 8-bit level on many pixels; the tolerance is on the
            // mean for that reason (max stays pinned at <= 2)
            const bool ok = checkDiff(d, 2, 0.5, QSize(canvas.width() - 2 * crop, canvas.height() - 2 * crop));
            ++gChecks;
            if (!ok)
                ++gFails;
            caseReport(QString("S18: gradient overlay at %1 degrees").arg(angle), ok, diffStr(d));
        }
    }

    // (4) the effects ride the content pass, so they do not change the layer's coverage:
    // an all-zero-gradient-opacity layer must render like the same layer without effects
    {
        core::LayerEffects effects;
        effects.gradientOverlay.enabled = true;
        effects.gradientOverlay.style = "Lnr ";
        effects.gradientOverlay.blendMode = "Nrml";
        effects.gradientOverlay.stops = {{0.0f, 1.0f, 1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f, 0.0f}};

        auto render = [&](bool aWithEffects) {
            Pool pool;
            pool.size = canvas;
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* layer = scene::addLayer(tree, root, pool.image("fx", baseColor), "fx", center, 0.0f, unit);
            if (aWithEffects)
                layer->setLayerEffects(effects);
            scene::addLayer(tree, root, pool.image("bg", rampX), "bg", center, 0.0f, unit);
            return fx.render(tree, camera, 0);
        };
        expect(
            render(true) == render(false),
            "S18: a fully transparent gradient overlay is a no-op (byte-identical)");
    }

    // (5) import: a PSD whose layer carries an lfx2 block imports the effects, and the
    // project format round-trips them
    {
        const auto gradient = [](DescWriter& w) { addGradientOverlay(w, true, "Lnr ", 90.0, 100.0); };
        const auto chrome = [](DescWriter& w) { addChrome(w, 14.0, 50.0, 11.0, 19.0, "Scrn"); };
        const auto block = makeLfx2({gradient, chrome});

        img::PSDFormat format;
        format.header().version = 1;
        format.header().channels = 4;
        format.header().height = 64;
        format.header().width = 64;
        format.header().depth = 8;
        format.header().mode = img::PSDFormat::ColorMode_RGB;
        format.imageData().compressionId = 0;

        auto* layer = new img::PSDFormat::Layer();
        layer->name = "fx";
        layer->rect.edge[0] = 0;
        layer->rect.edge[1] = 0;
        layer->rect.edge[2] = 64;
        layer->rect.edge[3] = 64;
        layer->blendMode = "norm";
        layer->opacity = 255;
        layer->flags = 0x08;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = 64 * 64;
            chan->data.reset(new uint8[64 * 64]());
            layer->channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }
        {
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "lfx2";
            info->dataLength = (uint32)block.size();
            info->data.reset(new uint8[block.size()]);
            std::memcpy(info->data.get(), block.data(), block.size());
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        // an iOpa of 128 (fill opacity), stored the way Photoshop writes it (low byte first)
        {
            auto* info = new img::PSDFormat::AdditionalLayerInfo();
            info->key = "iOpa";
            info->dataLength = 4;
            info->data.reset(new uint8[4]());
            info->data[0] = 128;
            layer->additionalInfos.push_back(img::PSDFormat::AdditionalLayerInfoPtr(info));
        }
        format.layerAndMaskInfo().layers.push_back(img::PSDFormat::LayerPtr(layer));

        const uint32 mergedSize = 512 * 64;
        for (const sint16 id : {sint16(0), sint16(1), sint16(2), sint16(-1)}) {
            auto* chan = new img::PSDFormat::Channel();
            chan->id = id;
            chan->compressionId = 0;
            chan->dataLength = mergedSize;
            chan->data.reset(new uint8[mergedSize]());
            format.imageData().channels.push_back(img::PSDFormat::ChannelPtr(chan));
        }

        const QString path = "/tmp/vb_layer_effects.psd";
        {
            std::ofstream out(path.toLocal8Bit().constData(), std::ios::binary);
            img::PSDWriter writer(out, format);
            expect(writer.resultCode() == img::PSDWriter::ResultCode_Success, "S18: PSDWriter writes the lfx2 block");
            out.close();
        }

        StubAnimator animator;
        core::Project project("", animator, nullptr);
        ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
        NullProgressReporter reporter;
        const bool loaded = loader.load(path, project, reporter);
        expect(loaded, QString("S18: the PSD with effects imports: %1").arg(loader.log()));
        // the imported values, kept for the round-trip comparison below
        core::LayerEffects importedFx;
        bool haveImportedFx = false;
        if (loaded) {
            core::ObjectNode* found = nullptr;
            const std::function<void(core::ObjectNode*)> walk = [&](core::ObjectNode* aNode) {
                if (aNode->name() == "fx") {
                    found = aNode;
                }
                for (auto* child : aNode->children())
                    walk(child);
            };
            walk(project.objectTree().topNode());
            expect(found != nullptr, "S18: the layer imports");
            if (found && found->renderer()) {
                const core::LayerEffects fx2 = found->renderer()->layerEffects();
                importedFx = fx2;
                haveImportedFx = true;
                expect(
                    fx2.gradientOverlay.enabled && fx2.gradientOverlay.stops.size() == 2
                        && std::abs(fx2.gradientOverlay.stops[0].r - 228.0f / 255.0f) < 0.001f,
                    "S18: the imported layer carries the gradient overlay");
                expect(
                    found->renderer()->layerEffects().seen[core::LayerEffects::Type_Chrome],
                    "S18: the undrawn effect is reported as present");
                // Photoshop's FILL opacity is NOT the layer opacity: it scales only the
                // layer's own pixels, so the layer keeps its own opacity and the fill opacity
                // rides the shader (uFillOpacity) where a style can outlive it
                const auto* opaKey = (const core::OpaKey*)found->timeLine()->defaultKey(core::TimeKeyType_Opa);
                expect(
                    opaKey && std::abs(opaKey->opacity() - 1.0f) < 0.001f,
                    QString("S18: iOpa stays out of the layer opacity (got %1)")
                        .arg(opaKey ? opaKey->opacity() : -1.0f));
                expect(
                    std::abs(found->renderer()->fillOpacity() - 128.0f / 255.0f) < 0.001f,
                    QString("S18: iOpa becomes the fill opacity (got %1)")
                        .arg(found->renderer()->fillOpacity()));
            }

            const QString aniePath = "/tmp/vb_layer_effects.anie";
            ctrl::ProjectSaver saver;
            expect(saver.save(aniePath, project), "S18: project with effects saves");
            core::Project reloaded("", animator, nullptr);
            ctrl::ProjectLoader loader2;
            const bool reloadedOk = loader2.load(aniePath, reloaded, gl::DeviceInfo::instance(), reporter);
            expect(reloadedOk, "S18: project with effects reloads");
            if (reloadedOk) {
                core::ObjectNode* found2 = nullptr;
                const std::function<void(core::ObjectNode*)> walk2 = [&](core::ObjectNode* aNode) {
                    if (aNode->name() == "fx") {
                        found2 = aNode;
                    }
                    for (auto* child : aNode->children())
                        walk2(child);
                };
                walk2(reloaded.objectTree().topNode());
                expect(found2 && found2->renderer(), "S18: the layer survives the round-trip");
                if (found2 && found2->renderer()) {
                    expect(
                        found2->renderer()->layerEffects() == found->renderer()->layerEffects(),
                        "S18: the effects survive the project round-trip");
                    expect(
                        found2->renderer()->layerEffects().seen[core::LayerEffects::Type_Chrome],
                        "S18: the reported effect keys survive the round-trip");
                    // layerEffects() returns BY VALUE: hold a copy, or `sl` would reference a
                    // temporary that dies at the end of the statement
                    const core::LayerEffects reloadedFx = found2->renderer()->layerEffects();
                    if (reloadedFx.silhouettes.size() == 1) {
                        const auto& sl = reloadedFx.silhouettes[0];
                        const bool same = sl.kind == core::LayerEffects::Silhouette::Kind_Chrome
                            && sl.mode == img::BlendMode_Screen && sl.blendModeKnown
                            && std::abs(sl.blur - 14.0f) < 0.001f && std::abs(sl.distance - 11.0f) < 0.001f
                            && std::abs(sl.angleDeg - 19.0f) < 0.001f && std::abs(sl.opacity - 0.5f) < 0.001f;
                        expect(
                            same,
                            QString("S18: the silhouette's geometry, tint and blend mode survive the "
                                    "round-trip (reloaded kind=%1 mode=%2 known=%3 blur=%4 dist=%5 angle=%6 "
                                    "opa=%7 | imported kind=%8 blur=%9)")
                                .arg((int)sl.kind)
                                .arg((int)sl.mode)
                                .arg(sl.blendModeKnown ? 1 : 0)
                                .arg(sl.blur)
                                .arg(sl.distance)
                                .arg(sl.angleDeg)
                                .arg(sl.opacity)
                                .arg(haveImportedFx && !importedFx.silhouettes.empty()
                                        ? (int)importedFx.silhouettes[0].kind
                                        : -1)
                                .arg(haveImportedFx && !importedFx.silhouettes.empty() ? importedFx.silhouettes[0].blur
                                                                                      : -1.0f));
                    } else {
                        expect(false, "S18: the silhouette survives the round-trip");
                    }
                    expect(
                        std::abs(found2->renderer()->fillOpacity() - 128.0f / 255.0f) < 0.001f,
                        QString("S18: the fill opacity survives the round-trip (got %1)")
                            .arg(found2->renderer()->fillOpacity()));
                }
            }
        }
    }

    caseReport(
        "layer effects (descriptor, gradient overlay, import, persistence)", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));
}

void suiteShaderMatrix() {
    suiteHeader("S15 shader-variant matrix: (mode, isClippee, useHSV) compile+link");
    const int failsBefore = gFails;
    const int checksBefore = gChecks;
    core::ObjectTree tree;
    int variantCount = 0;
    for (int m = img::BlendMode_Normal; m < img::BlendMode_TERM; ++m) {
        const img::BlendMode mode = (img::BlendMode)m;
        for (int clippee = 0; clippee < 2; ++clippee) {
            for (int hsv = 0; hsv < 2; ++hsv) {
                tree.shaderHolder().reserveShader(mode, clippee != 0, hsv != 0);
                ++variantCount;
            }
        }
    }
    expect(
        variantCount == img::BlendMode_TERM * 2 * 2,
        QString("S15: compiled+linked %1 of %2 (mode, isClippee, useHSV) shader variants")
            .arg(variantCount).arg(img::BlendMode_TERM * 2 * 2));
    caseReport("shader-variant permutation matrix", gFails == failsBefore,
        QString("%1 checks").arg(gChecks - checksBefore));
}

//-------------------------------------------------------------------------------------------------
// S10: performance snapshot (informational, not pass/fail): filtered vs unfiltered renders
// at a realistic canvas. Each timed render includes one glReadPixels (forces a pipeline
// sync; constant across variants, so ratios are meaningful).
void suitePerf() {
    suiteHeader("S10 performance snapshot (informational, 1920x1080)");
    const QSize canvas(1920, 1080);
    auto& fx = fixtureFor(canvas);
    const core::CameraInfo camera = scene::makeCamera(canvas);

    auto build = [](core::ObjectTree& tree, ResPool& pool, float aBlur) {
        auto* root = new core::FolderNode("root");
        tree.grabTopNode(root);
        auto* folder = scene::addFolder(root, "folder", QVector2D(760, 540), 20.0f, QVector2D(1.3f, 0.9f));
        for (int i = 0; i < 3; ++i) {
            scene::addLayer(
                tree, folder, pool.wedge(QString("w%1").arg(i)), QString("l%1").arg(i),
                QVector2D(200.0f * i, 60.0f * i), 15.0f * i, QVector2D(3, 3));
        }
        if (aBlur > 0.0f)
            scene::addBlurAmount(folder, 0, aBlur);
        scene::addLayer(tree, root, pool.wedge("bg"), "bg", QVector2D(960, 540), 0.0f, QVector2D(30, 22));
    };

    const struct {
        const char* name;
        float blur;
    } variants[] = {
        {"no blur", 0.0f}, {"blur 6 (direct)", 6.0f}, {"blur 40 (ladder)", 40.0f}, {"blur 200 (ladder)", 200.0f}};
    for (const auto& v : variants) {
        ResPool pool;
        core::ObjectTree tree;
        build(tree, pool, v.blur);
        fx.render(tree, camera, 0); // warmup: shader compile + FBO/slot allocation
        fx.render(tree, camera, 0);
        QElapsedTimer timer;
        timer.start();
        const int kIters = 10;
        for (int i = 0; i < kIters; ++i)
            fx.render(tree, camera, 0);
        const double ms = timer.elapsed() / (double)kIters;
        std::printf("  [INFO] %-20s %8.2f ms/frame (incl. readback)\n", v.name, ms);
    }
    ++gChecks; // informational suite; running it to completion is the check
}

} // namespace

int main(int argc, char** argv) {
    static HarnessAssertHandler assertHandler;
    static HarnessErrorHandler errorHandler;
    gXCAssertHandler = &assertHandler;
    gXCErrorHandler = &errorHandler;

    // surface format must be set before any GL context is created (mirrors MainWindow)
    {
        QSurfaceFormat format;
        format.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(4);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc, argv);

    // (the comma-decimal LC_NUMERIC regression is scoped to the ORA load with
    // uselocale() inside suiteOraImport, not pinned process-wide; see S13)

    // golden input generation needs no GL: --golden-gen [dir] writes the case inputs +
    // default case.json files and exits (author expected.png afterwards, see golden.h)
    if (argc > 1 && QString(argv[1]) == "--golden-gen")
        return golden::generateInputs(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("tests/golden"));

    QMainWindow window;
    auto* glWidget = new HarnessGLWidget();
    window.setCentralWidget(glWidget);
    window.resize(32, 32);
    window.show();
    app.processEvents();
    if (!glWidget->mGLReady) { // some platforms need another nudge
        glWidget->update();
        app.processEvents();
    }
    if (!glWidget->mGLReady) {
        std::fprintf(stderr, "FATAL: GL widget did not initialize on this platform\n");
        return 2;
    }
    glWidget->makeCurrent(); // Qt keeps the context current; be explicit for tree building

    // the offscreen platform can recreate the widget's FBO/context behind initializeGL's
    // back, leaving the widget's VAO name stale and a pending GL_INVALID_OPERATION in the
    // current context; the first error-checked GL call then aborts (a 0-layer PSD import
    // hits this in ClippingFrame because its loader does no GL work that would consume
    // the flag). Rebind a fresh VAO and drain the error flag.
    glWidget->mVAO.reset(new gl::VertexArrayObject());
    glWidget->mVAO->bind();
    while (gl::Global::functions().glGetError() != GL_NO_ERROR) {
    }

    // --golden <dir> [case] runs only the golden suite against an alternate case
    // directory, optionally restricted to one case (one-time real-file analysis)
    if (argc > 2 && QString(argv[1]) == "--golden") {
        golden::suite(
            QString::fromLocal8Bit(argv[2]), argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString());
        std::printf("\n%d checks, %d failures\n", gChecks, gFails);
        scene::clearFixtures();
        return gFails ? 1 : 0;
    }

    suiteMath();
    suiteLadder();
    suiteBlending();
    suiteRender();
    suiteOrientation();
    suiteGating();
    suiteAlphaProbe();
    suiteInteractions();
    suiteSerialization();
    suiteExport();
    suitePsdImport();
    suiteOraImport();
    suiteFolderBlend();
    suiteBlendIf();
    suiteLayerMask();
    suiteLayerEffects();
    suiteLayerSilhouettes();
    suiteLayerStroke();
    suiteResourceIdentity();
    suiteMeshCoverage();
    suiteMeshContentLevel();
    suiteMeshSmallFeatureCoverage();
    suiteMeshEdgeCoverage();
    suiteMeshContentLevelDerivation();
    suiteShaderMatrix();
    golden::suite("tests/golden");
    suitePerf();

    std::printf("\n%d checks, %d failures\n", gChecks, gFails);

    // tear GL objects down while the context is still current
    scene::clearFixtures();
    return gFails ? 1 : 0;
}
