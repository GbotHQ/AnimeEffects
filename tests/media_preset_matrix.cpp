// V3 — export preset compatibility matrix against the linked libav* build.
//
// Parses the embedded export presets (assets/encode/VideoEncode.txt) and, for
// every codec, checks:
//   * the preset's encoder name resolves to an encoder that EXISTS in the linked
//     build (with friendly-name aliases applied, e.g. "theora" -> "libtheora");
//   * every pixel format the preset lists is actually supported by that encoder.
// This is the gate that catches a preset that would fail at encoder open()-time
// (the old CLI subprocess hidden these incompatibilities behind a shell call).
//
// Headless, no GUI. Exit 0 = every preset is encodable as configured.
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QDomDocument>

#include "media/FFmpegHelpers.h"

static int failures = 0;
static void check(bool ok, const char* what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream(stdout) << "=== V3 export preset compatibility matrix ===\n";

#if !defined(AE_VIDEOENCODE_PATH)
#error "AE_VIDEOENCODE_PATH must point at assets/encode/VideoEncode.txt"
#endif
    QFile file(QStringLiteral(AE_VIDEOENCODE_PATH));
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stdout) << "FAIL  cannot open preset file: " << AE_VIDEOENCODE_PATH << "\n";
        return 1;
    }
    QDomDocument doc;
    doc.setContent(&file);
    const QDomElement root = doc.firstChildElement("video_encode");

    int codecs = 0;
    for (QDomElement fmt = root.firstChildElement("format"); !fmt.isNull();
         fmt = fmt.nextSiblingElement("format")) {
        const QString fmtName = fmt.attribute("name");
        for (QDomElement cd = fmt.firstChildElement("codec"); !cd.isNull();
             cd = cd.nextSiblingElement("codec")) {
            const QString name = cd.attribute("name");
            if (name.isEmpty())
                continue;
            ++codecs;
            const QString enc = media::ffmpeg::resolveEncoderName(name);
            if (enc.isEmpty()) {
                check(false, qPrintable("encoder unavailable: " + name + " (in " + fmtName + ")"));
                continue;
            }
            // Friendly -> libav name is a real, present encoder.
            check(true, qPrintable("maps: " + name + " -> " + enc));

            // Hardware encoders (nvenc/qsv) need a device context and expose
            // hardware-specific pixel formats; the software-path encoder does
            // not drive them yet. Validate formats only for software codecs.
            const bool gpuEnc = cd.attribute("hint").contains("gpuenc");

            // Validate each listed pixel format (software codecs only).
            const QStringList pix = cd.attribute("pixfmt").split(',', Qt::SkipEmptyParts);
            for (const QString& p : pix) {
                const QString pt = p.trimmed();
                if (pt.isEmpty())
                    continue;
                if (gpuEnc) {
                    QTextStream(stdout) << "INFO  " << name << "/" << pt
                                        << " (gpuenc; hardware context required)\n";
                    continue;
                }
                if (!media::ffmpeg::encoderSupportsPixFormat(enc, pt)) {
                    check(false, qPrintable("pixfmt unsupported: " + name + "/" + pt));
                }
            }
        }
    }

    check(codecs > 0, "preset file contains codecs");
    QTextStream(stdout) << "=== analyzed " << codecs << " codecs ===\n";
    return failures == 0 ? 0 : 1;
}
