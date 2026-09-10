#include "gui/DockTitleBar.h"

#include "gui/DockDropOverlay.h"
#include "gui/DockPanel.h"
#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFile>
#include <QGuiApplication>
#include <QMainWindow>
#include <QDrag>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <QWindow>
#include <cstdio>

namespace gui {

namespace {

/// Text inset for the title. It is the panel's own content inset, because the
/// title is that panel's first line of text: the card padding in the theme
/// (0.375em) plus the content layout's margin measures 11px at the default font,
/// where the old 0.5 * font-height put the title at 8px - three pixels left of
/// everything under it, and against the window edge for a left sidebar.
int titleInset(const QFontMetrics& aMetrics) {
    return static_cast<int>(aMetrics.height() * 0.72);
}

/// Padding above and below the text, on top of the font's own height.
constexpr int kVerticalPadding = 8;

} // namespace

/// The point of this change is evidence, not a look: one line per gesture
/// carrying the values the design turns on - which platform plugin is live,
/// what the floating window's flags and frame margins actually are, and which
/// gesture claimed the press. Written to stderr and to a file so it can be read
/// back without a terminal attached.
void dockSpikeLog(const QDockWidget* aDock, const QString& aMessage) {
    static const QString path = qEnvironmentVariable(
            "AE_DOCK_SPIKE_LOG", QStringLiteral("/tmp/ae-dock-spike.log"));
    const QString line = QStringLiteral("%1 [%2] %3")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 aDock ? aDock->objectName() : QStringLiteral("-"), aMessage);
    std::fprintf(stderr, "dock-spike: %s\n", qPrintable(line));
    std::fflush(stderr);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line.toUtf8());
        file.write("\n");
    }
}

DockTitleBar::DockTitleBar(QDockWidget* aDock):
    QWidget(aDock),
    mDock(aDock) {
    setObjectName(QStringLiteral("DockTitleBar"));
    setMouseTracking(true);
    // The header is painted by hand, so the universal QWidget QSS background
    // must not fill it.
    setAttribute(Qt::WA_TranslucentBackground);
    // Opaque to the mouse in both states, deliberately: the press is what
    // decides which gesture this is, and a press that reaches the QDockWidget
    // starts Qt's own dock drag-and-drop instead. Floating, that drag is what
    // used to swallow the move; docked, it is what made panels draggable.
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    dockSpikeLog(mDock, QStringLiteral("titlebar installed: platform=%1 qt=%2")
            .arg(QGuiApplication::platformName(), QString::fromLatin1(qVersion())));
}

/// The tab: the title text plus the padding a tab carries, left-aligned in the
/// header. This is the dock handle while the panel floats - the title area
/// around it moves the window, which is the split the platform forces: a move
/// hands the client no positions and a drag hands it no way to move the window,
/// so one gesture cannot be both.
QRect DockTitleBar::tabRect() const {
    const int inset = titleInset(fontMetrics());
    const int tabWidth = qMin(width() - 2 * inset,
                              fontMetrics().horizontalAdvance(mDock->windowTitle()) + 2 * inset);
    return QRect(inset, 0, qMax(0, tabWidth), height());
}

QSize DockTitleBar::sizeHint() const {
    return QSize(0, qMax(18, fontMetrics().height() + kVerticalPadding));
}

QSize DockTitleBar::minimumSizeHint() const {
    return sizeHint();
}

void DockTitleBar::paintEvent(QPaintEvent*) {
    const theme::Colors c = theme::Colors::current();
    QPainter p(this);

    p.fillRect(rect(), c.base);

    // A floating panel is a window, and this is its title area: the rule under
    // it is what says so, and it is the boundary of the region that will move
    // the window (or, in the docked state, of nothing at all). Docked, the
    // surrounding sashes and tabs already draw that boundary, so a line here
    // would just double the dock separator.
    if (mDock->isFloating()) {
        p.setPen(c.hairline);
        p.drawLine(0, height() - 1, width() - 1, height() - 1);
    }

    // The title text replaces the QSS `QDockWidget::title` box, which Qt no
    // longer draws once a title bar widget exists. The dock carries the bold
    // dock font, so this text is bold without asking.
    //
    // The text is also the dock handle, so it is drawn as the tab it becomes
    // when the panel joins a group: hovered, it takes the same accent wash and
    // focus-coloured rule the theme gives a selected tab - which is the only
    // thing that says this region can be dragged somewhere. Docked, the tab in the
    // group's strip holds that role and this header is inert.
    const QRect tab = mDock->isFloating() ? tabRect() : rect();
    if (mTabHover) {
        QColor wash = c.accent;
        wash.setAlpha(40);
        p.fillRect(tab, wash);
        // the rule goes under the tab, not over it: the tab names the panel
        // below it, and it lands on the header's own bottom rule so the two
        // read as one line
        p.fillRect(QRect(tab.left(), tab.bottom() - 1, tab.width(), 2), c.focus);
    }

    p.setPen(c.text);
    p.drawText(tab, Qt::AlignVCenter | Qt::AlignLeft, mDock->windowTitle());
}

void DockTitleBar::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(aEvent);
        return;
    }

    mPressPos = aEvent->position().toPoint();
    mMoving = false;
    if (mDock->isFloating()) {
        // The window's own title area: either the tab (dock) or the rest of the
        // bar (move the window). The press is claimed here so it cannot reach
        // the dock and start Qt's own dock drag-and-drop instead of this.
        mDockDragArmed = tabRect().contains(mPressPos);
        log(QStringLiteral("floating title-press pos=%1,%2 -> %3 armed")
                .arg(mPressPos.x()).arg(mPressPos.y())
                .arg(mDockDragArmed ? QStringLiteral("dock drag") : QStringLiteral("window move")));
    } else {
        // A docked panel's title is a handle as well: dragging it is how a
        // panel moves to another area, splits one, or stacks as a tab. There is
        // no window to move here, so the whole title is the handle - and the
        // press is still claimed rather than left to reach the dock, so what
        // starts is this drag and not Qt's.
        mDockDragArmed = true;
        log(QStringLiteral("docked title-press pos=%1,%2 -> dock drag armed")
                .arg(mPressPos.x()).arg(mPressPos.y()));
    }
    aEvent->accept();
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* aEvent) {
    const bool dragged = (aEvent->buttons() & Qt::LeftButton)
            && (aEvent->position().toPoint() - mPressPos).manhattanLength()
                    >= QApplication::startDragDistance();

    if (mDockDragArmed && !mDragging && dragged) {
        startDockDrag();
        aEvent->accept();
        return;
    }

    // Moving a floating window is a compositor gesture, not ours to perform:
    // startSystemMove() is the same call a decorated window's title bar makes,
    // and the compositor then owns the pointer until the button is released.
    // The consequence is that the client receives no positions at all for the
    // rest of the gesture - which is exactly why docking cannot ride along on
    // this drag, and needs a gesture (and a drop target) of its own.
    if (mDock->isFloating() && !mMoving && dragged) {
        QWindow* window = mDock->windowHandle();
        mMoving = window && window->startSystemMove();
        log(QStringLiteral("title-drag -> startSystemMove=%1").arg(mMoving));
        if (mMoving) {
            aEvent->accept();
            return;
        }
    }
    // Hover feedback for the handle: the region is washed and the cursor says it
    // is grabbable. Floating that region is the tab; docked it is the whole
    // title, because there is no window there to move and nothing else the bar
    // could mean.
    const bool hover = mDock->isFloating() ? tabRect().contains(aEvent->position().toPoint())
                                           : rect().contains(aEvent->position().toPoint());
    if (hover != mTabHover) {
        mTabHover = hover;
        setCursor(hover ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(aEvent);
}

/// Hand the panel to a drag-and-drop, which is the only gesture on Wayland in
/// which a client is told where the pointer is - and therefore the only one that
/// can know what it is hovering. The image that follows the cursor is the
/// panel's own snapshot, scaled: the panel cannot follow the cursor itself (its
/// toplevel is already mapped, and the compositor's toplevel-drag protocol only
/// adopts windows that are not), so the ghost is what makes the gesture read as
/// "this panel is going there".
void DockTitleBar::startDockDrag() {
    auto* overlay = DockDropOverlay::forPanel(mDock);
    if (!overlay)
        log(QStringLiteral("tab drag: NO drop target found for this panel"));
    // Heap-allocated with a parent, as the documentation requires: setMimeData
    // hands over ownership of the data, and Qt has to be able to clean up after
    // the operation has completed.
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData();
    mime->setData(QString::fromLatin1(DockDropOverlay::mimeType()), mDock->objectName().toUtf8());
    drag->setMimeData(mime);

    const QPixmap shot = mDock->grab();
    if (!shot.isNull()) {
        const QPixmap ghost = shot.scaledToWidth(qBound(160, shot.width() / 3, 360), Qt::SmoothTransformation);
        drag->setPixmap(ghost);
        drag->setHotSpot(QPoint(ghost.width() / 2, qMin(12, ghost.height() / 4)));
    }

    mDragging = true;
    if (overlay) overlay->beginDrag(mDock);
    log(QStringLiteral("tab drag started (ghost %1x%2)")
            .arg(drag->pixmap().width()).arg(drag->pixmap().height()));
    const Qt::DropAction result = drag->exec(Qt::MoveAction);
    if (overlay) overlay->endDrag();
    mDragging = false;
    mDockDragArmed = false;
    log(QStringLiteral("tab drag finished action=%1").arg(int(result)));
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent* aEvent) {
    if (aEvent->button() == Qt::LeftButton) {
        // The same toggle in both states: docked it floats, floating it docks.
        // Qt has its own handler that does this, but it only sees the press
        // while the header lets it through - which it no longer does.
        toggleFloating();
        aEvent->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(aEvent);
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent* aEvent) {
    mDockDragArmed = false;
    mMoving = false;
    QWidget::mouseReleaseEvent(aEvent);
}

void DockTitleBar::contextMenuEvent(QContextMenuEvent* aEvent) {
    popupMenu(aEvent->globalPos());
    aEvent->accept();
}

void DockTitleBar::popupMenu(const QPoint& aGlobalPos) {
    // the docked case: the header is opaque there, so it gets the right-click
    showDockPanelMenu(mDock, aGlobalPos);
}

void DockTitleBar::toggleFloating() {
    const bool floating = !mDock->isFloating();
    log(QStringLiteral("double-click/menu -> setFloating(%1)").arg(floating));
    if (auto* panel = qobject_cast<DockPanel*>(mDock))
        panel->setPanelFloating(floating);
    else
        mDock->setFloating(floating);
}

void DockTitleBar::log(const QString& aMessage) const {
    dockSpikeLog(mDock, aMessage);
}

} // namespace gui
