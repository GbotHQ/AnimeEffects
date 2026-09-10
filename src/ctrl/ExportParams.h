#ifndef ANIMEEFFECTS_EXPORTPARAMS_H
#define ANIMEEFFECTS_EXPORTPARAMS_H

#include <QString>
#include <QList>
// For UI
#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QWidget>
// For exporter
#include <cmath>
#include <list>
#include <memory>
#include <QSize>
#include <QFileInfo>
#include <QProcess>
#include <QOpenGLFramebufferObject>
#include <utility>
#include "qdir.h"
#include "qmessagebox.h"
#include "gl/Global.h"
#include "gl/EasyTextureDrawer.h"
#include "core/Project.h"
#include "core/TimeInfo.h"
#include "core/ClippingFrame.h"
#include "core/FilterFrame.h"
#include "core/DestinationTexturizer.h"
#include "gl/Util.h"
#include "qpushbutton.h"
#include "qregularexpression.h"
#include "qurl.h"
#include "util/SelectArgs.h"
#include "gui/AudioPlaybackWidget.h"
#include "ExportTarget.h"   // ctrl::QualityBucket

#include <QBuffer>
#include <QDesktopServices>
// I was not meant to do OOP...

// Formats other than video and image are for internal use only, ***DO NOT*** use them on export
enum class exportTarget { video, image, pxFmt, aviEnc, mkvEnc, movEnc, mp4Enc, webmEnc };
enum targetRatio { oneToOne, keep, custom };
enum class availableVideoFormats { apng, avi, f4v, flv, gif, mkv, mov, mp2, mp4, ogv, swf, webm, webp };
enum class availableImageFormats { bmp, jpeg, jpg, png, ppm, xbm, xpm, tiff, webp };
enum class availableIntermediateFormats { png, bmp, jpg, jpeg, ppm };
enum class pixelFormats { Auto, yuv420p, yuva420p, rgb24, rgba, bgr24, bgra, gray };

enum class aviEncoders { Auto, mpeg2, mpeg4 };
enum class mkvEncoders { Auto, x264, x265, vp8, vp9, av1, ffv1, magicyuv, huffyuv, theora };
enum class movEncoders { Auto, libx264, h264, prores_ks, utvideo };
enum class mp4Encoders { Auto, mpeg4, h264 };
enum class webmEncoders { Auto, vp8, vp9 };

struct defaultEncoders {
    aviEncoders avi = aviEncoders::Auto;
    mkvEncoders mkv = mkvEncoders::Auto;
    movEncoders mov = movEncoders::Auto;
    mp4Encoders mp4 = mp4Encoders::Auto;
    webmEncoders webm = webmEncoders::Auto;
};

QStringList const videoFormats{ "apng", "avi", "f4v", "flv", "gif", "mkv", "mov", "mp2", "mp4", "ogv", "swf", "webm", "webp"};
QStringList const audioTargettable{ "avi", "mkv", "mov", "mp4", "webm"};
QStringList const imageFormats{"bmp", "jpeg", "jpg", "png", "ppm", "xbm", "xpm", "tiff", "webp"};
QStringList const intermediateImageFormats{"PNG", "BMP", "JPG", "JPEG", "PPM"};
QStringList const pxFormats{"Auto", "yuv420p", "yuva420p", "rgb24", "rgba", "bgr24", "bgra", "gray"};
QStringList const aviEnc{"Auto", "mpeg2", "mpeg4"};
QStringList const mkvEnc{"Auto", "x264", "x265", "vp8", "vp9", "av1", "ffv1", "magicyuv", "huffyuv", "theora"};
QStringList const movEnc{"Auto", "libx264", "h264", "prores_ks", "utvideo"};
QStringList const mp4Enc{"Auto", "mpeg4", "h264"};
QStringList const webmEnc{"Auto", "vp8", "vp9"};

struct frameExportRange {
    int firstFrame = 0;
    int lastFrame = 0;
};

struct GeneralParams {
    QString fileName = "New Project";
    QString exportName = "New Project Export";
    QDir exportDirectory = QDir();
    QDir exportFileName = QDir();
    QString osExportTarget = "";
    int nativeWidth = 0;
    int nativeHeight = 0;
    int exportWidth = 0;
    int exportHeight = 0;
    targetRatio aspectRatio = keep;
    int nativeFPS = 24;
    int fps = 24;
    int bitrate = 0;
    bool allowTransparency = true;
    bool forcePipe = false;
    bool loop = true;
    bool exportWithAudio = false;
    bool useCustomParam = false;
    bool useIntermediate = true;
    bool usePost = true;
    bool useCustomPalette = false;
    QDir palettePath = QDir();
    QVector<frameExportRange> exportRange = QVector<frameExportRange>();
    QString customInterCommand = "";
    QString customPostCommand = "";
    QVector<int> framesToBeExported = QVector<int>();
    frameExportRange nativeFrameRange = frameExportRange();
    bool exportAllFrames = false;
    bool exportToLast = false;
    int imageExportQuality = -1;
};

struct VideoParams {
    availableVideoFormats format = availableVideoFormats::gif;
    availableIntermediateFormats intermediateFormat = availableIntermediateFormats::png;
    pixelFormats pixelFormat = pixelFormats::Auto;
    defaultEncoders encoders;
};

struct ImageParams {
    availableImageFormats format = availableImageFormats::png;
};

class exportParam {
public:
    exportParam();
    exportTarget exportType = exportTarget::video;
    GeneralParams generalParams;
    // The chosen destination row from assets/export/ExportTargets.txt. Empty means
    // the export is driven by the individual controls instead. When set,
    // ctrl::planExport() decides the container, codec, its private options and
    // stream tag, transparency, the exact size, the caps and the audio.
    QString destinationId;
    ctrl::QualityBucket qualityBucket = ctrl::QualityBucket::Balanced;
    // One of these will be ignored on export //
    VideoParams videoParams;
    ImageParams imageParams;
    // They are available for ease of access ///
};

template<typename formatEnum>
QString getFormatAsString(exportTarget target, formatEnum formatIndex) {
    switch (target) {
    case exportTarget::video:
        return videoFormats[formatIndex];
    case exportTarget::image:
        return imageFormats[formatIndex];
    case exportTarget::pxFmt:
        return pxFormats[formatIndex];
    case exportTarget::aviEnc:
        return aviEnc[formatIndex];
    case exportTarget::mkvEnc:
        return mkvEnc[formatIndex];
    case exportTarget::movEnc:
        return movEnc[formatIndex];
    case exportTarget::mp4Enc:
        return mp4Enc[formatIndex];
    case exportTarget::webmEnc:
        return webmEnc[formatIndex];
    }
    return nullptr;
}
// getFormatAsInt() used to live here, converting a combo's text back to an enum.
// It had no breaks between its switch cases, so every path fell through to
// `default: index = 0`: it returned 0 for EVERY input. That is why the dialog's
// pixel-format and per-container codec choices were silently discarded for as
// long as the control existed, and why removing the controls changed nothing.
// Its callers are gone with the Format tab, so it is deleted rather than fixed.


inline QString q_translate(const QString& str) { return QCoreApplication::translate("ExportParams", str.toStdString().c_str(), nullptr); }

// #define tr(str) QCoreApplication::translate("exportParam", str, nullptr)

// Validation for what the export dialog still genuinely owns.
//
// The previous version was ~340 lines, and its job was reconciling choices the
// user could make independently and get wrong: transparency against the
// container, against the pixel format, against the intermediate format; the
// per-container codec; the custom command lines; the bitrate. A destination makes
// those contradictions UNREPRESENTABLE — transparency is a property of the row,
// the codec is dictated or picked by the ladder, the pixel format follows from
// alpha — so reconciling them is no longer a thing that can fail.
//
// What is left is what the user still decides: where the file goes, how big, how
// fast, which frames, and the GIF palette file.
inline bool isExportParamValid(exportParam* exParam, QWidget* widget, std::vector<audioConfig>* pConf) {
    Q_UNUSED(pConf);
    QStringList warnings;
    QStringList warningDetail;
    QStringList errors;
    QStringList errorDetail;
    auto* params = &exParam->generalParams;

    // Export name / path
    if (params->osExportTarget.contains("&#32")) {
        errors.append(q_translate("Export name or location contains an invalid character (\"&#32\")"));
        errorDetail.append(q_translate("Export name or location has the invalid character, please rename it to proceed."));
    }

    // Size and rate. Odd dimensions are NOT rejected: the encoder rounds a frame
    // up to an even size and the real-path harness asserts that a 321x241 export
    // succeeds, so refusing odd numbers here contradicted the encoder.
    if (params->exportWidth == 0 || params->exportHeight == 0) {
        errors.append(q_translate("Export size is zero"));
        errorDetail.append(q_translate("The export width or height is zero, please increase the resolution."));
    }
    if (params->fps == 0) {
        errors.append(q_translate("Export FPS is equal to zero."));
        errorDetail.append(q_translate("Target FPS was set to zero, cannot proceed with a framerate lower than 1."));
    }

    // Destination directory
    if (!params->exportDirectory.exists()) {
        errors.append(q_translate("Target directory could not be found"));
        errorDetail.append(q_translate("The directory \"") + params->osExportTarget + q_translate("\" could not be found."));
    }
    QFileInfo exDir(params->exportDirectory.absolutePath());
    if (!exDir.isWritable()) {
        errors.append(q_translate("Target directory is protected"));
        errorDetail.append(
            q_translate("The directory \"") + params->osExportTarget +
            q_translate("\" could not be written into as it's protected or otherwise inaccessible, "
                        "if you wish to save onto it please launch AnimeEffects as admin.")
        );
    }

    // Bitrate is still a user-set number when no destination is chosen.
    if (params->bitrate > 5 * 1000) {
        warnings.append(q_translate("Target bitrate is low"));
        warningDetail.append(
            q_translate("The target bitrate for encoding will generate a low quality export, we "
                        "recommend letting the encoder decide for you unless you know what you're doing.")
        );
    }
    if (params->bitrate == -1) {
        warnings.append(q_translate("Target bitrate invalid"));
        warningDetail.append(
            q_translate("The target bitrate for encoding was invalid in some way, we have changed it to automatic so the "
                        "export process may continue without issues should you decide to continue with the export.")
        );
        params->bitrate = 0;
    }

    // Export range: at least one frame. Inverted ranges are swapped in place; a
    // range of exactly one frame is VALID. The old check used
    // `first - last >= 0`, which rejected a single-frame range (first == last) as
    // "empty", so the dialog could not export one frame at all. Nothing is
    // flattened here any more either: the exporter takes the list as a
    // FrameSelection, and the old `framesToBeExported` was computed and never read.
    if (params->exportRange.isEmpty()) {
        errors.append(q_translate("Export range is empty"));
        errorDetail.append(q_translate("No frame range was selected, please add a range of one frame or more."));
    } else {
        int frames = 0;
        int x = 0;
        for (auto& range : params->exportRange) {
            if (range.firstFrame > range.lastFrame) {
                warnings.append(q_translate("Export range at index ") + QString::number(x) +
                                q_translate(" is invalid, swapping."));
                std::swap(range.firstFrame, range.lastFrame);
                warningDetail.append(q_translate("Swapped frames from range at index ") + QString::number(x) + ".");
            }
            frames += range.lastFrame - range.firstFrame + 1;
            ++x;
        }
        if (frames < 1) {
            errors.append(q_translate("Export range is empty"));
            errorDetail.append(q_translate("The export range lasts less than one frame, please select at least one frame."));
        }
    }

    // GIF palette: the only file the dialog still hands to an encoder.
    if (params->useCustomPalette) {
        if (!params->palettePath.exists()) {
            errors.append(q_translate("Selected palette does not exist"));
            errorDetail.append(q_translate("The palette referenced at \"") + params->palettePath.absolutePath() +
                               q_translate("\" does not exist or is otherwise inaccessible."));
        } else if (!params->palettePath.isReadable()) {
            errors.append(q_translate("Selected palette is unreadable"));
            errorDetail.append(q_translate("The palette located at \"") + params->palettePath.absolutePath() +
                               q_translate("\" could not be read, please ensure it's a valid file."));
        } else if (!QFileInfo(params->palettePath.absolutePath())
                        .suffix()
                        .contains(QRegularExpression("(png|\\.png)"))) {
            errors.append(q_translate("Selected palette format unsupported"));
            errorDetail.append(q_translate("The palette located at \"") + params->palettePath.absolutePath() +
                               q_translate("\" seems to not be a PNG file, please convert it into one."));
        }
    }

    QSettings settings;
    bool proceedToExport = true;
    QVariant ignoreWarnings = settings.value("export_ignore_warning");
    if (!ignoreWarnings.isValid()) { ignoreWarnings.setValue(false); }
    if (!errors.isEmpty() || (!ignoreWarnings.toBool() && !warnings.isEmpty())) {
        QMessageBox msg;
        bool error = !errors.isEmpty();
        msg.setIcon(error ? QMessageBox::Icon::Critical : QMessageBox::Warning);
        msg.setWindowTitle(
            error ? q_translate("Critical errors have been found, cannot continue exporting.")
                  : q_translate("Minor errors have been found, please review.")
        );
        QString conMsg = error ? q_translate("You cannot proceed with the export until the critical issues found have been "
                                    "resolved.")
                               : q_translate("You may ignore the errors and proceed should you choose to, click \"Ok\" "
                                    "to export anyway or \"Cancel\" if you wish to cancel the export.");
        QString textMsg = q_translate("Some issues have been found while exporting, you can review them down below:\n") +
            "-----\n" + q_translate("Critical errors: ") + QString::number(errors.size()) + q_translate(" | Warnings: ") +
            QString::number(warnings.size()) + "\n-----\n" + conMsg;
        msg.setText(textMsg);
        QString detMsg = "-----\n" + q_translate("Critical errors (") + QString::number(errors.size()) + "): \n";
        if (!errors.isEmpty()) {
            int x = 0;
            for (const auto& err : errors) {
                detMsg.append("-----\n" + q_translate("Error: ") + err + "\n");
                detMsg.append("-\n" + q_translate("Error detail: ") + errorDetail[x] + "\n");
                x++;
            }
        } else {
            detMsg.append("-----\n" + q_translate("No errors found.\n"));
        }

        detMsg.append("-----\n" + q_translate("Warnings (") + QString::number(warnings.size()) + "): \n");
        if (!warnings.isEmpty()) {
            int x = 0;
            for (const auto& warn : warnings) {
                detMsg.append("-----\n" + q_translate("Warning: ") + warn + "\n");
                detMsg.append("-\n" + q_translate("Warning detail: ") + warningDetail[x] + "\n");
                x++;
            }
        } else {
            detMsg.append("-----\n" + q_translate("No warnings found.\n"));
        }
        msg.setDetailedText(detMsg);
        msg.setStandardButtons(error ? QMessageBox::Cancel : QMessageBox::Ok | QMessageBox::Cancel);
        msg.setParent(widget);
        msg.setWindowFlag(Qt::Window);
        int ret = msg.exec();
        if (ret == QMessageBox::Cancel) {
            proceedToExport = false;
        }
    }
    return proceedToExport;
}
inline exportParam::exportParam() { exportType = exportTarget::video; }

// Sorry for this
#endif // ANIMEEFFECTS_EXPORTPARAMS_H
