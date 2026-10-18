#include "gui/GraphPanelWidget.h"

#include <QFocusEvent>
#include <QHideEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWheelEvent>

// The GL3 backend is named directly for one call: the frame's vertices go into THIS
// widget's framebuffer with the renderer's own entry point. It is the backend the
// gui/GraphPanelFrame.{h,cpp} frame driver brought up; nothing here initializes it.
#include "backends/imgui_impl_opengl3.h"

#include "core/Frame.h"
#include "core/ObjectNode.h"
#include "core/TimeLineEvent.h"
#include "ctrl/Driver.h"
#include "ctrl/GraphEditor.h"
#include "gl/Util.h"
#include "gui/GUIResources.h"
#include "gui/ImGuiA11y.h"
#include "gui/ImGuiInputBridge.h"
#include "gui/MainDisplayWidget.h"
#include "gui/MouseSetting.h"
#include "gui/theme/Colors.h"

namespace gui {

//-------------------------------------------------------------------------------------------------
GraphPanelWidget::GraphPanelWidget(GUIResources& aResources, QWidget* aParent):
    QOpenGLWidget(aParent),
    mResources(aResources),
    mThemeSlot(),
    mDriver(nullptr),
    mBridge(),
    mFrame(),
    mCursor(),
    mReady(false) {
    this->setObjectName(QStringLiteral("GraphPanelWidget"));
    // THE PANEL CAN OWN THE KEYBOARD. It has two keys of its own (Ctrl+A and Escape,
    // see panelKeyAction) and they are the panel's only while the panel is the focus
    // widget, so a press inside it has to make it one: without a focus policy Qt gives
    // a click focus to nobody (QWidget's default is Qt::NoFocus), the panel never sees
    // a key press (the widget has held focus only for an ImGui text field so far, see
    // paintGL), and the two actions are unreachable by construction. Qt::StrongFocus is
    // what the timeline page beside it uses (TimeLineWidget), so the dock's two pages
    // take the keyboard the same way.
    this->setFocusPolicy(Qt::StrongFocus);
    // The panel's own hover states need moves with no button down: the hovered curve
    // gets its chip, and the tangent handle nearest the pointer is the one drawn
    // highlighted (both read ImGui::GetIO().MousePos, which this widget's bridge feeds
    // from these moves).
    this->setMouseTracking(true);

    // The accessible factory goes in HERE, from the constructor, for the reason
    // gui::MainDisplayWidget's constructor does it (gui/ImGuiA11y.h, "WHERE TO CALL IT"):
    // a widget that is shown or parented while accessibility is active caches its plain
    // interface first, and no factory installed later is reached for it - so the panel's
    // publication (gui::publishPanelAccessibility, from this host's first frame) would
    // find nothing to take over. It needs no GL context and no model.
    a11y::installFactoryOnly(*this);
    // AND THAT IS THE WHOLE INSTALLATION: this widget is the panel's only host, so
    // there is no second widget that could take the factory (and no hand-over to get
    // wrong). The action callback arrives with the first frame (see
    // publishAccessibility).

    // The floor colour and the panel's palette are read per frame from
    // theme::Colors::current(), so a theme change needs no cached state - only a frame,
    // which nothing else would ask this widget for.
    mThemeSlot = mResources.onThemeChanged.connect(this, &GraphPanelWidget::onThemeUpdated);
}

GraphPanelWidget::~GraphPanelWidget() {
    mResources.onThemeChanged.disconnect(mThemeSlot);
    // The GL objects (this host's hold on the shared context, its input bridge) go with
    // a current context, while this widget still has one.
    shutdownHost();
}

void GraphPanelWidget::shutdownHost() {
    // The bridge is ImGui's platform IME target and reads the context: drop it before
    // the shared context can go.
    mBridge.reset();
    mFrame = PanelFrameState();

    if (!mReady)
        return;

    // The context, the backend and the atlas are NOT this host's: releasePanelContext
    // drops this host's hold and tears them down only when it was the last holder, with
    // the context made current for it while the widget is still usable.
    releasePanelContext(*this);
    mReady = false;
}

void GraphPanelWidget::setDriver(ctrl::Driver* aDriver) {
    mDriver = aDriver;

    // A NEW driver is a NEW panel (the driver owns the editor and is recreated per
    // project), so nothing of the old one may survive in this host: the last frame's
    // model and editor are dropped, or a hit between this call and the next paint would
    // be tested against a panel that is gone.
    mFrame.model = GraphPanelModel();
    mFrame.editor = nullptr;
    refresh();
}

void GraphPanelWidget::refresh() { this->update(); }

void GraphPanelWidget::onThemeUpdated(theme::Theme&) { this->update(); }

//-------------------------------------------------------------------------------------------------
void GraphPanelWidget::initializeGL() {
    // initializeGL runs when this page is first SHOWN (Qt initializes the widget's GL
    // resources on its first paint), and it runs again whenever the widget is given a
    // new GL context (a reparent, a screen change). Both are handled the same way: the
    // old hold is dropped and the shared context acquired again, so this host draws with
    // the objects of the context it is actually rendering into.
    if (mReady)
        shutdownHost();

    if (!acquirePanelContext(*this)) {
        // The backend refused. The page still exists and still paints its floor; the
        // panel is simply not drawn, which is the degradation the panel always had on a
        // context that cannot host ImGui.
        return;
    }

    mBridge.reset(new ImGuiInputBridge(*this));
    mReady = true;
}

//-------------------------------------------------------------------------------------------------
void GraphPanelWidget::paintGL() {
    // THE FLOOR FIRST, and always. The panel draws no background of its own (it is a
    // model and a draw list; there is no fill anywhere in drawPanel), so behind THIS panel
    // there is only what this widget paints, and it has to paint something:
    // a QOpenGLWidget is composited over the window and the widgets beside it do not
    // show through it (qopenglwidget.html, "Putting other widgets underneath and making
    // the QOpenGLWidget transparent will not lead to the expected results"). The colour
    // is the dock's own panel floor - theme::Colors::base, "window, docks, panels,
    // chrome, menu floor", which is the reference's own _floor (mock-graph.js:
    // cssVar('--base')) - read per frame so a theme change needs no signal.
    //
    // resetRenderState() is the application's own "known state before a pass": it turns
    // off the scissor box (a glClear is scissored, and the ImGui backend's last draw in
    // this shared context left one set), blending, depth and stencil. The backend sets
    // everything IT needs before its own draws, so this is the floor's state only.
    gl::Util::resetRenderState();
    const QColor floor = theme::Colors::current().base;
    gl::Util::clearColorBuffer(floor.redF(), floor.greenF(), floor.blueF(), 1.0f);

    if (!mReady)
        return; // no context for the panel: the floor is all this host can draw

    ctrl::GraphEditor* editor = graphEditor();

    // THE EDITOR'S GEOMETRY, PINNED TO THIS WIDGET. The frame window the panel shares
    // with the timeline is mapped in PIXELS - pixels per frame, computed from the plot's
    // width - so the mapping left behind by the viewport's frame (or by a previous size
    // of this one) must not be what the panel is built from. This is the call
    // MainDisplayWidget::paintEvent makes, for exactly this reason, and it is idempotent
    // when nothing changed. The WINDOW itself and the SELECTION are the timeline's, wired
    // once by MainDisplayWidget::syncGraphPanelSharedState: nothing here sets them.
    if (editor)
        editor->syncFrameWindowFor(this->size());

    // THE FRAME: the shared one, and the panel is up whenever this page is on screen and
    // a project's editor exists. aTopInset stays 0 - nothing floats over the dock's page
    // (the tab names it and its header belongs to the dock, not to this widget), so the
    // ruler band sits at the plot's own top here.
    ImDrawData* drawData = runPanelFrame(
        *this, GraphPanelHost::Dock, editor, editor != nullptr, mFrame
    );

    // ...into THIS widget's framebuffer, which QOpenGLWidget has bound and whose viewport
    // it has already set: the renderer takes the projection, the viewport and the scissor
    // boxes from the draw data, which the frame built from this host's own size and device
    // pixel ratio. No contrast-outline composite here: its ring answers ink lost against
    // arbitrary artwork, and this floor is one known opaque surface.
    if (drawData)
        ImGui_ImplOpenGL3_RenderDrawData(drawData);

    // The QWidget side of the IME contract (see ImGuiInputBridge's class comment): Qt
    // asks the FOCUS OBJECT for input-method queries, and a plain QOpenGLWidget is not an
    // input item at all - so the attribute is set exactly while a Dear ImGui text field
    // wants text, and the widget takes focus so Qt asks it. With no ImGui field on screen
    // this is a no-op every frame.
    const bool wantsText = mBridge && mBridge->imeWantsTextInput();
    if (this->testAttribute(Qt::WA_InputMethodEnabled) != wantsText)
        this->setAttribute(Qt::WA_InputMethodEnabled, wantsText);
    if (wantsText && !this->hasFocus())
        this->setFocus(Qt::OtherFocusReason);

    publishAccessibility();
}

void GraphPanelWidget::hideEvent(QHideEvent* aEvent) {
    QOpenGLWidget::hideEvent(aEvent);

    // A GESTURE LEFT OPEN GOES NO FURTHER. The tab can be switched away from with a key
    // move, a handle drag, a marquee or a pan in flight (the release usually beats the
    // click, but not always), and a gesture kept open would coalesce the next edit into a
    // command whose geometry nobody can see any more - so the editor drops it here
    // (ctrl::GraphEditor::panelHidden: endDrag(), and nothing else). The keys, the
    // selection and the scale are kept, so the page coming back is the panel as it was,
    // and the command a dropped gesture had already pushed stays on the undo stack -
    // dropping the gesture is not an undo.
    if (ctrl::GraphEditor* editor = graphEditor())
        editor->panelHidden();

    // Another page of the dock is current: the panel is not on screen, and the ONE
    // accessibility publication has to say so rather than leave the tree showing the last
    // frame of a hidden tab - the installation is this widget's for the process, so this
    // empties it. A host that was never shown publishes nothing at all: the shared
    // function tells the two apart.
    PanelFrameState offScreen;
    publishPanelAccessibility(*this, offScreen, &GraphPanelWidget::accessibilityAction, this);
}

//-------------------------------------------------------------------------------------------------
ctrl::GraphEditor* GraphPanelWidget::graphEditor() const {
    return mDriver ? mDriver->graphEditor() : nullptr;
}

core::CameraInfo GraphPanelWidget::panelCamera() const {
    // ctrl::GraphEditor derives its panel rect, and every hit-test and gesture it runs,
    // from aCamera.screenSize() alone (GraphEditor::currentPanelRect reads nothing else),
    // so this is the whole of the camera state the panel's own geometry needs - and it
    // must be THIS widget's size, because the panel drawn here is this widget. The world
    // transform is never read on the graph's cursor path; a default CameraInfo (scale 1,
    // no rotation) is therefore not a stand-in for a scene camera, it is the absence of
    // one, which is what the panel asks for.
    core::CameraInfo camera;
    camera.setScreenSize(this->size());
    camera.setDevicePixelRatio(this->devicePixelRatioF());
    return camera;
}

bool GraphPanelWidget::panelTakesCursor() const {
    ctrl::GraphEditor* editor = graphEditor();
    return editor && editor->ownsCursor(panelCamera(), mCursor);
}

void GraphPanelWidget::routeCursor() {
    ctrl::GraphEditor* editor = graphEditor();
    if (!editor || !panelTakesCursor())
        return; // this point is nobody's (or the panel is gone): nothing acts on it

    // THE EDITOR ITSELF, and not through the viewport: ctrl::Driver routes the viewport's
    // events to the tools and no longer arbitrates the panel at all, and ctrl::GraphEditor
    // has no shown/hidden flag left to gate on - this page IS the panel while its tab is
    // current, which is the only "visible" there is. The editor's own contract is
    // unchanged: ownsCursor above is its arbitration, updateCursor its pass, and the camera
    // it is handed is this widget's (see panelCamera()).
    if (editor->updateCursor(panelCamera(), mCursor))
        this->update(); // the editor reports it changed something (selection, window, document)
}

//-------------------------------------------------------------------------------------------------
void GraphPanelWidget::mouseMoveEvent(QMouseEvent* aEvent) {
    // Every move is fed to ImGui, including the ones it does not care about: ImGui
    // derives where the pointer is from this stream, and it has to know that before a
    // press can be attributed to anything (see ImGuiInputBridge's class comment). The
    // move itself is never swallowed by the panel - it has no ImGui window to hover.
    if (mBridge)
        mBridge->handleMouseMove(*aEvent);

    ctrl::GraphEditor* editor = graphEditor();
    if (!editor) {
        aEvent->ignore();
        return;
    }

    // The panel's gestures read the modifier state from here: neither
    // core::AbstractCursor nor the editor's updateCursor carries modifiers, so every
    // move re-reads them, which is what lets Shift constrain a key drag already in
    // flight (see ctrl::GraphEditor::setModifiers).
    editor->setModifiers(aEvent->modifiers());
    mCursor.setMouseMove(aEvent, panelCamera());
    routeCursor();
    aEvent->accept();
}

void GraphPanelWidget::mousePressEvent(QMouseEvent* aEvent) {
    if (mBridge)
        mBridge->handleMouseButton(*aEvent);

    ctrl::GraphEditor* editor = graphEditor();
    if (!editor) {
        aEvent->ignore();
        return;
    }

    const QPoint pos = aEvent->position().toPoint();

    // The panel's ONE control takes a left press before the plot's gestures do, and it
    // takes it from the geometry that was DRAWN (the model of the last frame), so the
    // control under the pointer is the control on screen.
    if (aEvent->button() == Qt::LeftButton && pressChannelButton(pos)) {
        this->update(); // the list may have changed what the panel shows
        aEvent->accept();
        return;
    }

    // A right press is the key menu's gesture - the panel's only right-button action -
    // and it is taken here because this is the only place this host sees one. The menu
    // is the application's own QMenu and QMenu::popup() returns immediately, so nothing
    // here blocks the press.
    if (aEvent->button() == Qt::RightButton) {
        openKeyMenuAt(pos);
        aEvent->accept();
        return;
    }

    editor->setModifiers(aEvent->modifiers());
    mCursor.setMousePress(aEvent, panelCamera());
    routeCursor();
    aEvent->accept();
}

void GraphPanelWidget::mouseReleaseEvent(QMouseEvent* aEvent) {
    // The release must reach ImGui even for a press the panel acted on, or ImGui would
    // keep the button latched.
    if (mBridge)
        mBridge->handleMouseButton(*aEvent);

    ctrl::GraphEditor* editor = graphEditor();
    if (!editor) {
        aEvent->ignore();
        return;
    }

    editor->setModifiers(aEvent->modifiers());
    mCursor.setMouseRelease(aEvent, panelCamera());
    // A gesture in flight owns the release wherever it lands (the editor says so in
    // ownsCursor), which is what ends a drag that left the widget.
    routeCursor();
    aEvent->accept();
}

void GraphPanelWidget::mouseDoubleClickEvent(QMouseEvent* aEvent) {
    if (aEvent->button() == Qt::LeftButton && doubleClickAt(aEvent->position().toPoint())) {
        // Qt delivers the second press of a double-click HERE and not as a
        // MouseButtonPress, so this is the only place ImGui can be told about it - and
        // it is told here rather than at the top of the handler: the other path falls
        // through to mousePressEvent() below, which feeds the bridge, so either way the
        // bridge sees the event exactly once.
        if (mBridge)
            mBridge->handleMouseButton(*aEvent);
        this->update();
        aEvent->accept();
        return;
    }

    // Not the panel's fit. QWidget's own double-click behaviour is "a press" (its
    // default implementation calls mousePressEvent), and that is what this page wants:
    // the second click still selects, drags or inserts in the graph.
    QOpenGLWidget::mouseDoubleClickEvent(aEvent);
}

void GraphPanelWidget::wheelEvent(QWheelEvent* aEvent) {
    if (mBridge)
        mBridge->handleWheel(*aEvent);

    ctrl::GraphEditor* editor = graphEditor();
    if (!editor) {
        aEvent->ignore();
        return;
    }

    editor->setModifiers(aEvent->modifiers());

    // THE ZOOM DIRECTION is the dock's own setting. The panel takes the wheel while its
    // tab is current, and it takes the direction of the SURFACE it is drawn on: this page
    // covers the TIMELINE's body, so it zooms the graph the way the timeline's own wheel
    // zooms it (gui::TimeLineEditorWidget reads invertTimeLineScaling), and one wheel in
    // one body cannot turn the frame axis two
    // ways. The setting is read from the application's settings here (the same file
    // gui::GeneralSettingDialog writes and gui::ViaPoint's live copy is kept in step
    // with), because this host is handed neither.
    MouseSetting settings;
    settings.load();
    if (editor->updateWheel(
            panelCamera(), aEvent->position().toPoint(), aEvent->angleDelta().y(), aEvent->modifiers(),
            settings.invertTimeLineScaling
        )) {
        this->update();
    }
    aEvent->accept();
}

void GraphPanelWidget::tabletEvent(QTabletEvent* aEvent) {
    // A pen is a pointer to ImGui, and the accept() below is what keeps Qt from also
    // synthesizing a mouse event for a tablet event the bridge consumed (see
    // ImGuiInputBridge::handleTablet).
    const bool consumed = mBridge && mBridge->handleTablet(*aEvent);

    ctrl::GraphEditor* editor = graphEditor();
    if (editor) {
        editor->setModifiers(aEvent->modifiers());
        if (mCursor.setTabletEvent(aEvent, panelCamera()))
            routeCursor();
    }

    if (consumed)
        aEvent->accept();
    else
        QOpenGLWidget::tabletEvent(aEvent);
}

void GraphPanelWidget::keyPressEvent(QKeyEvent* aEvent) {
    // An ImGui text field owns the keyboard this frame: the key is the field's and must
    // not travel on. gui::KeyCommandInvoker gates the application's rebindable commands
    // on the same flag (ImGuiInputBridge::keyboardCaptured), and that flag is read off
    // the ONE ImGui context - so the two hosts of one panel, and the key map, cannot
    // disagree about who has the keyboard.
    if (mBridge && mBridge->handleKey(*aEvent)) {
        aEvent->accept();
        return;
    }

    // THE PANEL'S OWN TWO KEYS, for a press that got this far unconsumed. Consumed
    // exactly when the panel acted on it (see panelKeyAction): a Ctrl+A or an Escape
    // that changed nothing is not this widget's to swallow.
    if (panelKeyAction(*aEvent)) {
        aEvent->accept();
        return;
    }

    // Otherwise the event is ignored, i.e. it propagates to the main window's key map,
    // which is where the application's key routing has always been.
    QOpenGLWidget::keyPressEvent(aEvent);
}

bool GraphPanelWidget::panelKeyAction(const QKeyEvent& aEvent) {
    // WHO HAS THE KEYBOARD, in two conditions that are about the keyboard rather than
    // about the key. Together they are the answer to "is this press the panel's?":
    // the two actions edit the panel's own selection, so they belong to the panel only
    // while the panel is what the user is looking at AND nothing ahead of it took the
    // press.
    //
    // 1. THIS WIDGET MUST BE THE FOCUS WIDGET. Ctrl+A here is the panel's Ctrl+A, not
    //    a global shortcut: while some other widget owns the keyboard (the object
    //    tree, the timeline, a property field) its Ctrl+A and its Escape are that
    //    widget's business, and a key this panel did not consume must reach
    //    gui::MainWindow's key map exactly as it always has. hasFocus() is also what
    //    keeps a press that ARRIVED here by propagation honest: Qt sends a key up the
    //    parent chain only after the focus widget ignored it, so a forwarded press
    //    (from a future child widget of this host) would otherwise act on the panel
    //    while the user's focus is elsewhere.
    // 2. DEAR IMGUI MUST NOT HAVE TAKEN IT. keyboardCaptured() is
    //    io.WantCaptureKeyboard off the ONE shared context - the flag
    //    gui::KeyCommandInvoker gates the application's commands on, and the flag the
    //    bridge's own handleKey() above answers with. It is asked again here rather
    //    than inferred from that call because this host may have NO bridge while the
    //    other host has one: with no ImGui context at all (no GL, or initializeGL
    //    never ran) mBridge is null and handleKey() is skipped, while an active ImGui
    //    text field in the viewport's host still owns the keys - the rule is about the
    //    context, not about this host's hold on it.
    //
    // AND NOTHING IS NEEDED FOR A QMENU OR THE TIMELINE'S EASING PICKER, which is the
    // other half of "Escape belongs to whoever consumed it": both are Qt popups that
    // hold the keyboard while they are up, and the popup takes the key ON THE WINDOW'S
    // OWN ROUTING - a path QApplication::sendEvent (and every test that injects a
    // QKeyEvent) does not enter. Measured against Qt 6.11 for this binding: with a QMenu
    // popped up, an Escape delivered through the window closed the menu and the focused
    // widget's handler was NOT called, while the very same widget kept hasFocus() true
    // throughout. So the first condition cannot stand in for this one - a popup really
    // does leave this widget the focus widget - and the popup case is decided by Qt's
    // delivery, before this function is reached. That is also why aEvent.isAccepted() is
    // not consulted: an event something else consumed is never delivered here, and a
    // delivered key event arrives ACCEPTED by Qt's own default (QEvent's m_accept), the
    // flag QWidget::keyPressEvent is what clears.
    if (!this->hasFocus())
        return false;
    if (ImGuiInputBridge::keyboardCaptured())
        return false;

    ctrl::GraphEditor* editor = graphEditor();
    if (!editor)
        return false; // no project on screen: the panel has no selection to edit

    // THE TWO BINDINGS, with the application's own rule for what a binding is
    // (ctrl::KeyBinding::matchesExactlyWith): the key and the modifiers exactly -
    // Ctrl+A is not Ctrl+Shift+A, and a bare Escape is not Escape-with-a-modifier.
    // Both actions ANSWER whether they changed the selection, and that answer is what
    // this returns: the panel consumes the press it acted on and leaves the rest.
    if (aEvent.key() == Qt::Key_A && aEvent.modifiers() == Qt::ControlModifier)
        return editor->selectAllMarkers();
    if (aEvent.key() == Qt::Key_Escape && aEvent.modifiers() == Qt::NoModifier)
        return editor->clearSelection();
    return false;
}

void GraphPanelWidget::keyReleaseEvent(QKeyEvent* aEvent) {
    if (mBridge && mBridge->handleKey(*aEvent)) {
        aEvent->accept();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(aEvent);
}

void GraphPanelWidget::inputMethodEvent(QInputMethodEvent* aEvent) {
    if (mBridge && mBridge->handleInputMethod(*aEvent)) {
        // A composition or a commit the ImGui text field swallowed: Qt's platform input
        // context is done with it.
        aEvent->accept();
        return;
    }
    QOpenGLWidget::inputMethodEvent(aEvent);
}

QVariant GraphPanelWidget::inputMethodQuery(Qt::InputMethodQuery aQuery) const {
    // Qt asks the FOCUS OBJECT for these, and the panel's input bridge is where the
    // caret of an ImGui text field lives (see ImGuiInputBridge::inputMethodQuery). An
    // unanswered query falls through to the base implementation.
    if (mBridge) {
        const QVariant answer = mBridge->inputMethodQuery(aQuery);
        if (answer.isValid())
            return answer;
    }
    return QOpenGLWidget::inputMethodQuery(aQuery);
}

void GraphPanelWidget::focusInEvent(QFocusEvent* aEvent) {
    // ImGui needs to know when the stream of events stops and starts again: on focus
    // loss it clears its own key and mouse state, and the platform input context is reset
    // with it. This widget only ever holds focus for an ImGui text field (paintGL() asks
    // for it while a field wants text).
    if (mBridge)
        mBridge->handleFocus(true);
    QOpenGLWidget::focusInEvent(aEvent);
}

void GraphPanelWidget::focusOutEvent(QFocusEvent* aEvent) {
    if (mBridge)
        mBridge->handleFocus(false);
    QOpenGLWidget::focusOutEvent(aEvent);
}

bool GraphPanelWidget::event(QEvent* aEvent) {
    const bool handled = QOpenGLWidget::event(aEvent);

    if (mBridge) {
        switch (aEvent->type()) {
        case QEvent::DevicePixelRatioChange:
            // After the base class, which is what has just updated this widget's ratio.
            // The frame rasterizes glyphs at io.DisplayFramebufferScale, so the bridge
            // has to be told when that changes (it re-keys the atlas), and this frame of
            // the panel is stale for the same reason.
            mBridge->handleDprChanged(this->devicePixelRatioF());
            this->update();
            break;
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            // One finger is a pointer to ImGui, more than one is not (the bridge says so
            // by returning false) - and what the bridge consumed is accepted here so Qt
            // does not ALSO synthesize a mouse event for it. Like the viewport host, this
            // widget does not set Qt::WA_AcceptTouchEvents, so this branch is the
            // translation path for a platform that delivers the events anyway.
            if (mBridge->handleTouch(*static_cast<QTouchEvent*>(aEvent)))
                aEvent->accept();
            break;
        default:
            break;
        }
    }

    return handled;
}

//-------------------------------------------------------------------------------------------------
bool GraphPanelWidget::pressChannelButton(const QPoint& aPos) {
    if (!mFrame.editor)
        return false; // no panel on screen: nothing to hit
    if (mFrame.model.channelsButton.isEmpty() || !mFrame.model.channelsButton.contains(aPos))
        return false; // an empty box is not a control (no room for it in the plot)

    openChannelMenuAt(aPos);
    return true;
}

void GraphPanelWidget::openChannelMenuAt(const QPoint& aPos) {
    MainDisplayWidget* display = appDisplay();
    if (!display)
        return; // no viewport in this window (a harness): no menu to show

    // THE LIST IS THE APPLICATION'S OWN: the rows travel to gui::MainDisplayWidget, which
    // builds and pops a real QMenu and whose actions toggle a channel's visibility and
    // focus it. This page draws no popup and measures none - there is one list of channels
    // in the application.
    //
    // AND THE POSITION IS MAPPED INTO IT. showGraphChannelMenu anchors the menu at
    // aWidgetPos IN ITS OWN COORDINATES (it is the object that calls mapToGlobal), and
    // this page sits in the dock, not over the viewport: without the mapping the menu
    // would open one dock-width away from the button that asked for it.
    display->showGraphChannelMenu(mFrame.model.channels, display->mapFrom(this, aPos));
}

bool GraphPanelWidget::doubleClickAt(const QPoint& aPos) {
    if (!mFrame.editor)
        return false;
    if (!mFrame.model.plot.contains(aPos))
        return false; // not the plot: nothing here owns a double-click outside it

    // A double-click ON the panel's ONE control is that control's business, not the
    // fit's: the first press already opened the channel list, and a second click must not
    // turn it into a zoom change. An empty button box is not a control.
    if (!mFrame.model.channelsButton.isEmpty() && mFrame.model.channelsButton.contains(aPos))
        return false;

    // Both axes to the shown data: the panel's own fit gesture, the same one the
    // viewport host performs on the same double-click.
    mFrame.editor->fitAll();
    return true;
}

bool GraphPanelWidget::openKeyMenuAt(const QPoint& aPos) {
    if (!mFrame.editor)
        return false; // no panel on screen: nothing to hit

    // THE KEY UNDER THE POINTER, by the editor's own rule - the one hit test both hosts
    // of the panel ask (panelKeyAt, in gui/GraphPanelFrame.h). A handle is not considered:
    // the panel's right-click has no handle action, and the editor's own handle grab is
    // the LEFT press's.
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    core::Frame frame;
    if (!panelKeyAt(mFrame.model, aPos, node, channel, frame))
        return false; // no key that close: this is not the panel's right-click

    // The right-click makes that key the panel's selection, which is the timeline's own
    // behaviour with its key menu: the menu acts on the selection, so the key it was
    // opened on has to be in it.
    mFrame.editor->selectKey(*node, channel, frame);

    // ...and the menu is the TIMELINE'S OWN, through the widget that owns the timeline:
    // one menu, one list of easings, one edit path for both views. Like the channel list
    // above, the position goes in the display's coordinates.
    core::TimeLineEvent selection;
    if (!mFrame.editor->retrieveSelection(selection) || selection.targets().isEmpty())
        return false;
    MainDisplayWidget* display = appDisplay();
    if (!display)
        return false;
    display->showGraphKeyMenu(selection, display->mapFrom(this, aPos));

    this->update(); // the selection changed under the panel's ink (the key turns selected)
    return true;
}

MainDisplayWidget* GraphPanelWidget::appDisplay() const {
    // The application's QMenus live in the viewport widget, and the panel's host
    // interface carries no reference to it (this widget is constructed with a
    // GUIResources and a parent). There is exactly ONE per window - the dock this page
    // lives in is in the same one - so the window is asked for it. Nothing else about the
    // viewport is used: no editor state, no camera, no GL.
    QWidget* top = this->window();
    return top ? top->findChild<MainDisplayWidget*>() : nullptr;
}

//-------------------------------------------------------------------------------------------------
void GraphPanelWidget::publishAccessibility() {
    publishPanelAccessibility(*this, mFrame, &GraphPanelWidget::accessibilityAction, this);
}

void GraphPanelWidget::accessibilityAction(void* aUser, int aId) {
    static_cast<GraphPanelWidget*>(aUser)->onAccessibilityAction(aId);
}

void GraphPanelWidget::onAccessibilityAction(int aId) {
    if (!mFrame.editor)
        return;

    if (aId == kPanelFitItemId) {
        // The very action the double-click on the plot performs.
        mFrame.editor->fitAll();
        this->update();
        return;
    }

    if (aId == kPanelKeyMenuItemId) {
        // The route the pointer takes, at the key's own position: the menu is anchored
        // where the key is drawn, so an assistive activation puts it exactly where a
        // right-click on that key would. The selection is re-read here because a frame
        // may have passed since the item was published.
        core::TimeLineEvent selection;
        QPoint dot;
        if (!mFrame.editor->retrieveSelection(selection)
            || !panelSelectedKeyDot(mFrame.model, selection, dot))
            return; // the selection moved on: nothing to open the menu on
        MainDisplayWidget* display = appDisplay();
        if (display)
            display->showGraphKeyMenu(selection, display->mapFrom(this, dot));
        return;
    }

    // The panel's ONE control: the very action a pointer press on it performs, at the
    // same position (the box the model gave the button).
    if (aId == kPanelChannelsItemId)
        openChannelMenuAt(mFrame.model.channelsButton.topLeft());
}

} // namespace gui
