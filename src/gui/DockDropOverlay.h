#ifndef GUI_DOCKDROPOVERLAY_H
#define GUI_DOCKDROPOVERLAY_H

#include <QDockWidget>
#include <QWidget>

namespace gui {

/**
 * Highlights the dock area a dragged panel would land in.
 *
 * A child of the main window rather than a window of its own: on Wayland a
 * client cannot position a top-level, so an overlay window is not an option -
 * and it is not needed, because the thing being dragged is a small chip rather
 * than the panel, so nothing covers the overlay.
 *
 * The band it draws is the edge region the drop would use, not the panel's
 * final geometry (that comes from the dock's own size hint once it is docked),
 * so it answers "which side" rather than "how wide".
 */
class DockDropOverlay: public QWidget {
    Q_OBJECT

public:
    explicit DockDropOverlay(QWidget* aParent);

    void showArea(Qt::DockWidgetArea aArea);
    void clear();

private:
    static QRect bandFor(const QRect& aWindow, Qt::DockWidgetArea aArea);

    void paintEvent(QPaintEvent* aEvent) override;

    Qt::DockWidgetArea mArea;
};

} // namespace gui

#endif
