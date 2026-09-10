#include "gui/DockPanel.h"

#include "gui/DockTitleBar.h"
#include "gui/theme/Colors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QWindow>

namespace gui {

DockPanel::DockPanel(QWidget* aParent):
    QDockWidget(aParent),
    mPressPos(0, 0),
    mRingPress(false) {
    setTitleBarWidget(new DockTitleBar(this));
    installEventFilter(this);
    connect(this, &QDockWidget::topLevelChanged, this, &DockPanel::onTopLevelChanged);
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

bool DockPanel::eventFilter(QObject* aObject, QEvent* aEvent) {
    if (aObject != this || aEvent->type() != QEvent::MouseButtonPress)
        return false;

    auto* mouse = static_cast<QMouseEvent*>(aEvent);
    if (mouse->button() != Qt::LeftButton || !isFloating())
        return false;

    // Only the ring around the panel belongs to this filter; a press inside it
    // is the window edge, and on this platform an edge can only be resized
    // through the compositor. Consuming the press keeps Qt's own resize handler
    // (a QWidgetResizeHandler Qt installs when a dock floats) out of the
    // gesture: that one resizes by recomputing the widget geometry, and on
    // Wayland the position half of it is ignored - which is why dragging the
    // top or left edge used to resize from the opposite corner.
    const Qt::Edges edges = edgesAt(mouse->position().toPoint());
    if (!edges)
        return false;

    mPressPos = mouse->position().toPoint();
    mRingPress = true;
    QWindow* window = windowHandle();
    const bool accepted = window && window->startSystemResize(edges);
    dockSpikeLog(this, QStringLiteral("ring edge-press edges=0x%1 startSystemResize=%2")
            .arg(int(edges), 0, 16).arg(accepted));
    return true;
}

Qt::Edges DockPanel::edgesAt(const QPoint& aPos) const {
    Qt::Edges edges;
    if (aPos.y() < kRing) edges |= Qt::TopEdge;
    if (aPos.y() >= height() - kRing) edges |= Qt::BottomEdge;
    if (aPos.x() < kRing) edges |= Qt::LeftEdge;
    if (aPos.x() >= width() - kRing) edges |= Qt::RightEdge;
    return edges;
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
