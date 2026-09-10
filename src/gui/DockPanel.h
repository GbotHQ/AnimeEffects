#ifndef GUI_DOCKPANEL_H
#define GUI_DOCKPANEL_H

#include <QPoint>
#include <QDockWidget>
#include <QVector>

namespace gui {

/// The panel's right-click menu (undock/dock, hide). Shared, because which
/// widget receives the click depends on the state: a docked panel's header is
/// opaque to the mouse and handles it, a floating panel's header is transparent
/// so the QDockWidget underneath receives it.
void showDockPanelMenu(QDockWidget* aDock, const QPoint& aGlobalPos);

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

    /// Undock/dock the panel. The translucent attributes have to be in place
    /// before the floating window is created, and must not outlive it, so every
    /// float goes through here rather than calling setFloating() directly.
    void setPanelFloating(bool aFloating);

protected:
    void paintEvent(QPaintEvent* aEvent) override;
    void resizeEvent(QResizeEvent* aEvent) override;
    void contextMenuEvent(QContextMenuEvent* aEvent) override;
    bool eventFilter(QObject* aObject, QEvent* aEvent) override;

private:
    /// Width of the border ring; also the resize band on every edge.
    static constexpr int kRing = 6;
    /// Corner radius of the floating window; kRing keeps the content's square
    /// corners inside the rounded body, so they are never visible.
    static constexpr int kRadius = kRing;

    /// Input-only strip lying in one band of the floating panel's edge ring.
    ///
    /// These exist because owning the edge gesture through an event filter is a
    /// race that was lost: Qt emits QDockWidget::topLevelChanged *before* it
    /// installs its own QWidgetResizeHandler (qdockwidget.cpp, setWindowState),
    /// and event filters run newest-first, so Qt's handler always ends up in
    /// front of any filter re-installed from that signal - and its resize
    /// recomputes the widget geometry, whose position half Wayland throws away,
    /// which is why the top and left edges resized from the opposite corner.
    /// A child widget is hit before its parent, so a press in the ring never
    /// reaches the dock and Qt's handler never sees it.
    class RingBand;

    void layoutRing();
    void onTopLevelChanged(bool aFloating);

    QVector<RingBand*> mRing;
};

} // namespace gui

#endif
