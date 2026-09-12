// PSD composite probe: load a real PSD through the production importer, render one
// frame through the production composite wiring, and diff the render against the
// composite image the PSD itself stores (section 5), which is decoded independently
// (see extract_composite.py) rather than re-generated from the layers.
//
// Built by tools/psd_composite_probe/build_and_run.sh; the offscreen GL fixture is the
// repo's tools/verify_blur/scene.{h,cpp} (-Itools/verify_blur), not a private copy.
// See tools/psd_composite_probe/README.md for the printed sections and the env flags.
//
// usage: psd_composite_probe <file.psd> <composite.png> <outDir> [frame]
//
// Prints: production PSDReader result + layer record dump (masks, fill opacity, layer
// effects, per-layer decode checksums), import tree (vs records), render alpha/stat
// summary, diff metrics vs the stored composite, leave-one-out attribution, worst tiles,
// and PNG dumps (render_straight.png, diff_heatmap.png, worst_tile_*.png, ...).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QApplication>
#include <QDir>
#include <cmath>
#include <algorithm>
#include <vector>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QRegularExpression>
#include <QFileInfo>
#include <QImageReader>
#include <QStringList>
#include <QMainWindow>
#include <QOpenGLWidget>
#include <QOpenGLVersionFunctionsFactory>
#include <QScopedPointer>
#include <QString>

#include "core/Animator.h"
#include "core/FolderNode.h"
#include "core/LayerNode.h"
#include "core/ObjectNode.h"
#include "core/ObjectType.h"
#include "core/OpaKey.h"
#include "core/ImageKey.h"
#include "core/Project.h"
#include "core/Renderer.h"
#include "core/TimeLine.h"
#include "core/TimeKeyType.h"
#include "ctrl/ImageFileLoader.h"
#include "gl/DeviceInfo.h"
#include "gl/Global.h"
#include "gl/VertexArrayObject.h"
#include "img/BlendMode.h"
#include "img/PSDFormat.h"
#include "img/PSDReader.h"
#include "img/PSDUtil.h"
#include "img/GridMeshCreator.h"
#include "img/MeshContentLevel.h"
#include "img/ResourceData.h"
#include "core/Constant.h"
#include "core/ImageKey.h"
#include "core/GridMesh.h"
#include "util/IProgressReporter.h"

#include "scene.h"

// the libs reference these globals (defined by src/gui/Main.cpp in the app)
class ProbeAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::fprintf(stderr, "XC_ASSERT failure\n"); }
};
class ProbeErrorHandler: public XCErrorHandler {
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

// src/core/Project.cpp references this gui method, but libgui is not linked here
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

class StubAnimator: public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(0); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

class VerboseReporter: public util::IProgressReporter {
public:
    void setSection(const QString& aSection) override {
        std::printf("  [reporter] section: %s\n", aSection.toUtf8().constData());
        std::fflush(stdout);
    }
    void setMaximum(int aMax) override { mMax = aMax; }
    void setProgress(int aValue) override {
        if (mMax <= 0)
            return;
        const int step = std::max(1, mMax / 10);
        if (aValue == mMax || aValue % step == 0) {
            std::printf("  [reporter] %d/%d\n", aValue, mMax);
            std::fflush(stdout);
        }
    }
    bool wasCanceled() const override { return false; }

private:
    int mMax = 0;
};

class ProbeGLWidget: public QOpenGLWidget {
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
            std::fprintf(stderr, "FATAL: failed to get OpenGL core functions\n");
            std::exit(2);
        }
        if (!mGLReady) {
            gl::Global::setContext(*this);
            gl::Global::setFunctions(*functions);
            mDeviceInfo.load();
            gl::DeviceInfo::setInstance(&mDeviceInfo);
            const char* version = (const char*)context()->functions()->glGetString(GL_VERSION);
            const char* renderer = (const char*)context()->functions()->glGetString(GL_RENDERER);
            std::printf("OpenGL: %s | %s\n", version ? version : "?", renderer ? renderer : "?");
        } else {
            gl::Global::clearFunctions();
            gl::Global::setFunctions(*functions);
        }
        mVAO.reset(new gl::VertexArrayObject());
        mVAO->bind();
        mGLReady = true;
    }
};

const char* entryTypeName(img::PSDFormat::LayerEntryType aType) {
    switch (aType) {
    case img::PSDFormat::LayerEntryType_Layer: return "layer";
    case img::PSDFormat::LayerEntryType_OpenFolder: return "folder-open";
    case img::PSDFormat::LayerEntryType_CloseFolder: return "folder-closed";
    case img::PSDFormat::LayerEntryType_Bounding: return "bounding";
    default: return "?";
    }
}

// decodes one layer channel plane (raw or RLE) into w*h bytes
bool decodePlane(const img::PSDFormat::Channel& aChan, int aW, int aH, std::vector<uint8_t>& aOut) {
    aOut.assign((size_t)aW * aH, 0);
    if (aW <= 0 || aH <= 0 || !aChan.data)
        return false;
    if (aChan.compressionId == 0) {
        if (aChan.dataLength < (uint32)aW * (uint32)aH)
            return false;
        std::memcpy(aOut.data(), aChan.data.get(), (size_t)aW * aH);
        return true;
    }
    return img::PSDUtil::decodePlanePackBits(aOut.data(), aOut.size(), aChan.data.get(), aChan.dataLength, aW, aH, 1);
}

//-------------------------------------------------------------------------------------------------
// 1. production PSDReader: does the container parse, and what do the records say?
bool dumpPsdRecords(const QString& aPath, const QString& aCompositePng, const QString& aOutDir) {
    std::printf("== PSDReader (production container parse) ==\n");
    std::ifstream file(aPath.toLocal8Bit(), std::ios::binary);
    if (file.fail()) {
        std::printf("  cannot open\n");
        return false;
    }
    img::PSDReader reader(file);
    std::printf(
        "  resultCode=%d msg=%s\n", (int)reader.resultCode(), reader.resultMessage().c_str());
    if (reader.resultCode() != img::PSDReader::ResultCode_Success)
        return false;

    auto& fmt = *reader.format();
    const auto& hdr = fmt.header();
    std::printf(
        "  header: %ux%u channels=%u depth=%u mode=%u\n", hdr.width, hdr.height, hdr.channels, hdr.depth, hdr.mode);

    auto& lmi = fmt.layerAndMaskInfo();
    std::printf(
        "  layerAndMaskInfo: layerCount(field)=%d records=%zu globalMask=%s globalInfos=%zu\n",
        (int)lmi.layerCount, lmi.layers.size(), lmi.globalLayerMaskInfo ? "yes" : "no",
        lmi.additionalLayerInfos.size());
    std::printf("  imageResources: %zu blocks\n", fmt.imageResources().blocks.size());

    std::printf("  image resource block ids:");
    for (const auto& b : fmt.imageResources().blocks)
        std::printf(" %d", (int)b->id);
    std::printf("\n");

    int i = 0;
    for (const auto& lp : lmi.layers) {
        const auto& L = *lp;
        std::printf(
            "  rec[%02d] %-13s name=%-40s rect=(%d,%d %dx%d) opa=%3d clip=%d flags=0x%02x vis=%d blend='%s'\n",
            i++, entryTypeName(L.entryType), L.name.c_str(), L.rect.left(), L.rect.top(), L.rect.width(),
            L.rect.height(), (int)L.opacity, (int)L.clipping, (int)L.flags, L.isVisible() ? 1 : 0,
            L.blendMode.c_str());
        std::printf("         channels:");
        for (const auto& c : L.channels)
            std::printf(" %d(%uB)", (int)c->id, (unsigned)c->dataLength);
        std::printf("  mask=%s  infos:", L.mask ? "YES" : "no");
        for (const auto& info : L.additionalInfos)
            std::printf(" %s(%u)", info->key.c_str(), (unsigned)info->dataLength);
        std::printf("\n");

        // coverage: how much of the layer rect the stored alpha / user mask actually cover
        const img::PSDFormat::Channel* alpha = nullptr;
        const img::PSDFormat::Channel* mask = nullptr;
        for (const auto& c : L.channels) {
            if (c->id == -1) alpha = c.get();
            if (c->id == -2) mask = c.get();
        }
        if (alpha || mask) {
            std::printf("         ");
            struct PlaneSpec { const char* name; const img::PSDFormat::Channel* chan; int w, h; };
            const PlaneSpec specs[2] = {
                {"alpha", alpha, L.rect.width(), L.rect.height()},
                {"mask", mask, L.mask ? L.mask->rect.width() : L.rect.width(),
                 L.mask ? L.mask->rect.height() : L.rect.height()},
            };
            for (const auto& spec : specs) {
                std::vector<uint8_t> plane;
                if (!spec.chan || !decodePlane(*spec.chan, spec.w, spec.h, plane)) {
                    if (spec.chan)
                        std::printf("%s=DECODE-FAIL(%dx%d) ", spec.name, spec.w, spec.h);
                    continue;
                }
                long long sum = 0, zero = 0, full = 0;
                for (uint8_t v : plane) {
                    sum += v;
                    if (v < 8) ++zero;
                    if (v > 248) ++full;
                }
                const double n = (double)plane.size();
                std::printf(
                    "%s(%dx%d): mean=%6.1f clear=%5.1f%% opaque=%5.1f%% ", spec.name, spec.w, spec.h, sum / n,
                    100.0 * zero / n, 100.0 * full / n);
            }
            std::printf("\n");
        }
        // per-layer extras the importer has no code for: fill opacity, layer effects
        for (const auto& info : L.additionalInfos) {
            if (info->key == "iOpa" && info->dataLength >= 4) {
                const uint8* d = info->data.get();
                const uint32 asInt = ((uint32)d[0] << 24) | ((uint32)d[1] << 16) | ((uint32)d[2] << 8) | d[3];
                float asFloat = 0.0f;
                std::memcpy(&asFloat, &asInt, 4);
                std::printf("         iOpa(fill opacity): int=%u float=%.4f\n", (unsigned)asInt, (double)asFloat);
            }
            if (info->key == "lfx2") {
                // the effects the importer actually reads, through the same parser
                // (img::PSDDescriptor + core::parseLayerEffects) instead of a byte scan
                img::PSDDescriptor descriptor;
                core::LayerEffects fx;
                if (!descriptor.parse(info->data.get(), info->dataLength)) {
                    std::printf("         lfx2: descriptor parse FAILED\n");
                } else {
                    core::parseLayerEffects(descriptor, fx);
                    std::printf("         lfx2 effects present:");
                    for (auto t : fx.presentTypes()) {
                        std::printf(" %s(%s)", core::LayerEffects::typeName(t).toUtf8().constData(),
                            fx.active[t] ? "on" : "off");
                    }
                    std::printf("\n");
                    if (fx.colorOverlay.enabled) {
                        std::printf("           SoCo colour overlay: rgb=(%.3f,%.3f,%.3f) opa=%.3f blend=%s\n",
                            (double)fx.colorOverlay.r, (double)fx.colorOverlay.g, (double)fx.colorOverlay.b,
                            (double)fx.colorOverlay.opacity, fx.colorOverlay.blendMode.c_str());
                    }
                    if (fx.gradientOverlay.enabled) {
                        std::printf("           GrFl gradient: style=%s blend=%s opa=%.3f angle=%.1f "
                                    "scale=%.1f off=(%.3f,%.3f) rev=%d stops=%zu\n",
                            fx.gradientOverlay.style.c_str(), fx.gradientOverlay.blendMode.c_str(),
                            (double)fx.gradientOverlay.opacity, (double)fx.gradientOverlay.angleDeg,
                            (double)fx.gradientOverlay.scale, (double)fx.gradientOverlay.offsetX,
                            (double)fx.gradientOverlay.offsetY, (int)fx.gradientOverlay.reverse,
                            fx.gradientOverlay.stops.size());
                    }
                    if (fx.stroke.enabled) {
                        std::printf("           FrFX stroke: style=%s paint=%s size=%.2f blend=%s opa=%.3f "
                                    "inside=%d drawable=%d",
                            fx.stroke.style.c_str(), fx.stroke.paintType.c_str(), (double)fx.stroke.size,
                            fx.stroke.blendMode.c_str(), (double)fx.stroke.opacity, (int)fx.stroke.isInside(),
                            (int)fx.stroke.isDrawable());
                        if (fx.stroke.isGradient()) {
                            std::printf(" gradient stops=%zu angle=%.1f scale=%.1f", fx.stroke.stops.size(),
                                (double)fx.stroke.angleDeg, (double)fx.stroke.scale);
                        }
                        std::printf("\n");
                    }
                    for (const auto& sl : fx.silhouettes) {
                        std::printf("           silhouette %s: blur=%.2f distance=%.2f angle=%.1f "
                                    "rgb=(%.3f,%.3f,%.3f) opa=%.3f blend=%s under=%d\n",
                            core::LayerEffects::silhouetteName(sl.kind).toUtf8().constData(), (double)sl.blur,
                            (double)sl.distance, (double)sl.angleDeg, (double)sl.r, (double)sl.g, (double)sl.b,
                            (double)sl.opacity, sl.blendMode.c_str(), (int)sl.isUnder());
                    }
                }
            }
        }
        // the stable identity the importer stores (Photoshop's "lyid"), which a source reload
        // matches on: printed so the ids a file actually carries are visible, including whether
        // they are distinct
        {
            const QString stableId = img::psdLayerStableId(L);
            std::printf("         stableId=%s\n",
                stableId.isEmpty() ? "(none)" : stableId.toUtf8().constData());
        }

        // Blend If: the composite pair is what the importer applies; the per-channel pairs are
        // printed too, so a file that sets them (which this build ignores) is visible
        if (L.compositeBlendingRange && L.compositeBlendingRange->isValid) {
            const auto& r = *L.compositeBlendingRange;
            std::printf(
                "         blendIf composite src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d]", (int)r.src[0], (int)r.src[1],
                (int)r.src[2], (int)r.src[3], (int)r.dst[0], (int)r.dst[1], (int)r.dst[2], (int)r.dst[3]);
            for (const auto& chn : L.channels) {
                const auto& c = chn->blendingRange;
                std::printf(" ch%d=[%d,%d,%d,%d|%d,%d,%d,%d]", (int)chn->id, (int)c.src[0], (int)c.src[1],
                    (int)c.src[2], (int)c.src[3], (int)c.dst[0], (int)c.dst[1], (int)c.dst[2], (int)c.dst[3]);
            }
            std::printf("\n");
        }
        if (L.mask)
            std::printf(
                "         mask record: rect=(%d,%d %dx%d) hasReal=%d realRect=(%d,%d %dx%d) defColor=%d flags=0x%02x\n",
                L.mask->rect.left(), L.mask->rect.top(), L.mask->rect.width(), L.mask->rect.height(),
                L.mask->hasReal ? 1 : 0, L.mask->realUserRect.left(), L.mask->realUserRect.top(),
                L.mask->realUserRect.width(), L.mask->realUserRect.height(), (int)L.mask->defaultColor,
                (int)L.mask->flags);

        // per-layer pixel decode checksum (FNV-1a over the interleaved RGBA bytes) for a
        // byte-exact cross-check against an independent decoder (see layers.py)
        {
            auto mem = img::PSDUtil::makeInterleavedImage(hdr, L, img::PSDUtil::ColorFormat_RGBA8);
            if (!mem.data) {
                std::printf("         decode: NULL\n");
            } else {
                uint64_t hsh = 0xcbf29ce484222325ULL;
                for (size_t k = 0; k < mem.size; ++k) {
                    hsh ^= mem.data[k];
                    hsh *= 0x100000001b3ULL;
                }
                std::printf("         decode: %zu bytes fnv1a=%016llx\n", mem.size, (unsigned long long)hsh);
                const QStringList dumpList =
                    qEnvironmentVariable("PROBE_LAYER_DUMP").split(',', Qt::SkipEmptyParts);
                // '*' dumps every layer: the PSD's own names are frequently not typable (this
                // corpus is Shift-JIS/UTF-16 Japanese), and "which layer owns this pixel" is a
                // question only the pixels can answer.
                const bool dumpAll = dumpList.contains("*");
                if ((dumpAll || dumpList.contains(QString::fromStdString(L.name)))
                    && L.rect.width() > 0) {
                    QImage im(L.rect.width(), L.rect.height(), QImage::Format_RGBA8888);
                    for (int y = 0; y < L.rect.height(); ++y)
                        std::memcpy(
                            im.scanLine(y), mem.data + (size_t)y * L.rect.width() * 4, (size_t)L.rect.width() * 4);
                    QString tag = QString::fromStdString(L.name).replace(' ', '_');
                    tag.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
                    im.save(aOutDir + QString("/layer_%1_rec%2.png").arg(tag).arg(i - 1, 2, 10, QLatin1Char('0')));
                }
                delete[] mem.data;
            }
        }
    }

    // layer masks: which layers carry one, and how much of their rect it hides
    for (const auto& lp : lmi.layers) {
        const auto& L = *lp;
        if (!L.mask || !L.mask->hasReal)
            continue;
        std::printf(
            "  MASK layer '%s' rect=(%d,%d %dx%d) defaultColor=%d flags=0x%02x realFlags=0x%02x bg=%d\n",
            L.name.c_str(), L.mask->realUserRect.left(), L.mask->realUserRect.top(), L.mask->realUserRect.width(),
            L.mask->realUserRect.height(), (int)L.mask->defaultColor, (int)L.mask->flags, (int)L.mask->realFlags,
            (int)L.mask->realUserMaskBG);
    }

    const auto& idata = fmt.imageData();
    std::printf("  merged image data: compression=%d channels=%zu\n", (int)idata.compressionId, idata.channels.size());

    // cross-check the production merged-image decode against the independently decoded PNG
    auto mem = img::PSDUtil::makeInterleavedImage(hdr, idata, img::PSDUtil::ColorFormat_RGBA8);
    if (!mem.data) {
        std::printf("  merged decode: FAILED (null)\n");
        return true;
    }
    QImageReader cr(aCompositePng);
    cr.setAllocationLimit(1024);
    const QImage comp = cr.read().convertToFormat(QImage::Format_RGBA8888);
    if (comp.isNull() || comp.width() != (int)hdr.width || comp.height() != (int)hdr.height) {
        std::printf("  merged decode: composite.png unusable for cross-check\n");
        return true;
    }
    long long bad = 0, maxd = 0;
    for (uint32 y = 0; y < hdr.height; ++y) {
        const uint8* row = mem.data + (size_t)y * hdr.width * 4;
        const uint8* crow = comp.constScanLine(y);
        for (uint32 x = 0; x < hdr.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs((int)row[x * 4 + c] - (int)crow[x * 4 + c]);
                if (d) {
                    ++bad;
                    maxd = std::max(maxd, (long long)d);
                }
            }
        }
    }
    std::printf(
        "  merged decode vs composite.png: differing channel samples=%lld maxDelta=%lld (0 == byte-identical)\n", bad,
        maxd);

    // what does the merged alpha channel look like, and does the loader consider it opaque?
    {
        const auto& chans = idata.channels;
        std::printf("  merged channel ids:");
        for (const auto& c : chans)
            std::printf(" %d", (int)c->id);
        std::printf("\n");

        // .anie-independent: dump AE's merged decode to a file for the record
        QImage img((int)hdr.width, (int)hdr.height, QImage::Format_RGBA8888);
        for (uint32 y = 0; y < hdr.height; ++y)
            std::memcpy(img.scanLine((int)y), mem.data + (size_t)y * hdr.width * 4, (size_t)hdr.width * 4);
        img.save(aOutDir + "/merged_decode_AE.png");
    }
    return true;
}

//-------------------------------------------------------------------------------------------------
// 3. print the imported tree (top-first) next to the record list
void dumpTree(const core::ObjectNode& aNode, int aDepth) {
    QString ind;
    ind.fill(' ', aDepth * 2);
    const bool isLayer = aNode.type() == core::ObjectType_Layer;
    float opa = -1.0f;
    if (const core::TimeLine* tl = aNode.timeLine()) {
        if (const core::TimeKey* k = tl->defaultKey(core::TimeKeyType_Opa))
            opa = static_cast<const core::OpaKey*>(k)->opacity();
    }
    img::BlendMode bm = img::BlendMode_Normal;
    bool clipped = false;
    if (const core::Renderer* r = aNode.renderer()) {
        bm = r->blendMode();
        clipped = r->isClipped();
    }
    const QRect r = aNode.initialRect();
    std::printf(
        "%s%-10s %-42s rect=(%d,%d %dx%d) vis=%d clip=%d opa=%.3f blend=%s%s\n", ind.toUtf8().constData(),
        isLayer ? "Layer" : "Folder", aNode.name().toUtf8().constData(), r.left(), r.top(), r.width(), r.height(),
        aNode.isVisible() ? 1 : 0, clipped ? 1 : 0, opa, img::getBlendNameFromBlendMode(bm).toUtf8().constData(),
        aNode.isSlimmedDown() ? " slim" : "");
    for (auto it = aNode.children().begin(); it != aNode.children().end(); ++it)
        dumpTree(**it, aDepth + 1);
}

//-------------------------------------------------------------------------------------------------
std::vector<uint8_t> toViewOrder(const std::vector<uint8_t>& aGlRows, const QSize& aSize) {
    const int w = aSize.width(), h = aSize.height();
    std::vector<uint8_t> out((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const auto* src = aGlRows.data() + (size_t)(h - 1 - y) * w * 4;
        std::memcpy(out.data() + (size_t)y * w * 4, src, (size_t)w * 4);
    }
    return out;
}

// GL readback is the raw accumulated framebuffer: RGB carries alpha-weighted color, so
// un-premultiply before comparing with a straight-alpha reference.
void unpremultiply(uint8_t* aPx) {
    const int a = aPx[3];
    if (a == 0) {
        aPx[0] = aPx[1] = aPx[2] = 0;
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const int v = (aPx[c] * 255 + a / 2) / a;
        aPx[c] = (uint8_t)std::min(255, v);
    }
}

struct Stats {
    long long maxd = 0;
    double mean = 0.0;
    long long over2 = 0, over8 = 0, over32 = 0, over64 = 0;
    long long n = 0;
};

Stats diffStats(const std::vector<uint8_t>& aA, const std::vector<uint8_t>& aB, const QSize& aSize) {
    Stats s;
    const int w = aSize.width(), h = aSize.height();
    long long sum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto* pa = aA.data() + ((size_t)y * w + x) * 4;
            const auto* pb = aB.data() + ((size_t)y * w + x) * 4;
            int m = 0;
            for (int c = 0; c < 3; ++c)
                m = std::max(m, std::abs((int)pa[c] - (int)pb[c]));
            sum += m;
            ++s.n;
            s.maxd = std::max(s.maxd, (long long)m);
            if (m > 2) ++s.over2;
            if (m > 8) ++s.over8;
            if (m > 32) ++s.over32;
            if (m > 64) ++s.over64;
        }
    }
    s.mean = s.n ? (double)sum / (double)s.n : 0.0;
    return s;
}

void printStats(const char* aLabel, const Stats& s) {
    std::printf(
        "  %-22s max=%3lld mean=%7.4f  >2:%8lld (%5.2f%%)  >8:%8lld  >32:%7lld  >64:%7lld\n", aLabel, s.maxd, s.mean,
        s.over2, 100.0 * (double)s.over2 / (double)std::max(1LL, s.n), s.over8, s.over32, s.over64);
}

// ---------------------------------------------------------------------------------------------
// PROBE_CUTOFF=<none|constant|mean|ratio>[,<ratio-anchor>]: set every layer's MESH CONTENT LEVEL,
// leaving the pixels alone.
//
// This is the experiment the artist asked for: render the same file with a cutoff and without
// one, so the only difference between the two renders is the cutoff. (Comparing a render against
// the PSD's stored composite cannot answer it - the composite contains the residue, so removing
// anything can only move the render away from it.)
//
// The cutoff decides CELL EXISTENCE only, i.e. the level at which a pixel counts as content. That
// is production's test (alpha > 10). A cell that exists draws its own texels, faint ones included,
// so the TEXTURE is never touched: only the mesh changes, which is exactly the artist's axis (the
// mesh is the geometry they deform, and the picture it draws is bit-identical).
//
// It is passed to GridMesh::createFromImage as a parameter, NOT applied by masking the pixels
// first. Masking cannot express a lower threshold: zeroing alpha <= T leaves the test "alpha > 10"
// to run on the survivors, so any pixel the mask would drop was already below the bar, and the
// one-pixel alpha dilation in GridMeshCreator::Image then copies a NEIGHBOUR'S alpha into the
// zeroed pixel - the mechanism could only ever ADD content, never relax the level. An earlier
// revision of this probe did mask, and its "cutoff" renders were mostly the unchanged file plus
// that artifact (see S23 in verify_blur).
//
// The formulas, per layer:
//   none      T = 0    (no cutoff: every nonzero alpha is content - the reference "keep everything")
//   constant  T = 10   (production)
//   mean      T = clamp(round(0.05 * mean(alpha : alpha > 2)), 1, 10)
//   ratio     T = clamp(round(10 * (1 - R / anchor)), 1, 10),  R = count(2<a<=10)/count(a>10)
// The clamps only ever RELAX production's bar (they cannot make it stricter than 10); whether that
// is the right direction is part of what the comparison is for.
// The ratio's anchor is the one invented number, so it is a parameter. The default 0.60 is
// FITTED: the anchor that maximises exact agreement with the mean variant over the 51 layers of
// the 114 MB test file. It reaches only 63 % agreement (mean |dT| 1.37), and the residual is
// systematic, not noise - the ratio is far less protective of soft layers (fog: mean 38.3 -> T=2,
// R 0.300 -> T=7; glow: mean 46.6 -> T=2, R 0.190 -> T=8). No anchor reconciles the two, because
// they rank layers differently (Spearman -0.87): 0.375 protects the soft end but drops agreement
// to 37 %. Both anchors are therefore worth running side by side.
static QString gCutPolicy;
static double gCutAnchor = 0.60;

// returns T, and reports what it removes from this layer
static int cutoffForLayer(const uint8* aRgba, long aPixels, long& aOutRemoved, double& aOutInkFrac) {
    aOutRemoved = 0;
    aOutInkFrac = 0.0;
    if (gCutPolicy == "none") {
        aOutInkFrac = 0.0;
        return 0;
    }
    if (gCutPolicy == "fixed") {
        // fixed:<T>: an explicit level, so the parameter can be driven in both directions (T > 10
        // makes the bar stricter than production, T < 10 relaxes it). PROBE_CUTOFF=fixed:30.
        const int T = std::max(0, std::min(255, (int)std::lround(gCutAnchor)));
        long long total = 0, ink = 0;
        for (long i = 0; i < aPixels; ++i) {
            const int a = aRgba[i * 4 + 3];
            total += a;
            if (a <= T) {
                ++aOutRemoved;
                ink += a;
            }
        }
        aOutInkFrac = total ? (double)ink / total : 0.0;
        return T;
    }
    if (gCutPolicy == "none" || gCutPolicy == "constant") {
        const int T = gCutPolicy == "none" ? 0 : 10;
        long long total = 0, ink = 0;
        for (long i = 0; i < aPixels; ++i) {
            const int a = aRgba[i * 4 + 3];
            total += a;
            if (a <= T) {
                ++aOutRemoved;
                ink += a;
            }
        }
        aOutInkFrac = total ? (double)ink / total : 0.0;
        return T;
    }
    long hist[256] = {};
    long long totalInk = 0;
    for (long i = 0; i < aPixels; ++i) {
        const int a = aRgba[i * 4 + 3];
        ++hist[a];
        totalInk += a;
    }
    if (totalInk <= 0) {
        return 0;
    }
    double raw = 0.0;
    if (gCutPolicy == "mean") {
        long long mass = 0;
        long n = 0;
        for (int a = 3; a < 256; ++a) {
            mass += (long long)a * hist[a];
            n += hist[a];
        }
        // the level production derives from the layer (img::MeshContentLevel), not a copy of it:
        // this variant has to be the same rule that ships, or the comparison measures nothing
        raw = (double)img::MeshContentLevel::fromMean(n ? (double)mass / n : 0.0);
        aOutRemoved = 0;
        aOutInkFrac = 0.0;
        long long ink = 0, tot = 0;
        for (int a = 0; a < 256; ++a) {
            tot += (long long)a * hist[a];
        }
        for (int a = 1; a <= raw; ++a) {
            aOutRemoved += hist[a];
            ink += (long long)a * hist[a];
        }
        aOutInkFrac = tot ? (double)ink / tot : 0.0;
        return (int)raw;
    } else if (gCutPolicy == "ratio") {
        long strong = 0, mid = 0;
        for (int a = 11; a < 256; ++a) {
            strong += hist[a];
        }
        for (int a = 3; a <= 10; ++a) {
            mid += hist[a];
        }
        // no strong pixels at all: the ratio is undefined, so treat the layer as soft (cut little)
        const double r = strong ? (double)mid / strong : 1.0;
        raw = 10.0 * (1.0 - r / (gCutAnchor > 1e-6 ? gCutAnchor : 1e-6));
    }
    int T = (int)std::lround(raw);
    T = std::max(1, std::min(10, T));
    long long ink = 0;
    for (int a = 1; a <= T; ++a) {
        aOutRemoved += hist[a];
        ink += (long long)a * hist[a];
    }
    aOutInkFrac = (double)ink / totalInk;
    return T;
}

// PROBE_MESH_MASK=<dir>: write every layer's MESH SILHOUETTE as a PNG next to its pixels, in layer
// coordinates. This is the artist's axis: the mesh is the geometry they deform, and what matters is
// whether its silhouette follows the form they drew. It cannot be read off a render (the picture is
// bit-identical either way) and it cannot be read off a triangle count (that is a proxy - density,
// not shape), so the geometry is written out drawn: every triangle of the mesh, filled.
static void dumpMeshMask(core::ImageKey& aKey, const QSize& aSize, const QString& aDir,
                         const QString& aName) {
    const core::GridMesh& mesh = aKey.data().gridMesh();
    const int tris = mesh.indexCount() / 3;
    if (tris <= 0 || !aSize.isValid()) {
        return;
    }
    QImage out(aSize, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(255, 255, 255, 255), 1.0));
        painter.setBrush(QColor(255, 255, 255, 90));
        const gl::Vector3* pos = mesh.positions();
        const GLuint* idx = mesh.indices();
        QPolygonF tri;
        for (int i = 0; i < tris; ++i) {
            tri.clear();
            bool inside = true;
            for (int k = 0; k < 3; ++k) {
                const GLuint v = idx[i * 3 + k];
                if ((int)v >= mesh.vertexCount()) {
                    inside = false;
                    break;
                }
                tri << QPointF(pos[v].x, pos[v].y);
            }
            if (inside) {
                painter.drawPolygon(tri);
            }
        }
    }
    QDir().mkpath(aDir);
    QString safe = aName;
    safe.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    out.save(QString("%1/%2.png").arg(aDir, safe));
    std::printf("    meshmask %-40s triangles %6d  vertices %6d  rect=(%d,%d %dx%d)\n",
                qPrintable(safe), tris, mesh.vertexCount(), mesh.vertexRect().x(),
                mesh.vertexRect().y(), mesh.vertexRect().width(), mesh.vertexRect().height());
}

// walk the tree and write every layer's mesh silhouette (no cutoff applied by itself)
static void dumpMeshMasks(core::ObjectNode& aNode, const QString& aDir) {
    if (aNode.type() == core::ObjectType_Layer) {
        if (auto* key = dynamic_cast<core::ImageKey*>(aNode.timeLine()->defaultKey(core::TimeKeyType_Image))) {
            if (key->hasImage()) {
                const img::ResourceData& res = *key->data().resource();
                if (res.hasImage()) {
                    dumpMeshMask(*key, res.image().pixelSize(), aDir, aNode.name());
                }
            }
        }
    }
    for (auto* child : aNode.children()) {
        dumpMeshMasks(*child, aDir);
    }
}

// walk the tree and re-mesh every layer at the policy's content level
static void applyCutoffToTree(core::ObjectNode& aNode) {
    if (aNode.type() == core::ObjectType_Layer) {
        if (auto* key = dynamic_cast<core::ImageKey*>(aNode.timeLine()->defaultKey(core::TimeKeyType_Image))) {
            if (key->hasImage()) {
                const img::ResourceData& res = *key->data().resource();
                if (res.hasImage()) {
                    const auto& image = res.image();
                    const QSize size = image.pixelSize();
                    const size_t bytes = (size_t)size.width() * size.height() * 4;
                    if (size.isValid() && bytes > 0) {
                        long removed = 0;
                        double inkFrac = 0.0;
                        const int T = cutoffForLayer((const uint8*)image.data(),
                                                     (long)size.width() * size.height(),
                                                     removed, inkFrac);
                        int cell = std::max((int)core::ImageKey::kDefaultMeshCellSize,
                                            core::Constant::imageCellSizeMin());
                        while (img::GridMeshCreator::getCellTableCount(size, cell)
                               > core::Constant::imageCellCountMax()) {
                            ++cell;
                        }
                        cell = std::min(cell, core::Constant::imageCellSizeMax());
                        // production's mesh first, for a per-layer reference of the same layer's
                        // silhouette under the shipped test; then the policy's mesh, which is what
                        // this run renders. Triangles are a proxy for the silhouette (its density),
                        // not the silhouette itself.
                        key->data().gridMesh().createFromImage(image.data(), size, cell);
                        const int prodTris = key->data().gridMesh().indexCount() / 3;
                        key->data().gridMesh().createFromImage(image.data(), size, cell, T);
                        const int tris = key->data().gridMesh().indexCount() / 3;
                        std::printf("    cutoff %-24s T=%2d  cells<=T %8ld px (%5.2f%% of ink)  "
                                    "triangles %6d vs production %6d (%+6.1f%%)\n",
                            aNode.name().toUtf8().constData(), T, removed, 100.0 * inkFrac, tris,
                            prodTris, prodTris ? 100.0 * (tris - prodTris) / prodTris : 0.0);
                    }
                }
            }
        }
    }
    for (auto* child : aNode.children()) {
        applyCutoffToTree(*child);
    }
}

} // namespace

int main(int argc, char** argv) {
    static ProbeAssertHandler assertHandler;
    static ProbeErrorHandler errorHandler;
    gXCAssertHandler = &assertHandler;
    gXCErrorHandler = &errorHandler;

    {
        QSurfaceFormat format;
        format.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(4);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc, argv);

    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <file.psd> <composite.png> <outDir> [frame]\n", argv[0]);
        return 64;
    }
    const QString psdPath = QString::fromLocal8Bit(argv[1]);
    const QString compPath = QString::fromLocal8Bit(argv[2]);
    const QString outDir = QString::fromLocal8Bit(argv[3]);
    const int frame = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 0;
    QDir().mkpath(outDir);

    if (!dumpPsdRecords(psdPath, compPath, outDir))
        std::printf("  (container parse failed; continuing to the importer anyway)\n");

    // GL context
    QMainWindow window;
    auto* glWidget = new ProbeGLWidget();
    window.setCentralWidget(glWidget);
    window.resize(32, 32);
    window.show();
    app.processEvents();
    if (!glWidget->mGLReady) {
        glWidget->update();
        app.processEvents();
    }
    if (!glWidget->mGLReady) {
        std::fprintf(stderr, "FATAL: GL widget did not initialize\n");
        return 2;
    }
    glWidget->makeCurrent();
    glWidget->mVAO.reset(new gl::VertexArrayObject());
    glWidget->mVAO->bind();
    while (gl::Global::functions().glGetError() != GL_NO_ERROR) {
    }

    // 2. import through the production loader
    std::printf("\n== ImageFileLoader (production import) ==\n");
    StubAnimator animator;
    core::Project project("", animator, nullptr);
    ctrl::ImageFileLoader loader(gl::DeviceInfo::instance());
    loader.setOraImportMode(ctrl::ImageFileLoader::OraImportMode::Layered);
    VerboseReporter reporter;
    QElapsedTimer timer;
    timer.start();
    const bool loaded = loader.load(psdPath, project, reporter);
    const qint64 loadMs = timer.elapsed();
    std::printf("  load()=%s  log='%s'  %lld ms\n", loaded ? "true" : "false", loader.log().toUtf8().constData(), (long long)loadMs);
    if (!loaded)
        return 1;

    const QSize canvas = project.attribute().imageSize();
    std::printf("  canvas=%dx%d\n", canvas.width(), canvas.height());

    if (const QByteArray spec = qgetenv("PROBE_CUTOFF"); !spec.isEmpty()) {
        // both separators are accepted: cutoff_batch.sh passes "<policy>,<value>" and a manual run
        // usually types "<policy>:<value>"
        const QStringList parts =
            QString::fromLatin1(QByteArray(spec).replace(':', ',')).split(',');
        gCutPolicy = parts.value(0).trimmed();
        if (parts.size() > 1) {
            gCutAnchor = parts.value(1).trimmed().toDouble();
        }
        // an unknown policy would silently leave every layer at production's threshold: the run
        // would look like a cutoff experiment while rendering the unchanged file.
        if (gCutPolicy != "none" && gCutPolicy != "constant" && gCutPolicy != "fixed"
            && gCutPolicy != "mean" && gCutPolicy != "ratio") {
            std::fprintf(stderr, "FATAL: unknown PROBE_CUTOFF policy '%s'\n", qPrintable(gCutPolicy));
            return 64;
        }
        std::printf("\n== PROBE_CUTOFF=%s (anchor %.3f) ==\n", qPrintable(gCutPolicy), gCutAnchor);
        if (core::ObjectNode* top = project.objectTree().topNode()) {
            applyCutoffToTree(*top);
        }
    }
    // the silhouette of the mesh this run renders - whatever produced it - so the four variants of a
    // file can be compared on the axis that is actually at stake
    if (const QByteArray maskDir = qgetenv("PROBE_MESH_MASK"); !maskDir.isEmpty()) {
        if (core::ObjectNode* top = project.objectTree().topNode()) {
            std::printf("\n== PROBE_MESH_MASK=%s ==\n", maskDir.constData());
            dumpMeshMasks(*top, QString::fromLocal8Bit(maskDir));
        }
    }

    std::printf("\n== imported object tree (top-first) ==\n");
    if (const core::ObjectNode* top = project.objectTree().topNode())
        dumpTree(*top, 0);

    // optional: force every image key's grid-mesh cell size. A cell size at or above the
    // bitmap size makes GridMesh::createFromImage fall back to createQuadMesh(), i.e. an
    // exact identity quad with no burr reduction, which is the control for "does the layer
    // mesh change what a layer draws?" (PROBE_CELL=30000).
    if (qEnvironmentVariableIsSet("PROBE_CELL")) {
        const int cell = qEnvironmentVariable("PROBE_CELL").toInt();
        int count = 0;
        if (core::ObjectNode* top = project.objectTree().topNode()) {
            core::ObjectNode::Iterator itr(top);
            while (itr.hasNext()) {
                auto node = itr.next();
                if (auto* tl = node->timeLine()) {
                    if (auto* key = dynamic_cast<core::ImageKey*>(tl->defaultKey(core::TimeKeyType_Image))) {
                        key->resetGridMesh(cell);
                        ++count;
                    }
                }
            }
        }
        std::printf("\n== PROBE_CELL=%d: %d image keys re-meshed ==\n", cell, count);
    }

    // 4. render one frame through the production composite wiring
    std::printf("\n== render frame %d ==\n", frame);
    timer.restart();
    const std::vector<uint8_t> raw = scene::fixtureFor(canvas).render(project.objectTree(), scene::makeCamera(canvas), frame);
    std::printf("  render %lld ms, %zu bytes\n", (long long)timer.elapsed(), raw.size());
    if (raw.size() != (size_t)canvas.width() * canvas.height() * 4) {
        std::printf("  unexpected readback size\n");
        return 1;
    }

    std::vector<uint8_t> render = toViewOrder(raw, canvas);
    long long a0 = 0, apart = 0, amin = 255;
    for (size_t i = 0; i < render.size(); i += 4) {
        const int a = render[i + 3];
        amin = std::min<long long>(amin, a);
        if (a == 0) ++a0;
        else if (a != 255) ++apart;
    }
    std::printf(
        "  render alpha: min=%lld transparent=%lld partial=%lld of %lld px\n", amin, a0, apart,
        (long long)canvas.width() * canvas.height());
    for (size_t i = 0; i < render.size(); i += 4)
        unpremultiply(&render[i]);
    scene::dumpPNG(outDir + "/render_raw_premultiplied.png", raw, canvas); // GL rows -> view orientation
    // straight-alpha, already in view orientation (no flip)
    {
        QImage img(canvas, QImage::Format_RGBA8888);
        for (int y = 0; y < canvas.height(); ++y)
            std::memcpy(img.scanLine(y), render.data() + (size_t)y * canvas.width() * 4, (size_t)canvas.width() * 4);
        img.save(outDir + "/render_straight.png");
    }

    // 5. diff vs the PSD's own stored composite
    std::printf("\n== diff: render vs the composite stored in the PSD ==\n");
    QImageReader cr(compPath);
    cr.setAllocationLimit(2048);
    const QImage comp = cr.read().convertToFormat(QImage::Format_RGBA8888);
    if (comp.isNull()) {
        std::printf("  cannot read %s\n", compPath.toUtf8().constData());
        return 1;
    }
    std::printf("  composite: %dx%d format=%s\n", comp.width(), comp.height(), comp.format() == QImage::Format_RGBA8888 ? "RGBA8888" : "?");
    if (comp.size() != canvas) {
        std::printf("  SIZE MISMATCH: canvas %dx%d vs composite %dx%d\n", canvas.width(), canvas.height(), comp.width(), comp.height());
        return 1;
    }
    std::vector<uint8_t> cref((size_t)canvas.width() * canvas.height() * 4);
    for (int y = 0; y < canvas.height(); ++y)
        std::memcpy(cref.data() + (size_t)y * canvas.width() * 4, comp.constScanLine(y), (size_t)canvas.width() * 4);

    const Stats s = diffStats(render, cref, canvas);
    printStats("whole canvas", s);

    // leave-one-out attribution: hide one node, re-render, re-diff. A large improvement
    // means AE's treatment of that node (or its descendants) is what drives the
    // divergence from the composite.
    {
        struct NodeRef {
            core::ObjectNode* node;
            QString path;
        };
        std::vector<NodeRef> nodes;
        std::function<void(core::ObjectNode*, const QString&)> collect =
            [&](core::ObjectNode* aNode, const QString& aPrefix) {
                for (auto it = aNode->children().begin(); it != aNode->children().end(); ++it) {
                    core::ObjectNode* c = *it;
                    const QString path = aPrefix.isEmpty() ? c->name() : aPrefix + "/" + c->name();
                    nodes.push_back({c, path});
                    collect(c, path);
                }
            };
        if (core::ObjectNode* top = project.objectTree().topNode())
            collect(top, QString());

        struct Entry {
            QString path;
            QRect rect;
            Stats hidden;
        };
        std::vector<Entry> entries;
        auto& fx = scene::fixtureFor(canvas);
        const core::CameraInfo camera = scene::makeCamera(canvas);
        for (const NodeRef& nr : nodes) {
            if (!nr.node->isVisible())
                continue;
            nr.node->setVisibility(false);
            const std::vector<uint8_t> r = fx.render(project.objectTree(), camera, frame);
            nr.node->setVisibility(true);
            std::vector<uint8_t> v = toViewOrder(r, canvas);
            for (size_t i = 0; i < v.size(); i += 4)
                unpremultiply(&v[i]);
            entries.push_back({nr.path, nr.node->initialRect(), diffStats(v, cref, canvas)});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hidden.mean < b.hidden.mean; });
        std::printf(
            "\n  leave-one-out (%zu nodes; baseline mean=%.3f). Hiding a node IMPROVES the match\n"
            "  when 'hidden mean' < baseline: those are the nodes AE renders differently:\n",
            entries.size(), s.mean);
        const int shown = std::min<int>(14, entries.size());
        for (int k = 0; k < shown; ++k) {
            const Entry& e = entries[k];
            std::printf(
                "    hide %-58s rect=(%4d,%4d %4dx%4d) hidden mean=%7.3f  delta=%+7.3f\n",
                e.path.toUtf8().constData(), e.rect.left(), e.rect.top(), e.rect.width(), e.rect.height(),
                e.hidden.mean, e.hidden.mean - s.mean);
        }
        std::printf("    ...\n");
        for (int k = std::max<int>(0, (int)entries.size() - 5); k < (int)entries.size(); ++k) {
            const Entry& e = entries[k];
            std::printf(
                "    hide %-58s rect=(%4d,%4d %4dx%4d) hidden mean=%7.3f  delta=%+7.3f\n",
                e.path.toUtf8().constData(), e.rect.left(), e.rect.top(), e.rect.width(), e.rect.height(),
                e.hidden.mean, e.hidden.mean - s.mean);
        }
        // restore the tree to its imported state and re-render, verifying the baseline
        std::vector<uint8_t> again = toViewOrder(fx.render(project.objectTree(), camera, frame), canvas);
        for (size_t i = 0; i < again.size(); i += 4)
            unpremultiply(&again[i]);
        const Stats s2 = diffStats(again, cref, canvas);
        std::printf("  restored baseline mean=%.3f (must equal %.3f)\n", s2.mean, s.mean);
    }

    // orientation sanity: is the render flipped or globally shifted?
    {
        struct Variant { const char* name; bool flipX; bool flipY; };
        const Variant variants[] = {{"as-is", false, false}, {"flip-X", true, false}, {"flip-Y", false, true}, {"flip-XY", true, true}};
        for (const auto& v : variants) {
            std::vector<uint8_t> alt((size_t)canvas.width() * canvas.height() * 4);
            for (int y = 0; y < canvas.height(); ++y) {
                for (int x = 0; x < canvas.width(); ++x) {
                    const int sx = v.flipX ? canvas.width() - 1 - x : x;
                    const int sy = v.flipY ? canvas.height() - 1 - y : y;
                    std::memcpy(alt.data() + ((size_t)y * canvas.width() + x) * 4,
                                render.data() + ((size_t)sy * canvas.width() + sx) * 4, 4);
                }
            }
            if (v.flipX || v.flipY) {
                const Stats sv = diffStats(alt, cref, canvas);
                std::printf("  [orientation] %-9s mean=%.4f max=%lld\n", v.name, sv.mean, sv.maxd);
            }
        }
    }

    // blend probe: render the stack up to (and including) one named layer, and the same
    // stack without it, so an independent implementation of the blend can be run on the
    // identical inputs and compared with what AE produced.
    if (qEnvironmentVariableIsSet("PROBE_BLEND")) {
        const QString groupName = qEnvironmentVariable("PROBE_GROUP", "more tint");
        const QString layerName = qEnvironmentVariable("PROBE_LAYER", "Layer 49");
        core::ObjectNode* top = project.objectTree().topNode();
        core::ObjectNode* group = nullptr;
        for (auto it = top->children().begin(); it != top->children().end(); ++it)
            if ((*it)->name() == groupName)
                group = *it;
        std::printf("\n== blend probe: group='%s' layer='%s' ==\n", groupName.toUtf8().constData(), layerName.toUtf8().constData());
        if (!group) {
            std::printf("  group not found\n");
        } else {
            bool past = false;
            for (auto it = top->children().begin(); it != top->children().end(); ++it) {
                if (*it == group) {
                    past = true;
                    continue;
                }
                if (!past)
                    (*it)->setVisibility(false); // everything above the group
            }
            auto& fx = scene::fixtureFor(canvas);
            const core::CameraInfo camera = scene::makeCamera(canvas);
            auto dump = [&](const QString& aPath) {
                std::vector<uint8_t> v = toViewOrder(fx.render(project.objectTree(), camera, frame), canvas);
                for (size_t i = 0; i < v.size(); i += 4)
                    unpremultiply(&v[i]);
                QImage img(canvas, QImage::Format_RGBA8888);
                for (int y = 0; y < canvas.height(); ++y)
                    std::memcpy(img.scanLine(y), v.data() + (size_t)y * canvas.width() * 4, (size_t)canvas.width() * 4);
                img.save(aPath);
                const Stats st = diffStats(v, cref, canvas);
                printStats(QString("  " + QFileInfo(aPath).baseName()).toUtf8().constData(), st);
            };
            dump(outDir + "/blend_upto_layer.png");
            for (auto it = group->children().begin(); it != group->children().end(); ++it) {
                if ((*it)->name() == layerName) {
                    (*it)->setVisibility(false);
                    std::printf("  hid %s inside %s\n", layerName.toUtf8().constData(), groupName.toUtf8().constData());
                }
            }
            dump(outDir + "/blend_backdrop.png");
        }
    }

    // greedy search: which layers must be removed for AE's render to look like the
    // composite? (tests whether the stored composite corresponds to the current stack)
    if (qEnvironmentVariableIsSet("PROBE_GREEDY")) {
        struct NodeRef2 {
            core::ObjectNode* node;
            QString path;
        };
        std::vector<NodeRef2> all;
        std::function<void(core::ObjectNode*, const QString&)> collect2 =
            [&](core::ObjectNode* aNode, const QString& aPrefix) {
                for (auto it = aNode->children().begin(); it != aNode->children().end(); ++it) {
                    core::ObjectNode* c = *it;
                    const QString path = aPrefix.isEmpty() ? c->name() : aPrefix + "/" + c->name();
                    all.push_back({c, path});
                    collect2(c, path);
                }
            };
        if (core::ObjectNode* top = project.objectTree().topNode())
            collect2(top, QString());
        auto& fx = scene::fixtureFor(canvas);
        const core::CameraInfo camera = scene::makeCamera(canvas);
        auto currentMean = [&]() {
            std::vector<uint8_t> v = toViewOrder(fx.render(project.objectTree(), camera, frame), canvas);
            for (size_t i = 0; i < v.size(); i += 4)
                unpremultiply(&v[i]);
            return diffStats(v, cref, canvas).mean;
        };
        std::printf("\n== greedy layer removal (baseline mean=%.3f) ==\n", s.mean);
        double best = currentMean();
        for (int step = 0; step < 12; ++step) {
            core::ObjectNode* winner = nullptr;
            QString winnerPath;
            double winnerMean = best;
            for (const NodeRef2& nr : all) {
                if (!nr.node->isVisible())
                    continue;
                nr.node->setVisibility(false);
                const double m = currentMean();
                nr.node->setVisibility(true);
                if (m < winnerMean) {
                    winnerMean = m;
                    winner = nr.node;
                    winnerPath = nr.path;
                }
            }
            if (!winner) {
                std::printf("  step %2d: no removal improves the match (mean=%.3f)\n", step + 1, best);
                break;
            }
            winner->setVisibility(false);
            best = winnerMean;
            std::printf("  step %2d: hide %-58s -> mean=%.3f\n", step + 1, winnerPath.toUtf8().constData(), best);
        }
        std::printf("  best achievable by removing layers: mean=%.3f (composite match would be ~1)\n", best);
    }

    // tile map: localize the divergence
    const int tile = 128;
    const int tw = (canvas.width() + tile - 1) / tile, th = (canvas.height() + tile - 1) / tile;
    std::vector<Stats> tiles((size_t)tw * th);
    std::vector<long long> tileSum((size_t)tw * th, 0);
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            Stats t;
            long long sum = 0;
            int cnt = 0;
            for (int y = ty * tile; y < std::min(canvas.height(), (ty + 1) * tile); ++y) {
                for (int x = tx * tile; x < std::min(canvas.width(), (tx + 1) * tile); ++x) {
                    const auto* pa = render.data() + ((size_t)y * canvas.width() + x) * 4;
                    const auto* pb = cref.data() + ((size_t)y * canvas.width() + x) * 4;
                    int m = 0;
                    for (int c = 0; c < 3; ++c)
                        m = std::max(m, std::abs((int)pa[c] - (int)pb[c]));
                    sum += m;
                    ++cnt;
                    t.maxd = std::max(t.maxd, (long long)m);
                    if (m > 2) ++t.over2;
                }
            }
            t.n = cnt;
            t.mean = cnt ? (double)sum / cnt : 0.0;
            tiles[(size_t)ty * tw + tx] = t;
            tileSum[(size_t)ty * tw + tx] = sum;
        }
    }
    // worst tiles by summed diff
    std::vector<std::pair<long long, int>> order;
    for (size_t i = 0; i < tileSum.size(); ++i)
        order.push_back({tileSum[i], (int)i});
    std::sort(order.rbegin(), order.rend());
    std::printf("  worst %dx%d tiles (sum of max-channel diffs):\n", tile, tile);
    for (int k = 0; k < std::min<int>(20, order.size()); ++k) {
        const int idx = order[k].second;
        const int tx = idx % tw, ty = idx / tw;
        const Stats& t = tiles[idx];
        std::printf(
            "    tile(%2d,%2d) x=%4d y=%4d  mean=%8.3f max=%3lld  >2:%6lld/%lld\n", tx, ty, tx * tile, ty * tile,
            t.mean, t.maxd, t.over2, t.n);
    }
    // coarse map: mean diff per tile, 40 cols
    std::printf("  tile mean-diff map (>9 = '#'):\n");
    for (int ty = 0; ty < th; ++ty) {
        std::printf("    ");
        for (int tx = 0; tx < tw; ++tx) {
            const double m = tiles[(size_t)ty * tw + tx].mean;
            char c = '.';
            if (m > 0.5) c = ':';
            if (m > 2) c = '+';
            if (m > 6) c = '*';
            if (m > 12) c = '#';
            std::printf("%c", c);
        }
        std::printf("\n");
    }

    // heatmap + a crop of the worst tile (10x amplified)
    {
        QImage heat(canvas, QImage::Format_RGBA8888);
        heat.fill(Qt::black);
        for (int y = 0; y < canvas.height(); ++y) {
            auto* row = heat.scanLine(y);
            for (int x = 0; x < canvas.width(); ++x) {
                const auto* pa = render.data() + ((size_t)y * canvas.width() + x) * 4;
                const auto* pb = cref.data() + ((size_t)y * canvas.width() + x) * 4;
                int m = 0;
                for (int c = 0; c < 3; ++c)
                    m = std::max(m, std::abs((int)pa[c] - (int)pb[c]));
                const int v = std::min(255, m * 10);
                row[x * 4 + 0] = (uint8_t)v;
                row[x * 4 + 1] = (uint8_t)(v > 128 ? 255 - (v - 128) * 2 : v);
                row[x * 4 + 2] = (uint8_t)std::min(255, m * 2);
                row[x * 4 + 3] = 255;
            }
        }
        heat.save(outDir + "/diff_heatmap.png");
        if (!order.empty()) {
            const int idx = order[0].second;
            const int tx = idx % tw, ty = idx / tw;
            const QRect crop = QRect(tx * tile, ty * tile, tile, tile).intersected(QRect(QPoint(), canvas));
            heat.copy(crop).save(outDir + "/worst_tile_heat.png");
            QImage rimg(canvas, QImage::Format_RGBA8888);
            for (int y = 0; y < canvas.height(); ++y)
                std::memcpy(rimg.scanLine(y), render.data() + (size_t)y * canvas.width() * 4, (size_t)canvas.width() * 4);
            rimg.copy(crop).save(outDir + "/worst_tile_render.png");
            comp.copy(crop).save(outDir + "/worst_tile_composite.png");
            std::printf("  worst tile crop -> %s/worst_tile_{heat,render,composite}.png\n", outDir.toUtf8().constData());
        }
    }

    scene::clearFixtures();
    std::printf("\ndone. dumps in %s\n", outDir.toUtf8().constData());
    return 0;
}
