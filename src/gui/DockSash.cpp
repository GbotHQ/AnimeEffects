#include "gui/DockSash.h"
#include "gui/DockMetrics.h"
#include "gui/DockTabBar.h"
#include "gui/theme/Colors.h"

#include <QDockWidget>
#include <QMap>
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
        // One-sided boxes lie on the canvas's side of the seam, which is the
        // direction the subject grows in; centred boxes straddle it.
        const int boxStart = aCentered ? aBoundary.line - kHitBoxSize / 2
                                       : (aBoundary.sign > 0 ? aBoundary.line
                                                             : aBoundary.line - kHitBoxSize + 1);
        if (aBoundary.axis == Qt::Horizontal) {
            setGeometry(boxStart, aBoundary.spanStart, kHitBoxSize, span);
        } else {
            setGeometry(aBoundary.spanStart, boxStart, span, kHitBoxSize);
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

    /// Whether this hitbox is the seam aBoundary describes, so a drag in progress
    /// is not shown a different one.
    bool holds(const Boundary& aBoundary) const {
        return mBoundary.subject == aBoundary.subject && mBoundary.axis == aBoundary.axis
                && mBoundary.line == aBoundary.line && mBoundary.spanStart == aBoundary.spanStart;
    }

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
        // A one-sided box lies on the canvas's side of the seam, so the line is
        // at the edge that touches the panel: the box's start when the subject
        // grows towards positive coordinates, its end when it grows the other
        // way (the bottom area's seam, for instance, has the canvas above it).
        if (mBoundary.axis == Qt::Horizontal) {
            const int x = mCentered ? width() / 2
                                    : (mBoundary.sign > 0 ? 0 : width() - lineSize);
            return QRect(x, 0, lineSize, height());
        }
        const int y = mCentered ? height() / 2 : (mBoundary.sign > 0 ? 0 : height() - lineSize);
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

    // Seams belong to areas, not to pairs of docks.
    //
    // A dock area is one slot with one edge facing the canvas, and that edge is
    // one boundary however many panels sit in the area - the line where the
    // sidebar's bottom meets the timeline is the *bottom area's* top edge, not a
    // seam between two panels that happen to touch. Enumerating pairs instead put
    // two handles on that one line (each highlighting its own half, since each
    // spanned only the overlap of the pair), and the pair across areas resized
    // docks from two different areas, which QMainWindow::resizeDocks does not
    // support: measured, it aborted the process on the next drag.
    QMap<int, QVector<QDockWidget*>> byArea;
    for (auto* dock : mWindow->findChildren<QDockWidget*>()) {
        const Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        if (area == Qt::NoDockWidgetArea || dock->isFloating() || !dock->isVisible()) continue;
        byArea[int(area)].push_back(dock);
    }

    QVector<Boundary> boundaries;
    const auto addBoundary = [&boundaries](const Boundary& aBoundary) {
        for (const Boundary& existing : boundaries) {
            if (existing.axis == aBoundary.axis && existing.line == aBoundary.line
                    && existing.spanStart == aBoundary.spanStart
                    && existing.spanEnd == aBoundary.spanEnd)
                return; // the same seam seen from the other side
        }
        boundaries.push_back(aBoundary);
    };

    // (1) one seam per area: the edge of the area that faces the canvas. The
    //     subject is a panel of that area; nothing absorbs it, because resizing
    //     an area is what changes - the canvas and the areas beside it take up
    //     the difference.
    for (auto it = byArea.begin(); it != byArea.end(); ++it) {
        const Qt::DockWidgetArea area = Qt::DockWidgetArea(it.key());
        const QVector<QDockWidget*>& docks = it.value();
        if (docks.isEmpty()) continue;
        QRect areaRect;
        for (QDockWidget* dock : docks) {
            const QRect rect = dock->geometry();
            areaRect = areaRect.isNull() ? rect : areaRect.united(rect);
        }
        if (areaRect.isNull()) continue;

        Boundary boundary;
        boundary.subject = docks.first();
        boundary.partner = nullptr;
        switch (area) {
        case Qt::LeftDockWidgetArea:
            // the seam is the area's right edge: dragging right grows it
            boundary.axis = Qt::Horizontal;
            boundary.sign = 1;
            boundary.line = areaRect.right() + 1;
            boundary.spanStart = areaRect.top();
            boundary.spanEnd = areaRect.bottom();
            break;
        case Qt::RightDockWidgetArea:
            boundary.axis = Qt::Horizontal;
            boundary.sign = -1;
            boundary.line = areaRect.left();
            boundary.spanStart = areaRect.top();
            boundary.spanEnd = areaRect.bottom();
            break;
        case Qt::TopDockWidgetArea:
            boundary.axis = Qt::Vertical;
            boundary.sign = 1;
            boundary.line = areaRect.bottom() + 1;
            boundary.spanStart = areaRect.left();
            boundary.spanEnd = areaRect.right();
            break;
        default:
            // the bottom area's top edge: the seam the timeline shares with
            // everything above it, across the area's full width
            boundary.axis = Qt::Vertical;
            boundary.sign = -1;
            boundary.line = areaRect.top();
            boundary.spanStart = areaRect.left();
            boundary.spanEnd = areaRect.right();
            break;
        }
        addBoundary(boundary);
    }

    // (2) seams between panels *sharing* an area, which is the one case where
    //     resizing moves two docks against each other: they are nested on the
    //     same level, which is what QMainWindow::resizeDocks requires.
    for (auto it = byArea.begin(); it != byArea.end(); ++it) {
        const QVector<QDockWidget*>& docks = it.value();
        for (int i = 0; i < docks.size(); ++i) {
            for (int j = 0; j < docks.size(); ++j) {
                if (i == j) continue;
                const QRect a = docks[i]->geometry();
                const QRect b = docks[j]->geometry();
                if (a.isEmpty() || b.isEmpty()) continue;
                const int vTop = qMax(a.top(), b.top());
                const int vBottom = qMin(a.bottom(), b.bottom());
                const int hLeft = qMax(a.left(), b.left());
                const int hRight = qMin(a.right(), b.right());

                Boundary boundary;
                boundary.subject = docks[i];
                boundary.partner = docks[j];
                if (vBottom > vTop) {
                    if (qAbs(b.right() + 1 - a.left()) <= kAdjacentSlack) {
                        // the seam is on a's left edge: dragging right shrinks a
                        boundary.axis = Qt::Horizontal;
                        boundary.sign = -1;
                        boundary.line = a.left();
                        boundary.spanStart = vTop;
                        boundary.spanEnd = vBottom;
                        addBoundary(boundary);
                    } else if (qAbs(b.left() - 1 - a.right()) <= kAdjacentSlack) {
                        boundary.axis = Qt::Horizontal;
                        boundary.sign = 1;
                        boundary.line = a.right() + 1;
                        boundary.spanStart = vTop;
                        boundary.spanEnd = vBottom;
                        addBoundary(boundary);
                    }
                }
                if (hRight > hLeft) {
                    if (qAbs(b.bottom() + 1 - a.top()) <= kAdjacentSlack) {
                        boundary.axis = Qt::Vertical;
                        boundary.sign = -1;
                        boundary.line = a.top();
                        boundary.spanStart = hLeft;
                        boundary.spanEnd = hRight;
                        addBoundary(boundary);
                    } else if (qAbs(b.top() - 1 - a.bottom()) <= kAdjacentSlack) {
                        boundary.axis = Qt::Vertical;
                        boundary.sign = 1;
                        boundary.line = a.bottom() + 1;
                        boundary.spanStart = hLeft;
                        boundary.spanEnd = hRight;
                        addBoundary(boundary);
                    }
                }
            }
        }
    }

    // A seam being dragged keeps the boundary it was pressed on. Collapsing a
    // panel changes the layout, the layout rebuilds this list, and the hitbox
    // that holds the mouse grab is pooled - so without this it is handed a
    // different seam mid-drag and starts resizing a panel the user never
    // touched (measured: dragging the sidebar past its collapse point began
    // resizing the animation panel above it).
    int draggingIndex = -1;
    for (int i = 0; i < mHitBoxes.size(); ++i) {
        if (mHitBoxes[i]->isDragging()) {
            draggingIndex = i;
            break;
        }
    }

    // The pool grows with the seams; without this the first assignment indexes
    // an empty list and Qt asserts (measured: abort on startup, at
    // QList::operator[] in this function).
    while (mHitBoxes.size() < boundaries.size() && mHitBoxes.size() < kMaxHitBoxes)
        mHitBoxes.push_back(new HitBox(this, mWindow));

    // Bounded by the pool, not by the cap: skipping the dragged seam's slot
    // consumes one, and indexing past the pool is what Qt asserts on (measured,
    // twice, at exactly this line).
    int used = 0;
    for (const Boundary& boundary : boundaries) {
        if (used >= mHitBoxes.size()) break;
        if (draggingIndex >= 0 && used == draggingIndex) ++used; // keep its slot
        if (used >= mHitBoxes.size()) break;
        if (draggingIndex >= 0 && mHitBoxes[draggingIndex]->holds(boundary)) continue;
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
