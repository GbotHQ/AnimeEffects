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
 * What it does with the mouse is deliberately minimal:
 *
 *  - A floating header hands the press back to Qt, through QMouseEvent::ignore(),
 *    so Qt's own dock drag runs. That drag is the one gesture that both moves a
 *    panel and docks it: on Wayland it becomes a platform drag-and-drop with the
 *    toplevel attached, so the compositor moves the window under the cursor and
 *    where it is dropped decides whether it docks. Handing it back is also why
 *    there is no grip and no other handle - dragging the header is the whole
 *    gesture, and a separate handle that means "move" is a pattern from another
 *    era.
 *  - A docked header consumes the press and does nothing else: a docked panel is
 *    not draggable, and Qt's drag must not start from it.
 *  - Double-clicking a docked header floats the panel; the floating case is
 *    Qt's own, with the same result.
 *
 * The drop indicator during that drag is Qt's own QRubberBand, which the layout
 * positions at the rect the panel would occupy - the truthful answer, since it
 * is the layout's decision. Only its appearance is ours (see
 * AppStyle::drawControl).
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
    void mouseDoubleClickEvent(QMouseEvent* aEvent) override;
    void contextMenuEvent(QContextMenuEvent* aEvent) override;

private:
    void popupMenu(const QPoint& aGlobalPos);
    void toggleFloating();
    void log(const QString& aMessage) const;

    QDockWidget* mDock;
};

} // namespace gui

#endif
