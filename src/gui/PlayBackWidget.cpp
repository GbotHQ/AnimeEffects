#include "gui/PlayBackWidget.h"
#include <QFile>
#include <QPainter>
#include <QPoint>
#include <QResizeEvent>
#include <QTimer>
#include <QTextStream>
#include <functional>
#include "gui/theme/Colors.h"

namespace {
constexpr double kButtonSize = 24.0 * 1.2;
constexpr double kIconSize = 18.0 * 1.2;
constexpr int kGap = 4;
constexpr int kButtonCount = 8;
constexpr int kEdgeMargin = 4; ///< padding at the row's left and right ends
constexpr int kTopMargin = 3;  ///< padding above the row
constexpr int kGroupGap = 22;  ///< between groups, wider than the gap inside one

/// The row's groups, read left to right. The split is by meaning, not by
/// symmetry:
///
///  - the transport (rewind, step back, play, step on, fast forward) is what
///    the panel is *for*, so it starts at the reading edge;
///  - the loop toggle is a mode of that whole transport rather than another
///    step in it, so it stands apart, centred;
///  - animation settings and the audio track open dialogs and configure the
///    panel rather than driving it, so they sit at the trailing edge, out of
///    the transport's run.
///
/// The bar used to be a column beside the timeline, which made its button stack
/// the animation panel's minimum height (262px, so the panel could never be
/// shorter than 290) - a floor no other panel had. As a row above the timeline
/// it is 35px tall instead.
struct ButtonGroup {
    int first;
    int count;
};
constexpr ButtonGroup kGroups[] = {{0, 5}, {5, 1}, {6, 2}};
constexpr int kGroupCount = int(sizeof(kGroups) / sizeof(kGroups[0]));

/// Total width the row needs: the buttons, the gaps inside each group, and the
/// gaps between the groups.
int rowWidth() {
    const int step = int(kButtonSize) + kGap;
    int width = kEdgeMargin * 2;
    for (int g = 0; g < kGroupCount; ++g) {
        width += step * kGroups[g].count - kGap;
        if (g + 1 < kGroupCount) width += kGroupGap;
    }
    return width;
}

// A playback button whose glyph brightens to the active (max-contrast) color
// on hover — the icon participates in the hover feedback, not just the
// background. setRestIcon() records the resting icon, so the play/pause glyph
// swap keeps working; the hover icon is set explicitly via setHoverIcon and
// re-set on theme changes. (QAbstractButton::setIcon is not virtual, so the
// rest icon is tracked through the explicit setter only.)
class IconButton : public QPushButton {
public:
    using QPushButton::QPushButton;
    void setRestIcon(const QIcon& aIcon) { mRest = aIcon; QPushButton::setIcon(aIcon); }
    void setHoverIcon(const QIcon& aIcon) { mHover = aIcon; }
protected:
    void enterEvent(QEnterEvent*) override {
        if (isEnabled() && !mHover.isNull()) QPushButton::setIcon(mHover);
    }
    void leaveEvent(QEvent*) override { QPushButton::setIcon(mRest); }
private:
    QIcon mRest;
    QIcon mHover;
};
} // namespace

namespace gui {

PlayBackWidget::PlayBackWidget(GUIResources& aResources, QWidget* aParent, core::Project& mProject):
    QWidget(aParent), mGUIResources(aResources), mButtons(), mDoesLoop(true) {
    // A row above the timeline: fixed height, and the layout may stretch it
    // sideways (the buttons stay left-anchored).
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    aProject = &mProject;

    mButtons.push_back(createButton("skip-back", false, 0, tr("Return to initial frame")));
    mButtons.push_back(createButton("step-back", false, 1, tr("One frame back")));
    mButtons.push_back(createButton("play", true, 2, tr("Play")));
    mButtons.push_back(createButton("step-forward", false, 3, tr("One frame forward")));
    mButtons.push_back(createButton("skip-forward", false, 4, tr("Advance to final frame")));
    mButtons.push_back(createButton("arrows-clockwise", true, 5, tr("Loop animation")));
    mButtons.back()->setChecked(mDoesLoop);
    mButtons.push_back(createButton("faders-horizontal", false, 6, tr("Animation settings")));
    mButtons.push_back(createButton("speaker-high", false, 7, tr("Audio track")));
    // Frame nudge buttons repeat on hold: fire once on press, then every
    // 50ms after a 400ms hold (QAbstractButton re-emits pressed()).
    for (const int column : {1, 3}) {
        QPushButton* b = mButtons.at(column);
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(400);
        b->setAutoRepeatInterval(50);
    }
    audioWidget->setupUi(audioUI, &mediaPlayer, aConf);
    mGUIResources.onThemeChanged.connect(this, &PlayBackWidget::onThemeUpdated);
    layoutButtons();
    // The audio window's speaker glyph is a runtime-tinted icon; refresh it
    // (and let the ToolSlider repaint) on theme changes.
    mGUIResources.onThemeChanged.connect(audioWidget, &AudioPlaybackWidget::onThemeUpdated);
}

void PlayBackWidget::setPushDelegate(const PushDelegate& aDelegate) {
    PlayBackWidget* owner = this;
    mPushDelegate = aDelegate;

    connect(mButtons.at(2), &QPushButton::pressed, [=]() {
        const bool isChecked = owner->mButtons.at(2)->isChecked();
        const char* name = isChecked ? "play" : "pause";
        owner->setButtonIcon(2, name);
        owner->mButtons.at(2)->setToolTip(isChecked ? tr("Play") : tr("Pause"));
        owner->mPushDelegate(isChecked ? PushType_Pause : PushType_Play);
    });
    // clicked (release), not pressed: the handler opens a modal exec() loop,
    // which would swallow the mouse release and leave the button stuck in the
    // pressed visual state.
    connect(mButtons.at(6), &QPushButton::clicked, [=]() { owner->mPushDelegate(PushType_Options); });

    // Loop toggle: checked state IS the live playback loop; the timeline
    // mirrors it via setPlayBackLoop and the loop shortcut via checkLoop()
    // (the Project menu's "Loop animation" is a separate persisted attribute).
    connect(mButtons.at(5), &QPushButton::clicked, [=]() {
        owner->mDoesLoop = owner->mButtons.at(5)->isChecked();
        owner->mPushDelegate(PushType_Loop);
    });

    connect(mButtons.at(0), &QPushButton::pressed, [=]() { owner->mPushDelegate(PushType_Rewind); });
    connect(mButtons.at(1), &QPushButton::pressed, [=]() { owner->mPushDelegate(PushType_StepBack); });
    connect(mButtons.at(3), &QPushButton::pressed, [=]() { owner->mPushDelegate(PushType_Step); });
    connect(mButtons.at(4), &QPushButton::pressed, [=]() { owner->mPushDelegate(PushType_Fast); });
    connect(mButtons.at(7), &QPushButton::pressed, [=](){
        if(audioUI == nullptr || audioWidget == nullptr){
            audioWidget =  new AudioPlaybackWidget;
            audioUI = new QWidget(this, Qt::Window);
            audioWidget->setupUi(audioUI, &mediaPlayer, aConf);
        }

        if(aProject && aProject->mediaRefresh){
            owner->aConf = aProject->pConf;
            owner->mediaPlayer = *aProject->mediaPlayer;
            aProject->mediaRefresh = false;
        }
        if(!audioUI->isHidden()){
            return;
        }
        for(auto player: mediaPlayer.players){ player->stop(); }

        if(aProject && aProject->uiRefresh){
            audioWidget->rectifyUI(aProject->pConf, aProject->mediaPlayer, true);
            aProject->uiRefresh = false;
        }

        audioUI->show();
    });
}

bool PlayBackWidget::isLoopChecked() {
    return mDoesLoop;
}

void PlayBackWidget::checkLoop(bool checkStatus) {
    mDoesLoop = checkStatus;
    if (mButtons.size() > 5) {
        mButtons.at(5)->setChecked(checkStatus);
    }
}

void PlayBackWidget::PlayPause() {
    PlayBackWidget* owner = this;
    bool isChecked = owner->mButtons.at(2)->isChecked();
    // There are no functions currently available to check for playback, this'll do for now
    if (!isChecked) {
        auto name = "pause";
        owner->setButtonIcon(2, name);
        owner->mButtons.at(2)->setToolTip(tr("Pause"));
        owner->mPushDelegate(PushType_Play);
        owner->mButtons.at(2)->setChecked(true);
    } else {
        auto name = "play";
        owner->setButtonIcon(2, name);
        owner->mButtons.at(2)->setToolTip(tr("Play"));
        owner->mPushDelegate(PushType_Pause);
        owner->mButtons.at(2)->setChecked(false);
    }
}

QSize PlayBackWidget::sizeHint() const {
    return QSize(rowWidth(), kButtonSize + kTopMargin * 2);
}

QSize PlayBackWidget::minimumSizeHint() const {
    return sizeHint();
}

void PlayBackWidget::paintEvent(QPaintEvent* aEvent) {
    // Nothing is painted, deliberately. There used to be a hairline under the
    // bar, left over from when the bar was a column and the line was the
    // boundary it shared with the timeline; as a row above the timeline that
    // line separates nothing that needs separating, and the timeline draws its
    // own header right below. The override stays so the universal QWidget
    // background rule in the QSS cannot fill the row.
    Q_UNUSED(aEvent);
}

/// Places the buttons: one run per group, the first from the left edge, the last
/// from the right edge, and the one in between centred. Positions depend on the
/// bar's width, so this runs on every resize.
void PlayBackWidget::layoutButtons() {
    const int step = int(kButtonSize) + kGap;
    const auto place = [this, step](int aFrom, int aCount, int aStartX) {
        int x = aStartX;
        for (int i = aFrom; i < aFrom + aCount && i < int(mButtons.size()); ++i) {
            mButtons[i]->setGeometry(x, kTopMargin, int(kButtonSize), int(kButtonSize));
            x += step;
        }
    };

    place(kGroups[0].first, kGroups[0].count, kEdgeMargin);

    const int middleWidth = step * kGroups[1].count - kGap;
    place(kGroups[1].first, kGroups[1].count, (width() - middleWidth) / 2);

    const int trailingWidth = step * kGroups[2].count - kGap;
    place(kGroups[2].first, kGroups[2].count, width() - kEdgeMargin - trailingWidth);
}

void PlayBackWidget::resizeEvent(QResizeEvent* aEvent) {
    QWidget::resizeEvent(aEvent);
    layoutButtons();
}

void PlayBackWidget::pushPauseButton() {
    QPushButton* button = mButtons.at(2);
    if (button->isChecked()) {
        setButtonIcon(2, QStringLiteral("play"));
        button->setChecked(false);
        mPushDelegate(PushType_Pause);
    }
}

void PlayBackWidget::setButtonIcon(int aIndex, const QString& aName) {
    auto* b = static_cast<IconButton*>(mButtons.at(aIndex));
    b->setRestIcon(mGUIResources.icon(aName));
    b->setHoverIcon(mGUIResources.iconActive(aName));
}

QPushButton*
PlayBackWidget::createButton(const QString& aName, bool aIsCheckable, int aColumn, const QString& aToolTip) {
    auto* button = new IconButton(this);
    XC_PTR_ASSERT(button);
    button->setObjectName(aName);
    button->setRestIcon(mGUIResources.icon(aName));
    button->setHoverIcon(mGUIResources.iconActive(aName));
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setCheckable(aIsCheckable);
    if (aIsCheckable) {
        // active-state button: press reads as hover, not the sunken press
        button->setProperty("activeButton", true);
    }
    button->setToolTip(aToolTip);
    button->setFocusPolicy(Qt::NoFocus);
    // The QSS width/height rule pins the box; geometry alone is not enough
    // because theme restyles resize absolute children to the style's
    // sizeFromContents (icon + button margins -> 29px tall).
    return button;
}

void PlayBackWidget::onThemeUpdated(theme::Theme& aTheme) {
    this->setStyleSheet(aTheme.loadStylesheet("playbackwidget.ssa"));

    // Theme restyles (the cascade through MainWindow) resize these
    // absolute-positioned children to the style's sizeFromContents (icon +
    // button margins), which sticks at 29px tall even after the QSS pins
    // the sizeHint to 24x24. Re-assert the fixed grid once the restyle
    // cascade has settled.
    QTimer::singleShot(0, [this]() { layoutButtons(); });

    if (!mButtons.empty()) {
        for (int i = 0; i < (int)mButtons.size(); ++i) {
            // The play button shows pause while playing; its objectName stays
            // "play", so pick the glyph by the check state here.
            const QString name = (i == 2 && mButtons.at(2)->isChecked())
                                     ? QStringLiteral("pause") : mButtons.at(i)->objectName();
            setButtonIcon(i, name);
        }
    }
}
bool PlayBackWidget::isPlaying() {  return this->mButtons.at(2)->isChecked(); }

} // namespace gui
