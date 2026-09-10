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
 * its own float/close buttons as soon as one exists (see
 * QDockWidgetPrivate::updateButtons(): `hideButtons = nativeDeco ||
 * customTitleBar`), which is what removes the "undock on click" affordance
 * from the header, and it makes Qt stop painting both the title text and the
 * floating frame (QDockWidget::paintEvent returns early in that case), so
 * everything visible in the header is drawn here.
 *
 * The gesture split below is the part that QDockWidget's own title bar can
 * never provide on Wayland:
 *   - floating, drag on the header -> QWindow::startSystemMove()
 *   - docked, drag on the header  -> ours: consumed on press, so Qt's own
 *     drag never starts (on Wayland that drag is a platform DnD through
 *     QDrag/QMimeData "application/x-qt-mainwindowdrag-window").
 *
 * The reason it has to be ours: QDockWidget moves a floating window with
 * QWidget::move() and never calls the platform move API, while on Wayland
 * QWaylandWindow ignores every requested toplevel position
 * (fixedToplevelPositions -> rect.moveTo(screen origin)). QWindow::
 * startSystemMove() is the only API that can move a Wayland toplevel, and Qt
 * documents it as such.
 *
 * Window edges are not handled here: DockPanel's ring covers them.
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
    void popupMenu(const QPoint& aGlobalPos);
    void toggleFloating();
    void log(const QString& aMessage) const;

    QDockWidget* mDock;
    QPoint mPressPos;
    bool mPressed;
    bool mGestureStarted;
};

} // namespace gui

#endif
