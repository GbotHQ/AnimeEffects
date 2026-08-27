#include "media/FFmpegHelpers.h"

#include <QHash>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
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
    // Friendly preset names that differ from the libav encoder name.
    static const QHash<QString, QString> aliases = {
        { QStringLiteral("theora"), QStringLiteral("libtheora") },
    };
    const QString candidate = aliases.value(aName, aName);
    return encoderAvailable(candidate.toUtf8().constData()) ? candidate : QString();
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
