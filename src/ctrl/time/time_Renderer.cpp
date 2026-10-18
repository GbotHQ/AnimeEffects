#include "ctrl/time/time_Renderer.h"
#include "ctrl/TimeLineEditor.h"
#include "gui/theme/Colors.h"

#include <cmath>

using namespace core;

namespace ctrl {
namespace time {

    static QColor withAlpha(QColor aColor, int aAlpha) { aColor.setAlpha(aAlpha); return aColor; }

    // The focused key's fill: the type colour mixed toward the keyframe
    // highlight (design s3 — "the fill brightens toward the keyframe highlight
    // ... the size never changes"). The mock's own step is
    // mixWhite(hue, 0.45) = round(c + (255 - c) * 0.45) per channel, i.e. 45%
    // of the distance to white; its --key-highlight token is #ffffff
    // (proposal.css), and the app's palette has no pure-white cell, so the mix
    // target is white itself and the step is 0.45. Hover brightens the same way
    // in the design; the app still has no hover state, so only focus uses it.
    constexpr double kSelectFillMix = 0.45;
    static QColor brightenTowardHighlight(const QColor& aColor) {
        const auto mix = [](int aChannel) {
            return static_cast<int>(std::lround(aChannel + (255 - aChannel) * kSelectFillMix));
        };
        return QColor(mix(aColor.red()), mix(aColor.green()), mix(aColor.blue()), aColor.alpha());
    }

    // The lane label's ink: the channel's hue at 0.42 (design s1 — "the ink
    // becomes the channel's hue at 0.42", one step quieter than the keys). The
    // value is the mock's own (hexA(hue, 0.42) in its band-name pass).
    constexpr int kLabelAlpha = 107; // 0.42
    // The selected key's ring (design s3): 1px wide, clear of the marker, in
    // the accent — the marker itself keeps its size and its own type colour.
    // Radius and alpha are the mock's own values for that ring (marker
    // r=keyRadius, ring r=keyRadius+2.6, at 0.85). The mock paints that ring in
    // --key-highlight (#ffffff); this pass keeps the app's accentBright, the
    // token every other focus/selection edge in the app already uses.
    constexpr int kSelectRingAlpha = 217; // 0.85
    constexpr double kSelectRingRadius = keyRadius + 2.6;

    // The channel hue of a key type: the timeline's own per-type key colours
    // (theme::Colors kKeyColorRow at the theme's key column). One lookup for
    // both the keys and their lane's label, so a band's name can never
    // disagree with the keys under it.
    static QColor keyColor(TimeKeyType aType) {
        const theme::Colors& c = theme::Colors::current();
        switch (aType) {
            case TimeKeyType_Move: return c.moveKey;
            case TimeKeyType_Rotate: return c.rotateKey;
            case TimeKeyType_Scale: return c.scaleKey;
            case TimeKeyType_Depth: return c.depthKey;
            case TimeKeyType_Opa: return c.opaKey;
            case TimeKeyType_Bone: return c.boneKey;
            case TimeKeyType_Pose: return c.poseKey;
            case TimeKeyType_Mesh: return c.meshKey;
            case TimeKeyType_FFD: return c.FFDKey;
            case TimeKeyType_Image: return c.imageKey;
            case TimeKeyType_HSV: return c.HSVKey;
            case TimeKeyType_Blur: return c.blurKey;
            case TimeKeyType_TERM: break;
        }
        return c.baseKey; // the neutral key, drawn plain in every state
    }

    Renderer::Renderer(QPainter& aPainter, const CameraInfo& aCamera, const theme::TimeLine& aTheme):
        mPainter(aPainter), mCamera(aCamera), mTheme(aTheme), mMargin(), mRange(), mScale() {}

    void Renderer::renderLines(const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect) {
        // draw each line
        mPainter.setRenderHint(QPainter::Antialiasing);

        const QBrush kBrushBody(mTheme.trackColor());
        const QBrush kBrushBodySelect(mTheme.trackSelectColor());
        const QBrush kBrushEdge(mTheme.trackEdgeColor());
        const QBrush kBrushSepa(mTheme.trackSeperatorColor());
        const int textWidth = 100;
        const int textLeft = aCameraRect.center().x() - textWidth / 2;
        // Design s1: a name is never drawn under the playhead's own pill — the
        // pill carries the only number in its column. The test is the pill's
        // own half-width about the playhead's x, i.e. exactly the rule the
        // ruler applies to its numbers (the mock suppresses both the same
        // way), so the lane names and the ruler cannot disagree about which
        // column the pill owns. The label's x is its own centre: its
        // placement is unchanged (centred on the viewport).
        const bool labelUnderPlayhead = qAbs(textLeft + textWidth / 2 - mPlayheadX) < mPlayheadHalfWidth;

        for (const TimeLineRow& row : aRows) {
            const QRect rect = row.rect;

            // Rows are culled by their vertical band only: the horizontal
            // view extent is never bounded by the frame range, so lanes stay
            // visible (fills, keys and all) even when the view floats
            // entirely outside it.
            if (rect.bottom() < aCullRect.top() || aCullRect.bottom() < rect.top())
                continue;

            // The lane fills, separators and hairlines span the visible width
            // (the camera rect), not just the frame range: when the content is
            // narrower than the viewport, the lanes reach the panel edges.
            // There is no performance reason to bound them to the content.
            const QRect band(QPoint(aCameraRect.left(), rect.top()), QPoint(aCameraRect.right(), rect.bottom()));

            // draw line (fill only; the 1px border is a separate hairline
            // pass below, one per seam)
            mPainter.setPen(Qt::NoPen);
            mPainter.setBrush(row.selecting ? kBrushBodySelect : kBrushBody);
            mPainter.drawRect(band);

            if (!row.node || !row.node->timeLine())
                continue;

            if (!row.node->isSlimmedDown()) {
                // draw separators
                mPainter.setPen(QPen(kBrushSepa, 1, Qt::DotLine));
                const int sepa = row.node->timeLine()->validTypeCount();
                for (int i = 1; i < sepa; ++i) {
                    const float h = static_cast<float>(rect.height()) / sepa;
                    const float y = rect.top() + i * h;
                    const QPointF v0(band.left(), y);
                    const QPointF v1(band.right(), y);
                    mPainter.drawLine(v0, v1);
                }

                // draw type labels: in the channel's hue at 0.42 (design s1),
                // and not at all when the playhead's pill owns this column.
                if (!labelUnderPlayhead) {
                    int i = 0;
                    for (int typei = 0; typei < TimeKeyType_TERM; ++typei) {
                        auto type = TimeLine::getTimeKeyTypeInOrderOfOperations(typei);
                        if (row.node->timeLine()->isEmpty(type)) {
                            continue;
                        }

                        const float h = static_cast<float>(rect.height()) / sepa;
                        mPainter.setPen(QPen(withAlpha(keyColor(type), kLabelAlpha), 1));
                        mPainter.drawText(
                            QRect(textLeft, rect.top() + static_cast<int>(i * h), textWidth, static_cast<int>(h)),
                            TimeLine::getTimeKeyName(type),
                            QTextOption(Qt::AlignCenter)
                        );
                        ++i;
                    }
                }
            }

        }

        // 1px borders: one hairline per seam (each row's top) plus the last
        // row's bottom, so adjacent rows' 1px outlines can't stack into a 2px
        // border. AA off keeps each line exactly one pixel. The hairlines span
        // the visible width like the fills above.
        mPainter.setRenderHint(QPainter::Antialiasing, false);
        mPainter.setPen(QPen(kBrushEdge, 1));
        mPainter.setBrush(Qt::NoBrush);
        for (const TimeLineRow& row : aRows) {
            if (row.rect.bottom() < aCullRect.top() || aCullRect.bottom() < row.rect.top()) continue;
            mPainter.drawLine(aCameraRect.left(), row.rect.top(), aCameraRect.right(), row.rect.top());
        }
        if (!aRows.isEmpty()) {
            const TimeLineRow& last = aRows.back();
            if (!(last.rect.bottom() < aCullRect.top() || aCullRect.bottom() < last.rect.top())) {
                mPainter.drawLine(aCameraRect.left(), last.rect.bottom(), aCameraRect.right(), last.rect.bottom());
            }
        }
        mPainter.setRenderHint(QPainter::Antialiasing);
    }

    void Renderer::renderRangeMarkers(const QRect& aCameraRect) {
        // Vertical hairlines at the frame range's first and last frame:
        // continue the ruler's boundary ticks down through the track area.
        // The ruler already marks the range ends (the first frame's tick, and
        // the specially-labeled last frame's tick), so the lines start where
        // those ticks end — at the ruler's bottom hairline — and run to the
        // bottom of the viewport.
        const int top = aCameraRect.top() + ctrl::TimeLineEditor::kHeaderHeight;
        const int x0 = mMargin + mScale->pixelWidth(mProjectRange.min());
        const int x1 = mMargin + mScale->pixelWidth(mProjectRange.max());
        // A degenerate range (min == max) collapses both markers onto one
        // pixel; skip the double draw.
        if (x0 == x1)
            return;

        mPainter.setRenderHint(QPainter::Antialiasing, false);
        // The design system's 1px section-separator token — the same line
        // family as the ruler hairline the markers start from and the row
        // seams they cross — at 40% opacity so the guides recede behind the
        // track content. While a temporary playback window overrides the
        // range, the markers drop to 25%: they no longer gate playback, and
        // the window's own brackets carry the boundary reading.
        const int markerAlpha = mPlayWindow.active() ? 64 : 102;
        mPainter.setPen(QPen(withAlpha(theme::Colors::current().hairline, markerAlpha), 1));
        mPainter.setBrush(Qt::NoBrush);
        mPainter.drawLine(x0, top, x0, aCameraRect.bottom());
        mPainter.drawLine(x1, top, x1, aCameraRect.bottom());
    }

    void Renderer::renderKeys(const QVector<TimeLineRow>& aRows, const QRect& aCullRect) {
        mPainter.setRenderHint(QPainter::Antialiasing);
        for (const TimeLineRow& row : aRows) {
            const QRect rect = row.rect;
            // same vertical-band culling as the background pass above
            if (rect.bottom() < aCullRect.top() || aCullRect.bottom() < rect.top())
                continue;
            if (!row.node || !row.node->timeLine())
                continue;

            const QPoint cpos(mMargin, rect.bottom());
            if (row.closedFolder) {
                drawChildKeys(row.node, cpos);
            }
            drawKeys(row.node, row);
        }
    }

    void Renderer::renderHandle(const QPoint& aPoint, int aRange) {
        const QPoint pos = aPoint + QPoint(0, -static_cast<int>(mCamera.leftTopPos().y()));
        const int range = aRange;

        const QBrush kBrushBody(withAlpha(theme::Colors::current().text, 180));
        const QBrush kBrushEdge(withAlpha(theme::Colors::current().textMuted, 180));

        mPainter.setPen(QPen(kBrushEdge, 1));
        mPainter.setBrush(kBrushBody);
        mPainter.drawLine(pos + QPoint(0, range), pos + QPoint(0, mCamera.screenHeight()));

        mPainter.setRenderHint(QPainter::Antialiasing);
        mPainter.drawEllipse(pos, range, range);
    }

    void Renderer::renderSelectionRange(const QRect& aRect) {
        if (aRect.width() >= 2 && aRect.height() >= 2) {
            // brand-hued selection box: the marching-ants edge is the accent
            // color, the fill the same color at lower opacity; corners are
            // slightly rounded (6px), which needs AA so the curve reads smooth
            mPainter.setRenderHint(QPainter::Antialiasing, true);
            const QBrush kSelectEdge(withAlpha(theme::Colors::current().accentBright, 230));
            const QBrush kSelectBody(withAlpha(theme::Colors::current().accentBright, 24));
            mPainter.setPen(QPen(kSelectEdge, 1, Qt::DashLine));
            mPainter.setBrush(kSelectBody);
            // Inset by half a pixel so the 1px pen is centered inside the
            // rect rather than straddling its edge; the AA'd edge is
            // intentionally soft to keep the rounded corners smooth.
            mPainter.drawRoundedRect(QRectF(aRect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }
    }

    void Renderer::drawKeys(const ObjectNode* aNode, const TimeLineRow& aRow) {
        const QBrush kBrushKeyEdge(theme::Colors::current().outline);
        QPointF holder[4] = {QPointF(0.0, -4.2), QPointF(4.2, 0.0), QPointF(0.0, 4.2), QPointF(-4.2, 0.0)};

        if (aNode && aNode->timeLine()) {
            const TimeLine& timeLine = *(aNode->timeLine());
            const int validNum = timeLine.validTypeCount();
            // Keys sit at their absolute frame position (margin + spacing*frame),
            // the same base the playhead, ruler ticks and child keys use.
            const int left = mMargin;
            const bool isSlimmed = aNode->isSlimmedDown();
            int validIndex = 0;

            for (int i = 0; i < TimeKeyType_TERM; ++i) {
                auto type = TimeLine::getTimeKeyTypeInOrderOfOperations(i);
                const TimeLine::MapType& map = timeLine.map(type);
                if (map.isEmpty())
                    continue;

                const float height = aRow.keyHeight(validIndex, validNum);
                ++validIndex;

                auto itr = map.lowerBound(mRange.min());
                while (itr != map.end() && itr.key() <= mRange.max()) {
                    const bool isFocused = itr.value()->isFocused();
                    const QColor hue = keyColor(itr.value()->type());
                    // Unfocused keys keep their own type colour outright; a
                    // focused one brightens its fill toward the keyframe
                    // highlight (above), which is the design's selection
                    // feedback for the fill — the .lighter(150) body swap and
                    // the neutral key's polarity inversion (lit when unfocused,
                    // plain when focused) are both gone.
                    mPainter.setPen(QPen(kBrushKeyEdge, 1));
                    mPainter.setBrush(isFocused ? brightenTowardHighlight(hue) : hue);

                    auto attr = mScale->attribute(itr.key());
                    QPointF pos(left + attr.pos + 0.5, height + 0.5f);

                    if (isSlimmed) {
                        /*
                        const QPointF poly[3] = {
                            pos + QPointF(0.0f, -2.5f),
                            pos + QPointF(-3.0f, 2.5f),
                            pos + QPointF(3.0f, 2.5f) };
                        mPainter.drawConvexPolygon(poly, 3);
                        */
                        // mPainter.drawEllipse(pos, 3.0f, 1.5f);
                        const QPointF poly[] = {
                            pos + QPointF(keyRadius * -1, keyRadiusSlim * -1),
                            pos + QPointF(keyRadius, keyRadiusSlim * -1),
                            pos + QPointF(keyRadius, keyRadiusSlim),
                            pos + QPointF(keyRadius * -1, keyRadiusSlim)};
                        mPainter.drawConvexPolygon(poly, 4);
                    } else if (itr.value()->canHoldChild()) {
                        const QPointF poly[4] = {pos + holder[0], pos + holder[1], pos + holder[2], pos + holder[3]};
                        mPainter.drawConvexPolygon(poly, 4);
                    } else {
                        mPainter.drawEllipse(pos, keyRadius, keyRadius);
                    }

                    if (isFocused) {
                        // Design s3: the selected key's 1px accent ring, clear
                        // of the marker; no size change, so the drawn radius
                        // stays keyRadius (the hit radius is ~4x it, and no hit
                        // test keys off the drawn size). It replaces the old
                        // fill brightening outright.
                        mPainter.setPen(
                            QPen(withAlpha(theme::Colors::current().accentBright, kSelectRingAlpha), 1)
                        );
                        mPainter.setBrush(Qt::NoBrush);
                        mPainter.drawEllipse(pos, kSelectRingRadius, kSelectRingRadius);
                    }

                    ++itr;
                }
            }
        }
    }

    void Renderer::drawChildKeys(const ObjectNode* aNode, const QPoint& aPos) {
        const QBrush kBrushKey(theme::Colors::current().baseKey);

        mPainter.setPen(QPen(kBrushKey, 1));
        mPainter.setBrush(kBrushKey);

        for (auto child : aNode->children()) {
            ObjectNode::ConstIterator treeItr(child);
            while (treeItr.hasNext()) {
                const ObjectNode* node = treeItr.next();
                if (!node->timeLine())
                    continue;
                const TimeLine& timeLine = *(node->timeLine());

                for (int i = 0; i < TimeKeyType_TERM; ++i) {
                    auto type = TimeLine::getTimeKeyTypeInOrderOfOperations(i);
                    const TimeLine::MapType& map = timeLine.map(type);

                    auto itr = map.lowerBound(mRange.min());
                    while (itr != map.end() && itr.key() <= mRange.max()) {
                        auto attr = mScale->attribute(itr.key());
                        QPointF pos[3];
                        pos[0] = QPointF(aPos.x() + attr.pos + 0.5, aPos.y());
                        pos[1] = pos[0] + QPointF(keyRadiusChild, keyRadiusChildPoly * -1);
                        pos[2] = pos[0] + QPointF(keyRadiusChild * -1, keyRadiusChildPoly * -1);
                        mPainter.drawConvexPolygon(pos, 3);
                        ++itr;
                    }
                }
            }
        }
    }

} // namespace time
} // namespace ctrl
