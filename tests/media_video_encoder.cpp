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
#include <QFile>
#include <QTextStream>
#include <QByteArray>
#include <QtMath>

#include "media/VideoEncoder.h"
#include "media/FFmpegHelpers.h"
#include "media/AudioMixer.h"

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

// Open a media file and report whether it has video + audio streams, and the
// RMS energy of the decoded audio (so we know the audio stream is not silent).
static bool probeStreams(const QString& path, bool* hasVideo, bool* hasAudio, double* audioRms) {
    *hasVideo = *hasAudio = false;
    *audioRms = 0.0;
    AVFormatContext* fmt = nullptr;
    if (::avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    ::avformat_find_stream_info(fmt, nullptr);
    const int vs = ::av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int as = ::av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    *hasVideo = vs >= 0;
    *hasAudio = as >= 0;

    if (as >= 0) {
        const AVCodec* dec = ::avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
        AVCodecContext* ctx = ::avcodec_alloc_context3(dec);
        ::avcodec_parameters_to_context(ctx, fmt->streams[as]->codecpar);
        ::avcodec_open2(ctx, dec, nullptr);
        AVFrame* fr = ::av_frame_alloc();
        AVPacket* pk = ::av_packet_alloc();
        double sum = 0.0;
        int n = 0;
        const int limit = 200000;
        while (::av_read_frame(fmt, pk) >= 0) {
            if (pk->stream_index == as) {
                ::avcodec_send_packet(ctx, pk);
                while (::avcodec_receive_frame(ctx, fr) >= 0) {
                    const int ch = fr->ch_layout.nb_channels > 0 ? fr->ch_layout.nb_channels : 1;
                    const bool planar = ::av_sample_fmt_is_planar(static_cast<AVSampleFormat>(fr->format));
                    if (planar) {
                        for (int c = 0; c < ch && n < limit; ++c) {
                            const float* d = reinterpret_cast<const float*>(fr->extended_data[c]);
                            for (int i = 0; i < fr->nb_samples && n < limit; ++i, ++n)
                                sum += double(d[i]) * d[i];
                        }
                    } else {
                        const float* d = reinterpret_cast<const float*>(fr->extended_data[0]);
                        for (int i = 0; i < fr->nb_samples * ch && n < limit; ++i, ++n)
                            sum += double(d[i]) * d[i];
                    }
                    if (n >= limit) break;
                }
            }
            ::av_packet_unref(pk);
            if (n >= limit) break;
        }
        ::av_frame_free(&fr);
        ::av_packet_free(&pk);
        ::avcodec_free_context(&ctx);
        *audioRms = n > 0 ? std::sqrt(sum / n) : 0.0;
    }
    ::avformat_close_input(&fmt);
    return true;
}

// Mux a video stream (frames) + one audio stream (from AudioMixer) in a single
// pass, and verify both streams + that the audio is non-silent.
static bool run_video_audio_mux() {
    const QString out = QDir::temp().filePath("ae_v3_mux.mp4");
    const QString wav = QDir::temp().filePath("ae_v3_tone.wav");

    // Generate a stereo 48k sine WAV (1 s), then mix it as the audio track.
    const int rate = 48000;
    QFile wf(wav);
    if (wf.open(QIODevice::WriteOnly)) {
        const int n = rate; // 1 s of samples
        const int block = 4; // stereo 16-bit
        auto w32 = [&](quint32 v) { wf.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](quint16 v) { wf.write(reinterpret_cast<const char*>(&v), 2); };
        wf.write("RIFF", 4); w32(quint32(36 + n * block)); wf.write("WAVE", 4);
        wf.write("fmt ", 4); w32(16); w16(1); w16(2); w32(quint32(rate));
        w32(quint32(rate * block)); w16(quint16(block)); w16(16);
        wf.write("data", 4); w32(quint32(n * block));
        for (int i = 0; i < n; ++i) {
            const qint16 v = qint16(0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / rate) * 32767.0);
            w16(quint16(v)); w16(quint16(v));
        }
    }

    media::AudioMixer::Track tr; tr.path = wav; tr.startFrame = 0; tr.volume = 1.0f;
    media::AudioMixer mx;
    media::AudioMixer::Config acfg;
    acfg.fps = 24; acfg.totalFrames = 24; acfg.sampleRate = rate; acfg.channels = 2;
    check(mx.mix({ tr }, acfg), "video+audio: audio mix succeeds");

    const QList<QImage> frames = { makeFrame(160, 120, 0), makeFrame(160, 120, 1), makeFrame(160, 120, 2) };

    media::VideoEncoder enc;
    media::VideoEncoder::Config cfg;
    cfg.outputPath = out; cfg.container = "mp4"; cfg.codec = "libx264";
    cfg.width = 160; cfg.height = 120; cfg.fps = 24; cfg.pixFmt = "yuv420p";
    cfg.audioCodec = "aac"; cfg.audioChannels = 2; cfg.audioSampleRate = rate; cfg.audioBitrate = 128000;
    check(enc.open(cfg), "video+audio: open with audio succeeds");
    bool wrote = true;
    for (const QImage& f : frames) if (!enc.write(f)) { wrote = false; break; }
    check(wrote, "video+audio: video frames written");
    check(enc.setAudio(mx.samples(), mx.channels(), mx.sampleRate(), mx.sampleCount()),
          "video+audio: setAudio succeeds");
    check(enc.close(), "video+audio: close succeeds");

    bool hasV = false, hasA = false;
    double rms = 0.0;
    probeStreams(out, &hasV, &hasA, &rms);
    check(hasV, "video+audio: output has a video stream");
    check(hasA, "video+audio: output has an audio stream");
    check(rms > 1e-3, "video+audio: audio stream is non-silent (RMS > 1e-3)");

    QFile::remove(out); QFile::remove(wav);
    return true;
}

// Verify the preset -> VideoEncoder::Config bridge (makeConfig) that the
// exporter uses, including a live encode from a makeConfig-built config.
static bool run_make_config() {
    QString err;
    auto c = media::VideoEncoder::makeConfig("mp4", "libx264", "yuv420p", false,
                                             320, 240, 24, 2000, &err);
    check(c.codec == QLatin1String("libx264") && c.bitrate == 2000 * 1000 &&
              c.pixFmt == QLatin1String("yuv420p") && c.container == QLatin1String("mp4"),
          "makeConfig: mp4/libx264/yuv420p maps (codec, bitrate kbps->bps, pixFmt, container)");
    check(err.isEmpty(), "makeConfig: no error for a valid preset");

    auto t = media::VideoEncoder::makeConfig("ogv", "theora", "yuv420p", false,
                                             320, 240, 24, 0, &err);
    check(t.codec == QLatin1String("libtheora"),
          "makeConfig: friendly alias theora -> libtheora");

    auto tr = media::VideoEncoder::makeConfig("mp4", "libx264", "", true,
                                              320, 240, 24, 0, &err);
    // The transparent hint flows through; the actual alpha format is chosen at
    // encoder open() (the build's libx264 may not report yuva420p here).
    check(tr.transparent, "makeConfig: transparent hint passes through");

    QString err2;
    auto bad = media::VideoEncoder::makeConfig("mp4", "definitely_not_a_codec", "", false,
                                               320, 240, 24, 0, &err2);
    check(bad.codec.isEmpty() && !err2.isEmpty(),
          "makeConfig: unknown encoder rejected (empty codec + error)");

    // Live: build a config via makeConfig, then encode + decode-verify.
    auto cfg = media::VideoEncoder::makeConfig("mp4", "libx264", "yuv420p", false,
                                               64, 64, 10, 1000, &err);
    cfg.outputPath = QDir::temp().filePath("ae_v4_cfg.mp4");
    media::VideoEncoder enc;
    const QList<QImage> frames = { makeFrame(64, 64, 0), makeFrame(64, 64, 1) };
    check(enc.writeAll(frames, cfg), "makeConfig-built config encodes successfully");
    int w = 0, h = 0, fps = 0;
    const QList<QImage> dec = decodeFrames(cfg.outputPath, &w, &h, &fps);
    check(dec.size() == 2 && w == 64 && h == 64, "makeConfig-built config decodes correctly");
    QFile::remove(cfg.outputPath);
    return true;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTextStream(stdout) << "=== V1 media::VideoEncoder suite ===\n";    run_lossless_roundtrip();
    run_mp4_x264();
    run_single_frame();
    run_bad_params();
    run_odd_size_norm();
    run_container_coverage();
    run_video_audio_mux();
    run_make_config();
    QTextStream(stdout) << "=== result ===\n";
    return failures == 0 ? 0 : 1;
}
