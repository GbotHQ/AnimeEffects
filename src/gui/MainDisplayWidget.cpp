#include <functional>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QTouchEvent>
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
#include "gui/ViaPoint.h"
#include "gui/ProjectTabBar.h"
#include "gui/KeyCommandMap.h"
#include "gui/MouseSetting.h"
#ifdef AE_ENABLE_IMGUI
// ImGuiA11y is the other half of the overlay's presence in the accessibility
// tree: the constructor below installs its factory, and the overlay's first frame
// (ImGuiOverlay::init) only adopts the action callback onto that installation.
#include "gui/ImGuiA11y.h"
#include "gui/ImGuiOverlay.h"
#endif

namespace gui {

#ifdef AE_ENABLE_IMGUI
// The Dear ImGui overlay (gui/ImGuiOverlay.h): the animation graph panel, drawn
// with ImGui in this widget's own GL pass.
//
// Held in the translation unit rather than as a member on purpose - the widget's
// interface does not have to change to carry it - and created unconditionally in
// initializeGL, because it IS the shipping panel (the QPainter one it replaced is
// no longer called; ctrl::Driver::renderQt says so).
namespace {
QScopedPointer<ImGuiOverlay> gImGuiOverlay;

// The editor the panel is built from, or null when the driver has none (before the
// driver is set, or after it is gone). The widget hands it to the overlay's own
// interaction so a press is answered with the same editor the pixels came from.
ctrl::GraphEditor* graphEditorOrNull(ctrl::Driver* aDriver) {
    return aDriver ? aDriver->graphEditor() : nullptr;
}
}
#endif

namespace {
// Whether the animation graph panel is UP. Independent of the ImGui build flag:
// the editor exists either way, and ctrl::GraphEditor::setVisible is the state the
// Driver's cursor routing reads, so this is the same answer the graph itself gives.
//
// What it decides is the MIDDLE button: while the panel is up the middle drag pans
// the panel's window, so this widget must NOT also hand it to ctrl::CanvasMover -
// the graph's own cursor path (ctrl::GraphEditor::updateCursor) is what pans, and
// two owners of one drag would move two cameras at once.
bool graphPanelShown(ctrl::Driver* aDriver) {
    ctrl::GraphEditor* editor = aDriver ? aDriver->graphEditor() : nullptr;
    return editor && editor->isVisible();
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
    mViewportBackground(),
    mUsingTablet(false),
    mViewSetting(),
    mCanvasMover(),
    mMovingCanvasByTool(false),
    mMovingCanvasByKey(false),
    mMovingCanvasByMiddleMouseButton(false),
    mDevicePixelRatio(1.0) {
#ifdef AE_ENABLE_IMGUI
    // FIRST, before anything this widget does can raise an accessibility event:
    // Qt caches ONE accessible interface per QObject, and a QWidget raises its own
    // events (ObjectShow, ParentChanged, StateChanged) as soon as a reader is
    // active - so a widget that is shown or parented while accessibility is on
    // caches its plain interface first, and no factory installed later is ever
    // reached for it (gui/ImGuiA11y.h, "WHERE TO CALL IT"). This is why the
    // factory is installed here rather than from the first frame: the overlay's
    // a11y::install(*this, callback) (ImGuiOverlay::init) is documented as
    // idempotent per widget, so it finds this installation, keeps its interface
    // and id, and only takes over the action callback. Needs no GL context and no
    // model, which is why a constructor may make this call at all.
    a11y::installFactoryOnly(*this);
#endif
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
                        mCanvasMover.setDragAndMove(mMovingCanvasByKey || mMovingCanvasByTool);
                    },
                    [=]() {
                        mMovingCanvasByKey = false;
                        mCanvasMover.setDragAndMove(mMovingCanvasByKey || mMovingCanvasByTool);
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
#ifdef AE_ENABLE_IMGUI
    // Before the painter handle and the GL objects: the overlay's teardown makes this
    // widget's context current to delete the backend's shaders, buffers and textures.
    gImGuiOverlay.reset();
#endif
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

    // a driver created after the toggle was flipped must agree with the panel
    if (mDriver) {
        mDriver->setGraphEditorVisible(mViewSetting.showGraphEditor);
    }
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

#ifdef AE_ENABLE_IMGUI
    // The overlay, on this widget's own context (initializeGL has it current). It IS
    // the animation graph panel: ctrl::Driver::renderQt no longer paints one. A
    // refused backend leaves the pointer null and every hook below a null test, so a
    // context that cannot host ImGui degrades to "no panel" rather than to a crash.
    gImGuiOverlay.reset(new ImGuiOverlay());
    if (!gImGuiOverlay->init(*this))
        gImGuiOverlay.reset();
#endif

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

#ifdef AE_ENABLE_IMGUI
    // The ImGui overlay draws AFTER the QPainter pass, in the same framebuffer and
    // the same GL context, and that order is load-bearing:
    // ctrl::PainterHandle::end() flushes the commands the overlay engine queued
    // (gl::PrimitiveDrawer::end -> flushCommands), so ImGui's draws land on top of
    // the gizmos. Drawing before the painter pass would put ImGui underneath them.
    //
    // The panel is drawn only while the graph is shown - and "shown" is the editor's
    // own state, the same one the Driver's cursor routing obeys, so the pixels and the
    // hit-test can never disagree about whether the panel is up. The frame itself runs
    // either way (it drains ImGuiIO's input queue and keeps its capture flags
    // meaningful).
    if (gImGuiOverlay && gImGuiOverlay->isReady()) {
        ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
        gImGuiOverlay->paint(
            *this, editor, mProject ? &*mProject : nullptr, editor && editor->isVisible()
        );
    }
#endif
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

void MainDisplayWidget::mouseMoveEvent(QMouseEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    // Every move is fed through, including the ones the overlay does not care about:
    // ImGui derives where the pointer is from this stream, and it must know that
    // before a press can be attributed to anything (see ImGuiInputBridge's class
    // comment). The move itself is never swallowed - the panel has no ImGui window to
    // hover, and the viewport's own hover states are what a move is for here.
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->mouseMove(*aEvent);
#endif
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
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady()) {
        // 1. ImGuiIO first, always: a press has to reach ImGui even when the
        //    overlay is about to act on it, because ImGui derives its own state from
        //    the stream and the next frame's capture flags come from it.
        gImGuiOverlay->mouseButton(*aEvent);
        // 2. Then the overlay's OWN controls, which take precedence over the
        //    viewport's cursor routing: a press on a legend swatch toggles that
        //    channel, a press on a legend label focuses it. Both are hit-tested
        //    against the geometry of the frame that was drawn - the model of the last
        //    paint - so the control under the pointer is the control on screen.
        ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
        if (aEvent->button() == Qt::LeftButton && editor && gImGuiOverlay->press(*editor, aEvent->position().toPoint())) {
            // Our own interaction changed something: the panel's next frame must show
            // it (a channel left the model, or the focused one moved).
            this->update();
            return;
        }
    }
#endif
    if (mRenderInfo) {
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setMousePress(aEvent, mRenderInfo->camera)) {
            updateCursor();
            // if (!mUsingTablet) qDebug() << "press";

            // The MIDDLE drag belongs to the animation graph panel while the panel
            // is up: the graph pans its own window (ctrl::GraphEditor::updateCursor
            // handles the middle button through the cursor this press just fed),
            // and this widget must not also start moving the canvas camera - two
            // owners of one drag would move two views at once.
            if (mViaPoint.mouseSetting().middleMouseMoveCanvas &&
                aEvent->button() == Qt::MouseButton::MiddleButton && !graphPanelShown(mDriver)) {
                mMovingCanvasByMiddleMouseButton = true;
                mCanvasMover.setDragAndMove(
                    mMovingCanvasByKey || mMovingCanvasByTool || mMovingCanvasByMiddleMouseButton
                );
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
#ifdef AE_ENABLE_IMGUI
    // The release must reach ImGui even for a press the overlay consumed, or ImGui
    // would keep the button latched; the viewport path below never sees a press it
    // did not get, and the tools' gesture state is unaffected.
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->mouseButton(*aEvent);
#endif
    if (mRenderInfo) {
        if (mDriver) {
            mDriver->setGraphModifiers(aEvent->modifiers());
        }
        if (mAbstractCursor.setMouseRelease(aEvent, mRenderInfo->camera)) {
            updateCursor();
            // if (!mUsingTablet) qDebug() << "release";

            if (aEvent->button() == Qt::MouseButton::MiddleButton) {
                // A canvas middle pan this widget began ends here; a middle drag
                // the GRAPH took never set this flag (see mousePressEvent), so this
                // is a no-op for it and its own endPan() runs on the cursor path.
                mMovingCanvasByMiddleMouseButton = false;
                mCanvasMover.setDragAndMove(
                    mMovingCanvasByKey || mMovingCanvasByTool || mMovingCanvasByMiddleMouseButton
                );
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
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->wheel(*aEvent);
#endif
    // the graph owns the wheel while the cursor is over its HUD
    if (mDriver && mRenderInfo) {
        // The modifier state goes with it: the wheel zooms BOTH axes by default
        // and one axis under Shift (the frame) or Ctrl (the value), so the graph
        // needs the same modifiers the cursor path gets.
        mDriver->setGraphModifiers(aEvent->modifiers());
        if (mDriver->updateGraphWheel(
                mRenderInfo->camera, aEvent->position().toPoint(), aEvent->angleDelta().y(), aEvent->modifiers()
            )) {
            // The graph took the wheel, so its zoom changed - a PANEL state, not a canvas
            // one, which is why the graph reports the take instead of letting the viewport
            // scale the artwork. Nothing below this return runs for that case, and the
            // panel is drawn in paintEvent, so without a request here the widget has no
            // reason to paint: the new frames-per-pixel would stay invisible until
            // something unrelated asked for a frame.
            //
            // This is the same request the cursor path makes: updateCursor(), below, calls
            // updateRender() when the Driver answers that the graph needs a repaint. The
            // canvas wheel below paints itself for the same reason, through the
            // CanvasMover's own true.
            updateRender();
            return;
        }
    }

    if (mCanvasMover.updateByWheel(
            QVector2D(aEvent->position()), aEvent->angleDelta().y(), mViaPoint.mouseSetting().invertMainViewScaling
        )) {
        updateRender();
    }
}

void MainDisplayWidget::tabletEvent(QTabletEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    // A pen is a pointer to ImGui. The accept() at the end of this handler is what
    // keeps Qt from also synthesizing a mouse event for a tablet event the overlay
    // consumed (see ImGuiInputBridge::handleTablet).
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->tablet(*aEvent);
#endif
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
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady() && aEvent->button() == Qt::LeftButton) {
        ctrl::GraphEditor* editor = graphEditorOrNull(mDriver);
        if (editor && gImGuiOverlay->doubleClick(*editor, aEvent->position().toPoint())) {
            // Qt delivers the second press of a double-click HERE and not as a
            // MouseButtonPress, so this is the only place ImGui can be told about
            // it - and this return is why it is told here rather than at the top of
            // the handler: the event falls through to mousePressEvent() below, which
            // feeds the bridge, so either path feeds it exactly once.
            gImGuiOverlay->mouseButton(*aEvent);
            // The panel's own fit gesture: both axes back on the shown data.
            this->update();
            return;
        }
    }
#endif
    // Not the panel's gesture. QWidget's own double-click behaviour is "a press" (its
    // default implementation calls mousePressEvent), and that is what the viewport
    // wants here: the second click still drags, and it still inserts or selects in
    // the graph.
    QOpenGLWidget::mouseDoubleClickEvent(aEvent);
}

void MainDisplayWidget::keyPressEvent(QKeyEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady() && gImGuiOverlay->key(*aEvent)) {
        // An ImGui text field owns the keyboard this frame: the key is the field's and
        // must not travel on. (gui::KeyCommandInvoker gates the rebindable commands on
        // the same flag, so nothing fires twice either way.)
        aEvent->accept();
        return;
    }
#endif
    // Otherwise the event is ignored, i.e. it propagates to the main window's key
    // map, which is where the application's key routing has always been.
    QOpenGLWidget::keyPressEvent(aEvent);
}

void MainDisplayWidget::keyReleaseEvent(QKeyEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady() && gImGuiOverlay->key(*aEvent)) {
        aEvent->accept();
        return;
    }
#endif
    QOpenGLWidget::keyReleaseEvent(aEvent);
}

void MainDisplayWidget::inputMethodEvent(QInputMethodEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady() && gImGuiOverlay->inputMethod(*aEvent)) {
        // A composition or a commit the ImGui text field swallowed: Qt's platform
        // input context is done with it.
        aEvent->accept();
        return;
    }
#endif
    QOpenGLWidget::inputMethodEvent(aEvent);
}

QVariant MainDisplayWidget::inputMethodQuery(Qt::InputMethodQuery aQuery) const {
#ifdef AE_ENABLE_IMGUI
    // Qt asks the FOCUS OBJECT for these, and the overlay is where the caret of an
    // ImGui text field lives (see ImGuiInputBridge::inputMethodQuery). An unanswered
    // query falls through to the base implementation.
    if (gImGuiOverlay && gImGuiOverlay->isReady()) {
        const QVariant answer = gImGuiOverlay->inputMethodQuery(aQuery);
        if (answer.isValid())
            return answer;
    }
#endif
    return QOpenGLWidget::inputMethodQuery(aQuery);
}

void MainDisplayWidget::focusInEvent(QFocusEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    // ImGui needs to know when the stream of events stops and starts again: on focus
    // loss it clears its own key and mouse state, and the platform input context is
    // reset with it. The widget only ever holds focus for an ImGui text field (see
    // paint(): it is asked for focus while a field wants text), which is exactly when
    // that matters.
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->focus(true);
#endif
    QOpenGLWidget::focusInEvent(aEvent);
}

void MainDisplayWidget::focusOutEvent(QFocusEvent* aEvent) {
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady())
        gImGuiOverlay->focus(false);
#endif
    QOpenGLWidget::focusOutEvent(aEvent);
}

bool MainDisplayWidget::event(QEvent* aEvent) {
    const bool handled = QOpenGLWidget::event(aEvent);
#ifdef AE_ENABLE_IMGUI
    if (gImGuiOverlay && gImGuiOverlay->isReady()) {
        switch (aEvent->type()) {
        case QEvent::DevicePixelRatioChange:
            // After the base class, which is what has just updated this widget's
            // ratio. ImGui rasterizes glyphs at io.DisplayFramebufferScale, so it has
            // to be told when that changes (the bridge re-keys the atlas; see
            // ImGuiInputBridge::handleDprChanged).
            gImGuiOverlay->dprChanged(this->devicePixelRatioF());
            break;
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            // One finger is a pointer to ImGui, more than one is not (the bridge says
            // so by returning false) - and what the bridge consumed is accepted here so
            // Qt does not ALSO synthesize a mouse event for it. NOTE: this widget does
            // not set Qt::WA_AcceptTouchEvents, because Qt's synthesis of mouse events
            // from touches is what the viewport's tools are built on today; without
            // that attribute Qt never delivers a QTouchEvent here, so this branch is
            // the translation path for a platform that does.
            if (gImGuiOverlay->touch(*static_cast<QTouchEvent*>(aEvent)))
                aEvent->accept();
            break;
        default:
            break;
        }
    }
#endif
    return handled;
}

void MainDisplayWidget::onVisualUpdated() { updateRender(); }

void MainDisplayWidget::onToolChanged(ctrl::ToolType aType) {
    if (aType == ctrl::ToolType_Cursor) {
        this->setCursor(Qt::OpenHandCursor);
        mMovingCanvasByTool = true;
        mCanvasMover.setDragAndMove(mMovingCanvasByKey || mMovingCanvasByTool);
    } else {
        this->setCursor(Qt::ArrowCursor);
        mMovingCanvasByTool = false;
        mCanvasMover.setDragAndMove(mMovingCanvasByKey || mMovingCanvasByTool);
    }
}

void MainDisplayWidget::onFinalizeTool(ctrl::ToolType) {
    mAbstractCursor.suspendEvent([=]() { updateCursor(); });
    mAbstractCursor.resumeEvent();
}

void MainDisplayWidget::onViewSettingChanged(const MainViewSetting& aSetting) {
    mViewSetting = aSetting;

    if (mDriver) {
        mDriver->setGraphEditorVisible(mViewSetting.showGraphEditor);
    }

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
            updateRender();
        }
    }
}

} // namespace gui
