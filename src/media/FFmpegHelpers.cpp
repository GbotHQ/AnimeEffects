#include "media/FFmpegHelpers.h"

#include <QHash>
#include <QElapsedTimer>
#include <QDir>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
}

namespace media {
namespace ffmpeg {

QString license() {
    return QString::fromUtf8(avcodec_license());
}

QString configuration() {
    return QString::fromUtf8(avcodec_configuration());
}

bool encoderAvailable(const char* aEncoderName) {
    return ::avcodec_find_encoder_by_name(aEncoderName) != nullptr;
}

namespace {

QHash<QString, bool>& probeCache() {
    static QHash<QString, bool> cache;
    return cache;
}

// The one real capability test: open the encoder AND push a frame through it.
// Static capability queries are not trustworthy — avcodec_find_encoder_by_name
// reports encoders with no usable device, and avcodec_get_supported_config
// lists pixel formats the encoder then REFUSES at open time (libx265 advertises
// yuva420p and fails avcodec_open2 with ENOSYS). Only an actual open+encode
// answers the question.
//
// aPixFmt empty = let the encoder choose its first supported format.
bool probeImpl(const QString& aName, const QString& aPixFmt) {
    const AVCodec* codec = ::avcodec_find_encoder_by_name(aName.toUtf8().constData());
    if (!codec) return false; // not compiled into this libavcodec at all

    AVCodecContext* ctx = ::avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width = 64;
    ctx->height = 64;
    ctx->time_base = AVRational{1, 25};
    ctx->framerate = AVRational{25, 1};

    if (!aPixFmt.isEmpty()) {
        ctx->pix_fmt = ::av_get_pix_fmt(aPixFmt.toUtf8().constData());
        if (ctx->pix_fmt == AV_PIX_FMT_NONE) {
            ::avcodec_free_context(&ctx);
            return false;
        }
    } else {
        // Ask the encoder which pixel formats it claims to support rather than
        // assuming yuv420p (FFmpeg 8 removed AVCodec::pix_fmts).
        ctx->pix_fmt = AV_PIX_FMT_NONE;
        const AVPixelFormat* fmts = nullptr;
        if (::avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                          (const void**)&fmts, nullptr) == 0 && fmts) {
            ctx->pix_fmt = fmts[0];
        }
        if (ctx->pix_fmt == AV_PIX_FMT_NONE) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    if (::avcodec_open2(ctx, codec, nullptr) < 0) {
        ::avcodec_free_context(&ctx);
        return false; // e.g. a hardware encoder with no capable device, or a
                      // pixel format the encoder lists but cannot encode
    }

    bool ok = false;
    AVFrame* frame = ::av_frame_alloc();
    if (frame) {
        frame->format = ctx->pix_fmt;
        frame->width = ctx->width;
        frame->height = ctx->height;
        if (::av_frame_get_buffer(frame, 32) >= 0) {
            // Zero it: encoders read every byte, and uninitialised memory would
            // both trip sanitizers and make the probe nondeterministic. Plane
            // heights are NOT all the frame height — chroma planes are
            // subsampled, so sizing them by frame->height overflows the buffer
            // (this corrupted the heap on yuv420p).
            const AVPixFmtDescriptor* desc =
                ::av_pix_fmt_desc_get((AVPixelFormat)frame->format);
            for (int p = 0; p < 4 && frame->data[p]; ++p) {
                int planeH = frame->height;
                if (desc && (p == 1 || p == 2)) {
                    planeH = AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h);
                }
                memset(frame->data[p], 0, (size_t)frame->linesize[p] * (size_t)planeH);
            }
            if (::avcodec_send_frame(ctx, frame) >= 0) {
                AVPacket* pkt = ::av_packet_alloc();
                if (pkt) {
                    const int r = ::avcodec_receive_packet(ctx, pkt);
                    // 0 = produced a packet. EAGAIN = it simply wants more frames
                    // before emitting one (normal for B-frame/lag encoders).
                    ok = (r >= 0 || r == AVERROR(EAGAIN));
                    ::av_packet_free(&pkt);
                }
            }
        }
        ::av_frame_free(&frame);
    }

    ::avcodec_free_context(&ctx);
    return ok;
}

bool cachedProbe(const QString& aName, const QString& aPixFmt) {
    const QString key = aName + QLatin1Char('|') + aPixFmt;
    auto& cache = probeCache();
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    const bool result = probeImpl(aName, aPixFmt);
    cache.insert(key, result);
    return result;
}

} // namespace

bool encoderUsable(const char* aEncoderName) {
    return cachedProbe(QString::fromUtf8(aEncoderName), QString());
}

namespace {

// Allocate a frame matching ctx and zero every plane (encoders read all bytes,
// and uninitialised memory trips sanitizers and makes timings noisy).
AVFrame* allocZeroedFrame(AVCodecContext* aCtx) {
    AVFrame* frame = ::av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = aCtx->pix_fmt;
    frame->width = aCtx->width;
    frame->height = aCtx->height;
    if (::av_frame_get_buffer(frame, 32) < 0) {
        ::av_frame_free(&frame);
        return nullptr;
    }
    const AVPixFmtDescriptor* desc = ::av_pix_fmt_desc_get((AVPixelFormat)frame->format);
    for (int p = 0; p < 4 && frame->data[p]; ++p) {
        int planeH = frame->height;
        if (desc && (p == 1 || p == 2)) {
            planeH = AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h);
        }
        memset(frame->data[p], 0, (size_t)frame->linesize[p] * (size_t)planeH);
    }
    return frame;
}

} // namespace

double encoderThroughput(const QString& aEncoderName, int aWidth, int aHeight,
                         const QString& aPixFmt) {
    const QString key = aEncoderName + QLatin1Char('|') + QString::number(aWidth) +
                        QLatin1Char('x') + QString::number(aHeight) + QLatin1Char('|') + aPixFmt;
    static QHash<QString, double> cache;
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    double result = 0.0;
    const AVCodec* codec = ::avcodec_find_encoder_by_name(aEncoderName.toUtf8().constData());
    AVCodecContext* ctx = codec ? ::avcodec_alloc_context3(codec) : nullptr;
    if (ctx) {
        ctx->width = aWidth;
        ctx->height = aHeight;
        ctx->time_base = AVRational{1, 25};
        ctx->framerate = AVRational{25, 1};
        if (!aPixFmt.isEmpty()) {
            ctx->pix_fmt = ::av_get_pix_fmt(aPixFmt.toUtf8().constData());
        } else {
            ctx->pix_fmt = AV_PIX_FMT_NONE;
            const AVPixelFormat* fmts = nullptr;
            if (::avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                              (const void**)&fmts, nullptr) == 0 && fmts) {
                ctx->pix_fmt = fmts[0];
            }
            if (ctx->pix_fmt == AV_PIX_FMT_NONE) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        if (::avcodec_open2(ctx, codec, nullptr) >= 0) {
            AVFrame* frame = allocZeroedFrame(ctx);
            AVPacket* pkt = ::av_packet_alloc();
            if (frame && pkt) {
                // Enough frames that startup cost is amortised, few enough that
                // this stays cheap even for a slow encoder.
                const int kFrames = 8;
                QElapsedTimer timer;
                timer.start();
                for (int i = 0; i < kFrames; ++i) {
                    if (::avcodec_send_frame(ctx, frame) < 0) break;
                    while (::avcodec_receive_packet(ctx, pkt) >= 0) ::av_packet_unref(pkt);
                }
                // Drain: flushing is part of the real cost and lag-based
                // encoders would otherwise hide it entirely.
                ::avcodec_send_frame(ctx, nullptr);
                while (::avcodec_receive_packet(ctx, pkt) >= 0) ::av_packet_unref(pkt);
                const qint64 ms = timer.elapsed();
                const double secs = (double)ms / 1000.0;
                // Frames pushed, not packets emitted: lag-based encoders emit
                // nothing for the first frames, so packet count understates rate.
                result = (secs > 0.0) ? ((double)kFrames / secs) : 0.0;
            }
            if (pkt) ::av_packet_free(&pkt);
            if (frame) ::av_frame_free(&frame);
        }
        ::avcodec_free_context(&ctx);
    }

    cache.insert(key, result);
    return result;
}

// True iff the encoder can actually be opened AND encode a frame USING this
// pixel format. encoderSupportsPixFormat() only queries the encoder's claimed
// list; libx265 is a live counter-example (lists yuva420p, refuses at open).
bool encoderSupportsPixFormatUsable(const QString& aEncoderName, const QString& aPixFmt) {
    if (aPixFmt.isEmpty()) return encoderUsable(aEncoderName.toUtf8().constData());
    return cachedProbe(aEncoderName, aPixFmt);
}

QStringList availableVideoEncoders() {
    QStringList names;
    const AVCodec* codec = nullptr;
    void* opaque = nullptr;
    while ((codec = ::av_codec_iterate(&opaque)) != nullptr) {
        if (codec->type == AVMEDIA_TYPE_VIDEO && ::av_codec_is_encoder(codec)) {
            if (codec->name != nullptr) {
                names.append(QString::fromUtf8(codec->name));
            }
        }
    }
    // Deterministic, human-usable ordering (av_codec_iterate order is by
    // registration, not by name).
    std::sort(names.begin(), names.end());
    return names;
}

QString resolveEncoderName(const QString& aName) {
    // Friendly names (from the export-preset and the dialog's per-container
    // encoder combos) that differ from the libav encoder name. Names that are
    // already libav encoder names pass through unchanged.
    static const QHash<QString, QString> aliases = {
        { QStringLiteral("theora"), QStringLiteral("libtheora") },
        { QStringLiteral("x264"),    QStringLiteral("libx264") },
        { QStringLiteral("x265"),    QStringLiteral("libx265") },
        { QStringLiteral("h264"),    QStringLiteral("libx264") },
        { QStringLiteral("vp8"),     QStringLiteral("libvpx") },
        { QStringLiteral("vp9"),     QStringLiteral("libvpx-vp9") },
        { QStringLiteral("av1"),     QStringLiteral("libaom-av1") },
        { QStringLiteral("mpeg2"),   QStringLiteral("mpeg2video") },
    };
    const QString candidate = aliases.value(aName, aName);
    return encoderAvailable(candidate.toUtf8().constData()) ? candidate : QString();
}

// True iff the given container's muxer (e.g. "mp4", "webm", "mkv") can carry the
// named encoder's codec. Uses avformat_query_codec against the allocated muxer,
// so a requested codec that a container can't hold is REJECTED up front instead
// of surfacing as a confusing avformat_write_header failure.
bool muxerSupportsCodec(const QString& aContainer, const QString& aEncoderName) {
    const AVCodec* codec = ::avcodec_find_encoder_by_name(aEncoderName.toUtf8().constData());
    if (!codec)
        return false;
    AVFormatContext* oc = nullptr;
    const QByteArray fname = QDir::temp().filePath("ae_codec_probe.tmp").toUtf8();
    // "mkv" is not a registered muxer name (ffmpeg uses "matroska").
    const QByteArray container =
        (aContainer == QLatin1String("mkv")) ? QByteArrayLiteral("matroska")
                                             : aContainer.toUtf8();
    if (::avformat_alloc_output_context2(&oc, nullptr, container.constData(),
                                         fname.constData()) < 0) {
        return false;
    }
    const AVOutputFormat* ofmt = oc ? oc->oformat : nullptr;
    const int r = ofmt ? ::avformat_query_codec(ofmt, codec->id, FF_COMPLIANCE_NORMAL) : 0;
    ::avformat_free_context(oc);
    return r > 0;
}

// Pick a default video encoder the given container's muxer can actually carry
// (and that the linked libav* build provides), in priority order. Returns an
// empty string if none of the candidates fit, so the caller can reject cleanly
// instead of muxing a codec the container can't hold.
QString defaultCodecForContainer(const QString& aContainer) {
    static const char* kCandidates[] = {
        "libx264",       "libx265",   "libvpx-vp9", "libvpx",
        "mpeg4",         "libtheora", "ffv1",       "huffyuv",
        "prores_ks",     "utvideo",   "apng",       "libwebp",
    };
    const QByteArray container = aContainer.toUtf8();
    for (const char* name : kCandidates) {
        if (!encoderAvailable(name))
            continue;
        if (muxerSupportsCodec(aContainer, QString::fromUtf8(name)))
            return QString::fromUtf8(name);
    }
    return QString();
}

bool encoderSupportsPixFormat(const QString& aEncoderName, const QString& aPixFmt) {
    static auto pixfmtFromName = [](const QString& n) -> AVPixelFormat {
        if (n == QLatin1String("yuv420p"))  return AV_PIX_FMT_YUV420P;
        if (n == QLatin1String("yuv422p"))  return AV_PIX_FMT_YUV422P;
        if (n == QLatin1String("yuv444p"))  return AV_PIX_FMT_YUV444P;
        if (n == QLatin1String("yuva420p")) return AV_PIX_FMT_YUVA420P;
        if (n == QLatin1String("rgba"))     return AV_PIX_FMT_RGBA;
        if (n == QLatin1String("bgra"))     return AV_PIX_FMT_BGRA;
        if (n == QLatin1String("rgb24"))    return AV_PIX_FMT_RGB24;
        if (n == QLatin1String("gray"))     return AV_PIX_FMT_GRAY8;
        if (n == QLatin1String("yuv444p10le")) return AV_PIX_FMT_YUV444P10LE;
        return AV_PIX_FMT_NONE;
    };
    const AVPixelFormat fmt = pixfmtFromName(aPixFmt);
    if (fmt == AV_PIX_FMT_NONE) {
        // Unknown name: treat as unsupported so a typo is caught, not silently
        // passed to an encoder that may reject it at open() time.
        return false;
    }
    const AVCodec* codec = ::avcodec_find_encoder_by_name(aEncoderName.toUtf8().constData());
    if (!codec)
        return false;
    const void* out = nullptr;
    int n = 0;
    if (::avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &out, &n) < 0)
        return true; // unknown config -> assume any
    const enum AVPixelFormat* list = static_cast<const enum AVPixelFormat*>(out);
    if (!list)
        return true;
    for (int i = 0; list[i] != AV_PIX_FMT_NONE; ++i) {
        if (list[i] == fmt)
            return true;
    }
    return false;
}

} // namespace ffmpeg
} // namespace media
