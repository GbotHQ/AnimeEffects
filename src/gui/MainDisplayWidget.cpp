#include <functional>
#include <QAction>
#include <QFont>
#include <QIcon>
#include <QInputMethodEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QOpenGLFunctions>
#include <QGuiApplication>
#include "XC.h"
#include "util/Finally.h"
#include "gl/Util.h"
#include "gl/Framebuffer.h"
#include "gl/Texture.h"
#include "core/ClippingFrame.h"
#include "gui/MainDisplayWidget.h"
#include "gui/ProjectHook.h"
#include "gui/TimeLineWidget.h"
#include "gui/ViaPoint.h"
#include "gui/ProjectTabBar.h"
#include "gui/KeyCommandMap.h"
#include "gui/MouseSetting.h"

namespace gui {

namespace {
// The panel's editor, or null when the driver has none (before the driver is set, or
// after it is gone). The panel itself is DRAWN elsewhere (gui::GraphPanelWidget, the
// dock's Graph tab); this widget keeps the editor's applications-side connections:
// the CHANNEL LIST and the KEY MENU are this class's QMenus (showGraphChannelMenu,
// showGraphKeyMenu) and the panel's accessibility items open them through here.
// and the channel list's actions read it HERE, at the moment they fire, rather than
// capturing one: a menu can be open across a project change. Independent of the
// ImGui build flag, like the two panel questions below: the editor is the driver's
// either way.
ctrl::GraphEditor* graphEditorOrNull(ctrl::Driver* aDriver) {
    return aDriver ? aDriver->graphEditor() : nullptr;
}

// The colour key of one channel-list row (showGraphChannelMenu): a 12px rounded
// square in the channel's own hue while the channel is shown, and the same square
// HOLLOW - the hue as its 1px outline - while it is hidden. That is the reference's
// own swatch to the pixel (mock.css's .gp-chrow .gw: 12x12, radius 3, `background:
// var(--h)`, and `.gp-chrow.off .gw { background: transparent }`), and it is the ONE
// mark a row carries: the row is the toggle, so no second, redundant control sits
// beside it.
QIcon channelSwatch(const QColor& aHue, bool aVisible) {
    constexpr int kSide = 12;
    constexpr qreal kRadius = 3.0;

    QPixmap pixmap(kSide, kSide);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(aVisible ? QPen(Qt::NoPen) : QPen(aHue, 1.0));
    painter.setBrush(aVisible ? QBrush(aHue) : QBrush(Qt::NoBrush));
    painter.drawRoundedRect(QRectF(0.5, 0.5, kSide - 1.0, kSide - 1.0), kRadius, kRadius);
    painter.end();
    return QIcon(pixmap);
}

// What one channel-list row's action acts on, snapshotted for the life of the menu:
// the channel names the component, `visible` is the state the action flips, and the
// node is a RAW pointer held only for the menu's lifetime. It cannot be a QPointer -
// core::ObjectNode is no QObject, so there is nothing to auto-clear it - so the
// liveness rule is explicit instead: every action re-checks the node against the LIVE
// editor's own targets (channelRowNodeAlive below) before it writes. A menu outlives
// the press that popped it and the document can change under it, so this is what
// stops an action writing through a node the document has since dropped.
struct ChannelRowTarget {
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    bool visible = true;
};

// Whether a row's node is still one the live editor knows: the panel's rows are built
// from exactly that list (GraphPanelModel.cpp's buildChannels walks aEditor.targets()),
// so a node the document has dropped since the menu was popped - or one a project
// change took out of the editor's targets - is no longer in it. Only POINTERS are
// compared, never dereferenced, so a stale node cannot be touched by the test itself.
bool channelRowNodeAlive(const ctrl::GraphEditor* aEditor, core::ObjectNode* aNode) {
    return aEditor && aNode && aEditor->targets().contains(aNode);
}
}

//-------------------------------------------------------------------------------------------------
MainDisplayWidget::MainDisplayWidget(ViaPoint& aViaPoint, QWidget* aParent):
    QOpenGLWidget(aParent),
    mViaPoint(aViaPoint),
    mGLDeviceInfo(),
    mProject(),
    mGLRoot(),
    mGLContextAccessor(this),
    mDefaultVAO(),
    mFramebuffer(),
    mClippingFrame(),
    mDestinationTexturizer(),
    mTextureDrawer(),
    mPainterHandle(),
    mRenderingLock(),
    mRenderInfo(),
    mAbstractCursor(),
    mDriver(),
    mProjectTabBar(),
    mTimeLine(),
    mTimeLineRangeSlot(),
    mViewportBackground(),
    mUsingTablet(false),
    mViewSetting(),
    mCanvasMover(),
    mMovingCanvasByTool(false),
    mMovingCanvasByKey(false),
    mMovingCanvasByMiddleMouseButton(false),
    mDevicePixelRatio(1.0) {
    this->setObjectName(QStringLiteral("MainDisplayWidget"));
    this->setMouseTracking(true);
    this->setAutoFillBackground(false); // avoid autofill on QPainter::begin()

    // key binding
    if (mViaPoint.keyCommandMap()) {
        // move canvas
        {
            auto key = mViaPoint.keyCommandMap()->get("MoveCanvas");
            if (key) {
                key->addListener(
                    [=]() {
                        mAbstractCursor.suspendEvent([=]() { updateCursor(); });
                        mMovingCanvasByKey = true;
                        applyCanvasMoverDragMode();
                    },
                    [=]() {
                        mMovingCanvasByKey = false;
                        applyCanvasMoverDragMode();
                        mAbstractCursor.resumeEvent();
                    }
                );
            }
        }

        // rotate canvas
        {
            auto key = mViaPoint.keyCommandMap()->get("RotateCanvas");
            if (key) {
                key->addListener(
                    [=]() {
                        mAbstractCursor.suspendEvent([=]() { updateCursor(); });
                        mCanvasMover.setDragAndRotate(true);
                    },
                    [=]() {
                        mCanvasMover.setDragAndRotate(false);
                        mAbstractCursor.resumeEvent();
                    }
                );
            }
        }

        // rotate canvas clockwise
        {
            auto key = mViaPoint.keyCommandMap()->get("RotateCanvas15Clockwise");
            if (key) {

                key->addListener([=]() {
                    mCanvasMover.rotate(qDegreesToRadians(15.0f));
                    updateRender();
                });
            }
        }

        // rotate canvas anti-clockwise
        {
            auto key = mViaPoint.keyCommandMap()->get("RotateCanvas15AntiClockwise");
            if (key) {

                key->addListener([=]() {
                    mCanvasMover.rotate(qDegreesToRadians(-15.0f));
                    updateRender();
                });
            }
        }

        // reset canvas angle
        {
            auto key = mViaPoint.keyCommandMap()->get("ResetCanvasAngle");
            if (key) {
                key->addListener([=]() {
                    mViaPoint.mainViewSetting().resetRotateView = true;
                    onViewSettingChanged(mViaPoint.mainViewSetting());
                    mViaPoint.mainViewSetting().resetRotateView = false;
                });
            }
        }
    }
}

MainDisplayWidget::~MainDisplayWidget() {
    mPainterHandle.reset();
    mTextureDrawer.reset();
    mDestinationTexturizer.reset();
    mClippingFrame.reset();
    mFilterFrame.reset();
    mFramebuffer.reset();
    mDefaultVAO.reset();

    gl::DeviceInfo::setInstance(nullptr);
    gl::Global::clearFunctions();
}

void MainDisplayWidget::setProject(core::Project* aProject) {
    mProject.reset();

    mRenderInfo = nullptr;
    mAbstractCursor = core::AbstractCursor();

    if (aProject) {
        mProject = aProject->pointee();
        mRenderInfo = &(static_cast<ProjectHook*>(mProject->hook())->renderInfo());
        mRenderInfo->camera.setDevicePixelRatio(this->devicePixelRatioF());
        mRenderInfo->camera.setScreenSize(this->size());
        mRenderInfo->camera.setImageSize(mProject->attribute().imageSize());
        mCanvasMover.setCamera(&(mRenderInfo->camera));
    }

    updateRender();
}

void MainDisplayWidget::setDriver(ctrl::Driver* aDriver) {
    mDriver = aDriver;

    // The driver owns the graph editor (and is created per project), so a NEW
    // driver is a new panel with no shared state: hand it the timeline's selection
    // and frame window here, which is also the moment a project's timeline is set
    // up (MainWindow::resetProjectRefs sets the timeline before it sets the
    // driver).
    syncGraphPanelSharedState();
}

void MainDisplayWidget::setTimeLineWidget(TimeLineWidget* aTimeLine) {
    if (mTimeLine == aTimeLine)
        return;

    if (mTimeLine) {
        mTimeLine->onViewRangeChanged.disconnect(mTimeLineRangeSlot);
    }

    mTimeLine = aTimeLine;

    if (mTimeLine) {
        mTimeLineRangeSlot =
            mTimeLine->onViewRangeChanged.connect(this, &MainDisplayWidget::syncGraphPanelSharedState);
    }

    syncGraphPanelSharedState();
}

void MainDisplayWidget::syncGraphPanelSharedState() {
    ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
    if (!editor)
        return; // no panel yet (no driver, so no editor): nothing to hand it to

    ctrl::TimeLineEditor* timelineEditor = mTimeLine ? mTimeLine->editor() : nullptr;
    if (!mTimeLine || !timelineEditor) {
        // No timeline attached: the panel owns its frame axis and its selection
        // (its own private instance of the same set), which is the state the unit
        // tests drive it in.
        editor->clearFrameAxis();
        editor->setSelectionSource(nullptr);
        return;
    }

    // ONE selection: the panel writes the timeline's live set, which is what the
    // timeline renders its highlights from and what its own gestures (and its
    // Delete key, its easing menu) read - so a key selected in either view is
    // selected and highlighted in both.
    editor->setSelectionSource(&timelineEditor->selection());

    // ONE frame window, pushed as VALUES: the frames the timeline is showing.
    // The panel maps them onto its own plot; it never fits, zooms or pans its own
    // X while this is in force, and the two gestures below send the horizontal
    // half of a wheel or a pan back HERE, so the panel scrolls and zooms the
    // timeline instead of drifting beside it. A sub-pixel pan therefore moves the
    // timeline, its new window comes back through onViewRangeChanged -> this
    // method, and the panel's X follows: one window, one owner.
    editor->setFrameGestureDelegates(
        [this](double aFrameDelta) {
            if (mTimeLine)
                mTimeLine->scrollFramesBy(aFrameDelta);
        },
        [this](int aWheelDelta, double aAnchorFrame) {
            if (mTimeLine)
                mTimeLine->zoomFramesAbout(aWheelDelta, aAnchorFrame);
        }
    );

    // The window itself: fractional frames, because that is what the timeline's
    // content-pixel viewport is. A whole-frame carrier (util::Range) made every pan
    // land on a frame and left the panel's zoom unable to hold its anchor.
    double lo = 0.0;
    double hi = 0.0;
    mTimeLine->visibleFrameWindow(lo, hi);
    if (hi > lo)
        editor->setFrameWindow(lo, hi);

    // The panel draws that window, so its own frame is now stale.
    this->updateRender();
}

void MainDisplayWidget::showGraphKeyMenu(const core::TimeLineEvent& aTargets, const QPoint& aWidgetPos) {
    if (!mTimeLine)
        return; // no timeline to open the picker from (and no document to act on)

    mTimeLine->showKeyEasingPicker(aTargets, this->mapToGlobal(aWidgetPos));
}

void MainDisplayWidget::showGraphChannelMenu(const QVector<PanelChannelRow>& aRows, const QPoint& aWidgetPos) {
    if (aRows.isEmpty())
        return; // no channels: no list to show

    // ONE menu, built fresh from the rows the panel's model published for the frame
    // the button was pressed in. It is DELETED ON CLOSE and parented to this widget,
    // because popup() is not exec(): it returns immediately, so a stack QMenu would
    // be destroyed before the user ever saw it. A press with a QMenu already open
    // cannot arrive here (the menu owns the mouse grab), so no second one is
    // possible while one is up.
    QMenu* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // What each row's action acts on, snapshotted with the node GUARDED: the menu
    // outlives the press that popped it and the document can change under it, so an
    // action resolves its node at trigger time rather than holding a raw pointer
    // into a model that has been replaced (or a node that has been deleted) since.
    // The actions carry the INDEX into this vector; the lambdas below hold a copy.
    QVector<ChannelRowTarget> rows;
    rows.reserve(aRows.size());
    for (const PanelChannelRow& row : aRows) {
        ChannelRowTarget target;
        target.node = row.node;
        target.channel = row.channel;
        target.visible = row.visible;
        rows.append(target);
    }

    QString layer;
    for (int i = 0; i < aRows.size(); ++i) {
        const PanelChannelRow& row = aRows.at(i);
        if (!row.node)
            continue;

        // The layer heading: a disabled row, so the list reads as one group per layer
        // and the heading is not itself a control (the reference's own .gp-chlayer).
        // It carries the sentinel index -1 so the hover handler below can tell it from
        // a channel row (an action with NO data would read back as index 0, i.e. as
        // the first channel).
        if (row.layer != layer) {
            layer = row.layer;
            QAction* heading = menu->addAction(layer);
            heading->setEnabled(false);
            heading->setData(-1);
        }

        QAction* action = menu->addAction(channelSwatch(row.color, row.visible), row.name);
        action->setData(i);
        if (row.focused) {
            // The focused channel, marked the one way a QMenu row can be: bold. It is
            // the channel whose tangent handles the panel draws (and the one the
            // accessible summary names), and the list is how it is chosen.
            QFont font = action->font();
            font.setBold(true);
            action->setFont(font);
        }

        // THE ROW'S ONE ACTION is its channel's visibility switch - the swatch beside
        // it IS its colour key, filled while the channel is shown and hollow while it
        // is hidden. The editor is read from the DRIVER at this moment, not captured:
        // the menu can be open across a project change, and the driver is what owns
        // the live editor.
        connect(action, &QAction::triggered, this, [this, rows, i] {
            ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
            const ChannelRowTarget& row = rows.at(i);
            if (!channelRowNodeAlive(editor, row.node))
                return; // the node is not one the live editor holds any more
            editor->setChannelVisible(*row.node, row.channel, !row.visible);
            this->updateRender();
        });
    }

    // Highlighting a row FOCUSES its channel - the other half of what the list is
    // for ("the row itself focuses the type"): the panel then draws that channel's
    // tangent handles and the panel's own summary names it. Hover, not activation,
    // because activation is already spent on the visibility switch.
    connect(menu, &QMenu::hovered, this, [this, rows](QAction* aAction) {
        if (!aAction)
            return;
        const int index = aAction->data().toInt();
        if (index < 0 || index >= rows.size())
            return; // a layer heading, which carries no index
        ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
        const ChannelRowTarget& row = rows.at(index);
        if (!channelRowNodeAlive(editor, row.node))
            return; // the node is not one the live editor holds any more
        editor->setFocusedChannel(*row.node, row.channel);
        this->updateRender();
    });

    if (menu->actions().isEmpty()) {
        delete menu; // every row was a node-less one: nothing to show
        return;
    }
    menu->popup(this->mapToGlobal(aWidgetPos));
}

void MainDisplayWidget::resetCamera() {
    if (mRenderInfo) {
        auto& camera = mRenderInfo->camera;
        auto scrSize = this->size();
        auto imgSize = camera.imageSize();
        camera.setCenter(QVector2D(scrSize.width() * 0.5f, scrSize.height() * 0.5f));
        if (scrSize.width() > 0 && scrSize.height() > 0 && imgSize.width() > 0 && imgSize.height() > 0) {
            auto scaleX = (float)scrSize.width() / imgSize.width();
            auto scaleY = (float)scrSize.height() / imgSize.height();
            auto minScale = scaleX < scaleY ? scaleX : scaleY;
            camera.setScale(minScale);
        }

        mCanvasMover.setCamera(&camera);
    }
}

void MainDisplayWidget::setProjectTabBar(ProjectTabBar* aTabBar) { mProjectTabBar = aTabBar; }

void MainDisplayWidget::setViewportBackground(const QColor& aColor) {
    if (mViewportBackground != aColor) {
        mViewportBackground = aColor;
        this->updateRender();
    }
}

void MainDisplayWidget::updateRender() { this->update(); }

void MainDisplayWidget::initializeGL() {
    if (!this->context()->isValid()) {
        XC_FATAL_ERROR("OpenGL Error", "Invalid opengl context.", "");
    }
    // check version
    {
        auto version = this->format().version();
        // fallback
        if (version < gl::Global::kVersion) {
            const QString v = (const char*)this->context()->functions()->glGetString(GL_VERSION);
            version = {v.at(0).digitValue(), v.at(2).digitValue()};
            if (version < gl::Global::kVersion) {
                // we ball
                QMessageBox::warning(this, "OpenGL version check fail", "The minimum OpenGL version for AnimeEffects is "
                    + QString::number(gl::Global::kVersion.first) + "." + QString::number(gl::Global::kVersion.second) +
                    " but we have detected version " + QString::number(this->format().version().first) + "." + QString::number(this->format().version().second) +
                    ". AnimeEffects will attempt to run regardless but the app may crash unexpectedly.");
                version = {gl::Global::kVersion};
            }
        }
        if (version < gl::Global::kVersion) {
            auto obtainedVersion = QString::number(version.first) + "." + QString::number(version.second);
            auto requiredVersion =
                QString::number(gl::Global::kVersion.first) + "." + QString::number(gl::Global::kVersion.second);
            XC_FATAL_ERROR(
                "OpenGL Error",
                QString("Failed to get the correct OpenGL version.\n"
                        "The obtained version is %1,\n"
                        "but the required version is %2.")
                    .arg(obtainedVersion)
                    .arg(requiredVersion),
                ""
            );
        }
    }
    // initialize opengl functions
    auto functions = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(this->context());
    if (!functions) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to get opengl functions.", "");
    }
    if (!functions->initializeOpenGLFunctions()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to initialize opengl functions.", "");
    }

    // setup global info
    gl::Global::setContext(*this);
    gl::Global::setFunctions(*functions);

    // setup gl root
    mGLRoot.setContextAccessor(mGLContextAccessor);
    mGLRoot.setFunctions(*functions);

    // initialize opengl device info
    mGLDeviceInfo.load();
    gl::DeviceInfo::setInstance(&mGLDeviceInfo);
    mViaPoint.setGLDeviceInfo(mGLDeviceInfo);

#ifdef USE_GL_CORE_PROFILE
    // initialize default vao
    mDefaultVAO.reset(new gl::VertexArrayObject());
    mDefaultVAO->bind(); // keep binding
#endif

    mDevicePixelRatio = this->devicePixelRatioF();

    // create framebuffer for display
    mFramebuffer.reset(new QOpenGLFramebufferObject(deviceSize()));

    // create clipping buffer
    mClippingFrame.reset(new core::ClippingFrame());
    mClippingFrame->resize(deviceSize());

    // create filter frame for folder composite filters
    mFilterFrame.reset(new core::FilterFrame());
    mFilterFrame->resize(deviceSize());

    // create texturizer for destination colors of the framebuffer
    mDestinationTexturizer.reset(new core::DestinationTexturizer());
    mDestinationTexturizer->resize(deviceSize());

    // create texture drawer for copying framebuffer to display
    mTextureDrawer.reset(new gl::EasyTextureDrawer());
    if (!mTextureDrawer->init()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to initialize texture drawer.", "");
    }

    mPainterHandle.reset(new ctrl::PainterHandle());

    GL_CHECK_ERROR();
}

void MainDisplayWidget::paintGL() {
    gl::Global::Functions& ggl = gl::Global::functions();

    // clear clipping
    mClippingFrame->clearTexture();
    mClippingFrame->resetClippingId();
    GL_CHECK_ERROR();

    mDestinationTexturizer->clearTexture();
    GL_CHECK_ERROR();

    mFilterFrame->clearAll();
    GL_CHECK_ERROR();

    if (!mFramebuffer->bind()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to bind framebuffer.", "");
    }

    // setup
    gl::Util::setViewportAsActualPixels(deviceSize());
    gl::Util::clearColorBuffer(mViewportBackground.redF(), mViewportBackground.greenF(), mViewportBackground.blueF(), 1.0f);
    gl::Util::resetRenderState();
    GL_CHECK_ERROR();

    // setup renderinfo
    if (mProject) {
        XC_PTR_ASSERT(mRenderInfo);
        mRenderInfo->time = mProject->currentTimeInfo();
        mRenderInfo->framebuffer = mFramebuffer->handle();
        mRenderInfo->dest = mFramebuffer->texture();
        mRenderInfo->isGrid = false;
        mRenderInfo->nonPosed = false;
        mRenderInfo->clippingId = 0;
        mRenderInfo->clippingFrame = mClippingFrame.data();
        mRenderInfo->destTexturizer = mDestinationTexturizer.data();
        mRenderInfo->filterFrame = mFilterFrame.data();
        mRenderInfo->opacityScale = 1.0f;
        XC_ASSERT(mRenderInfo->framebuffer != 0);
        XC_ASSERT(mRenderInfo->dest != 0);
        mRenderInfo->camera.setFlip(mViewSetting.flipCanvas);
    }

    if (mDriver) {
        XC_PTR_ASSERT(mRenderInfo);
        auto gridTarget = mViewSetting.showLayerMesh ? mDriver->currentTarget() : nullptr;
        mDriver->renderGL(*mRenderInfo, gridTarget);
        GL_CHECK_ERROR();
    }

    if (!mFramebuffer->release()) {
        XC_FATAL_ERROR("OpenGL Error", "Failed to unbind framebuffer.", "");
    }

    ggl.glBindFramebuffer(GL_FRAMEBUFFER, this->defaultFramebufferObject());

    if (mViewSetting.cutImagesByTheFrame && mProject) {
        XC_PTR_ASSERT(mRenderInfo);
        gl::Util::clearColorBuffer(mViewportBackground.redF(), mViewportBackground.greenF(), mViewportBackground.blueF(), 1.0f);

        auto imageQuad = mRenderInfo->camera.screenImageQuadangle();
        const QSize screenSize = mRenderInfo->camera.screenSize();

        mTextureDrawer->draw(mFramebuffer->texture(), imageQuad, screenSize, imageQuad, screenSize);
    } else {
        mTextureDrawer->draw(mFramebuffer->texture());
    }

    ggl.glFlush();
    GL_CHECK_ERROR();
}

void MainDisplayWidget::paintEvent(QPaintEvent* aEvent) {
    // try lock rendering!!
    if (!mRenderingLock.tryLockForRead())
        return;
    util::Finally unlocker([=]() { this->mRenderingLock.unlock(); });

    QOpenGLWidget::paintEvent(aEvent);

    QPainter* painter = mPainterHandle->begin(*this);
    painter->setRenderHint(QPainter::Antialiasing);

    if (mDriver) {
        XC_PTR_ASSERT(mRenderInfo);
        mDriver->renderQt(*mRenderInfo, *painter);
        GL_CHECK_ERROR();
    }
    // we must call end function
    mPainterHandle->end();

    // NOTHING ELSE IS PAINTED HERE. The animation graph panel used to be drawn into
    // this widget's framebuffer right here (a Dear ImGui overlay over the artwork, with
    // the contrast-outline composite). It is drawn in the TIMELINE DOCK now
    // (gui::GraphPanelWidget, the Graph tab's page, design section 5), on a floor of its
    // own, and it pins the editor's frame window to ITS plot size in its own paint - so
    // this paint leaves the editor's scale alone (the old call here re-derived the shared
    // frame window for this widget's geometry, which no longer draws the panel).
}

void MainDisplayWidget::resizeGL(int w, int h) {
    // currrent device pixel ratio (Attention that it is variable.)
    mDevicePixelRatio = this->devicePixelRatioF();
    const QSize devSize(mDevicePixelRatio * w, mDevicePixelRatio * h); // device pixel size
    const QSize absSize(w, h); // abstract pixel size

    if (mRenderInfo) {
        mRenderInfo->camera.setDevicePixelRatio(mDevicePixelRatio);
        mRenderInfo->camera.setScreenSize(absSize);
        mCanvasMover.onScreenResized();
    }
    mFramebuffer.reset();
    mFramebuffer.reset(new QOpenGLFramebufferObject(devSize.width(), devSize.height()));

    mClippingFrame.reset();
    mClippingFrame.reset(new core::ClippingFrame());
    mClippingFrame->resize(devSize);

    mDestinationTexturizer->resize(devSize);

    mFilterFrame->resize(devSize);

    if (mProjectTabBar) {
        mProjectTabBar->updateTabPosition(absSize);
    }
    GL_CHECK_ERROR();
}

void MainDisplayWidget::applyCanvasMoverDragMode() {
    // ONE OWNER PER DRAG, written once: the flags are the gestures the user is making
    // (the Cursor tool, the MoveCanvas key, a middle press this widget began), and
    // folding them into one expression is what stops a tool change or a key press
    // mid-drag from re-enabling a pan the user has not asked for. The animation graph
    // panel used to hold ONE of these flags (it took the left drag while it was drawn
    // over this widget); it lives in the dock's own page now, where its gestures never
    // reach this camera.
    const bool dragging = mMovingCanvasByKey || mMovingCanvasByTool || mMovingCanvasByMiddleMouseButton;
    mCanvasMover.setDragAndMove(dragging);
}

void MainDisplayWidget::mouseMoveEvent(QMouseEvent* aEvent) {
    if (mRenderInfo) {
        // The panel's own gestures read the modifier state from here: neither
        // core::AbstractCursor nor the editor's updateCursor carries modifiers, so
        // the widget hands them over before it routes the event (see
        // ctrl::Driver::setGraphModifiers). Every move re-reads them, which is what
        // lets Shift constrain a key drag that is already in flight.
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setMouseMove(aEvent, mRenderInfo->camera)) {
            updateCursor();
            // if (!mUsingTablet) qDebug() << "move" << aEvent->pos();
        }

        if (mCanvasMover.updateByMove(
                mAbstractCursor.screenPos(),
                mAbstractCursor.screenVel(),
                mAbstractCursor.isPressedLeft(),
                mAbstractCursor.isPressedMiddle(),
                mAbstractCursor.isPressedRight()
            )) {
            updateRender();
        }
    }
}

void MainDisplayWidget::mousePressEvent(QMouseEvent* aEvent) {
    if (mRenderInfo) {
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setMousePress(aEvent, mRenderInfo->camera)) {
            updateCursor();
            // if (!mUsingTablet) qDebug() << "press";

            // The MIDDLE button pans the canvas (the graph panel used to own this
            // drag while it was up over this widget; it is in the dock now and owns
            // the cursor inside its own page).
            if (mViaPoint.mouseSetting().middleMouseMoveCanvas &&
                aEvent->button() == Qt::MouseButton::MiddleButton) {
                mMovingCanvasByMiddleMouseButton = true;
                applyCanvasMoverDragMode();
            }

            // A canvas grab shows the closed hand; the resting pan cursor
            // (open hand) only shows before the button goes down.
            if ((mMovingCanvasByTool || mMovingCanvasByKey || mMovingCanvasByMiddleMouseButton) &&
                (aEvent->button() == Qt::MouseButton::LeftButton ||
                 aEvent->button() == Qt::MouseButton::MiddleButton)) {
                this->setCursor(Qt::ClosedHandCursor);
            }
        }
    }
}

void MainDisplayWidget::mouseReleaseEvent(QMouseEvent* aEvent) {
    if (mRenderInfo) {
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setMouseRelease(aEvent, mRenderInfo->camera)) {
            updateCursor();
            // if (!mUsingTablet) qDebug() << "release";

            if (aEvent->button() == Qt::MouseButton::MiddleButton) {
                // A canvas middle pan this widget began ends here.
                mMovingCanvasByMiddleMouseButton = false;
                applyCanvasMoverDragMode();
            }

            // back to the resting cursor once every button is up (a
            // right-button release mid-drag must not clear the closed hand)
            if (aEvent->buttons() == Qt::NoButton) {
                this->setCursor(mMovingCanvasByTool ? Qt::OpenHandCursor : Qt::ArrowCursor);
            }
        }
    }
}

void MainDisplayWidget::wheelEvent(QWheelEvent* aEvent) {
    if (mCanvasMover.updateByWheel(
            QVector2D(aEvent->position()), aEvent->angleDelta().y(), mViaPoint.mouseSetting().invertMainViewScaling
        )) {
        updateRender();
    }
}

void MainDisplayWidget::tabletEvent(QTabletEvent* aEvent) {
    if (mRenderInfo) {
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setTabletEvent(aEvent, mRenderInfo->camera)) {
            updateCursor();
        }
        if (mAbstractCursor.shouldResetCanvas) {
            mCanvasMover.resetRotation();
            updateRender();
        }
    }

    if (aEvent->modifiers().testFlag(Qt::ShiftModifier)) {
        mCanvasMover.rotate(qDegreesToRadians(1.0 * mAbstractCursor.screenVel().x()));
        mCanvasMover.updateByWheel(mAbstractCursor.screenPos(), 2.0 * mAbstractCursor.screenVel().y(), false);
        updateRender();
    }
    else if (mCanvasMover.updateByMove(
            mAbstractCursor.screenPos(),
            mAbstractCursor.screenVel(),
            mAbstractCursor.isPressedLeft(),
            mAbstractCursor.isPressedMiddle(),
            mAbstractCursor.isPressedRight()
        )) {
        updateRender();
    }
    aEvent->accept();
}

void MainDisplayWidget::mouseDoubleClickEvent(QMouseEvent* aEvent) {
    // QWidget's own double-click behaviour is "a press" (its default implementation
    // calls mousePressEvent), and that is what the viewport wants here: the second
    // click still drags the canvas and the tools' own gestures. (The panel's fit
    // gesture used to be taken here; it is the docked panel's, on its own page.)
    QOpenGLWidget::mouseDoubleClickEvent(aEvent);
}

void MainDisplayWidget::keyPressEvent(QKeyEvent* aEvent) {
    // The event is ignored, i.e. it propagates to the main window's key map, which is
    // where the application's key routing has always been. (A Dear ImGui text field
    // used to hold the keyboard while the panel was drawn over this widget; the panel
    // is the dock's page now and its own widget forwards its keys.)
    QOpenGLWidget::keyPressEvent(aEvent);
}

void MainDisplayWidget::keyReleaseEvent(QKeyEvent* aEvent) { QOpenGLWidget::keyReleaseEvent(aEvent); }

void MainDisplayWidget::inputMethodEvent(QInputMethodEvent* aEvent) { QOpenGLWidget::inputMethodEvent(aEvent); }

QVariant MainDisplayWidget::inputMethodQuery(Qt::InputMethodQuery aQuery) const {
    // The ImGui caret an input context used to ask about lives in the panel's own page
    // now (gui::GraphPanelWidget forwards the same queries).
    return QOpenGLWidget::inputMethodQuery(aQuery);
}

void MainDisplayWidget::focusInEvent(QFocusEvent* aEvent) { QOpenGLWidget::focusInEvent(aEvent); }

void MainDisplayWidget::focusOutEvent(QFocusEvent* aEvent) { QOpenGLWidget::focusOutEvent(aEvent); }

void MainDisplayWidget::onVisualUpdated() { updateRender(); }

void MainDisplayWidget::onToolChanged(ctrl::ToolType aType) {
    if (aType == ctrl::ToolType_Cursor) {
        this->setCursor(Qt::OpenHandCursor);
        mMovingCanvasByTool = true;
        applyCanvasMoverDragMode();
    } else {
        this->setCursor(Qt::ArrowCursor);
        mMovingCanvasByTool = false;
        applyCanvasMoverDragMode();
    }
}

void MainDisplayWidget::onFinalizeTool(ctrl::ToolType) {
    mAbstractCursor.suspendEvent([=]() { updateCursor(); });
    mAbstractCursor.resumeEvent();
}

void MainDisplayWidget::onViewSettingChanged(const MainViewSetting& aSetting) {
    mViewSetting = aSetting;

    if (mViewSetting.resetRotateView) {
        mCanvasMover.resetRotation();
    } else if (mViewSetting.rotateViewACW) {
        mCanvasMover.rotate(qDegreesToRadians(-15.0f));
    } else if (mViewSetting.rotateViewCW) {
        mCanvasMover.rotate(qDegreesToRadians(15.0f));
    }

    updateRender();
}

void MainDisplayWidget::onProjectAttributeUpdated() {
    if (mProject && mRenderInfo) {
        const QSize newSize = mProject->attribute().imageSize();
        if (newSize != mRenderInfo->camera.imageSize()) {
            mRenderInfo->camera.setImageSize(newSize);
        }
    }
}

void MainDisplayWidget::updateCursor() {
    if (mDriver) {
        XC_PTR_ASSERT(mRenderInfo);
        if (mDriver->updateCursor(mAbstractCursor, mRenderInfo->camera)) {
            // The panel changed something: its selection, its focus, its window,
            // or the document under a drag. The TIMELINE draws the same selection
            // (they share one set) and reads the same document, and nothing else
            // would ask it to paint while the pointer is over the viewport - so a
            // key selected here would keep an unhighlighted square there, and a
            // key the panel drags would be drawn at its old frame. One repaint
            // request, coalesced by Qt with any other.
            if (mTimeLine) {
                mTimeLine->update();
            }
            updateRender();
        }
    }
}

} // namespace gui
