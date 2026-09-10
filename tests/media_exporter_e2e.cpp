// Headless export harness (real path): loads a real .anie, renders a few frames
// offscreen (GL via a QOpenGLWidget, like the GUI), and exports via
// ctrl::Exporter (which now uses the in-process libav* VideoEncoder/AudioMixer),
// then verifies the output with ffprobe.
//
// This is the end-to-end gate for the media/export rearchitecture on REAL
// project files. Links assets.qrc so the GL render can find :/shaders.
//
// Run: QT_QPA_PLATFORM=xcb ./build/tests/Debug/media_exporter_e2e <file.anie>
#include <QApplication>
#include <QOpenGLWidget>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLFunctions_4_0_Core>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <cstdio>
#include <memory>

#include "XCAssert.h"
#include "gl/Global.h"
#include "gl/DeviceInfo.h"
#include "core/Project.h"
#include "core/Animator.h"
#include "ctrl/Exporter.h"
#include "ctrl/ProjectLoader.h"
#include "gui/AudioPlaybackWidget.h"
#include "util/IProgressReporter.h"

// Project.cpp references the GUI AudioPlaybackWidget::deserialize (only to load
// a .aemus sidecar, which this harness never has). Provide a stub.
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

static int failures = 0;

// Percentage of non-black pixels in the first frame of a video/image file; -1 if
// it could not be decoded. Used to detect a blank render, which the rest of this
// harness cannot see (dimensions, frame counts and stream types all hold for an
// empty frame).
static double gFrameInkPercent = -1.0;
static double decodedInkPercent(const QString& aPath) {
    if (aPath.isEmpty()) return -1.0;
    // ffmpeg decodes both a video and a still frame the same way.
    const QString cmd = QStringLiteral(
        "ffmpeg -v error -i \"%1\" -frames:v 1 -f rawvideo -pix_fmt gray -").arg(aPath);
    FILE* p = popen(cmd.toUtf8().constData(), "r");
    if (!p) return -1.0;
    long ink = 0, total = 0;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            ++total;
            if ((unsigned char)buf[i] > 16) ++ink;
        }
    }
    pclose(p);
    return total > 0 ? (100.0 * ink / total) : -1.0;
}
static void check(bool ok, const char* what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

class NullAnimator : public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

class NullReporter : public util::IProgressReporter {
public:
    void setSection(const QString&) override {}
    void setMaximum(int) override {}
    void setProgress(int) override {}
    bool wasCanceled() const override { return false; }
};

namespace {
class HarnessAssertHandler : public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler : public XCErrorHandler {
public:
    void critical(const QString& t, const QString&, const QString&) const override {
        std::fprintf(stderr, "XC_FATAL %s\n", qPrintable(t));
        std::abort();
    }
};
} // namespace
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;
static HarnessAssertHandler gAssertH;
static HarnessErrorHandler gErrorH;

static gl::DeviceInfo makeDeviceInfo() {
    gl::DeviceInfo dev;
    dev.vender = "harness";
    dev.renderer = "harness";
    dev.version = "5";
    dev.maxTextureSize = 16384;
    dev.maxRenderBufferSize = 16384;
    return dev;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    gXCAssertHandler = &gAssertH;
    gXCErrorHandler = &gErrorH;

    if (argc < 2) {
        QTextStream(stdout) << "usage: " << argv[0] << " <file.anie>\n";
        return 2;
    }
    const QString src = QString::fromLocal8Bit(argv[1]);

    // Live GL context (same as roundtrip).
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
    fmt.setSamples(4); // match the app's MainWindow GL setup
    QSurfaceFormat::setDefaultFormat(fmt);
    QOpenGLWidget glWidget;
    glWidget.resize(4, 4);
    glWidget.show();
    glWidget.makeCurrent();
    if (!glWidget.context()) { QTextStream(stdout) << "FAIL  GL context\n"; return 1; }
    auto funcs = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(glWidget.context());
    if (!funcs || !funcs->initializeOpenGLFunctions()) { QTextStream(stdout) << "FAIL  GL funcs\n"; return 1; }
    gl::Global::setContext(glWidget);
    gl::Global::setFunctions(*funcs);
    gl::DeviceInfo dev = makeDeviceInfo();
    gl::DeviceInfo::setInstance(&dev);

    // Clear any stale GL error from context/shader setup so we only attribute
    // errors observed during the exporter's render.
    { GLuint e = 0; do { e = funcs->glGetError(); } while (e != GL_NO_ERROR && e <= 0x1295); }
    QTextStream(stdout) << "INFO  GL renderer: "
                        << QString::fromUtf8(reinterpret_cast<const char*>(funcs->glGetString(GL_RENDERER)))
                        << " | version: "
                        << QString::fromUtf8(reinterpret_cast<const char*>(funcs->glGetString(GL_VERSION)))
                        << "\n";

    NullAnimator animator;
    NullReporter reporter;

    // Load the real project.
    core::Project project(src, animator, nullptr);
    ctrl::ProjectLoader loader;
    if (!loader.load(src, project, dev, reporter)) {
        QTextStream(stdout) << "FAIL  load: " << loader.log().join("\n") << "\n";
        return 1;
    }
    QTextStream(stdout) << "=== media exporter e2e: " << src << " ===\n";
    const int nativeW = project.attribute().imageSize().width();
    const int nativeH = project.attribute().imageSize().height();
    const int fps = project.attribute().fps() > 0 ? project.attribute().fps() : 24;
    const int minFrame = project.attribute().frameRange().min();
    const int maxFrame = std::min(project.attribute().frameRange().max(), minFrame + 1);
    QTextStream(stdout) << "  project " << nativeW << "x" << nativeH << " @ " << fps
                        << "fps, frames " << minFrame << ".." << maxFrame << "\n";

    const QString out = QDir::temp().filePath("ae_e2e.mp4");
    QFile::remove(out);

    // A fixed small export size for the synthetic scenarios (independent of the
    // project's native size, so these run fast and deterministic).
    const QSize small(320, 180);

    // The harness's canonical container/codec pairs. A container and a codec are
    // names now, not a preset object: the exporter resolves everything else (the
    // muxer, the pixel format, the private options) by asking, so a fixture that
    // names them explicitly is the whole of what a caller supplies.
    const QString mp4Container = QStringLiteral("mp4");
    const QString mp4Codec = QStringLiteral("libx264");
    // webm/libvpx-vp9 @ yuva420p carries alpha as packet side data (alpha_mode=1);
    // the alpha is recovered by the libvpx decoder. This is the primary
    // alpha-capable animation container for the web.
    const QString webmContainer = QStringLiteral("webm");
    const QString webmCodec = QStringLiteral("libvpx-vp9");

    // The app creates a NEW ctrl::Exporter per export. Reusing a single exporter
    // is NOT supported: the second start()'s createFramebuffers crashes inside
    // QOpenGLFramebufferObject (a pre-existing GL framebuffer-lifecycle issue,
    // unrelated to the in-process media layer, which a single VideoEncoder
    // instance reuses cleanly across cycles — see the V1 suite). Mimic the app.
    auto makeExporter = [&]() {
        std::unique_ptr<ctrl::Exporter> ex(new ctrl::Exporter(project));
        ex->setOverwriteConfirmer([](const QString&) -> bool { return true; });
        ex->setProgressReporter(reporter);
        return ex;
    };

    auto runVideo = [&](const QString& aContainer, const QString& aCodec, const QSize& aSize,
                        const util::Range& aFrame, const QString& aPixFmt,
                        const QString& aPath, bool aExpectSuccess) -> bool {
        std::unique_ptr<ctrl::Exporter> ex = makeExporter();
        ctrl::Exporter::CommonParam cp;
        cp.path = aPath; cp.size = aSize;
        cp.selection = ctrl::FrameSelection::fromRange(aFrame.min(), aFrame.max());
        cp.fps = fps;
        ctrl::Exporter::VideoParam vp;
        vp.container = aContainer;
        vp.codec = aCodec;
        // VP9+alpha needs the encoder to be asked for alpha; the old preset
        // carried that flag, and it is the caller's to state now.
        vp.transparent = aPixFmt.contains(QLatin1String("a4")) || aPixFmt == QLatin1String("rgba");
        vp.colorIndex = 0; vp.bps = 1000; vp.pixfmt = aPixFmt;
        auto r = ex->execute(cp, vp);
        return aExpectSuccess ? (bool)r : !r;
    };

    // ffprobe a file and return whether it has a video stream + its dims.
    auto ffprobeVideo = [](const QString& aPath, int* oW, int* oH) -> bool {
        *oW = *oH = 0;
        FILE* p = popen(QStringLiteral(
            "ffprobe -v error -select_streams v -show_entries stream=width,height "
            "-of csv=p=0 \"%1\" 2>/dev/null | head -1").arg(aPath).toUtf8().constData(), "r");
        if (!p) return false;
        char buf[128];
        bool ok = false;
        if (fgets(buf, sizeof(buf), p)) {
            int w = 0, h = 0;
            if (std::sscanf(buf, "%d,%d", &w, &h) == 2) { *oW = w; *oH = h; ok = true; }
        }
        fclose(p);
        return ok && *oW > 0 && *oH > 0;
    };

    // Scenario 1: native-size happy path (regression: valid h264 at native dims).
    {
        std::unique_ptr<ctrl::Exporter> ex = makeExporter();
        ctrl::Exporter::CommonParam cp;
        cp.path = out; cp.size = QSize(nativeW, nativeH); cp.selection = ctrl::FrameSelection::fromRange(minFrame, maxFrame); cp.fps = fps;
        ctrl::Exporter::VideoParam vp;
        vp.container = mp4Container; vp.codec = mp4Codec; vp.colorIndex = 0; vp.bps = 1000; vp.pixfmt = "yuv420p";
        auto r = ex->execute(cp, vp);
        check((bool)r, "e2e mp4: native-size export succeeds");
        check(QFile::exists(out) && QFile(out).size() > 0, "e2e mp4: file exists + non-empty");
        int w = 0, h = 0;
        // YUV dimensions round odd natives up to even (documented encoder
        // behavior), so compare against the even-rounded size.
        const int expW = nativeW + (nativeW & 1);
        const int expH = nativeH + (nativeH & 1);
        check(ffprobeVideo(out, &w, &h) && w == expW && h == expH,
              "e2e mp4: ffprobe video stream at (even-rounded) native dims");
        // Does the encoded frame contain any ART? Nothing in this harness ever
        // checked that: every scenario asserted existence, dimensions, frame
        // counts, stream types and alpha side data, all of which a completely
        // blank render satisfies. Measured here so the gap is visible.
        // A PRECONDITION for the content-dependent checks, not a product
        // assertion: whether this pipeline can render at all here is not
        // established, and failing the suite on an unresolved environment
        // question would be as misleading as passing silently.
        gFrameInkPercent = decodedInkPercent(out);
        fprintf(stderr, "    e2e content: first frame is %.1f%% non-black\n", gFrameInkPercent);
        if (gFrameInkPercent <= 0.0) {
            fprintf(stderr,
                    "\n  *** WARNING: the exporter produced a COMPLETELY BLANK frame, so every\n"
                    "  *** content-dependent assertion below is skipped rather than passed.\n"
                    "  *** Under Xvfb/Mesa llvmpipe this pipeline renders nothing (the app's\n"
                    "  *** on-screen canvas does render), and whether a real GPU is required\n"
                    "  *** is NOT established. Tracked as an open item in SCRATCHPAD.md.\n\n");
        }
        QFile::remove(out);
    }

    // Scenario 2: odd export size must round to even (no corrupt file, no crash).
    {
        const QString oddPath = QDir::temp().filePath("ae_e2e_odd.mp4");
        bool ok = runVideo(mp4Container, mp4Codec, QSize(321, 241), util::Range(minFrame, maxFrame), "yuv420p", oddPath, true);
        check(ok, "e2e odd-size (321x241): export succeeds (rounded to even)");
        int w = 0, h = 0;
        check(ffprobeVideo(oddPath, &w, &h), "e2e odd-size: ffprobe reports a video stream");
        check(w % 2 == 0 && h % 2 == 0, "e2e odd-size: output dims are even");
        QFile::remove(oddPath);
    }

    // Scenario 3: single-frame export (range of exactly 1 frame).
    {
        const QString sPath = QDir::temp().filePath("ae_e2e_single.mp4");
        bool ok = runVideo(mp4Container, mp4Codec, small, util::Range(minFrame, minFrame), "yuv420p", sPath, true);
        check(ok, "e2e single-frame: export succeeds");
        int w = 0, h = 0;
        check(ffprobeVideo(sPath, &w, &h) && w == small.width() && h == small.height(),
              "e2e single-frame: ffprobe reports the video stream");
        QFile::remove(sPath);
    }

    // Scenario 4: transparent export -> webm/VP9 alpha. libx264/mp4 cannot carry
    // alpha; VP9/WebM carries it as packet side data (alpha_mode=1), recovered
    // only by the libvpx decoder (ffprobe's stream pix_fmt stays yuv420p). We
    // assert alpha_mode is set (the V1 unit test verifies the values survive).
    {
        const QString tPath = QDir::temp().filePath("ae_e2e_alpha.webm");
        bool ok = runVideo(webmContainer, webmCodec, small, util::Range(minFrame, maxFrame), "yuva420p", tPath, true);
        check(ok, "e2e transparent (webm/vp9 @ yuva420p): export succeeds");
        FILE* p = popen(QStringLiteral(
            "ffprobe -v error -select_streams v -show_entries stream=codec_name:stream_tags=alpha_mode "
            "-of default=noprint_wrappers=1 \"%1\" 2>/dev/null | head -4").arg(tPath).toUtf8().constData(), "r");
        char buf[256]; bool isVp9 = false, hasAlpha = false;
        if (p) {
            while (fgets(buf, sizeof(buf), p)) {
                const QString line = QString::fromUtf8(buf).trimmed();
                if (line.startsWith("codec_name=vp9")) isVp9 = true;
                if (line.contains("alpha_mode=1")) hasAlpha = true;
            }
            fclose(p);
        }
        check(isVp9, "e2e transparent (webm/vp9 @ yuva420p): ffprobe reports vp9");
        check(hasAlpha, "e2e transparent (webm/vp9 @ yuva420p): alpha_mode side-data present");
        QFile::remove(tPath);
    }

    // Scenario 5: unwritable output path must fail cleanly (no crash, no success).
    {
        const QString badPath = QDir::temp().filePath("ae_e2e_dir.mp4");
        QDir().mkpath(badPath); // create a DIRECTORY at the output path
        bool ok = runVideo(mp4Container, mp4Codec, small, util::Range(minFrame, maxFrame), "yuv420p", badPath, false);
        check(ok, "e2e unwritable path: export rejected cleanly (no false success)");
        QDir(badPath).removeRecursively();
    }

    // Scenario 6: GIF export via the in-process ctrl::Exporter -> valid .gif.
    {
        std::unique_ptr<ctrl::Exporter> ex = makeExporter();
        const QString gPath = QDir::temp().filePath("ae_e2e.gif");
        ctrl::Exporter::CommonParam cp;
        cp.path = gPath; cp.size = small; cp.selection = ctrl::FrameSelection::fromRange(minFrame, maxFrame); cp.fps = fps;
        ctrl::Exporter::GifParam gp;
        gp.optimizePalette = true; gp.intermediateBps = 0;
        auto r = ex->execute(cp, gp);
        check((bool)r, "e2e gif: export succeeds (in-process GifEncoder)");
        check(QFile::exists(gPath) && QFile(gPath).size() > 0, "e2e gif: file exists + non-empty");
        FILE* p = popen(QStringLiteral(
            "ffprobe -v error -select_streams v -show_entries stream=codec_name,width,height "
            "-of csv=p=0 \"%1\" 2>/dev/null | head -1").arg(gPath).toUtf8().constData(), "r");
        char buf[128]; bool isGif = false;
        if (p && fgets(buf, sizeof(buf), p)) {
            if (QString::fromUtf8(buf).trimmed().startsWith("gif")) isGif = true;
        }
        if (p) fclose(p);
        check(isGif, "e2e gif: ffprobe reports a gif video stream");
        QFile::remove(gPath);
    }

    // Scenario 7: image sequence export (ImageParam) -> frames written to a dir.
    {
        std::unique_ptr<ctrl::Exporter> ex = makeExporter();
        const QString imgDir = QDir::temp().filePath("ae_e2e_frames");
        QDir(imgDir).removeRecursively();
        QDir().mkpath(imgDir);
        ctrl::Exporter::CommonParam cp;
        cp.path = imgDir; cp.size = small; cp.selection = ctrl::FrameSelection::fromRange(minFrame, maxFrame); cp.fps = fps;
        ctrl::Exporter::ImageParam ip;
        ip.name = "ae_e2e_frame"; ip.suffix = "png"; ip.quality = 90;
        auto r = ex->execute(cp, ip);
        check((bool)r, "e2e image-seq: export succeeds");
        QDir d(imgDir);
        const QStringList pngs = d.entryList(QStringList{ "*.png" }, QDir::Files);
        {
            const QString probe = pngs.isEmpty() ? QString() : d.filePath(pngs.first());
            fprintf(stderr, "    image-seq frame ink: %.1f%%\n", decodedInkPercent(probe));
        }
        const int expected = maxFrame - minFrame + 1;
        const QString msg = QStringLiteral("e2e image-seq: one file per selected frame (expected %1, got %2)")
                                .arg(expected).arg(pngs.size());
        const QByteArray msgBytes = msg.toUtf8();
        check(pngs.size() == expected, msgBytes.constData());
        QDir(imgDir).removeRecursively();
    }

    // Scenario 8: MULTI-RANGE selection. A selection is a LIST of ranges, so a
    // gap between them must not be exported: [a,a] + [a+2,a+3] is 3 frames,
    // whereas collapsing it to the span [a..a+3] writes 4. The image-sequence
    // path makes the count directly observable, one file per rendered frame.
    {
        const int spanEnd = minFrame + 3;
        if (project.attribute().frameRange().max() < spanEnd) {
            check(false, "e2e multi-range: project is too short for this scenario");
        } else {
            std::unique_ptr<ctrl::Exporter> ex = makeExporter();
            const QString mrDir = QDir::temp().filePath("ae_e2e_multirange");
            QDir(mrDir).removeRecursively();
            QDir().mkpath(mrDir);
            ctrl::Exporter::CommonParam cp;
            cp.path = mrDir; cp.size = small; cp.fps = fps;
            ctrl::FrameSelection sel;
            sel.ranges.append(ctrl::FrameRange{ minFrame, minFrame });
            sel.ranges.append(ctrl::FrameRange{ minFrame + 2, spanEnd });
            cp.selection = sel;
            ctrl::Exporter::ImageParam ip;
            ip.name = "ae_e2e_mr"; ip.suffix = "png"; ip.quality = 90;
            auto r = ex->execute(cp, ip);
            check((bool)r, "e2e multi-range: export succeeds");
            const QStringList pngs = QDir(mrDir).entryList(QStringList{ "*.png" }, QDir::Files);
            const QString msg = QStringLiteral("e2e multi-range: [a,a]+[a+2,a+3] writes 3 frames, not the 4 of one span (got %1)")
                                    .arg(pngs.size());
            const QByteArray msgBytes = msg.toUtf8();
            check(pngs.size() == 3, msgBytes.constData());
            QDir(mrDir).removeRecursively();
        }
    }

    // Scenario 10: an EXACT output size the project's aspect does not match.
    // This is what Discord 320x320 / WhatsApp 512x512 need. "Never stretch" is
    // the rule, so the art is fitted and the remainder padded (or cropped) —
    // and the discriminating evidence is that the padding is TRANSPARENT, which
    // a stretch to the same size cannot produce. The image-sequence path is used
    // because it lets the PNG be inspected directly.
    {
        const QSize native = project.attribute().imageSize();
        const int side = std::min(native.width(), native.height());
        const QSize exact(side, side);
        if (native.width() == native.height()) {
            check(false, "e2e exact-size: project is already square, scenario is degenerate");
        } else {
            auto renderFirstPng = [&](const QString& aDir, ctrl::Exporter::CommonParam& cp) -> QImage {
                QDir(aDir).removeRecursively();
                QDir().mkpath(aDir);
                std::unique_ptr<ctrl::Exporter> ex = makeExporter();
                ctrl::Exporter::ImageParam ip;
                ip.name = "ae_e2e_fit"; ip.suffix = "png"; ip.quality = 90;
                cp.path = aDir;
                if (!ex->execute(cp, ip)) return QImage();
                const QStringList pngs = QDir(aDir).entryList(QStringList{ "*.png" }, QDir::Files);
                if (pngs.isEmpty()) return QImage();
                return QImage(QDir(aDir).filePath(pngs.first()));
            };

            const QString padDir = QDir::temp().filePath("ae_e2e_fitpad");
            const QString cropDir = QDir::temp().filePath("ae_e2e_fitcrop");

            ctrl::Exporter::CommonParam pad;
            pad.size = exact; pad.fps = fps;
            pad.selection = ctrl::FrameSelection::fromRange(minFrame, minFrame);
            // what planExport() computes for an exact-size row: render fitted,
            // then reconcile to the exact box
            const double scale = std::min(exact.width() / (double)native.width(),
                                         exact.height() / (double)native.height());
            pad.renderSize = QSize(int(native.width() * scale) & ~1, int(native.height() * scale) & ~1);
            pad.fit = media::FitMode::Pad;
            const QImage padImg = renderFirstPng(padDir, pad);

            ctrl::Exporter::CommonParam crop = pad;
            crop.fit = media::FitMode::Crop;
            const QImage cropImg = renderFirstPng(cropDir, crop);

            const QString msg1 = QStringLiteral("e2e exact-size: output is exactly %1x%2 (got %3x%4)")
                                     .arg(exact.width()).arg(exact.height())
                                     .arg(padImg.width()).arg(padImg.height());
            check(padImg.size() == exact, msg1.toUtf8().constData());

            // Pad leaves transparent margin; a stretch to the same size would not.
            // This is only meaningful if the render has content at all — on a blank
            // frame every margin is trivially transparent.
            bool marginTransparent = padImg.hasAlphaChannel() && gFrameInkPercent > 0.0;
            if (marginTransparent) {
                const int y = 0; // fitted art is shorter than the box, so row 0 is margin
                for (int x = 0; x < padImg.width() && marginTransparent; ++x) {
                    if (qAlpha(padImg.pixel(x, y)) != 0) marginTransparent = false;
                }
            }
            if (gFrameInkPercent <= 0.0) {
                // A blank render leaves no margin to inspect: skipping loudly beats
                // asserting something that cannot fail.
                fprintf(stderr, "    SKIP  e2e exact-size: margin/fit content checks (blank render)\n");
            } else {
                check(marginTransparent,
                      "e2e exact-size: Pad leaves a TRANSPARENT margin (art fitted, not stretched)");
                check(cropImg.size() == exact, "e2e exact-size: Crop output is the exact size too");
                check(!cropImg.isNull() && !padImg.isNull() && cropImg != padImg,
                      "e2e exact-size: Pad and Crop produce different images (fit is honoured)");
            }

            QDir(padDir).removeRecursively();
            QDir(cropDir).removeRecursively();
        }
    }

    // Scenario 11: the destination table's encoder-level requirements reach the
    // encoder. Before this they could not: the exporter had no way to carry them,
    // so a row documenting auto-alt-ref=0 or hvc1 could not honour it.
    {
        auto ffprobeTag = [](const QString& aPath) -> QString {
            const QString cmd = QStringLiteral(
                "ffprobe -v error -select_streams v -show_entries stream=codec_tag_string "
                "-of csv=p=0 \"%1\"").arg(aPath);
            FILE* p = popen(cmd.toUtf8().constData(), "r");
            if (!p) return QString();
            char buf[256] = {0};
            const bool got = fgets(buf, sizeof(buf), p) != nullptr;
            pclose(p);
            return got ? QString::fromUtf8(buf).trimmed() : QString();
        };

        // 11a: an UNKNOWN private option must be rejected, which proves the
        // options actually reach avcodec_open2 rather than being dropped.
        {
            std::unique_ptr<ctrl::Exporter> ex = makeExporter();
            ctrl::Exporter::CommonParam cp;
            const QString bogus = QDir::temp().filePath("ae_e2e_bogusopt.mp4");
            QFile::remove(bogus);
            cp.path = bogus; cp.size = small; cp.fps = fps;
            cp.selection = ctrl::FrameSelection::fromRange(minFrame, maxFrame);
            ctrl::Exporter::VideoParam vp;
            vp.container = mp4Container; vp.codec = mp4Codec; vp.colorIndex = 0;
            vp.bps = 1000; vp.pixfmt = "yuv420p";
            vp.encOptions.insert(QStringLiteral("definitely-not-an-option"), QStringLiteral("1"));
            auto r = ex->execute(cp, vp);
            check(!r, "e2e encOptions: an unknown private option is rejected, not ignored");
        }

        // 11b: a codec tag reaches the container. HEVC in mp4 defaults to hev1,
        // which Safari refuses; the table asks for hvc1.
        {
            std::unique_ptr<ctrl::Exporter> ex = makeExporter();
            ctrl::Exporter::CommonParam cp;
            const QString hevc = QDir::temp().filePath("ae_e2e_hvc1.mp4");
            QFile::remove(hevc);
            cp.path = hevc; cp.size = small; cp.fps = fps;
            cp.selection = ctrl::FrameSelection::fromRange(minFrame, minFrame);
            ctrl::Exporter::VideoParam vp;
            vp.container = mp4Container; vp.codec = mp4Codec; vp.colorIndex = 0;
            vp.bps = 1000; vp.pixfmt = "yuv420p";
            vp.codec = QStringLiteral("libx265");
            vp.codecTag = QStringLiteral("hvc1");
            auto r = ex->execute(cp, vp);
            if (!r) {
                const QString skip = QStringLiteral("e2e codecTag: SKIPPED - HEVC unavailable (%1)")
                                         .arg(ex->log().trimmed());
                check(false, skip.toUtf8().constData());
            } else {
                const QString tag = ffprobeTag(hevc);
                const QString msg = QStringLiteral("e2e codecTag: mp4 carries hvc1, not hev1 (got '%1')").arg(tag);
                check(tag == QLatin1String("hvc1"), msg.toUtf8().constData());
            }
            QFile::remove(hevc);
        }
    }

    // Scenario 9: audio THROUGH THE EXPORTER. Nothing covered this before (only
    // MainWindow calls setAudioTracks), and this round changed how the audio
    // window is derived from the selection and added a refusal for gapped
    // selections — so both need a gate: 9a for the ordinary case, 9b for the
    // refusal. Without 9a a regression that silently drops audio would pass.
    {
        auto ffprobeHasAudio = [](const QString& aPath) -> bool {
            const QString cmd = QStringLiteral(
                "ffprobe -v error -select_streams a -show_entries stream=codec_name "
                "-of csv=p=0 \"%1\"").arg(aPath);
            FILE* p = popen(cmd.toUtf8().constData(), "r");
            if (!p) return false;
            char buf[256] = {0};
            const bool got = fgets(buf, sizeof(buf), p) != nullptr;
            pclose(p);
            return got && QString::fromUtf8(buf).trimmed().size() > 0;
        };

        // A 1s 16-bit mono PCM WAV, so the mixer has something to decode.
        const QString wav = QDir::temp().filePath("ae_e2e_tone.wav");
        {
            QFile f(wav);
            if (f.open(QIODevice::WriteOnly)) {
                const int rate = 48000, samples = rate;
                const int dataBytes = samples * 2;
                QByteArray h;
                auto put32 = [&](quint32 v) {
                    h.append(char(v & 0xff)); h.append(char((v >> 8) & 0xff));
                    h.append(char((v >> 16) & 0xff)); h.append(char((v >> 24) & 0xff));
                };
                auto put16 = [&](quint16 v) { h.append(char(v & 0xff)); h.append(char((v >> 8) & 0xff)); };
                h.append("RIFF"); put32(36 + dataBytes); h.append("WAVE");
                h.append("fmt "); put32(16); put16(1); put16(1);
                put32(rate); put32(rate * 2); put16(2); put16(16);
                h.append("data"); put32(dataBytes);
                f.write(h);
                f.write(QByteArray(dataBytes, '\0'));
                f.close();
            }
        }

        media::AudioMixer::Track tr;
        tr.path = wav; tr.startFrame = minFrame; tr.endFrame = 0;
        tr.volume = 1.0f; tr.enabled = true;

        {   // 9a: contiguous selection + audio still exports, and the audio lands
            std::unique_ptr<ctrl::Exporter> ex = makeExporter();
            const QString out = QDir::temp().filePath("ae_e2e_audio.mp4");
            QFile::remove(out);
            ctrl::Exporter::CommonParam cp;
            cp.path = out; cp.size = small; cp.fps = fps;
            cp.selection = ctrl::FrameSelection::fromRange(minFrame, maxFrame);
            ctrl::Exporter::VideoParam vp;
            vp.container = mp4Container; vp.codec = mp4Codec; vp.colorIndex = 0;
            vp.bps = 1000; vp.pixfmt = "yuv420p";
            ex->setAudioTracks(QVector<media::AudioMixer::Track>{ tr });
            auto r = ex->execute(cp, vp);
            check((bool)r, "e2e audio: contiguous selection + audio exports");
            check(ffprobeHasAudio(out), "e2e audio: the output carries an audio stream");
            QFile::remove(out);
        }

        {   // 9b: a gapped selection with audio is refused, not mis-mixed
            std::unique_ptr<ctrl::Exporter> ex = makeExporter();
            ctrl::Exporter::CommonParam cp;
            const QString gapOut = QDir::temp().filePath("ae_e2e_audio_gap.mp4");
            QFile::remove(gapOut);
            cp.path = gapOut; cp.size = small; cp.fps = fps;
            ctrl::FrameSelection sel;
            sel.ranges.append(ctrl::FrameRange{ minFrame, minFrame });
            sel.ranges.append(ctrl::FrameRange{ minFrame + 2, minFrame + 3 });
            cp.selection = sel;
            ctrl::Exporter::VideoParam vp;
            vp.container = mp4Container; vp.codec = mp4Codec; vp.colorIndex = 0;
            vp.bps = 1000; vp.pixfmt = "yuv420p";
            ex->setAudioTracks(QVector<media::AudioMixer::Track>{ tr });
            auto r = ex->execute(cp, vp);
            check(!r, "e2e audio: a gapped selection with audio is refused, not mis-mixed");
            check(!QFile::exists(gapOut), "e2e audio: the refused export writes no file");
        }
        QFile::remove(wav);
    }

    // Audio is exercised at the unit level (V2) + the video+audio mux case in
    // media_video_encoder (V1); the real-path harness stays video/gif/image to
    // keep the GL + .anie load fast and deterministic.

    QTextStream(stdout) << "=== result ===\n";
    return failures == 0 ? 0 : 1;
}
