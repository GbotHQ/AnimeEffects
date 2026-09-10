#include "gui/DockPanel.h"

#include "gui/DockTitleBar.h"
#include "gui/theme/Colors.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QWindow>
#include <QResizeEvent>

namespace gui {

void showDockPanelMenu(QDockWidget* aDock, const QPoint& aGlobalPos) {
    if (!aDock) return;

    QMenu menu;
    const bool floating = aDock->isFloating();
    QAction* floatAction = menu.addAction(floating ? QObject::tr("Dock panel")
                                                   : QObject::tr("Undock panel"));
    QAction* hideAction = menu.addAction(QObject::tr("Hide panel"));

    QAction* chosen = menu.exec(aGlobalPos);
    if (!chosen) return;

    if (chosen == floatAction) {
        dockSpikeLog(aDock, QStringLiteral("menu -> setFloating(%1)").arg(!floating));
        aDock->setFloating(!floating);
    } else if (chosen == hideAction) {
        // hiding through the dock keeps the Panels menu's check state in step
        dockSpikeLog(aDock, QStringLiteral("hide panel"));
        aDock->hide();
    }
}

class DockPanel::RingBand : public QWidget {
public:
    RingBand(DockPanel* aPanel, Qt::Edges aSpanned):
        QWidget(aPanel),
        mPanel(aPanel),
        mSpanned(aSpanned) {
        setObjectName(QStringLiteral("DockRingBand"));
        setMouseTracking(true);
        // an input surface only: the ring itself is painted by DockPanel
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void mouseMoveEvent(QMouseEvent* aEvent) override {
        if (!(aEvent->buttons() & Qt::LeftButton))
            setCursor(cursorFor(edgesAt(aEvent->position().toPoint())));
        QWidget::mouseMoveEvent(aEvent);
    }

    void mousePressEvent(QMouseEvent* aEvent) override {
        if (aEvent->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(aEvent);
            return;
        }
        // Only the compositor can resize a frameless Wayland toplevel, and only
        // from an edge it is told about: this is the same call a decorated
        // window's edge gets, which is what keeps the anchor under the cursor.
        const Qt::Edges edges = edgesAt(aEvent->position().toPoint());
        QWindow* window = mPanel->windowHandle();
        const bool accepted = window && window->startSystemResize(edges);
        dockSpikeLog(mPanel, QStringLiteral("ring band press edges=0x%1 startSystemResize=%2")
                .arg(int(edges), 0, 16).arg(accepted));
        aEvent->accept();
    }

private:
    /// The band spans one edge; a position near either end of it adds the
    /// perpendicular edge, so the corners resize diagonally.
    Qt::Edges edgesAt(const QPoint& aPos) const {
        Qt::Edges edges = mSpanned;
        const QPoint inPanel = mapTo(mPanel, aPos);
        const QRect panelRect = mPanel->rect();
        if (mSpanned & (Qt::TopEdge | Qt::BottomEdge)) {
            if (inPanel.x() < kRing) edges |= Qt::LeftEdge;
            if (inPanel.x() >= panelRect.width() - kRing) edges |= Qt::RightEdge;
        } else {
            if (inPanel.y() < kRing) edges |= Qt::TopEdge;
            if (inPanel.y() >= panelRect.height() - kRing) edges |= Qt::BottomEdge;
        }
        return edges;
    }

    static Qt::CursorShape cursorFor(Qt::Edges aEdges) {
        const bool top = aEdges & Qt::TopEdge;
        const bool bottom = aEdges & Qt::BottomEdge;
        const bool left = aEdges & Qt::LeftEdge;
        if ((top && left) || (bottom && (aEdges & Qt::RightEdge))) return Qt::SizeFDiagCursor;
        if ((top && (aEdges & Qt::RightEdge)) || (bottom && left)) return Qt::SizeBDiagCursor;
        if (top || bottom) return Qt::SizeVerCursor;
        return Qt::SizeHorCursor;
    }

    DockPanel* mPanel;
    Qt::Edges mSpanned;
};

DockPanel::DockPanel(QWidget* aParent):
    QDockWidget(aParent) {
    setTitleBarWidget(new DockTitleBar(this));
    // top, bottom, then the vertical bands: created last, they take the corners
    mRing = {new RingBand(this, Qt::TopEdge), new RingBand(this, Qt::BottomEdge),
             new RingBand(this, Qt::LeftEdge), new RingBand(this, Qt::RightEdge)};
    installEventFilter(this);
    connect(this, &QDockWidget::topLevelChanged, this, &DockPanel::onTopLevelChanged);
}

void DockPanel::layoutRing() {
    if (mRing.size() != 4) return;

    const int w = width();
    const int h = height();
    mRing[0]->setGeometry(0, 0, w, kRing);
    mRing[1]->setGeometry(0, h - kRing, w, kRing);
    mRing[2]->setGeometry(0, 0, kRing, h);
    mRing[3]->setGeometry(w - kRing, 0, kRing, h);

    // the ring is the floating window's edge, so it only exists while floating;
    // a docked panel's edges belong to the main window's sashes
    const bool visible = isFloating();
    for (RingBand* band : mRing) {
        if (visible) band->raise();
        band->setVisible(visible);
    }
}

void DockPanel::resizeEvent(QResizeEvent* aEvent) {
    QDockWidget::resizeEvent(aEvent);
    layoutRing();
}

void DockPanel::paintEvent(QPaintEvent* aEvent) {
    // Docked, the panel has no surface of its own: the QSS gives it the window
    // floor colour and the content widget covers it. Nothing to add.
    QDockWidget::paintEvent(aEvent);
    if (!isFloating()) return;

    const theme::Colors c = theme::Colors::current();

    // The floating window is translucent so its corners can actually be round:
    // everything outside the rounded body stays unpainted and the backdrop
    // shows through, the same trick AppStyle uses for menus. The content's own
    // square corners sit inside the body because the ring insets them by
    // exactly the radius.
    QPainterPath body;
    body.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(body, c.base);
    p.setPen(QPen(c.outline, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(body);
}

void DockPanel::contextMenuEvent(QContextMenuEvent* aEvent) {
    // reached while floating, where the header is transparent to the mouse
    showDockPanelMenu(this, aEvent->globalPos());
    aEvent->accept();
}

bool DockPanel::eventFilter(QObject* aObject, QEvent* aEvent) {
    // Diagnostic: what the dock itself receives. Qt's dock drag needs the press
    // to arrive here and then the moves, so this separates "the press never
    // arrived" from "it arrived and Qt's drag still did nothing".
    if (aObject == this) {
        switch (aEvent->type()) {
        case QEvent::MouseButtonPress: {
            auto* m = static_cast<QMouseEvent*>(aEvent);
            dockSpikeLog(this, QStringLiteral("dock press pos=%1,%2 button=0x%3 floating=%4")
                    .arg(int(m->position().x())).arg(int(m->position().y()))
                    .arg(int(m->button()), 0, 16).arg(isFloating()));
            break;
        }
        case QEvent::MouseMove: {
            auto* m = static_cast<QMouseEvent*>(aEvent);
            if (m->buttons() & Qt::LeftButton)
                dockSpikeLog(this, QStringLiteral("dock drag-move pos=%1,%2")
                        .arg(int(m->position().x())).arg(int(m->position().y())));
            break;
        }
        case QEvent::MouseButtonRelease:
            dockSpikeLog(this, QStringLiteral("dock release"));
            break;
        default:
            break;
        }
    }
    // no gesture is claimed here: the ring bands own the edges, and a header
    // press belongs to Qt's dock drag
    return false;
}

void DockPanel::onTopLevelChanged(bool aFloating) {
    // The floating look is the theme's business, so the ring is a QSS rule
    // selected by this property (see standard.ssa); QSS property selectors are
    // only re-evaluated on a repolish.
    setProperty("aeFloating", aFloating);
    style()->unpolish(this);
    style()->polish(this);

    // Qt installs its resize handler when the dock floats - after this object's
    // constructor - and event filters run in reverse order of installation.
    // Re-installing puts this filter back in front, so a ring press is decided
    // by edgesAt() instead of by Qt.
    installEventFilter(this);

    layoutRing();
    update();
    if (auto* header = titleBarWidget()) header->update();

    const QWindow* window = windowHandle();
    const QMargins margins = window ? window->frameMargins() : QMargins(-1, -1, -1, -1);
    const theme::Colors c = theme::Colors::current();
    dockSpikeLog(this, QStringLiteral("topLevelChanged floating=%1 alphaBuffer=%2 frameWidth=%3 flags=0x%4 "
                                      "frameMargins=%5,%6,%7,%8 geometry=%9,%10 %11x%12 "
                                      "tokens base=%13 outline=%14 hairline=%15 floaterEdge=%16")
            .arg(aFloating)
            .arg(window ? window->format().alphaBufferSize() : -1)
            .arg(style()->pixelMetric(QStyle::PM_DockWidgetFrameWidth, nullptr, this))
            .arg(window ? int(window->flags()) : 0, 0, 16)
            .arg(margins.left()).arg(margins.top()).arg(margins.right()).arg(margins.bottom())
            .arg(geometry().x()).arg(geometry().y()).arg(width()).arg(height())
            .arg(c.base.name(), c.outline.name(), c.hairline.name(), c.floaterEdge.name()));
}

} // namespace gui
