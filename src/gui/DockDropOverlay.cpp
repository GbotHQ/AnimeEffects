#include "gui/DockDropOverlay.h"

#include "gui/DockMetrics.h"
#include "gui/DockPanel.h"
#include "gui/DockTitleBar.h"
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

/// Whether a group is big enough to be divided: both halves have to clear what
/// the area allows. A 200px sidebar split down the middle is two 100px columns,
/// both under the side minimum, so the offer there is a tab instead - which is
/// also what the drop would amount to.
bool canSplit(const QRect& aGroup, Qt::Edge aEdge, Qt::DockWidgetArea aArea) {
    const bool horizontal = (aEdge == Qt::LeftEdge || aEdge == Qt::RightEdge);
    const bool sideArea = (aArea == Qt::LeftDockWidgetArea || aArea == Qt::RightDockWidgetArea);
    const int needed = horizontal ? 2 * dockMetrics::kMinWidth
                                  : (sideArea ? 2 * dockMetrics::kCollapseHeight
                                              : 2 * dockMetrics::kMinHeight);
    const int available = horizontal ? aGroup.width() : aGroup.height();
    return available >= needed;
}

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

DockDropOverlay* DockDropOverlay::forPanel(QDockWidget* aPanel) {
    if (!aPanel) return nullptr;
    // The panel's own window chain is not the way there: a floating panel *is*
    // a toplevel window, so window() returns the panel. The panel remembers the
    // window it docks into instead (DockPanel::setMainWindow), which is what
    // makes a drag that starts on a floating panel able to find its drop target
    // at all - without this the drag ran with nothing accepting it, and every
    // drop ended as IgnoreAction.
    if (auto* panel = qobject_cast<DockPanel*>(aPanel)) {
        if (QMainWindow* window = panel->mainWindow())
            return window->findChild<DockDropOverlay*>();
    }
    return forWidget(aPanel);
}

void DockDropOverlay::beginDrag(QDockWidget* aPanel) {
    mPanel = aPanel;
    mTarget = DockDropTarget();
    if (mWindow) setGeometry(mWindow->rect());
    raise();
    show();
    dockSpikeLog(aPanel, QStringLiteral("overlay shown for drag: cover=%1x%2 visible=%3")
            .arg(width()).arg(height()).arg(isVisible()));
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

/// The slice of an area a drop will take: on the canvas side of whatever is
/// already in that area, sized like the panel. This mirrors
/// MainWindow::applyDockDrop exactly - it splits the area's innermost dock - so
/// that the rectangle drawn here is the rectangle the panel lands in.
QRect DockDropOverlay::areaSlice(Qt::DockWidgetArea aArea, const QRect& aCanvas) const {
    const int w = bandExtent(Qt::Horizontal, aCanvas.width());
    const int h = bandExtent(Qt::Vertical, aCanvas.height());
    const QRect window = mWindow ? mWindow->rect() : QRect();

    QRect occupied;
    for (QDockWidget* dock : mWindow->findChildren<QDockWidget*>()) {
        if (dock == mPanel || !dock->isVisible() || dock->isFloating()) continue;
        if (mWindow->dockWidgetArea(dock) != aArea) continue;
        const QRect group = groupRect(dock);
        occupied = occupied.isNull() ? group : occupied.united(group);
    }

    // With docks already in the area the panel goes beside the one nearest the
    // canvas (the split anchor). With the area empty it *is* the area, so it
    // starts at the window's edge on a side - measured: a canvas-top drop landed
    // at y=23, the client area's top, with the canvas pushed down by its height.
    switch (aArea) {
    case Qt::LeftDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.right() + 1, occupied.top(), w, occupied.height());
        return QRect(window.left(), aCanvas.top(), w, aCanvas.height());
    case Qt::RightDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.left() - w, occupied.top(), w, occupied.height());
        return QRect(window.right() + 1 - w, aCanvas.top(), w, aCanvas.height());
    case Qt::TopDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.left(), occupied.bottom() + 1, occupied.width(), h);
        return QRect(window.left(), aCanvas.top(), window.width(), h);
    default:
        if (!occupied.isNull())
            return QRect(occupied.left(), occupied.top() - h, occupied.width(), h);
        return QRect(window.left(), aCanvas.bottom() + 1 - h, window.width(), h);
    }
}

int DockDropOverlay::bandExtent(Qt::Orientation aAxis, int aLimit) const {
    // What the panel would take along this axis: QDockWidget::sizeHint(), which
    // is the size Qt gives a docked panel. Its *floating* size is not that - a
    // floating property panel is 800 tall, and the preview would then cover the
    // whole canvas and say nothing.
    QSize hint = mPanel ? mPanel->sizeHint() : QSize(dockMetrics::kMinWidth, dockMetrics::kMinHeight);
    // A floating panel's hint carries its frame: the ring is a QSS border on the
    // window, so it is inside the hint but will not be there once the panel is
    // docked. Without this the preview was exactly one frame too big on both
    // axes, which for the property panel is 20px of lie in each direction.
    if (mPanel && mPanel->isFloating())
        hint -= QSize(2 * dockMetrics::kFrameInset, 2 * dockMetrics::kFrameInset);
    const int own = (aAxis == Qt::Horizontal) ? hint.width() : hint.height();
    const int floor = (aAxis == Qt::Horizontal) ? dockMetrics::kMinWidth : dockMetrics::kMinHeight;
    const int ceiling = (aAxis == Qt::Horizontal) ? dockMetrics::kMaxWidth : dockMetrics::kMaxHeight;
    return qBound(floor, own, qMin(ceiling, aLimit));
}

DockDropTarget DockDropOverlay::resolve(const QPoint& aPos) const {
    DockDropTarget target;
    if (!mWindow) return target;

    // A docked panel dropped back onto itself does nothing. Without this the
    // cursor over the panel's own area falls through to the canvas rule and the
    // preview points at a slice of canvas the pointer is nowhere near, while the
    // drop would be a no-op.
    if (mPanel && !mPanel->isFloating() && mPanel->isVisible() && mPanel->geometry().contains(aPos))
        return target;

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
        if (!canSplit(group, edge, area)) {
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
    target.rect = areaSlice(target.area, canvas);
    return target;
}

DockDropTarget DockDropOverlay::hover(const QPoint& aPos) {
    const DockDropTarget previous = mTarget;
    mTarget = resolve(aPos);
    if (mTarget.kind != previous.kind || mTarget.rect != previous.rect) {
        dockSpikeLog(mPanel, QStringLiteral("overlay target at %1,%2 -> kind=%3 area=%4 rect=%5,%6 %7x%8")
                .arg(aPos.x()).arg(aPos.y()).arg(int(mTarget.kind)).arg(int(mTarget.area))
                .arg(mTarget.rect.x()).arg(mTarget.rect.y())
                .arg(mTarget.rect.width()).arg(mTarget.rect.height()));
    }
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
    dockSpikeLog(mPanel, QStringLiteral("overlay drag enter"));
    hover(aEvent->position().toPoint());
    aEvent->acceptProposedAction();
}

void DockDropOverlay::dragMoveEvent(QDragMoveEvent* aEvent) {
    if (!aEvent->mimeData()->hasFormat(QString::fromLatin1(mimeType()))) {
        aEvent->ignore();
        return;
    }
    hover(aEvent->position().toPoint());
    aEvent->acceptProposedAction();
}

void DockDropOverlay::dragLeaveEvent(QDragLeaveEvent* aEvent) {
    // This is the path Qt's rubber band can miss, leaving its preview on screen
    // after the drag has gone: the preview is dropped here unconditionally.
    mTarget = DockDropTarget();
    update();
    dockSpikeLog(mPanel, QStringLiteral("overlay drag leave: preview cleared"));
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
    dockSpikeLog(mPanel, QStringLiteral("overlay drop kind=%1").arg(int(target.kind)));
    if (mPanel && target.kind != DockDropKind::None && onDropped) onDropped(mPanel, target);
}

} // namespace gui
