#ifndef GUI_DOCKTITLEBAR_H
#define GUI_DOCKTITLEBAR_H

#include <QPoint>
#include <QWidget>

class QDockWidget;

namespace gui {

/// Gesture/state line for the docking work, to stderr and to the file named by
/// AE_DOCK_SPIKE_LOG (/tmp/ae-dock-spike.log by default). Temporary: this is
/// how the behaviour of a drag is read back off the machine that can perform
/// it, and it goes away once the behaviour is settled and tested.
void dockSpikeLog(const QDockWidget* aDock, const QString& aMessage);

/**
 * The dock's header, installed through QDockWidget::setTitleBarWidget().
 *
 * Setting a title bar widget is the seam that makes the panel ours: Qt hides
 * its own float and close buttons as soon as one exists (see
 * QDockWidgetPrivate::updateButtons(): `hideButtons = nativeDeco ||
 * customTitleBar`), and it stops painting both the title text and the floating
 * frame (QDockWidget::paintEvent returns early in that case), so everything
 * visible here is drawn here.
 *
 * Two gestures, deliberately separate, because on Wayland they cannot be one:
 * a compositor-driven window move gives the client no positions, and a
 * drag-and-drop gives positions but cannot move an already-floating window.
 *
 *  - The header is the floating window's title area, so dragging it moves the
 *    window, through QWindow::startSystemMove() once the drag threshold is
 *    passed. That is the compositor's own move: smooth, anchored under the
 *    cursor, and it needs no positions from us. Only the floating state moves;
 *    a docked panel's header is not a window title.
 *  - Docked, the header is inert: it consumes the press so neither Qt's dock
 *    drag nor anything of ours starts, and a docked panel cannot be dragged out
 *    by accident.
 *  - Double-click toggles floating in both states, and the right-click menu
 *    (undock/dock, hide) is the same one DockPanel offers.
 *
 * It is opaque to the mouse in both states. That matters: the press is what
 * decides which gesture this is, and a press that reaches the QDockWidget
 * instead starts Qt's own dock drag - which, floating, swallowed the move, and
 * docked, made panels draggable. (The documented alternative - ignoring the
 * press so it "propagates to the QDockWidget parent" - does not work either: an
 * ignored press never reaches the dock. Verified with QWidget::childAt.)
 */
class DockTitleBar: public QWidget {
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* aDock);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* aEvent) override;
    void mousePressEvent(QMouseEvent* aEvent) override;
    void mouseMoveEvent(QMouseEvent* aEvent) override;
    void mouseReleaseEvent(QMouseEvent* aEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* aEvent) override;
    void contextMenuEvent(QContextMenuEvent* aEvent) override;

private:
    /// The handle region: the title text plus tab padding, left-aligned.
    QRect tabRect() const;
    void startDockDrag();
    void popupMenu(const QPoint& aGlobalPos);
    void toggleFloating();
    void log(const QString& aMessage) const;

    QDockWidget* mDock;
    QPoint mPressPos;
    bool mMoving = false;
    bool mDockDragArmed = false;
    bool mDragging = false;
    bool mTabHover = false;
};

} // namespace gui

#endif
