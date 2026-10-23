#include "gui/FrameRulerCanvas.h"

#include <QFont>
#include <QFontMetrics>
#include <QCanvasPainter>
#include <QRectF>

namespace gui {
namespace frameRuler {

namespace {

// Canvas2D strokes are centred on the coordinates they are given, so a 1px mark on
// column x needs x + 0.5 to cover that column and nothing else. Every mark in the ruler
// is asked for an INTEGER column by the model, which is what makes this exact.
constexpr float kHalf = 0.5f;

} // namespace

void paintRulerCanvas(QCanvasPainter& aPainter, const Model& aModel, const PaintInk& aInk) {
    if (aModel.band.isEmpty())
        return;

    aPainter.save();
    aPainter.setAntialias(0.0f); // the ruler's chrome is integer-column ink, never feathered

    // TICKS: one length for every tier, prominence by alpha, and the shared 0.4 recession
    // outside the active playback range.
    // The raster renderer draws its tick from band.bottom() UP to bottom-kTickLength, and
    // an inclusive drawLine inks kTickLength+1 ROWS. In Canvas2D's edge coordinates a
    // 1px-wide stroke on column x is centred at x+0.5, and the segment must reach from the
    // last row's BOTTOM edge (bottom+1) to one row past the top one drawn.
    const float base = float(aModel.band.bottom()) + 1.0f;
    const float tickEnd = base - float(kTickLength + 1);
    for (const Tick& tick : aModel.ticks) {
        QColor colour = aInk.content;
        colour.setAlphaF(tickAlpha(tick.level) * (tick.inActiveRange ? 1.0 : kOutsideRangeFactor));
        aPainter.setStrokeStyle(colour);
        aPainter.setLineWidth(1.0f);
        aPainter.beginPath();
        aPainter.moveTo(float(tick.x) + kHalf, base);
        aPainter.lineTo(float(tick.x) + kHalf, tickEnd);
        aPainter.stroke();
    }

    // NUMBERS: anchored LEFT on the box the model published, top-aligned on the number
    // row - the convention both hosts state, and the one the graph panel's `LabelVAlign::
    // Top` exists for (the two rasterizers' glyph bearings differ, so the run is placed by
    // its ink row rather than centred).
    if (!aModel.labels.isEmpty()) {
        aPainter.setFont(QFont());
        aPainter.setTextAlign(QCanvasPainter::TextAlign::Left);
        aPainter.setTextBaseline(QCanvasPainter::TextBaseline::Top);
    }
    const QFontMetrics metrics(QFont());
    for (const Label& label : aModel.labels) {
        QColor colour = aInk.content;
        if (!label.inActiveRange)
            colour.setAlphaF(kOutsideRangeFactor);
        aPainter.setFillStyle(colour);
        // The model's row is the box's top; Canvas2D's Top baseline puts the line box's
        // ascent there, which is where Qt's own drawText(rect) ink starts.
        aPainter.fillText(label.text, float(label.left), float(label.top));
    }

    // THE TEMPORARY PLAYBACK WINDOW: the wash, then the stems, then the pills - the
    // timeline's own order, so a merged pill wider than the window cannot hide the
    // stems' lower half.
    const PlayWindowOverlay& window = aModel.window;
    if (window.active) {
        if (!window.band.isEmpty()) {
            aPainter.setFillStyle(aInk.windowBand);
            aPainter.fillRect(QRectF(window.band));
        }
        aPainter.setFillStyle(aInk.windowEdge);
        if (!window.startStem.isEmpty())
            aPainter.fillRect(QRectF(window.startStem));
        if (!window.endStem.isEmpty())
            aPainter.fillRect(QRectF(window.endStem));

        const auto pill = [&aPainter, &aModel, &aInk](const Pill& aPill, bool aSquareLeft, bool aSquareRight) {
            if (!aPill.isValid())
                return;
            aPainter.setFillStyle(aInk.windowEdge);
            aPainter.roundRect(QRectF(aPill.rect), float(kPillRadius));
            aPainter.fill();
            // The faces against the stems stay square, so the bracket joint reads
            // continuous (the QPainter renderer overdraws the same corners).
            const int r = kPillRadius;
            if (aSquareLeft) {
                aPainter.fillRect(
                    QRectF(aPill.rect.x(), aPill.rect.y(), r, r)
                );
                aPainter.fillRect(QRectF(aPill.rect.x(), aPill.rect.bottom() - r + 1, r, r));
            }
            if (aSquareRight) {
                aPainter.fillRect(QRectF(aPill.rect.right() - r + 1, aPill.rect.y(), r, r));
                aPainter.fillRect(QRectF(aPill.rect.right() - r + 1, aPill.rect.bottom() - r + 1, r, r));
            }
            aPainter.setFillStyle(aInk.windowText);
            aPainter.setTextAlign(QCanvasPainter::TextAlign::Left);
            aPainter.setTextBaseline(QCanvasPainter::TextBaseline::Top);
            aPainter.fillText(
                aPill.text, float(aPill.rect.left() + kPillPadding), float(aModel.band.top() + kNumberTop)
            );
        };
        if (window.merged.isValid())
            pill(window.merged, false, false);
        else {
            pill(window.start, true, false);
            pill(window.end, false, true);
        }
    }

    aPainter.restore();
}

} // namespace frameRuler
} // namespace gui
