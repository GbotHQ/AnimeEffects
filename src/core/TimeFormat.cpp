#include "TimeFormat.h"

namespace {

// Floor division for positive divisors: frame-to-second decomposition must
// round toward negative infinity, not toward zero (C++ division truncates),
// so negative frames count down through -1, -2, ... instead of colliding at
// "0" with the frames just before 0.
int floorDiv(int aValue, int aDivisor) {
    return aValue >= 0 ? aValue / aDivisor : (aValue - aDivisor + 1) / aDivisor;
}

} // namespace

namespace core {

TimeFormat::TimeFormat(util::Range aRange, int aFps) {
    mRange = aRange;
    mFps = aFps;
}

QString TimeFormat::frameToString(const int aFrame, const TimeFormatType aTimeFormatType) const {
    switch (aTimeFormatType) {
    case TimeFormatType_Frames_From1:
        return QString::number(aFrame + 1);

    case TimeFormatType_Relative_FPS: {
        // The whole second this frame belongs to, floored so negative frames
        // count down monotonically (-1..-23 -> -1, -24..-47 -> -2).
        return QString::number(floorDiv(aFrame, mFps));
    }

    case TimeFormatType_Seconds_Frames: { // (SS:FF)
        // Seconds are a floor decomposition (SS carries into negatives, FF
        // stays in [0, 59]): -1 frame reads "-1:59", i.e. one frame before 0.
        const int seconds = floorDiv(aFrame, 60);
        const int frames = static_cast<int>(util::MathUtil::cycle(aFrame, 0.0, 60.0));

        return QString("%1:%2").arg(seconds).arg(frames, 2, 10, QLatin1Char('0'));
    }

    case TimeFormatType_Timecode_SMPTE: { // (HH:MM:SS:FF)
        // Negative frames display with a leading '-' and the magnitude
        // decomposed (HH:MM:SS from |frame|, FF = |frame| mod 60), the
        // conventional negative timecode reading: -1 frame -> "-0:0:0:1".
        const qint64 absFrame = qAbs(static_cast<qint64>(aFrame));
        const double ms = util::MathUtil::remap(absFrame, 0.0, 60.0, 0.0, 1000.0);

        const DDHHMMSSmmm timeStruct = msToDDHHMMSSmmm(ms);

        const QString text = QString("%1:%2:%3:%4")
                                 .arg(timeStruct.hours)
                                 .arg(timeStruct.minutes)
                                 .arg(timeStruct.seconds)
                                 .arg(static_cast<int>(util::MathUtil::cycle(absFrame, 0.0, 60.0)));

        return aFrame < 0 ? QString("-") + text : text;
    }

    case TimeFormatType_Timecode_HHMMSSmmm: { // (HH:MM:SS:mmm)
        // Same sign-prefix convention as SMPTE: the magnitude decomposes,
        // a leading '-' marks negative frames.
        const qint64 absFrame = qAbs(static_cast<qint64>(aFrame));
        const double ms = util::MathUtil::remap(absFrame, 0.0, 60.0, 0.0, 1000.0);

        const DDHHMMSSmmm timeStruct = msToDDHHMMSSmmm(ms);

        const QString text = QString("%1:%2:%3:%4")
                                 .arg(timeStruct.hours)
                                 .arg(timeStruct.minutes)
                                 .arg(timeStruct.seconds)
                                 .arg(timeStruct.milliseconds);

        return aFrame < 0 ? QString("-") + text : text;
    }

    default:
        return QString::number(aFrame);
    }
}


DDHHMMSSmmm TimeFormat::msToDDHHMMSSmmm(double ms) {
    int d = 0, h = 0, mi = 0, s = 0;
    // Days to milli
    if (ms >= 86400 * 1000) {
        d = qFloor(ms / 1000 / 60 / 60 / 24);
        ms = ms - d * (86400 * 1000);
    }
    // Hour to milli
    if (ms >= 3600 * 1000) {
        h = qFloor(ms / 1000 / 60 / 60);
        ms = ms - h * (3600 * 1000);
    }
    // Minute to milli
    if (ms >= 60 * 1000) {
        mi = qFloor(ms / 1000 / 60);
        ms = ms - mi * (60 * 1000);
    }
    // Second to milli
    if (ms >= 1000) {
        s = qFloor(ms / 1000);
        ms = ms - s * 1000;
    }
    DDHHMMSSmmm timeStruct;
    timeStruct.days = d;
    timeStruct.hours = h;
    timeStruct.minutes = mi;
    timeStruct.seconds = s;
    timeStruct.milliseconds = static_cast<int>(ms);

    return timeStruct;
}

} // namespace core
