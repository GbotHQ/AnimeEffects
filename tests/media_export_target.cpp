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
#include <QDebug>
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

// ---------------------------------------------------------------------------
// Section registration.
//
// Sections used to be listed by hand in main(), and run_live_encode() was
// written, read as coverage, and never listed — so six destinations' worth of
// encoding assertions silently contributed nothing. Adding the missing call
// fixes the instance; it does not fix the class, because the next section could
// be forgotten the same way.
//
// So DEFINING a section is now what makes it run. The registrar appends itself
// at static-init time and main() walks the registry, which makes
// "defined but never executed" unrepresentable rather than merely discouraged.
// The title is part of the macro so a section cannot forget to announce itself
// either — the run banner and the section are one declaration.
//
// The registry is heap-allocated and deliberately never freed: a static QVector
// would be destroyed in an order we do not control, while registrars hold
// pointers to it.
namespace {

struct TestSection {
    const char* title;
    void (*fn)();
};

QVector<TestSection>& testSections() {
    static QVector<TestSection>* registry = new QVector<TestSection>();
    return *registry;
}

struct TestSectionRegistrar {
    TestSectionRegistrar(const char* aTitle, void (*aFn)()) {
        testSections().append(TestSection{aTitle, aFn});
    }
};

} // namespace

#define TEST_SECTION(title, fn)                              \
    static void fn();                                        \
    static TestSectionRegistrar fn##_registrar(title, &fn);  \
    static void fn()

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

TEST_SECTION("load + validate every row", run_load_and_validate) {

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

TEST_SECTION("size rules", run_size_rules) {
    // A Free rule means the destination imposes no size. That is NOT the same as
    // deciding one: the export dialog's own Width/Height stay in force, and the
    // plan must echo them. Without this a 1920x1080 upload from a 1100x790
    // project was impossible, and — worse — a size forced by the PREVIOUS
    // destination (Twitch's 112px) could stand in as if it were the user's.
    {
        const ctrl::ExportTarget freeRow =
            ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                     QStringLiteral("youtube.video"));
        check(freeRow.size.mode == ctrl::SizeMode::Free, "youtube.video imposes no size");

        ctrl::ExportRequest req;
        req.sourceSize = QSize(1100, 790);
        req.selection = ctrl::FrameSelection::fromCount(1);
        const ctrl::ExportPlan defaults = ctrl::planExport(freeRow, req, ctrl::QualityBucket::Balanced);
        check(defaults.ok() && defaults.size == QSize(1100, 790),
              QStringLiteral("no requested size -> the art's own (%1x%2)")
                  .arg(defaults.size.width()).arg(defaults.size.height()));

        req.requestedSize = QSize(1920, 1080);
        const ctrl::ExportPlan chosen = ctrl::planExport(freeRow, req, ctrl::QualityBucket::Balanced);
        check(chosen.ok() && chosen.size == QSize(1920, 1080),
              QStringLiteral("requested size is used on a Free rule (%1x%2)")
                  .arg(chosen.size.width()).arg(chosen.size.height()));

        // Odd dimensions are not a user error: YUV needs even, so floor.
        req.requestedSize = QSize(1921, 1081);
        const ctrl::ExportPlan odd = ctrl::planExport(freeRow, req, ctrl::QualityBucket::Balanced);
        check(odd.size == QSize(1920, 1080),
              QStringLiteral("odd requested size floors to even (%1x%2)")
                  .arg(odd.size.width()).arg(odd.size.height()));

        // The ASPECT a fit rule preserves must stay the ART's, so a size typed
        // for a free destination cannot leak into an exact one's padding.
        req.requestedSize = QSize(1920, 1080);
        req.sourceSize = QSize(1000, 800);
        const ctrl::ExportTarget discordRow =
            ctrl::ExportTargets::find(ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH)),
                                     QStringLiteral("discord.sticker"));
        const ctrl::ExportPlan exact = ctrl::planExport(discordRow, req, ctrl::QualityBucket::Balanced);
        check(exact.ok() && exact.size == QSize(320, 320),
              "an exact rule ignores requestedSize (the destination decides)");
        check(exact.renderSize == QSize(320, 256),
              QStringLiteral("exact renderSize uses the ART's aspect, not the request's (%1x%2)")
                  .arg(exact.renderSize.width()).arg(exact.renderSize.height()));
    }

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
    exactReq.selection = ctrl::FrameSelection::fromCount(1);
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

TEST_SECTION("caps (deterministic pre-encode enforcement)", run_caps) {

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
    // which clamped to the same numbers but re-derived and re-appended every
    // cap note, so the user saw each warning twice.
    ctrl::ExportRequest creq;
    creq.outputPath = QDir::temp().filePath("ae_caps_once.png");
    creq.sourceSize = QSize(320, 240);
    creq.fps = 30;
    creq.selection = ctrl::FrameSelection::fromCount(50); // LINE's cap is 20
    const ctrl::ExportPlan cplan = ctrl::planExport(line, creq, ctrl::QualityBucket::Balanced);
    check(cplan.ok(), QStringLiteral("the capped plan builds (%1)").arg(cplan.error));
    check(cplan.frames() == 20, QStringLiteral("LINE's cap lands on 20 frames (got %1)").arg(cplan.frames()));
    QSet<QString> seenNotes;
    bool duplicated = false;
    for (const QString& n : cplan.notes) {
        if (seenNotes.contains(n)) duplicated = true;
        seenNotes.insert(n);
    }
    check(!duplicated, QStringLiteral("no note is duplicated by the cap pass"));
}

TEST_SECTION("frame selection (several non-contiguous ranges)", run_frame_selection) {

    using ctrl::FrameRange;
    using ctrl::FrameSelection;

    const FrameSelection one = FrameSelection::fromRange(10, 19);
    check(one.total() == 10, QStringLiteral("fromRange(10,19) is 10 frames (got %1)").arg(one.total()));
    check(one.rangeCount() == 1, QStringLiteral("fromRange is one range"));
    check(FrameSelection::fromCount(5).total() == 5, QStringLiteral("fromCount(5) is 5 frames"));
    check(FrameSelection::fromCount(1).indices() == (QList<int>{0}),
          QStringLiteral("fromCount(1) is frame 0 alone"));
    check(FrameSelection::fromCount(0).isEmpty(), QStringLiteral("fromCount(0) is empty"));
    check(FrameSelection::fromRange(5, 4).isEmpty(),
          QStringLiteral("a backwards range contributes nothing"));
    // Bounds are inclusive, and negative frames are ordinary in this codebase
    // (util::Range supports them), so a negative selection must survive.
    check(FrameSelection::fromRange(-3, -1).indices() == (QList<int>{-3, -2, -1}),
          QStringLiteral("negative ranges are inclusive and preserved"));

    // THE reason this type exists: ranges are PACKED and the gaps are dropped.
    // The old model collapsed them to [first..last] and exported frames the user
    // had excluded.
    FrameSelection multi;
    multi.ranges.append(FrameRange{0, 10});
    multi.ranges.append(FrameRange{50, 60});
    check(multi.total() == 22,
          QStringLiteral("0-10 + 50-60 is 22 frames, not 61 (got %1)").arg(multi.total()));
    check(multi.rangeCount() == 2, QStringLiteral("two usable ranges"));
    const QList<int> idx = multi.indices();
    check(idx.size() == 22, QStringLiteral("indices() yields exactly what is written (got %1)").arg(idx.size()));
    check(idx.value(10) == 10 && idx.value(11) == 50 && idx.last() == 60,
          QStringLiteral("indices() jumps the gap: element 10 -> frame 10, element 11 -> frame 50"));
    check(!idx.contains(11) && !idx.contains(49),
          QStringLiteral("nothing from inside the gap is present"));

    // Order is honoured, not sorted, so the output matches the list on screen.
    FrameSelection reversed;
    reversed.ranges.append(FrameRange{50, 51});
    reversed.ranges.append(FrameRange{0, 1});
    check(reversed.indices() == (QList<int>{50, 51, 0, 1}),
          QStringLiteral("range order is honoured rather than sorted"));

    // Overlaps are not merged: total() must equal the frames actually written,
    // and repeating a frame is a real way to hold one.
    FrameSelection overlap;
    overlap.ranges.append(FrameRange{0, 10});
    overlap.ranges.append(FrameRange{5, 15});
    check(overlap.total() == 22,
          QStringLiteral("overlapping ranges count twice (got %1)").arg(overlap.total()));

    // firstN is the truncation cap enforcement uses, so it must be monotone:
    // a smaller N is always a prefix of a larger one.
    check(multi.firstN(0).isEmpty(), QStringLiteral("firstN(0) is empty"));
    check(multi.firstN(5).indices() == (QList<int>{0, 1, 2, 3, 4}),
          QStringLiteral("firstN(5) takes the first five"));
    check(multi.firstN(15).total() == 15, QStringLiteral("firstN(15) spans the gap and totals 15"));
    check(multi.firstN(15).indices().last() == 53,
          QStringLiteral("firstN crosses into the second range (11 + 4, last=%1)")
              .arg(multi.firstN(15).indices().last()));
    check(multi.firstN(15).rangeCount() == 2, QStringLiteral("firstN keeps the range structure"));
    check(multi.firstN(999).total() == multi.total(), QStringLiteral("firstN past the end is a no-op"));

    // describe() is what the user reads.
    check(one.describe() == QLatin1String("10 frames"),
          QStringLiteral("a single range describes as a count (got '%1')").arg(one.describe()));
    check(FrameSelection::fromCount(1).describe() == QLatin1String("1 frame"),
          QStringLiteral("singular is not pluralised"));
    check(multi.describe().contains(QLatin1String("2 separate ranges")),
          QStringLiteral("a multi-range selection says so (got '%1')").arg(multi.describe()));
    check(FrameSelection().describe().contains(QLatin1String("no frames")),
          QStringLiteral("an empty selection describes clearly"));

    // --- through the planner -------------------------------------------------
    const QList<ctrl::ExportTarget> all =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    if (all.isEmpty()) return;
    const ctrl::ExportTarget generic =
        ctrl::ExportTargets::find(all, QStringLiteral("generic.file")); // no caps
    const ctrl::ExportTarget line =
        ctrl::ExportTargets::find(all, QStringLiteral("line.sticker")); // 5-20 frames

    ctrl::ExportRequest req;
    req.outputPath = QDir::temp().filePath("ae_selection.mp4");
    req.sourceSize = QSize(320, 240);
    req.fps = 30;
    req.selection = multi;

    const ctrl::ExportPlan plan = ctrl::planExport(generic, req, ctrl::QualityBucket::Balanced);
    check(plan.ok(), QStringLiteral("a multi-range plan builds (%1)").arg(plan.error));
    check(plan.frames() == 22, QStringLiteral("plan.frames() is the selection total (got %1)").arg(plan.frames()));
    check(plan.selection.indices() == multi.indices(),
          QStringLiteral("the plan carries the selection through unchanged"));
    bool gapNote = false;
    for (const QString& n : plan.notes) {
        if (n.contains(QLatin1String("back to back"))) gapNote = true;
    }
    check(gapNote, QStringLiteral("the plan explains that the gaps are dropped"));

    // A cap must truncate the SELECTION, not merely a counter — otherwise the
    // caller cannot tell which frames survived.
    ctrl::ExportRequest lreq = req;
    lreq.outputPath = QDir::temp().filePath("ae_selection.png");
    lreq.selection = FrameSelection::fromCount(50);
    const ctrl::ExportPlan lplan = ctrl::planExport(line, lreq, ctrl::QualityBucket::Balanced);
    check(lplan.ok(), QStringLiteral("the capped plan builds (%1)").arg(lplan.error));
    check(lplan.frames() == 20, QStringLiteral("LINE's 20-frame cap applies (got %1)").arg(lplan.frames()));
    check(lplan.selection.total() == 20,
          QStringLiteral("the selection itself is truncated, not just the count"));
    check(lplan.selection.indices().last() == 19,
          QStringLiteral("the cap keeps the FIRST N frames (last=%1)").arg(lplan.selection.indices().last()));

    // (The "cap notes appear exactly once" check lives in run_caps, where the
    // cap behaviour is tested; it is not repeated here.)

    // An empty selection is refused, rather than silently planning 0 frames.
    ctrl::ExportRequest ereq = req;
    ereq.selection = FrameSelection();
    check(!ctrl::planExport(generic, ereq, ctrl::QualityBucket::Balanced).ok(),
          QStringLiteral("an empty selection is an error, not a zero-frame export"));

    // exportWithFit must refuse a hand-off that does not match the plan instead
    // of encoding a file the plan never described.
    QList<QImage> wrong;
    for (int i = 0; i < 5; ++i) wrong.append(makeFrame(320, 240, i));
    const ctrl::FitResult fr =
        ctrl::exportWithFit(generic, req, ctrl::QualityBucket::Balanced, wrong);
    check(!fr.ok && fr.error.contains(QLatin1String("frame mismatch")),
          QStringLiteral("a frame-count mismatch is refused (%1)").arg(fr.error));
}

TEST_SECTION("availability probe: name lookup vs real one-frame encode", run_probe_sanity) {

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

TEST_SECTION("codec ladder (class x bucket)", run_ladder) {

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

TEST_SECTION("destination conformance (every row)", run_conformance) {

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
        req.selection = ctrl::FrameSelection::fromCount(3);
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
        for (int i = 0; i < plan.frames(); ++i) {
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
                makeAudio(plan.frames(), plan.fps, plan.config.audioSampleRate, plan.config.audioChannels);
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
TEST_SECTION("unknown encoder option is rejected", run_option_typo_is_fatal) {

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

TEST_SECTION("throughput gate (is the codec fast enough HERE?)", run_throughput_gate) {

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

TEST_SECTION("audio is derived, not requested", run_audio_is_derived) {

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
        req.selection = ctrl::FrameSelection::fromCount(3);
        req.exportableAudioTracks = aProjectHasAudio ? 2 : 0;
        req.mute = aMute;

        const ctrl::ExportPlan plan =
            ctrl::planExport(t, req, ctrl::QualityBucket::Balanced);
        if (!plan.ok()) return -1;

        QList<QImage> frames;
        for (int i = 0; i < plan.frames(); ++i) {
            frames.append(makeFrame(plan.renderSize.width(), plan.renderSize.height(), i));
        }

        media::VideoEncoder enc;
        if (!enc.open(plan.config)) return -1;
        for (const QImage& f : frames) enc.write(f);
        if (plan.config.audioChannels > 0) {
            const QVector<float> pcm = makeAudio(plan.frames(), plan.fps,
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

// The artifact list the escape hatch offers when the user has no destination in
// mind. Every claim it makes is checked against the probe, because the previous
// answer to "which formats can I export?" was a text file that listed AVI,
// Huffyuv, Ogg and Theora as though every build had them.
TEST_SECTION("artifact list is derived, and every entry is real", run_artifacts) {
    const QList<media::ffmpeg::ContainerOption> containers = media::ffmpeg::writableContainers();
    QTextStream out(stdout);
    out << "    containers this build can write: " << containers.size() << "\n";
    for (const media::ffmpeg::ContainerOption& c : containers) {
        out << "      " << c.name.leftJustified(7) << c.label.leftJustified(20) << " -> "
            << c.defaultCodec.leftJustified(14) << " alpha=" << (c.alphaCapable ? "yes" : "no")
            << "\n";
    }

    check(!containers.isEmpty(), "at least one container is offerable");
    bool hasMp4 = false;
    bool hasGif = false;
    for (const media::ffmpeg::ContainerOption& c : containers) {
        if (c.name == QLatin1String("mp4")) hasMp4 = true;
        if (c.name == QLatin1String("gif")) hasGif = true;
        check(!c.label.isEmpty(), QStringLiteral("%1 has a label").arg(c.name));
        if (c.name == QLatin1String("gif")) {
            // media::GifEncoder, not libav: present whenever the app is.
            check(c.defaultCodec == QLatin1String("gif"), "gif routes to the in-process GifEncoder");
        } else {
            check(media::ffmpeg::containerAcceptsUsableCodec(c.name, c.defaultCodec),
                  QStringLiteral("%1's default (%2) really runs and is really muxable here")
                      .arg(c.name, c.defaultCodec));
        }
    }
    check(hasMp4, "mp4 is offerable (the one container everything plays)");
    check(hasGif, "gif is offerable (the one format every chat accepts)");

    // The codec list narrows to what the container can carry: H.264 in WebM is
    // impossible, and offering it would produce a confusing mux failure.
    const QList<media::ffmpeg::CodecOption> mp4 = media::ffmpeg::usableCodecsForContainer(QStringLiteral("mp4"));
    out << "    mp4 codecs: " << mp4.size() << "\n";
    for (const media::ffmpeg::CodecOption& c : mp4) {
        out << "      " << c.encoder.leftJustified(14) << c.label.leftJustified(22)
            << " lossless=" << (c.lossless ? "yes" : "no")
            << " alpha=" << (c.alphaCapable ? "yes" : "no") << "\n";
    }
    check(!mp4.isEmpty(), "mp4 offers at least one codec");
    for (const media::ffmpeg::CodecOption& c : mp4) {
        check(!c.label.isEmpty(), QStringLiteral("%1 is labelled").arg(c.encoder));
        check(media::ffmpeg::containerAcceptsUsableCodec(QStringLiteral("mp4"), c.encoder),
              QStringLiteral("offered codec %1 is usable + muxable").arg(c.encoder));
    }
    // Every codec each container offers, so the escape hatch's reach is visible
    // in the test output rather than assumed: this list is what has to keep the
    // legacy menu's promise (ProRes for an editor, lossless FFV1 for a master,
    // Theora/AVI for anyone who still needs them) without a hand-written table.
    for (const media::ffmpeg::ContainerOption& c : containers) {
        if (c.name == QLatin1String("gif"))
            continue; // media::GifEncoder, not a libav codec choice
        QStringList names;
        for (const media::ffmpeg::CodecOption& codec : media::ffmpeg::usableCodecsForContainer(c.name))
            names << codec.encoder;
        out << "      " << c.name.leftJustified(7) << "offers: " << names.join(QLatin1String(", ")) << "\n";
    }

    bool webmOffersH264 = false;
    for (const media::ffmpeg::CodecOption& c : media::ffmpeg::usableCodecsForContainer(QStringLiteral("webm"))) {
        if (c.encoder == QLatin1String("libx264")) webmOffersH264 = true;
    }
    check(!webmOffersH264, "H.264 in WebM is not offered (the muxer cannot carry it)");

    // The capabilities the deprecated menu existed to reach must remain
    // reachable, where this build can produce them at all — otherwise deleting
    // it would be a silent loss.
    auto offers = [](const QString& aContainer, const QString& aEncoder) {
        for (const media::ffmpeg::CodecOption& c : media::ffmpeg::usableCodecsForContainer(aContainer)) {
            if (c.encoder == aEncoder && media::ffmpeg::containerAcceptsUsableCodec(aContainer, aEncoder))
                return true;
        }
        return false;
    };
    if (media::ffmpeg::encoderUsable("prores_ks")) {
        check(offers(QStringLiteral("mov"), QStringLiteral("prores_ks")),
              "ProRes 4444 in MOV stays reachable (the editor handoff, with alpha)");
    }
    if (media::ffmpeg::encoderUsable("ffv1")) {
        check(offers(QStringLiteral("mkv"), QStringLiteral("ffv1")),
              "FFV1 in MKV stays reachable (the lossless master)");
    }
    if (media::ffmpeg::encoderUsable("utvideo")) {
        check(offers(QStringLiteral("avi"), QStringLiteral("utvideo")),
              "Ut Video in AVI stays reachable (the old lossless/alpha pair)");
    }
    check(media::ffmpeg::usableCodecsForContainer(QStringLiteral("no_such_container")).isEmpty(),
          "an unknown container offers nothing rather than a fabricated default");
}

// The Quality selector has to reach the encoder for EVERY destination, not just
// the ones whose codec comes from the ladder. Ten of the fourteen rows fix the
// codec (Telegram, Discord, WhatsApp, LINE, OBS...), so for those the bucket used
// to change nothing at all: "Smaller file" and "Best quality" emitted identical
// instructions and produced near-identical files.
TEST_SECTION("quality intent reaches the encoder", run_bucket_quality) {
    const QList<ctrl::ExportTarget> targets =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const ctrl::ExportTarget telegram =
        ctrl::ExportTargets::find(targets, QStringLiteral("telegram.sticker"));
    check(telegram.codecLadder == false, "telegram.sticker fixes its codec (the case that was inert)");

    ctrl::ExportRequest req;
    req.sourceSize = QSize(512, 512);
    req.selection = ctrl::FrameSelection::fromCount(30);
    req.fps = 24;

    auto crfFor = [&](ctrl::QualityBucket aBucket) -> QString {
        const ctrl::ExportPlan plan = ctrl::planExport(telegram, req, aBucket);
        if (!plan.ok()) return QStringLiteral("<plan error: %1>").arg(plan.error);
        return plan.config.encOptions.value(QStringLiteral("crf"), QStringLiteral("<absent>"));
    };
    const QString smaller = crfFor(ctrl::QualityBucket::Smaller);
    const QString balanced = crfFor(ctrl::QualityBucket::Balanced);
    const QString best = crfFor(ctrl::QualityBucket::Best);
    QTextStream(stdout) << "    telegram.sticker crf: smaller=" << smaller
                        << " balanced=" << balanced << " best=" << best << "\n";

    check(smaller != QStringLiteral("<absent>"),
          "the fixed-codec destination gets a quality knob at all");
    check(smaller != best, "Smaller and Best are no longer the same instruction");
    check(balanced != best, "Balanced differs from Best");

    // Direction: VP9's knob is CRF, where a LARGER number means a smaller file.
    // Asserting the direction and not merely inequality is what stops this test
    // from passing if the mapping is ever reversed.
    check(smaller.toInt() > balanced.toInt() && balanced.toInt() > best.toInt(),
          QStringLiteral("CRF rises as the intent gets smaller-quality (%1 > %2 > %3)")
              .arg(smaller, balanced, best));

    // And inside the span the data declares, or the fit-to-cap loop would start
    // outside the range it is allowed to search.
    const QHash<QString, ctrl::QualityControl> qc =
        ctrl::ExportTargets::loadQualityControl(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const ctrl::QualityControl control = qc.value(QStringLiteral("libvpx-vp9"));
    check(control.option == QLatin1String("crf") && control.best == 24 && control.worst == 52,
          "the span for libvpx-vp9 comes from the table");
    for (const QString& v : {smaller, balanced, best}) {
        check(v.toInt() >= control.best && v.toInt() <= control.worst,
              QStringLiteral("crf %1 stays inside [%2, %3]").arg(v).arg(control.best).arg(control.worst));
    }

    // A codec with no quality knob must NOT be given a fabricated one: FFV1 is
    // lossless, and inventing a "crf" for it would be a lie that the encoder
    // then rejects as an unknown option.
    const ctrl::ExportTarget master =
        ctrl::ExportTargets::find(targets, QStringLiteral("archive.master"));
    const ctrl::ExportPlan losslessPlan =
        ctrl::planExport(master, req, ctrl::QualityBucket::Lossless);
    check(losslessPlan.ok(), QStringLiteral("lossless master plan builds (%1)").arg(losslessPlan.error));
    check(losslessPlan.config.encOptions.value(QStringLiteral("crf"), QString()).isEmpty(),
          "a lossless codec is left alone (no invented quality knob)");

    // Asking a lossy codec for "Lossless" cannot be honoured, and the plan says
    // so rather than quietly pretending.
    const ctrl::ExportPlan losslessOnVp9 =
        ctrl::planExport(telegram, req, ctrl::QualityBucket::Lossless);
    bool saidSo = false;
    for (const QString& n : losslessOnVp9.notes) {
        if (n.contains(QLatin1String("lossy"), Qt::CaseInsensitive)) saidSo = true;
    }
    check(saidSo, "Lossless on a lossy codec is stated, not silently approximated");

    // The option being SET is not the claim; the claim is that the file changes.
    // Encode real frames both ways through the same code path the app uses and
    // compare the output — the whole point of this session's fixes is that a
    // setting which does not reach the output is worse than no setting.
    {
        const ctrl::ExportTarget generic =
            ctrl::ExportTargets::find(targets, QStringLiteral("generic.file"));
        const QString smallPath = QDir::temp().filePath(QStringLiteral("ae_bucket_small.mp4"));
        const QString bestPath = QDir::temp().filePath(QStringLiteral("ae_bucket_best.mp4"));
        auto encodeAt = [&](ctrl::QualityBucket aBucket, const QString& aPath) -> qint64 {
            ctrl::ExportRequest r = req;
            r.outputPath = aPath;
            r.sourceSize = QSize(320, 240);
            r.selection = ctrl::FrameSelection::fromCount(8);
            ctrl::ExportPlan p = ctrl::planExport(generic, r, aBucket);
            if (!p.ok()) return -1;
            p.config.outputPath = aPath;
            QList<QImage> frames;
            for (int i = 0; i < p.frames(); ++i)
                frames.append(makeFrame(p.renderSize.width(), p.renderSize.height(), i));
            media::VideoEncoder enc;
            if (!enc.open(p.config) || !enc.writeAll(frames, p.config)) return -1;
            return QFileInfo(aPath).size();
        };
        const qint64 smallBytes = encodeAt(ctrl::QualityBucket::Smaller, smallPath);
        const qint64 bestBytes = encodeAt(ctrl::QualityBucket::Best, bestPath);
        QTextStream(stdout) << "    generic.file: smaller=" << smallBytes
                            << " bytes, best=" << bestBytes << " bytes\n";
        check(smallBytes > 0 && bestBytes > 0, "both buckets encode");
        check(bestBytes > smallBytes,
              QStringLiteral("Best really produces a larger file than Smaller (%1 > %2)")
                  .arg(bestBytes).arg(smallBytes));
        QFile::remove(smallPath);
        QFile::remove(bestPath);
    }

    // libwebp's knob runs the OTHER way (0-100, higher is better), so the same
    // mapping must produce the opposite ordering. One table, both directions.
    const ctrl::ExportTarget discordEmoji =
        ctrl::ExportTargets::find(targets, QStringLiteral("discord.emoji"));
    const ctrl::ExportPlan webpSmall =
        ctrl::planExport(discordEmoji, req, ctrl::QualityBucket::Smaller);
    const ctrl::ExportPlan webpBest =
        ctrl::planExport(discordEmoji, req, ctrl::QualityBucket::Best);
    const QString ws = webpSmall.config.encOptions.value(QStringLiteral("quality"));
    const QString wb = webpBest.config.encOptions.value(QStringLiteral("quality"));
    QTextStream(stdout) << "    discord.emoji webp quality: smaller=" << ws << " best=" << wb << "\n";
    check(!ws.isEmpty() && !wb.isEmpty(), "the WebP rows get their quality knob too");
    check(ws.toInt() < wb.toInt(),
          QStringLiteral("a higher-is-better knob moves the other way (%1 < %2)").arg(ws, wb));
}

TEST_SECTION("post-encode fit-to-cap", run_fit_to_cap) {

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
    req.selection = ctrl::FrameSelection::fromCount(frames.size());

    // A cap small enough that the FIRST attempt cannot possibly fit, so the
    // loop has to degrade. File size is the one limit that cannot be known
    // before encoding — that is the whole reason this loop exists.
    ctrl::ExportTarget tiny = base;
    tiny.caps.maxBytes = 2500;

    const ctrl::FitResult r =
        ctrl::exportWithFit(tiny, req, ctrl::QualityBucket::Balanced, frames);
    for (const QString& step : r.steps) QTextStream(stdout) << "      " << step << "\n";
    check(r.attempts > 1, QStringLiteral("the fit loop iterated (%1 attempts)").arg(r.attempts));

    // Cancellation, polled before each attempt. Without this the loop ran to
    // completion (up to 16 encodes) while the progress dialog's Cancel button did
    // nothing — a button that lies about what it does.
    {
        int polls = 0;
        const ctrl::FitResult c = ctrl::exportWithFit(
            tiny, req, ctrl::QualityBucket::Balanced, frames, nullptr,
            [&polls] { return ++polls > 2; }); // let two attempts run, then stop
        check(c.canceled, "a canceled fit reports cancellation");
        check(!c.ok, "a canceled fit is not reported as success");
        check(c.attempts <= 3, QStringLiteral("the loop stopped promptly (%1 attempts)").arg(c.attempts));
        check(c.error == QLatin1String("canceled"),
              QStringLiteral("the reason is 'canceled', not the did-not-fit message (got '%1')").arg(c.error));
    }
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

TEST_SECTION("live encode from a resolved plan", run_live_encode) {

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
        req.selection = ctrl::FrameSelection::fromCount(c.frames);

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
        for (int i = 0; i < plan.frames(); ++i) {
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

    // Every section that was DEFINED is in here. There is no list to keep in
    // sync, so a section cannot be written and silently not run — which is how
    // run_live_encode() went unexecuted while reading as coverage.
    for (const TestSection& s : testSections()) {
        QTextStream(stdout) << "\n== " << s.title << " ==\n";
        s.fn();
    }

    QTextStream(stdout) << "\n" << testSections().size() << " sections ran\n";
    QTextStream(stdout) << "\n" << (failures ? "FAILURES: " : "ALL PASS ") << failures << "\n";
    return failures ? 1 : 0;
}
