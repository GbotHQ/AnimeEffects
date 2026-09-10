#ifndef GUI_DOCKMETRICS_H
#define GUI_DOCKMETRICS_H

namespace gui {

/**
 * One size range, shared by every panel.
 *
 * Panel-local ranges do not survive tabbing. A tab group is a single area, so
 * its reachable sizes are the intersection of its members': the widest minimum
 * wins, the smallest maximum wins, and a panel whose content caps its own width
 * leaves a gap beside itself once the group is wider than that cap. That is
 * exactly what the three panels did before this file existed - Tools was capped
 * at its own design width of 172, Properties claimed a 320..640 window, and the
 * timeline's content asked for 380 - so no two of them could share an area
 * sensibly, which is the whole point of the tabs.
 *
 * The minimum is also the collapse threshold: dragging a sash past it hides the
 * panel rather than clamping, the way a VS Code sidebar collapses (see
 * DockSash::mouseMoveEvent). It is deliberately the smallest size that still
 * leaves every panel usable, not a comfortable size, because that is what makes
 * collapsing useful rather than something the user fights.
 */
namespace dockMetrics {

/// Narrowest a panel may be dragged before it collapses.
constexpr int kMinWidth = 200;
/// Shortest a panel may be dragged before it collapses.
constexpr int kMinHeight = 160;
/// Widest a panel grows; past this a side panel is just stretched empty space.
constexpr int kMaxWidth = 640;

} // namespace dockMetrics

} // namespace gui

#endif // GUI_DOCKMETRICS_H
