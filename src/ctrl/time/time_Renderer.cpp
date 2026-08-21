#include "ctrl/time/time_Renderer.h"
#include "ctrl/TimeLineEditor.h"
#include "gui/theme/Colors.h"

using namespace core;

namespace ctrl {
namespace time {

    static QColor withAlpha(QColor aColor, int aAlpha) { aColor.setAlpha(aAlpha); return aColor; }

    Renderer::Renderer(QPainter& aPainter, const CameraInfo& aCamera, const theme::TimeLine& aTheme,
        const TimeFormatType& timeFormat):
        mPainter(aPainter), mCamera(aCamera), mTheme(aTheme), mMargin(), mRange(), mScale(), timeFormatVar(timeFormat){}

    void Renderer::renderLines(const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect) {
        // draw each line
        mPainter.setRenderHint(QPainter::Antialiasing);

        const QBrush kBrushBody(mTheme.trackColor());
        const QBrush kBrushBodySelect(mTheme.trackSelectColor());
        const QBrush kBrushEdge(mTheme.trackEdgeColor());
        const QBrush kBrushSepa(mTheme.trackSeperatorColor());
        const QBrush kBrushText(mTheme.trackTextColor());
        const int textWidth = 100;
        const int textLeft = aCameraRect.center().x() - textWidth / 2;

        for (const TimeLineRow& row : aRows) {
            const QRect rect = row.rect;
            const QPoint cpos(mMargin, rect.bottom());

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

            // draw line (fill only — the 1px border is one hairline per seam
            // below; per-row outlines would double the shared boundary)
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

                // draw type labels
                mPainter.setPen(QPen(kBrushText, 1));
                int i = 0;
                for (int typei = 0; typei < TimeKeyType_TERM; ++typei) {
                    auto type = TimeLine::getTimeKeyTypeInOrderOfOperations(typei);
                    if (row.node->timeLine()->isEmpty(type)) {
                        continue;
                    }

                    const float h = static_cast<float>(rect.height()) / sepa;
                    mPainter.drawText(
                        QRect(textLeft, rect.top() + static_cast<int>(i * h), textWidth, static_cast<int>(h)),
                        TimeLine::getTimeKeyName(type),
                        QTextOption(Qt::AlignCenter)
                    );
                    ++i;
                }
            }

            // draw child keys
            if (row.closedFolder) {
                drawChildKeys(row.node, cpos);
            }
            // draw keys
            drawKeys(row.node, row);
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
        // A degenerate 0-frame range collapses both markers onto the same
        // pixel (x0 == x1); drawing twice would darken the line.
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

    void Renderer::renderHeader(int aHeight, int aFps) {
        const QRect cameraRect(-mCamera.leftTopPos().toPoint(), mCamera.screenSize());

        mPainter.setRenderHint(QPainter::Antialiasing, false);

        // draw header background
        {
            const QBrush kBrush(mTheme.headerBackgroundColor());
            QRect rect = cameraRect;
            rect.setHeight(aHeight);
            mPainter.setPen(QPen(kBrush, 1));
            mPainter.setBrush(kBrush);
            mPainter.drawRect(rect);

            // hairline under the ruler separates it from the track area.
            // The ticks end one row above this line (top + height - 1), so
            // the line never crosses them.
            const int lineY = rect.top() + rect.height();
            mPainter.setPen(QPen(QBrush(mTheme.rulerLineColor()), 1));
            mPainter.drawLine(rect.left(), lineY, rect.right(), lineY);
        }

        // draw header info
        {
            const QBrush kBrush(mTheme.headerContentColor());
            const int numberWidth = 6;
            const QPoint lt(mMargin, cameraRect.top());
            // the ticks end one row above the ruler hairline (drawn at
            // top + height) so they never draw over it
            const QPoint rb = lt + QPoint(mScale->maxPixelWidth(), aHeight - 1);
            const TimeFormat timeFormat(mRange, aFps);

            // The project's last frame owns the end of the ruler: when it is
            // in view but not on a scale multiple, it reads as a major tick
            // with its number, and any numbered neighbor whose label would
            // overlap it yields instead. The label occupies its slot in view
            // regardless of grid alignment — the loop draws it when the frame
            // falls on a scale multiple, the end block below otherwise — so
            // the yield rule must not depend on which path drew it (alignment
            // changes with zoom; gating on it made neighbor labels pop in and
            // out with the view).
            const int lastFrame = mScale->frameEnd();
            const auto lastAttr = mScale->attribute(lastFrame);
            const bool endLabelInView = lastFrame >= mRange.min() && lastFrame <= mRange.max();
            const bool lastLabelActive = endLabelInView && lastAttr.grid.y() < 10;
            const int lastLabelCenter = lt.x() + lastAttr.grid.x();
            const QString lastNumber = timeFormat.frameToString(lastFrame, timeFormatVar);
            const int lastNumberWidth = numberWidth * lastNumber.size();

            // The project's first frame mirrors the end label: off the scale
            // grid it reads as a major tick with its number, and numbered
            // neighbors yield to it the same way. For the default range this
            // is frame 0, which is always on a major (0 mod every step), so
            // the block is inert.
            const int firstFrame = mProjectRange.min();
            const auto firstAttr = mScale->attribute(firstFrame);
            const bool firstLabelInView = firstFrame >= mRange.min() && firstFrame <= mRange.max();
            const bool firstLabelActive = firstLabelInView && firstAttr.grid.y() < 10;
            const int firstLabelCenter = lt.x() + firstAttr.grid.x();
            const QString firstNumber = timeFormat.frameToString(firstFrame, timeFormatVar);
            const int firstNumberWidth = numberWidth * firstNumber.size();

            for (int i = mRange.min(); i <= mRange.max(); ++i) {
                // The boundary labels own their frames: when the range's
                // first/last frame label is active (the frame is off-major),
                // the loop's tier tick would stack under the boundary's major
                // tick — skip the frame entirely, the boundary block below
                // draws both the tick and the number.
                if ((i == firstFrame && firstLabelActive) || (i == lastFrame && lastLabelActive))
                    continue;

                auto attr = mScale->attribute(i);
                if (attr.grid.y() <= 0) continue;

                QPoint pos(lt.x() + attr.grid.x(), rb.y());

                // Fade the tick tiers so minor blips recede and even the
                // numbered major ticks stay a step below full brightness.
                qreal alpha = attr.grid.y() >= 10 ? 0.85
                            : attr.grid.y() >= 8  ? 0.65
                            : attr.grid.y() >= 6  ? 0.45
                                                  : 0.25;
                // Ticks outside the active playback range recede further:
                // the tier structure stays, but a fixed extra dim makes the
                // range edge read at a glance. The active range is the
                // project frame range — or the temporary playback window
                // while it overrides — and the boundary frames themselves
                // keep full tier brightness.
                const bool inRange = mActiveRange.contains(i);
                if (!inRange)
                    alpha *= 0.4;
                QColor tickColor = kBrush.color();
                tickColor.setAlphaF(alpha);
                mPainter.setPen(QPen(tickColor, 1));
                mPainter.drawLine(pos, pos + QPoint(0, -attr.grid.y()));

                if (attr.showNumber) {
                    QString number = timeFormat.frameToString(i, timeFormatVar);
                    const int width = static_cast<int>(numberWidth * number.size());
                    // A neighbor label never overlaps the range's boundary
                    // labels: the first and last frame's numbers always win
                    // the space they occupy. The test is symmetric — a label
                    // yields only when it actually crosses the boundary
                    // label's span, on either side (a one-sided test would
                    // swallow every label past the end).
                    if (endLabelInView && i != lastFrame) {
                        const int left = pos.x() - (width >> 1);
                        const int endRight = lastLabelCenter + (lastNumberWidth >> 1);
                        const int endLeft = lastLabelCenter - (lastNumberWidth >> 1);
                        if (left < endRight && pos.x() + (width >> 1) > endLeft)
                            continue;
                    }
                    if (firstLabelInView && i != firstFrame) {
                        const int left = pos.x() - (width >> 1);
                        const int firstRight = firstLabelCenter + (firstNumberWidth >> 1);
                        const int firstLeft = firstLabelCenter - (firstNumberWidth >> 1);
                        if (left < firstRight && pos.x() + (width >> 1) > firstLeft)
                            continue;
                    }
                    QColor numberColor = kBrush.color();
                    if (!inRange)
                        numberColor.setAlphaF(0.4);
                    mPainter.setPen(QPen(numberColor, 1));
                    const int left = pos.x() - (width >> 1);
                    const int top = lt.y() + ctrl::TimeLineEditor::kNumberTop;
                    const QRect rect(
                        QPoint(left, top), QPoint(left + width + 1, top + ctrl::TimeLineEditor::kNumberHeight)
                    );
                    mPainter.drawText(rect, number);
                }
            }

            if (firstLabelActive) {
                // The range's first frame reads as a major even off the grid;
                // while a playback window overrides the range it recedes with
                // the out-of-range content, like the end label.
                const bool inRange = mActiveRange.contains(firstFrame);
                const qreal firstAlpha = inRange ? 1.0 : 0.4;

                const QPoint pos(lt.x() + firstAttr.grid.x(), rb.y());

                QColor tickColor = kBrush.color();
                tickColor.setAlphaF(0.85 * firstAlpha);
                mPainter.setPen(QPen(tickColor, 1));
                mPainter.drawLine(pos, pos + QPoint(0, -10));

                // The loop skips this frame while the label is active, so the
                // number is always ours here (the yield rule keeps numbered
                // neighbors away from the slot).
                QColor numberColor = kBrush.color();
                numberColor.setAlphaF(firstAlpha);
                mPainter.setPen(QPen(numberColor, 1));
                const int width = numberWidth * firstNumber.size();
                const int left = pos.x() - (width >> 1);
                const int top = lt.y() + ctrl::TimeLineEditor::kNumberTop;
                const QRect rect(
                    QPoint(left, top), QPoint(left + width + 1, top + ctrl::TimeLineEditor::kNumberHeight)
                );
                mPainter.drawText(rect, firstNumber);
            }

            if (lastLabelActive) {
                // The end label marks the range's last frame; while a
                // playback window overrides the range, it recedes with the
                // rest of the out-of-range content instead of holding full
                // brightness.
                const bool inRange = mActiveRange.contains(lastFrame);
                const qreal endAlpha = inRange ? 1.0 : 0.4;

                const QPoint pos(lt.x() + lastAttr.grid.x(), rb.y());

                QColor tickColor = kBrush.color();
                tickColor.setAlphaF(0.85 * endAlpha);
                mPainter.setPen(QPen(tickColor, 1));
                mPainter.drawLine(pos, pos + QPoint(0, -10));

                // The loop skips this frame while the label is active, so the
                // number is always ours here (the yield rule keeps numbered
                // neighbors away from the slot).
                QColor numberColor = kBrush.color();
                numberColor.setAlphaF(endAlpha);
                mPainter.setPen(QPen(numberColor, 1));
                const int width = numberWidth * lastNumber.size();
                const int left = pos.x() - (width >> 1);
                const int top = lt.y() + ctrl::TimeLineEditor::kNumberTop;
                const QRect rect(
                    QPoint(left, top), QPoint(left + width + 1, top + ctrl::TimeLineEditor::kNumberHeight)
                );
                mPainter.drawText(rect, lastNumber);
            }

            // The playback window overlays the ruler content: the translucent
            // band washes ticks and numbers, the bracket stems and label
            // pills sit on top (the playhead widget paints above all of it).
            renderPlayWindow(cameraRect, aHeight, aFps);
        }
    }

    void Renderer::renderPlayWindow(const QRect& aCameraRect, int aHeight, int aFps) {
        if (!mPlayWindow.active())
            return;

        // Window edges on the content axis: the start frame's tick and the
        // tick of the frame AFTER the end. The window spans whole frames, so
        // the end frame's right edge is the next frame's tick (the same
        // convention as the range marker's x1 = margin + maxPixelWidth).
        const int x0 = mMargin + mScale->attribute(mPlayWindow.start).grid.x();
        const int x1 = mMargin + mScale->attribute(mPlayWindow.end + 1).grid.x();
        if (x1 <= x0)
            return;

        const int top = aCameraRect.top();
        const theme::Colors& c = theme::Colors::current();

        // The translucent band washes the ruler content inside the window,
        // clipped to the visible ruler (off-view pills are clipped by the
        // widget itself).
        const int c0 = qMax(x0, aCameraRect.left());
        const int c1 = qMin(x1, aCameraRect.right() + 1);
        if (c1 > c0) {
            mPainter.setPen(Qt::NoPen);
            mPainter.setBrush(c.windowBand);
            mPainter.drawRect(QRect(c0, top, c1 - c0, aHeight));
        }

        // Bracket stems: 2px, rising to the label pill's top and running to
        // the ruler bottom, so the arm and the stem share a full edge and
        // read as one bracket.
        const QRect stemL(x0, top, 2, aHeight);
        const QRect stemR(x1 - 2, top, 2, aHeight);
        mPainter.setPen(Qt::NoPen);
        mPainter.setBrush(c.windowEdge);
        mPainter.drawRect(stemL);
        mPainter.drawRect(stemR);

        // Label pills: the frame number is the bracket's horizontal arm,
        // attached to the stem's inner face. Rounded on the outer corners,
        // square on the face against the stem, so the joint stays
        // continuous. Two pills that would collide collapse into one
        // centered "a–b" label; the stems remain the edge.
        const core::TimeFormat timeFormat(mRange, aFps);
        const QString sL = timeFormat.frameToString(mPlayWindow.start, timeFormatVar);
        const QString sR = timeFormat.frameToString(mPlayWindow.end, timeFormatVar);
        const QFontMetrics fm(mPainter.font());
        const int wL = fm.horizontalAdvance(sL) + ctrl::TimeLineEditor::kPillPadding * 2;
        const int wR = fm.horizontalAdvance(sR) + ctrl::TimeLineEditor::kPillPadding * 2;
        const QRect pillL(x0 + 2, top, wL, ctrl::TimeLineEditor::kNumberHeight);
        const QRect pillR(x1 - 2 - wR, top, wR, ctrl::TimeLineEditor::kNumberHeight);

        mPainter.setRenderHint(QPainter::Antialiasing, true);
        mPainter.setPen(Qt::NoPen);
        mPainter.setBrush(c.windowEdge);
        if (pillL.right() >= pillR.left()) {
            const QString merged = sL + QString::fromUtf8("\u2013") + sR;
            const int w = fm.horizontalAdvance(merged) + ctrl::TimeLineEditor::kPillPadding * 2;
            const QRect pill((x0 + x1) / 2 - w / 2, top, w, ctrl::TimeLineEditor::kNumberHeight);
            mPainter.drawRoundedRect(
                QRectF(pill), ctrl::TimeLineEditor::kPillRadius, ctrl::TimeLineEditor::kPillRadius, Qt::AbsoluteSize
            );
            drawPillText(pill, merged, c.windowText, top);
        } else {
            mPainter.drawRoundedRect(
                QRectF(pillL), ctrl::TimeLineEditor::kPillRadius, ctrl::TimeLineEditor::kPillRadius, Qt::AbsoluteSize
            );
            mPainter.drawRoundedRect(
                QRectF(pillR), ctrl::TimeLineEditor::kPillRadius, ctrl::TimeLineEditor::kPillRadius, Qt::AbsoluteSize
            );
            // Square the inner corners (the faces against the stems) by
            // overdrawing the rounded slivers with the pill color, so the
            // bracket joint reads continuous. The cut pixels run to
            // right()/bottom() inclusive, so the right/bottom overdraws
            // start at -r+1 (right()-r / bottom()-r would miss the last
            // pixel row/column and leave the corner visibly rounded).
            const int r = ctrl::TimeLineEditor::kPillRadius;
            mPainter.fillRect(QRect(pillL.x(), pillL.y(), r, r), c.windowEdge);
            mPainter.fillRect(QRect(pillL.x(), pillL.bottom() - r + 1, r, r), c.windowEdge);
            mPainter.fillRect(QRect(pillR.right() - r + 1, pillR.y(), r, r), c.windowEdge);
            mPainter.fillRect(QRect(pillR.right() - r + 1, pillR.bottom() - r + 1, r, r), c.windowEdge);
            drawPillText(pillL, sL, c.windowText, top);
            drawPillText(pillR, sR, c.windowText, top);
        }
        mPainter.setRenderHint(QPainter::Antialiasing, false);
    }

    void Renderer::drawPillText(const QRect& aPill, const QString& aText, const QColor& aColor, int aTop) {
        const QFontMetrics fm(mPainter.font());
        const int textWidth = fm.horizontalAdvance(aText);
        // The exact same rect convention as the ruler numbers (top-aligned
        // at kNumberTop), so the pill text lines up with them precisely.
        const QRect numberRect(
            aPill.center().x() - textWidth / 2, aTop + ctrl::TimeLineEditor::kNumberTop, textWidth,
            ctrl::TimeLineEditor::kNumberHeight
        );
        mPainter.setPen(QPen(aColor, 1));
        mPainter.drawText(numberRect, aText);
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
            const int left = aRow.rect.left();
            const bool isSlimmed = aNode->isSlimmedDown();
            int validIndex = 0;

            for (int i = 0; i < TimeKeyType_TERM; ++i) {
                auto type = TimeLine::getTimeKeyTypeInOrderOfOperations(i);
                const TimeLine::MapType& map = timeLine.map(type);
                if (map.isEmpty())
                    continue;

                const float height = aRow.keyHeight(validIndex, validNum);
                ++validIndex;

                mPainter.setPen(QPen(kBrushKeyEdge, 1));

                auto itr = map.lowerBound(mRange.min());
                while (itr != map.end() && itr.key() <= mRange.max()) {
                    const bool isFocused = itr.value()->isFocused();
                    constexpr int lighter_val_selected = 150;
                    QBrush kBrushKeyBody1(theme::Colors::current().baseKey.lighter(lighter_val_selected));
                    QBrush kBrushKeyBody2(theme::Colors::current().baseKey);
                    switch (itr.value()->type()) {
                        case TimeKeyType_Move:
                            kBrushKeyBody1 = theme::Colors::current().moveKey;
                            kBrushKeyBody2 = theme::Colors::current().moveKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Rotate:
                            kBrushKeyBody1 = theme::Colors::current().rotateKey;
                            kBrushKeyBody2 = theme::Colors::current().rotateKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Scale:
                            kBrushKeyBody1 = theme::Colors::current().scaleKey;
                            kBrushKeyBody2 = theme::Colors::current().scaleKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Depth:
                            kBrushKeyBody1 = theme::Colors::current().depthKey;
                            kBrushKeyBody2 = theme::Colors::current().depthKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Opa:
                            kBrushKeyBody1 = theme::Colors::current().opaKey;
                            kBrushKeyBody2 = theme::Colors::current().opaKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Bone:
                            kBrushKeyBody1 = theme::Colors::current().boneKey;
                            kBrushKeyBody2 = theme::Colors::current().boneKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Pose:
                            kBrushKeyBody1 = theme::Colors::current().poseKey;
                            kBrushKeyBody2 = theme::Colors::current().poseKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Mesh:
                            kBrushKeyBody1 = theme::Colors::current().meshKey;
                            kBrushKeyBody2 = theme::Colors::current().meshKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_FFD:
                            kBrushKeyBody1 = theme::Colors::current().FFDKey;
                            kBrushKeyBody2 = theme::Colors::current().FFDKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Image:
                            kBrushKeyBody1 = theme::Colors::current().imageKey;
                            kBrushKeyBody2 = theme::Colors::current().imageKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_HSV:
                            kBrushKeyBody1 = theme::Colors::current().HSVKey;
                            kBrushKeyBody2 = theme::Colors::current().HSVKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_Blur:
                            kBrushKeyBody1 = theme::Colors::current().blurKey;
                            kBrushKeyBody2 = theme::Colors::current().blurKey.lighter(lighter_val_selected);
                            break;
                        case TimeKeyType_TERM:
                            break;
                    }
                    mPainter.setBrush(isFocused ? kBrushKeyBody2 : kBrushKeyBody1);

                    auto attr = mScale->attribute(itr.key());
                    QPointF pos(left + attr.grid.x() + 0.5, height + 0.5f);

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
                        pos[0] = QPointF(aPos.x() + attr.grid.x() + 0.5, aPos.y());
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
