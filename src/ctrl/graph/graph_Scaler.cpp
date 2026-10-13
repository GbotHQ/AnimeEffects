#include "ctrl/graph/graph_Scaler.h"

#include <algorithm>
#include <cmath>

namespace {
// A window whose span falls below this fraction of the value it shows has no
// resolution left: at float precision the pixel mapping would be rounding
// noise, and the inverse would amplify it into a curve that flickers.
constexpr double kMinRelativeSpan = 1e-6;

// One wheel notch in the units ctrl::time::Scaler accumulates: its private
// kWheelValue (120), where the zoom rung is mWheel / 120. fitFrameAxis moves
// the rung by handing update() a whole number of notches, and it has no other
// way to address the rung - update() is the only public writer of it.
constexpr int kWheelNotch = 120;
} // namespace

namespace ctrl {
namespace graph {

    //-------------------------------------------------------------------------------------------------
    // The value window is seeded non-degenerate (0..1): every mapping in the
    // class assumes a positive span, and the setters below are what keep that
    // true.
    Scaler::Scaler(): mFrameScale(), mRect(), mFrameOffset(0.0), mValueMin(0.0f), mValueMax(1.0f) {}

    //-------------------------------------------------------------------------------------------------
    void Scaler::setRect(const QRect& aRect) { mRect = aRect; }

    QRect Scaler::plotRect() const {
        // The label column is a left-only inset: it is where the value numbers
        // sit, so the curves start to its right. The frame offset is NOT baked
        // in here - it lives on the frame mapping, so this stays a pure
        // function of the panel geometry.
        return mRect.adjusted(kMargin + kAxisLabelWidth, kMargin, -kMargin, -kMargin);
    }

    //-------------------------------------------------------------------------------------------------
    // The frame axis is linear in pixels-per-frame, and the ladder of that
    // rate belongs to ctrl::time::Scaler. What this class adds is the origin:
    // mFrameOffset is a free content pixel, kept in doubles precisely so a
    // wheel zoom can be re-anchored without the half-pixel drift an integer
    // origin would accumulate over a wheel's worth of notches.
    double Scaler::pixelXOfFrame(double aFrame) const {
        // pixelsPerFrame() is the same (mIndex + 1) factor time::Scaler's
        // pixelWidth() multiplies by, but applied in doubles here so a
        // sub-frame position is not truncated toward zero first.
        return plotRect().left() + mFrameOffset + mFrameScale.pixelsPerFrame() * aFrame;
    }

    int Scaler::pixelX(core::Frame aFrame) const {
        // getDecimal(), not get(): the double form carries the fraction (a key
        // may sit on a sub-frame position). Only the pixel boundary rounds.
        return static_cast<int>(std::lround(pixelXOfFrame(aFrame.getDecimal())));
    }

    double Scaler::frameAtPixel(double aPixelX) const {
        // The exact inverse of pixelXOfFrame, in doubles end to end.
        // pixelsPerFrame() is at least 1 (time::Scaler's index floor is 0), so
        // this never divides by zero.
        return (aPixelX - plotRect().left() - mFrameOffset) / mFrameScale.pixelsPerFrame();
    }

    core::Frame Scaler::frameAt(int aPixelX) const {
        return core::Frame(static_cast<int>(std::lround(frameAtPixel(static_cast<double>(aPixelX)))));
    }

    //-------------------------------------------------------------------------------------------------
    bool Scaler::updateFrameZoom(int aWheelDelta, int aAnchorPixelX) {
        // The anchor frame is captured sub-pixel (frameAtPixel, never frameAt):
        // re-anchoring through a rounded frame would move the content by up to
        // half a frame, which at the ladder's ceiling is 8 px of drift for a
        // single notch. ctrl::TimeLineEditor::anchorPixelAfterScale is the same
        // rule - scale the content coordinate exactly, round only the final
        // pixel - and this is its graph-side equivalent with the rounding
        // deferred entirely into the returned int.
        const double anchorX = static_cast<double>(aAnchorPixelX);
        const double anchorFrame = frameAtPixel(anchorX);
        if (!mFrameScale.update(aWheelDelta))
            return false; // clamped at the ladder's floor or ceiling: nothing moved to re-anchor
        mFrameOffset = anchorX - plotRect().left() - mFrameScale.pixelsPerFrame() * anchorFrame;
        return true;
    }

    void Scaler::panFrame(int aPixelDeltaX) { mFrameOffset += static_cast<double>(aPixelDeltaX); }

    //-------------------------------------------------------------------------------------------------
    void Scaler::fitFrameAxis(core::Frame aLo, core::Frame aHi) {
        const QRect plot = plotRect();
        if (!(plot.width() > 0))
            return; // no plot to fit into (an unlaid-out scale)

        double lo = std::min(static_cast<double>(aLo.getDecimal()), static_cast<double>(aHi.getDecimal()));
        double hi = std::max(static_cast<double>(aLo.getDecimal()), static_cast<double>(aHi.getDecimal()));
        // A small pad so an extreme key is drawn inside the plot instead of on
        // its border; at least one frame so a single-frame span still has a
        // positive width to divide by.
        const double pad = std::max(1.0, (hi - lo) * 0.05);
        lo -= pad;
        hi += pad;
        const double span = hi - lo;

        // Pick the largest zoom rung that still fits the span. pixelsPerFrame()
        // is (mIndex + 1), so the rung that makes the span exactly fill the plot
        // is floor(width / span) - 1 frames-per-pixel-wise; update() clamps the
        // result to its own floor and ceiling, so an over-wide or over-narrow
        // span lands on the nearest rung the axis can actually reach.
        const int currentIndex = mFrameScale.pixelsPerFrame() - 1;
        const int wantedIndex = static_cast<int>(std::floor(static_cast<double>(plot.width()) / span)) - 1;
        mFrameScale.update((currentIndex - wantedIndex) * kWheelNotch);

        // Centre the padded span in the plot. mFrameOffset is the content pixel
        // of frame 0 relative to plotRect().left(), which is exactly where
        // pixelXOfFrame() reads it back from.
        const double ppf = static_cast<double>(mFrameScale.pixelsPerFrame());
        mFrameOffset = 0.5 * (static_cast<double>(plot.width()) - span * ppf) - ppf * lo;
    }

    //-------------------------------------------------------------------------------------------------
    // The value axis is linear and inverted: the top of the plot is valueMax,
    // its bottom end is valueMin. Only the two bounds are stored, so the
    // mapping is a pure function of them and a zoom can hold the value under
    // the cursor exactly.
    void Scaler::setValueWindow(float aMin, float aMax) {
        if (!std::isfinite(aMin) || !std::isfinite(aMax))
            return; // a NaN or Inf bound is not a window: keep showing the previous one
        const float lo = std::min(aMin, aMax);
        const float hi = std::max(aMin, aMax);
        // A zero-height window has no inverse (and no pixel mapping that means
        // anything), so a degenerate request keeps the previous window rather
        // than blanking the axis. fitValueWindow is what widens a real
        // constant channel into a usable one.
        if (!(hi > lo))
            return;
        mValueMin = lo;
        mValueMax = hi;
    }

    void Scaler::fitValueWindow(float aMin, float aMax) {
        if (!std::isfinite(aMin) || !std::isfinite(aMax))
            return; // fitting a NaN has no meaning; keep the window we have

        double lo = std::min(aMin, aMax);
        double hi = std::max(aMin, aMax);
        const double span = hi - lo;
        // The window is stored in float, so a span that does not survive the
        // round trip is a constant channel as far as the axis can tell - and a
        // degenerate window would leave pixelY with nothing to divide by.
        if (!(static_cast<float>(span) > 0.f)) {
            // Constant channel: centre it with room to breathe, or the curve
            // would be glued to the edge of a zero-height window and its keys
            // would be unreachable. The padding scales with the value so 1000
            // and 0.001 both stay readable, and never shrinks below 1.
            const double value = 0.5 * (lo + hi);
            const double pad = std::max(1.0, std::fabs(value) * 0.1);
            lo = value - pad;
            hi = value + pad;
        } else {
            // 5% of the span on each side, so an extreme key is drawn inside
            // the plot instead of on its border.
            const double pad = span * 0.05;
            lo -= pad;
            hi += pad;
        }
        setValueWindow(static_cast<float>(lo), static_cast<float>(hi));
    }

    //-------------------------------------------------------------------------------------------------
    int Scaler::pixelY(float aValue) const {
        const double height = plotHeight();
        const double span = static_cast<double>(mValueMax) - mValueMin;
        if (!(height > 0.0) || !(span > 0.0))
            return static_cast<int>(std::lround(plotTop())); // no vertical span to map through
        const double value = static_cast<double>(aValue);
        return static_cast<int>(std::lround(plotTop() + (mValueMax - value) / span * height));
    }

    double Scaler::valueAtPixel(double aPixelY) const {
        const double height = plotHeight();
        const double span = static_cast<double>(mValueMax) - mValueMin;
        if (!(height > 0.0) || !(span > 0.0))
            return 0.5 * (static_cast<double>(mValueMin) + mValueMax); // degenerate plot: no value to read
        return mValueMax - (aPixelY - plotTop()) / height * span;
    }

    float Scaler::valueAt(int aPixelY) const { return static_cast<float>(valueAtPixel(aPixelY)); }

    //-------------------------------------------------------------------------------------------------
    bool Scaler::updateValueZoom(float aFactor, int aAnchorPixelY) {
        if (!std::isfinite(aFactor) || !(aFactor > 0.f))
            return false; // a zero, negative or NaN factor is not a zoom
        const double span = static_cast<double>(mValueMax) - mValueMin;
        if (!(span > 0.0))
            return false; // unreachable while the setters hold, but the mapping depends on it

        // Keep the value under the cursor fixed: split the span about that
        // value, so the anchor is preserved exactly and nothing else in the
        // window moves relative to it.
        const double anchorValue = valueAtPixel(aAnchorPixelY);
        const double newSpan = span / aFactor;
        if (!(newSpan > kMinRelativeSpan * std::max(1.0, std::fabs(anchorValue))))
            return false; // the axis would have no resolution left around the anchor
        const double lo = anchorValue - (anchorValue - mValueMin) / aFactor;
        const double hi = anchorValue + (mValueMax - anchorValue) / aFactor;
        if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
            return false; // the window overflowed its storage: leave the axis alone
        mValueMin = static_cast<float>(lo);
        mValueMax = static_cast<float>(hi);
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    void Scaler::panValue(int aPixelDeltaY) {
        const double height = plotHeight();
        const double span = static_cast<double>(mValueMax) - mValueMin;
        if (!(height > 0.0) || !(span > 0.0))
            return; // nothing to pan
        // Positive delta drags the content down the panel, which reveals
        // higher values, so the window moves up the value axis by however much
        // those pixels are worth. setValueWindow keeps the result ordered and
        // finite (a pan past the float range is refused there rather than
        // producing an infinity).
        const double delta = aPixelDeltaY / height * span;
        setValueWindow(static_cast<float>(mValueMin + delta), static_cast<float>(mValueMax + delta));
    }

    //-------------------------------------------------------------------------------------------------
    // The plot's vertical pixel geometry, in doubles. Every value mapping goes
    // through these two so the int rect is converted in one place.
    double Scaler::plotTop() const { return plotRect().top(); }

    double Scaler::plotHeight() const { return plotRect().height(); }

} // namespace graph
} // namespace ctrl
