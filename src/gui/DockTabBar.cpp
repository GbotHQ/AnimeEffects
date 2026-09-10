#include "gui/DockTabBar.h"

#include "gui/DockDropOverlay.h"
#include "gui/DockPanel.h"
#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QFile>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMimeData>
#include <QMouseEvent>
#include <QTime>
#include <QWindow>
#include <cstdio>

namespace gui {

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

DockTabBar::DockTabBar(QDockWidget* aDock):
    QTabBar(aDock) {
    // Named so the theme dresses this row and a group's Qt bar with one rule.
    setObjectName(QStringLiteral("dockTabBar"));
    setDrawBase(false);
    setExpanding(false);
    setMovable(false);
    setFocusPolicy(Qt::NoFocus);
    setUsesScrollButtons(false);
    if (aDock) {
        addTab(aDock->windowTitle());
        connect(aDock, &QDockWidget::windowTitleChanged, this, [this](const QString& aTitle) {
            if (count() > 0) setTabText(0, aTitle);
        });
        dockSpikeLog(aDock, QStringLiteral("title tab bar installed: platform=%1 qt=%2")
                .arg(QGuiApplication::platformName(), QString::fromLatin1(qVersion())));
    }
}

DockTabBarFilter::DockTabBarFilter(QTabBar* aBar, QObject* aParent):
    QObject(aParent),
    mBar(aBar) {
    mBar->installEventFilter(this);
}

QDockWidget* DockTabBarFilter::dockForTab(int aIndex) const {
    // A panel's own title row lives inside its dock; a group's bar is the main
    // window's, and its tabs are named after the panels they hold.
    if (auto* dock = qobject_cast<QDockWidget*>(mBar->parentWidget())) return dock;
    if (auto* window = qobject_cast<QMainWindow*>(mBar->window())) {
        const QString title = aIndex >= 0 ? mBar->tabText(aIndex) : QString();
        for (QDockWidget* dock : window->findChildren<QDockWidget*>()) {
            if (!dock->property("aeDockPreview").toBool() && dock->windowTitle() == title)
                return dock;
        }
    }
    return nullptr;
}

bool DockTabBarFilter::eventFilter(QObject* aObject, QEvent* aEvent) {
    if (aObject != mBar) return QObject::eventFilter(aObject, aEvent);

    switch (aEvent->type()) {
    case QEvent::MouseButtonPress: {
        auto* e = static_cast<QMouseEvent*>(aEvent);
        if (e->button() != Qt::LeftButton) break;
        mPressPos = e->position().toPoint();
        mMoving = false;
        const int index = mBar->tabAt(mPressPos);
        // The tab is the handle; the row's empty space past the last tab is the
        // window's, and only a floating panel has a window to move.
        QDockWidget* dock = dockForTab(index);
        const bool floating = dock && dock->isFloating();
        mDockDragArmed = (index >= 0) || (dock && !floating);
        if (index >= 0 && dock) {
            log(QStringLiteral("tab press pos=%1,%2 tab=%3 (%4) floating=%5 -> dock drag armed")
                    .arg(mPressPos.x()).arg(mPressPos.y()).arg(index).arg(dock->objectName())
                    .arg(floating));
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* e = static_cast<QMouseEvent*>(aEvent);
        if (!(e->buttons() & Qt::LeftButton)) break;
        const QPoint pos = e->position().toPoint();
        if ((pos - mPressPos).manhattanLength() < QApplication::startDragDistance()) break;

        QDockWidget* dock = dockForTab(mBar->tabAt(mPressPos));
        if (!dock) break;
        if (mDockDragArmed && !mDragging) {
            // Deferred by one event-loop turn, not started inside the filter:
            // a QDrag begun from within another widget's event handling never
            // tracked the pointer (measured: one drag-enter, no drag-moves, then
            // action=0 - the drop was never offered to anything). Handing the
            // drag its own turn is what a widget's own mouseMoveEvent would have
            // had.
            mDockDragArmed = false;
            mDragging = true;
            QMetaObject::invokeMethod(this, [this, dock]() {
                beginDockPanelDrag(dock, mBar);
                mDragging = false;
            }, Qt::QueuedConnection);
            return true;
        }
        if (dock->isFloating() && !mMoving) {
            QWindow* window = dock->windowHandle();
            mMoving = window && window->startSystemMove();
            log(QStringLiteral("title bar drag -> startSystemMove=%1").arg(mMoving));
            if (mMoving) return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        mDockDragArmed = false;
        mMoving = false;
        break;
    case QEvent::MouseButtonDblClick: {
        auto* e = static_cast<QMouseEvent*>(aEvent);
        if (e->button() != Qt::LeftButton) break;
        if (QDockWidget* dock = dockForTab(mBar->tabAt(e->position().toPoint()))) {
            const bool floating = !dock->isFloating();
            log(QStringLiteral("double-click -> setFloating(%1)").arg(floating));
            if (auto* panel = qobject_cast<DockPanel*>(dock)) panel->setPanelFloating(floating);
            else dock->setFloating(floating);
            return true;
        }
        break;
    }
    case QEvent::ContextMenu: {
        auto* e = static_cast<QContextMenuEvent*>(aEvent);
        const int index = mBar->tabAt(e->pos());
        if (QDockWidget* dock = dockForTab(index >= 0 ? index : 0)) {
            showDockPanelMenu(dock, e->globalPos());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(aObject, aEvent);
}

void DockTabBarFilter::log(const QString& aMessage) const {
    dockSpikeLog(dockForTab(mBar->tabAt(mPressPos)), aMessage);
}

} // namespace gui
