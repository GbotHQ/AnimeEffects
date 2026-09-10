#include "ctrl/ExportTarget.h"

#include "media/FFmpegHelpers.h"

#include <QDomDocument>
#include <QFileInfo>
#include <QFile>

#include <cmath>
#include <climits>
#include <functional>

namespace ctrl {
namespace {

// ---------- parsing helpers -------------------------------------------------

Capability parseCapability(const QString& aText) {
    if (aText == QLatin1String("required")) return Capability::Required;
    if (aText == QLatin1String("forbidden")) return Capability::Forbidden;
    if (aText == QLatin1String("default-on")) return Capability::DefaultOn;
    if (aText == QLatin1String("default-off")) return Capability::DefaultOff;
    return Capability::Choice;
}

DestClass parseClass(const QString& aText) {
    if (aText == QLatin1String("reencode")) return DestClass::Reencode;
    if (aText == QLatin1String("direct")) return DestClass::Direct;
    if (aText == QLatin1String("archive")) return DestClass::Archive;
    return DestClass::Fixed;
}

FitMode parseFitMode(const QString& aText) {
    if (aText == QLatin1String("crop")) return FitMode::Crop;
    if (aText == QLatin1String("none")) return FitMode::None;
    return FitMode::Pad;
}

const char* bucketName(QualityBucket aBucket) {
    switch (aBucket) {
    case QualityBucket::Smaller: return "smaller";
    case QualityBucket::Balanced: return "balanced";
    case QualityBucket::Best: return "best";
    case QualityBucket::Lossless: return "lossless";
    }
    return "balanced";
}

SizeMode parseSizeMode(const QString& aText) {
    if (aText == QLatin1String("exact")) return SizeMode::Exact;
    if (aText == QLatin1String("max")) return SizeMode::Max;
    if (aText == QLatin1String("maxExactSide")) return SizeMode::MaxExactSide;
    if (aText == QLatin1String("maxMinSide")) return SizeMode::MaxMinSide;
    return SizeMode::Free;
}

int attrInt(const QDomElement& aElem, const QString& aName, int aDefault = 0) {
    bool ok = false;
    const int v = aElem.attribute(aName).toInt(&ok);
    return ok ? v : aDefault;
}

double attrDouble(const QDomElement& aElem, const QString& aName, double aDefault = 0.0) {
    bool ok = false;
    const double v = aElem.attribute(aName).toDouble(&ok);
    return ok ? v : aDefault;
}

// ---------- size helpers ---------------------------------------------------

// YUV planar formats need even dimensions. Always floor to even so we never
// overshoot a maximum.
int evenFloor(double aValue) {
    int v = (int)std::floor(aValue);
    if (v < 2) v = 2;
    if (v & 1) --v;
    return v;
}

// The audio codec a container can actually carry. Derived, not requested:
// WebM cannot hold AAC and fails at avformat_write_header with EINVAL if you
// try — so the container decides.
QString audioCodecForContainer(const QString& aContainer) {
    const QString c = aContainer.toLower();
    if (c == QLatin1String("webm")) return QStringLiteral("libopus");
    if (c == QLatin1String("mp4") || c == QLatin1String("mov") || c == QLatin1String("m4v"))
        return QStringLiteral("aac");
    if (c == QLatin1String("mkv") || c == QLatin1String("matroska"))
        return QStringLiteral("aac");
    return QString(); // this container carries no standard audio stream for us
}


// ---------- the (class, bucket) codec ladder --------------------------------
//
// Ordered best -> fallback. A candidate is used only if its encoder actually
// exists in the linked build (availability probe). `ffmpeg -encoders` is NOT a
// reliable answer here: it reports what the binary was compiled with, so
// e.g. av1_nvenc is listed on an RTX 3080 and fails at runtime.

bool encoderExists(const QString& aName) {
    // encoderUsable, NOT encoderAvailable: a hardware encoder is routinely
    // present in the binary while having no capable device on this machine.
    const QByteArray bytes = aName.toUtf8();
    return media::ffmpeg::encoderUsable(bytes.constData());
}

QString firstAvailable(const QStringList& aGroup) {
    for (const QString& name : aGroup) {
        if (encoderExists(name)) return name;
    }
    return QString();
}

const char* describeGroup(int aGroupIndex) {
    switch (aGroupIndex) {
    case 0: return "AV1 (software — highest quality per bit)";
    case 1: return "AV1 (hardware — real-time, near-software quality)";
    case 2: return "H.265 (software)";
    case 3: return "H.265 (hardware)";
    case 4: return "H.264 (software)";
    case 5: return "H.264 (hardware)";
    case 6: return "FFV1 (lossless master)";
    default: return "";
    }
}

// Group indices into describeGroup above.
enum {
    G_AV1_SW = 0,
    G_AV1_HW = 1,
    G_HEVC_SW = 2,
    G_HEVC_HW = 3,
    G_H264_SW = 4,
    G_H264_HW = 5,
    G_LOSSLESS = 6
};

struct Ladder {
    QList<int> groups;
};

Ladder ladderFor(DestClass aClass, QualityBucket aBucket) {
    Ladder out;
    switch (aClass) {
    case DestClass::Direct:
        // The platform serves our file as-is: decode compatibility dominates.
        // AV1 is ~80% of devices and HEVC is patchy in browsers, so H.264 wins
        // at every bucket. AV1/HEVC stay available in Advanced.
        out.groups = {G_H264_SW, G_H264_HW};
        return out;

    case DestClass::Archive:
        if (aBucket == QualityBucket::Lossless) out.groups = {G_LOSSLESS, G_HEVC_SW, G_H264_SW};
        else if (aBucket == QualityBucket::Best) out.groups = {G_HEVC_SW, G_AV1_SW, G_H264_SW};
        else if (aBucket == QualityBucket::Balanced) out.groups = {G_HEVC_SW, G_H264_SW};
        else out.groups = {G_AV1_SW, G_HEVC_SW, G_H264_SW};
        return out;

    case DestClass::Reencode:
        // The platform re-encodes whatever we upload, so their transcode — not
        // our file — is what viewers see. Optimize for quality per bit.
        if (aBucket == QualityBucket::Lossless) out.groups = {G_LOSSLESS, G_HEVC_SW, G_AV1_SW, G_H264_SW};
        else if (aBucket == QualityBucket::Best) out.groups = {G_AV1_SW, G_AV1_HW, G_HEVC_SW, G_HEVC_HW, G_H264_SW, G_H264_HW};
        else if (aBucket == QualityBucket::Balanced) out.groups = {G_AV1_HW, G_AV1_SW, G_HEVC_HW, G_HEVC_SW, G_H264_SW, G_H264_HW};
        else out.groups = {G_AV1_SW, G_AV1_HW, G_HEVC_SW, G_HEVC_HW, G_H264_SW, G_H264_HW};
        return out;

    default:
        out.groups = {G_H264_SW, G_H264_HW};
        return out;
    }
}

QStringList groupEncoders(int aGroup) {
    switch (aGroup) {
    case G_AV1_SW: return {"libsvtav1", "libaom-av1"};
    case G_AV1_HW: return {"av1_nvenc", "av1_qsv", "av1_vaapi", "av1_amf"};
    case G_HEVC_SW: return {"libx265"};
    case G_HEVC_HW: return {"hevc_nvenc", "hevc_qsv", "hevc_vaapi", "hevc_amf"};
    case G_H264_SW: return {"libx264"};
    case G_H264_HW: return {"h264_nvenc", "h264_qsv", "h264_vaapi", "h264_amf"};
    case G_LOSSLESS: return {"ffv1"};
    default: return {};
    }
}

const char* classClause(DestClass aClass) {
    switch (aClass) {
    case DestClass::Reencode:
        return " — this site re-encodes uploads, so source quality matters more than compatibility";
    case DestClass::Direct:
        return " — this plays your file directly, so decode compatibility matters most";
    case DestClass::Archive:
        return " — a master copy, so fidelity matters most";
    default:
        return "";
    }
}

} // namespace

// ---------------------------------------------------------------------------

QString ExportTargets::defaultPath() {
    return QStringLiteral(":/export/ExportTargets.txt");
}

QList<ExportTarget> ExportTargets::load(const QString& aPath, QString* aError) {
    QList<ExportTarget> out;

    QFile file(aPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (aError) *aError = QStringLiteral("cannot open %1").arg(aPath);
        return out;
    }

    QDomDocument doc;
    const auto parsed = doc.setContent(&file);
    if (!parsed) {
        if (aError) {
            *aError = QStringLiteral("%1:%2:%3 %4")
                          .arg(aPath)
                          .arg((qlonglong)parsed.errorLine)
                          .arg((qlonglong)parsed.errorColumn)
                          .arg(parsed.errorMessage);
        }
        return out;
    }

    const QDomElement root = doc.firstChildElement(QStringLiteral("export_targets"));
    if (root.isNull()) {
        if (aError) *aError = QStringLiteral("%1: missing <export_targets> root").arg(aPath);
        return out;
    }

    for (QDomElement d = root.firstChildElement(QStringLiteral("destination")); !d.isNull();
         d = d.nextSiblingElement(QStringLiteral("destination"))) {
        ExportTarget t;
        t.id = d.attribute(QStringLiteral("id"));
        if (t.id.isEmpty()) continue;
        t.label = d.attribute(QStringLiteral("label"));
        t.family = d.attribute(QStringLiteral("family"));
        t.klass = parseClass(d.attribute(QStringLiteral("class")));
        t.codecLadder = d.attribute(QStringLiteral("codecSelect")) == QLatin1String("ladder");
        t.container = d.attribute(QStringLiteral("container"));
        t.codec = d.attribute(QStringLiteral("codec"));
        t.pixFmt = d.attribute(QStringLiteral("pixfmt"));
        t.alpha = parseCapability(d.attribute(QStringLiteral("alpha")));
        t.audio = parseCapability(d.attribute(QStringLiteral("audio")));
        t.loop = parseCapability(d.attribute(QStringLiteral("loop")));
        t.note = d.attribute(QStringLiteral("note"));
        t.checked = d.attribute(QStringLiteral("checked"));
        t.source = d.attribute(QStringLiteral("source"));
        t.verified = d.attribute(QStringLiteral("verified")) == QLatin1String("true");
        t.platform = d.attribute(QStringLiteral("platform"));
        if (t.label.isEmpty()) t.label = t.id;

        const QDomElement sizeElem = d.firstChildElement(QStringLiteral("size"));
        if (!sizeElem.isNull()) {
            t.size.mode = parseSizeMode(sizeElem.attribute(QStringLiteral("mode")));
            t.size.w = attrInt(sizeElem, QStringLiteral("w"));
            t.size.h = attrInt(sizeElem, QStringLiteral("h"));
            t.size.side = attrInt(sizeElem, QStringLiteral("side"));
            t.size.fit = parseFitMode(sizeElem.attribute(QStringLiteral("fit")));
        }
        const QString degrade = d.attribute(QStringLiteral("degrade"));
        t.degradeOrder = degrade.isEmpty()
                             ? QStringList{QStringLiteral("quality"), QStringLiteral("scale"),
                                           QStringLiteral("frames")}
                             : degrade.split(QLatin1Char(','), Qt::SkipEmptyParts);

        const QDomElement capsElem = d.firstChildElement(QStringLiteral("caps"));
        if (!capsElem.isNull()) {
            t.caps.maxBytes = attrInt(capsElem, QStringLiteral("maxBytes"));
            t.caps.maxDurationSec = attrDouble(capsElem, QStringLiteral("maxDurationSec"));
            t.caps.maxFps = attrInt(capsElem, QStringLiteral("maxFps"));
            t.caps.minFrames = attrInt(capsElem, QStringLiteral("minFrames"));
            t.caps.maxFrames = attrInt(capsElem, QStringLiteral("maxFrames"));
        }

        const QDomElement encOptsElem = d.firstChildElement(QStringLiteral("encOptions"));
        for (QDomElement o = encOptsElem.firstChildElement(QStringLiteral("opt")); !o.isNull();
             o = o.nextSiblingElement(QStringLiteral("opt"))) {
            const QString key = o.attribute(QStringLiteral("name"));
            if (!key.isEmpty()) t.encOptions.insert(key, o.attribute(QStringLiteral("value")));
        }

        const QDomElement audioElem = d.firstChildElement(QStringLiteral("audio"));
        if (!audioElem.isNull()) {
            t.audioSpec.codec = audioElem.attribute(QStringLiteral("codec"));
            t.audioSpec.bitrate = attrInt(audioElem, QStringLiteral("bitrate"));
            t.audioSpec.channels = attrInt(audioElem, QStringLiteral("channels"));
            t.audioSpec.sampleRate = attrInt(audioElem, QStringLiteral("rate"));
        }
        t.minScalePct = attrInt(d, QStringLiteral("minScalePct"), 25);

        const QDomElement tagElem = d.firstChildElement(QStringLiteral("codecTag"));
        if (!tagElem.isNull()) t.codecTag = tagElem.attribute(QStringLiteral("value"));

        const QDomElement noteElem = d.firstChildElement(QStringLiteral("note"));
        if (!noteElem.isNull() && t.note.isEmpty()) t.note = noteElem.text().trimmed();

        out.push_back(t);
    }

    return out;
}

QHash<QString, QualityControl> ExportTargets::loadQualityControl(const QString& aPath) {
    // Parsed once per path: the table is tiny and does not change at runtime.
    static QHash<QString, QHash<QString, QualityControl>> cache;
    const auto it = cache.constFind(aPath);
    if (it != cache.constEnd()) return *it;

    QHash<QString, QualityControl> out;
    QFile file(aPath);
    if (file.open(QIODevice::ReadOnly)) {
        QDomDocument doc;
        const auto parsed = doc.setContent(&file);
        if (parsed) {
            const QDomElement root = doc.firstChildElement(QStringLiteral("export_targets"));
            const QDomElement qc = root.firstChildElement(QStringLiteral("quality_control"));
            for (QDomElement c = qc.firstChildElement(QStringLiteral("codec")); !c.isNull();
                 c = c.nextSiblingElement(QStringLiteral("codec"))) {
                QualityControl q;
                q.codec = c.attribute(QStringLiteral("name"));
                q.option = c.attribute(QStringLiteral("option"));
                q.best = attrInt(c, QStringLiteral("best"));
                q.worst = attrInt(c, QStringLiteral("worst"));
                if (!q.codec.isEmpty() && !q.option.isEmpty()) out.insert(q.codec, q);
            }
        }
    }
    cache.insert(aPath, out);
    return out;
}

QHash<QString, double> ExportTargets::loadTimeBudgets(const QString& aPath) {
    static QHash<QString, QHash<QString, double>> cache;
    const auto it = cache.constFind(aPath);
    if (it != cache.constEnd()) return *it;

    QHash<QString, double> out;
    QFile file(aPath);
    if (file.open(QIODevice::ReadOnly)) {
        QDomDocument doc;
        if (doc.setContent(&file)) {
            const QDomElement root = doc.firstChildElement(QStringLiteral("export_targets"));
            const QDomElement tb = root.firstChildElement(QStringLiteral("time_budgets"));
            for (QDomElement b = tb.firstChildElement(QStringLiteral("bucket")); !b.isNull();
                 b = b.nextSiblingElement(QStringLiteral("bucket"))) {
                out.insert(b.attribute(QStringLiteral("name")),
                           (double)attrDouble(b, QStringLiteral("maxSeconds")));
            }
        }
    }
    cache.insert(aPath, out);
    return out;
}

ExportTarget ExportTargets::find(const QList<ExportTarget>& aTargets, const QString& aId) {
    for (const ExportTarget& t : aTargets) {
        if (t.id == aId) return t;
    }
    return ExportTarget();
}

// ---------------------------------------------------------------------------
// FrameSelection

FrameSelection FrameSelection::fromRange(int aFirst, int aLast) {
    FrameSelection s;
    if (aLast >= aFirst) s.ranges.append(FrameRange{aFirst, aLast});
    return s;
}

FrameSelection FrameSelection::fromCount(int aCount) {
    return aCount > 0 ? fromRange(0, aCount - 1) : FrameSelection();
}

int FrameSelection::total() const {
    int n = 0;
    for (const FrameRange& r : ranges) n += r.count();
    return n;
}

QList<int> FrameSelection::indices() const {
    QList<int> out;
    out.reserve(total());
    for (const FrameRange& r : ranges) {
        for (int f = r.first; f <= r.last; ++f) out.append(f);
    }
    return out;
}

FrameSelection FrameSelection::firstN(int aCount) const {
    FrameSelection out;
    int remaining = aCount;
    for (const FrameRange& r : ranges) {
        if (remaining <= 0) break;
        const int take = std::min(remaining, r.count());
        if (take <= 0) continue;
        out.ranges.append(FrameRange{r.first, r.first + take - 1});
        remaining -= take;
    }
    return out;
}

int FrameSelection::rangeCount() const {
    int valid = 0;
    for (const FrameRange& r : ranges) {
        if (r.count() > 0) ++valid;
    }
    return valid;
}

QString FrameSelection::describe() const {
    const int n = total();
    if (n == 0) return QStringLiteral("no frames selected");
    // A backwards range contributes nothing, but is not pretended away: it just
    // does not add to the range count either.
    const int valid = rangeCount();
    if (valid <= 1) return QStringLiteral("%1 frame%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    return QStringLiteral("%1 frames from %2 separate ranges").arg(n).arg(valid);
}

// ---------------------------------------------------------------------------

QSize applySizeRule(const SizeRule& aRule, const QSize& aSource, QStringList* aNotes) {
    if (!aSource.isValid() || aSource.isEmpty()) return aSource;

    const double sw = aSource.width();
    const double sh = aSource.height();

    switch (aRule.mode) {
    case SizeMode::Free:
        return QSize(evenFloor(sw), evenFloor(sh));

    case SizeMode::Exact:
        if (aNotes) {
            // Crop-or-pad is resolved (FitMode), so the note states which one
            // applies rather than calling it an open question.
            *aNotes << QStringLiteral("Size forced to %1x%2 (required by this destination); "
                                      "the art is %3.")
                           .arg(aRule.w)
                           .arg(aRule.h)
                           .arg(aRule.fit == FitMode::Crop ? QStringLiteral("cropped to fill it")
                                                           : QStringLiteral("padded to fit it"));
        }
        return QSize(evenFloor(aRule.w), evenFloor(aRule.h));

    case SizeMode::Max: {
        const double scale = std::min(aRule.w / sw, aRule.h / sh);
        return QSize(evenFloor(sw * scale), evenFloor(sh * scale));
    }

    case SizeMode::MaxExactSide: {
        const double scale = aRule.side / std::max(sw, sh);
        return QSize(evenFloor(sw * scale), evenFloor(sh * scale));
    }

    case SizeMode::MaxMinSide: {
        double scale = std::min(aRule.w / sw, aRule.h / sh);
        QSize out(evenFloor(sw * scale), evenFloor(sh * scale));
        if (std::max(out.width(), out.height()) < aRule.side) {
            scale = aRule.side / (double)std::max(out.width(), out.height());
            out = QSize(evenFloor(out.width() * scale), evenFloor(out.height() * scale));
        }
        return out;
    }
    }
    return aSource;
}

void applyCaps(const Caps& aCaps, int* aFps, int* aFrames, QStringList* aNotes) {
    if (!aFps || !aFrames) return;

    if (aCaps.maxFps > 0 && *aFps > aCaps.maxFps) {
        if (aNotes) *aNotes << QStringLiteral("Frame rate reduced %1 -> %2 fps (destination limit).").arg(*aFps).arg(aCaps.maxFps);
        *aFps = aCaps.maxFps;
    }

    if (aCaps.maxDurationSec > 0.0 && *aFps > 0) {
        const double duration = (double)*aFrames / (double)*aFps;
        if (duration > aCaps.maxDurationSec) {
            const int allowed = (int)std::floor(aCaps.maxDurationSec * *aFps);
            if (aNotes) {
                *aNotes << QStringLiteral("Trimmed %1 -> %2 frames (%3s limit).")
                               .arg(*aFrames)
                               .arg(allowed)
                               .arg(aCaps.maxDurationSec);
            }
            *aFrames = allowed;
        }
    }

    if (aCaps.maxFrames > 0 && *aFrames > aCaps.maxFrames) {
        if (aNotes) *aNotes << QStringLiteral("Trimmed %1 -> %2 frames (destination limit).").arg(*aFrames).arg(aCaps.maxFrames);
        *aFrames = aCaps.maxFrames;
    }

    if (aCaps.minFrames > 0 && *aFrames < aCaps.minFrames) {
        if (aNotes) {
            *aNotes << QStringLiteral("WARNING: destination needs at least %1 frames but only %2 "
                                      "are available — the upload will likely be rejected.")
                           .arg(aCaps.minFrames)
                           .arg(*aFrames);
        }
    }
}

QString resolveLadderCodec(DestClass aClass, QualityBucket aBucket,
                           const LadderHints& aHints, QString* aWhy) {
    const Ladder ladder = ladderFor(aClass, aBucket);
    QStringList rejected;
    // Fastest rung seen, used when NOTHING meets the budget. Returning nothing
    // would strand the user; the right answer is the fastest available plus an
    // honest explanation (the real ETA comes from actual progress anyway).
    QString fallback;
    QString fallbackWhy;

    for (int group : ladder.groups) {
        const QString name = firstAvailable(groupEncoders(group));
        if (name.isEmpty()) continue;
        fallback = name;
        fallbackWhy = QString::fromLatin1(describeGroup(group)) +
                      QString::fromLatin1(classClause(aClass));

        // Second gate: is it fast enough HERE? Only meaningful with a size, a
        // frame count and a budget — otherwise we have nothing to project.
        if (aHints.maxSeconds > 0.0 && aHints.frameCount > 0 && aHints.width > 0 &&
            aHints.height > 0) {
            const double fps =
                media::ffmpeg::encoderThroughput(name, aHints.width, aHints.height, aHints.pixFmt);
            if (fps > 0.0) {
                const double projected = (double)aHints.frameCount / fps;
                if (projected > aHints.maxSeconds) {
                    rejected << QStringLiteral("%1 (~%2s)")
                                    .arg(name)
                                    .arg((int)std::llround(projected));
                    continue; // try the next rung down
                }
                if (aWhy) {
                    *aWhy = QStringLiteral("%1 — about %2s here%3")
                                .arg(QString::fromLatin1(describeGroup(group)))
                                .arg((int)std::llround(projected))
                                .arg(QString::fromLatin1(classClause(aClass)));
                }
                return name;
            }
            // Unmeasurable: don't reject on a failed measurement, just fall
            // through to using it (the real ETA comes from actual progress).
        }

        if (aWhy) {
            *aWhy = QString::fromLatin1(describeGroup(group)) +
                    QString::fromLatin1(classClause(aClass));
        }
        return name;
    }

    if (!fallback.isEmpty()) {
        if (aWhy) {
            *aWhy = QStringLiteral("%1 — nothing here is fast enough for this budget (%2), "
                                   "so this is the fastest available")
                        .arg(fallbackWhy, rejected.join(QLatin1String(", ")));
        }
        return fallback;
    }

    if (aWhy) *aWhy = QStringLiteral("no suitable encoder found in this build");
    return QString();
}

// ---------------------------------------------------------------------------

ExportPlan planExport(const ExportTarget& aTarget, const ExportRequest& aRequest,
                      QualityBucket aBucket) {
    ExportPlan plan;
    plan.target = aTarget;

    if (aTarget.id.isEmpty()) {
        plan.error = QStringLiteral("no destination selected");
        return plan;
    }
    if (!aRequest.sourceSize.isValid() || aRequest.sourceSize.isEmpty()) {
        plan.error = QStringLiteral("invalid source size");
        return plan;
    }

    // --- capabilities: declared per row, only ever a control when Choice -----
    auto resolveCapability = [&](Capability aCap, const char* aName, bool* aOut) {
        switch (aCap) {
        case Capability::Required:
            *aOut = true;
            plan.notes << QString::fromLatin1("%1 is required by this destination.").arg(QString::fromLatin1(aName));
            break;
        case Capability::Forbidden:
            *aOut = false;
            break;
        case Capability::DefaultOn: *aOut = true; break;
        case Capability::DefaultOff: *aOut = false; break;
        case Capability::Choice: *aOut = false; break; // real dialog carries the user's choice
        }
    };

    resolveCapability(aTarget.alpha, "Transparency", &plan.alphaEnabled);
    resolveCapability(aTarget.audio, "Audio", &plan.audioEnabled);

    // --- size, rate and frames: the deterministic, pre-encode half -----------
    //
    // Applied EXACTLY ONCE, and before the codec ladder, because the ladder
    // needs the real frame count to project its time budget. Keeping it in one
    // place, rather than also inside each codec branch, is what keeps the cap
    // notes from being emitted twice.
    if (aRequest.selection.isEmpty()) {
        plan.error = QStringLiteral("no frames selected for export");
        return plan;
    }

    plan.size = applySizeRule(aTarget.size, aRequest.sourceSize, &plan.notes);
    plan.fps = aRequest.fps;
    int allowedFrames = aRequest.selection.total();
    applyCaps(aTarget.caps, &plan.fps, &allowedFrames, &plan.notes);
    plan.selection = aRequest.selection.firstN(allowedFrames);
    // applyCaps has already reported any trim by COUNT. Add the selection detail
    // only when the selection has gaps, because "trimmed to 90 frames" does not
    // say which frames survived, and packing the ranges back-to-back is the
    // non-obvious consequence of choosing several ranges.
    if (plan.selection.rangeCount() > 1) {
        plan.notes << QStringLiteral("%1; the ranges play back to back and the frames "
                                     "between them are not exported.")
                          .arg(plan.selection.describe());
    }

    // --- codec --------------------------------------------------------------
    QString codec = aTarget.codec;
    if (aTarget.codecLadder) {
        LadderHints hints;
        hints.width = plan.size.width();
        hints.height = plan.size.height();
        hints.pixFmt = aTarget.pixFmt;
        hints.frameCount = plan.frames();
        const QHash<QString, double> budgets = ExportTargets::loadTimeBudgets();
        hints.maxSeconds =
            budgets.value(bucketName(aBucket), 0.0);

        QString why;
        codec = resolveLadderCodec(aTarget.klass, aBucket, hints, &why);
        if (codec.isEmpty()) {
            plan.error = QStringLiteral("no encoder available for this destination in this build");
            return plan;
        }
        plan.notes << why;
    }

    // Exact-size destinations (Discord 320x320, WhatsApp 512x512) need the
    // FILE to be that size while the art keeps its aspect. So render at the
    // fitted size and have the encoder pad the remainder with transparency —
    // never stretch, never crop.
    plan.renderSize = plan.size;
    if (aTarget.size.mode == SizeMode::Exact) {
        const double sw = aRequest.sourceSize.width();
        const double sh = aRequest.sourceSize.height();
        const double scale = std::min((double)aTarget.size.w / sw, (double)aTarget.size.h / sh);
        plan.renderSize = QSize(evenFloor(sw * scale), evenFloor(sh * scale));
    }

    if (!aTarget.note.isEmpty()) plan.notes << aTarget.note;

    if (!aTarget.verified) {
        plan.notes << QStringLiteral("WARNING: this destination's limits are UNVERIFIED — re-check before relying on them.");
    }

    // --- build the config ---------------------------------------------------
    // GIF does not go through VideoEncoder (media::GifEncoder owns it).
    if (aTarget.container.compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0) {
        plan.gifPath = true;
        return plan;
    }

    QString err;
    plan.config = media::VideoEncoder::makeConfig(aTarget.container, codec, aTarget.pixFmt,
                                                  plan.alphaEnabled, plan.size.width(),
                                                  plan.size.height(), plan.fps, 0, &err);
    if (plan.config.codec.isEmpty()) {
        plan.error = QStringLiteral("%1: %2").arg(aTarget.id, err);
        return plan;
    }

    // The resolved pixel format must be genuinely ENCODABLE, not merely
    // advertised. Codecs list formats they then refuse: libx265 advertises
    // yuva420p and avcodec_open2 fails with ENOSYS, so makeConfig's static
    // compatibility check is not sufficient. Verify by probe, before the config
    // ever reaches open().
    if (!media::ffmpeg::encoderSupportsPixFormatUsable(plan.config.codec, plan.config.pixFmt)) {
        const bool alphaRequired = (aTarget.alpha == Capability::Required);
        QString err2;
        const media::VideoEncoder::Config fallback = media::VideoEncoder::makeConfig(
            aTarget.container, plan.config.codec, QString(), false, plan.size.width(),
            plan.size.height(), plan.fps, 0, &err2);
        const bool fallbackOk = !fallback.codec.isEmpty() &&
                                media::ffmpeg::encoderSupportsPixFormatUsable(fallback.codec,
                                                                              fallback.pixFmt);

        if (alphaRequired) {
            // Dropping alpha here would hand the user a file the platform
            // rejects, with no symptom until upload. Refuse and name the
            // alternative instead.
            plan.error = QStringLiteral(
                             "%1 requires transparency, but '%2' cannot encode an alpha "
                             "channel in this build (it advertises %3 and then refuses it). "
                             "Use a destination whose codec carries alpha — WebM/VP9, APNG, "
                             "or FFV1.")
                             .arg(aTarget.label, plan.config.codec, plan.config.pixFmt);
            return plan;
        }
        if (!fallbackOk) {
            plan.error = QStringLiteral("%1: %2 could not encode %3 and no fallback worked (%4)")
                             .arg(aTarget.id, plan.config.codec, plan.config.pixFmt, err2);
            return plan;
        }
        plan.alphaEnabled = false;
        plan.notes << QStringLiteral(
                          "Transparency dropped: '%1' advertises %2 but cannot encode it "
                          "here, so %3 is used instead.")
                          .arg(plan.config.codec, plan.config.pixFmt,
                               fallback.pixFmt.isEmpty() ? QStringLiteral("the encoder default")
                                                         : fallback.pixFmt);
        plan.config = fallback;
    }

    plan.config.outputPath = aRequest.outputPath;
    plan.config.encOptions = aTarget.encOptions;
    plan.config.codecTag = aTarget.codecTag;
    plan.config.fit = (aTarget.size.mode == SizeMode::Exact)
                          ? (aTarget.size.fit == FitMode::Crop ? media::FitMode::Crop
                                                                              : media::FitMode::Pad)
                          : media::FitMode::None;

    // Enforce the audio capability here, not by convention. Telegram rejects a
    // video sticker that carries an audio stream, so a forbidden capability has
    // to actively strip the audio config rather than rely on the caller
    // remembering not to call setAudio().
    // Audio presence is DERIVED, not requested: project has it, user didn't mute,
    // destination permits it. Because the only user intent is `mute` (a
    // restriction), it can never conflict with a destination that forbids audio
    // — so there is nothing to enforce and nothing to silently deny.
    const QString acodec = aTarget.audioSpec.codec.isEmpty() ? audioCodecForContainer(aTarget.container)
                                                         : aTarget.audioSpec.codec;
    // Per-destination audio parameters, with defaults where unspecified.
    const int channels = aTarget.audioSpec.channels > 0 ? aTarget.audioSpec.channels : aRequest.audioChannels;
    const int rate = aTarget.audioSpec.sampleRate > 0 ? aTarget.audioSpec.sampleRate : aRequest.audioSampleRate;
    const int bitrate = aTarget.audioSpec.bitrate > 0 ? aTarget.audioSpec.bitrate : 128000;
    // Audio encoders are software, so plain availability is enough here (the
    // "listed but unusable" problem is specific to hardware encoders).
    const bool codecUsable =
        !acodec.isEmpty() && media::ffmpeg::encoderAvailable(acodec.toUtf8().constData());
    const bool projectHasExportableAudio = aRequest.exportableAudioTracks > 0;
    const bool includeAudio = (aTarget.audio != Capability::Forbidden) &&
                              projectHasExportableAudio && !aRequest.mute && codecUsable;
    plan.audioEnabled = includeAudio;

    if (includeAudio) {
        plan.config.audioCodec = acodec;
        plan.config.audioChannels = channels;
        plan.config.audioSampleRate = rate;
        plan.config.audioBitrate = bitrate;
    } else {
        plan.config.audioCodec.clear();
        plan.config.audioChannels = 0;
        plan.config.audioSampleRate = 0;
        // State the reason only when there WAS audio to leave out — otherwise
        // this is noise on every silent project.
        if (projectHasExportableAudio && !aRequest.mute &&
            aTarget.audio == Capability::Forbidden) {
            plan.notes << QStringLiteral("No sound: %1 does not accept an audio track.")
                              .arg(aTarget.label);
        }
        if (aTarget.audio == Capability::Required && !projectHasExportableAudio) {
            plan.notes << QStringLiteral("WARNING: %1 expects sound but this project has no "
                                         "audio track.")
                              .arg(aTarget.label);
        }
    }
    return plan;
}

namespace {

// One encode attempt at a given degradation, returning the written byte count.
struct Attempt {
    bool wrote = false;
    qint64 bytes = 0;
    QString error;
};

Attempt encodeAttempt(const ExportPlan& aPlan, const QList<QImage>& aFrames,
                      const QSize& aSize, int aFrameStride, int aQualityValue,
                      const QualityControl* aQuality) {
    Attempt out;
    media::VideoEncoder::Config cfg = aPlan.config;
    cfg.width = aSize.width();
    cfg.height = aSize.height();
    if (aQuality && aQualityValue >= 0) {
        cfg.encOptions.insert(aQuality->option, QString::number(aQualityValue));
    }

    QList<QImage> frames;
    for (int i = 0; i < aFrames.size(); i += aFrameStride) frames.append(aFrames[i]);
    if (frames.isEmpty()) { out.error = QStringLiteral("no frames left"); return out; }

    media::VideoEncoder enc;
    if (!enc.open(cfg)) { out.error = enc.error(); return out; }
    for (const QImage& f : frames) {
        if (!enc.write(f)) { out.error = enc.error(); return out; }
    }
    if (!enc.close()) { out.error = enc.error(); return out; }

    const QFileInfo info(cfg.outputPath);
    if (!info.exists()) { out.error = QStringLiteral("no file produced"); return out; }
    out.wrote = true;
    out.bytes = info.size();
    return out;
}

} // namespace

FitResult exportWithFit(const ExportTarget& aTarget, const ExportRequest& aRequest,
                        QualityBucket aBucket, const QList<QImage>& aFrames) {
    FitResult result;
    result.path = aRequest.outputPath;

    const ExportPlan plan = planExport(aTarget, aRequest, aBucket);
    if (!plan.ok()) { result.error = plan.error; return result; }

    // The plan is authoritative about WHICH frames to write (see FrameSelection).
    // If the caller hands over a different number, the file would not match the
    // plan — and for a capped destination it could exceed the very limit this
    // function exists to enforce, while the fit loop reported success. Fail
    // loudly instead of encoding something the plan never described.
    if (aFrames.size() != plan.frames()) {
        result.error = QStringLiteral("frame mismatch: the plan selects %1 frame%2 but %3 "
                                      "were supplied (render plan.selection.indices())")
                           .arg(plan.frames())
                           .arg(plan.frames() == 1 ? QString() : QStringLiteral("s"))
                           .arg(aFrames.size());
        return result;
    }

    const qint64 cap = aTarget.caps.maxBytes;

    QList<QImage> base;
    for (const QImage& f : aFrames) {
        base.append(f.size() == plan.renderSize
                        ? f
                        : f.scaled(plan.renderSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }

    const QHash<QString, QualityControl> qc = ExportTargets::loadQualityControl();
    const auto qit = qc.constFind(plan.config.codec);
    const QualityControl* quality = (qit != qc.constEnd()) ? &(*qit) : nullptr;

    // No cap: one encode, no loop.
    qint64 smallest = 0; // smallest file seen (only used in the give-up message)
    auto encodeOnce = [&](int aCrf, double aScale, int aStride, const QString& aWhat) -> int {
        const int w = std::max(2, evenFloor(plan.size.width() * aScale));
        const int h = std::max(2, evenFloor(plan.size.height() * aScale));
        QList<QImage> frames;
        for (int i = 0; i < base.size(); i += aStride) frames.append(base[i]);
        if (frames.isEmpty()) frames.append(base.first());
        if (aScale != 1.0) {
            const QSize size(w, h);
            for (QImage& f : frames) {
                f = f.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
        const Attempt a = encodeAttempt(plan, frames, QSize(w, h), aStride, aCrf, quality);
        ++result.attempts;
        result.steps << QStringLiteral("%1: crf=%2 %3x%4 stride=%5 -> %6 bytes")
                            .arg(aWhat)
                            .arg(aCrf)
                            .arg(w)
                            .arg(h)
                            .arg(aStride)
                            .arg(a.wrote ? (qlonglong)a.bytes : (qlonglong)-1);
        if (!a.wrote) { result.error = a.error; return -1; }
        // `bytes` is the file the caller gets; `smallest` is only for the
        // failure message (the last probe is not necessarily the smallest).
        result.bytes = a.bytes;
        result.finalSize = QSize(w, h);
        if (smallest == 0 || a.bytes < smallest) smallest = a.bytes;
        return (int)std::min<qint64>(a.bytes, (qint64)INT_MAX);
    };

    if (cap <= 0) {
        if (encodeOnce(quality ? quality->best : -1, 1.0, 1, QStringLiteral("initial")) < 0)
            return result;
        result.ok = true;
        return result;
    }

    // ---- model-driven degradation -----------------------------------------
    // Each knob has a predictive model, so we jump to the predicted setting and
    // then correct from the real measurement, rather than walking arbitrary
    // steps. The models are the published/verified relationships:
    //   quality: size ~ 2^(crf/6)  — a change of +-6 CRF halves/doubles the bitrate
    //   scale:   size ~ scale^2    — bytes scale with pixel count
    //   frames:  size ~ frameCount — bytes scale with the number of frames
    // Order comes from the destination's `degrade` list; the default is
    // quality -> scale -> frames because quality changes neither composition nor
    // timing, scale costs detail but keeps both, and dropping frames changes the
    // animation itself.
    double scale = 1.0;
    int stride = 1;
    int crf = quality ? quality->best : -1;

    int bytes = encodeOnce(crf, scale, stride, QStringLiteral("initial"));
    if (bytes < 0) return result;
    if (bytes <= cap) { result.ok = true; return result; }

    // ---- monotone bisection ------------------------------------------------
    // Each knob trades bytes monotonically, so the least destructive setting
    // that fits can be SEARCHED for rather than guessed:
    //   quality: bytes fall as CRF rises
    //   scale:   bytes rise with area
    //   frames:  bytes fall as the stride rises
    // Bisection converges inside the range and needs NO accurate size model —
    // which matters, because the published "+-6 CRF halves/doubles the bitrate"
    // rule measurably fails at the extremes: CRF 24 -> 52 on VP9 gave ~3x here,
    // not the ~25x the rule predicts. A model-driven jump undershot and
    // converged worse than a plain search, so: search, don't predict.
    //
    // `lastBytes` carries each probe's result out of the evaluator. -1 = error,
    // -2 = even the extreme end of the range does not fit.
    int lastBytes = bytes;
    const int kMaxAttempts = 16;

    // Bisect [lo,hi] for the SMALLEST value whose encode fits (bytes
    // non-increasing over the range). Returns the winning value, or -1/-2.
    auto bisectSmallest = [&](int lo, int hi, const std::function<int(int)>& eval) -> int {
        if (eval(hi) < 0) return -1;
        if (lastBytes > cap) return -2;
        int a = lo, b = hi; // invariant: f(a) > cap, f(b) <= cap
        while (b - a > 1) {
            if (result.attempts >= kMaxAttempts) break;
            const int m = a + (b - a) / 2;
            if (eval(m) < 0) return -1;
            if (lastBytes <= cap) b = m; else a = m;
        }
        return b;
    };

    // Bisect [lo,hi] for the LARGEST value that fits (bytes non-decreasing).
    auto bisectLargest = [&](int lo, int hi, const std::function<int(int)>& eval) -> int {
        if (eval(lo) < 0) return -1;
        if (lastBytes > cap) return -2;
        int a = lo, b = hi; // invariant: f(a) <= cap, f(b) > cap
        while (b - a > 1) {
            if (result.attempts >= kMaxAttempts) break;
            const int m = a + (b - a) / 2;
            if (eval(m) < 0) return -1;
            if (lastBytes <= cap) a = m; else b = m;
        }
        return a;
    };

    for (const QString& phase : aTarget.degradeOrder) {
        if (result.ok) break;
        if (phase == QLatin1String("quality") && quality) {
            const int win = bisectSmallest(
                crf, quality->worst,
                std::function<int(int)>([&](int c) {
                    lastBytes = encodeOnce(c, scale, stride, QStringLiteral("quality"));
                    return lastBytes;
                }));
            if (win >= 0) {
                crf = win;
                result.ok = true;
            } else if (win == -2) {
                // Even the worst quality is too big: KEEP the floor and let the
                // next phase build on it (otherwise later phases silently
                // revert to the best quality and undo this phase's work).
                crf = quality->worst;
            }
        } else if (phase == QLatin1String("scale")) {
            // Search the scale in percent so bisection works in integers.
            // Floor comes from the data (never shrink past it: a visible cliff).
            const int floorPct = std::max(1, aTarget.minScalePct);
            const int win = bisectLargest(
                floorPct, 100,
                std::function<int(int)>([&](int pct) {
                    lastBytes = encodeOnce(crf, (double)pct / 100.0, stride, QStringLiteral("scale"));
                    return lastBytes;
                }));
            if (win >= 0) {
                scale = (double)win / 100.0;
                result.ok = true;
            } else if (win == -2) {
                scale = (double)floorPct / 100.0; // keep the floor, keep going
            }
        } else if (phase == QLatin1String("frames")) {
            if (base.size() < 2) continue;
            const int win = bisectSmallest(
                stride, (int)base.size(),
                std::function<int(int)>([&](int st) {
                    lastBytes = encodeOnce(crf, scale, st, QStringLiteral("frames"));
                    return lastBytes;
                }));
            if (win >= 0) {
                stride = win;
                result.ok = true;
            } else if (win == -2) {
                stride = (int)base.size(); // nothing left to drop
            }
        }
    }

    if (result.ok) {
        result.steps << QStringLiteral("fit under the %1 byte cap in %2 attempts")
                            .arg((qlonglong)cap)
                            .arg(result.attempts);
        return result;
    }

    result.error = QStringLiteral(
                       "could not fit under %1's %2 byte cap (best was %3 bytes at %4x%5). "
                       "Shorten the animation or simplify the art — quality and size are "
                       "already at their floors.")
                       .arg(aTarget.label)
                       .arg((qlonglong)cap)
                       .arg((qlonglong)smallest)
                       .arg(result.finalSize.width())
                       .arg(result.finalSize.height());
    return result;
}

} // namespace ctrl
