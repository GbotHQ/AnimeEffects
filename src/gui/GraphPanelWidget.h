#ifndef GUI_GRAPHPANELWIDGET_H
#define GUI_GRAPHPANELWIDGET_H

#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QPoint>
#include <QScopedPointer>
#include <QVariant>

#include "core/AbstractCursor.h"
#include "core/CameraInfo.h"
#include "gui/GraphPanelFrame.h"
#include "util/SlotId.h"

class QHideEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QTabletEvent;
class QTouchEvent;
class QWheelEvent;

namespace ctrl {
class Driver;
}
namespace theme {
class Theme;
}

namespace gui {

class GUIResources;
class ImGuiInputBridge;
class MainDisplayWidget;

// THE PANEL'S HOST: the animation graph panel drawn in the TIMELINE DOCK, as the
// Graph tab's page (design section 5). It is a QOpenGLWidget because the panel is
// drawn with Dear ImGui into a GL framebuffer, and it is a HOST and not a renderer:
// the frame it runs is gui::runPanelFrame (the io setup, the backend's NewFrame,
// gui::buildGraphPanelModel, gui::drawPanel) plus gui::publishPanelAccessibility, and
// the ImGui context, GL3 backend and font atlas are that file's (see the frame driver
// in gui/GraphPanelFrame.h). Nothing about the panel is re-implemented here. It is the
// ONLY host - the viewport overlay that used to draw the panel over the artwork is
// gone.
//
// WHAT IT DRAWS, AND WHAT ELSE IS ON ITS FLOOR. The panel draws no background of its
// own ("the panel must leave the artwork showing through") - and here there is no
// artwork to show through: this widget's floor is the DOCK's, opaque, and it is
// painted by this widget because a QOpenGLWidget is composited over the window and the
// widgets beside it do not show through it. The colour is the theme's panel/dock floor
// (theme::Colors::base, the reference's own _floor: cssVar('--base')), read per frame
// so a theme change needs no signal. NOTHING ELSE is drawn: no title (the tab names
// the page), no header row of its own (the panel's one control sits in its ruler band,
// which the model lays out for GraphPanelHost::Dock) and no scrim (an opaque floor has
// nothing to sit on, so the contrast-outline composite gui::outline offers - the pass the
// removed overlay host ran - is NOT run here).
//
// ONE FRAME AXIS, ONE CURRENT FRAME, TWO VIEWS. The editor this host draws from is the
// ONE ctrl::GraphEditor the driver owns, and it already carries the timeline's shared
// state (MainDisplayWidget::
// syncGraphPanelSharedState wired the frame window, the selection source and the two
// horizontal delegates). What a HOST must still do is pin the mapping to its own
// geometry: the frame window is mapped in pixels, computed from the plot's width, so
// paintGL() calls ctrl::GraphEditor::syncFrameWindowFor() with THIS widget's size
// before the model is built - the same call MainDisplayWidget::paintEvent makes for the
// same reason. Nothing here sets the frame window or the selection.
//
// INTERACTION. Every Qt event goes to the panel's own gui::ImGuiInputBridge first
// (ImGuiIO), and the bridge's answer is the panel's key routing rule: a rebindable
// command must not fire while gui::ImGuiInputBridge::keyboardCaptured() is true, and that
// flag is read off the ONE ImGui context, so a text field anywhere in the application
// holds the keyboard for this page too. The panel's own gestures then go to the EDITOR -
// ctrl::GraphEditor::ownsCursor/updateCursor/updateWheel, reached through the driver's
// graphEditor() - and not through the viewport at all: ctrl::Driver no longer routes a
// panel event (the viewport's cursor, wheel and keys are the tools' alone), and the
// editor has no shown/hidden flag left to gate on, because THIS PAGE is the panel
// whenever its tab is current. The two controls the panel opens (the channel list and the
// timeline's easing picker) are the application's own QMenus, which live in
// gui::MainDisplayWidget - see appDisplay() below.
//
// THE PANEL'S TWO KEYS. Ctrl+A selects every marker of the shown channels and Escape
// clears the selection - ctrl::GraphEditor::selectAllMarkers / clearSelection, the
// keyboard form of the panel's own marquee rule (see GraphEditor.h). Both are WIDGET
// bindings, not application shortcuts: the panel consumes a press only while IT is the
// focus widget (Qt::StrongFocus, so a press inside it makes it the focus widget) and
// only when it acted, and every key it does not consume still propagates to
// gui::MainWindow's key map - the one place the application's own commands fire. The
// full rule (and each condition's reason) is on panelKeyAction().
//
// ACCESSIBILITY. The panel has ONE publication for the process (gui::a11y keeps one
// installation) and this is its only host: the factory is installed for this widget in
// the CONSTRUCTOR (a11y::installFactoryOnly - the half that has to run before anything
// can ask Qt for the widget's interface), the first frame on screen hands the shared
// publisher the action callback (a repeat install for the same widget: one interface,
// one id), and every frame after that publishes the model that was just drawn. While
// the Graph tab is not on screen the empty model is published, so the tree never shows
// a panel that is not there.
//
// INTERFACE. This class is constructed by the dock (gui::TargetWidget) with the
// application's GUIResources and its parent, is handed the live ctrl::Driver when a
// project is set, and is asked to refresh() when the document changed. That is the
// whole of what the dock calls.
class GraphPanelWidget: public QOpenGLWidget {
    Q_OBJECT
public:
    GraphPanelWidget(GUIResources& aResources, QWidget* aParent = nullptr);
    ~GraphPanelWidget() override;

    // The driver that owns the panel's editor (a driver is created per project, so this
    // is called again for every project, exactly like MainDisplayWidget::setDriver).
    // The editor is asked for at every use and never cached, so a new driver is a new
    // panel with nothing stale in this host: the frame is dropped and a repaint asked
    // for, which is what makes the new project's curves appear.
    void setDriver(ctrl::Driver* aDriver);

    // The document (or the dock's own layout) changed and this page is on screen: ask
    // for a repaint. The dock calls it when it sets a project and when it switches to
    // this tab; both are moments the frame's content is already different and nothing
    // else would ask for the pixels.
    void refresh();

protected:
    void initializeGL() override;
    void paintGL() override;
    void hideEvent(QHideEvent* aEvent) override;
    void mouseMoveEvent(QMouseEvent* aEvent) override;
    void mousePressEvent(QMouseEvent* aEvent) override;
    void mouseReleaseEvent(QMouseEvent* aEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* aEvent) override;
    void wheelEvent(QWheelEvent* aEvent) override;
    void tabletEvent(QTabletEvent* aEvent) override;
    void keyPressEvent(QKeyEvent* aEvent) override;
    void keyReleaseEvent(QKeyEvent* aEvent) override;
    void inputMethodEvent(QInputMethodEvent* aEvent) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery aQuery) const override;
    void focusInEvent(QFocusEvent* aEvent) override;
    void focusOutEvent(QFocusEvent* aEvent) override;
    // DevicePixelRatioChange (the frame rasterizes at the widget's ratio) and the touch
    // events the input bridge translates. QWidget has no touchEvent() virtual in Qt 6,
    // so those arrive here.
    bool event(QEvent* aEvent) override;

private:
    // The panel's editor: the driver owns it and a new driver is a new panel, so it is
    // asked for at every use rather than cached. Null before a project is set.
    ctrl::GraphEditor* graphEditor() const;
    // The camera the editor's own geometry is pinned to. ctrl::GraphEditor derives its
    // panel rect and every hit-test from aCamera.screenSize() alone, so this is the one
    // piece of camera state the panel's gestures need - and it has to be THIS widget's
    // size, because the panel drawn here is this widget. (The world transform is never
    // read on the graph's cursor path.)
    core::CameraInfo panelCamera() const;
    // Whether the editor owns the point the cursor is at (a gesture in flight, a key or
    // a handle, a left press inside the plot, the middle button): the editor's own
    // arbitration, asked before its cursor pass exactly as ctrl::Driver asks it.
    bool panelTakesCursor() const;
    // The editor's cursor pass for the cursor's current state, when the editor owns it.
    // Repaints when the editor reports that something changed.
    void routeCursor();
    // The panel's two keyboard actions, for a key press that has reached this widget
    // unconsumed: Ctrl+A selects every marker of the shown channels
    // (ctrl::GraphEditor::selectAllMarkers) and Escape clears the selection
    // (ctrl::GraphEditor::clearSelection). Returns whether the panel ACTED, and the
    // caller consumes the event exactly then - a press that changed nothing is left to
    // propagate to the application's own key routing, as every unconsumed key here
    // always was. The conditions (this widget is the focus widget; Dear ImGui has not
    // taken the keyboard) are stated at the definition.
    bool panelKeyAction(const QKeyEvent& aEvent);

    // The panel's ONE control (the "N channels" button the model lays out in the ruler
    // band) under aPos, in this widget's logical pixels: opens the application's channel
    // list. An empty box (no room for it) is not a control. Returns true when the press
    // was the button's.
    bool pressChannelButton(const QPoint& aPos);
    // A double-click on the plot: fits both axes (the panel's own gesture), unless it
    // landed on the panel's one control. Returns true when it acted.
    bool doubleClickAt(const QPoint& aPos);
    // The key under aPos by the EDITOR's own distance rule: selects it and opens the
    // application's easing picker for the selection. Returns true when a key was hit.
    bool openKeyMenuAt(const QPoint& aPos);
    // The application's channel list for the model's rows, at aPos in this widget's
    // pixels.
    void openChannelMenuAt(const QPoint& aPos);
    // The viewport widget the application's QMenus live in (see the class comment):
    // MainDisplayWidget owns the channel list and the timeline's easing picker and is
    // the object that can map a panel position to a screen position. Asked for through
    // the window rather than held, because the panel's host interface carries no
    // reference to it (the dock constructs this widget with a GUIResources and a parent).
    MainDisplayWidget* appDisplay() const;

    // Runs the action an accessible item advertises, i.e. the one the pointer performs
    // on the same control. The thunk gui::publishPanelAccessibility is handed
    // (kPanelFitItemId, kPanelKeyMenuItemId, kPanelChannelsItemId).
    void onAccessibilityAction(int aId);
    static void accessibilityAction(void* aUser, int aId);

    // Publishes the panel's accessibility model for the state this host is in (an
    // editor: the frame; none: "not on screen").
    void publishAccessibility();

    // Drops this host's GL objects and its hold on the shared context (the context, the
    // backend and the atlas go only when no host holds them). Called from the destructor
    // and from initializeGL when this widget is given a NEW GL context.
    void shutdownHost();

    void onThemeUpdated(theme::Theme& aTheme);

    GUIResources& mResources;
    // The theme change that repaints the floor and the panel's palette.
    util::SlotId mThemeSlot;
    ctrl::Driver* mDriver;
    QScopedPointer<ImGuiInputBridge> mBridge;
    // The frame this host drew last: its model (which the hits below are tested
    // against), the editor, the palette and this host's own clock. Its topInset stays 0:
    // nothing floats over the dock's page.
    PanelFrameState mFrame;
    // The Qt mouse stream in the editor's own currency (ctrl::GraphEditor reads
    // screenPoint()), fed from this page's events and pinned to panelCamera().
    core::AbstractCursor mCursor;
    bool mReady;
};

} // namespace gui

#endif // GUI_GRAPHPANELWIDGET_H
