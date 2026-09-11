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

namespace {
/// Whether this row's panel shares its slot, in which case the slot's rail is the
/// title UI and this row has no business taking space.
bool rowIsRedundant(QDockWidget* aDock) {
    if (!aDock || aDock->isFloating()) return false;
    auto* panel = qobject_cast<DockPanel*>(aDock);
    return panel && panel->isTabGrouped();
}
} // namespace

QSize DockTabBar::sizeHint() const {
    if (rowIsRedundant(qobject_cast<QDockWidget*>(parentWidget()))) return QSize(0, 0);
    return QTabBar::sizeHint();
}

QSize DockTabBar::minimumSizeHint() const {
    if (rowIsRedundant(qobject_cast<QDockWidget*>(parentWidget()))) return QSize(0, 0);
    return QTabBar::minimumSizeHint();
}

DockTabBarFilter::DockTabBarFilter(QTabBar* aBar, QObject* aParent):
    QObject(aParent),
    mBar(aBar) {
    if (mBar) mBar->installEventFilter(this);
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

bool DockTabBarFilter::isTitleRow() const {
    return mBar && qobject_cast<QDockWidget*>(mBar->parentWidget()) != nullptr;
}

QRect DockTabBarFilter::railRect() const {
    // The rail is the bar itself, plus a couple of pixels of slack so that
    // nudging a tab a hair past the row's edge does not start docking.
    return mBar->rect().adjusted(-2, -2, 2, 2);
}

void DockTabBarFilter::commitRailOrder() {
    auto* window = mBar ? qobject_cast<QMainWindow*>(mBar->window()) : nullptr;
    if (!window || mBar->count() < 2) return;

    // The group in rail order, as docks.
    QVector<QDockWidget*> order;
    for (int i = 0; i < mBar->count(); ++i) {
        if (QDockWidget* dock = dockForTab(i)) order.push_back(dock);
    }
    if (order.size() < 2) return;

    // tabifyDockWidget puts a dock at the *end* of the rail, whatever the anchor
    // (measured: tools<-properties gave Tools|Properties, tools<-animation gave
    // Tools|Properties|Animation, animation<-tools gave Properties|Animation|Tools).
    // So the wanted order is built from the back: move the last wanted tab to the
    // end, then the one before it, and so on - each lands in front of the ones
    // already placed. Moving them front-to-back instead puts the first wanted tab
    // at the end and undoes the lot.
    for (int i = order.size() - 1; i >= 1; --i)
        window->tabifyDockWidget(order.at(0), order.at(i));
    // The dragged tab is the one the user was working on and the one Qt activated
    // on press, so it stays the visible panel: re-tabifying raises whatever it
    // moved last, which would otherwise flip the group to another panel on release.
    if (mPressed) mPressed->raise();

    QStringList rail;
    for (int i = 0; i < mBar->count(); ++i) rail << mBar->tabText(i);
    log(QStringLiteral("rail order committed: %1").arg(rail.join(QStringLiteral(" | "))));
}

bool DockTabBarFilter::eventFilter(QObject* aObject, QEvent* aEvent) {
    if (!mBar || aObject != mBar) return QObject::eventFilter(aObject, aEvent);

    switch (aEvent->type()) {
    case QEvent::MouseButtonPress: {
        auto* e = static_cast<QMouseEvent*>(aEvent);
        if (e->button() != Qt::LeftButton) break;
        mPressPos = e->position().toPoint();
        mMoving = false;
        mRailDrag = false;
        mDockDragArmed = false;
        mPressed = nullptr;

        const int index = mBar->tabAt(mPressPos);
        QDockWidget* dock = dockForTab(index);
        if (index >= 0 && dock) {
            // A press on a tab is a rail drag first: inside the rail it moves the
            // tab, and docking starts only when the pointer leaves the rail (see
            // MouseMove). Starting the dock drag on the first few pixels - which
            // is what this did - left no way to reorder a tab at all.
            mPressed = dock;
            mRailDrag = true;
            mTabIndex = index;
            mMovedTab = false;
            log(QStringLiteral("tab press pos=%1,%2 tab=%3 (%4) floating=%5 -> rail drag")
                    .arg(mPressPos.x()).arg(mPressPos.y()).arg(index).arg(dock->objectName())
                    .arg(dock->isFloating()));
        } else if (dock && !dock->isFloating() && isTitleRow()) {
            // The rest of a lone panel's title row, which is all of it but the tab
            // text: one tab reads as the title, so the row is the panel's handle
            // and not a tab strip with dead space in it. Measured before this: the
            // only interactive part was tabRect(0), local x 8..78, so 65% of the
            // tools title row and 93% of the animation one did nothing on a drag -
            // which is "dragging the tabs does not initiate docking any more".
            mPressed = dock;
            mDockDragArmed = true;
            log(QStringLiteral("title row press pos=%1,%2 -> dock drag armed")
                    .arg(mPressPos.x()).arg(mPressPos.y()));
        } else if (dock && dock->isFloating()) {
            // The rail's empty space past the last tab is window chrome: it moves
            // the window (see MouseMove) instead of starting a docking drag, which
            // is what makes a floating panel readable as a window.
            log(QStringLiteral("rail press pos=%1,%2 -> window move").arg(mPressPos.x()).arg(mPressPos.y()));
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* e = static_cast<QMouseEvent*>(aEvent);
        if (!(e->buttons() & Qt::LeftButton)) break;
        const QPoint pos = e->position().toPoint();
        if ((pos - mPressPos).manhattanLength() < QApplication::startDragDistance()) break;

        if (mRailDrag && mPressed) {
            if (railRect().contains(pos)) {
                // Still in the rail: this is a reorder, not a docking gesture, and
                // it is a *visual* one - moveTab shifts the item inside the bar and
                // touches nothing else. The group's real order is committed once,
                // on release. Re-tabifying on every move, which is what this did,
                // re-parents docks and re-lays out the whole area per pixel of the
                // drag: that is what the tab snapping is.
                const int target = mBar->tabAt(pos);
                if (target >= 0 && target != mTabIndex) {
                    mBar->moveTab(mTabIndex, target);
                    mTabIndex = target;
                    mMovedTab = true;
                }
                mPressPos = pos;
                return true;
            }
            // left the rail: docking takes over, and it is the same drag a
            // floating panel's title starts. The reorder the user has already made
            // is committed first, so the tabs left behind keep the order they are
            // shown in instead of reverting when the layout next re-syncs the bar.
            if (mMovedTab) commitRailOrder();
            mMovedTab = false;
            mRailDrag = false;
            const bool docked = mPressed;
            log(QStringLiteral("left the rail -> dock drag"));
            beginDockPanelDrag(mPressed, mBar);
            mPressed = nullptr;
            Q_UNUSED(docked);
            return true;
        }

        QDockWidget* dock = dockForTab(mBar->tabAt(mPressPos));
        if (!dock) break;
        if (mDockDragArmed && !mDragging) {
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
        // the one place the layout is told about a reorder
        if (mRailDrag && mMovedTab) commitRailOrder();
        mDockDragArmed = false;
        mMoving = false;
        mRailDrag = false;
        mMovedTab = false;
        mTabIndex = -1;
        mPressed = nullptr;
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
