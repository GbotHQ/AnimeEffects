#ifndef GUI_TIMELINEHEADER_H
#define GUI_TIMELINEHEADER_H

#include <QWidget>
#include <QString>
#include <QScopedPointer>
#include <vector>
#include "util/SlotId.h"

class QHBoxLayout;
class QLabel;
#ifdef AE_ENABLE_IMGUI
class QTabBar;
#endif

namespace theme {
class Theme;
}

namespace gui {

class GUIResources;
class IconButton;
class PlayBackWidget;
class TimeLineWidget;
class TimeLineEditorWidget;

namespace prop {
class EasingItem;
} // the easing chip + its picker (gui/prop/prop_Items.h)

// The timeline dock's header bar (design section 1): the 40px row above the
// surface, replacing the fixed-width PlayBackWidget column that used to sit
// beside the lanes. Left to right it holds
//
//   tabs | transport | frame readout | easing chip | stretch | zoom
//
// with a 1x16 rule after the tabs, between the readout and the chip, and another
// before the zoom group (the mock's .tl-hairline). The TABS are the dock's two
// views (design section 5, "one dock, two tabs"): Timeline is the lanes' surface
// (this bar's own surface) and Graph is the animation graph panel drawn on the
// same dock floor, with the object tree still beside it - the bar keeps ONE
// transport, ONE frame readout, ONE chip and ONE zoom group for both, and the tab
// is what says which view the dock's body - and the panel's own ruler band - is
// showing. In a build without the graph panel (AE_ENABLE_IMGUI off) the strip and
// its rule above do not exist at all: the bar keeps the transport, the frame
// readout, the chip and the zoom group, with no tabs and no graph view. The
// transport is the existing eight-button PlayBackWidget group, moved
// out of its column and laid out horizontally; the frame readout names the frame a
// key will land on and the range the playhead is clamped to; the easing chip is the
// picker's only home (design section 2) and names the timeline's key selection; and
// the zoom group drives the timeline's own scale ladder (ctrl::time::Scaler) - in
// for one rung, out for one rung, and fit for the whole frame range.
//
// The header does NOT carry the panel's own control (the "N channels" button): the
// design puts the DOCKED panel's one control in the panel's ruler band, so the
// panel draws it there (gui::GraphPanelModel's CHANNELS BUTTON, the Dock host).
//
// The header's own section separator - the 1px hairline on its bottom edge - is
// painted, not styled: QSS box rules do not render on plain widget surfaces.
class TimeLineHeader: public QWidget {
    Q_OBJECT
public:
    // The dock's views. Timeline is the lanes' surface and, when the graph panel is
    // part of the build, Graph is the animation graph panel; the dock's body shows
    // the one this names, and the tree is the one part that is on screen in both.
    // Without the panel only Timeline exists: the header has no tab bar at all, so
    // no graph value can be reported.
    enum class Surface {
        Timeline,
#ifdef AE_ENABLE_IMGUI
        Graph,
#endif
    };

    TimeLineHeader(GUIResources& aResources, PlayBackWidget* aTransport, QWidget* aParent = nullptr);
    ~TimeLineHeader() override;

    // The timeline the zoom group drives, the frame readout follows and the easing
    // chip names the selection of. Null (or a timeline with no project behind it)
    // leaves the readout at zero, the zoom buttons inert and the chip disabled
    // rather than acting on a stale surface.
    void setTimeLineWidget(TimeLineWidget* aTimeLine);

    // The 196px area between the frame readout and the zoom group, which the
    // easing chip fills. Fixed on purpose: the chip's content changes with the key
    // selection, and a control that resizes would re-flow the bar beside it.
    QWidget* easingChipSlot() const { return mEaseSlot; }

    // Re-reads the frame readout from the timeline. Public because a frame range
    // edited outside the timeline - the animation-settings popup - arrives through
    // neither of the timeline signals this widget follows.
    void refreshFrame();

signals:
    // The easing chip was activated (a left-click on it). The chip opens the
    // picker itself; this is the bar's own report that its focal control was used.
    void easingChipActivated();

    // The dock's view changed (a click on a tab). The dock's body follows this: it
    // shows the Timeline tab's lanes or the Graph tab's panel, and the tree stays
    // on screen for both. Never emitted with a graph value in a build without the
    // graph panel: the header has no tabs then, and Timeline is the only surface.
    void surfaceChanged(Surface aSurface);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void addHairline(QHBoxLayout& aLayout);
    void zoomByNotch(bool aZoomIn);
    void fitFrameRange();
    void refreshZoomIcons();
#ifdef AE_ENABLE_IMGUI
    // The tab strip's own ink: the mock's .tl-tabs numbers (11px, 3px/8px padding,
    // a 4px radius) on the application's tokens - muted text at rest, the ACCENT
    // fill for the selected tab, `raised` on hover - re-applied on every theme
    // change. Built in code because the header has no stylesheet asset of its own
    // (PlayBackWidget's playbackwidget.ssa styles the buttons, not a QTabBar) and
    // this bar must not add one: the theme's tokens are the same ones the app's
    // own tab bar (ProjectTabBar's modetabbar.ssa) is written against.
    void refreshTabStyle();
#endif
    // The chip's click: the picker opens for the timeline's key selection, under
    // the chip (the chip's state is pushed to it by the surface, see
    // TimeLineEditorWidget::setEasingItemHost).
    void onEasingChipClicked();
#ifdef AE_ENABLE_IMGUI
    // QTabBar::currentChanged, in the header's own currency.
    void onTabChanged(int aIndex);
#endif
    void onThemeUpdated(theme::Theme& aTheme);

    GUIResources& mGUIResources;
    PlayBackWidget* mTransport;
#ifdef AE_ENABLE_IMGUI
    // The dock's views, as tabs at the bar's left end (the mock's .tl-tabs). The
    // control is a real QTabBar: it is the application's own tab control
    // (gui::ProjectTabBar is a QTabBar too), so keyboard left/right, the hover and
    // the selected states all come from Qt rather than from a hand-rolled pair of
    // buttons. Not built without the graph panel (see the constructor).
    QTabBar* mTabs;
#endif
    TimeLineWidget* mTimeLine;
    // The timeline's surface widget, found inside mTimeLine: the ONE easing picker
    // and the chip's state both live there, and the timeline itself hands out its
    // editor, not its surface.
    TimeLineEditorWidget* mInner;
    // The timeline's frame reports, so the readout follows the playhead (and the
    // ruler's own range gestures, which report through the same signal).
    util::SlotId mFrameConnection;
    QLabel* mFrameText;
    QLabel* mFrameEndText;
    QWidget* mEaseSlot;
    // The easing control: the chip in the slot above and the single picker
    // popover it opens (see prop::EasingItem).
    QScopedPointer<prop::EasingItem> mEasing;
    // The zoom group, with the icon each button carries: the glyphs are tinted per
    // theme at runtime, so every one of them is re-read on a theme change (the same
    // reason PlayBackWidget re-sets the transport's). gui::IconButton is the
    // glyph-hover button these are built from (see makeBarButton), so the pair of
    // icons is reachable by name.
    struct ZoomButton {
        IconButton* button;
        QString icon;
    };
    std::vector<ZoomButton> mZoomButtons;
};

} // namespace gui

#endif // GUI_TIMELINEHEADER_H
