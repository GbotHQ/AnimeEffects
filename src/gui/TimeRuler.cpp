#include "gui/TimeRuler.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include "core/TimeFormat.h"
#include "ctrl/TimeLineEditor.h"
#include "gui/TimeLineEditorWidget.h"
#include "gui/FrameRuler.h"
#include "gui/theme/Colors.h"

namespace gui {

namespace {

} // namespace

//-------------------------------------------------------------------------------------------------
TimeRuler::TimeRuler(TimeLineEditorWidget& aSurface, QWidget* aParent):
    QWidget(aParent),
    mSurface(aSurface),
    mEditor(aSurface.editor()),
    mCamera(),
    mAbstractCursor(),
    mTimelineTheme(),
    mGesturing(false),
    mPanKeyHeld(false) {
    this->setMouseTracking(true);
    // The ruler owns its strip's paint and left-button gestures, but the
    // context menus live on the surface (the playhead overlays this widget,
    // so right-clicks over the pill would otherwise split the menu paths).
    this->setContextMenuPolicy(Qt::NoContextMenu);
    this->setAutoFillBackground(false);
}

void TimeRuler::setCamera(const core::CameraInfo* aCamera) {
    mCamera = aCamera;
    update();
}

void TimeRuler::updateTheme() {
    mTimelineTheme.reset();
    update();
}

//-------------------------------------------------------------------------------------------------
void TimeRuler::paintEvent(QPaintEvent* aEvent) {
    QPainter painter;
    painter.begin(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // The strip is viewport-fixed: its paint translates by the view origin,
    // exactly like the track surface, so the numbers scroll with the content
    // while the strip itself never moves. camRect mirrors the renderer's
    // camera rect in this widget's painter space.
    const QPoint origin = mCamera ? mCamera->leftTopPos().toPoint() : QPoint();
    painter.translate(origin);
    const QRect cameraRect(-origin, this->size());

    const QRect cullRect(aEvent->rect().translated(-origin).marginsAdded(QMargins(2, 2, 2, 2)));
    const int margin = ctrl::TimeLineEditor::kTimeLineMargin;
    const ctrl::time::Scaler& scale = mEditor->timeScale();
    const int bgn = scale.frame(cullRect.left() - margin - 5);
    const int end = scale.frame(cullRect.right() - margin + 5);

    renderHeader(painter, cameraRect, util::Range(bgn, end), mEditor->fps());
    painter.end();
}

void TimeRuler::renderHeader(QPainter& aPainter, const QRect& aCameraRect, const util::Range& aVisibleRange, int aFps) {
    const int aHeight = ctrl::TimeLineEditor::kHeaderHeight;
    const ctrl::time::Scaler& scale = mEditor->timeScale();
    const int margin = ctrl::TimeLineEditor::kTimeLineMargin;

    aPainter.setRenderHint(QPainter::Antialiasing, false);

    // draw header background (the hairline under it is drawn by the surface:
    // it sits on the first track row, below this widget)
    {
        const QBrush kBrush(mTimelineTheme.headerBackgroundColor());
        QRect rect = aCameraRect;
        rect.setHeight(aHeight);
        aPainter.setPen(QPen(kBrush, 1));
        aPainter.setBrush(kBrush);
        aPainter.drawRect(rect);
    }

    // THE STRIP IS THE SHARED RULER'S MODEL (gui/FrameRuler.h): this widget supplies
    // the geometry, the font's measurements and the editor's facts; the model decides
    // which frames are ticked, which are numbered, how a number yields to the
    // project's own first/last frame, and how the temporary window's band, stems and
    // pills are laid out. The graph panel builds the same model and paints it with
    // ImGui, so the two rulers cannot disagree about any of that.
    const QRect band(aCameraRect.left(), aCameraRect.top(), aCameraRect.width(), aHeight);
    const QFontMetrics metrics(aPainter.font());
    const core::TimeFormat format(mEditor->frameRange(), aFps);

    frameRuler::Inputs in;
    in.band = band;
    in.visible = band;
    in.labelTop = band.top() + frameRuler::kNumberTop;
    in.frameRange = mEditor->frameRange();
    in.hasFrameRange = true;
    in.activeRange = mEditor->playbackRange();
    in.visibleFrames = aVisibleRange;
    in.fps = aFps;
    in.formatType = mEditor->timeFormatType();

    // The playhead's own column: gui::TimeCursor draws the frame-number pill there, and
    // that pill carries the only number in its column, so the model suppresses any
    // number whose centre falls inside the pill's half-width. The span is the PILL's own
    // geometry, measured with the font both are painted in.
    const int playFrame = mEditor->currentFrame().get();
    in.hasPlayhead = true;
    in.playheadFrame = playFrame;
    in.playheadText = format.frameToString(playFrame, in.formatType);
    in.playheadTextWidth = metrics.horizontalAdvance(in.playheadText);

    const ctrl::TimeLineEditor::PlayWindow& window = mEditor->playWindow();
    if (window.active()) {
        in.windowStart = window.start;
        in.windowEnd = window.end;
        in.windowStartText = format.frameToString(window.start, in.formatType);
        in.windowEndText = format.frameToString(window.end, in.formatType);
        in.windowStartTextWidth = metrics.horizontalAdvance(in.windowStartText);
        in.windowEndTextWidth = metrics.horizontalAdvance(in.windowEndText);
        in.windowMergedTextWidth = metrics.horizontalAdvance(
            in.windowStartText + QString::fromUtf8("\u2013") + in.windowEndText
        );
    }

    // The timeline's own axis: the ladder's integer positions, offset by the margin that
    // the keys, the playhead and the range markers share.
    frameRuler::Axis axis;
    axis.pixelOfFrame = [&scale](int aFrame) { return double(margin + scale.pixelWidth(aFrame)); };
    axis.frameOfPixel = [&scale](int aPixel) { return scale.frame(aPixel - margin); };
    axis.attributeAt = [&scale](int aFrame, double) { return scale.attribute(aFrame); };
    axis.pixelsPerFrame = scale.pixelsPerFrame();
    axis.tickStride = scale.tickStride(scale.pixelsPerFrame());

    const theme::Colors& colors = theme::Colors::current();
    frameRuler::PaintInk ink;
    ink.content = mTimelineTheme.headerContentColor();
    ink.windowBand = colors.windowBand;
    ink.windowEdge = colors.windowEdge;
    ink.windowText = colors.windowText;

    frameRuler::paintRuler(aPainter, frameRuler::build(in, axis), ink);
}

//-------------------------------------------------------------------------------------------------
// Input: the strip's gestures (header seek, shift-drag window) run through
// the editor's cursor machinery, driven by this widget's own AbstractCursor
// — the same updateCursor call the track surface uses, so the controller
// remains the single gesture state machine. The mouse grab keeps the drag
// alive while the cursor leaves the 22px strip. Everything else (MMB pan,
// wheel zoom, right-click menus) propagates to the surface / timeline.
void TimeRuler::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton || !mCamera) {
        aEvent->ignore();
        return;
    }
    // With the MoveCanvas key held, left-drag pans the timeline (like the
    // viewport); the strip yields the press so the pan starts from anywhere.
    if (mPanKeyHeld) {
        aEvent->ignore();
        return;
    }
    bool frameMoved = false;
    if (mAbstractCursor.setMousePress(aEvent, *mCamera)) {
        frameMoved = mSurface.updateCursor(mAbstractCursor, aEvent->modifiers());
    }
    aEvent->accept();
    mGesturing = true;
    grabMouse();
    if (frameMoved)
        emit frameChanged();
}

void TimeRuler::mouseMoveEvent(QMouseEvent* aEvent) {
    if (!mGesturing || !mCamera) {
        aEvent->ignore();
        return;
    }
    bool frameMoved = false;
    if (mAbstractCursor.setMouseMove(aEvent, *mCamera)) {
        frameMoved = mSurface.updateCursor(mAbstractCursor, aEvent->modifiers());
    }
    aEvent->accept();
    if (frameMoved)
        emit frameChanged();
}

void TimeRuler::mouseReleaseEvent(QMouseEvent* aEvent) {
    if (!mGesturing || !mCamera || aEvent->button() != Qt::LeftButton) {
        aEvent->ignore();
        return;
    }
    bool frameMoved = false;
    if (mAbstractCursor.setMouseRelease(aEvent, *mCamera)) {
        frameMoved = mSurface.updateCursor(mAbstractCursor, aEvent->modifiers());
    }
    mGesturing = false;
    releaseMouse();
    aEvent->accept();
    if (frameMoved)
        emit frameChanged();
}

} // namespace gui
