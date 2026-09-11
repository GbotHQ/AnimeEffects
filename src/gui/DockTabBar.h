#ifndef GUI_DOCTABBAR_H
#define GUI_DOCTABBAR_H

#include <QObject>
#include <QPoint>
#include <QPointer>
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

    /// No size at all while the panel shares its slot, because then this row is
    /// not drawn - the slot's rail holds the titles - and the dock's layout
    /// reserves the title area from the *sizeHint*, not from the height, so
    /// hiding the row or fixing its height at zero still left the band
    /// (measured: a 32px strip under the rail, with the row 256x0).
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

/**
 * The gestures every dock tab bar shares, installed by MainWindow on the panels'
 * own title bars and on the tab bars Qt builds for groups.
 *
 *  - dragging a tab *inside the rail* moves the tab in the rail (the group's
 *    order, which Qt's layout then keeps), and only once the pointer leaves the
 *    rail does the dock drag start - the VS Code gesture, so a tab can be
 *    reordered without a docking preview appearing whenever the pointer twitches;
 *  - dragging the rest of a lone panel's title row docks that panel: with one tab
 *    the row reads as a title, so the row is the handle and none of it is dead;
 *  - dragging the empty space past the last tab of a *floating* panel's row moves
 *    its window: the split Wayland forces, since a compositor move reports no
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
    /// Whether this bar is a panel's own title row rather than the rail Qt builds
    /// for a group. The difference is what the rest of the row does: for a lone
    /// panel the row is the panel's handle, for a group it is empty space.
    bool isTitleRow() const;
    /// The rail's rectangle: the band a tab drag has to leave before docking.
    QRect railRect() const;
    /// Applies the order the bar is *showing* to the layout, once the drag that
    /// produced it is over. tabifyDockWidget is the only way to set a group's
    /// order and it puts a dock at the *end* of the rail, so the order is built by
    /// moving docks to the end from last to first.
    void commitRailOrder();
    void log(const QString& aMessage) const;

    /// Watched, not merely pointed at: Qt creates and destroys these bars as
    /// groups come and go, and a reorder makes it rebuild one.
    QPointer<QTabBar> mBar;
    QDockWidget* mPressed = nullptr;
    QPoint mPressPos;
    /// The pressed tab's *current* index in the bar: moveTab shifts it, so the next
    /// move has to be applied from where the tab actually is.
    int mTabIndex = -1;
    /// Whether the bar's order was changed during this drag, which is what decides
    /// whether the layout has to be told about it at all.
    bool mMovedTab = false;
    bool mMoving = false;
    bool mDockDragArmed = false;
    bool mDragging = false;
    bool mRailDrag = false;
};

} // namespace gui

#endif // GUI_DOCTABBAR_H
