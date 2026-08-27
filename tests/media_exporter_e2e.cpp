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
#include "ctrl/VideoFormat.h"
#include "ctrl/ProjectLoader.h"
#include "gui/AudioPlaybackWidget.h"
#include "util/IProgressReporter.h"

// Project.cpp references the GUI AudioPlaybackWidget::deserialize (only to load
// a .aemus sidecar, which this harness never has). Provide a stub.
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

static int failures = 0;
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

    // Video params: mp4/libx264@yuv420p at native size.
    ctrl::VideoFormat vidFmt;
    vidFmt.name = "mp4";
    vidFmt.label = "MP4";
    vidFmt.icodec = "ppm";
    ctrl::VideoCodec codec;
    codec.name = "libx264";
    codec.label = "H.264";
    codec.icodec = "ppm";
    codec.pixfmts = QStringList{ QStringLiteral("yuv420p") };
    vidFmt.codecs.push_back(codec);

    ctrl::Exporter::CommonParam common;
    common.path = out;
    common.size = QSize(nativeW, nativeH);
    common.frame = util::Range(minFrame, maxFrame);
    common.fps = fps;

    ctrl::Exporter::VideoParam video;
    video.format = vidFmt;
    video.codecIndex = 0;
    video.colorIndex = 0;
    video.bps = 1000;
    video.pixfmt = "yuv420p";

    ctrl::Exporter exporter(project);
    exporter.setOverwriteConfirmer([=](const QString&) -> bool { return true; });
    exporter.setProgressReporter(reporter);

    auto result = exporter.execute(common, video);
    if (!result) {
        QTextStream(stdout) << "FAIL  exporter.execute: " << exporter.log() << "\n";
        QFile::remove(out);
        return 1;
    }

    QTextStream(stdout) << "INFO  exported -> " << out << " (ok)\n";
    check(QFile::exists(out) && QFile(out).size() > 0, "exported file exists + non-empty");

    {
        FILE* p = popen(QStringLiteral("ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,avg_frame_rate "
                                       "-of csv=p=0 \"%1\" 2>/dev/null | head -8").arg(out).toUtf8().constData(), "r");
        if (p) {
            char buf[512];
            bool hasVideo = false;
            while (fgets(buf, sizeof(buf), p)) {
                QTextStream(stdout) << "  ffprobe: " << QString::fromUtf8(buf).trimmed() << "\n";
                if (QString::fromUtf8(buf).contains("video")) hasVideo = true;
            }
            fclose(p);
            check(hasVideo, "ffprobe reports a video stream");
        } else {
            check(false, "could not run ffprobe");
        }
    }

    QFile::remove(out);
    QTextStream(stdout) << "=== result ===\n";
    return failures == 0 ? 0 : 1;
}
