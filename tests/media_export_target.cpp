// Export-destination table prototype (DESIGN RESEARCH).
//
// This is the gate for the "destination-driven export" design: the table in
// assets/export/ExportTargets.txt is DATA, and this test proves three things
// about it.
//
//   1. Every row is well-formed and its codec actually exists in the linked
//      libav* build (and its muxer can carry it).
//   2. Constraint application (size rules, fps/duration/frame caps) does what
//      the destination's published limits require.
//   3. A resolved plan actually encodes — the loop is closed from data to a
//      real, decodable file.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/media_export_target
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QTextStream>

#include "ctrl/ExportTarget.h"
#include "media/FFmpegHelpers.h"
#include "media/VideoEncoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QVector>
#include <algorithm>
#include <cmath>
#include <climits>

static int failures = 0;

static void check(bool ok, const char* what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

static void check(bool ok, const QString& what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// Deterministic synthetic RGBA frame: gradient + alpha ramp, so both luma and
// chroma vary and alpha is exercised when the target carries it.
static QImage makeFrame(int w, int h, int idx) {
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            line[4 * x + 0] = uchar((x * 255) / (w == 0 ? 1 : w));
            line[4 * x + 1] = uchar((y * 255) / (h == 0 ? 1 : h));
            line[4 * x + 2] = uchar(((x + y + idx) * 255) / ((w + h) == 0 ? 1 : (w + h)));
            line[4 * x + 3] = uchar(255 - ((x + y) % 256));
        }
    }
    return img;
}

// Everything we can learn about a produced file that a destination actually
// cares about: dimensions, frame count, the container's stream codec tag, how
// many audio streams exist, and whether the decoded alpha channel varies.
struct Probe {
    bool opened = false;
    int frames = 0;
    int width = 0;
    int height = 0;
    uint32_t codecTag = 0;
    int audioStreams = 0;
    bool alphaDecoded = false;
    int minAlpha = 255;
    int maxAlpha = 0;
    QString decodedPixFmt; // what the DECODER produced (not the container's tag)
};

// `aDecoderName` forces a specific decoder. This matters: the native FFmpeg VP9
// decoder DISCARDS the alpha plane, so VP9+alpha can only be observed through
// the libvpx decoder.
static Probe probeFile(const QString& path, const char* aDecoderName = nullptr) {
    Probe p;
    AVFormatContext* fmt = nullptr;
    if (::avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return p;
    ::avformat_find_stream_info(fmt, nullptr);
    const int vs = ::av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { ::avformat_close_input(&fmt); return p; }

    p.opened = true;
    p.codecTag = fmt->streams[vs]->codecpar->codec_tag;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++p.audioStreams;
    }

    const AVCodec* dec = aDecoderName
                             ? ::avcodec_find_decoder_by_name(aDecoderName)
                             : ::avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) { ::avformat_close_input(&fmt); return p; }

    AVCodecContext* ctx = ::avcodec_alloc_context3(dec);
    ::avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
    if (::avcodec_open2(ctx, dec, nullptr) < 0) {
        ::avcodec_free_context(&ctx);
        ::avformat_close_input(&fmt);
        return p;
    }
    p.width = ctx->width;
    p.height = ctx->height;

    // The sws context is built from the FIRST DECODED FRAME's format, not from
    // ctx->pix_fmt: a decoder's negotiated context format can differ from what
    // its frames actually carry (this is exactly how VP9 alpha hides — the
    // decoder emits yuva420p frames while the context still advertises a
    // non-alpha format, and scaling from the context format silently drops the
    // alpha plane).
    SwsContext* sws = nullptr;
    int swsFmt = -1;
    AVFrame* frame = ::av_frame_alloc();
    AVPacket* pkt = ::av_packet_alloc();

    auto pull = [&](AVFrame* f) {
        if (!sws || swsFmt != f->format) {
            if (sws) ::sws_freeContext(sws);
            swsFmt = f->format;
            sws = ::sws_getContext(ctx->width, ctx->height, (AVPixelFormat)f->format, ctx->width,
                                   ctx->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                                   nullptr);
            if (!sws) return;
        }
        QImage img(ctx->width, ctx->height, QImage::Format_RGBA8888);
        uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
        if (::sws_scale(sws, f->data, f->linesize, 0, ctx->height, dst, dstStride) <= 0) return;
        ++p.frames;
        p.alphaDecoded = true;
        if (p.decodedPixFmt.isEmpty()) {
            p.decodedPixFmt = QString::fromUtf8(::av_get_pix_fmt_name((AVPixelFormat)f->format));
        }
        for (int y = 0; y < img.height(); ++y) {
            const uchar* line = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int a = line[4 * x + 3];
                if (a < p.minAlpha) p.minAlpha = a;
                if (a > p.maxAlpha) p.maxAlpha = a;
            }
        }
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
    if (sws) ::sws_freeContext(sws);
    ::avcodec_free_context(&ctx);
    ::avformat_close_input(&fmt);
    return p;
}

static int decodeInfo(const QString& path, int* outW, int* outH) {
    const Probe p = probeFile(path);
    *outW = p.width;
    *outH = p.height;
    return p.frames;
}

// ---------------------------------------------------------------------------

static void section(const char* title) {
    QTextStream(stdout) << "\n== " << title << " ==\n";
}

// Some destinations can only be produced on one OS (HEVC+alpha needs Apple's
// VideoToolbox). Those rows are declared, so validation can skip them instead
// of reporting them as broken.
static bool platformMatches(const QString& aPlatform) {
    if (aPlatform.isEmpty() || aPlatform == QLatin1String("any")) return true;
#ifdef Q_OS_MACOS
    if (aPlatform == QLatin1String("apple")) return true;
#endif
#ifdef Q_OS_WIN
    if (aPlatform == QLatin1String("windows")) return true;
#endif
#ifdef Q_OS_LINUX
    if (aPlatform == QLatin1String("linux")) return true;
#endif
    return false;
}

static void run_load_and_validate() {
    section("load + validate every row");

    QString err;
    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH), &err);
    check(err.isEmpty(), QStringLiteral("table parses (%1)").arg(err.isEmpty() ? QStringLiteral("ok") : err));
    check(all.size() >= 12, QStringLiteral("table has a meaningful row count (%1)").arg(all.size()));
    if (all.isEmpty()) return;

    // ids unique
    QSet<QString> seen;
    bool unique = true;
    for (const auto& t : all) {
        if (seen.contains(t.id)) unique = false;
        seen.insert(t.id);
    }
    check(unique, "destination ids are unique");

    // every row: container present; fixed codecs exist and fit their muxer
    int fixedRows = 0, ladderRows = 0;
    for (const auto& t : all) {
        if (t.container.isEmpty()) {
            check(false, QStringLiteral("%1: missing container").arg(t.id));
            continue;
        }
        if (!platformMatches(t.platform)) {
            QTextStream(stdout) << "      note: " << t.id << " is platform-specific ("
                                << t.platform << ") — skipped on this OS\n";
            continue;
        }
        if (t.codecLadder) {
            ++ladderRows;
            continue;
        }
        ++fixedRows;
        const bool isGif = t.container.compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0;
        if (isGif) continue; // media::GifEncoder owns GIF, not VideoEncoder

        const QString resolved = media::ffmpeg::resolveEncoderName(t.codec);
        check(!resolved.isEmpty(),
              QStringLiteral("%1: codec '%2' exists in this build").arg(t.id, t.codec));
        if (resolved.isEmpty()) continue;

        check(media::ffmpeg::muxerSupportsCodec(t.container, resolved),
              QStringLiteral("%1: muxer '%2' can carry '%3'").arg(t.id, t.container, resolved));
    }
    check(fixedRows >= 8, QStringLiteral("dictated-codec rows present (%1)").arg(fixedRows));
    check(ladderRows >= 3, QStringLiteral("ladder rows present (%1)").arg(ladderRows));

    // provenance: every row is dated, and unverified rows are flagged as such
    int dated = 0, unverified = 0;
    for (const auto& t : all) {
        if (!t.checked.isEmpty()) ++dated;
        if (!t.verified) {
            ++unverified;
            QTextStream(stdout) << "      note: " << t.id << " is UNVERIFIED (checked " << t.checked << ")\n";
        }
    }
    check(dated == all.size(), QStringLiteral("every row carries a checked date (%1/%2)").arg(dated).arg(all.size()));
    check(unverified <= 1, QStringLiteral("at most one unverified row (%1)").arg(unverified));

    // The app loads this table from the embedded resource, not from disk. Prove
    // that path works — otherwise the default is only ever exercised in a full
    // app run, which is exactly where a missing qrc entry would hide.
    const QList<ctrl::ExportTarget> fromResource = ctrl::ExportTargets::load();
    check(fromResource.size() == all.size(),
          QStringLiteral("table loads from the app resource %1 (%2 rows)")
              .arg(ctrl::ExportTargets::defaultPath())
              .arg(fromResource.size()));
}

static void run_size_rules() {
    section("size rules");

    const ctrl::ExportTarget telegram =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("telegram.sticker"));
    check(telegram.id == QLatin1String("telegram.sticker"), "found telegram.sticker");

    // Telegram: one side exactly 512, other <= 512.
    const QSize a = ctrl::applySizeRule(telegram.size, QSize(1000, 800));
    check(a.width() == 512 && a.height() <= 512 && a.height() > 0,
          QStringLiteral("telegram 1000x800 -> %1x%2 (one side exactly 512)").arg(a.width()).arg(a.height()));
    const QSize b = ctrl::applySizeRule(telegram.size, QSize(1000, 1000));
    check(b == QSize(512, 512), QStringLiteral("telegram 1000x1000 -> 512x512 (got %1x%2)").arg(b.width()).arg(b.height()));

    // LINE: fit inside 320x270, long side >= 270.
    const ctrl::ExportTarget line =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("line.sticker"));
    const QSize c = ctrl::applySizeRule(line.size, QSize(1000, 800));
    check(c.width() <= 320 && c.height() <= 270 && std::max(c.width(), c.height()) >= 270,
          QStringLiteral("LINE 1000x800 -> %1x%2 (within 320x270, long side >= 270)").arg(c.width()).arg(c.height()));

    // Discord: exactly 320x320.
    const ctrl::ExportTarget discord =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("discord.sticker"));
    const QSize d = ctrl::applySizeRule(discord.size, QSize(1000, 800));
    check(d == QSize(320, 320), QStringLiteral("discord 1000x800 -> exactly 320x320 (got %1x%2)").arg(d.width()).arg(d.height()));

    // Exact-size rows must also say what to RENDER at, so non-square art is
    // letterboxed with transparency rather than stretched.
    ctrl::ExportRequest exactReq;
    exactReq.outputPath = QDir::temp().filePath("ae_exact.apng");
    exactReq.sourceSize = QSize(1000, 800);
    exactReq.frames = 1;
    const ctrl::ExportPlan exactPlan =
        ctrl::planExport(discord, exactReq, ctrl::QualityBucket::Balanced);
    check(exactPlan.ok(), QStringLiteral("exact-size plan builds (%1)").arg(exactPlan.error));
    check(exactPlan.renderSize == QSize(320, 256),
          QStringLiteral("renderSize fits inside the exact box (got %1x%2)")
              .arg(exactPlan.renderSize.width())
              .arg(exactPlan.renderSize.height()));
    check(exactPlan.config.fit == media::FitMode::Pad,
          "exact-size rows default to Pad (fit + transparent margin, never stretch)");

    // ...but pad is a CHOICE, not a law: platforms disagree about margins
    // (Signal asks for 16px, WhatsApp an 8px stroke, LINE forbids them), so a
    // destination may declare crop instead.
    ctrl::ExportTarget croppable = discord;
    croppable.size.fit = ctrl::FitMode::Crop;
    const ctrl::ExportPlan cropPlan =
        ctrl::planExport(croppable, exactReq, ctrl::QualityBucket::Balanced);
    check(cropPlan.config.fit == media::FitMode::Crop,
          "a destination declaring fit=crop gets Crop");

    // Signal: fits inside 512x512 and fills it (stickers resize to 512).
    const ctrl::ExportTarget signal =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("signal.sticker"));
    const QSize e = ctrl::applySizeRule(signal.size, QSize(200, 200));
    check(e == QSize(512, 512), QStringLiteral("signal 200x200 -> 512x512 (got %1x%2)").arg(e.width()).arg(e.height()));

    // Even dimensions for YUV.
    const QSize f = ctrl::applySizeRule(telegram.size, QSize(1001, 801));
    check((f.width() % 2) == 0 && (f.height() % 2) == 0,
          QStringLiteral("odd source still yields even output (%1x%2)").arg(f.width()).arg(f.height()));
}

static void run_caps() {
    section("caps (deterministic pre-encode enforcement)");

    const ctrl::ExportTarget telegram =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("telegram.sticker"));

    int fps = 60, frames = 300; // 5 seconds at 60fps
    QStringList notes;
    ctrl::applyCaps(telegram.caps, &fps, &frames, &notes);
    check(fps == 30, QStringLiteral("fps clamped to 30 (got %1)").arg(fps));
    check(frames == 90, QStringLiteral("duration clamped to 3s -> 90 frames (got %1)").arg(frames));

    // LINE's minimum frame count cannot be invented — it must WARN, not silently pass.
    const ctrl::ExportTarget line =
        ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                 QStringLiteral("line.sticker"));
    int lfps = 24, lframes = 3;
    QStringList lnotes;
    ctrl::applyCaps(line.caps, &lfps, &lframes, &lnotes);
    const bool warned = lnotes.filter(QStringLiteral("WARNING")).size() == 1;
    check(warned, "LINE min-frames (5) under-run produces a warning, not a silent pass");

    // The caps must be applied exactly ONCE by planExport. It used to reset
    // fps/frames back to the requested values and re-apply them at the end,
    // which clamped to the same numbers but appended every cap note to
    // plan.notes a second time — so the user saw each warning twice.
    ctrl::ExportRequest req;
    req.outputPath = QDir::temp().filePath("ae_caps_once.png");
    req.sourceSize = QSize(320, 240);
    req.fps = 30;
    req.frames = 50; // LINE's cap is 20, so this must be trimmed
    const ctrl::ExportPlan plan = ctrl::planExport(line, req, ctrl::QualityBucket::Balanced);
    check(plan.ok(), QStringLiteral("the capped plan builds (%1)").arg(plan.error));
    check(plan.frames == 20, QStringLiteral("LINE's cap lands on 20 frames (got %1)").arg(plan.frames));
    QSet<QString> seenNotes;
    bool duplicated = false;
    for (const QString& n : plan.notes) {
        if (seenNotes.contains(n)) duplicated = true;
        seenNotes.insert(n);
    }
    check(!duplicated, QStringLiteral("no note is duplicated by the cap pass"));
}

static void run_probe_sanity() {
    section("availability probe: name lookup vs real one-frame encode");

    // The point of this section: encoderAvailable() only proves an encoder was
    // compiled in. Hardware encoders are listed on machines that cannot run
    // them, so the ladder MUST gate on encoderUsable(). Report every name where
    // the two disagree — that is the false-positive set we would otherwise
    // have selected.
    const char* hwNames[] = {"av1_nvenc", "hevc_nvenc", "h264_nvenc",
                             "av1_qsv",   "hevc_qsv",   "h264_qsv",
                             "av1_vaapi", "hevc_vaapi", "h264_vaapi",
                             "av1_amf",   "hevc_amf",   "h264_amf"};
    int disagree = 0;
    for (const char* n : hwNames) {
        const bool listed = media::ffmpeg::encoderAvailable(n);
        const bool usable = media::ffmpeg::encoderUsable(n);
        if (listed != usable) {
            ++disagree;
            QTextStream(stdout) << "      " << n << ": listed=" << (listed ? "yes" : "no")
                                << " usable=" << (usable ? "yes" : "no") << "\n";
        }
    }
    QTextStream(stdout) << "      hardware encoders where listed != usable: " << disagree << "\n";
    check(true, "hardware-encoder disagreement reported (informational)");

    // Software encoders we rely on must be genuinely usable.
    check(media::ffmpeg::encoderUsable("libx264"), "libx264 is usable");
    check(media::ffmpeg::encoderUsable("libx265"), "libx265 is usable");
}

static void run_ladder() {
    section("codec ladder (class x bucket)");

    const ctrl::DestClass classes[] = {ctrl::DestClass::Reencode, ctrl::DestClass::Direct,
                                       ctrl::DestClass::Archive};
    const ctrl::QualityBucket buckets[] = {ctrl::QualityBucket::Smaller, ctrl::QualityBucket::Balanced,
                                           ctrl::QualityBucket::Best, ctrl::QualityBucket::Lossless};
    const char* classNames[] = {"reencode", "direct", "archive"};
    const char* bucketNames[] = {"smaller", "balanced", "best", "lossless"};

    for (int ci = 0; ci < 3; ++ci) {
        for (int bi = 0; bi < 4; ++bi) {
            QString why;
            const QString codec = ctrl::resolveLadderCodec(classes[ci], buckets[bi], ctrl::LadderHints(), &why);
            const bool ok = !codec.isEmpty() && media::ffmpeg::encoderUsable(codec.toUtf8().constData());
            check(ok, QStringLiteral("%1/%2 -> %3").arg(classNames[ci], bucketNames[bi],
                                                         ok ? codec : QStringLiteral("NONE")));
            if (ok) QTextStream(stdout) << "      why: " << why << "\n";
        }
    }

    // The one ordering claim the evidence actually supports: direct playback
    // must favour decode compatibility over efficiency at every bucket.
    bool allH264 = true;
    for (int bi = 0; bi < 4; ++bi) {
        const QString codec =
            ctrl::resolveLadderCodec(ctrl::DestClass::Direct, buckets[bi], ctrl::LadderHints());
        if (!codec.contains(QLatin1String("264"))) allH264 = false;
    }
    check(allH264, "direct playback picks H.264 at every bucket (compatibility first)");
}

static uint32_t fourcc(const QString& s) {
    if (s.size() != 4) return 0;
    return (uint32_t)MKTAG(s.toUtf8()[0], s.toUtf8()[1], s.toUtf8()[2], s.toUtf8()[3]);
}

// Interleaved float sine, one buffer for every channel.
static QVector<float> makeAudio(int aFrames, int aFps, int aRate, int aChannels) {
    const int perChannel = (int)((double)aFrames / (double)aFps * (double)aRate);
    QVector<float> out(perChannel * aChannels);
    for (int i = 0; i < perChannel; ++i) {
        const double kPi = 3.14159265358979323846; // M_PI needs _USE_MATH_DEFINES on MSVC
        const float v = 0.25f * (float)std::sin(2.0 * kPi * 440.0 * (double)i / (double)aRate);
        for (int c = 0; c < aChannels; ++c) out[i * aChannels + c] = v;
    }
    return out;
}

// Codecs whose decoded alpha we can actually observe in-process. WebP's
// libwebp decoder cannot read ANIMATED WebP, and HEVC alpha is not surfaced
// through the native decoder, so those rows skip the alpha assertion (rather
// than asserting something the probe cannot see).
static bool alphaObservable(const QString& aCodec) {
    return aCodec == QLatin1String("libvpx-vp9") || aCodec == QLatin1String("apng") ||
           aCodec == QLatin1String("ffv1");
}

static void run_conformance() {
    section("destination conformance (every row)");

    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));

    for (const ctrl::ExportTarget& t : all) {
        if (t.container.compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0) {
            check(true, QStringLiteral("%1: GIF row routed to GifEncoder (not encoded here)").arg(t.id));
            continue;
        }

        const QString out = QDir::temp().filePath(QStringLiteral("ae_conf_%1.%2")
                                                      .arg(QString(t.id).replace(QLatin1Char('.'), QLatin1Char('_')),
                                                           t.container));

        ctrl::ExportRequest req;
        req.outputPath = out;
        req.sourceSize = QSize(640, 480); // deliberately non-square
        req.fps = 30;
        req.frames = 3;
        req.exportableAudioTracks = 2; // the project has sound; each row decides if it lands

        const ctrl::ExportPlan plan = ctrl::planExport(t, req, ctrl::QualityBucket::Balanced);
        if (!plan.ok()) {
            // A destination whose codec is platform-specific (HEVC+alpha needs
            // VideoToolbox) cannot always produce a file. The acceptable outcome
            // is a CLEAN, EXPLAINED refusal — never a silent one, and never a
            // crash. Silently producing wrong output is the only failure here.
            check(!plan.error.isEmpty(),
                  QStringLiteral("%1: unavailable here, refused cleanly: %2").arg(t.id, plan.error));
            QTextStream(stdout) << "      " << t.id << " -> UNAVAILABLE: " << plan.error << "\n";
            continue;
        }

        // Render at the plan's RENDER size, exactly as the app would: the
        // encoder is what fits + pads to the output size.
        QList<QImage> frames;
        for (int i = 0; i < plan.frames; ++i) {
            frames.append(makeFrame(plan.renderSize.width(), plan.renderSize.height(), i));
        }

        media::VideoEncoder enc;
        if (!enc.open(plan.config)) {
            check(false, QStringLiteral("%1: open succeeds (%2)").arg(t.id, enc.error()));
            continue;
        }
        for (const QImage& f : frames) enc.write(f);
        if (plan.config.audioChannels > 0) {
            const QVector<float> pcm =
                makeAudio(plan.frames, plan.fps, plan.config.audioSampleRate, plan.config.audioChannels);
            enc.setAudio(pcm.constData(), plan.config.audioChannels, plan.config.audioSampleRate,
                         pcm.size() / plan.config.audioChannels);
        }
        if (!enc.close()) {
            check(false, QStringLiteral("%1: encode succeeds (%2)").arg(t.id, enc.error()));
            continue;
        }
        check(QFileInfo(out).size() > 0, QStringLiteral("%1: file written").arg(t.id));

        // VP9 alpha is only visible to the libvpx decoder; the native one drops it.
        const char* forced =
            plan.config.codec == QLatin1String("libvpx-vp9") ? "libvpx-vp9" : nullptr;
        const Probe p = probeFile(out, forced);

        QTextStream(stdout) << "      " << t.id << " -> " << plan.config.codec << " "
                            << plan.size.width() << "x" << plan.size.height()
                            << " alpha=" << (plan.alphaEnabled ? "yes" : "no")
                            << " audio=" << (plan.config.audioChannels > 0 ? "yes" : "no")
                            << " decoded=" << p.decodedPixFmt
                            << " pixFmt=" << plan.config.pixFmt << "\n";

        if (!p.opened || p.frames == 0) {
            // Animated WebP is the known case: encodable, not decodable here.
            QTextStream(stdout) << "        (no in-process decode; skipping decode assertions)\n";
            continue;
        }

        check(p.width == plan.size.width() && p.height == plan.size.height(),
              QStringLiteral("%1: decoded dims %2x%3 match the plan")
                  .arg(t.id)
                  .arg(p.width)
                  .arg(p.height));

        // Audio capability is enforced, not advisory.
        const int expectedAudio = plan.config.audioChannels > 0 ? 1 : 0;
        check(p.audioStreams == expectedAudio,
              QStringLiteral("%1: audio streams = %2 (expected %3; capability=%4)")
                  .arg(t.id)
                  .arg(p.audioStreams)
                  .arg(expectedAudio)
                  .arg(plan.audioEnabled ? "allowed" : "forbidden"));

        // Alpha: required rows must carry a varying alpha; forbidden rows must not.
        if (alphaObservable(plan.config.codec)) {
            if (plan.alphaEnabled) {
                check(p.minAlpha < p.maxAlpha,
                      QStringLiteral("%1: alpha varies (min=%2 max=%3) as required")
                          .arg(t.id)
                          .arg(p.minAlpha)
                          .arg(p.maxAlpha));
            } else {
                check(p.minAlpha == p.maxAlpha,
                      QStringLiteral("%1: no alpha channel (min==max==%2) as required")
                          .arg(t.id)
                          .arg(p.minAlpha));
            }
        }

        // Stream codec tag (hvc1) must actually land in the container.
        if (!t.codecTag.isEmpty()) {
            check(p.codecTag == fourcc(t.codecTag),
                  QStringLiteral("%1: stream codec tag is '%2' (got 0x%3)")
                      .arg(t.id, t.codecTag)
                      .arg(p.codecTag, 0, 16));
        }
    }
}

// A misspelt encoder option must be an ERROR, not a silently-ignored typo:
// "auto-alt-ref" typo'd means broken alpha with no other symptom.
static void run_option_typo_is_fatal() {
    section("unknown encoder option is rejected");

    media::VideoEncoder::Config cfg;
    cfg.outputPath = QDir::temp().filePath("ae_badopt.webm");
    cfg.container = QStringLiteral("webm");
    cfg.codec = QStringLiteral("libvpx-vp9");
    cfg.pixFmt = QStringLiteral("yuva420p");
    cfg.transparent = true;
    cfg.width = 64;
    cfg.height = 64;
    cfg.fps = 24;
    cfg.encOptions.insert(QStringLiteral("auto-alt-reff"), QStringLiteral("0")); // typo

    media::VideoEncoder enc;
    const bool opened = enc.open(cfg);
    check(!opened, "a typo'd encoder option fails open() instead of being ignored");
    if (!opened) QTextStream(stdout) << "      error: " << enc.error() << "\n";

    // ...and the correctly-spelled option still works.
    media::VideoEncoder::Config ok = cfg;
    ok.encOptions.clear();
    ok.encOptions.insert(QStringLiteral("auto-alt-ref"), QStringLiteral("0"));
    media::VideoEncoder enc2;
    check(enc2.open(ok), "the correctly-spelled option opens");
    enc2.close();
}

static void run_throughput_gate() {
    section("throughput gate (is the codec fast enough HERE?)");

    // Same job, two budgets. The invariant is not "which codec" (that depends
    // on the machine) but that a tight budget never selects a SLOWER codec than
    // an unconstrained one — i.e. the gate actually does something.
    ctrl::LadderHints base;
    base.width = 1280;
    base.height = 720;
    base.pixFmt = QStringLiteral("yuv420p");
    base.frameCount = 20000;

    ctrl::LadderHints tight = base;
    tight.maxSeconds = 30.0; // 20k frames in 30s is impossible for slow codecs

    QString whyTight, whyFree;
    const QString freePick = ctrl::resolveLadderCodec(
        ctrl::DestClass::Reencode, ctrl::QualityBucket::Best, base, &whyFree);
    const QString tightPick = ctrl::resolveLadderCodec(
        ctrl::DestClass::Reencode, ctrl::QualityBucket::Best, tight, &whyTight);

    check(!freePick.isEmpty() && !tightPick.isEmpty(), "both budgets yield a codec");
    QTextStream(stdout) << "      unconstrained: " << freePick << " (" << whyFree << ")\n";
    QTextStream(stdout) << "      tight budget : " << tightPick << " (" << whyTight << ")\n";

    const double tf =
        media::ffmpeg::encoderThroughput(freePick, base.width, base.height, base.pixFmt);
    const double tt =
        media::ffmpeg::encoderThroughput(tightPick, base.width, base.height, base.pixFmt);
    check(tf > 0.0 && tt > 0.0, "throughput is measurable for both");
    check(tt >= tf * 0.9,
          QStringLiteral("a tight budget never selects a slower codec (%1 -> %2 fps)")
              .arg(freePick, tightPick)
              .arg(tf)
              .arg(tt));
}

static void run_audio_is_derived() {
    section("audio is derived, not requested");

    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const ctrl::ExportTarget yt =
        ctrl::ExportTargets::find(all, QStringLiteral("youtube.video"));
    const ctrl::ExportTarget tg =
        ctrl::ExportTargets::find(all, QStringLiteral("telegram.sticker"));

    // Audio is not a preference: it is included iff the project HAS audio, the
    // user did not mute, and the destination permits it. Encode each case and
    // count the actual audio streams in the produced file.
    auto audioStreams = [&](const ctrl::ExportTarget& t, bool aProjectHasAudio, bool aMute) -> int {
        ctrl::ExportRequest req;
        req.outputPath = QDir::temp().filePath(QStringLiteral("ae_audio.%1").arg(t.container));
        req.sourceSize = QSize(320, 240);
        req.fps = 30;
        req.frames = 3;
        req.exportableAudioTracks = aProjectHasAudio ? 2 : 0;
        req.mute = aMute;

        const ctrl::ExportPlan plan =
            ctrl::planExport(t, req, ctrl::QualityBucket::Balanced);
        if (!plan.ok()) return -1;

        QList<QImage> frames;
        for (int i = 0; i < plan.frames; ++i) {
            frames.append(makeFrame(plan.renderSize.width(), plan.renderSize.height(), i));
        }

        media::VideoEncoder enc;
        if (!enc.open(plan.config)) return -1;
        for (const QImage& f : frames) enc.write(f);
        if (plan.config.audioChannels > 0) {
            const QVector<float> pcm = makeAudio(plan.frames, plan.fps,
                                                 plan.config.audioSampleRate,
                                                 plan.config.audioChannels);
            enc.setAudio(pcm.constData(), plan.config.audioChannels,
                         plan.config.audioSampleRate,
                         pcm.size() / plan.config.audioChannels);
        }
        if (!enc.close()) return -1;
        return probeFile(req.outputPath).audioStreams;
    };

    check(audioStreams(yt, true, false) == 1,
          "project has audio + destination permits -> 1 audio stream");
    check(audioStreams(yt, true, true) == 0, "muted -> no audio stream");
    check(audioStreams(yt, false, false) == 0,
          "silent project (or all tracks reference-only) -> no audio stream");
    check(audioStreams(tg, true, false) == 0,
          "destination that forbids audio -> no audio stream");

    // And the derived decision must be visible, not silent.
    ctrl::ExportRequest noteReq;
    noteReq.sourceSize = QSize(640, 480);
    noteReq.exportableAudioTracks = 2;
    const ctrl::ExportPlan notePlan =
        ctrl::planExport(tg, noteReq, ctrl::QualityBucket::Balanced);
    const bool explained = notePlan.notes.filter(QStringLiteral("No sound")).size() == 1;
    check(explained, "a destination leaving sound out says so in the plan notes");

    // Audio parameters are per-destination, and the container still wins for
    // the codec: WebM cannot carry AAC.
    ctrl::ExportRequest ar;
    ar.sourceSize = QSize(640, 480);
    ar.exportableAudioTracks = 1;
    const ctrl::ExportPlan ytPlan =
        ctrl::planExport(yt, ar, ctrl::QualityBucket::Balanced);
    check(ytPlan.config.audioCodec == QLatin1String("aac") &&
              ytPlan.config.audioBitrate == 256000,
          QStringLiteral("YouTube gets its declared audio params (%1 @ %2)")
              .arg(ytPlan.config.audioCodec)
              .arg(ytPlan.config.audioBitrate));
    const ctrl::ExportPlan obsPlan =
        ctrl::planExport(ctrl::ExportTargets::find(all, QStringLiteral("obs.overlay")), ar,
                         ctrl::QualityBucket::Balanced);
    check(obsPlan.config.audioCodec == QLatin1String("libopus"),
          QStringLiteral("a WebM destination gets Opus, not AAC (got %1)")
              .arg(obsPlan.config.audioCodec));
    const ctrl::ExportPlan arcPlan =
        ctrl::planExport(ctrl::ExportTargets::find(all, QStringLiteral("archive.master")), ar,
                         ctrl::QualityBucket::Balanced);
    check(arcPlan.config.audioCodec == QLatin1String("flac"),
          "an archive master keeps audio lossless (flac)");
}

static void run_fit_to_cap() {
    section("post-encode fit-to-cap");

    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const ctrl::ExportTarget base =
        ctrl::ExportTargets::find(all, QStringLiteral("telegram.sticker"));

    QList<QImage> frames;
    for (int i = 0; i < 6; ++i) frames.append(makeFrame(640, 480, i));

    ctrl::ExportRequest req;
    req.outputPath = QDir::temp().filePath("ae_fit.webm");
    req.sourceSize = QSize(640, 480);
    req.fps = 30;
    req.frames = frames.size();

    // A cap small enough that the FIRST attempt cannot possibly fit, so the
    // loop has to degrade. File size is the one limit that cannot be known
    // before encoding — that is the whole reason this loop exists.
    ctrl::ExportTarget tiny = base;
    tiny.caps.maxBytes = 2500;

    const ctrl::FitResult r =
        ctrl::exportWithFit(tiny, req, ctrl::QualityBucket::Balanced, frames);
    for (const QString& step : r.steps) QTextStream(stdout) << "      " << step << "\n";
    check(r.attempts > 1, QStringLiteral("the fit loop iterated (%1 attempts)").arg(r.attempts));
    if (r.ok) {
        check(r.bytes <= tiny.caps.maxBytes,
              QStringLiteral("fitted under the cap: %1 <= %2 bytes")
                  .arg((qlonglong)r.bytes)
                  .arg((qlonglong)tiny.caps.maxBytes));
    } else {
        check(!r.error.isEmpty(),
              QStringLiteral("gave up cleanly with an explanation: %1").arg(r.error));
    }

    // An unreachable cap must be REPORTED, never silently accepted: a sticker
    // over its cap is a hard rejection at upload time.
    ctrl::ExportTarget impossible = base;
    impossible.caps.maxBytes = 100;
    const ctrl::FitResult r2 =
        ctrl::exportWithFit(impossible, req, ctrl::QualityBucket::Balanced, frames);
    check(!r2.ok && !r2.error.isEmpty(),
          "an impossible cap is reported, not silently accepted");
    QTextStream(stdout) << "      " << r2.error << "\n";
}

static void run_live_encode() {
    section("live encode from a resolved plan");

    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));

    struct Case {
        const char* id;
        ctrl::QualityBucket bucket;
        QSize source;
        int frames;
        const char* ext;
        bool decode; // false when FFmpeg's decoder can't read it back (animated WebP)
    };

    const Case cases[] = {
        {"telegram.sticker", ctrl::QualityBucket::Balanced, QSize(1000, 800), 3, "webm", true},
        {"discord.sticker", ctrl::QualityBucket::Balanced, QSize(1000, 800), 3, "apng", true},
        {"obs.overlay", ctrl::QualityBucket::Balanced, QSize(320, 240), 3, "webm", true},
        {"youtube.video", ctrl::QualityBucket::Best, QSize(320, 240), 5, "mp4", true},
        {"archive.master", ctrl::QualityBucket::Lossless, QSize(320, 240), 3, "mkv", true},
    };

    for (const Case& c : cases) {
        const ctrl::ExportTarget t = ctrl::ExportTargets::find(all, QString::fromLatin1(c.id));
        if (t.id.isEmpty()) {
            check(false, QStringLiteral("%1: row exists").arg(QString::fromLatin1(c.id)));
            continue;
        }

        const QString out = QDir::temp().filePath(QStringLiteral("ae_target_%1.%2").arg(
            QString::fromLatin1(c.id).replace(QLatin1Char('.'), QLatin1Char('_')),
            QString::fromLatin1(c.ext)));

        ctrl::ExportRequest req;
        req.outputPath = out;
        req.sourceSize = c.source;
        req.fps = 30;
        req.frames = c.frames;

        const ctrl::ExportPlan plan = ctrl::planExport(t, req, c.bucket);
        if (!plan.ok()) {
            check(false, QStringLiteral("%1: plan builds (%2)").arg(QString::fromLatin1(c.id), plan.error));
            continue;
        }

        QTextStream(stdout) << "      " << c.id << " -> " << plan.config.codec << " "
                            << plan.size.width() << "x" << plan.size.height() << " @ " << plan.fps
                            << "fps, alpha=" << (plan.alphaEnabled ? "yes" : "no") << "\n";
        for (const QString& n : plan.notes) QTextStream(stdout) << "        - " << n << "\n";

        // Render at the plan's RENDER size, exactly as the app would: the
        // encoder is what fits + pads to the output size.
        QList<QImage> frames;
        for (int i = 0; i < plan.frames; ++i) {
            frames.append(makeFrame(plan.renderSize.width(), plan.renderSize.height(), i));
        }

        media::VideoEncoder enc;
        const bool wrote = enc.open(plan.config) && enc.writeAll(frames, plan.config);
        if (!wrote) {
            check(false, QStringLiteral("%1: encode succeeds (%2)").arg(QString::fromLatin1(c.id), enc.error()));
            continue;
        }
        check(QFileInfo(out).size() > 0, QStringLiteral("%1: file written").arg(QString::fromLatin1(c.id)));

        if (!c.decode) continue;
        int w = 0, h = 0;
        const int n = decodeInfo(out, &w, &h);
        check(n > 0, QStringLiteral("%1: decodes back (%2 frames)").arg(QString::fromLatin1(c.id)).arg(n));
        check(w == plan.size.width() && h == plan.size.height(),
              QStringLiteral("%1: decoded dims %2x%3 match the plan").arg(QString::fromLatin1(c.id)).arg(w).arg(h));
    }
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    run_load_and_validate();
    run_size_rules();
    run_caps();
    run_probe_sanity();
    run_ladder();
    run_conformance();
    run_option_typo_is_fatal();
    run_throughput_gate();
    run_audio_is_derived();
    run_fit_to_cap();
    // run_live_encode() was defined but never called, so this section had never
    // actually run and its assertions were not part of the count that "passed".
    // Everything the suite claims to check has to be reachable from main.
    run_live_encode();

    QTextStream(stdout) << "\n" << (failures ? "FAILURES: " : "ALL PASS ") << failures << "\n";
    return failures ? 1 : 0;
}
