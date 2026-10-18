#include "gui/TargetWidget.h"
#include "gui/AnimationSettingDialog.h"
#ifdef AE_ENABLE_IMGUI
#include "gui/GraphPanelWidget.h"
#endif
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
    mObjTree(nullptr),
    mSurfaceStack(nullptr),
    mTimeLine(nullptr),
#ifdef AE_ENABLE_IMGUI
    mGraphPanel(nullptr),
#endif
    mHeader(nullptr),
    mSuspendCount(0) {
    mHorizontalSplitter = new ThinSplitter(Qt::Horizontal, this);
    // The hitbox grab area sits 2px into the object tree / 10px into the
    // timeline, so it clears the tree's edge scrollbar; the hairline stays on
    // the boundary.
    mHorizontalSplitter->setHitBoxOffset(4);
    mObjTree = new ObjectTreeWidget(aViaPoint, aResources, mHorizontalSplitter);

    // The dock's body, as ONE pane with the views stacked in it (design section 5):
    // the timeline's own surface and, when the graph panel is part of the build, the
    // animation graph panel. The tab in the header is what swaps them; the TREE stays
    // the splitter's first widget in both, so it is on screen either way. A
    // QStackedWidget rather than hide/show by hand: the layout of the hidden page is
    // not computed, and the surface the user sees is the one Qt sizes and shows.
    mSurfaceStack = new QStackedWidget(mHorizontalSplitter);
    mTimeLine = new TimeLineWidget(aResources, aViaPoint, *this, mSurfaceStack);
#ifdef AE_ENABLE_IMGUI
    // The Graph tab is the SAME panel the viewport's overlay draws, docked: it is the
    // panel's own widget (gui::GraphPanelWidget), pinned over the dock's floor at the
    // dock's size - no scrim, because that floor is already opaque.
    mGraphPanel = new GraphPanelWidget(aResources, mSurfaceStack);
#endif
    mSurfaceStack->addWidget(mTimeLine);
#ifdef AE_ENABLE_IMGUI
    mSurfaceStack->addWidget(mGraphPanel);
#endif
    mSurfaceStack->setCurrentWidget(mTimeLine);

    // The transport is a fixed-size group of buttons; it belongs to the header
    // bar's own layout (not to the splitter, and no longer to a column beside
    // it), so it can never be resized and the lanes get the dock's full width.
    mPlayBack = new PlayBackWidget(aResources, this, *mProject);

    mHorizontalSplitter->addWidget(mObjTree);
    mHorizontalSplitter->addWidget(mSurfaceStack);
    mHorizontalSplitter->setCollapsible(0, false);
    mHorizontalSplitter->setCollapsible(1, false);


    // The dock's header: the 40px bar the transport moved into, above the
    // splitter, spanning the dock. The row below it is the one the dock has
    // always had - object tree | surface - now without the transport column.
    mHeader = new TimeLineHeader(aResources, mPlayBack, this);
    mHeader->setTimeLineWidget(mTimeLine);
#ifdef AE_ENABLE_IMGUI
    // The header's tab is the dock's view switch, for the whole life of the dock: the
    // tab names the page, so the panel needs no title of its own and both pages keep
    // the one header's transport, readout, chip and zoom.
    connect(mHeader, &TimeLineHeader::surfaceChanged, this, &TargetWidget::onSurfaceChanged);
#endif

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
    // The Graph tab's panel (when the panel is part of the build) is NOT re-read
    // here: it reads the document through the driver, and this call comes BEFORE
    // MainWindow hands the new one over - the previous project's driver is already
    // destroyed at this point, so a refresh here would read a dead editor.
    // setDriver() itself re-reads the panel, in the same resetProjectRefs pass, and a
    // tab switch re-reads it again.
}

void TargetWidget::setDriver(ctrl::Driver* aDriver) {
#ifdef AE_ENABLE_IMGUI
    // The driver owns ctrl::GraphEditor, and it is created and destroyed with the
    // project: MainWindow hands it over from the same call site that hands it to the
    // viewport and the tool widget, so the docked panel and the overlay read the ONE
    // editor of the project on screen (see MainWindow::resetProjectRefs).
    mGraphPanel->setDriver(aDriver);
    // A new editor is a new panel: the view it draws is built from it, so re-read it
    // now rather than on the next frame that happens to repaint.
    mGraphPanel->refresh();
#else
    // No graph surface in this build: the dock has nothing that reads the driver
    // (the viewport's overlay and the tool widget have their own setDriver).
    (void)aDriver;
#endif
}

#ifdef AE_ENABLE_IMGUI
void TargetWidget::onSurfaceChanged(TimeLineHeader::Surface aSurface) {
    const bool graph = aSurface == TimeLineHeader::Surface::Graph;
    mSurfaceStack->setCurrentWidget(graph ? static_cast<QWidget*>(mGraphPanel) : static_cast<QWidget*>(mTimeLine));
    // Becoming visible is the one moment the panel has to re-read the document and
    // the frame it is shown at: it is not repainted while the timeline tab is up
    // (Qt lays out and paints only the current page).
    if (graph)
        mGraphPanel->refresh();
}
#endif

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
