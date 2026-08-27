#include "media/VideoEncoder.h"

#include <QList>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace media {

namespace {

AVPixelFormat pixfmtFromString(const QString& aName) {
    if (aName == QLatin1String("yuv420p"))  return AV_PIX_FMT_YUV420P;
    if (aName == QLatin1String("yuv422p"))  return AV_PIX_FMT_YUV422P;
    if (aName == QLatin1String("yuv444p"))  return AV_PIX_FMT_YUV444P;
    if (aName == QLatin1String("yuva420p")) return AV_PIX_FMT_YUVA420P;
    if (aName == QLatin1String("rgba"))     return AV_PIX_FMT_RGBA;
    if (aName == QLatin1String("bgra"))     return AV_PIX_FMT_BGRA;
    if (aName == QLatin1String("rgb24"))    return AV_PIX_FMT_RGB24;
    if (aName == QLatin1String("gray"))     return AV_PIX_FMT_GRAY8;
    return AV_PIX_FMT_NONE;
}

bool isYuvPlanar(AVPixelFormat aFmt) {
    switch (aFmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVA420P:
        return true;
    default:
        return false;
    }
}

// Modern (FFmpeg 8) replacement for the deprecated AVCodec::pix_fmts. Returns
// the codec's supported pixel formats (terminated by AV_PIX_FMT_NONE), or NULL
// if all formats are supported. The returned pointer is owned by the codec —
// it must NOT be freed by the caller (verified: it equals the codec's own
// static list).
const enum AVPixelFormat* codecPixFmtList(const AVCodec* aCodec) {
    const void* out = nullptr;
    int n = 0;
    if (::avcodec_get_supported_config(nullptr, aCodec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &out, &n) < 0)
        return nullptr;
    return static_cast<const enum AVPixelFormat*>(out);
}

bool codecSupportsPixFmt(const AVCodec* aCodec, AVPixelFormat aFmt) {
    const enum AVPixelFormat* list = codecPixFmtList(aCodec);
    if (!list)
        return true;
    for (int i = 0; list[i] != AV_PIX_FMT_NONE; ++i) {
        if (list[i] == aFmt)
            return true;
    }
    return false;
}

} // namespace

// pimpl: keep libav* types out of the public header.
struct VideoEncoder::Impl {
    AVFormatContext* mFmt = nullptr;
    AVCodecContext* mCtx = nullptr;
    AVStream* mStream = nullptr;
    SwsContext* mSws = nullptr;
    AVFrame* mFrame = nullptr;
    AVPixelFormat mPixFmt = AV_PIX_FMT_NONE;
    bool mClosed = true;
};

VideoEncoder::VideoEncoder() : mImpl(new Impl()) {}
VideoEncoder::~VideoEncoder() { close(); delete mImpl; }

QSize VideoEncoder::normalizedSize(const QSize& aSize, const QString& aPixFmt) {
    const AVPixelFormat fmt = pixfmtFromString(aPixFmt);
    QSize s = aSize;
    if (fmt != AV_PIX_FMT_NONE && isYuvPlanar(fmt)) {
        if (s.width() % 2 != 0) s.setWidth(s.width() + 1);
        if (s.height() % 2 != 0) s.setHeight(s.height() + 1);
    }
    return s;
}

bool VideoEncoder::open(const Config& aConfig) {
    // Closing any prior session first.
    close();

    mError.clear();
    mWrittenFrames = 0;

    if (aConfig.fps <= 0) { mError = "invalid fps (must be > 0)"; return false; }
    if (aConfig.width <= 0 || aConfig.height <= 0) { mError = "invalid dimensions"; return false; }
    if (aConfig.codec.isEmpty()) { mError = "no codec selected"; return false; }
    if (aConfig.outputPath.isEmpty()) { mError = "no output path"; return false; }

    // Determine target pixel format.
    const AVCodec* codec = ::avcodec_find_encoder_by_name(aConfig.codec.toUtf8().constData());
    if (!codec) { mError = "encoder not found: " + aConfig.codec; return false; }

    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    if (!aConfig.pixFmt.isEmpty()) {
        pixFmt = pixfmtFromString(aConfig.pixFmt);
        if (pixFmt == AV_PIX_FMT_NONE || !codecSupportsPixFmt(codec, pixFmt)) {
            pixFmt = AV_PIX_FMT_NONE; // fall through to auto
        }
    }
    if (pixFmt == AV_PIX_FMT_NONE) {
        if (aConfig.transparent) {
            if (codecSupportsPixFmt(codec, AV_PIX_FMT_RGBA))
                pixFmt = AV_PIX_FMT_RGBA;
            else if (codecSupportsPixFmt(codec, AV_PIX_FMT_YUVA420P))
                pixFmt = AV_PIX_FMT_YUVA420P;
        }
        if (pixFmt == AV_PIX_FMT_NONE && codecPixFmtList(codec))
            pixFmt = codecPixFmtList(codec)[0];
        if (pixFmt == AV_PIX_FMT_NONE)
            pixFmt = AV_PIX_FMT_YUV420P;
    }

    const QSize size = normalizedSize(QSize(aConfig.width, aConfig.height),
                                      QString::fromUtf8(av_get_pix_fmt_name(pixFmt)));
    const int width = size.width();
    const int height = size.height();

    // Muxer. The container name may be an alias that isn't a registered muxer
    // (e.g. "mkv" vs "matroska"); if the named muxer fails, fall back to
    // guessing the muxer from the output file extension.
    const char* container = aConfig.container.isEmpty() ? nullptr
                                                       : aConfig.container.toUtf8().constData();
    if (::avformat_alloc_output_context2(&mImpl->mFmt, nullptr, container,
                                         aConfig.outputPath.toUtf8().constData()) < 0) {
        mImpl->mFmt = nullptr;
        if (::avformat_alloc_output_context2(&mImpl->mFmt, nullptr, nullptr,
                                             aConfig.outputPath.toUtf8().constData()) < 0) {
            mError = "could not allocate muxer for " + aConfig.outputPath;
            return false;
        }
    }

    // Codec context.
    mImpl->mCtx = ::avcodec_alloc_context3(codec);
    if (!mImpl->mCtx) { mError = "could not allocate codec context"; return false; }

    mImpl->mCtx->width = width;
    mImpl->mCtx->height = height;
    mImpl->mCtx->pix_fmt = pixFmt;
    mImpl->mCtx->time_base = AVRational{ 1, aConfig.fps };
    mImpl->mCtx->framerate = AVRational{ aConfig.fps, 1 };
    mImpl->mCtx->gop_size = 12;
    if (aConfig.bitrate > 0)
        mImpl->mCtx->bit_rate = aConfig.bitrate;
    if (aConfig.quality >= 0 &&
        (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_H265)) {
        // Map a 0-100 quality shorthand onto -crf for x264/x265.
        // (Ignored for other codecs; they keep their own default.)
        const int crf = 51 - (aConfig.quality * 51) / 100;
        ::av_opt_set_int(mImpl->mCtx->priv_data, "crf", crf, 0);
    }

    if (::avcodec_open2(mImpl->mCtx, codec, nullptr) < 0) {
        mError = "could not open encoder: " + aConfig.codec;
        return false;
    }
    mImpl->mPixFmt = pixFmt;

    // Stream.
    mImpl->mStream = ::avformat_new_stream(mImpl->mFmt, nullptr);
    if (!mImpl->mStream) { mError = "could not create stream"; return false; }
    mImpl->mStream->id = mImpl->mFmt->nb_streams - 1;
    if (::avcodec_parameters_from_context(mImpl->mStream->codecpar, mImpl->mCtx) < 0) {
        mError = "could not copy codec parameters"; return false;
    }
    mImpl->mStream->time_base = mImpl->mCtx->time_base;
    // Make the real framerate discoverable to decoders/players (avg_frame_rate
    // is not the same as time_base and would otherwise read as 0/0).
    mImpl->mStream->avg_frame_rate = AVRational{ aConfig.fps, 1 };
    mImpl->mStream->r_frame_rate = AVRational{ aConfig.fps, 1 };

    // Open the output container.
    if (::avio_open(&mImpl->mFmt->pb, aConfig.outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
        mError = "could not open output file: " + aConfig.outputPath;
        return false;
    }
    if (::avformat_write_header(mImpl->mFmt, nullptr) < 0) {
        mError = "could not write container header";
        return false;
    }

    // Format converter: QImage is normalized to RGBA before sws.
    mImpl->mSws = ::sws_getContext(width, height, AV_PIX_FMT_RGBA,
                                   width, height, pixFmt, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
    if (!mImpl->mSws) { mError = "could not create swscale context"; return false; }

    // Allocate the input frame.
    mImpl->mFrame = ::av_frame_alloc();
    if (!mImpl->mFrame) { mError = "could not allocate frame"; return false; }
    mImpl->mFrame->format = pixFmt;
    mImpl->mFrame->width = width;
    mImpl->mFrame->height = height;
    if (::av_frame_get_buffer(mImpl->mFrame, 32) < 0) {
        mError = "could not allocate frame buffer";
        return false;
    }

    mImpl->mClosed = false;
    mOpened = true;
    return true;
}

bool VideoEncoder::write(const QImage& aFrame) {
    if (!mOpened) { mError = "encoder not open"; return false; }
    return writeFrame(aFrame);
}

bool VideoEncoder::writeFrame(const QImage& aFrame) {
    QImage rgba = aFrame.convertToFormat(QImage::Format_RGBA8888);
    const int width = mImpl->mCtx->width;
    const int height = mImpl->mCtx->height;

    if (::av_frame_make_writable(mImpl->mFrame) < 0) {
        mError = "could not make frame writable";
        return false;
    }

    // Scale/convert RGBA source into the encoded frame.
    if (mImpl->mPixFmt == AV_PIX_FMT_RGBA && rgba.bytesPerLine() == width * 4) {
        // Same format: direct copy (pixel-exact, no sws pass).
        const uchar* src = rgba.constBits();
        for (int y = 0; y < height; ++y) {
            std::memcpy(mImpl->mFrame->data[0] + y * mImpl->mFrame->linesize[0],
                        src + y * rgba.bytesPerLine(), size_t(width) * 4);
        }
    } else {
        const uint8_t* srcData[4] = { rgba.constBits(), nullptr, nullptr, nullptr };
        const int srcStride[4] = { static_cast<int>(rgba.bytesPerLine()), 0, 0, 0 };
        ::sws_scale(mImpl->mSws, srcData, srcStride, 0, height,
                    mImpl->mFrame->data, mImpl->mFrame->linesize);
    }

    mImpl->mFrame->pts = mWrittenFrames;
    ++mWrittenFrames;

    const int sendRet = ::avcodec_send_frame(mImpl->mCtx, mImpl->mFrame);
    if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
        mError = "avcodec_send_frame failed";
        return false;
    }
    return drainEncoder();
}

bool VideoEncoder::drainEncoder() {
    AVPacket* pkt = ::av_packet_alloc();
    if (!pkt) { mError = "could not allocate packet"; return false; }

    bool ok = true;
    int ret;
    while ((ret = ::avcodec_receive_packet(mImpl->mCtx, pkt)) >= 0) {
        ::av_packet_rescale_ts(pkt, mImpl->mCtx->time_base, mImpl->mStream->time_base);
        pkt->stream_index = mImpl->mStream->index;
        if (::av_interleaved_write_frame(mImpl->mFmt, pkt) < 0) {
            mError = "could not write packet";
            ok = false;
            break;
        }
        ::av_packet_unref(pkt);
    }
    if (ok && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        mError = "avcodec_receive_packet error";
        ok = false;
    }
    ::av_packet_free(&pkt);
    return ok;
}

bool VideoEncoder::writeAll(const QList<QImage>& aFrames, const Config& aConfig) {
    if (!open(aConfig))
        return false;
    for (const QImage& f : aFrames) {
        if (!write(f))
            return false;
    }
    return close();
}

bool VideoEncoder::close() {
    if (!mOpened && mImpl->mClosed)
        return true;

    bool ok = true;
    if (mOpened) {
        // Flush the encoder (send NULL, drain until EOF).
        ::avcodec_send_frame(mImpl->mCtx, nullptr);
        ok = drainEncoder();
        if (::av_write_trailer(mImpl->mFmt) < 0)
            ok = false;
    }

    cleanup();
    return ok;
}

void VideoEncoder::cleanup() {
    if (mImpl->mFrame) { ::av_frame_free(&mImpl->mFrame); }
    if (mImpl->mSws) { ::sws_freeContext(mImpl->mSws); mImpl->mSws = nullptr; }
    if (mImpl->mFmt) {
        if (mImpl->mFmt->pb) { ::avio_closep(&mImpl->mFmt->pb); }
        if (mImpl->mCtx) { ::avcodec_free_context(&mImpl->mCtx); }
        ::avformat_free_context(mImpl->mFmt);
        mImpl->mFmt = nullptr;
    }
    mImpl->mCtx = nullptr;
    mImpl->mStream = nullptr;
    mImpl->mPixFmt = AV_PIX_FMT_NONE;
    mImpl->mClosed = true;
    mOpened = false;
}

} // namespace media
