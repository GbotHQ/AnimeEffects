#include "gui/DockTitleBar.h"

#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QPixmap>
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
    mGripPressed(false),
    mHoveredGrip(false),
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
    const int inset = titleInset(fontMetrics());
    // floating headers carry the grip, so their title starts after it; a docked
    // header has no grip and keeps the old inset
    const int textLeft = mDock->isFloating() ? gripRect().right() + inset / 2 : inset;
    p.setPen(c.text);
    p.drawText(rect().adjusted(textLeft, 0, -inset, 0),
               Qt::AlignVCenter | Qt::AlignLeft, mDock->windowTitle());

    // the grip: two columns of three dots, dimmed until the header is hovered
    if (mDock->isFloating()) {
        const QRect grip = gripRect();
        const QColor dot = mHoveredGrip ? c.text : c.textMuted;
        const int step = qMax(3, grip.height() / 4);
        const int r = qMax(1, step / 3);
        for (int col = 0; col < 2; ++col) {
            for (int row = 0; row < 3; ++row) {
                const QPoint centre(grip.center().x() + (col * 2 - 1) * r * 2,
                                    grip.center().y() + (row - 1) * step);
                p.setPen(Qt::NoPen);
                p.setBrush(dot);
                p.drawEllipse(centre, r, r);
            }
        }
    }
}

QRect DockTitleBar::gripRect() const {
    // comfortably larger than the dots: along with a grip, the header is the
    // dock handle, so a near miss would start the wrong gesture
    const int side = qMax(18, fontMetrics().height());
    const int inset = titleInset(fontMetrics());
    return QRect(inset, (height() - side) / 2, side, side);
}

void DockTitleBar::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(aEvent);
        return;
    }

    mPressPos = aEvent->position().toPoint();
    mPressed = true;
    mGestureStarted = false;
    // floating only: a docked panel is not draggable at all (its header press is
    // consumed below and nothing else happens)
    mGripPressed = mDock->isFloating() && gripRect().contains(mPressPos);

    // Consume the press. Qt starts its dock drag from a press that reaches the
    // QDockWidget itself (QDockWidgetPrivate::mousePressEvent only accepts
    // presses inside the title area), and on Wayland that drag becomes a
    // platform drag-and-drop. Accepting here is what keeps the gesture ours.
    aEvent->accept();
}

void DockTitleBar::mouseMoveEvent(QMouseEvent* aEvent) {
    if (!mPressed) {
        const bool overGrip = mDock->isFloating()
                && gripRect().contains(aEvent->position().toPoint());
        if (overGrip != mHoveredGrip) {
            mHoveredGrip = overGrip;
            setCursor(overGrip ? Qt::SizeAllCursor : Qt::ArrowCursor);
            update();
        }
        QWidget::mouseMoveEvent(aEvent);
        return;
    }
    if (mGestureStarted) {
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
        if (mGripPressed) {
            // the grip is the move handle
            const bool accepted = window && window->startSystemMove();
            log(QStringLiteral("floating grip-drag startSystemMove=%1 flags=0x%2")
                    .arg(accepted).arg(window ? int(window->flags()) : 0, 0, 16));
        } else {
            // the header is the dock handle
            startPanelDrag();
        }
    } else {
        // Phase 3 puts the drop-zone overlay here. Until then the value of this
        // line is the negative result: the panel must stay put and in the
        // layout, i.e. Qt's drag and its platform DnD never started.
        auto* mainWindow = qobject_cast<QMainWindow*>(mDock->parentWidget());
        log(QStringLiteral("docked header-drag -> inert: area=%1 geometry=%2,%3 %4x%5")
                .arg(mainWindow ? int(mainWindow->dockWidgetArea(mDock)) : -1)
                .arg(mDock->geometry().x()).arg(mDock->geometry().y())
                .arg(mDock->width()).arg(mDock->height()));
    }
    aEvent->accept();
}

void DockTitleBar::startPanelDrag() {
    QMimeData* mime = new QMimeData;
    mime->setData(kDockPanelMime, mDock->objectName().toUtf8());

    QDrag drag(this);
    drag.setMimeData(mime);

    // The chip is the header, at a fraction of its width. The panel itself must
    // not be what follows the cursor: on Wayland we cannot position the window
    // ourselves, and even if the compositor moved it, a full-size panel would
    // cover the drop band the overlay is drawing in the main window.
    const int chipWidth = qMin(width(), 200);
    const QPixmap chip = grab(QRect(0, 0, chipWidth, height()));
    drag.setPixmap(chip);
    drag.setHotSpot(QPoint(chip.width() / 2, chip.height() / 2));

    log(QStringLiteral("grip drag started (%1x%2 chip)").arg(chip.width()).arg(chip.height()));
    const Qt::DropAction result = drag.exec(Qt::MoveAction);
    log(QStringLiteral("grip drag finished result=%1").arg(int(result)));
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent* aEvent) {
    mPressed = false;
    mGripPressed = false;
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
