#include "gui/TimeLineHeader.h"
#include "gui/IconButton.h"
#include "gui/MainDisplayWidget.h"
#include "gui/PlayBackWidget.h"
#include "gui/TimeLineWidget.h"
#include "gui/TimeLineEditorWidget.h"
#include "gui/prop/prop_Items.h"
#include "gui/theme/Colors.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/graph/graph_Channel.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/time/time_Scaler.h"
#include <QAbstractButton>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#ifdef AE_ENABLE_IMGUI
#include "gui/GraphPanelWidget.h"
#include <QTabBar>
#endif
#include <algorithm>
#include <functional>

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
// The easing chip's reserved width. Fixed on purpose: the chip's content changes
// with the key selection, and a control that resizes re-flows the bar beside it.
const int kEaseSlotWidth = 196;
// The bar's TEXT buttons (the four quick easings and the channels trigger): the
// mock's .gp-chbtn numbers - 10px text, 6px of side padding, a 4px radius and a
// 28px box (the bar's own grid) - and the 2px between the quick easings.
const int kTextButtonSize = 10;
const int kTextButtonPadX = 6;
const int kTextButtonRadius = 4;
const int kQuickEaseGap = 2;
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

// A TEXT BUTTON in the bar's own vocabulary: the mock's .gp-chbtn - 10px text at the
// muted ink, `raised` under the pointer, and the ACCENT fill with the accent's content
// colour while it is CHECKED (the quick easings are lit that way; the channels button
// is not checkable). It paints from the LIVE tokens rather than from a stylesheet,
// because the header owns no sheet of its own (playbackwidget.ssa styles the transport,
// not this) - the same rule the tab strip follows. gui::IconButton, the bar's other
// button, is the glyph-hover pair and has no text.
class HeaderTextButton: public QAbstractButton {
public:
    explicit HeaderTextButton(QWidget* aParent): QAbstractButton(aParent) {
        // Keyboard reachable: Tab moves between the bar's text buttons and Space (or
        // Return, handled below) activates one. The focus ring is painted.
        this->setFocusPolicy(Qt::StrongFocus);
        this->setFixedHeight(kButtonSize);
    }

    QSize sizeHint() const override {
        QFont font = this->font();
        font.setPixelSize(kTextButtonSize);
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(this->text()) + 2 * kTextButtonPadX;
        return QSize(std::max(kButtonSize, width), kButtonSize);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const theme::Colors& colors = theme::Colors::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF box = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (this->isEnabled() && (this->isChecked() || this->isDown() || this->underMouse())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(this->isChecked() ? QBrush(colors.accent) : QBrush(colors.raised));
            painter.drawRoundedRect(box, kTextButtonRadius, kTextButtonRadius);
        }

        QFont font = this->font();
        font.setPixelSize(kTextButtonSize);
        painter.setFont(font);
        const QColor ink = !this->isEnabled() ? colors.textMutedDim
            : this->isChecked()               ? colors.accentText
            : this->underMouse()              ? colors.text
                                              : colors.textMuted;
        painter.setPen(ink);
        painter.drawText(this->rect(), Qt::AlignCenter, this->text());

        if (this->hasFocus()) {
            painter.setPen(QPen(colors.accent, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, kTextButtonRadius, kTextButtonRadius);
        }
    }

    void keyPressEvent(QKeyEvent* aEvent) override {
        // Return activates like Space: QAbstractButton only binds Space (and Select),
        // and a bar of buttons with no dialog around it has no auto-default machinery
        // to borrow.
        if (aEvent->key() == Qt::Key_Return || aEvent->key() == Qt::Key_Enter) {
            this->click();
            aEvent->accept();
            return;
        }
        QAbstractButton::keyPressEvent(aEvent);
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
    mSelectionSlot(),
    mEaseSlot(nullptr),
    mEasing(),
    mQuickEases(),
#ifdef AE_ENABLE_IMGUI
    mChannelsButton(nullptr),
#endif
    mZoomButtons() {
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

    // The mock's rule between the transport and the chip: the frame readout used to
    // sit between them and is gone (the playhead pill names the current frame and the
    // ruler the end frame), so this is the mock's own .tl-hairline - one group's end,
    // the next one's start.
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

    // THE FOUR QUICK EASINGS, beside the chip: the picker's four common tiles as one
    // click each, on the same selection the chip names. They are the TYPE shortcut -
    // the range and the blend stay the key's own, exactly as the picker's tiles leave
    // them - and they read back which of the four the selection agrees on.
    // The four types, in the picker catalog's own display head (its first tile row).
    // Their labels come from util::Easing::getTypeName - the application's one public
    // type vocabulary, which the picker's menus and the general settings also read -
    // rather than from literals here, so the bar cannot introduce a name of its own.
    // getTypeName is already translated (its "EasingName" context), so its result is
    // used as-is and must not be wrapped in tr() again.
    const util::Easing::Type quickDefs[] = {
        util::Easing::Type_Linear,
        util::Easing::Type_Floor,
        util::Easing::Type_Round,
        util::Easing::Type_Auto,
    };
    auto* quickBox = new QWidget(this);
    auto* quickLay = new QHBoxLayout(quickBox);
    quickLay->setContentsMargins(0, 0, 0, 0);
    quickLay->setSpacing(kQuickEaseGap);
    for (const util::Easing::Type type : quickDefs) {
        auto* button = new HeaderTextButton(quickBox);
        const QString name = util::Easing::getTypeName(type);
        button->setText(name);
        button->setToolTip(tr("Set the selected keys' easing to %1").arg(name));
        button->setCheckable(true);
        button->setEnabled(false); // until a selection names a key that carries an easing
        quickLay->addWidget(button);
        mQuickEases.push_back({button, type});
        connect(button, &QAbstractButton::clicked, this, [this, type]() { this->onQuickEasingClicked(type); });
    }
    lay->addWidget(quickBox, 0, Qt::AlignVCenter);

    // The mock's .tl-spacer: the zoom group is the bar's right end, whatever the
    // chip's content and the quick easings' widths do.
    lay->addStretch(1);

#ifdef AE_ENABLE_IMGUI
    // THE CHANNELS TRIGGER: the panel's ONE control, in the dock's header (the mock's
    // .tl-chans - a view action, so it sits with the view controls and not in the
    // panel's own ruler band). Hidden while the Timeline tab is the dock's view, since
    // there is no panel to control then (see onTabChanged) - but its BOX stays in the
    // layout. The dock is sized from this bar's minimum width, and the trigger is the
    // one item whose visibility changes with the tab, so a hidden trigger used to take
    // its box plus its group gap out of that minimum and move the dock (and the tree
    // splitter) on every tab switch.
    mChannelsButton = new HeaderTextButton(this);
    mChannelsButton->setToolTip(tr("Which channels are drawn"));
    // The box is FIXED, not the live label width: the trigger's words follow the
    // panel's rows ("6 channels" / "4 of 6"), and a count change must not re-flow the
    // bar any more than a tab change may. The width is the widest three-digit label,
    // measured in the button's own 10px text (the same rule HeaderTextButton's
    // sizeHint uses) plus its 6px side padding.
    QFont channelsFont = mChannelsButton->font();
    channelsFont.setPixelSize(kTextButtonSize);
    const QFontMetrics channelsMetrics(channelsFont);
    const int channelsWidth =
        std::max(channelsMetrics.horizontalAdvance(tr("%n channel(s)", nullptr, 999)),
                 channelsMetrics.horizontalAdvance(tr("%1 of %2").arg(999).arg(999)))
        + 2 * kTextButtonPadX;
    mChannelsButton->setFixedWidth(std::max(kButtonSize, channelsWidth));
    // ...and the fixed box is reserved while the button is hidden: QWidgetItem::
    // isEmpty() stays false for a hidden widget whose policy retains its size, so the
    // box layout keeps the item's size hint (the fixed width and height) as its own -
    // hiding the trigger now changes nothing in the row's minimum width.
    QSizePolicy channelsPolicy = mChannelsButton->sizePolicy();
    channelsPolicy.setRetainSizeWhenHidden(true);
    mChannelsButton->setSizePolicy(channelsPolicy);
    mChannelsButton->setVisible(false);
    connect(mChannelsButton, &QAbstractButton::clicked, this, [this]() { this->onChannelsClicked(); });
    lay->addWidget(mChannelsButton, 0, Qt::AlignVCenter);
#endif

    // The mock's second rule: between the channels trigger and the zoom group. (The
    // channel chips the mock puts on this side of it are not built yet.)
    this->addHairline(*lay);

    auto* zoomBox = new QWidget(this);
    auto* zoomLay = new QHBoxLayout(zoomBox);
    zoomLay->setContentsMargins(0, 0, 0, 0);
    zoomLay->setSpacing(0); // the mock's .tl-zoom carries no gap of its own
    gui::IconButton* zoomOut = makeBarButton(aResources, zoomBox, QStringLiteral("minus-bold"), kZoomIconSize);
    zoomOut->setToolTip(tr("Zoom out"));
    gui::IconButton* zoomIn = makeBarButton(aResources, zoomBox, QStringLiteral("plus-bold"), kZoomIconSize);
    zoomIn->setToolTip(tr("Zoom in"));
    // THE FRAME AXIS' OWN FIT (the action fits the frames, not the value axis): the
    // horizontal-only expand, not the cardinal one, so the button says which axis it
    // fits.
    gui::IconButton* zoomFit = makeBarButton(aResources, zoomBox, QStringLiteral("arrows-out-line-horizontal"),
                                             kZoomIconSize);
    zoomFit->setToolTip(tr("Fit the frame range"));
    mZoomButtons.push_back({zoomOut, QStringLiteral("minus-bold")});
    mZoomButtons.push_back({zoomIn, QStringLiteral("plus-bold")});
    mZoomButtons.push_back({zoomFit, QStringLiteral("arrows-out-line-horizontal")});
    for (gui::IconButton* button : {zoomOut, zoomIn, zoomFit})
        zoomLay->addWidget(button);
    lay->addWidget(zoomBox, 0, Qt::AlignVCenter);

    connect(zoomOut, &QPushButton::clicked, this, [this]() { this->zoomByNotch(false); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { this->zoomByNotch(true); });
    connect(zoomFit, &QPushButton::clicked, this, [this]() { this->fitFrameRange(); });

    // The glyphs are tinted per theme, so they are re-read on a theme change - the
    // same route the transport takes for its own buttons.
    mGUIResources.onThemeChanged.connect(this, &TimeLineHeader::onThemeUpdated);
    this->refreshFrame();
}

TimeLineHeader::~TimeLineHeader() {
    // The surface holds the control's callbacks (they capture this widget), so it
    // must forget the control before either goes away. This widget is destroyed
    // before the timeline in every construction order the dock uses, and the
    // clearance makes it independent of that order.
    if (mInner) {
        mInner->setEasingItemHost(nullptr);
        if (mInner->editor() && mSelectionSlot.isValid())
            mInner->editor()->selection().onChanged.disconnect(mSelectionSlot);
    }
}

void TimeLineHeader::setTimeLineWidget(TimeLineWidget* aTimeLine) {
    if (mTimeLine == aTimeLine)
        return;
    if (mInner) {
        mInner->setEasingItemHost(nullptr);
        if (mInner->editor() && mSelectionSlot.isValid())
            mInner->editor()->selection().onChanged.disconnect(mSelectionSlot);
    }
    mSelectionSlot = util::SlotId();

    mTimeLine = aTimeLine;

    // The surface widget inside the timeline: it owns the ONE easing picker and
    // pushes the chip's state to the control (setEasingItemHost). It is a direct
    // child, so this is a one-time lookup, not a search per refresh.
    mInner = mTimeLine
                 ? mTimeLine->findChild<TimeLineEditorWidget*>(QString(), Qt::FindDirectChildrenOnly)
                 : nullptr;
    if (mInner) {
        this->bindEasingHost();
        // The live selection's own change notification: whichever view moved the
        // selection (a click on the lanes, a marquee, a right-click's temporary loan,
        // the graph panel), this is what re-reads the quick easings. The chip is pushed
        // from the same source (TimeLineEditorWidget::pushEasingChipState).
        if (mInner->editor())
            mSelectionSlot =
                mInner->editor()->selection().onChanged.connect(this, &TimeLineHeader::refreshQuickEasings);
    }

    this->refreshFrame();
}

void TimeLineHeader::bindEasingHost() {
    if (!mInner)
        return;

    // The chip and the quick easings read ONE state (the timeline's live selection).
    // The chip is pushed its state by the surface (setEasingItemHost wires the
    // callbacks that write the document); those callbacks capture the surface, so
    // binding the host is what this bar has to do - and the picker's COMMIT is the one
    // easing write that does not move the selection, so the bar's own re-read is
    // composed with the host's callback rather than left to a signal that never fires.
    mInner->setEasingItemHost(mEasing.data());
    const std::function<void(util::Easing::Param)> applied = mEasing->onEasingApplied;
    mEasing->onEasingApplied = [this, applied](util::Easing::Param aEasing) {
        if (applied)
            applied(aEasing);
        this->refreshQuickEasings();
    };
    const std::function<void()> closed = mEasing->onPickerClosed;
    mEasing->onPickerClosed = [this, closed]() {
        if (closed)
            closed();
        this->refreshQuickEasings();
    };
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

void TimeLineHeader::onQuickEasingClicked(util::Easing::Type aType) {
    // THE SAME EDIT AS THE PICKER'S TILE, through the application's own
    // selection-easing action. The action lives on the panel's editor
    // (ctrl::GraphEditor::setSelectedKeysEasing), which holds the TIMELINE'S live
    // selection (MainDisplayWidget::syncGraphPanelSharedState wires it with
    // setSelectionSource), so this button needs no copy of the edit path: one macro,
    // one history entry for the whole selection, and the same skips the picker's own
    // commit makes. The editor is reached through the widget that owns it.
    MainDisplayWidget* display = this->window() ? this->window()->findChild<MainDisplayWidget*>() : nullptr;
    if (display)
        display->applyEasingToSelection(aType);

    // The document's own notification re-reads the chip and the surfaces; the BAR's
    // state is re-read here, because the selection itself did not move.
    this->refreshQuickEasings();
}

void TimeLineHeader::refreshQuickEasings() {
    // WHICH OF THE FOUR IS ACTIVE, and whether any of them can act at all, from the
    // timeline's own live selection: every selected key that carries an easing and
    // whose channel playback interpolates (the image offset does not, and
    // setSelectedKeysEasing refuses it) votes for its type. Nothing to vote with - no
    // selection, or only keys of types with no easing - leaves every button disabled;
    // a split vote lights none of them.
    ctrl::TimeLineEditor* editor = mTimeLine ? mTimeLine->editor() : nullptr;
    bool votable = false;
    bool mixed = false;
    util::Easing::Type active = util::Easing::Type_TERM;

    if (editor) {
        for (const ctrl::time::Selection::Item& item : editor->selection().items()) {
            if (!item.key)
                continue;
            core::TimeKeyPos pos = item.target.pos;
            core::TimeKey* key = pos.key();
            if (!key)
                continue; // a stale entry names a key the document no longer holds
            const util::Easing::Param* param = ctrl::graph::Curve::easingOf(*key, pos.type());
            if (!param)
                continue; // a key of a type that carries no easing
            if (!ctrl::graph::Channel(pos.type(), 0).isInterpolated())
                continue; // step-held: playback never consults its param
            if (!votable) {
                votable = true;
                active = param->type;
            } else if (active != param->type) {
                mixed = true;
            }
        }
    }

    for (const QuickEase& quick : mQuickEases) {
        quick.button->setEnabled(votable);
        quick.button->setChecked(votable && !mixed && active == quick.type);
    }
}

void TimeLineHeader::addHairline(QHBoxLayout& aLayout) {
    auto* hairline = new Hairline(this);
    aLayout.addWidget(hairline, 0, Qt::AlignVCenter);
}

void TimeLineHeader::refreshFrame() {
    // The frame readout this used to re-read is gone (the playhead pill and the ruler
    // name both numbers), so what is left to re-read is the state the dock's two calls
    // still owe the bar: the quick easings' availability/active type (which follows the
    // timeline's editor, and the editor changes with the project) and, in a build with
    // the panel, the channels button's own words.
    this->refreshQuickEasings();
#ifdef AE_ENABLE_IMGUI
    GraphPanelWidget* panel = this->graphPanelWidget();
    this->updateChannelsButton(panel ? panel->channelRows() : QVector<PanelChannelRow>());
#endif
}

void TimeLineHeader::zoomByNotch(bool aZoomIn) {
    if (!mTimeLine)
        return;

    // The timeline's own anchored zoom, which is the one its wheel and the graph
    // panel both drive: a notch from the buttons and a notch from the wheel are the
    // same call on the same ladder, so the two cannot disagree about where a rung is.
    // A button has no cursor over the lanes, so it anchors on the frame the playhead
    // names and that frame keeps its place while the ladder moves one rung: x1.1 a
    // notch, the scaler's own step.
    //
    // The SIGN is the application's own, not the arrow's: Scaler::update SUBTRACTS
    // its argument, so a positive delta is one rung OUT (the same sense the timeline
    // wheel and CanvasMover use - see tests/time_scaler.cpp's notchIn/notchOut).
    // The mouse setting that inverts the timeline's own wheel sense is deliberately
    // NOT applied here: it is a wheel preference, and a + button that zoomed out
    // would not be readable.
    //
    // The panel that draws this frame window is repainted by the window's own report:
    // the zoom moves the timeline's view transform, TimeLineWidget::notifyViewRange
    // fires onViewRangeChanged, and MainDisplayWidget::syncGraphPanelSharedState -
    // which is what pushes the window into the panel's editor - refreshes the dock's
    // Graph page with it.
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
    //
    // Each primitive that really moves the view reports it (onViewRangeChanged), and
    // that report is what repaints the docked panel with the new window
    // (MainDisplayWidget::syncGraphPanelSharedState).
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
GraphPanelWidget* TimeLineHeader::graphPanelWidget() const {
    // The dock's Graph page, through the WINDOW: the header is constructed with a
    // GUIResources, a transport and a parent, and the dock's body is a sibling of this
    // bar - the same route gui::GraphPanelWidget takes to reach the application's
    // menus (there is exactly one page per window).
    QWidget* top = this->window();
    return top ? top->findChild<GraphPanelWidget*>() : nullptr;
}

void TimeLineHeader::updateChannelsButton(const QVector<PanelChannelRow>& aRows) {
    if (!mChannelsButton)
        return;

    // The button's words are its own: the total while every channel is shown, "shown of
    // total" once one is hidden - the count IS the state the button changes. The panel no
    // longer publishes this control or its text (the button is a header control now). No
    // rows at all means no list to open, so the control is disabled rather than opening
    // an empty popup.
    int shown = 0;
    for (const PanelChannelRow& row : aRows) {
        if (row.visible)
            ++shown;
    }
    const int total = aRows.size();
    mChannelsButton->setEnabled(total > 0);
    if (total <= 0)
        mChannelsButton->setText(tr("%n channel(s)", nullptr, 0));
    else if (shown == total)
        mChannelsButton->setText(tr("%n channel(s)", nullptr, total));
    else
        mChannelsButton->setText(tr("%1 of %2").arg(shown).arg(total));
    mChannelsButton->updateGeometry();
}

void TimeLineHeader::onChannelsClicked() {
    MainDisplayWidget* display = this->window() ? this->window()->findChild<MainDisplayWidget*>() : nullptr;
    GraphPanelWidget* panel = this->graphPanelWidget();
    if (!display || !panel)
        return;

    // The rows are the page's own model rows, built on demand (the page may not have
    // painted since the tab switched): one read for the button's words and one for the
    // list, so the two cannot show different counts.
    const QVector<PanelChannelRow> rows = panel->channelRows();
    this->updateChannelsButton(rows);
    if (rows.isEmpty())
        return; // no channels: no list to open

    // ANCHORED UNDER THE BUTTON, RIGHT-ALIGNED TO IT (the mock's .tl-chanpop): the
    // anchor is the button's bottom-right corner, mapped into the display's own pixels
    // because that is the object that owns the popup and maps it to the screen. The
    // two widgets are SIBLINGS (the dock's bar and the viewport), not parent and child,
    // so the mapping goes through GLOBAL coordinates - QWidget::mapTo/mapFrom assume
    // the target is an ancestor.
    const QPoint anchorGlobal =
        mChannelsButton->mapToGlobal(QPoint(mChannelsButton->width(), mChannelsButton->height()));
    display->showGraphChannelMenu(rows, display->mapFromGlobal(anchorGlobal));
}

void TimeLineHeader::onTabChanged(int aIndex) {
    // The tab order is Surface's own (see the constructor), so an index maps back by
    // position. An index Qt has no tab for is no view at all: nothing is reported,
    // and the dock's body stays where it is.
    if (aIndex < static_cast<int>(Surface::Timeline) || static_cast<int>(Surface::Graph) < aIndex)
        return;

    // The channels control belongs to the Graph page (there is no panel on the
    // Timeline tab), so it comes and goes with the tab, and its words are re-read as
    // it appears: the count is the panel's own and a key edit beside the dock changed
    // it while the tab was away.
    if (mChannelsButton) {
        const bool graph = static_cast<Surface>(aIndex) == Surface::Graph;
        if (graph && !mChannelsButton->isVisible()) {
            GraphPanelWidget* panel = this->graphPanelWidget();
            this->updateChannelsButton(panel ? panel->channelRows() : QVector<PanelChannelRow>());
        }
        mChannelsButton->setVisible(graph);
    }

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
    // The bar's text buttons paint from the live tokens too, so a theme change is a
    // repaint and nothing else.
    for (const QuickEase& quick : mQuickEases)
        quick.button->update();
#ifdef AE_ENABLE_IMGUI
    if (mChannelsButton)
        mChannelsButton->update();
#endif
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
