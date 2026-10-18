#include "gui/TargetWidget.h"
#include "gui/AnimationSettingDialog.h"
#include "gui/MainMenuBar.h"
#include "gui/TimeLineHeader.h"
#include "cmnd/BasicCommands.h"
#include "cmnd/ScopedMacro.h"
#include "ctrl/CmndName.h"
#include "core/ProjectEvent.h"
#include <QDialog>
#include <QHBoxLayout>
#include <QScopedPointer>
#include <QVBoxLayout>

namespace gui {

TargetWidget::TargetWidget(ViaPoint& aViaPoint, GUIResources& aResources, QWidget* aParent, const QSize& aSizeHint):
    QWidget(aParent),
    mProject(),
    mViaPoint(aViaPoint),
    mSizeHint(aSizeHint),
    mHorizontalSplitter(nullptr),
    mHeader(nullptr),
    mSuspendCount(0) {
    mHorizontalSplitter = new ThinSplitter(Qt::Horizontal, this);
    // The hitbox grab area sits 2px into the object tree / 10px into the
    // timeline, so it clears the tree's edge scrollbar; the hairline stays on
    // the boundary.
    mHorizontalSplitter->setHitBoxOffset(4);
    mObjTree = new ObjectTreeWidget(aViaPoint, aResources, mHorizontalSplitter);
    mTimeLine = new TimeLineWidget(aResources, aViaPoint, *this, mHorizontalSplitter);
    // The transport is a fixed-size group of buttons; it belongs to the header
    // bar's own layout (not to the splitter, and no longer to a column beside
    // it), so it can never be resized and the lanes get the dock's full width.
    mPlayBack = new PlayBackWidget(aResources, this, *mProject);

    mHorizontalSplitter->addWidget(mObjTree);
    mHorizontalSplitter->addWidget(mTimeLine);
    mHorizontalSplitter->setCollapsible(0, false);
    mHorizontalSplitter->setCollapsible(1, false);


    // The dock's header: the 40px bar the transport moved into, above the
    // splitter, spanning the dock. The row below it is the one the dock has
    // always had - object tree | timeline - now without the transport column.
    mHeader = new TimeLineHeader(aResources, mPlayBack, this);
    mHeader->setTimeLineWidget(mTimeLine);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(mHorizontalSplitter, 1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(mHeader);
    lay->addLayout(row, 1);

    mPlayBack->setPushDelegate([=](PlayBackWidget::PushType aType) { this->onPlayBackButtonPushed(aType); });
}

void TargetWidget::setProject(core::Project* aProject) {
    mPlayBack->pushPauseButton();
    mProject = aProject;
    mObjTree->setProject(aProject);
    mTimeLine->setProject(aProject);
    mPlayBack->aProject = aProject;
    // The frame range the readout names is the editor's own, so re-read it now
    // that the project (or no project at all) is behind it.
    mHeader->refreshFrame();
}

core::Frame TargetWidget::currentFrame() const { return mTimeLine->currentFrame(); }

void refreshMedia(core::Project* mProject, PlayBackWidget* mPlayBack){
    if(mProject->mediaRefresh){
        mPlayBack->aConf = mProject->pConf;
        mPlayBack->mediaPlayer = *mProject->mediaPlayer;
        mProject->mediaRefresh = false;
    }
}

void TargetWidget::stop() { mPlayBack->pushPauseButton(); }

void TargetWidget::suspend() { ++mSuspendCount; }

void TargetWidget::resume() {
    XC_ASSERT(mSuspendCount > 0);
    --mSuspendCount;
}

bool TargetWidget::isSuspended() const { return mSuspendCount > 0; }

void TargetWidget::onPlayBackButtonPushed(PlayBackWidget::PushType aType) {
    if (!mProject)
        return;
    if (aType == PlayBackWidget::PushType_Play) {
        refreshMedia(mProject, mPlayBack);
        mTimeLine->setPlayBackActivity(true, mPlayBack->aConf, &mPlayBack->mediaPlayer);
    } else if (aType == PlayBackWidget::PushType_Pause) {
        refreshMedia(mProject, mPlayBack);
        mTimeLine->setPlayBackActivity(false, mPlayBack->aConf, &mPlayBack->mediaPlayer);
    } else if (aType == PlayBackWidget::PushType_Step) {
        mTimeLine->setFrame(currentFrame().added(1));
    } else if (aType == PlayBackWidget::PushType_StepBack) {
        mTimeLine->setFrame(currentFrame().added(-1));
    } else if (aType == PlayBackWidget::PushType_Rewind) {
        // Land on the effective playback range's start (the temporary
        // window's start while it overrides) — Frame(0) would stay at 0 when
        // the window starts negative.
        mTimeLine->setFrame(core::Frame(mTimeLine->playbackRange().min()));
    } else if (aType == PlayBackWidget::PushType_Fast) {
        // Land on the effective playback range's end (the temporary window's
        // end while it overrides) — the project range's max would fall short
        // when the window extends past it.
        mTimeLine->setFrame(core::Frame(mTimeLine->playbackRange().max()));
    } else if (aType == PlayBackWidget::PushType_Options) {
        openAnimationSettings();
    } else if (aType == PlayBackWidget::PushType_Loop) {
        mTimeLine->setPlayBackLoop(mPlayBack->isLoopChecked());
    }
}

void TargetWidget::openAnimationSettings() {
    if (!mProject)
        return;

    const int curFPS = mProject->attribute().fps();
    const util::Range curRange = mProject->attribute().frameRange();
    const bool curLoop = mPlayBack->isLoopChecked();

    // Show the settings as a Qt::Popup anchored to the playback options button
    // (the EasingPickWidget idiom). The dialog is a pure view created ONCE and
    // reused for every open: it shows/hides (a popup auto-hides on an outside
    // click) and is never destroyed/recreated, so its QML scene is not torn
    // down per-open. The current project/loop state is passed in to showAt() on
    // each open, and is captured as the undo baseline (mLastAnim*) for the
    // single session command pushed on close.
    if (!mAnimSettings) {
        mAnimSettings = new AnimationSettingDialog(this);
        connect(mAnimSettings, &AnimationSettingDialog::changed,
                this, &TargetWidget::applyAnimLive);
        connect(mAnimSettings, &AnimationSettingDialog::applied,
                this, &TargetWidget::applyAnimCommit);
    }
    mLastAnimFps = curFPS;
    mLastAnimRange = curRange;
    mLastAnimLoop = curLoop;

    const QWidget* btn = mPlayBack->optionsButton();
    const QPoint anchor = btn ? btn->mapToGlobal(QPoint(0, btn->height())) : QPoint();
    mAnimSettings->showAt(anchor, curRange, curFPS, curLoop);
}

void TargetWidget::applyAnimLive() {
    if (!mAnimSettings || !mProject)
        return;
    // Live, non-undoable apply: the attribute setters drive onProjectAttributeModified
    // (bone caches) and refreshAnimationSettings() re-syncs the timeline/display/
    // driver, so the effect is visible while the popup is still open. Fires on
    // every edit, including each tick of a scrub drag.
    const int fps = mAnimSettings->fps();
    const util::Range range = mAnimSettings->frameRange();
    const bool loop = mAnimSettings->isLoopChecked();

    mProject->attribute().setFps(fps);
    mProject->attribute().setFrameRange(range);
    mPlayBack->checkLoop(loop);
    mTimeLine->setPlayBackLoop(loop);
    refreshAnimationSettings();
}

void TargetWidget::applyAnimCommit() {
    if (!mAnimSettings || !mProject)
        return;
    // The whole popup session is ONE undo step: push a single command that
    // transitions the project attribute from the pre-open baseline (mLastAnim*)
    // to the current draft. No net change -> nothing to record.
    const int fps = mAnimSettings->fps();
    const util::Range range = mAnimSettings->frameRange();
    const bool loop = mAnimSettings->isLoopChecked();
    if (fps == mLastAnimFps && range == mLastAnimRange && loop == mLastAnimLoop)
        return;

    cmnd::ScopedMacro macro(mProject->commandStack(), CmndName::tr("Change animation settings"));
    core::Project* projectPtr = mProject;
    auto* command = new cmnd::Delegatable(
        [=]() {
            projectPtr->attribute().setFps(fps);
            projectPtr->attribute().setFrameRange(range);
            mPlayBack->checkLoop(loop);
            mTimeLine->setPlayBackLoop(loop);
            this->refreshAnimationSettings();
        },
        [=]() {
            projectPtr->attribute().setFps(mLastAnimFps);
            projectPtr->attribute().setFrameRange(mLastAnimRange);
            mPlayBack->checkLoop(mLastAnimLoop);
            mTimeLine->setPlayBackLoop(mLastAnimLoop);
            this->refreshAnimationSettings();
        });
    projectPtr->commandStack().push(command);
}

void TargetWidget::refreshAnimationSettings() {
    if (auto* menu = mViaPoint.mainMenuBar()) {
        // Same fan-out as the project attribute dialogs: display, timeline
        // editor and playback driver all re-read the attributes.
        menu->onProjectAttributeUpdated();
        menu->onVisualUpdated();
    }
    // FPS changes the ruler strings (relative-to-FPS / timecode formats).
    mTimeLine->triggerOnTimeFormatChanged();
    // The frame range is one of the two numbers the header reads, and this popup
    // edits it without any frame report from the timeline: re-read it here.
    mHeader->refreshFrame();
}

} // namespace gui
