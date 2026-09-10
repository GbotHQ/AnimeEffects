#ifndef GUI_DOCTABBAR_H
#define GUI_DOCTABBAR_H

#include <QObject>
#include <QPoint>
#include <QTabBar>

class QDockWidget;

namespace gui {

/// Gesture/state line for the docking work, to stderr and to the file named by
/// AE_DOCK_SPIKE_LOG (/tmp/ae-dock-spike.log by default). Temporary: this is
/// how the behaviour of a drag is read back off the machine that can perform
/// it, and it goes away once the behaviour is settled and tested.
void dockSpikeLog(const QDockWidget* aDock, const QString& aMessage);

/**
 * A dock group's title row.
 *
 * The same widget class and the same stylesheet rule carry the titles whether a
 * panel is alone or sharing its area: a QTabBar, which is what Qt itself uses
 * for a tabbed group. A panel alone in its area gets one of these as its title
 * bar with a single tab in it; a group gets Qt's own tab bar, which this app
 * names (dockTabBar) so one QSS rule dresses both. That is the point: before,
 * a lone panel's title was a custom-painted widget and a group's titles were a
 * QTabBar, so "one tab" and "two tabs" were two implementations with different
 * looks, sizes and behaviour.
 *
 * Qt's model is the reason this is a tab bar and not a hand-painted row: for Qt
 * a group's titles *are* its tab bar, and it hides the docks' own title bars to
 * make room. Going the other way - keeping a custom row and suppressing Qt's -
 * leaves the layout reserving the suppressed bar's height (measured: a 32px
 * band above the group), so the row would have to be positioned by hand as well.
 */
class DockTabBar: public QTabBar {
    Q_OBJECT

public:
    /// The title row of one panel: a tab bar holding that panel's title.
    explicit DockTabBar(QDockWidget* aDock);
};

/**
 * The gestures every dock tab bar shares, installed by MainWindow on the panels'
 * own title bars and on the tab bars Qt builds for groups.
 *
 *  - dragging a tab starts the dock drag, so a panel is moved, split or tabbed
 *    from wherever its title is - alone, grouped, or floating;
 *  - dragging the row's empty space past the last tab moves a floating panel's
 *    window: the split Wayland forces, since a compositor move reports no
 *    positions and a drag cannot move a window;
 *  - right-click is the panel menu, double-click toggles floating, and a click
 *    on a group's inactive tab is Qt's own activation.
 */
class DockTabBarFilter: public QObject {
    Q_OBJECT

public:
    DockTabBarFilter(QTabBar* aBar, QObject* aParent);

protected:
    bool eventFilter(QObject* aObject, QEvent* aEvent) override;

private:
    /// The panel a tab belongs to: the bar's own dock for a panel's title row,
    /// and the dock named by the tab's text for a group's bar.
    QDockWidget* dockForTab(int aIndex) const;
    void log(const QString& aMessage) const;

    QTabBar* mBar;
    QPoint mPressPos;
    bool mMoving = false;
    bool mDockDragArmed = false;
    bool mDragging = false;
};

} // namespace gui

#endif // GUI_DOCTABBAR_H
