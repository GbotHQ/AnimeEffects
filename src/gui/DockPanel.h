#ifndef GUI_DOCKPANEL_H
#define GUI_DOCKPANEL_H

#include <QPoint>
#include <QDockWidget>

namespace gui {

/**
 * A QDockWidget that owns its own chrome.
 *
 * QMainWindow keeps doing all the docking work - areas, splitters, tabify,
 * resizeDocks, saveState - so this is a dock, not a docking engine. What it
 * takes over is the part Qt gets wrong on Linux:
 *
 *  - the header, through DockTitleBar (which also suppresses Qt's float and
 *    close buttons and Qt's own drag),
 *  - the floating window's frame. Qt paints neither frame nor title once a
 *    title bar widget exists, and on Wayland the panel is a frameless tool
 *    window that nothing else decorates, so the edge and its rounding are
 *    drawn here.
 *
 * The floating window carries a ring of kRing on every side. That ring is what
 * the border is painted in and what the resize grab covers; it is created by
 * the transparent QDockWidget border in the theme, because
 * QStyleSheetStyle::pixelMetric(PM_DockWidgetFrameWidth) follows the QSS border
 * width and QDockWidgetLayout insets the header and the content by exactly that
 * much. Without it the content widget is flush with the window edge and any
 * border painted by the dock ends up underneath it.
 */
class DockPanel: public QDockWidget {
    Q_OBJECT

public:
    explicit DockPanel(QWidget* aParent);

protected:
    void paintEvent(QPaintEvent* aEvent) override;
    bool eventFilter(QObject* aObject, QEvent* aEvent) override;

private:
    /// Width of the border ring; also the resize band on every edge.
    static constexpr int kRing = 6;
    /// Corner radius of the floating window; kRing keeps the content's square
    /// corners inside the rounded body, so they are never visible.
    static constexpr int kRadius = kRing;

    Qt::Edges edgesAt(const QPoint& aPos) const;
    void onTopLevelChanged(bool aFloating);

    QPoint mPressPos;
    bool mRingPress;
};

} // namespace gui

#endif
