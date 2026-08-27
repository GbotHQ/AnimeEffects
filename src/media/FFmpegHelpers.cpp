#include "media/FFmpegHelpers.h"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
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

} // namespace ffmpeg
} // namespace media
