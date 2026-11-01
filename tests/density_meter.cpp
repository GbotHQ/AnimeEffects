// GATE FOR THE DENSITY METER (gl::FrameStats::dens / dens_max, filled by the density pass
// inside core::MeshTransformer::callGL).
//
// The pass answers one question - how far did the DEFORMATION stretch the texels - and the
// only way to know it answers it correctly is to hand it a deformation whose answer is known
// in advance. So this harness builds the mesh the application builds (an image key's grid
// mesh, through GridMesh::createFromImage), runs the real callGL on it, and reads the frame
// line back:
//
//   [1] UNDEFORMED: every triangle's density is exactly 1.0 - log2 0, bin 8 - and dens_max
//       is 1. The uv area and the rest area are the same numbers, so anything but 1.0 means
//       the two areas are not in the same space (a transposed matrix, a uv source that is
//       not content pixels, a vertex order that does not match).
//   [2] SCALED 2x IN X: density 2.0 (log2 1, bin 10). Area scales with the linear factor in
//       ONE axis, which is what separates an area ratio from a length ratio.
//   [3] SCALED 4x IN BOTH: density 16.0 (log2 4, bin 15 - the top bin, clamped there). This
//       is the only case that exercises the top of the range, where a real project's
//       outliers live.
//   [4] ANIMATED: the deformation AND the layer's placement change on EVERY frame, and every
//       frame must still read exactly the density its own deformation implies. This is the
//       case a still image cannot see: a vertex buffer that is read one frame late, a
//       uniform that is only right on the first call, an inverse computed once, or a
//       readback that repeats the previous frame all pass cases [1]-[3] (which hold their
//       deformation for four frames) and fail here. The placement churn is the sharp end:
//       the pass divides the layer's world/inner matrices out, so a scale, rotation or
//       translation that changes every frame must not reach the number at all.
//
// It also asserts that the measured weight equals the mesh's own total uv area, which proves
// the histogram covered every triangle: the pass drops triangles below kMinUvArea (a quarter
// of a pixel) and a grid mesh has none, so a drop here would be a threshold that is too high.
//
// The line is read two frames after the one it describes (the copy is issued when the next
// frame starts and mapped one frame later, so that the map never waits on the GPU), hence
// FRAMES_PER_CASE frames per still deformation and the two-frame offset in [4].
//
// Run under xvfb-run (it creates a real core-profile context):
//   xvfb-run -a build/tests/Debug/density_meter
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QOpenGLFunctions_4_0_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLWidget>
#include <QSize>
#include <QStringList>
#include <QSurfaceFormat>
#include <QVector>
#include <QVector2D>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "XCAssert.h"
#include "core/GridMesh.h"
#include "core/ImageKey.h"
#include "core/MeshTransformer.h"
#include "core/TimeKeyExpans.h"
#include "gl/DeviceInfo.h"
#include "gl/FrameStats.h"
#include "gl/Global.h"
#include "gl/Vector2.h"
#include "gl/Vector3.h"
#include "gl/VertexArrayObject.h"
#include "util/ArrayBlock.h"

// core::Project.cpp's audio sidecar loader; nothing here loads a project.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

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

int failures = 0;
void check(bool aOk, const char* aWhat) {
    if (!aOk) {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}

// the image the grid is built from: a grid mesh needs an alpha mask, and a fully opaque one
// gives the plainest mesh the application ever draws
constexpr int kImageSize = 64;
constexpr int kCellPx = 8;
// enough frames for the readback pipeline to deliver the frame the case drew (see the top)
constexpr int kFramesPerCase = 4;

float triangleArea(const gl::Vector2& a, const gl::Vector2& b, const gl::Vector2& c) {
    return std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
}

// the bin the pass must put a triangle of this density in: the same arithmetic the geometry
// shader does, mirrored here so the gate reads the line the way a policy would
int binForDensity(double aDensity) {
    if (!(aDensity > 0.0))
        return 0;
    const double log2 = std::log2(aDensity);
    const double raw = (log2 - gl::FrameStats::kDensityBinBase) / gl::FrameStats::kDensityBinStep + 0.5;
    const int bin = (int)std::floor(raw);
    return bin < 0 ? 0 : (bin > gl::FrameStats::kDensityBins - 1 ? gl::FrameStats::kDensityBins - 1 : bin);
}

// one frame line: the 16 area-weighted bins and the largest density of that frame
struct FrameLine {
    bool valid = false;
    double bins[gl::FrameStats::kDensityBins] = {};
    double maxDensity = 0.0;
    double total() const {
        double sum = 0.0;
        for (int b = 0; b < gl::FrameStats::kDensityBins; ++b)
            sum += bins[b];
        return sum;
    }
    int dominant() const {
        int best = 0;
        for (int b = 1; b < gl::FrameStats::kDensityBins; ++b) {
            if (bins[b] > bins[best])
                best = b;
        }
        return best;
    }
};

// every frame line in the stats file, oldest first
QVector<FrameLine> frameLines(const QString& aPath) {
    QVector<FrameLine> result;
    QFile file(aPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray& raw : lines) {
        const QString text = QString::fromUtf8(raw);
        const int densAt = text.indexOf(QStringLiteral("dens=["));
        const int maxAt = text.indexOf(QStringLiteral("dens_max="));
        if (densAt < 0 || maxAt < 0)
            continue;
        const int close = text.indexOf(QLatin1Char(']'), densAt);
        if (close < densAt)
            continue;
        const QStringList bins = text.mid(densAt + 6, close - densAt - 6).split(QLatin1Char(','));
        if (bins.count() != gl::FrameStats::kDensityBins)
            continue;
        FrameLine line;
        for (int b = 0; b < bins.count(); ++b)
            line.bins[b] = bins.at(b).toDouble();
        line.maxDensity = text.mid(maxAt + 9).trimmed().toDouble();
        line.valid = true;
        result.push_back(line);
    }
    return result;
}

void reportLine(const char* aWhat, const FrameLine& aLine) {
    std::printf(
        "INFO %-20s bin=%2d weight=%9.2f/%9.2f dens_max=%.6f log2=%.6f\n",
        aWhat,
        aLine.dominant(),
        aLine.bins[aLine.dominant()],
        aLine.total(),
        aLine.maxDensity,
        aLine.maxDensity > 0.0 ? std::log2(aLine.maxDensity) : -99.0
    );
}

// one frame's reading: the deformation is known, so the whole mesh must be in one bin and
// dens_max must be the density that deformation implies
void checkLine(const FrameLine& aLine, double aDensity, double aTolerance, const char* aWhat) {
    check(aLine.valid, aWhat);
    if (!aLine.valid)
        return;
    const double total = aLine.total();
    const int expected = binForDensity(aDensity);
    check(total > 0.0, "the pass measured some area");
    check(std::fabs(aLine.maxDensity - aDensity) <= aDensity * aTolerance, "dens_max is the known density");
    check(aLine.dominant() == expected, "the weight lands in the bin the deformation implies");
    check(aLine.bins[expected] >= total * 0.999, "and essentially all of it does");
}

} // namespace

// The XC hooks are process-wide symbols the libraries reference: they must have EXTERNAL
// linkage, so they live outside the anonymous namespace.
XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
    QSurfaceFormat::setDefaultFormat(fmt);

    QOpenGLWidget glWidget;
    glWidget.resize(kImageSize, kImageSize);
    glWidget.show();
    glWidget.makeCurrent();
    if (!glWidget.context()) {
        std::fprintf(stderr, "FAIL: no GL context could be created\n");
        return 2;
    }
    auto funcs = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(glWidget.context());
    if (!funcs || !funcs->initializeOpenGLFunctions()) {
        std::fprintf(stderr, "FAIL: no GL function table for the widget's context\n");
        return 2;
    }
    gl::Global::setContext(glWidget);
    gl::Global::setFunctions(*funcs);

    gl::DeviceInfo dev;
    dev.vender = "harness";
    dev.renderer = "harness";
    dev.version = "5";
    dev.maxTextureSize = 16384;
    dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    std::printf(
        "INFO GL renderer=%s version=%s\n",
        reinterpret_cast<const char*>(funcs->glGetString(GL_RENDERER)),
        reinterpret_cast<const char*>(funcs->glGetString(GL_VERSION))
    );
    {
        GLuint e = 0;
        while ((e = funcs->glGetError()) != GL_NO_ERROR) {
        }
    }

    // a core-profile draw needs a bound VAO; the application relies on the viewport's
    // default one staying bound, so the harness does the same
    gl::VertexArrayObject vao;
    vao.bind();

    // the meter reports through FrameStats, which is a singleton built on first use: the
    // variables have to be set before anything touches it
    const QString statsPath = QDir::temp().filePath(QStringLiteral("ae_density_meter_gate.txt"));
    const QString tracePath = statsPath + QStringLiteral(".trace");
    QFile::remove(statsPath);
    QFile::remove(tracePath);
    qputenv("AE_FRAME_STATS", statsPath.toUtf8());
    // the trace is part of the instrument, so the gate runs it (a budget of two layers: it
    // does a synchronous read, and this is a gate, not a benchmark)
    qputenv("AE_FRAME_STATS_DENSITY_TRACE", "2");

    // ---- the mesh, built exactly as an image layer builds it -----------------------------
    QImage mask(kImageSize, kImageSize, QImage::Format_RGBA8888);
    mask.fill(QColor(255, 255, 255, 255));
    core::ImageKey imageKey;
    core::GridMesh& grid = imageKey.data().gridMesh();
    grid.createFromImage(mask.bits(), mask.size(), kCellPx);

    core::TimeKeyExpans expans;
    expans.setAreaImageKey(&imageKey);
    expans.setFFDMesh(&grid);

    const int vertexCount = grid.vertexCount();
    const int triangleCount = (int)grid.indexCount() / 3;
    check(vertexCount > 4, "the image grid has vertices");
    check(triangleCount > 4, "the image grid has triangles");
    if (vertexCount <= 0 || triangleCount <= 0)
        return 1;

    // the mesh's own uv area total: what the histogram must account for in full
    double totalUvArea = 0.0;
    {
        const gl::Vector2* uv = grid.texCoords();
        for (int t = 0; t < triangleCount; ++t) {
            const int i0 = (int)grid.indices()[t * 3 + 0];
            const int i1 = (int)grid.indices()[t * 3 + 1];
            const int i2 = (int)grid.indices()[t * 3 + 2];
            totalUvArea += triangleArea(uv[i0], uv[i1], uv[i2]);
        }
    }
    check(totalUvArea > 0.0, "the mesh has uv area");
    std::printf("INFO mesh verts=%d tris=%d total_uv=%.2f\n", vertexCount, triangleCount, totalUvArea);

    // the mesh's rest positions: every deformation below is a known function of these
    QVector<gl::Vector3> rest(vertexCount);
    for (int i = 0; i < vertexCount; ++i)
        rest[i] = grid.positions()[i];

    // ---- [1]-[3] three known deformations, held still -----------------------------------
    QVector<gl::Vector3> x2(vertexCount);
    QVector<gl::Vector3> xy4(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        const gl::Vector3 p = rest[i];
        x2[i] = gl::Vector3::make(p.x * 2.0f, p.y, p.z);
        xy4[i] = gl::Vector3::make(p.x * 4.0f, p.y * 4.0f, p.z);
    }

    core::MeshTransformer transformer(QStringLiteral(":/shaders/MeshTransformVert.glsl"));
    core::LayerMesh::MeshBuffer& buffer = grid.getMeshBuffer();

    gl::FrameStats& stats = gl::FrameStats::instance();
    check(stats.enabled(), "the meter is enabled by AE_FRAME_STATS");

    auto runFrames = [&](const QVector<gl::Vector3>& aPositions, int aFrames) {
        const util::ArrayBlock<const gl::Vector3> positions(aPositions.constData(), vertexCount);
        for (int frame = 0; frame < aFrames; ++frame) {
            stats.beginFrame();
            // the real pass: transform feedback, then the density draw and its copy
            transformer.callGL(expans, buffer, grid.originOffset(), positions, false, false);
            stats.endFrame(kImageSize, kImageSize);
        }
    };

    struct Case {
        const char* what;
        const QVector<gl::Vector3>* positions;
        double density;
    };
    const Case cases[] = {
        {"undeformed", &rest, 1.0},
        {"scaled 2x in x", &x2, 2.0},
        {"scaled 4x in both", &xy4, 16.0},
    };

    for (const Case& c : cases) {
        const int before = frameLines(statsPath).count();
        runFrames(*c.positions, kFramesPerCase);
        const QVector<FrameLine> lines = frameLines(statsPath);
        if (lines.count() <= before) {
            check(false, "the frame line carries dens=[..] dens_max=..");
            continue;
        }
        const FrameLine line = lines.last();
        reportLine(c.what, line);
        checkLine(line, c.density, 0.002, "the still frame reads the known density");
        // every triangle of this mesh is far above the pass's sliver threshold, so the
        // histogram must account for the mesh's whole uv area
        check(std::fabs(line.total() - totalUvArea) < totalUvArea * 0.001, "every triangle was measured");
    }

    // ---- [4] ANIMATED: a new deformation and a new placement on every frame --------------
    {
        constexpr int kAnimatedFrames = 6;
        QVector<QVector<gl::Vector3>> deformed(kAnimatedFrames);
        QVector<double> density(kAnimatedFrames);
        for (int i = 0; i < kAnimatedFrames; ++i) {
            const float factor = 1.0f + 0.5f * (float)i;
            density[i] = factor;
            QVector<gl::Vector3>& positions = deformed[i];
            positions.resize(vertexCount);
            for (int v = 0; v < vertexCount; ++v)
                positions[v] = gl::Vector3::make(rest[v].x * factor, rest[v].y, rest[v].z);
        }

        const int before = frameLines(statsPath).count();
        for (int i = 0; i < kAnimatedFrames; ++i) {
            // a different PLACEMENT every frame as well: scale, rotation and position. The
            // pass divides world * inner out, so none of this may reach the number.
            expans.srt().setScale(QVector2D(1.0f + 0.4f * (float)i, 0.7f + 0.3f * (float)i));
            expans.srt().setRotate(0.15f * (float)i);
            expans.srt().setPos(QVector2D(7.0f * (float)i, -5.0f * (float)i));

            const util::ArrayBlock<const gl::Vector3> positions(deformed[i].constData(), vertexCount);
            stats.beginFrame();
            transformer.callGL(expans, buffer, grid.originOffset(), positions, false, false);
            stats.endFrame(kImageSize, kImageSize);
        }

        const QVector<FrameLine> lines = frameLines(statsPath);
        // the line a frame writes describes the frame two frames back (the copy is issued
        // when the next frame starts and mapped one frame later), so the frame that wrote
        // lines[before + i] measured deformation i - 2
        for (int i = 2; i < kAnimatedFrames; ++i) {
            if (lines.count() <= before + i) {
                check(false, "the animated frame wrote a line");
                continue;
            }
            const FrameLine& line = lines.at(before + i);
            const double expected = density[i - 2];
            char what[64] = {};
            std::snprintf(what, sizeof(what), "animated frame %d", i);
            reportLine(what, line);
            // the placement churn makes the composed inverse inexact by a few ulps, so the
            // tolerance is relative and small rather than exact
            checkLine(line, expected, 0.002, "the animated frame reads its own deformation");
            check(std::fabs(line.total() - totalUvArea) < totalUvArea * 0.001, "every triangle was measured");
        }
    }

    // the trace is the instrument a real project is read with: it must have run and produced
    // both spaces for the worst triangle
    {
        QFile trace(tracePath);
        const bool opened = trace.open(QIODevice::ReadOnly | QIODevice::Text);
        const QString text = opened ? QString::fromUtf8(trace.readAll()) : QString();
        check(!text.isEmpty(), "the density trace wrote a file");
        check(text.contains(QStringLiteral("det_wi=")), "the trace reports the content/mesh space factor");
        check(text.contains(QStringLiteral("d_mesh=")), "the trace reports the mesh-space density");
    }

    // a line that describes a frame must never carry a NaN or an infinity
    {
        const QVector<FrameLine> lines = frameLines(statsPath);
        bool finite = !lines.isEmpty();
        for (const FrameLine& line : lines) {
            for (int b = 0; b < gl::FrameStats::kDensityBins; ++b)
                finite = finite && std::isfinite(line.bins[b]);
            finite = finite && std::isfinite(line.maxDensity);
        }
        check(finite, "every line is finite");
    }

    if (failures == 0) {
        std::printf("PASS density_meter\n");
        QFile::remove(statsPath);
        QFile::remove(tracePath);
        return 0;
    }
    std::printf("FAIL density_meter (%d checks)\n", failures);
    std::printf("INFO frame lines left at %s\n", qPrintable(statsPath));
    return 1;
}
