// V1 — media::VideoEncoder correctness + edge cases.
//
// Verification-first: the encoder is exercised headless (no GUI, just Qt Gui
// for QImage), and output is round-tripped back through the SAME linked libav*
// decode path so we assert real fidelity, not a happy-path smoke test.
//
// Checks:
//   * lossless exact round-trip (ffv1 @ RGBA): decoded == source pixels.
//   * standard mp4 (libx264 @ yuv420p): valid, correct dims/fps/frame count.
//   * single frame, bad-param rejection, odd-size normalization.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/media_video_encoder
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QDir>
#include <QTextStream>
#include <QByteArray>

#include "media/VideoEncoder.h"
#include "media/FFmpegHelpers.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// Deterministic synthetic RGBA frame: horizontal gradient + alpha ramp + a
// stepped color band so both luma and chroma vary (catches sws errors).
// Format_RGBA8888 stores bytes R,G,B,A (unpremultiplied); write them directly.
static QImage makeFrame(int w, int h, int idx) {
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            line[4 * x + 0] = uchar((x * 255) / (w == 0 ? 1 : w));                 // R
            line[4 * x + 1] = uchar((y * 255) / (h == 0 ? 1 : h));                 // G
            line[4 * x + 2] = uchar(((x + y + idx) * 255) / ((w + h) == 0 ? 1 : (w + h))); // B
            line[4 * x + 3] = uchar(255 - ((x + y) % 256));                        // A (ramp)
        }
    }
    return img;
}

static bool imagesEqual(const QImage& a, const QImage& b) {
    if (a.size() != b.size()) return false;
    const QImage ra = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage rb = b.convertToFormat(QImage::Format_RGBA8888);
    const int rowBytes = ra.width() * 4;
    for (int y = 0; y < ra.height(); ++y) {
        if (std::memcmp(ra.constBits() + y * ra.bytesPerLine(),
                        rb.constBits() + y * rb.bytesPerLine(),
                        size_t(rowBytes)) != 0)
            return false;
    }
    return true;
}

// Decode every video frame of a file back into RGBA QImages.
static QList<QImage> decodeFrames(const QString& path, int* outW, int* outH, int* outFps) {
    QList<QImage> frames;
    AVFormatContext* fmt = nullptr;
    if (::avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return frames;
    ::avformat_find_stream_info(fmt, nullptr);
    const int vs = ::av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { ::avformat_close_input(&fmt); return frames; }

    const AVCodec* dec = ::avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVCodecContext* ctx = ::avcodec_alloc_context3(dec);
    ::avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
    if (::avcodec_open2(ctx, dec, nullptr) < 0) { ::avformat_close_input(&fmt); return frames; }

    *outW = ctx->width;
    *outH = ctx->height;
    *outFps = fmt->streams[vs]->avg_frame_rate.num
                  ? fmt->streams[vs]->avg_frame_rate.num /
                        (fmt->streams[vs]->avg_frame_rate.den ? fmt->streams[vs]->avg_frame_rate.den : 1)
                  : 0;

    SwsContext* sws = ::sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                                       ctx->width, ctx->height, AV_PIX_FMT_RGBA,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame* frame = ::av_frame_alloc();
    AVPacket* pkt = ::av_packet_alloc();

    auto pull = [&](AVFrame* f) {
        QImage img(ctx->width, ctx->height, QImage::Format_RGBA8888);
        uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
        const int dstStride[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };
        ::sws_scale(sws, f->data, f->linesize, 0, ctx->height, dst, dstStride);
        frames.append(img);
    };

    while (::av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs) {
            ::avcodec_send_packet(ctx, pkt);
            while (::avcodec_receive_frame(ctx, frame) >= 0) pull(frame);
        }
        ::av_packet_unref(pkt);
    }
    ::avcodec_send_packet(ctx, nullptr);
    while (::avcodec_receive_frame(ctx, frame) >= 0) pull(frame);

    ::av_frame_free(&frame);
    ::av_packet_free(&pkt);
    ::sws_freeContext(sws);
    ::avcodec_free_context(&ctx);
    ::avformat_close_input(&fmt);
    return frames;
}

static bool run_lossless_roundtrip() {
    const QList<QImage> src = { makeFrame(64, 48, 0), makeFrame(64, 48, 1), makeFrame(64, 48, 2) };
    const QString out = QDir::temp().filePath("ae_v1_lossless.avi");

    // Lossless codec that accepts 8-bit BGRA (huffyuv is lossless). We feed
    // RGBA; the RGBA<->BGRA sws shuffle is a byte reorder that cancels out on
    // the round-trip, so the decoded frames must match the source exactly.
    media::VideoEncoder::Config cfg;
    cfg.outputPath = out;
    cfg.container = "avi";
    cfg.codec = "huffyuv";
    cfg.width = 64;
    cfg.height = 48;
    cfg.fps = 24;
    cfg.pixFmt = "bgra";

    media::VideoEncoder enc;
    if (!enc.writeAll(src, cfg)) {
        check(false, "lossless encode failed");
        QFile::remove(out);
        return false;
    }

    int w = 0, h = 0, fps = 0;
    const QList<QImage> dec = decodeFrames(out, &w, &h, &fps);
    check(w == 64 && h == 48, "lossless decode dims match");
    check(dec.size() == src.size(), "lossless frame count matches");
    if (dec.size() == src.size()) {
        bool exact = true;
        for (int i = 0; i < dec.size(); ++i)
            if (!imagesEqual(src[i], dec[i])) { exact = false; break; }
        check(exact, "lossless round-trip is pixel-exact (RGBA)");
    }
    QFile::remove(out);
    return true;
}

static bool run_mp4_x264() {
    const QList<QImage> src = { makeFrame(320, 240, 0), makeFrame(320, 240, 1), makeFrame(320, 240, 2),
                                makeFrame(320, 240, 3), makeFrame(320, 240, 4) };
    const QString out = QDir::temp().filePath("ae_v1_x264.mp4");

    media::VideoEncoder::Config cfg;
    cfg.outputPath = out;
    cfg.container = "mp4";
    cfg.codec = "libx264";
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 24;
    cfg.pixFmt = "yuv420p";

    media::VideoEncoder enc;
    if (!enc.writeAll(src, cfg)) {
        check(false, "libx264 encode failed");
        QFile::remove(out);
        return false;
    }

    int w = 0, h = 0, fps = 0;
    const QList<QImage> dec = decodeFrames(out, &w, &h, &fps);
    check(w == 320 && h == 240, "libx264 decode dims match");
    check(fps == 24, "libx264 fps matches");
    check(dec.size() == src.size(), "libx264 frame count matches");
    QFile::remove(out);
    return true;
}

static bool run_single_frame() {
    const QList<QImage> src = { makeFrame(80, 60, 0) };
    const QString out = QDir::temp().filePath("ae_v1_single.png.mp4");
    media::VideoEncoder enc;
    media::VideoEncoder::Config cfg;
    cfg.outputPath = out; cfg.container = "mp4"; cfg.codec = "ffv1";
    cfg.width = 80; cfg.height = 60; cfg.fps = 10; cfg.pixFmt = "rgba";
    check(enc.writeAll(src, cfg), "single-frame encode succeeds");
    int w = 0, h = 0, fps = 0;
    const QList<QImage> dec = decodeFrames(out, &w, &h, &fps);
    check(dec.size() == 1 && w == 80 && h == 60, "single-frame decode succeeds");
    QFile::remove(out);
    return true;
}

static bool run_bad_params() {
    media::VideoEncoder enc;
    media::VideoEncoder::Config cfg;
    cfg.outputPath = QDir::temp().filePath("ae_v1_bad.mp4");
    cfg.container = "mp4"; cfg.codec = "libx264"; cfg.width = 64; cfg.height = 64; cfg.fps = 0; // invalid
    check(!enc.open(cfg), "fps=0 rejected (open returns false)");
    return true;
}

static bool run_odd_size_norm() {
    const QSize n = media::VideoEncoder::normalizedSize(QSize(321, 241), "yuv420p");
    check(n.width() == 322 && n.height() == 242, "odd yuv size normalized to even (322x242)");
    return true;
}

static bool run_container_coverage() {
    struct Case { const char* container; const char* codec; const char* pixFmt; };
    const Case cases[] = {
        { "mp4",  "libx264",   "yuv420p" },
        { "webm", "libvpx-vp9", "yuv420p" },
        { "mov",  "prores_ks", "" },
        { "avi",  "mpeg4",     "yuv420p" },
        { "ogv",  "libtheora", "yuv420p" },
        { "mkv",  "ffv1",      "bgra" },
    };
    const QList<QImage> src = { makeFrame(128, 96, 0), makeFrame(128, 96, 1) };
    bool all = true;
    for (const auto& c : cases) {
        if (!media::ffmpeg::encoderAvailable(c.codec)) {
            QTextStream(stdout) << "SKIP  " << c.container << "/" << c.codec
                                << " (encoder absent in this build)\n";
            continue;
        }
        const QString out = QDir::temp().filePath(QString("ae_v1_%1.%2").arg(c.container, c.container));
        media::VideoEncoder enc;
        media::VideoEncoder::Config cfg;
        cfg.outputPath = out; cfg.container = c.container; cfg.codec = c.codec;
        cfg.width = 128; cfg.height = 96; cfg.fps = 15; cfg.pixFmt = c.pixFmt;
        bool ok = enc.writeAll(src, cfg);
        int w = 0, h = 0, fps = 0;
        const QList<QImage> dec = ok ? decodeFrames(out, &w, &h, &fps) : QList<QImage>();
        const QSize norm = media::VideoEncoder::normalizedSize(QSize(128, 96), c.pixFmt);
        // Valid decodable file with the right dimensions. Some codecs (theora)
        // introduce a 1-frame delay, so do not assert an exact count here —
        // exact count correctness is covered by the lossless + libx264 tests.
        bool ok2 = ok && !dec.isEmpty() && w == norm.width() && h == norm.height();
        QTextStream(stdout) << (ok2 ? "PASS  " : "FAIL  ")
                            << c.container << "/" << c.codec
                            << (ok ? ("  -> " + QString::number(dec.size()) + " frames, " +
                                      QString::number(w) + "x" + QString::number(h)) : "  (encode failed)")
                            << "\n";
        if (!ok2) all = false;
        QFile::remove(out);
    }
    check(all, "all available container/codec pairs encode + decode correctly");
    return all;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTextStream(stdout) << "=== V1 media::VideoEncoder suite ===\n";
    run_lossless_roundtrip();
    run_mp4_x264();
    run_single_frame();
    run_bad_params();
    run_odd_size_norm();
    run_container_coverage();
    QTextStream(stdout) << "=== result ===\n";
    return failures == 0 ? 0 : 1;
}
