#include "gui/DockDropOverlay.h"

#include "gui/DockMetrics.h"
#include "gui/theme/Colors.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMainWindow>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>

namespace gui {

namespace {

/// A group's middle (this far in from every edge) is its title area: dropping
/// there stacks the panel as a tab.
constexpr double kGroupMiddleBand = 0.3;

Qt::Edge nearestEdge(const QRect& aRect, const QPoint& aPos) {
    const int left = aPos.x() - aRect.left();
    const int right = aRect.right() - aPos.x();
    const int top = aPos.y() - aRect.top();
    const int bottom = aRect.bottom() - aPos.y();
    const int smallest = qMin(qMin(left, right), qMin(top, bottom));
    if (smallest == left) return Qt::LeftEdge;
    if (smallest == right) return Qt::RightEdge;
    if (smallest == top) return Qt::TopEdge;
    return Qt::BottomEdge;
}

Qt::DockWidgetArea areaForEdge(Qt::Edge aEdge) {
    switch (aEdge) {
    case Qt::LeftEdge: return Qt::LeftDockWidgetArea;
    case Qt::RightEdge: return Qt::RightDockWidgetArea;
    case Qt::TopEdge: return Qt::TopDockWidgetArea;
    default: return Qt::BottomDockWidgetArea;
    }
}

/// The half of aRect the panel would take if the group were split towards
/// aEdge, which is also what the preview draws.
QRect halfTowards(const QRect& aRect, Qt::Edge aEdge) {
    QRect half = aRect;
    switch (aEdge) {
    case Qt::LeftEdge:
        half.setWidth(aRect.width() / 2);
        break;
    case Qt::RightEdge:
        half.setLeft(aRect.left() + aRect.width() / 2);
        break;
    case Qt::TopEdge:
        half.setHeight(aRect.height() / 2);
        break;
    default:
        half.setTop(aRect.top() + aRect.height() / 2);
        break;
    }
    return half;
}

/// A split that is worth doing: a band narrower than this is not a group, it is
/// a sliver, and the panel dropped there would be unusable.
constexpr int kMinSplitExtent = 2 * dockMetrics::kCollapseWidth;

} // namespace

const char* DockDropOverlay::mimeType() {
    return "application/x-animeeffects-dock-panel";
}

DockDropOverlay::DockDropOverlay(QMainWindow* aWindow):
    QWidget(aWindow),
    mWindow(aWindow) {
    setObjectName(QStringLiteral("DockDropOverlay"));
    // Paints only the preview: everything else has to stay the window's own
    // pixels, so the overlay is translucent and never erases.
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAcceptDrops(true);
    hide();
}

DockDropOverlay* DockDropOverlay::forWidget(QWidget* aWidget) {
    if (!aWidget) return nullptr;
    auto* window = qobject_cast<QMainWindow*>(aWidget->window());
    return window ? window->findChild<DockDropOverlay*>() : nullptr;
}

void DockDropOverlay::beginDrag(QDockWidget* aPanel) {
    mPanel = aPanel;
    mTarget = DockDropTarget();
    if (mWindow) setGeometry(mWindow->rect());
    raise();
    show();
}

void DockDropOverlay::endDrag() {
    mPanel = nullptr;
    mTarget = DockDropTarget();
    hide();
}

QRect DockDropOverlay::groupRect(QDockWidget* aDock) const {
    QRect rect = aDock->geometry();
    if (!mWindow) return rect;
    // A tab group is one area: its members are stacked, so the group's rect is
    // whichever of them currently holds the space (the visible one), and the
    // hidden ones may sit anywhere.
    for (QDockWidget* sibling : mWindow->tabifiedDockWidgets(aDock)) {
        if (sibling->isVisible()) rect = rect.united(sibling->geometry());
    }
    return rect;
}

int DockDropOverlay::bandExtent(Qt::Orientation aAxis, int aLimit) const {
    // What the panel would take along this axis: QDockWidget::sizeHint(), which
    // is the size Qt gives a docked panel. Its *floating* size is not that - a
    // floating property panel is 800 tall, and the preview would then cover the
    // whole canvas and say nothing.
    const QSize hint = mPanel ? mPanel->sizeHint() : QSize(dockMetrics::kMinWidth, dockMetrics::kMinHeight);
    const int own = (aAxis == Qt::Horizontal) ? hint.width() : hint.height();
    const int floor = (aAxis == Qt::Horizontal) ? dockMetrics::kMinWidth : dockMetrics::kMinHeight;
    const int ceiling = (aAxis == Qt::Horizontal) ? dockMetrics::kMaxWidth : dockMetrics::kMaxHeight;
    return qBound(floor, own, qMin(ceiling, aLimit));
}

DockDropTarget DockDropOverlay::resolve(const QPoint& aPos) const {
    DockDropTarget target;
    if (!mWindow) return target;

    // Docked panels first: a group is a sibling of the central widget at the
    // window's edge, so it wins where they meet.
    for (QDockWidget* dock : mWindow->findChildren<QDockWidget*>()) {
        if (dock == mPanel || !dock->isVisible() || dock->isFloating()) continue;
        const QRect group = groupRect(dock);
        if (group.isEmpty() || !group.contains(aPos)) continue;

        const Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        const int bandX = qMax(1, int(group.width() * kGroupMiddleBand));
        const int bandY = qMax(1, int(group.height() * kGroupMiddleBand));
        const QRect middle = group.adjusted(bandX, bandY, -bandX, -bandY);

        // A group of tabs can only take another tab: splitDockWidget on a
        // tabified dock adds a tab anyway (Qt says so), so offering a split
        // there would be a preview that lies.
        const bool tabbed = !mWindow->tabifiedDockWidgets(dock).isEmpty();
        if (tabbed || middle.contains(aPos)) {
            target.kind = DockDropKind::Tabify;
            target.area = area;
            target.group = dock;
            target.rect = group;
            return target;
        }

        const Qt::Edge edge = nearestEdge(group, aPos);
        const QRect half = halfTowards(group, edge);
        const bool horizontalSplit = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
        // dividing a group this thin would leave two slivers rather than two
        // panels; the whole group is then the only sensible target
        if ((horizontalSplit ? group.width() : group.height()) < kMinSplitExtent) {
            target.kind = DockDropKind::Tabify;
            target.area = area;
            target.group = dock;
            target.rect = group;
            return target;
        }
        target.kind = DockDropKind::Split;
        target.area = area;
        target.group = dock;
        target.rect = half;
        return target;
    }

    // Then the canvas: which side of it the pointer is on is which area the
    // panel takes. Docks surround the central widget, so there is no such thing
    // as dropping a panel onto the canvas itself.
    QWidget* central = mWindow->centralWidget();
    if (!central) return target;
    const QRect canvas = central->geometry();
    if (canvas.isEmpty()) return target;
    // Anything left over inside the window belongs to the canvas by proximity.
    // The layout leaves a few pixels between an area and the central widget
    // (measured: six), and a dead strip a few pixels wide, running the full
    // height of the window, is a hole to drop into by accident. Outside the
    // window there is no target at all, which is what "keep floating" means.
    if (!mWindow->rect().contains(aPos)) return target;
    const QPoint clamped(qBound(canvas.left(), aPos.x(), canvas.right()),
                         qBound(canvas.top(), aPos.y(), canvas.bottom()));

    // The nearest side wins everywhere on the canvas, so there is no dead
    // middle to drop into by accident and no tie to arbitrate beyond which edge
    // was fractionally closer.
    const Qt::Edge edge = nearestEdge(canvas, clamped);

    target.kind = DockDropKind::Area;
    target.area = areaForEdge(edge);
    switch (edge) {
    case Qt::LeftEdge:
        target.rect = QRect(canvas.left(), canvas.top(),
                            bandExtent(Qt::Horizontal, canvas.width()), canvas.height());
        break;
    case Qt::RightEdge:
        target.rect = QRect(canvas.right() + 1 - bandExtent(Qt::Horizontal, canvas.width()),
                            canvas.top(), bandExtent(Qt::Horizontal, canvas.width()),
                            canvas.height());
        break;
    case Qt::TopEdge:
        target.rect = QRect(canvas.left(), canvas.top(), canvas.width(),
                            bandExtent(Qt::Vertical, canvas.height()));
        break;
    default:
        target.rect = QRect(canvas.left(), canvas.bottom() + 1 - bandExtent(Qt::Vertical, canvas.height()),
                            canvas.width(), bandExtent(Qt::Vertical, canvas.height()));
        break;
    }
    return target;
}

DockDropTarget DockDropOverlay::hover(const QPoint& aPos) {
    mTarget = resolve(aPos);
    update();
    return mTarget;
}

QDockWidget* DockDropOverlay::panelFromMime(const QMimeData* aData) const {
    if (!aData || !mWindow) return nullptr;
    const QByteArray name = aData->data(QString::fromLatin1(mimeType()));
    if (name.isEmpty()) return nullptr;
    for (QDockWidget* dock : mWindow->findChildren<QDockWidget*>()) {
        if (dock->objectName() == QString::fromUtf8(name)) return dock;
    }
    return nullptr;
}

void DockDropOverlay::paintEvent(QPaintEvent*) {
    if (mTarget.kind == DockDropKind::None || mTarget.rect.isEmpty()) return;

    const theme::Colors c = theme::Colors::current();
    QColor fill = c.accent;
    fill.setAlpha(70);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(mTarget.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(rect, 4, 4);
    p.fillPath(path, fill);
    p.setPen(QPen(c.accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void DockDropOverlay::resizeEvent(QResizeEvent* aEvent) {
    QWidget::resizeEvent(aEvent);
    if (mWindow) setGeometry(mWindow->rect());
}

void DockDropOverlay::dragEnterEvent(QDragEnterEvent* aEvent) {
    if (!aEvent->mimeData()->hasFormat(QString::fromLatin1(mimeType()))) {
        aEvent->ignore();
        return;
    }
    if (QDockWidget* panel = panelFromMime(aEvent->mimeData())) mPanel = panel;
    mTarget = resolve(aEvent->position().toPoint());
    update();
    aEvent->acceptProposedAction();
}

void DockDropOverlay::dragMoveEvent(QDragMoveEvent* aEvent) {
    if (!aEvent->mimeData()->hasFormat(QString::fromLatin1(mimeType()))) {
        aEvent->ignore();
        return;
    }
    mTarget = resolve(aEvent->position().toPoint());
    update();
    aEvent->acceptProposedAction();
}

void DockDropOverlay::dragLeaveEvent(QDragLeaveEvent* aEvent) {
    // This is the path Qt's rubber band can miss, leaving its preview on screen
    // after the drag has gone: the preview is dropped here unconditionally.
    mTarget = DockDropTarget();
    update();
    aEvent->accept();
}

void DockDropOverlay::dropEvent(QDropEvent* aEvent) {
    if (!aEvent->mimeData()->hasFormat(QString::fromLatin1(mimeType()))) {
        aEvent->ignore();
        return;
    }
    if (QDockWidget* panel = panelFromMime(aEvent->mimeData())) mPanel = panel;
    const DockDropTarget target = resolve(aEvent->position().toPoint());
    mTarget = DockDropTarget();
    update();
    aEvent->acceptProposedAction();
    if (mPanel && target.kind != DockDropKind::None && onDropped) onDropped(mPanel, target);
}

} // namespace gui
