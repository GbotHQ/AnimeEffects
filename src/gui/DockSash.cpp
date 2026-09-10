#include "gui/DockSash.h"
#include "gui/DockMetrics.h"
#include "gui/DockTitleBar.h"
#include "gui/theme/Colors.h"

#include <QDockWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#endif


namespace gui {

namespace {
constexpr int kHitBoxSize = 12; // invisible grab area (VS Code style)
bool isVerticalEdge(Qt::DockWidgetArea aArea) {
    // left/right docks: the sash is a vertical line, dragged horizontally
    return aArea == Qt::LeftDockWidgetArea || aArea == Qt::RightDockWidgetArea;
}
} // namespace

// --------------------------------------------------------------------------
// Grab area overlaid on one dock edge. Child of the main window (QMainWindow
// only manages registered docks/central/toolbars, so a plain child floats
// freely). Draws only the 1px hairline on the boundary; the rest of the box
// stays transparent.
class DockSash::HitBox: public QWidget {
public:
    HitBox(DockSash* aSash, QMainWindow* aWindow):
        QWidget(nullptr),
        mSash(aSash),
        mWindow(aWindow),
        mDock(nullptr),
        mArea(Qt::NoDockWidgetArea),
        mHovered(false),
        mDragging(false),
        mPressPos(0),
        mStartSize(0) {
        setParent(aWindow);
        // paint only the hairline: with a universal `QWidget { background }`
        // rule in the QSS, the style sheet engine fills even a paintEvent-less
        // widget unless it is marked translucent
        setMouseTracking(true);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setWindowFlags(Qt::FramelessWindowHint);
        setObjectName("DockSash");
        hide();
    }

    void setTarget(QDockWidget* aDock, Qt::DockWidgetArea aArea) {
        mDock = aDock;
        mArea = aArea;
        setCursor(isVerticalEdge(aArea) ? Qt::SplitHCursor : Qt::SplitVCursor);
        // Dock geometry is in window coordinates. The hitbox lies entirely
        // outside the dock, on the central-widget side of the boundary: a box
        // straddling the boundary covers part of the dock's own edge, and for a
        // bottom-docked or left-docked panel that edge is where the header is.
        // A press there then belongs to the sash instead of the panel - and a
        // right-click escapes past the sash to the QMainWindow, whose default
        // popup is Qt's bare list of dock-visibility toggles, which is not the
        // panel's menu and looks nothing like the rest of the app. The drawn
        // hairline still sits on the boundary, because lineRect() puts it on
        // the edge facing the dock.
        const QRect r = aDock->geometry();
        switch (aArea) {
        case Qt::LeftDockWidgetArea:
            setGeometry(r.right() + 1, r.top(), kHitBoxSize, r.height());
            break;
        case Qt::RightDockWidgetArea:
            setGeometry(r.left() - kHitBoxSize, r.top(), kHitBoxSize, r.height());
            break;
        case Qt::TopDockWidgetArea:
            setGeometry(r.left(), r.bottom() + 1, r.width(), kHitBoxSize);
            break;
        case Qt::BottomDockWidgetArea:
            setGeometry(r.left(), r.top() - kHitBoxSize, r.width(), kHitBoxSize);
            break;
        default:
            break;
        }

#ifdef Q_OS_WIN
        // Fuck this, fuck the windows DWM, fuck the windows API, fuck everything.
        SetWindowRgn(reinterpret_cast<HWND>(this->winId()), QRegion(lineRect()).toHRGN(), TRUE);
#endif
        raise();
        show();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        // draw only the 1px hairline centered on the boundary; never call the
        // base paintEvent, so the universal QWidget QSS background cannot
        // fill the hitbox
        const theme::Colors c = theme::Colors::current();
        QPainter p(this);
        p.fillRect(lineRect(), mDragging ? c.hover : (mHovered ? c.hairlineHover : c.hairline));
    }

    void enterEvent(QEnterEvent*) override {
        mHovered = true;
        update();
    }

    void leaveEvent(QEvent*) override {
        mHovered = false;
        update();
    }

    void mousePressEvent(QMouseEvent* aEvent) override {
        if (aEvent->button() != Qt::LeftButton || !mDock)
            return;
        mDragging = true;
        // the widget keeps an implicit mouse grab until release, so the drag
        // survives the cursor leaving the hitbox
        const QPointF global = aEvent->globalPosition();
        mPressPos = isVerticalEdge(mArea) ? global.x() : global.y();
        mStartSize = isVerticalEdge(mArea) ? mDock->width() : mDock->height();
        update();
    }

    void mouseMoveEvent(QMouseEvent* aEvent) override {
        if (!mDragging || !mDock)
            return;
        const QPointF global = aEvent->globalPosition();
        const int pos = isVerticalEdge(mArea) ? global.x() : global.y();
        // left/top docks grow towards the central widget; right/bottom docks
        // grow away from it
        const int sign = (mArea == Qt::LeftDockWidgetArea || mArea == Qt::TopDockWidgetArea) ? 1 : -1;
        const int newSize = mStartSize + sign * (pos - mPressPos);
        // The collapse point is half the minimum, not the minimum: stopping at
        // the minimum would make the first few pixels of "too small" hide the
        // panel, so a panel could only ever be dragged to exactly its minimum
        // and never a hair less. Past the minimum it keeps shrinking down to
        // half of it, and only a drag that goes beyond that reads as "hide
        // this". Dragging back out asks just as unambiguously for it back, so
        // the gesture stays reversible while the button is still down.
        const int collapseSize = isVerticalEdge(mArea) ? dockMetrics::kCollapseWidth
                                                      : dockMetrics::kCollapseHeight;
        if (newSize < collapseSize) {
            if (mDock->isVisible()) {
                dockSpikeLog(mDock, QStringLiteral("sash drag requested=%1 < collapse point %2 -> collapsed")
                        .arg(newSize).arg(collapseSize));
                mDock->hide();
                mCollapsed = true;
            }
            return;
        }
        if (mCollapsed) {
            dockSpikeLog(mDock, QStringLiteral("sash drag requested=%1 >= collapse point %2 -> restored")
                    .arg(newSize).arg(collapseSize));
            mDock->show();
            mCollapsed = false;
        }
        // resizeDocks clamps to the dock's minimum/maximum sizes
        mWindow->resizeDocks({mDock}, {newSize},
                             isVerticalEdge(mArea) ? Qt::Horizontal : Qt::Vertical);
    }

    void mouseReleaseEvent(QMouseEvent* aEvent) override {
        if (aEvent->button() == Qt::LeftButton) {
            mDragging = false;
            mCollapsed = false;
            // A collapsed dock is no longer a target, so its sash has to go -
            // and it cannot go during the drag (see isDragging), which is why
            // this is the moment, not the collapse itself. Without it the last
            // hitbox stayed drawn at the boundary of a panel that is not there.
            if (mSash) mSash->scheduleRelayout();
            update();
        }
    }

public:
    /// A hitbox stays put (and stays alive) while its dock is collapsed
    /// mid-drag: hiding it would drop the implicit mouse grab this drag depends
    /// on, so the panel could never be dragged back out in the same gesture.
    bool isDragging() const { return mDragging; }

private:
    /// The 1px line the box paints, always on the boundary itself: the hitbox
    /// is one-sided (outside the dock), so the line goes on whichever edge
    /// faces the dock.
    QRect lineRect() const {
        int lineSize = 1;
#ifdef Q_OS_WIN
        lineSize = 2;
#endif
        switch (mArea) {
        case Qt::LeftDockWidgetArea:
            return QRect(0, 0, lineSize, height());
        case Qt::RightDockWidgetArea:
            return QRect(width() - lineSize, 0, lineSize, height());
        case Qt::TopDockWidgetArea:
            return QRect(0, 0, width(), lineSize);
        case Qt::BottomDockWidgetArea:
            return QRect(0, height() - lineSize, width(), lineSize);
        default:
            return QRect();
        }
    }

    DockSash* mSash;
    QMainWindow* mWindow;
    QPointer<QDockWidget> mDock;
    Qt::DockWidgetArea mArea;
    bool mHovered;
    bool mDragging;
    bool mCollapsed = false;
    int mPressPos;
    int mStartSize;
};

// --------------------------------------------------------------------------
DockSash::DockSash(QMainWindow* aWindow): QObject(aWindow), mWindow(aWindow) {
    mWindow->installEventFilter(this);
    watchDocks();
    scheduleRelayout();
}

bool DockSash::eventFilter(QObject*, QEvent* aEvent) {
    // never walk the widget tree synchronously here: during window teardown a
    // dying dock emits ChildRemoved mid-destruction, and touching the child
    // list then is use-after-free. Everything goes through the deferred,
    // context-bound relayout.
    switch (aEvent->type()) {
    case QEvent::Resize:
    case QEvent::LayoutRequest:
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::ChildAdded:
    case QEvent::ChildRemoved:
        scheduleRelayout();
        break;
    default:
        break;
    }
    return false;
}

void DockSash::watchDocks() {
    // track every dock's geometry/visibility changes through the same filter
    for (auto* dock : mWindow->findChildren<QDockWidget*>())
        dock->installEventFilter(this);
}

void DockSash::scheduleRelayout() {
    // coalesce bursts of layout/move events into one relayout per event loop
    // turn
    QTimer::singleShot(0, this, [this]() { relayout(); });
}

void DockSash::relayout() {
    watchDocks();
    QVector<QPair<QDockWidget*, Qt::DockWidgetArea>> targets;
    for (auto* dock : mWindow->findChildren<QDockWidget*>()) {
        const Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        if (area == Qt::NoDockWidgetArea || dock->isFloating() || !dock->isVisible())
            continue;
        targets.push_back({dock, area});
    }
    while (mHitBoxes.size() < targets.size())
        mHitBoxes.push_back(new HitBox(this, mWindow));
    int used = 0;
    for (int i = 0; i < mHitBoxes.size(); ++i) {
        if (used < targets.size()) {
            mHitBoxes[i]->setTarget(targets[used].first, targets[used].second);
            ++used;
        } else if (!mHitBoxes[i]->isDragging()) {
            // a hitbox whose panel was just collapsed stays where it is for the
            // rest of the drag (see isDragging)
            mHitBoxes[i]->hide();
        }
    }
}

} // namespace gui
