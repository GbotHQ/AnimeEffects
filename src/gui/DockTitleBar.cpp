#include "gui/DockTitleBar.h"

#include "gui/DockPanel.h"
#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFile>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <QWindow>
#include <cstdio>

namespace gui {

namespace {

/// Text inset, matching the 0.375em/0.5em the QSS title rule used.
int titleInset(const QFontMetrics& aMetrics) {
    return static_cast<int>(aMetrics.height() * 0.5);
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
    const int inset = titleInset(fontMetrics());
    p.setPen(c.text);
    p.drawText(rect().adjusted(inset, 0, -inset, 0),
               Qt::AlignVCenter | Qt::AlignLeft, mDock->windowTitle());
}

void DockTitleBar::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(aEvent);
        return;
    }

    if (mDock->isFloating()) {
        // The window's own title area: remember where the drag started and
        // claim the press, so it cannot reach the dock and start Qt's
        // dock drag-and-drop in place of the move.
        mPressPos = aEvent->position().toPoint();
        mMoving = false;
        log(QStringLiteral("floating title-press pos=%1,%2 -> move armed")
                .arg(mPressPos.x()).arg(mPressPos.y()));
        aEvent->accept();
        return;
    }

    // Docked: consume it. A docked panel is not draggable, and this is what
    // keeps Qt's own drag (and its Wayland platform DnD) from starting.
    log(QStringLiteral("docked header-press -> inert"));
    aEvent->accept();
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* aEvent) {
    // Moving a floating window is a compositor gesture, not ours to perform:
    // startSystemMove() is the same call a decorated window's title bar makes,
    // and the compositor then owns the pointer until the button is released.
    // The consequence is that the client receives no positions at all for the
    // rest of the gesture - which is exactly why docking cannot ride along on
    // this drag, and needs a gesture (and a drop target) of its own.
    if (mDock->isFloating() && !mMoving && (aEvent->buttons() & Qt::LeftButton)
            && (aEvent->position().toPoint() - mPressPos).manhattanLength()
                    >= QApplication::startDragDistance()) {
        QWindow* window = mDock->windowHandle();
        mMoving = window && window->startSystemMove();
        log(QStringLiteral("title-drag -> startSystemMove=%1").arg(mMoving));
        if (mMoving) {
            aEvent->accept();
            return;
        }
    }
    QWidget::mouseMoveEvent(aEvent);
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
