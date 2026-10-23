#include "ctrl/time/time_BodyModel.h"

#include <QFontMetrics>
#include <QPainter>

#include "core/TimeKey.h"
#include "core/TimeLine.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/TimeLineRow.h"
#include "ctrl/time/time_Scaler.h"
#include "gui/theme/Colors.h"

namespace ctrl {
namespace time {

namespace {

// The lane label's ink: the channel's hue at the timeline's own alpha (design s1 - one
// step quieter than the keys). Read from the SAME cells the keys use, so a lane's name,
// its keys and the graph panel's curve cannot disagree about a channel's colour.
constexpr int kLabelAlpha = 107; // 0.42

QColor withAlpha(QColor aColor, int aAlpha) {
    aColor.setAlpha(aAlpha);
    return aColor;
}

QColor keyColor(core::TimeKeyType aType) {
    return theme::channelColor(theme::Colors::current(), aType, 0);
}

constexpr int kLabelWidth = 100;

// THE FOCUSED KEY'S FILL: the channel hue mixed 0.45 of the way to the TEXT colour.
constexpr double kSelectFillMix = 0.45;

QColor mixTowardText(const QColor& aColor) {
    const QColor text = theme::Colors::current().text;
    const auto mix = [](int aChannel, int aTarget) {
        return static_cast<int>(std::lround(aChannel + (aTarget - aChannel) * kSelectFillMix));
    };
    return QColor(
        mix(aColor.red(), text.red()), mix(aColor.green(), text.green()), mix(aColor.blue(), text.blue()),
        aColor.alpha()
    );
}

// The selected key's ring: the key's OWN channel hue at the shared alpha, drawn OUTSIDE the marker.
constexpr double kSelectRingRadiusModel = keyRadius + kSelectRingOffset;
// A folder's holder diamond arm.
constexpr double kHolderArm = 4.2;


} // namespace

Lines buildLines(
    const QVector<ctrl::TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect, const Inks& aInks
) {
    Lines lines;
    lines.inks = aInks;
    const int textLeft = aCameraRect.center().x() - kLabelWidth / 2;

    for (const ctrl::TimeLineRow& row : aRows) {
        const QRect rect = row.rect;
        // Rows are culled by their vertical band only: the horizontal view extent is never
        // bounded by the frame range, so lanes stay visible even when the view floats
        // entirely outside it.
        if (rect.bottom() < aCullRect.top() || aCullRect.bottom() < rect.top())
            continue;

        // The fill spans the visible width (the camera rect), not the frame range: when the
        // content is narrower than the viewport the lanes reach the panel edges.
        const QRect band(QPoint(aCameraRect.left(), rect.top()), QPoint(aCameraRect.right(), rect.bottom()));
        Lane lane;
        lane.rect = band;
        lane.selected = row.selecting;

        if (row.node && row.node->timeLine() && !row.node->isSlimmedDown()) {
            const core::TimeLine& timeLine = *(row.node->timeLine());
            const int sepa = timeLine.validTypeCount();
            const float h = sepa > 0 ? static_cast<float>(rect.height()) / sepa : 0.0f;

            for (int i = 1; i < sepa; ++i) {
                const QPointF v0(band.left(), rect.top() + i * h);
                const QPointF v1(band.right(), rect.top() + i * h);
                lane.separators.append({int(v0.x()), int(v1.x()), v0.y()});
            }

            int i = 0;
            for (int typei = 0; typei < core::TimeKeyType_TERM; ++typei) {
                const core::TimeKeyType type = core::TimeLine::getTimeKeyTypeInOrderOfOperations(typei);
                if (timeLine.isEmpty(type))
                    continue;
                Label label;
                label.box = QRect(
                    textLeft, rect.top() + static_cast<int>(i * h), kLabelWidth, static_cast<int>(h)
                );
                label.text = core::TimeLine::getTimeKeyName(type);
                label.ink = withAlpha(keyColor(type), kLabelAlpha);
                lane.labels.append(label);
                ++i;
            }
        }

        lines.lanes.append(lane);
    }

    // 1px borders: one hairline per seam (each ROW's top) plus the LAST ROW's bottom, so
    // adjacent lanes' outlines cannot stack into a 2px border. The two rules are stated
    // over the rows, not over the culled lanes: the last-row bottom belongs to the row the
    // view pushed last, and a culled last row takes that hairline with it (the old pass's
    // own rule - a lane set that stops short must not grow a bottom border).
    for (const ctrl::TimeLineRow& row : aRows) {
        if (row.rect.bottom() < aCullRect.top() || aCullRect.bottom() < row.rect.top())
            continue;
        lines.seams.append({aCameraRect.left(), aCameraRect.right(), row.rect.top()});
    }
    if (!aRows.isEmpty()) {
        const ctrl::TimeLineRow& last = aRows.back();
        if (!(last.rect.bottom() < aCullRect.top() || aCullRect.bottom() < last.rect.top()))
            lines.seams.append({aCameraRect.left(), aCameraRect.right(), last.rect.bottom()});
    }

    return lines;
}

KeyPass buildKeys(
    const core::ObjectNode* aNode, const TimeLineRow& aRow, const ctrl::time::Scaler& aScale,
    const util::Range& aRange, int aMargin
) {
    KeyPass pass;
    pass.edge = theme::Colors::current().outline;
    if (!aNode || !aNode->timeLine())
        return pass;

    const core::TimeLine& timeLine = *(aNode->timeLine());
    const int validNum = timeLine.validTypeCount();
    const int left = aMargin;
    const bool isSlimmed = aNode->isSlimmedDown();
    int validIndex = 0;

    for (int i = 0; i < core::TimeKeyType_TERM; ++i) {
        const core::TimeKeyType type = core::TimeLine::getTimeKeyTypeInOrderOfOperations(i);
        const core::TimeLine::MapType& map = timeLine.map(type);
        if (map.isEmpty())
            continue;

        const float height = aRow.keyHeight(validIndex, validNum);
        ++validIndex;

        auto itr = map.lowerBound(aRange.min());
        while (itr != map.end() && itr.key() <= aRange.max()) {
            const bool isFocused = itr.value()->isFocused();
            const QColor hue = keyColor(itr.value()->type());
            // A focused key mixes its fill toward the text colour; a PROPORTIONAL one only
            // lowers the fill's opacity (the shape, radius and hue are untouched).
            QColor fill = isFocused ? mixTowardText(hue) : hue;
            if (itr.value()->isProportional())
                fill.setAlpha(kProportionalFillAlpha);

            const QPointF pos(left + aScale.attribute(itr.key()).pos + 0.5, height + 0.5f);
            KeyMark mark;
            mark.centre = pos;
            mark.fill = fill;
            if (isSlimmed) {
                mark.shape = KeyShape::Polygon;
                mark.polygon << pos + QPointF(keyRadius * -1, keyRadiusSlim * -1)
                             << pos + QPointF(keyRadius, keyRadiusSlim * -1)
                             << pos + QPointF(keyRadius, keyRadiusSlim)
                             << pos + QPointF(keyRadius * -1, keyRadiusSlim);
            } else if (itr.value()->canHoldChild()) {
                mark.shape = KeyShape::Polygon;
                mark.polygon << pos + QPointF(0.0, -kHolderArm) << pos + QPointF(kHolderArm, 0.0)
                             << pos + QPointF(0.0, kHolderArm) << pos + QPointF(-kHolderArm, 0.0);
            } else {
                mark.shape = KeyShape::Ellipse;
                mark.radiusX = keyRadius;
                mark.radiusY = keyRadius;
            }
            if (isFocused) {
                mark.hasRing = true;
                mark.ringRadius = kSelectRingRadiusModel;
                mark.ring = withAlpha(hue, kSelectRingAlpha);
            }
            pass.marks.append(mark);
            ++itr;
        }
    }
    return pass;
}

KeyPass buildChildKeys(
    const core::ObjectNode* aNode, const QPoint& aPos, const ctrl::time::Scaler& aScale,
    const util::Range& aRange
) {
    KeyPass pass;
    pass.edge = theme::Colors::current().baseKey;
    pass.child = true;
    if (!aNode)
        return pass;

    for (auto child : aNode->children()) {
        core::ObjectNode::ConstIterator treeItr(child);
        while (treeItr.hasNext()) {
            const core::ObjectNode* node = treeItr.next();
            if (!node->timeLine())
                continue;
            const core::TimeLine& timeLine = *(node->timeLine());

            for (int i = 0; i < core::TimeKeyType_TERM; ++i) {
                const core::TimeKeyType type = core::TimeLine::getTimeKeyTypeInOrderOfOperations(i);
                const core::TimeLine::MapType& map = timeLine.map(type);

                auto itr = map.lowerBound(aRange.min());
                while (itr != map.end() && itr.key() <= aRange.max()) {
                    QColor fill = pass.edge;
                    if (itr.value()->isProportional())
                        fill.setAlpha(kProportionalFillAlpha);
                    const QPointF pos(aPos.x() + aScale.attribute(itr.key()).pos + 0.5, aPos.y());
                    KeyMark mark;
                    mark.shape = KeyShape::Polygon;
                    mark.centre = pos;
                    mark.fill = fill;
                    mark.polygon << pos
                                 << pos + QPointF(keyRadiusChild, keyRadiusChildPoly * -1)
                                 << pos + QPointF(keyRadiusChild * -1, keyRadiusChildPoly * -1);
                    pass.marks.append(mark);
                    ++itr;
                }
            }
        }
    }
    return pass;
}

void paintKeys(QPainter& aPainter, const KeyPass& aPass) {
    // The markers are round and their rings are 1px arcs: the pass owns its antialiasing hint
    // (the old renderKeys set it once for the whole pass), or a caller that just painted
    // hairlines leaves the rings faceted and the ring's coverage profile wrong.
    aPainter.setRenderHint(QPainter::Antialiasing);
    for (const KeyMark& mark : aPass.marks) {
        aPainter.setPen(QPen(aPass.edge, 1));
        aPainter.setBrush(mark.fill);
        if (mark.shape == KeyShape::Ellipse)
            aPainter.drawEllipse(mark.centre, mark.radiusX, mark.radiusY);
        else
            aPainter.drawConvexPolygon(mark.polygon);
        if (mark.hasRing) {
            aPainter.setPen(QPen(mark.ring, 1));
            aPainter.setBrush(Qt::NoBrush);
            aPainter.drawEllipse(mark.centre, mark.ringRadius, mark.ringRadius);
        }
    }
}

QVector<RangeMarker> buildRangeMarkers(
    const QRect& aCameraRect, const util::Range& aProjectRange, bool aPlayWindowActive,
    const ctrl::time::Scaler& aScale, int aMargin
) {
    QVector<RangeMarker> markers;
    const int top = aCameraRect.top() + ctrl::TimeLineEditor::kHeaderHeight;
    const int x0 = aMargin + aScale.pixelWidth(aProjectRange.min());
    const int x1 = aMargin + aScale.pixelWidth(aProjectRange.max());
    // A degenerate range collapses both markers onto one column: skip the double draw.
    if (x0 == x1)
        return markers;
    // The section-separator token at 40% so the guides recede behind the lanes; while a
    // playback window overrides the range they drop to 25% (the window's brackets carry the
    // boundary reading then).
    const int alpha = aPlayWindowActive ? 64 : 102;
    const QColor ink = withAlpha(theme::Colors::current().hairline, alpha);
    markers.append({x0, top, aCameraRect.bottom(), ink});
    markers.append({x1, top, aCameraRect.bottom(), ink});
    return markers;
}

SelectionBox buildMarquee(const QRect& aRect) {
    SelectionBox marquee;
    if (aRect.width() < 2 || aRect.height() < 2)
        return marquee;
    marquee.visible = true;
    // Inset by half a pixel so the 1px pen is centred inside the rect rather than straddling
    // its edge; the AA'd edge is intentionally soft to keep the rounded corners smooth.
    marquee.rect = QRectF(aRect).adjusted(0.5, 0.5, -0.5, -0.5);
    marquee.edge = withAlpha(theme::Colors::current().accentBright, 230);
    marquee.fill = withAlpha(theme::Colors::current().accentBright, 24);
    return marquee;
}

void paintGuides(QPainter& aPainter, const QVector<RangeMarker>& aMarkers, const SelectionBox& aMarquee) {
    if (!aMarkers.isEmpty()) {
        aPainter.setRenderHint(QPainter::Antialiasing, false);
        aPainter.setBrush(Qt::NoBrush);
        for (const RangeMarker& marker : aMarkers) {
            aPainter.setPen(QPen(marker.ink, 1));
            aPainter.drawLine(marker.x, marker.top, marker.x, marker.bottom);
        }
    }
    if (aMarquee.visible) {
        aPainter.setRenderHint(QPainter::Antialiasing, true);
        aPainter.setPen(QPen(aMarquee.edge, 1, Qt::DashLine));
        aPainter.setBrush(aMarquee.fill);
        aPainter.drawRoundedRect(aMarquee.rect, aMarquee.radius, aMarquee.radius);
    }
}

BodyScene buildBodyScene(
    const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect,
    const ctrl::time::Scaler& aScale, const util::Range& aRange, const util::Range& aProjectRange,
    bool aPlayWindowActive, const QRect& aMarqueeRect, int aMargin, const Inks& aInks
) {
    BodyScene scene;
    scene.lines = buildLines(aRows, aCameraRect, aCullRect, aInks);
    scene.markers = buildRangeMarkers(aCameraRect, aProjectRange, aPlayWindowActive, aScale, aMargin);
    scene.selection = buildMarquee(aMarqueeRect);
    for (const TimeLineRow& row : aRows) {
        const QRect rect = row.rect;
        if (rect.bottom() < aCullRect.top() || aCullRect.bottom() < rect.top())
            continue;
        if (!row.node || !row.node->timeLine())
            continue;
        const QPoint cpos(aMargin, rect.bottom());
        if (row.closedFolder)
            scene.keyPasses.append(buildChildKeys(row.node, cpos, aScale, aRange));
        scene.keyPasses.append(buildKeys(row.node, row, aScale, aRange, aMargin));
    }
    return scene;
}

void paintBodyScene(QPainter& aPainter, const BodyScene& aScene) {
    paintLines(aPainter, aScene.lines);
    paintGuides(aPainter, aScene.markers, SelectionBox());
    for (const KeyPass& pass : aScene.keyPasses)
        paintKeys(aPainter, pass);
    paintGuides(aPainter, {}, aScene.selection);
}

void paintLines(QPainter& aPainter, const Lines& aLines) {
    aPainter.setRenderHint(QPainter::Antialiasing);

    const QBrush body(aLines.inks.body);
    const QBrush bodySelected(aLines.inks.bodySelected);
    const QBrush separator(aLines.inks.separator);

    for (const Lane& lane : aLines.lanes) {
        // fill only; the 1px border is the separate seam pass below
        aPainter.setPen(Qt::NoPen);
        aPainter.setBrush(lane.selected ? bodySelected : body);
        aPainter.drawRect(lane.rect);

        if (!lane.separators.isEmpty()) {
            aPainter.setPen(QPen(separator, 1, Qt::DotLine));
            for (const Separator& each : lane.separators)
                aPainter.drawLine(QPointF(each.x0, each.y), QPointF(each.x1, each.y));
        }

        for (const Label& label : lane.labels) {
            aPainter.setPen(QPen(label.ink, 1));
            aPainter.drawText(label.box, label.text, QTextOption(Qt::AlignCenter));
        }
    }

    aPainter.setRenderHint(QPainter::Antialiasing, false);
    aPainter.setPen(QPen(aLines.inks.edge, 1));
    aPainter.setBrush(Qt::NoBrush);
    for (const Seam& seam : aLines.seams)
        aPainter.drawLine(seam.x0, seam.y, seam.x1, seam.y);
    aPainter.setRenderHint(QPainter::Antialiasing);
}

} // namespace time
} // namespace ctrl
