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
/// Two panes count as adjacent when their edges are within this many pixels.
/// Qt leaves no gap between them, but the central widget and an area do not
/// always meet exactly (the corner rules move the seam by a pixel).
constexpr int kAdjacentSlack = 2;

/// One resizable seam: the line, what it resizes, and which way.
struct Boundary {
    QDockWidget* subject = nullptr;  ///< the dock whose size the drag changes
    QDockWidget* partner = nullptr;  ///< what absorbs it, null for the central widget
    Qt::Orientation axis = Qt::Horizontal; ///< Horizontal: the drag changes widths
    int sign = 1;                    ///< +1 if the subject grows with the coordinate
    int line = 0;                    ///< the seam, in window coordinates
    int spanStart = 0;               ///< the seam's extent along the other axis
    int spanEnd = 0;
};
} // namespace

// --------------------------------------------------------------------------
// One seam's grab area: an invisible box over the boundary, drawing only the
// 1px hairline, driving the resize through QMainWindow::resizeDocks.
//
// Two shapes, for two kinds of seam. Against the central widget the box lies
// entirely on the central widget's side: straddling it would cover part of the
// panel's own edge, and for a bottom-docked panel that edge is its header - so
// the drag started from a sash and the right-click escaped past it to the
// window's default popup (both were real bugs). Between two panels there is no
// such side to prefer, so the box is centred on the seam.
class DockSash::HitBox: public QWidget {
public:
    HitBox(DockSash* aSash, QMainWindow* aWindow):
        QWidget(nullptr),
        mSash(aSash),
        mWindow(aWindow) {
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

    void setBoundary(const Boundary& aBoundary, bool aCentered) {
        mBoundary = aBoundary;
        mCentered = aCentered;
        mCollapsed = false;

        setCursor(aBoundary.axis == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);

        const int span = aBoundary.spanEnd - aBoundary.spanStart + 1;
        if (aBoundary.axis == Qt::Horizontal) {
            const int x = aCentered ? aBoundary.line - kHitBoxSize / 2 : aBoundary.line;
            setGeometry(x, aBoundary.spanStart, kHitBoxSize, span);
        } else {
            const int y = aCentered ? aBoundary.line - kHitBoxSize / 2 : aBoundary.line;
            setGeometry(aBoundary.spanStart, y, span, kHitBoxSize);
        }

#ifdef Q_OS_WIN
        // Fuck this, fuck the windows DWM, fuck the windows API, fuck everything.
        SetWindowRgn(reinterpret_cast<HWND>(this->winId()), QRegion(lineRect()).toHRGN(), TRUE);
#endif
        raise();
        show();
    }

    /// A hitbox stays put (and stays alive) while its panel is collapsed
    /// mid-drag: hiding it would drop the implicit mouse grab this drag depends
    /// on, so the panel could never be dragged back out in the same gesture.
    bool isDragging() const { return mDragging; }

protected:
    void paintEvent(QPaintEvent*) override {
        // draw only the 1px hairline; never call the base paintEvent, so the
        // universal QWidget QSS background cannot fill the hitbox
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
        if (aEvent->button() != Qt::LeftButton || !mBoundary.subject)
            return;
        mDragging = true;
        // the widget keeps an implicit mouse grab until release, so the drag
        // survives the cursor leaving the hitbox
        const QPointF global = aEvent->globalPosition();
        mPressPos = (mBoundary.axis == Qt::Horizontal) ? global.x() : global.y();
        // The baseline is this moment's sizes, and it has to be captured here
        // rather than refreshed from the relayout: every resize triggers a
        // relayout, so a moving baseline made each move re-apply its whole delta
        // (measured: a 76px drag grew the panel by 181).
        mStartSize = (mBoundary.axis == Qt::Horizontal) ? mBoundary.subject->width()
                                                       : mBoundary.subject->height();
        mPartnerStart = (mBoundary.partner && mBoundary.partner->isVisible())
                ? ((mBoundary.axis == Qt::Horizontal) ? mBoundary.partner->width()
                                                      : mBoundary.partner->height())
                : 0;
        dockSpikeLog(mBoundary.subject,
                QStringLiteral("sash press subject=%1 partner=%2 axis=%3 line=%4 span=%5..%6 "
                               "start=%7 partnerStart=%8 centered=%9")
                .arg(mBoundary.subject->objectName())
                .arg(mBoundary.partner ? mBoundary.partner->objectName() : QStringLiteral("central"))
                .arg(mBoundary.axis == Qt::Horizontal ? QStringLiteral("widths") : QStringLiteral("heights"))
                .arg(mBoundary.line).arg(mBoundary.spanStart).arg(mBoundary.spanEnd)
                .arg(mStartSize).arg(mPartnerStart).arg(mCentered));
        update();
    }

    void mouseMoveEvent(QMouseEvent* aEvent) override {
        if (!mDragging || !mBoundary.subject)
            return;
        const QPointF global = aEvent->globalPosition();
        const int pos = (mBoundary.axis == Qt::Horizontal) ? global.x() : global.y();
        const int newSize = mStartSize + mBoundary.sign * (pos - mPressPos);

        // The collapse point is half the minimum, not the minimum: stopping at
        // the minimum would make the first few pixels of "too small" hide the
        // panel, so a panel could only ever be dragged to exactly its minimum
        // and never a hair less. Dragging back out asks just as unambiguously
        // for it back, so the gesture stays reversible while the button is down.
        const int collapseSize = (mBoundary.axis == Qt::Horizontal) ? dockMetrics::kCollapseWidth
                                                                    : dockMetrics::kCollapseHeight;
        if (newSize < collapseSize) {
            if (mBoundary.subject->isVisible()) {
                dockSpikeLog(mBoundary.subject,
                        QStringLiteral("sash drag requested=%1 < collapse point %2 -> collapsed")
                        .arg(newSize).arg(collapseSize));
                mBoundary.subject->hide();
                mCollapsed = true;
            }
            return;
        }
        if (mCollapsed) {
            dockSpikeLog(mBoundary.subject,
                    QStringLiteral("sash drag requested=%1 >= collapse point %2 -> restored")
                    .arg(newSize).arg(collapseSize));
            mBoundary.subject->show();
            mCollapsed = false;
        }

        // A seam between two panels keeps their total: the partner takes what
        // the subject gives up, which is what makes the seam follow the cursor
        // instead of only one side of it moving.
        if (mBoundary.partner && mBoundary.partner->isVisible() && mPartnerStart > 0) {
            const int total = mStartSize + mPartnerStart;
            mWindow->resizeDocks({mBoundary.subject, mBoundary.partner},
                                 {newSize, qMax(1, total - newSize)}, mBoundary.axis);
        } else {
            mWindow->resizeDocks({mBoundary.subject}, {newSize}, mBoundary.axis);
        }
    }

    void mouseReleaseEvent(QMouseEvent* aEvent) override {
        if (aEvent->button() == Qt::LeftButton) {
            mDragging = false;
            mCollapsed = false;
            dockSpikeLog(mBoundary.subject,
                    QStringLiteral("sash release subject=%1 now=%2 partner=%3 now=%4")
                    .arg(mBoundary.subject->objectName())
                    .arg(mBoundary.axis == Qt::Horizontal ? mBoundary.subject->width()
                                                          : mBoundary.subject->height())
                    .arg(mBoundary.partner ? mBoundary.partner->objectName() : QStringLiteral("central"))
                    .arg(mBoundary.partner
                            ? (mBoundary.axis == Qt::Horizontal ? mBoundary.partner->width()
                                                                : mBoundary.partner->height())
                            : -1));
            // A collapsed dock is no longer a target, so its sash has to go -
            // and it cannot go during the drag (see isDragging), which is why
            // this is the moment, not the collapse itself. Without it the last
            // hitbox stayed drawn at the boundary of a panel that is not there.
            if (mSash) mSash->scheduleRelayout();
            update();
        }
    }

private:
    /// The 1px line the box paints, always on the seam itself: a one-sided box
    /// puts it on the edge facing the panel, a centred one in the middle.
    QRect lineRect() const {
        int lineSize = 1;
#ifdef Q_OS_WIN
        lineSize = 2;
#endif
        if (mBoundary.axis == Qt::Horizontal) {
            const int x = mCentered ? width() / 2 : 0;
            return QRect(x, 0, lineSize, height());
        }
        const int y = mCentered ? height() / 2 : 0;
        return QRect(0, y, width(), lineSize);
    }

    DockSash* mSash;
    QMainWindow* mWindow;
    Boundary mBoundary;
    bool mCentered = false;
    bool mHovered = false;
    bool mDragging = false;
    bool mCollapsed = false;
    int mPressPos = 0;
    int mStartSize = 0;
    int mPartnerStart = 0;
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

    // The panes a seam can lie between: every docked panel plus the central
    // widget. Enumerating seams instead of docks is what makes a boundary
    // between two stacked panels exist at all - the old shape ("one hitbox per
    // dock, on its edge facing the canvas") could not express one, which is why
    // a vertical split had no horizontal resizer.
    struct Pane {
        QRect rect;
        QDockWidget* dock;
    };
    QVector<Pane> panes;
    if (QWidget* central = mWindow->centralWidget()) {
        if (central->isVisible()) panes.push_back({central->geometry(), nullptr});
    }
    for (auto* dock : mWindow->findChildren<QDockWidget*>()) {
        const Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        if (area == Qt::NoDockWidgetArea || dock->isFloating() || !dock->isVisible())
            continue;
        panes.push_back({dock->geometry(), dock});
    }

    QVector<Boundary> boundaries;
    const auto addBoundary = [&boundaries](const Boundary& aBoundary, bool aCentered) {
        for (const Boundary& existing : boundaries) {
            if (existing.axis == aBoundary.axis && existing.line == aBoundary.line
                    && existing.spanStart == aBoundary.spanStart
                    && existing.spanEnd == aBoundary.spanEnd)
                return; // the same seam seen from the other side
        }
        boundaries.push_back(aBoundary);
    };

    for (int i = 0; i < panes.size(); ++i) {
        if (!panes[i].dock) continue; // the central widget is never the subject
        const QRect a = panes[i].rect;
        for (int j = 0; j < panes.size(); ++j) {
            if (i == j) continue;
            const QRect b = panes[j].rect;
            if (b.isEmpty() || a.isEmpty()) continue;

            const int vTop = qMax(a.top(), b.top());
            const int vBottom = qMin(a.bottom(), b.bottom());
            const int hLeft = qMax(a.left(), b.left());
            const int hRight = qMin(a.right(), b.right());

            if (vBottom > vTop) {
                if (qAbs(b.right() + 1 - a.left()) <= kAdjacentSlack) {
                    // the seam is on a's left edge: dragging right shrinks a
                    Boundary boundary;
                    boundary.subject = panes[i].dock;
                    boundary.partner = panes[j].dock;
                    boundary.axis = Qt::Horizontal;
                    boundary.sign = -1;
                    boundary.line = a.left();
                    boundary.spanStart = vTop;
                    boundary.spanEnd = vBottom;
                    addBoundary(boundary, panes[j].dock != nullptr);
                } else if (qAbs(b.left() - 1 - a.right()) <= kAdjacentSlack) {
                    // the seam is on a's right edge: dragging right grows a
                    Boundary boundary;
                    boundary.subject = panes[i].dock;
                    boundary.partner = panes[j].dock;
                    boundary.axis = Qt::Horizontal;
                    boundary.sign = 1;
                    boundary.line = a.right() + 1;
                    boundary.spanStart = vTop;
                    boundary.spanEnd = vBottom;
                    addBoundary(boundary, panes[j].dock != nullptr);
                }
            }
            if (hRight > hLeft) {
                if (qAbs(b.bottom() + 1 - a.top()) <= kAdjacentSlack) {
                    // the seam is on a's top edge: dragging down shrinks a
                    Boundary boundary;
                    boundary.subject = panes[i].dock;
                    boundary.partner = panes[j].dock;
                    boundary.axis = Qt::Vertical;
                    boundary.sign = -1;
                    boundary.line = a.top();
                    boundary.spanStart = hLeft;
                    boundary.spanEnd = hRight;
                    addBoundary(boundary, panes[j].dock != nullptr);
                } else if (qAbs(b.top() - 1 - a.bottom()) <= kAdjacentSlack) {
                    // the seam is on a's bottom edge: dragging down grows a
                    Boundary boundary;
                    boundary.subject = panes[i].dock;
                    boundary.partner = panes[j].dock;
                    boundary.axis = Qt::Vertical;
                    boundary.sign = 1;
                    boundary.line = a.bottom() + 1;
                    boundary.spanStart = hLeft;
                    boundary.spanEnd = hRight;
                    addBoundary(boundary, panes[j].dock != nullptr);
                }
            }
        }
    }

    // Hitboxes are pooled: the count only changes when the layout does.
    while (mHitBoxes.size() < boundaries.size())
        mHitBoxes.push_back(new HitBox(this, mWindow));

    int used = 0;
    for (const Boundary& boundary : boundaries) {
        if (used >= kMaxHitBoxes) break;
        mHitBoxes[used]->setBoundary(boundary, boundary.partner != nullptr);
        ++used;
    }
    for (int i = used; i < mHitBoxes.size(); ++i) {
        // a hitbox whose panel was just collapsed stays where it is for the
        // rest of the drag (see isDragging)
        if (!mHitBoxes[i]->isDragging()) mHitBoxes[i]->hide();
    }
}

} // namespace gui
