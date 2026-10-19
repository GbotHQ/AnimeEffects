#ifndef GUI_MAINDISPLAYWIDGET_H
#define GUI_MAINDISPLAYWIDGET_H

#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFramebufferObject>
#include <QScopedPointer>
#include <QTabBar>
#include <QReadWriteLock>
#include <QColor>
#include <QVariant>
#include <QtMath>
#include "util/LinkPointer.h"
#include "util/SlotId.h"
#include "gl/Global.h"
#include "gl/Root.h"
#include "gl/VertexArrayObject.h"
#include "gl/EasyTextureDrawer.h"
#include "core/Project.h"
#include "core/AbstractCursor.h"
#include "core/TimeInfo.h"
#include "core/ClippingFrame.h"
#include "core/DestinationTexturizer.h"
#include "core/FilterFrame.h"
#include "ctrl/Driver.h"
#include "ctrl/Painter.h"
#include "util/Easing.h"
#include "gui/MainViewSetting.h"
#include "gui/CanvasMover.h"
// The channel list's rows travel from the panel's model to this widget's menus
// (showGraphChannelMenu), so the widget's interface names gui::PanelChannelRow.
#include "gui/GraphPanelModel.h"
namespace gui {
class ProjectTabBar;
}
namespace gui {
class TimeLineWidget;
}
namespace gui {
class ViaPoint;
}
namespace gui {
// The channel list the header's channels button opens (showGraphChannelMenu): one
// instance per viewport, parented to it, so the widget's interface can name it without
// pulling the popup's own header into every translation unit that includes this one.
class GraphChannelPopup;
}

namespace gui {

class MainDisplayWidget: public QOpenGLWidget {
    Q_OBJECT
public:
    MainDisplayWidget(ViaPoint& aViaPoint, QWidget* aParent);
    ~MainDisplayWidget();

    void setProject(core::Project* aProject);
    void setDriver(ctrl::Driver* aDriver);
    void setProjectTabBar(ProjectTabBar* aTabBar);
    // The timeline the animation graph panel shares its ONE frame axis with (the
    // dock's Graph tab draws it; see gui/GraphPanelWidget). MainWindow wires it once
    // (the timeline widget outlives every project, unlike the driver), and from then on
    // the panel's frame window is the timeline's visible one and its selection is the
    // timeline's live set - pushed in as values, on every timeline view change (see
    // syncGraphPanelSharedState). Null detaches: the panel's editor goes back to owning
    // both its frame axis and its selection.
    void setTimeLineWidget(TimeLineWidget* aTimeLine);
    // The timeline wired in above, for callers that need the timeline itself (the
    // panel's right-click reaches the application's easing menu through it; see
    // showGraphKeyMenu).
    TimeLineWidget* timeLineWidget() const { return mTimeLine; }
    // Shows the application's own key EASING MENU for aTargets (the panel's
    // current selection, in the timeline's currency) at aWidgetPos IN THIS
    // WIDGET'S COORDINATES - the docked panel's own pixels are not this widget's, so
    // its host maps the position here before it calls (gui::GraphPanelWidget). Does
    // nothing when no timeline is wired or the target list is empty.
    void showGraphKeyMenu(const core::TimeLineEvent& aTargets, const QPoint& aWidgetPos);
    // Shows the application's own CHANNEL LIST - the animation graph panel's ONE
    // control, which the user's design moved into the DOCK HEADER's channels button -
    // for the rows the panel's model published. aWidgetPos is the ANCHOR's BOTTOM-RIGHT
    // CORNER in this widget's coordinates (like showGraphKeyMenu, the docked host maps
    // its own pixels here first): the list opens just under that point, right-aligned
    // to it, which is where the header's button sits.
    //
    // WHY IT IS HERE. The list is a real popup widget (gui::GraphChannelPopup), the
    // application's own - keyboard navigable, a native window - exactly like the key
    // menu above: the panel's host draws no popup and measures none, it hands the rows
    // over and this class builds and shows the list. One list implementation for the
    // whole application, and the panel's control is the same kind of control as the
    // timeline's own menus. (It is NOT a QMenu: the design's 22px rows, its 8px
    // swatch-to-name gap and its 10px group titles at the swatch's padding are not
    // reachable through gui::AppStyle's menu metrics.)
    //
    // It is a plain list, grouped under each layer: one row per channel, the row's
    // SWATCH is its colour key (filled while the channel is shown, hollow while it is
    // hidden) and the row's activation toggles that channel's visibility - the one
    // action per row the design asks for. The row ALSO focuses its channel when it is
    // hovered, which is the other thing the list is for (the panel draws the focused
    // channel's tangent handles and the accessible summary names it). Rows for a
    // hidden channel are listed like any other: that is how it is turned back on.
    // Nothing happens with no rows, and no list is shown.
    void showGraphChannelMenu(const QVector<PanelChannelRow>& aRows, const QPoint& aWidgetPos);
    // Applies aEasing to every key the shared SELECTION names, through the panel's own
    // editor (ctrl::GraphEditor::setSelectedKeysEasing: ONE undo step for the whole
    // selection, keys whose type carries no easing and step-held channels skipped).
    // False when there is no editor or nothing would change. The header's four quick
    // easings call this; the timeline's chip and picker keep their own route (the same
    // edit, reached from the widget that hosts the picker).
    bool applyEasingToSelection(util::Easing::Type aEasing);
    void updateRender();
    void resetCamera();
    void setViewportBackground(const QColor& aColor);
    const QColor& viewportBackground() const { return mViewportBackground; }

    QReadWriteLock& renderingLock() { return mRenderingLock; }
    const QReadWriteLock& renderingLock() const { return mRenderingLock; }


    // boostlike signals
public:
    void onVisualUpdated();
    void onToolChanged(ctrl::ToolType);
    void onFinalizeTool(ctrl::ToolType);
    void onViewSettingChanged(const MainViewSetting&);
    void onProjectAttributeUpdated();

private:
    class GLContextAccessor: public gl::ContextAccessor {
        MainDisplayWidget* mOwner;

    public:
        GLContextAccessor(MainDisplayWidget* aOwner): mOwner(aOwner) {}
        virtual void makeCurrent() { mOwner->makeCurrent(); }
        virtual void doneCurrent() { mOwner->doneCurrent(); }
    };

    // from QOpenGLWidget
    virtual void initializeGL();
    virtual void paintGL();
    virtual void paintEvent(QPaintEvent* event);
    virtual void resizeGL(int w, int h);
    virtual void mouseMoveEvent(QMouseEvent* event);
    virtual void mousePressEvent(QMouseEvent* event);
    virtual void mouseReleaseEvent(QMouseEvent* event);
    virtual void mouseDoubleClickEvent(QMouseEvent* event);
    virtual void wheelEvent(QWheelEvent* event);
    virtual void tabletEvent(QTabletEvent* event);
    virtual void keyPressEvent(QKeyEvent* event);
    virtual void keyReleaseEvent(QKeyEvent* event);
    virtual void inputMethodEvent(QInputMethodEvent* event);
    virtual QVariant inputMethodQuery(Qt::InputMethodQuery query) const;
    virtual void focusInEvent(QFocusEvent* event);
    virtual void focusOutEvent(QFocusEvent* event);

    void updateCursor();
    // Sets ctrl::CanvasMover's drag mode from the state that decides it, so "which
    // gesture is in flight" is applied in one place (see the definition). The
    // animation graph panel used to hold one of these flags while it was drawn over
    // this widget; it is the dock's own page now and never sees this camera.
    void applyCanvasMoverDragMode();
    // Hands the panel's editor the timeline's shared state: the live selection it
    // writes (ctrl::TimeLineEditor::selection()) and the frame window it shows
    // (TimeLineWidget::visibleFrameRange()), plus the two delegates that send the
    // panel's horizontal gestures back to the timeline. The panel itself is drawn in
    // the dock (gui::GraphPanelWidget), which pins its own plot width per frame; the
    // state wired here is the editor's and is shared by whoever draws it. This widget
    // keeps the wiring because it is the one that has the timeline. Called whenever either end
    // changes - the timeline being wired in, a driver being created for a project,
    // the timeline's view moving - so the panel never holds a stale window or a
    // stale selection. A no-op before either end exists.
    void syncGraphPanelSharedState();
    // Asks the DOCK's Graph page for a repaint. The panel is a QOpenGLWidget of its
    // own, so nothing this widget paints shows it: every state this widget pushes into
    // the panel's editor - the frame window (syncGraphPanelSharedState) and the keys'
    // easing (applyEasingToSelection) - has to ask the page itself. A no-op in a build
    // without the panel and in a window without the dock.
    void refreshGraphPanel();
    QSize deviceSize() const { return this->size() * mDevicePixelRatio; }

    ViaPoint& mViaPoint;
    gl::DeviceInfo mGLDeviceInfo;
    util::LinkPointer<core::Project> mProject;
    gl::Root mGLRoot;
    GLContextAccessor mGLContextAccessor;
    QScopedPointer<gl::VertexArrayObject> mDefaultVAO;
    QScopedPointer<QOpenGLFramebufferObject> mFramebuffer;
    QScopedPointer<core::ClippingFrame> mClippingFrame;
    QScopedPointer<core::DestinationTexturizer> mDestinationTexturizer;
    QScopedPointer<core::FilterFrame> mFilterFrame;
    QScopedPointer<gl::EasyTextureDrawer> mTextureDrawer;
    QScopedPointer<ctrl::PainterHandle> mPainterHandle;
    QReadWriteLock mRenderingLock;
    core::RenderInfo* mRenderInfo;
    core::AbstractCursor mAbstractCursor;
    ctrl::Driver* mDriver;
    ProjectTabBar* mProjectTabBar;
    // The ONE channel list this widget owns (showGraphChannelMenu): created on the
    // first open, parented here, hidden and re-filled for every open after that.
    GraphChannelPopup* mChannelPopup;
    // The timeline the panel shares its frame window and its key selection with
    // (see setTimeLineWidget), and the connection to its view-range signal.
    TimeLineWidget* mTimeLine;
    util::SlotId mTimeLineRangeSlot;
    QColor mViewportBackground;
    bool mUsingTablet;
    MainViewSetting mViewSetting;
    CanvasMover mCanvasMover;
    bool mMovingCanvasByTool;
    bool mMovingCanvasByKey;
    bool mMovingCanvasByMiddleMouseButton;
    double mDevicePixelRatio;
};

} // namespace gui

#endif // GUI_MAINDISPLAYWIDGET_H
