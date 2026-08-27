#ifndef ANIMEEFFECTS_SRC_MEDIA_FFMPEGHELPERS_H
#define ANIMEEFFECTS_SRC_MEDIA_FFMPEGHELPERS_H

#include <QString>
#include <QStringList>

// Small, GL-free capability probe layer over the linked libav* libraries.
//
// This is intentionally the FIRST surface of the media rearchitecture: it lets
// the exporter (and tests) ask "which FFmpeg build am I linked against, and
// which encoders does it actually provide?" without touching the (removed)
// `ffmpeg` CLI subprocess. Everything here is pure C/libav* over Qt Core — no
// QApplication, no GL — so it is unit-testable headless.
namespace media {
namespace ffmpeg {

// The FFmpeg/library license string (e.g. "LGPL version 2.1 or later").
QString license();

// avcodec_configuration() — the configure flags the linked libavcodec was
// built with; useful for deciding if GPL encoders like libx264 are present.
QString configuration();

// Non-empty iff an encoder with the given name is registered (avcodec
// can find it). e.g. encoderAvailable("libx264").
bool encoderAvailable(const char* aEncoderName);

// Names of all registered VIDEO encoders (type == AVMEDIA_TYPE_VIDEO,
// encode != nullptr), sorted. Used for diagnostics and to gate presets
// against what the linked build actually provides.
QStringList availableVideoEncoders();

} // namespace ffmpeg
} // namespace media

#endif
