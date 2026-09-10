#include "gui/DockTitleBar.h"

#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QFile>
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
/// whether the platform accepted the move/resize, and what the floating
/// window's flags/frame margins actually are. Written to stderr and to a file
/// so it can be read back without a terminal attached.
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
    mDock(aDock),
    mPressPos(0, 0),
    mPressed(false),
    mGestureStarted(false) {
    setObjectName(QStringLiteral("DockTitleBar"));
    setMouseTracking(true);
    // The header is the whole grab surface of a floating window, so it must
    // not let the QSS fill it: everything painted here is painted by hand.
    setAttribute(Qt::WA_TranslucentBackground);

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

    // The title text replaces the QSS `QDockWidget::title` box, which Qt no
    // longer draws once a title bar widget exists. The dock carries the bold
    // dock font, so this text is bold without asking.
    p.setPen(c.text);
    p.drawText(rect().adjusted(titleInset(fontMetrics()), 0, -titleInset(fontMetrics()), 0),
               Qt::AlignVCenter | Qt::AlignLeft, mDock->windowTitle());
}

void DockTitleBar::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(aEvent);
        return;
    }

    mPressPos = aEvent->position().toPoint();
    mPressed = true;
    mGestureStarted = false;

    // Consume the press. Qt starts its dock drag from a press that reaches the
    // QDockWidget itself (QDockWidgetPrivate::mousePressEvent only accepts
    // presses inside the title area), and on Wayland that drag becomes a
    // platform drag-and-drop. Accepting here is what keeps the gesture ours.
    aEvent->accept();
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* aEvent) {
    if (!mPressed || mGestureStarted) {
        QWidget::mouseMoveEvent(aEvent);
        return;
    }
    if ((aEvent->position().toPoint() - mPressPos).manhattanLength()
            < QApplication::startDragDistance()) {
        return;
    }
    mGestureStarted = true;

    QWindow* window = mDock->windowHandle();
    if (mDock->isFloating()) {
        const bool accepted = window && window->startSystemMove();
        log(QStringLiteral("floating header-drag startSystemMove=%1 flags=0x%2")
                .arg(accepted).arg(window ? int(window->flags()) : 0, 0, 16));
    } else {
        // Phase 3 puts the drop-zone overlay here. Until then the value of this
        // line is the negative result: the panel must stay put and in the
        // layout, i.e. Qt's drag and its platform DnD never started.
        auto* mainWindow = qobject_cast<QMainWindow*>(mDock->parentWidget());
        log(QStringLiteral("docked header-drag -> ours: area=%1 geometry=%2,%3 %4x%5")
                .arg(mainWindow ? int(mainWindow->dockWidgetArea(mDock)) : -1)
                .arg(mDock->geometry().x()).arg(mDock->geometry().y())
                .arg(mDock->width()).arg(mDock->height()));
    }
    aEvent->accept();
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent* aEvent) {
    mPressed = false;
    mGestureStarted = false;
    QWidget::mouseReleaseEvent(aEvent);
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent* aEvent) {
    if (aEvent->button() == Qt::LeftButton) {
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
    QMenu menu;
    const bool floating = mDock->isFloating();
    QAction* floatAction = menu.addAction(floating ? tr("Dock panel") : tr("Undock panel"));
    QAction* hideAction = menu.addAction(tr("Hide panel"));

    QAction* chosen = menu.exec(aGlobalPos);
    if (!chosen) return;

    if (chosen == floatAction) {
        toggleFloating();
    } else if (chosen == hideAction) {
        log(QStringLiteral("hide panel"));
        mDock->hide();
    }
}

void DockTitleBar::toggleFloating() {
    const bool floating = !mDock->isFloating();
    log(QStringLiteral("double-click/undo -> setFloating(%1)").arg(floating));
    mDock->setFloating(floating);
}

void DockTitleBar::log(const QString& aMessage) const {
    dockSpikeLog(mDock, aMessage);
}

} // namespace gui
