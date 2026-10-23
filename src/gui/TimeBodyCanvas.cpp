#include "gui/TimeBodyCanvas.h"

#include <QCanvasPainter>
#include <QFont>
#include <QRectF>

namespace gui {
namespace timeBodyCanvas {

namespace {

constexpr float kHalf = 0.5f;

// Qt's DotLine at width 1 inks one pixel every three along the line. Canvas2D has no dashed
// strokes, so the same pitch is drawn as marks - the closest honest equivalent.
constexpr int kDotPitch = 3;

void strokePath(QCanvasPainter& aPainter, const QPolygonF& aPolygon) {
    if (aPolygon.isEmpty())
        return;
    aPainter.beginPath();
    aPainter.moveTo(aPolygon.first());
    for (int i = 1; i < aPolygon.size(); ++i)
        aPainter.lineTo(aPolygon.at(i));
    aPainter.closePath();
}

} // namespace

void paintLinesCanvas(QCanvasPainter& aPainter, const ctrl::time::Lines& aLines) {
    aPainter.setAntialias(0.0f);
    for (const ctrl::time::Lane& lane : aLines.lanes) {
        aPainter.setFillStyle(lane.selected ? aLines.inks.bodySelected : aLines.inks.body);
        aPainter.fillRect(QRectF(lane.rect));
        if (!lane.separators.isEmpty()) {
            aPainter.setFillStyle(aLines.inks.separator);
            for (const ctrl::time::Separator& each : lane.separators) {
                const int y = int(each.y);
                for (int x = each.x0; x <= each.x1; x += kDotPitch)
                    aPainter.fillRect(QRectF(x, y, 1, 1));
            }
        }
        // NOT inside the separator branch: a lane with ONE valid type has no separator, and
        // MEASURED 2026-10-22 that is exactly the lane whose label vanished - the timeline's
        // first lane lost its "Move", on every project whose lanes hold a single type each.
        // The gate could not see it: a pixel inside a label's box counts as "expected" (two
        // text engines cannot agree pixel for pixel), so a MISSING label was expected too.
        if (!lane.labels.isEmpty()) {
            aPainter.setFont(QFont());
            aPainter.setTextAlign(QCanvasPainter::TextAlign::Center);
            aPainter.setTextBaseline(QCanvasPainter::TextBaseline::Middle);
            for (const ctrl::time::Label& label : lane.labels) {
                aPainter.setFillStyle(label.ink);
                aPainter.fillText(
                    label.text, float(label.box.center().x()) + kHalf, float(label.box.center().y()) + kHalf
                );
            }
        }
    }

    aPainter.setAntialias(0.0f);
    // The seams: 1px rows. A FILLED rect is exact where a stroke is not: a 1px stroke at
    // y+0.5 with the antialias fringe inks the row above and below as well (measured: three
    // rows for one line).
    aPainter.setFillStyle(aLines.inks.edge);
    for (const ctrl::time::Seam& seam : aLines.seams)
        // QPainter's drawLine(x0, y, x1, y) with AA off inks the columns whose CENTRES lie on
        // the segment: x0 .. x1-1, i.e. a width of (x1 - x0).
        aPainter.fillRect(QRectF(seam.x0, seam.y, seam.x1 - seam.x0, 1));
}

void paintKeysCanvas(QCanvasPainter& aPainter, const ctrl::time::KeyPass& aPass) {
    aPainter.setAntialias(1.0f);
    for (const ctrl::time::KeyMark& mark : aPass.marks) {
        aPainter.setFillStyle(mark.fill);
        aPainter.setStrokeStyle(aPass.edge);
        aPainter.setLineWidth(1.0f);
        // A path is NOT reset by fill()/stroke(): without beginPath() every marker's shape
        // would be appended to the previous one's and re-inked with the next style.
        aPainter.beginPath();
        if (mark.shape == ctrl::time::KeyShape::Ellipse) {
            aPainter.circle(float(mark.centre.x()), float(mark.centre.y()), float(mark.radiusX));
            aPainter.fill();
            aPainter.stroke();
        } else {
            strokePath(aPainter, mark.polygon);
            aPainter.fill();
            aPainter.stroke();
        }
        if (mark.hasRing) {
            aPainter.setStrokeStyle(mark.ring);
            aPainter.setLineWidth(1.0f);
            aPainter.beginPath();
            aPainter.circle(float(mark.centre.x()), float(mark.centre.y()), float(mark.ringRadius));
            aPainter.stroke();
        }
    }
}

void paintGuidesCanvas(
    QCanvasPainter& aPainter, const QVector<ctrl::time::RangeMarker>& aMarkers,
    const ctrl::time::SelectionBox& aMarquee
) {
    aPainter.setAntialias(0.0f);
    for (const ctrl::time::RangeMarker& marker : aMarkers) {
        // A filled 1px column, for the same reason as the seams.
        aPainter.setFillStyle(marker.ink);
        // A 1px column needs its span to fall INSIDE one device column: [x+0.5, x+1.5) snaps to
        // column x+1, so a span of [x, x+1) is used with the fringe off.
        // MEASURED LIMIT: Canvas2D cannot fill a single DEVICE column. A span of [x, x+1) inks
        // three columns (x-1..x+1) and [x+0.5, x+1.5) two; `antialias: 0` does not change it
        // (it governs stroke fringes, not rect edges). The half-pixel span is the narrower of
        // the two, and the parity gate therefore counts the guide's columns as an expected
        // ink-path difference - like text and dashes - rather than as geometry.
        aPainter.fillRect(QRectF(float(marker.x) + 0.5f, float(marker.top), 1.0f, float(marker.bottom - marker.top + 1)));
    }
    if (!aMarquee.visible)
        return;
    aPainter.setAntialias(1.0f);
    aPainter.setFillStyle(aMarquee.fill);
    aPainter.roundRect(aMarquee.rect, float(aMarquee.radius));
    aPainter.fill();
    // The edge, dashed: Qt's DashLine is a 4-on/4-off pattern at width 1.
    aPainter.setStrokeStyle(aMarquee.edge);
    aPainter.setLineWidth(1.0f);
    const QRectF r = aMarquee.rect;
    for (double x = r.left(); x < r.right(); x += 8.0) {
        aPainter.beginPath();
        aPainter.moveTo(x, r.top() + kHalf);
        aPainter.lineTo(x + 4.0, r.top() + kHalf);
        aPainter.stroke();
        aPainter.beginPath();
        aPainter.moveTo(x, r.bottom() + kHalf);
        aPainter.lineTo(x + 4.0, r.bottom() + kHalf);
        aPainter.stroke();
    }
    for (double y = r.top(); y < r.bottom(); y += 8.0) {
        aPainter.beginPath();
        aPainter.moveTo(r.left() + kHalf, y);
        aPainter.lineTo(r.left() + kHalf, y + 4.0);
        aPainter.stroke();
        aPainter.beginPath();
        aPainter.moveTo(r.right() + kHalf, y);
        aPainter.lineTo(r.right() + kHalf, y + 4.0);
        aPainter.stroke();
    }
}

} // namespace timeBodyCanvas
} // namespace gui
