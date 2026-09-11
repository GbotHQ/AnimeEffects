#include "gui/DockDropOverlay.h"

#include "gui/DockMetrics.h"
#include "gui/DockPanel.h"
#include "gui/DockSlots.h"
#include "gui/DockTabBar.h"
#include "gui/theme/Colors.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMainWindow>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QTimer>
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

/// Whether a dock is actually occupying space on screen.
///
/// An inactive tab of a group is still "visible" and still reports a size: Qt
/// parks it off-screen (measured: at -820,-656). Trusting that geometry when an
/// area's extent is worked out makes the area enormous - one slot covering the
/// whole window, with its seam drawn somewhere off it - which is what a group
/// looked like after tabbing two panels together.
bool occupiesScreen(const QWidget* aWidget, const QRect& aWindow) {
    if (!aWidget || !aWidget->isVisible()) return false;
    const QRect rect = aWidget->geometry();
    return !rect.isEmpty() && aWindow.intersects(rect);
}

/// The size the panel brings to a slot: a floating panel's hint carries its
/// frame, and the frame is not there once docked.
QSize dockedHint(const QDockWidget* aPanel) {
    if (!aPanel) return QSize(dockMetrics::kMinWidth, dockMetrics::kMinHeight);
    QSize hint = aPanel->sizeHint();
    if (aPanel->isFloating())
        hint -= QSize(2 * dockMetrics::kFrameInset, 2 * dockMetrics::kFrameInset);
    return hint;
}


/// The region an area drop takes, following the rule the placement itself
/// follows: beside the area's innermost dock when the area has one (the split
/// anchor), or the area's own place when it is empty.
///
/// The extents come from the docks that are actually there plus the corner
/// rules, not from an idealised picture of the layout - the corner rules are
/// what the first version missed. The bottom area stops at the right sidebar,
/// because BottomRightCorner belongs to the right area: measured, a preview that
/// said 1305 wide against a dock that landed 965 wide.
QRect DockDropOverlay::targetRect(const DockDropTarget& aTarget, const QRect& aCanvas) const {
    if (!mWindow) return QRect();
    const QSize hint = dockedHint(mPanel);
    const QRect window = mWindow->rect();

    QRect occupied;
    for (QDockWidget* dock : mWindow->findChildren<QDockWidget*>()) {
        if (!dock->isVisible() || dock->isFloating()) continue;
        if (mWindow->dockWidgetArea(dock) != aTarget.area) continue;
        const QRect group = groupRect(dock);
        occupied = occupied.isNull() ? group : occupied.united(group);
    }

    // How much width a side area takes, for the corner arithmetic below.
    const auto sideWidth = [this, &window](Qt::DockWidgetArea aArea) {
        for (QDockWidget* dock : mWindow->findChildren<QDockWidget*>()) {
                if (dock->isFloating()) continue;
            if (mWindow->dockWidgetArea(dock) != aArea) continue;
            if (!occupiesScreen(dock, window)) continue;
            return aArea == Qt::LeftDockWidgetArea ? dock->geometry().right() + 1
                                                   : window.right() + 1 - dock->geometry().left();
        }
        return 0;
    };
    const bool leftOwnsBottom = mWindow->corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea;
    const bool rightOwnsBottom = mWindow->corner(Qt::BottomRightCorner) == Qt::RightDockWidgetArea;
    const bool leftOwnsTop = mWindow->corner(Qt::TopLeftCorner) == Qt::LeftDockWidgetArea;
    const bool rightOwnsTop = mWindow->corner(Qt::TopRightCorner) == Qt::RightDockWidgetArea;

    switch (aTarget.area) {
    case Qt::LeftDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.right() + 1, occupied.top(), hint.width(), occupied.height());
        return QRect(window.left(), aCanvas.top(), hint.width(), aCanvas.height());
    case Qt::RightDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.left() - hint.width(), occupied.top(), hint.width(), occupied.height());
        return QRect(window.right() + 1 - hint.width(), aCanvas.top(), hint.width(), aCanvas.height());
    case Qt::TopDockWidgetArea:
        if (!occupied.isNull())
            return QRect(occupied.left(), occupied.bottom() + 1, occupied.width(), hint.height());
        return QRect(window.left() + (leftOwnsTop ? sideWidth(Qt::LeftDockWidgetArea) : 0),
                     aCanvas.top(),
                     window.width() - (leftOwnsTop ? sideWidth(Qt::LeftDockWidgetArea) : 0)
                                    - (rightOwnsTop ? sideWidth(Qt::RightDockWidgetArea) : 0),
                     hint.height());
    default:
        if (!occupied.isNull())
            return QRect(occupied.left(), occupied.top() - hint.height(), occupied.width(), hint.height());
        return QRect(window.left() + (leftOwnsBottom ? sideWidth(Qt::LeftDockWidgetArea) : 0),
                     aCanvas.bottom() + 1 - hint.height(),
                     window.width() - (leftOwnsBottom ? sideWidth(Qt::LeftDockWidgetArea) : 0)
                                    - (rightOwnsBottom ? sideWidth(Qt::RightDockWidgetArea) : 0),
                     hint.height());
    }
}

void beginDockPanelDrag(QDockWidget* aPanel, QWidget* aSource) {
    if (!aPanel || !aSource) return;
    auto* overlay = DockDropOverlay::forPanel(aPanel);
    if (!overlay)
        dockSpikeLog(aPanel, QStringLiteral("drag: NO drop target found for this panel"));

    auto* drag = new QDrag(aSource);
    auto* mime = new QMimeData();
    mime->setData(QString::fromLatin1(DockDropOverlay::mimeType()), aPanel->objectName().toUtf8());
    drag->setMimeData(mime);

    // The ghost is the panel's own snapshot, scaled: the panel cannot follow the
    // cursor itself (its toplevel is already mapped, and the compositor's
    // toplevel-drag protocol only adopts windows that are not), so the ghost is
    // what makes the gesture read as "this panel is going there".
    const QPixmap shot = aPanel->grab();
    if (!shot.isNull()) {
        const QPixmap ghost = shot.scaledToWidth(qBound(160, shot.width() / 3, 360),
                                                 Qt::SmoothTransformation);
        drag->setPixmap(ghost);
        drag->setHotSpot(QPoint(ghost.width() / 2, qMin(12, ghost.height() / 4)));
    }

    if (overlay) overlay->beginDrag(aPanel);
    dockSpikeLog(aPanel, QStringLiteral("drag started (ghost %1x%2)")
            .arg(drag->pixmap().width()).arg(drag->pixmap().height()));
    const Qt::DropAction result = drag->exec(Qt::MoveAction);
    if (overlay) overlay->endDrag();
    dockSpikeLog(aPanel, QStringLiteral("drag finished action=%1").arg(int(result)));
}

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
        if (occupiesScreen(sibling, mWindow->rect())) rect = rect.united(sibling->geometry());
    }
    return rect;
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

    // Slots first: a slot is a sibling of the central widget along the window's
    // edge, so it wins where they meet.
    //
    // Iterating slots rather than docks is the fix for the worst version of this
    // bug: iterating docks meant asking a *parked* dock (the inactive tab of a
    // group, which Qt keeps off-screen at something like -700,-984) for its rect,
    // and uniting that with its sibling's produced a group rect of 2005x1787 on a
    // 1305x803 window - offered as a tab target for almost everywhere, while the
    // drop landed correctly in the slot. Measured, with the left sidebar emptied
    // by tabbing its panel onto the right one.
    for (const DockSlot& slot : dockSlots(*mWindow)) {
        if (slot.onScreen.isEmpty()) continue;
        // dropping a docked panel back into the slot it already occupies is a
        // no-op, so it is not a target
        if (mPanel && !mPanel->isFloating() && slot.panels.contains(mPanel)) return target;
        if (!slot.rect.contains(aPos)) continue;

        const int bandX = qMax(1, int(slot.rect.width() * kGroupMiddleBand));
        const int bandY = qMax(1, int(slot.rect.height() * kGroupMiddleBand));
        const QRect middle = slot.rect.adjusted(bandX, bandY, -bandX, -bandY);

        // A slot with more than one panel is a rail: it can only take another
        // tab, because splitDockWidget on a tabified dock adds a tab anyway (Qt
        // says so) and a preview that says "split" while the drop tabifies is
        // worse than no preview.
        const bool stacked = slot.panels.size() > 1;
        if (stacked || middle.contains(aPos)) {
            target.kind = DockDropKind::Tabify;
            target.area = slot.area;
            target.group = slot.onScreen.first();
            target.rect = slot.rect;
            return target;
        }

        const Qt::Edge edge = nearestEdge(slot.rect, aPos);
        if (!canSplit(slot.rect, edge, slot.area)) {
            // two halves of a group this small would both be under the area's
            // minimum, so the whole slot is the only sensible target
            target.kind = DockDropKind::Tabify;
            target.area = slot.area;
            target.group = slot.onScreen.first();
            target.rect = slot.rect;
            return target;
        }
        target.kind = DockDropKind::Split;
        target.area = slot.area;
        target.group = slot.onScreen.first();
        target.rect = halfTowards(slot.rect, edge);
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
    // middle to drop into by accident. No rectangle is computed here: for an
    // area drop the preview dock says where the panel lands, because the layout
    // decides it - including the corner rules that a hand-written slice got
    // wrong (measured: a bottom drop previewed full width and landed 965 wide,
    // stopping at the right sidebar).
    target.kind = DockDropKind::Area;
    target.area = areaForEdge(nearestEdge(canvas, clamped));
    target.rect = targetRect(target, canvas);

    // A panel already docked in that area has nowhere to go: the layout would
    // not change, so the target is "nothing" and no preview is shown. Without
    // this, dragging the timeline's own title back towards the bottom area
    // previewed a slot *above* the timeline (the preview dock was split against
    // it) while the drop left everything exactly as it was.
    if (mPanel && !mPanel->isFloating() && mWindow->dockWidgetArea(mPanel) == target.area)
        return DockDropTarget();

    return target;
}

DockDropTarget DockDropOverlay::hover(const QPoint& aPos) {
    const DockDropTarget previous = mTarget;
    mTarget = resolve(aPos);
    if (mTarget.kind != previous.kind || mTarget.area != previous.area
            || mTarget.group != previous.group || mTarget.rect != previous.rect) {
        dockSpikeLog(mPanel, QStringLiteral("overlay target at %1,%2 -> kind=%3 area=%4 group=%5 "
                                           "rect=%6,%7 %8x%9")
                .arg(aPos.x()).arg(aPos.y()).arg(int(mTarget.kind)).arg(int(mTarget.area))
                .arg(mTarget.group ? mTarget.group->objectName() : QStringLiteral("-"))
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
    // The feedback is a region, not a dock. A real dock placed by the layout
    // would be exact by construction - that is how this started - but placing
    // one restructures the window while the drag is in flight, and that ends the
    // drag: measured, the drop stopped being delivered at all (the moves tracked
    // the pointer, then the release produced a leave with action=0), while the
    // same drag drops correctly with no preview placed. So the region is
    // computed, from the rule the placement follows, and checked against the
    // settled geometry.
    if (mTarget.kind == DockDropKind::None || mTarget.rect.isEmpty()) return;

    const theme::Colors c = theme::Colors::current();
    QColor fill = c.accent;
    fill.setAlpha(70);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(mTarget.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(area, 4, 4);
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
    aEvent->acceptProposedAction();
    dockSpikeLog(mPanel, QStringLiteral("overlay drop kind=%1").arg(int(target.kind)));
    if (mPanel && target.kind != DockDropKind::None && onDropped) onDropped(mPanel, target);
}

} // namespace gui
