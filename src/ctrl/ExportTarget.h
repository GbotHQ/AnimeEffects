#ifndef ANIMEEFFECTS_SRC_CTRL_EXPORTTARGET_H
#define ANIMEEFFECTS_SRC_CTRL_EXPORTTARGET_H

#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QHash>

#include "media/VideoEncoder.h"

// Destination-driven export targets (DESIGN PROTOTYPE).
//
// The export UI is built from a data table (assets/export/ExportTargets.txt),
// not from a wall of codec combos. Each row is one destination: it resolves to
// a container, a codec (dictated, or picked by a ladder), and the hard limits
// and capabilities that destination imposes.
//
// The split that matters: the USER states a fact they possess ("it's for
// Telegram"); the SYSTEM applies the consequences (WebM/VP9+alpha, no audio,
// <=256KB, <=3s). Capabilities are therefore declared per row as a MODALITY,
// and only a `Choice` ever becomes a control the user can get wrong.
namespace ctrl {

enum class Capability {
    Required,    // forced on — stated in the UI, never a toggle
    Forbidden,   // impossible or rejected — stated, or hidden
    DefaultOn,   // pre-set checkbox, overridable
    DefaultOff,  // pre-set checkbox, overridable
    Choice       // a genuine user decision
};

// What happens to our file after we hand it over. This is the axis that decides
// whether to optimize for efficiency or for decode compatibility.
enum class DestClass {
    Fixed,     // destination dictates the codec (most rows)
    Reencode,  // platform re-encodes our upload (YouTube, X, TikTok)
    Direct,    // platform serves our file as-is (website, messaging)
    Archive    // keep a master
};

enum class SizeMode {
    Free,          // use the project size
    Exact,         // exactly w x h (needs crop-or-pad — open design item)
    Max,           // fit inside w x h, preserve aspect
    MaxExactSide,  // long side == side, short side <= side
    MaxMinSide     // fit inside w x h, then ensure long side >= side
};

// The user's quality intent, in their words. Never a bitrate number: bitrate is
// codec-specific and meaningless to this audience.
enum class QualityBucket { Smaller, Balanced, Best, Lossless };

enum class FitMode { None, Pad, Crop };

struct SizeRule {
    SizeMode mode = SizeMode::Free;
    int w = 0;
    int h = 0;
    int side = 0; // MaxExactSide: the exact long side. MaxMinSide: the minimum long side.

    // How to reconcile a differing aspect ratio for an exact size. Platforms
    // disagree about margins, so this is declared per destination: Signal asks
    // for a 16px margin, WhatsApp for an 8px stroke, LINE forbids margins
    // entirely. Pad is the safe default (never loses content); Crop fills the
    // frame at the risk of cutting off the subject.
    FitMode fit = FitMode::Pad;
};

// Audio parameters for a destination. Everything is optional: an empty codec
// means "derive from the container" (WebM cannot carry AAC), and zeros mean
// "use the defaults". These are per-destination because they genuinely differ:
// a source upload wants generous bitrate, a website cares about bandwidth, and
// an archive wants lossless.
struct AudioSpec {
    QString codec;     // empty = derive from the container
    int bitrate = 0;   // bits/sec; 0 = default
    int channels = 0;  // 0 = default
    int sampleRate = 0;
};

struct Caps {
    qint64 maxBytes = 0;      // hard upload rejection above this
    double maxDurationSec = 0;
    int maxFps = 0;
    int minFrames = 0;
    int maxFrames = 0;
};

struct ExportTarget {
    QString id;
    QString label;
    QString family;
    DestClass klass = DestClass::Fixed;
    bool codecLadder = false; // true => codec comes from the ladder, not @codec

    QString container;
    QString codec;
    QString pixFmt;

    Capability alpha = Capability::Forbidden;
    Capability audio = Capability::Choice;
    Capability loop = Capability::Choice;

    SizeRule size;
    Caps caps;

    // Two DIFFERENT mechanisms, deliberately not merged:
    //  - encOptions: private encoder options handed to avcodec_open2 (libvpx's
    //    auto-alt-ref=0, which VP9+alpha requires or the alpha flickers).
    //  - codecTag:   four-char container stream tag (hvc1 for HEVC, without
    //    which Safari/iOS refuse the file). It is not an encoder option.
    QMap<QString, QString> encOptions;
    QString codecTag;
    AudioSpec audioSpec; // parameters (the `audio` member is the capability)

    // Floor for fit-to-cap's scale phase, in percent. Below this the image is
    // visibly degraded, so the loop stops and reports rather than shrinking
    // further. Data, not a code constant.
    int minScalePct = 25;

    // Order in which fit-to-cap sacrifices quality, taken from the data.
    // Default rationale: quality first (platforms' own guidance, and it changes
    // neither composition nor timing), then scale (loses detail, keeps timing),
    // then frames (changes the animation itself — most destructive).
    QStringList degradeOrder;

    QString note; // shown to the user as the "why"

    // Provenance. These facts move; the date is how we know when to re-check.
    QString checked;
    QString source;
    bool verified = false;

    // Some destinations can only be produced on one OS (HEVC+alpha needs
    // VideoToolbox). Declared rather than discovered, so a row can be skipped
    // without pretending it is broken.
    QString platform; // empty/"any" = everywhere
};

struct ExportRequest {
    QString outputPath;
    QSize sourceSize;
    int fps = 24;
    int frames = 1;

    // Whether audio is exported is NOT a preference — it is derived:
    //   included  iff  the project HAS audio  AND  not muted  AND  the
    //   destination permits it.
    // There is deliberately no "wantAudio": the project either has sound or it
    // does not, and a destination either accepts it or it does not. Letting a
    // caller *demand* audio invented a conflict that then needed enforcement
    // machinery to resolve. The only real user intent is the restriction below.
    // How many of the project's audio tracks will actually be mixed: tracks
    // flagged exportEnable=false are timing/reference only (a voice pass, a temp
    // music bed) and are heard while animating but must not reach the file.
    // A COUNT, not a bool — the plan can then say how much it is mixing.
    int exportableAudioTracks = 0;
    bool mute = false; // INTENT: leave all sound out of this export
    int audioChannels = 2;
    int audioSampleRate = 48000;
    // NOTE: the audio CODEC is deliberately NOT here — it is derived from the
    // container, because a caller-supplied one can be impossible to mux
    // (AAC into WebM fails at avformat_write_header). Same rule as everything
    // else: don't let callers ask for things that cannot work.
};

struct ExportPlan {
    ExportTarget target;
    media::VideoEncoder::Config config;
    QSize size;        // the OUTPUT size (what the file must be)
    QSize renderSize;  // what to RENDER at — differs from `size` for exact-size
                       // destinations, which fit the art and pad the remainder
    int fps = 0;
    int frames = 0;
    bool alphaEnabled = false;
    bool audioEnabled = false;

    // GIF is not a VideoEncoder target — media::GifEncoder owns it (adaptive or
    // custom palette). Flagged here so a caller doesn't feed a GIF row into the
    // video path.
    bool gifPath = false;

    // Human-readable "why" + any warnings. The UI shows these; they are how a
    // non-expert learns what their destination requires.
    QStringList notes;

    QString error;
    bool ok() const { return error.isEmpty(); }
};

// The knob that trades quality for bytes, per codec. Loaded from the data file
// (the values move with encoder versions and content type), not hardcoded.
struct QualityControl {
    QString codec;
    QString option; // private encoder option name, e.g. "crf"
    int best = 0;   // start here: highest quality
    int worst = 0;  // lowest quality we accept before shrinking instead
};

struct FitResult {
    bool ok = false;
    QString path;
    qint64 bytes = 0; // size of the ACCEPTED file (or, on failure, the smallest seen)
    int attempts = 0;
    QSize finalSize;
    QStringList steps; // what the loop actually did, for the UI to show
    QString error;
};

class ExportTargets {
public:
    // Resource path the app loads. Overridable so headless tests can read the
    // source file directly (same pattern as the preset matrix test).
    static QString defaultPath();

    static QList<ExportTarget> load(const QString& aPath = defaultPath(), QString* aError = nullptr);

    static ExportTarget find(const QList<ExportTarget>& aTargets, const QString& aId);

    // Per-codec quality knobs (the <quality_control> block), keyed by codec.
    static QHash<QString, QualityControl> loadQualityControl(
        const QString& aPath = defaultPath());

    // Per-quality-intent time budgets (seconds), from the data file. Policy,
    // not fact — therefore data, so it can be tuned without a rebuild.
    static QHash<QString, double> loadTimeBudgets(const QString& aPath = defaultPath());
};

// Apply a destination's size rule to a source size. YUV planar formats need
// even dimensions, so the result is nudged to even.
QSize applySizeRule(const SizeRule& aRule, const QSize& aSource, QStringList* aNotes = nullptr);

// Clamp fps/frame-count to the destination's caps (the pre-encode, deterministic
// half of constraint enforcement).
void applyCaps(const Caps& aCaps, int* aFps, int* aFrames, QStringList* aNotes = nullptr);

// What the ladder needs to know to judge whether a codec is fast enough here.
struct LadderHints {
    int width = 0;
    int height = 0;
    QString pixFmt;
    int frameCount = 0;
    double maxSeconds = 0.0; // 0 = no time limit
};

// Pick a codec for a destination whose codec is NOT dictated. Walks the
// (class, bucket) ladder and returns the first candidate that (a) actually runs
// here and (b), when a budget and size are supplied, is projected to finish
// inside it. `aWhy` receives the one-line reason the UI shows.
//
// Both gates are measured, not read off a list: encoderUsable() (a one-frame
// test encode) and encoderThroughput() (timed frames at the real size).
QString resolveLadderCodec(DestClass aClass, QualityBucket aBucket,
                           const LadderHints& aHints = LadderHints(),
                           QString* aWhy = nullptr);

// The whole point: destination + request + quality intent -> a ready-to-open
// encoder config, plus constraint application and human-readable reasoning.
ExportPlan planExport(const ExportTarget& aTarget, const ExportRequest& aRequest,
                      QualityBucket aBucket);

// Post-encode half of constraint enforcement: encode, MEASURE, and if the file
// is over the destination's cap, degrade and re-encode until it fits.
//
// File size is the one limit that cannot be known before encoding, so measuring
// is the only honest way to satisfy it. Degradation order is least-destructive
// first: quality -> scale -> frame count. These caps are hard upload
// rejections, so this is the difference between a file the platform accepts and
// a 400.
FitResult exportWithFit(const ExportTarget& aTarget, const ExportRequest& aRequest,
                        QualityBucket aBucket, const QList<QImage>& aFrames);

} // namespace ctrl

#endif // ANIMEEFFECTS_SRC_CTRL_EXPORTTARGET_H
