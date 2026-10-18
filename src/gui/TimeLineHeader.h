#ifndef GUI_TIMELINEHEADER_H
#define GUI_TIMELINEHEADER_H

#include <QWidget>
#include <QString>
#include <QScopedPointer>
#include <vector>
#include "util/SlotId.h"

class QHBoxLayout;
class QLabel;

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
//   transport | frame readout | easing chip | stretch | zoom
//
// with a 1x16 rule between the readout and the chip, and another before the zoom
// group (the mock's .tl-hairline). The transport is the existing eight-button
// PlayBackWidget group, moved out of its column and laid out horizontally; the
// frame readout names the frame a key will land on and the range the playhead is
// clamped to; the easing chip is the picker's only home (design section 2) and
// names the timeline's key selection; and the zoom group drives the timeline's
// own scale ladder (ctrl::time::Scaler) - in for one rung, out for one rung, and
// fit for the whole frame range.
//
// The Timeline | Graph tabs and the channel chips belong to the same bar and are
// NOT here yet (they arrive with the panel's tab work). The header's own section
// separator - the 1px hairline on its bottom edge - is painted, not styled: QSS box
// rules do not render on plain widget surfaces.
class TimeLineHeader: public QWidget {
    Q_OBJECT
public:
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

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void addHairline(QHBoxLayout& aLayout);
    void zoomByNotch(bool aZoomIn);
    void fitFrameRange();
    void refreshZoomIcons();
    // The chip's click: the picker opens for the timeline's key selection, under
    // the chip (the chip's state is pushed to it by the surface, see
    // TimeLineEditorWidget::setEasingItemHost).
    void onEasingChipClicked();
    void onThemeUpdated(theme::Theme& aTheme);

    GUIResources& mGUIResources;
    PlayBackWidget* mTransport;
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
