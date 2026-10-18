#include "gui/TimeLineHeader.h"
#include "gui/IconButton.h"
#include "gui/PlayBackWidget.h"
#include "gui/TimeLineWidget.h"
#include "gui/TimeLineEditorWidget.h"
#include "gui/prop/prop_Items.h"
#include "gui/theme/Colors.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/time/time_Scaler.h"
#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#ifdef AE_ENABLE_IMGUI
#include <QTabBar>
#endif
#include <algorithm>

namespace {
// The header's geometry, all from the design's own numbers: a 40px bar, 8px of
// inset at both ends, 6px between the groups, and a 1x16 rule between two of them
// (the mock's .tl-head / .tl-hairline). The transport's own 28px/20px grid belongs
// to PlayBackWidget; the zoom group's buttons are the same 28px box with an 18px
// icon.
const int kHeaderHeight = 40;
const int kHeaderPad = 8;
const int kGroupGap = 6;
const int kHairlineHeight = 16;
// The same 28px box the stylesheet pins (playbackwidget.ssa) and the transport's
// own grid uses. setFixedSize, not the QSS pin alone: a layout takes the size hint
// a restyle rewrites, while a fixed size is min = max = 28.
const int kButtonSize = 28;
const int kZoomIconSize = 18;
// The frame readout: 3px between a number and the next, 4px of side padding, and a
// 96px floor, so a short frame number cannot pull the groups beside it inwards.
const int kFrameGap = 3;
const int kFramePad = 4;
const int kFrameMinWidth = 96;
// The easing chip's reserved width. Fixed on purpose: the chip's content changes
// with the key selection, and a control that resizes re-flows the bar beside it.
const int kEaseSlotWidth = 196;
#ifdef AE_ENABLE_IMGUI
// The tab strip: the mock's own numbers for a tab (.tl-tabs button - 11px text,
// 3px/8px of padding, a 4px radius) and the bar it sits in (a 2px gap between the
// two tabs, and the 1x16 rule the mock's border-right draws between the strip and
// the transport is the header's own Hairline widget, as for every other group).
const int kTabFontSize = 11;
const int kTabPadX = 8;
const int kTabPadY = 3;
const int kTabRadius = 4;
#endif
// One wheel notch, in the units ctrl::time::Scaler::update() is called with: the
// angleDelta of one detent, which is the scaler's own kWheelValue. A press is one
// notch, so a button and a wheel step land on the same rung.
const int kZoomNotch = 120;

// The mock's .tl-hairline: the 1x16 rule between two of the bar's groups. Painted
// rather than styled - QSS box rules do not render on plain widget surfaces (the
// same note as the splitter, the dock sash and the header's own bottom hairline).
class Hairline: public QWidget {
public:
    explicit Hairline(QWidget* aParent): QWidget(aParent) { this->setFixedSize(1, kHairlineHeight); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(this->rect(), theme::Colors::current().hairline);
    }
};

// A bar button (the mock's .ib): the glyph-hover pair the transport's buttons use,
// at the group's own icon size. The type is gui::IconButton, not QPushButton: the
// resting/hover glyph pair is that class's API (see refreshZoomIcons).
gui::IconButton* makeBarButton(gui::GUIResources& aResources, QWidget* aParent, const QString& aName,
                               int aIconSize) {
    auto* button = new gui::IconButton(aParent);
    button->setObjectName(aName);
    button->setRestIcon(aResources.icon(aName));
    button->setHoverIcon(aResources.iconActive(aName));
    button->setIconSize(QSize(aIconSize, aIconSize));
    button->setFixedSize(kButtonSize, kButtonSize);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}
} // namespace

namespace gui {

TimeLineHeader::TimeLineHeader(GUIResources& aResources, PlayBackWidget* aTransport, QWidget* aParent):
    QWidget(aParent),
    mGUIResources(aResources),
    mTransport(aTransport),
#ifdef AE_ENABLE_IMGUI
    mTabs(nullptr),
#endif
    mTimeLine(nullptr),
    mInner(nullptr),
    mFrameConnection(),
    mFrameText(nullptr),
    mFrameEndText(nullptr),
    mEaseSlot(nullptr),
    mEasing() {
    // The bar is one fixed 40px row; the surface below keeps its own geometry
    // whatever the bar holds.
    this->setFixedHeight(kHeaderHeight);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(kHeaderPad, 0, kHeaderPad, 0);
    lay->setSpacing(kGroupGap);

#ifdef AE_ENABLE_IMGUI
    // The dock's views, as the bar's FIRST group (the mock's .tl-tabs): the tab
    // names the page, so the Graph page carries no title and no header row of its
    // own - the panel is one component on two floors (design section 5). The control
    // is a real QTabBar, the application's own tab control (gui::ProjectTabBar is one
    // too): keyboard left/right, hover and the selected state all come from Qt, and
    // the mock's border-right between the strip and the transport is the header's own
    // rule, like every other group's.
    mTabs = new QTabBar(this);
    mTabs->setDrawBase(false);
    mTabs->setExpanding(false);
    mTabs->setUsesScrollButtons(false);
    mTabs->setFocusPolicy(Qt::NoFocus);
    mTabs->setMouseTracking(true);
    mTabs->addTab(tr("Timeline"));
    mTabs->addTab(tr("Graph"));
    // The tab order IS Surface's own (Timeline, Graph): onTabChanged maps an index
    // back by position, so the two cannot drift.
    mTabs->setCurrentIndex(static_cast<int>(Surface::Timeline));
    this->refreshTabStyle();
    connect(mTabs, &QTabBar::currentChanged, this, &TimeLineHeader::onTabChanged);
    lay->addWidget(mTabs, 0, Qt::AlignVCenter);
    this->addHairline(*lay);
#endif

    // The transport, moved out of its old column: a fixed-size group, centered in
    // the bar.
    lay->addWidget(mTransport, 0, Qt::AlignVCenter);

    // The frame readout: the frame a key will land on, then the range the playhead
    // is clamped to. Two labels rather than one string, because the design gives the
    // two numbers their own size and ink; Qt has no baseline alignment in a layout
    // box, so both are bottom-aligned in the row, which is the closest thing to the
    // mock's flex baseline. The digits are tabular: the application font's are (the
    // bundled Geist, see Main.cpp), so a changing number cannot reflow the bar.
    auto* frameBox = new QWidget(this);
    frameBox->setMinimumWidth(kFrameMinWidth);
    auto* frameLay = new QHBoxLayout(frameBox);
    frameLay->setContentsMargins(kFramePad, 0, kFramePad, 0);
    frameLay->setSpacing(kFrameGap);
    mFrameText = new QLabel(frameBox);
    mFrameText->setObjectName("frameCurrent");
    auto* frameSep = new QLabel(QStringLiteral("/"), frameBox);
    frameSep->setObjectName("frameSep");
    mFrameEndText = new QLabel(frameBox);
    mFrameEndText->setObjectName("frameEnd");
    frameLay->addWidget(mFrameText, 0, Qt::AlignBottom);
    frameLay->addWidget(frameSep, 0, Qt::AlignBottom);
    frameLay->addWidget(mFrameEndText, 0, Qt::AlignBottom);
    lay->addWidget(frameBox, 0, Qt::AlignVCenter);

    // The mock's first rule: between the frame readout and the easing chip's slot.
    this->addHairline(*lay);

    // The easing chip's slot: the reserved width, with a zero-margin layout so the
    // chip fills it without carrying the geometry (see easingChipSlot()). Fixed on
    // purpose - the chip's content changes with the selection, and the bar beside
    // it must not.
    mEaseSlot = new QWidget(this);
    mEaseSlot->setFixedWidth(kEaseSlotWidth);
    mEaseSlot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* easeLay = new QHBoxLayout(mEaseSlot);
    easeLay->setContentsMargins(0, 0, 0, 0);
    easeLay->setSpacing(0);
    lay->addWidget(mEaseSlot, 0, Qt::AlignVCenter);

    // The easing chip itself: the picker's only home (design section 2). It is
    // built by its own control (prop::EasingItem, which owns the one picker
    // popover) and placed here - the slot is the width, the control is the widget.
    // The chip names the timeline's key selection, which setTimeLineWidget wires.
    mEasing.reset(new prop::EasingItem(mEaseSlot, &aResources));
    easeLay->addWidget(mEasing->itemWidget());
    mEasing->chip()->connect(mEasing->chip(), &QAbstractButton::clicked, this,
                             [this]() { this->onEasingChipClicked(); });

    // The mock's .tl-spacer: the zoom group is the bar's right end, whatever the
    // frame number and the chip's content do.
    lay->addStretch(1);

    // The mock's second rule: between the spacer and the zoom group. (The channel
    // chips the mock puts on this side of it are not built yet.)
    this->addHairline(*lay);

    auto* zoomBox = new QWidget(this);
    auto* zoomLay = new QHBoxLayout(zoomBox);
    zoomLay->setContentsMargins(0, 0, 0, 0);
    zoomLay->setSpacing(0); // the mock's .tl-zoom carries no gap of its own
    gui::IconButton* zoomOut = makeBarButton(aResources, zoomBox, QStringLiteral("minus-bold"), kZoomIconSize);
    zoomOut->setToolTip(tr("Zoom out"));
    gui::IconButton* zoomIn = makeBarButton(aResources, zoomBox, QStringLiteral("plus-bold"), kZoomIconSize);
    zoomIn->setToolTip(tr("Zoom in"));
    gui::IconButton* zoomFit = makeBarButton(aResources, zoomBox, QStringLiteral("arrows-out-cardinal"),
                                             kZoomIconSize);
    zoomFit->setToolTip(tr("Fit the frame range"));
    mZoomButtons.push_back({zoomOut, QStringLiteral("minus-bold")});
    mZoomButtons.push_back({zoomIn, QStringLiteral("plus-bold")});
    mZoomButtons.push_back({zoomFit, QStringLiteral("arrows-out-cardinal")});
    for (gui::IconButton* button : {zoomOut, zoomIn, zoomFit})
        zoomLay->addWidget(button);
    lay->addWidget(zoomBox, 0, Qt::AlignVCenter);

    connect(zoomOut, &QPushButton::clicked, this, [this]() { this->zoomByNotch(false); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { this->zoomByNotch(true); });
    connect(zoomFit, &QPushButton::clicked, this, [this]() { this->fitFrameRange(); });

    // The labels' ink and figures come from the stylesheet, and the glyphs are
    // tinted per theme, so both are re-read on a theme change - the same route the
    // transport takes for its own buttons.
    mGUIResources.onThemeChanged.connect(this, &TimeLineHeader::onThemeUpdated);
    this->refreshFrame();
}

TimeLineHeader::~TimeLineHeader() {
    // The surface holds the control's callbacks (they capture this widget), so it
    // must forget the control before either goes away. This widget is destroyed
    // before the timeline in every construction order the dock uses, and the
    // clearance makes it independent of that order.
    if (mInner)
        mInner->setEasingItemHost(nullptr);
}

void TimeLineHeader::setTimeLineWidget(TimeLineWidget* aTimeLine) {
    if (mTimeLine == aTimeLine)
        return;
    if (mTimeLine)
        mTimeLine->onFrameUpdated.disconnect(mFrameConnection);
    if (mInner)
        mInner->setEasingItemHost(nullptr);
    mTimeLine = aTimeLine;
    if (mTimeLine)
        mFrameConnection = mTimeLine->onFrameUpdated.connect(this, &TimeLineHeader::refreshFrame);

    // The surface widget inside the timeline: it owns the ONE easing picker and
    // pushes the chip's state to the control (setEasingItemHost). It is a direct
    // child, so this is a one-time lookup, not a search per refresh.
    mInner = mTimeLine
                 ? mTimeLine->findChild<TimeLineEditorWidget*>(QString(), Qt::FindDirectChildrenOnly)
                 : nullptr;
    if (mInner)
        mInner->setEasingItemHost(mEasing.data());

    this->refreshFrame();
}

void TimeLineHeader::onEasingChipClicked() {
    emit easingChipActivated();
    if (!mInner || !mTimeLine)
        return;
    ctrl::TimeLineEditor* editor = mTimeLine->editor();
    if (!editor)
        return;

    // The keys the chip names are the editor's live selection, which is also what
    // the picker then edits: the chip is not a label about the selection, it is its
    // control. The popover opens under the chip (the mock's anchoring) through the
    // ONE entry point the key menu uses (design section 2).
    core::TimeLineEvent targets;
    if (!editor->retrieveSelectionTargets(targets))
        return;
    const QWidget* chip = mEasing->itemWidget();
    mInner->openEasingPicker(targets, chip->mapToGlobal(QPoint(0, chip->height())));
}

void TimeLineHeader::addHairline(QHBoxLayout& aLayout) {
    auto* hairline = new Hairline(this);
    aLayout.addWidget(hairline, 0, Qt::AlignVCenter);
}

void TimeLineHeader::refreshFrame() {
    // Both numbers come from the timeline's own state, the one the playback code
    // already reads: the current frame the animator plays and the transport lands on
    // (TimeLineWidget::currentFrame), and the effective playback range the playhead
    // is clamped to - the project's frame range, or the ruler's temporary window
    // while it overrides (TimeLineWidget::playbackRange).
    const int frame = mTimeLine ? mTimeLine->currentFrame().get() : 0;
    const int last = mTimeLine ? mTimeLine->playbackRange().max() : 0;
    mFrameText->setText(QString::number(frame));
    mFrameEndText->setText(QString::number(last));
}

void TimeLineHeader::zoomByNotch(bool aZoomIn) {
    if (!mTimeLine)
        return;

    // The timeline's own anchored zoom, which is the one its wheel and the graph
    // panel both drive: a notch from the buttons and a notch from the wheel are the
    // same call on the same ladder, so the two cannot disagree about where a rung is.
    // A button has no cursor over the lanes, so it anchors on the frame the readout
    // names - the playhead - and that frame keeps its place while the ladder moves
    // one rung: x1.1 a notch, the scaler's own step.
    //
    // The SIGN is the application's own, not the arrow's: Scaler::update SUBTRACTS
    // its argument, so a positive delta is one rung OUT (the same sense the timeline
    // wheel and CanvasMover use - see tests/time_scaler.cpp's notchIn/notchOut).
    // The mouse setting that inverts the timeline's own wheel sense is deliberately
    // NOT applied here: it is a wheel preference, and a + button that zoomed out
    // would not be readable.
    mTimeLine->zoomFramesAbout(aZoomIn ? -kZoomNotch : kZoomNotch,
                               static_cast<double>(mTimeLine->currentFrame().get()));
}

void TimeLineHeader::fitFrameRange() {
    if (!mTimeLine)
        return;
    ctrl::TimeLineEditor* editor = mTimeLine->editor();
    if (!editor)
        return;

    // The timeline's frame axis has no fit of its own - the graph panel's fit lives
    // inside ctrl::graph::Scaler, on THAT panel's axis - so this composes the three
    // primitives the timeline does expose, in the order the panel's own fit uses:
    //
    //   * the ladder's inverse (time::Scaler::indexForRate) for the rung whose rate
    //     still fits,
    //   * the wheel's anchored zoom (zoomFramesAbout) to move to that rung,
    //   * a frame scroll (scrollFramesBy) to put the range's start at the left edge.
    //
    // The span is the frame range's own on-screen extent: the first frame's tick sits
    // one margin in from the content origin, so the range occupies
    // (max - min) * pixelsPerFrame() px, whatever the current zoom.
    const util::Range range = editor->frameRange();
    double lo = 0.0;
    double hi = 0.0;
    mTimeLine->visibleFrameWindow(lo, hi);
    if (!(hi > lo))
        return; // no surface to fit into (an unlaid-out timeline, or no project)

    // The viewport's width in the same units: the fractional-frames window it shows,
    // times the rate it shows them at. Never a whole-frame window: a rounded span
    // would fit to the wrong rung.
    const double viewportPx = (hi - lo) * editor->pixelsPerFrame();
    const double span = std::max(1.0, static_cast<double>(range.diff()));
    const int targetIndex = editor->timeScale().indexForRate(viewportPx / span);
    const int currentIndex = editor->timeScale().index();
    if (targetIndex != currentIndex) {
        mTimeLine->zoomFramesAbout((currentIndex - targetIndex) * kZoomNotch,
                                   static_cast<double>(range.min()));
    }

    // The zoom above holds the range start's pixel; this puts that pixel back on the
    // viewport's left edge, so the fit shows the range from its first frame and no
    // earlier. Re-read the window: the zoom just moved it.
    mTimeLine->visibleFrameWindow(lo, hi);
    mTimeLine->scrollFramesBy(static_cast<double>(range.min()) - lo);
}

void TimeLineHeader::refreshZoomIcons() {
    for (const ZoomButton& zoom : mZoomButtons) {
        zoom.button->setRestIcon(mGUIResources.icon(zoom.icon));
        zoom.button->setHoverIcon(mGUIResources.iconActive(zoom.icon));
    }
}

#ifdef AE_ENABLE_IMGUI
void TimeLineHeader::onTabChanged(int aIndex) {
    // The tab order is Surface's own (see the constructor), so an index maps back by
    // position. An index Qt has no tab for is no view at all: nothing is reported,
    // and the dock's body stays where it is.
    if (aIndex < static_cast<int>(Surface::Timeline) || static_cast<int>(Surface::Graph) < aIndex)
        return;
    emit surfaceChanged(static_cast<Surface>(aIndex));
}

void TimeLineHeader::refreshTabStyle() {
    // The strip's own ink, built from the application's LIVE tokens rather than from
    // a stylesheet asset (the header owns none: playbackwidget.ssa styles the bar's
    // buttons, not a QTabBar, and this bar is not the place to add one). The roles
    // are the ones the application's own tab bar is written against (ProjectTabBar's
    // modetabbar.ssa): muted text at rest, `raised` under the pointer, and the ACCENT
    // fill - with the accent's own content colour - for the selected tab; the strip
    // itself is transparent, because it sits on the header's own floor (the dock's
    // base) and not on a surface of its own. The numbers are the mock's .tl-tabs.
    const theme::Colors& colors = theme::Colors::current();
    mTabs->setStyleSheet(
        QStringLiteral(
            "QTabBar { background: transparent; }"
            "QTabBar::tab { color: %1; background: transparent; border: none;"
            " border-radius: %2px; padding: %3px %4px; margin: 0; font-size: %5px; }"
            "QTabBar::tab:selected { color: %6; background: %7; }"
            "QTabBar::tab:hover:!selected { color: %8; background: %9; }"
        )
            .arg(colors.textMuted.name())
            .arg(kTabRadius)
            .arg(kTabPadY)
            .arg(kTabPadX)
            .arg(kTabFontSize)
            .arg(colors.accentText.name())
            .arg(colors.accent.name())
            .arg(colors.text.name())
            .arg(colors.raised.name())
    );
    mTabs->update();
}
#endif

void TimeLineHeader::onThemeUpdated(theme::Theme& aTheme) {
    this->setStyleSheet(aTheme.loadStylesheet("playbackwidget.ssa"));
    this->refreshZoomIcons();
#ifdef AE_ENABLE_IMGUI
    // The strip's ink is built from the tokens, not from a sheet: re-build it here so
    // an accent change (which re-applies the app's palette) re-inks the tabs too.
    this->refreshTabStyle();
#endif
    // The chip paints from the live palette, but its caret pixmap is tinted through
    // the icon pipeline and must be re-loaded for the new appearance.
    mEasing->refreshIcon();
    this->update();
}

void TimeLineHeader::paintEvent(QPaintEvent*) {
    // The bar's own section separator: the 1px hairline on its bottom edge, which is
    // what separates the header from the ruler below it. Painted here because QSS box
    // rules do not render on plain widget surfaces (the transport used to paint its
    // own left-edge hairline in this same place).
    QPainter painter(this);
    painter.fillRect(0, this->height() - 1, this->width(), 1, theme::Colors::current().hairline);
}

} // namespace gui
