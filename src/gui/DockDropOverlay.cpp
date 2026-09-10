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

/// The preview's body: an accent wash with a 1px accent edge. Everything around
/// it is the real layout - the preview is a real dock in the real dock area -
/// so the wash marks the slot without pretending to be the panel.
class DockPreviewBody: public QWidget {
public:
    DockPreviewBody(QWidget* aParent, const QSize& aHint):
        QWidget(aParent) {
        setMinimumSize(aHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const theme::Colors c = theme::Colors::current();
        QColor fill = c.accent;
        fill.setAlpha(70);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        QPainterPath path;
        path.addRoundedRect(area, 4, 4);
        p.fillPath(path, fill);
        p.setPen(QPen(c.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
};

/// The size the panel would bring to a slot: a floating panel's hint carries its
/// frame, and the frame is not there once docked.
QSize dockedHint(const QDockWidget* aPanel) {
    if (!aPanel) return QSize(dockMetrics::kMinWidth, dockMetrics::kMinHeight);
    QSize hint = aPanel->sizeHint();
    if (aPanel->isFloating())
        hint -= QSize(2 * dockMetrics::kFrameInset, 2 * dockMetrics::kFrameInset);
    return hint;
}

void DockDropOverlay::clearPreview() {
    if (!mPreview || !mWindow) return;
    mWindow->removeDockWidget(mPreview);
    mPreview->hide();
}

void DockDropOverlay::showPreview(const DockDropTarget& aTarget) {
    clearPreview();
    if (!mPreview || !mWindow || !mPanel || aTarget.kind == DockDropKind::None) return;

    if (auto* body = mPreview->widget()) body->setMinimumSize(dockedHint(mPanel));
    mPreview->setMinimumSize(mPanel->minimumSize());
    mPreview->setMaximumSize(mPanel->maximumSize());

    // Un-float first, exactly as a real drop does for the panel: addDockWidget
    // and tabifyDockWidget arrange docks that are in the layout, and a
    // freshly-created dock shown on its own is a floating window (measured: the
    // preview sat at 0,0 with its own size instead of taking the slot).
    mPreview->setFloating(false);
    if (onPlace) onPlace(mPreview, aTarget);
    mPreview->show();

    // Read after the layout has run: immediately after placement the preview
    // still carries its pre-placement geometry, which is what made this log look
    // like the preview was not being placed at all.
    QPointer<QDockWidget> preview = mPreview;
    const DockDropTarget target = aTarget;
    QTimer::singleShot(0, this, [this, preview, target]() {
        if (!preview) return;
        dockSpikeLog(mPanel, QStringLiteral("preview placed kind=%1 area=%2 at %3,%4 %5x%6")
                .arg(int(target.kind)).arg(int(target.area))
                .arg(preview->x()).arg(preview->y())
                .arg(preview->width()).arg(preview->height()));
    });
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

    if (!mPreview && mWindow) {
        mPreview = new QDockWidget(mWindow);
        mPreview->setObjectName(QStringLiteral("aeDockPreview"));
        // Marked so every pass that walks the docks can skip it: it is a place,
        // not a panel.
        mPreview->setProperty("aeDockPreview", true);
        mPreview->setWindowTitle(aPanel ? aPanel->windowTitle() : QString());
        // No title row of its own: the point of the preview is the space. A tab
        // it joins is labelled by the window title set here.
        auto* bare = new QWidget(mPreview);
        bare->setFixedHeight(0);
        mPreview->setTitleBarWidget(bare);
        mPreview->setWidget(new DockPreviewBody(mPreview, dockedHint(aPanel)));
        mPreview->hide();
    }
    raise();
    show();
    dockSpikeLog(aPanel, QStringLiteral("overlay shown for drag: cover=%1x%2 visible=%3")
            .arg(width()).arg(height()).arg(isVisible()));
}

void DockDropOverlay::endDrag() {
    clearPreview();
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
        if (dock == mPanel || dock == mPreview || !dock->isVisible() || dock->isFloating()) continue;
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
        if (!canSplit(group, edge, area)) {
            // two halves of a group this small would both be under the area's
            // minimum, so the whole group is the only sensible target
            target.kind = DockDropKind::Tabify;
            target.area = area;
            target.group = dock;
            target.rect = group;
            return target;
        }
        target.kind = DockDropKind::Split;
        target.area = area;
        target.group = dock;
        target.rect = halfTowards(group, edge);
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
        dockSpikeLog(mPanel, QStringLiteral("overlay target at %1,%2 -> kind=%3 area=%4 group=%5")
                .arg(aPos.x()).arg(aPos.y()).arg(int(mTarget.kind)).arg(int(mTarget.area))
                .arg(mTarget.group ? mTarget.group->objectName() : QStringLiteral("-")));
        showPreview(mTarget);
    }
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
    // Nothing to paint: the feedback is the preview dock, which the layout
    // placed. This widget is here to be the drop target, and to know when to
    // take the preview away again.
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
    clearPreview();
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
    clearPreview();
    aEvent->acceptProposedAction();
    dockSpikeLog(mPanel, QStringLiteral("overlay drop kind=%1").arg(int(target.kind)));
    if (mPanel && target.kind != DockDropKind::None && onDropped) onDropped(mPanel, target);
}

} // namespace gui
