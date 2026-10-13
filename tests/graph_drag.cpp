// Contract test for the animation graph's edit path: ONE handle drag IS ONE
// undo step, and the drag edits the key's OWN easing param where the drag points.
//
// The graph panel (ctrl::GraphEditor) is the only place a user reshapes a
// Custom easing's tangent handle, and that edit carries two contracts that are
// invisible from inside the implementation:
//
//   * it is ONE command. The drag holds a cmnd::Gesture over a single
//     cmnd::ModifiableAssign<util::Easing::Param>: the press only records the
//     hit, the FIRST move commits, and every later move rewrites that same
//     command. So one undo must restore the pre-drag param exactly, and a
//     second undo must find nothing left - a drag that pushed one command per
//     move cannot satisfy that.
//   * it edits the key's own param at the pixel it points to. The dragged pixel
//     maps back through the panel's own scale into the normalized (segment
//     frame, segment value) space graph::Curve stores handles in, and only the
//     GRABBED handle moves: the other one, and the keys' values, stay put.
//
// Everything is observed through the public API only - the editor's own live
// scale for the pixels, the document for the values, the project's command stack
// for the history - so the test cannot drift from the geometry the hit-test
// uses, and no formula is re-stated here that the implementation could then
// change underneath it.
//
// The panel is FULLSCREEN, so it does not own the cursor by rectangle: it owns
// it only when the pointer is on something it can act on (a key or a tangent
// handle), or while a drag is in flight. Both halves are pinned below: a press
// on empty space inside the viewport is nobody's edit, and an in-flight drag
// keeps the cursor even at a point with no item under it (which is also what
// proves the grab below is a real one rather than a press that missed).
//
// The value axis is pinned here too, because the drag pixels would otherwise
// hide the defect: the window has to be FITTED to the shown curves on the paths
// the application uses - the panel is toggled on and the layer selected with no
// cursor event ever reaching the panel - or a channel whose values are 0..90
// draws its whole curve and its keys outside the plot, an empty panel. The fit
// is keyed to the SET of shown (layer, channel) pairs (and narrowed to the
// STICKY-FOCUSED channel when there is one), so the other half is pinned as
// well: a manual window (the wheel's own path) survives any number of cursor
// passes, and only a change of shown curves refits. (The paint path is no longer
// a writer here at all: GraphEditor::renderQt was deleted with the ImGui
// cutover and the panel is drawn from gui::GraphPanelModel, whose own gate pins
// that building it writes nothing back.)
//
// The panel's three KEY EDITS are pinned the same way, at the end of this file
// (sections 16-18), on one layer of their own:
//
//   * INSERT: a press on a curve's BAND (a point on no key and no handle, within
//     GraphEditor::kCurveBandRadius of the value the curve evaluates at the
//     point's frame) writes the key the curve already passes through - the value
//     is the curve's own sample there, so no sampled value moves AT THAT FRAME -
//     and the band is a point the panel OWNS, which is what lets the press reach
//     the editor at all (the Driver routes by ownsCursor). A press on a key, a
//     point off the band and a point outside the plot insert nothing and push
//     nothing.
//   * MOVE: a press on a KEY arms a move and a horizontal drag changes its FRAME
//     through the repository's own ctrl::TimeLineUtil::MoveFrameOfKey; the value
//     and the easing never move, a destination another key owns is refused (and
//     leaves no entry of its own), and the whole gesture is ONE undo step.
//   * REMOVE: acts on the key the panel's selection names (the key a press last
//     hit) through TimeLine::createRemover; ONE undo step, and the undo restores
//     the SAME key object with its value and easing. With no key selected, or a
//     selection naming a frame with no key, nothing is pushed.
//   * EASING (section 19): setSelectedKeysEasing() changes the easing TYPE of
//     every key the selection names, on the timeline's own route - the command,
//     the notifier and the event the timeline's "Change key(s) easing to" action
//     uses - as ONE undo step. An empty or stale selection, a type whose easing
//     playback never reads (the step-held image offset) and a request for the
//     type the key already has all push NOTHING.
//
// Every one of the three goes through the same TimeLineUtil notifier +
// ScopedMacro route the handle drag uses, so undo/redo and the timeline's event
// currency are the same for all of them - which is what the stack probes below
// measure: after each gesture, the FIRST undo must be that gesture.
//
// THE PANEL'S SWITCH is asserted in BOTH directions rather than assumed - the show
// below, the hide at the end of main. setVisible() is the one state the rest of
// the application reads before it lets the panel see anything (ctrl::Driver routes
// a cursor or a wheel event to the editor only while isVisible() answers true), so
// a switch that quietly did not take would leave the panel dead in the running
// application while every assertion in this file - which drives the editor
// directly, and refits the axis from setTargets() as well - still passed. The gate
// belongs in this headless suite and not only in the application-level probe.
//
// TARGET LIFETIME, which every section that declares its own layer must honour:
// ctrl::GraphEditor holds the target list as RAW pointers and never owns them, so
// a node handed to setTargets() must OUTLIVE every use the editor makes of it -
// i.e. the section must either keep the node in a scope that outlives those uses,
// or drop it with setTargets({}) before its brace closes. The application does
// the same thing (the object tree's deletion path re-emits the selection change
// and the window re-calls setTargets before any notification or deferred repaint
// runs). A target left in mTargets past its owner's death is a dangling pointer
// the next buildEntries() walks: a SIGSEGV, not a failed assertion, and one that
// hides every section after it. Section 15b pins the safe direction - NO targets -
// across the fits and a cursor pass.
//
// Headless: QApplication + offscreen, no GL context, no window, no .anie
// fixture. The two keys are pushed through the production
// core::TimeLine::createPusher path onto a test-owned stack, so the project's
// own stack holds nothing but the graph's edits and every undo probe below is
// about the drag.
#include <QApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "XCAssert.h"
#include "cmnd/Stack.h"
#include "core/AbstractCursor.h"
#include "core/CameraInfo.h"
#include "core/Frame.h"
#include "core/ObjectNode.h"
#include "core/PoseKey.h"
#include "core/Project.h"
#include "core/RotateKey.h"
#include "core/TimeKey.h"
#include "core/TimeKeyType.h"
#include "core/TimeLine.h"
#include "ctrl/GraphEditor.h"
#include "ctrl/UILogger.h"
#include "ctrl/graph/graph_Channel.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Geometry.h"
#include "ctrl/graph/graph_Scaler.h"
#include "util/Easing.h"

// core::Project.cpp references the GUI AudioPlaybackWidget::deserialize (only to
// load a .aemus sidecar, which this test never has). Stub it so the link
// succeeds -- same as creation_select_keys / graph_curve.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

// ctrl's timeline editor references two GUI symbols that only matter on paths
// this test does not drive. Stub them (same pattern) instead of linking the
// whole gui library.
#include "gui/ObjectTreeWidget.h"
bool gui::obj::Item::isTopNode() const { return false; }
std::pair<core::TimeKey*, core::LayerMesh*> gui::ObjectTreeWidget::getAreaMeshImpl(core::ObjectNode&, int) {
    return { nullptr, nullptr };
}

namespace {

class NullAnimator: public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

// The graph editor's only outbound channel besides the command stack: an edit it
// cannot make says why. This test never provokes one, so the logger's whole job
// is to exist without a widget.
class NullLogger: public ctrl::UILogger {
public:
    void pushLog(const QString&, ctrl::UILogType) override {}
};

class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;

#define CHECK(cond)                                                                                    \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                                             \
            return 1;                                                                                  \
        }                                                                                              \
    } while (false)

using ctrl::graph::Channel;
using ctrl::graph::Curve;
using ctrl::graph::Scaler;

// A segment handle's pixel, mapped exactly the way the editor's own hit-test
// maps it: the NORMALIZED handle through the panel's two axes (x along the
// segment's frame span, y across its value span). The editor reads the same
// numbers to place the dot it draws, so a grab point built here is the dot.
QPoint handlePixel(const Scaler& aScale, const Curve::Segment& aSegment, int aHandle) {
    const QPointF normalized = Curve::handle(aSegment, aHandle);
    const double frameSpan = static_cast<double>(aSegment.to->frame() - aSegment.from->frame());
    const double valueSpan = static_cast<double>(aSegment.toValue - aSegment.fromValue);

    const double frame = static_cast<double>(aSegment.from->frame()) + normalized.x() * frameSpan;
    const float value = aSegment.fromValue + static_cast<float>(normalized.y() * valueSpan);
    return QPoint(
        aScale.pixelX(core::Frame::fromDecimal(static_cast<float>(frame))), aScale.pixelY(value)
    );
}

// The same pixel asked of the DOCUMENT as it stands now: an edit rewrites the
// param, so "where is this handle" has to be re-read before every grab instead
// of reusing a press-time pixel.
QPoint currentHandlePixel(ctrl::GraphEditor& aEditor, core::ObjectNode& aNode, int aHandle) {
    const Curve curve(*aNode.timeLine(), Channel(core::TimeKeyType_Rotate, 0));
    return handlePixel(aEditor.scale(), curve.segment(0), aHandle);
}

// One synthetic cursor event, delivered the way ctrl::Driver delivers it: the
// cursor consumes the QMouseEvent, then the editor is handed that cursor. The
// return is updateCursor's own answer - true when the graph wants a repaint,
// which is the same as "the panel took this event".
bool sendPress(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos
) {
    QMouseEvent event(
        QEvent::MouseButtonPress, aPos, aPos, aPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
    );
    if (!aCursor.setMousePress(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendMove(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos
) {
    QMouseEvent event(QEvent::MouseMove, aPos, aPos, aPos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    if (!aCursor.setMouseMove(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendRelease(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos
) {
    QMouseEvent event(QEvent::MouseButtonRelease, aPos, aPos, aPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    if (!aCursor.setMouseRelease(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

namespace core {
// Minimal ObjectNode subclass owning a real TimeLine (ObjectNode::timeLine()
// is virtual and null by default). No meshes, no resources -- just key maps.
class TestNode: public ObjectNode {
    TimeLine mTimeLine;

public:
    ObjectType type() const override { return ObjectType_Layer; }
    void setName(const QString&) override {}
    const QString& name() const override {
        static QString s;
        return s;
    }
    float initialDepth() const override { return 0; }
    void setVisibility(bool) override {}
    bool isVisible() const override { return true; }
    void setSlimDown(bool) override {}
    bool isSlimmedDown() const override { return false; }
    bool canHoldChild() const override { return false; }
    void setInitialRect(const QRect&) override {}
    QRect initialRect() const override { return QRect(); }
    bool serialize(Serializer&) const override { return true; }
    bool deserialize(Deserializer&) override { return true; }
    TimeLine* timeLine() override { return &mTimeLine; }
    const TimeLine* timeLine() const override { return &mTimeLine; }
};
} // namespace core

namespace {

// The easing the first key is authored with: an unambiguous Custom cubic bezier,
// so "did the drag move handle 0" is a two-number question.
util::Easing::Param customEasing() {
    util::Easing::Param param;
    param.type = util::Easing::Type_Custom;
    param.cubicBezier.x1 = 0.4f;
    param.cubicBezier.y1 = 0.1f;
    param.cubicBezier.x2 = 0.6f;
    param.cubicBezier.y2 = 0.9f;
    return param;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    NullAnimator animator;
    core::Project project("graph_drag.anie", animator, nullptr);

    // The keys go onto a stack this test owns. The project's stack must hold
    // nothing but the graph's edits, or the undo probes below would be undoing
    // the setup rather than the drag.
    cmnd::Stack setup;

    core::TestNode node;

    const util::Easing::Param kAuthored = customEasing();
    core::RotateKey* key0 = new core::RotateKey();
    key0->setRotate(0.0f);
    key0->data().easing() = kAuthored;
    setup.push(node.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, key0));

    core::RotateKey* key1 = new core::RotateKey();
    key1->setRotate(90.0f);
    setup.push(node.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, key1));

    // The document the drag is about: two keys, one segment, a Custom curve on
    // the first key, and a non-zero value span between them. Every one of these
    // is a precondition of the grab, so each is asserted rather than assumed.
    CHECK(node.timeLine()->map(core::TimeKeyType_Rotate).size() == 2);
    CHECK(key0->frame() == 0);
    CHECK(key1->frame() == 24);
    CHECK(key0->rotate() == 0.0f);
    CHECK(key1->rotate() == 90.0f);
    CHECK(key0->data().easing() == kAuthored);
    CHECK(key0->data().easing().type == util::Easing::Type_Custom);

    // The second key's own param: the drag must never write it, so it is read
    // once and compared after every phase.
    const util::Easing::Param kOtherKeyEasing = key1->data().easing();

    core::CameraInfo camera;
    camera.setScreenSize(QSize(800, 600));
    const QRect panel = ctrl::GraphEditor::panelRect(camera.screenSize());

    NullLogger logger;
    ctrl::GraphEditor editor(project, logger);
    // The POST-CONDITION of the show, asserted where the panel is first driven:
    // a setVisible() that did not take would leave the editor hidden for the whole
    // run - every section below drives it directly, so none of them would notice -
    // and the panel would be dead in the running application, where the Driver's
    // routing is a read of exactly this state.
    editor.setVisible(true);
    CHECK(editor.isVisible());
    editor.setTargets({ &node });

    // --- 0: the value axis is FITTED to the shown curves, on the paths the
    // application really uses: the panel is toggled on and the layer selected,
    // and no cursor event ever reaches the panel. Without the fit the window is
    // the scaler's seed 0..1, so a channel whose values are 0..90 (a rotate
    // curve) maps its whole curve AND its keys outside plotRect(), where the
    // renderer's geometric clip removes them: the panel draws an empty grid, and
    // neither this cursor-driven test nor the pixel test would notice, because
    // both set a window by hand.
    {
        // The precondition of every assertion in this section, asserted here so
        // a run that cannot observe the fit fails loudly instead of passing.
        const Curve shown(*node.timeLine(), Channel(core::TimeKeyType_Rotate, 0));
        CHECK(shown.keys().size() == 2);
        CHECK(shown.keys()[0]->frame() == 0);
        CHECK(shown.keys()[1]->frame() == 24);
        CHECK(shown.valueOf(*shown.keys()[0]) == 0.0f);
        CHECK(shown.valueOf(*shown.keys()[1]) == 90.0f);

        const Scaler& fitted = editor.scale();
        CHECK(fitted.valueMin() <= 0.0f); // the window really CONTAINS the data...
        CHECK(fitted.valueMax() >= 90.0f);
        // ...and is not the scaler's seed window (0..1), which is what "no fit at
        // all" leaves behind. The two bounds are checked as a pair because the
        // seed is the pair, and a fit of a degenerate channel widens around the
        // value rather than sitting on a bound.
        CHECK(fitted.valueMin() != 0.0f || fitted.valueMax() != 1.0f);
    }

    // --- 0b: the window is the USER's. A manual setValueWindow - and the wheel's
    // own value zoom above all - is not undone by a cursor pass: a cursor pass
    // changes no curve, so it may not refit. This is the other half of the policy,
    // and the reason the fit is keyed to the SET of shown (layer, channel) pairs
    // rather than to the values.
    //
    // The PAINT is no longer a path in this class at all: GraphEditor::renderQt
    // was deleted with the ImGui cutover, and the panel is drawn from
    // gui::GraphPanelModel, whose own gate asserts that building it writes
    // nothing back. So the survival checks below read the window after a cursor
    // pass, which is the only writer left to guard against.
    editor.scale().setValueWindow(-5.0f, 5.0f);
    CHECK(editor.scale().valueMin() == -5.0f);
    CHECK(editor.scale().valueMax() == 5.0f);

    const QPoint kProbe(5, 5);
    // The fullscreen panel covers the whole viewport, so this point is INSIDE
    // its rect - and still not the panel's: with hit-only ownership a point that
    // is on no key and no handle is passed through to the viewport tools.
    CHECK(panel.contains(kProbe));
    core::AbstractCursor probe;
    sendMove(editor, camera, probe, kProbe);
    CHECK(!editor.ownsCursor(camera, probe));
    CHECK(sendPress(editor, camera, probe, kProbe)); // delivered, though the panel does not own it
    CHECK(!sendRelease(editor, camera, probe, kProbe)); // and it closes no drag
    CHECK(editor.scale().valueMin() == -5.0f);
    CHECK(editor.scale().valueMax() == 5.0f);

    // The wheel's path too, since that is where a user really moves the window:
    // one notch of VALUE zoom over the panel, then the same cursor pass.
    // Identity is the honest assertion here - nothing is supposed to write the
    // window, so nothing may round it either.
    const QPoint kWheelPoint = panel.center();
    CHECK(editor.updateWheel(camera, kWheelPoint, 120, true)); // one notch, taken by the panel
    const float wheeledMin = editor.scale().valueMin();
    const float wheeledMax = editor.scale().valueMax();
    CHECK(wheeledMax > wheeledMin); // a notch that moved nothing would prove nothing
    CHECK(wheeledMin != -5.0f || wheeledMax != 5.0f);
    CHECK(sendPress(editor, camera, probe, kProbe));
    CHECK(!sendRelease(editor, camera, probe, kProbe));
    CHECK(editor.scale().valueMin() == wheeledMin);
    CHECK(editor.scale().valueMax() == wheeledMax);

    // --- 0c: a change of the shown curves DOES refit - that is what the fit is
    // for. Dropping the selection and taking the layer back is a new basis even
    // though it is the same layer, because the resulting set of drawn curves is
    // what the window is keyed to.
    editor.setTargets({});
    editor.setTargets({ &node });
    CHECK(editor.scale().valueMin() <= 0.0f); // fitted to the data again
    CHECK(editor.scale().valueMax() >= 90.0f);

    // Every change the graph announces, so "a press that commits nothing" is
    // observable from outside the editor too and not only through the stack.
    int changeEvents = 0;
    project.onTimeLineModified.connect([&changeEvents](core::TimeLineEvent& aEvent, bool aUndo) {
        if (!aUndo && aEvent.type() == core::TimeLineEvent::Type_ChangeKeyValue) {
            ++changeEvents;
        }
    });

    cmnd::Stack& stack = project.commandStack();

    // --- 1: a point on nothing is not the panel's, and a press+release there
    // edits nothing. Sending it is also what makes the editor sync its own
    // geometry -- the panel rect is written on the cursor path -- so every pixel
    // asserted from here on is the one the hit-test will use.
    core::AbstractCursor outside;
    CHECK(!editor.ownsCursor(camera, outside)); // the origin is on no item, so the fullscreen panel does not own it

    const QPoint kOutside(5, 5);
    CHECK(sendPress(editor, camera, outside, kOutside)); // the press is delivered; the panel just does not own it
    CHECK(!editor.ownsCursor(camera, outside));
    CHECK(!sendRelease(editor, camera, outside, kOutside)); // the release closes no drag: nothing to repaint
    CHECK(!editor.ownsCursor(camera, outside));

    CHECK(key0->data().easing() == kAuthored); // unchanged by the outside press+release
    CHECK(key1->data().easing() == kOtherKeyEasing);
    CHECK(changeEvents == 0); // and it announced nothing

    bool emptyUndo = true;
    stack.undo(&emptyUndo);
    CHECK(!emptyUndo); // nothing at all has been pushed yet

    // --- 2: a usable value window, set through the public accessor, so the two
    // keys and the handle land on distinct rows the drag can move between.
    editor.scale().setValueWindow(-10.0f, 100.0f);
    const Scaler& scale = editor.scale();
    CHECK(scale.valueMin() == -10.0f);
    CHECK(scale.valueMax() == 100.0f);
    CHECK(scale.pixelY(0.0f) != scale.pixelY(90.0f)); // the span really is on screen

    // --- 3: the grab point, computed from the editor's OWN live scale and the
    // segment the document holds now.
    const Curve curve(*node.timeLine(), Channel(core::TimeKeyType_Rotate, 0));
    CHECK(curve.segmentCount() == 1);
    const Curve::Segment seg = curve.segment(0);
    CHECK(seg.from == key0);
    CHECK(seg.to == key1);
    CHECK(seg.fromValue == 0.0f);
    CHECK(seg.toValue == 90.0f);
    CHECK(seg.easing == kAuthored); // the segment is shaped by its `from` key's param
    CHECK(curve.isCustom(seg));

    const QPointF h0 = Curve::handle(seg, 0);
    CHECK(std::fabs(h0.x() - 0.4) < 1e-6); // the authored handle, before anything touches it
    CHECK(std::fabs(h0.y() - 0.1) < 1e-6);

    const QPoint grab = handlePixel(scale, seg, 0);
    CHECK(panel.contains(grab)); // inside the viewport, where the panel covers

    // The grab is unambiguous: the segment's OTHER handle is far outside the
    // handle grab radius, so the press cannot be taken for the wrong dot.
    const QPoint otherHandle = handlePixel(scale, seg, 1);
    CHECK(std::hypot(otherHandle.x() - grab.x(), otherHandle.y() - grab.y())
        > 2.0 * ctrl::graph::kHandleHitRadius);

    // The OTHER half of the show, on the point the drag below rides on: with the
    // panel shown, a cursor pass on this handle is the panel's own. This is the
    // routing the application makes before any press reaches this class (the
    // Driver asks ownsCursor, never the editor), so the press in section 4 can
    // only be a grab while the panel both exists and takes the pointer - and the
    // pass is harmless to the window the press is interpreted against, since it
    // changes no curve and syncValueAxis therefore refits nothing.
    core::AbstractCursor handleProbe;
    sendMove(editor, camera, handleProbe, grab);
    CHECK(editor.ownsCursor(camera, handleProbe));

    // The value window the press is interpreted against, SNAPSHOTTED. This press
    // refocuses the channel (nothing is focused yet), and a focus's fit is
    // PENDING by contract: it lands when the gesture ends, so the drag itself -
    // all three moves, and the mapping section 5 checks the result against - runs
    // on the window the user saw at the press, not on the post-fit one the live
    // scale shows afterwards.
    const Scaler dragScale = editor.scale();

    // --- 4: the drag: press on the handle, three moves, release. The panel owns
    // the cursor at every step, and a point with NO item under it while the
    // button is down proves a drag is really in flight (ownsCursor's documented
    // exception; the fullscreen panel does not own off-item points otherwise).
    // That is what makes the sections below assertions about a drag rather than
    // about a press that missed the handle.
    core::AbstractCursor drag;
    CHECK(!editor.ownsCursor(camera, outside)); // no drag in flight yet

    CHECK(sendPress(editor, camera, drag, grab));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside)); // the drag holds the cursor even out there
    CHECK(key0->data().easing() == kAuthored); // a press alone commits nothing
    CHECK(changeEvents == 0);

    const QPoint step1 = grab + QPoint(4, -4);
    const QPoint step2 = grab + QPoint(8, -8);
    const QPoint step3 = grab + QPoint(12, -12); // right and up: a higher value

    CHECK(sendMove(editor, camera, drag, step1));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));
    CHECK(key0->data().easing() != kAuthored); // the FIRST move is what commits

    CHECK(sendMove(editor, camera, drag, step2));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));

    CHECK(sendMove(editor, camera, drag, step3));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));

    CHECK(key1->data().easing() == kOtherKeyEasing); // the other key's param is untouched
    CHECK(sendRelease(editor, camera, drag, step3));
    CHECK(!editor.ownsCursor(camera, outside)); // the drag is over: the outside point is nobody's again
    CHECK(changeEvents > 0); // the drag told the rest of the application the value changed

    // --- 5: the edit landed where the drag pointed. The released pixel maps
    // back through the SAME scale into the normalized space the handle lives in,
    // and only the GRABBED handle moved.
    const util::Easing::Param dragged = key0->data().easing();
    CHECK(dragged != kAuthored);
    CHECK(dragged.type == kAuthored.type); // a handle drag shapes the curve; it never writes the type
    CHECK(dragged.range == kAuthored.range);
    CHECK(dragged.weight == kAuthored.weight);
    CHECK(dragged.cubicBezier.x2 == kAuthored.cubicBezier.x2); // the handle that was NOT grabbed
    CHECK(dragged.cubicBezier.y2 == kAuthored.cubicBezier.y2);

    CHECK(key0->rotate() == 0.0f); // the drag edits the easing, not the key's value
    CHECK(key1->rotate() == 90.0f);
    CHECK(key1->data().easing() == kOtherKeyEasing);

    const double frameSpan = static_cast<double>(seg.to->frame() - seg.from->frame());
    const double valueSpan = static_cast<double>(seg.toValue - seg.fromValue);
    // Through the window the gesture RAN on (dragScale), which is the scale whose
    // pixels the moves were interpreted against. The post-release live scale is a
    // different window: the focus's pending fit landed on the release.
    const double nx = (dragScale.frameAtPixel(static_cast<double>(step3.x()))
                          - static_cast<double>(seg.from->frame()))
        / frameSpan;
    const double ny =
        (dragScale.valueAtPixel(static_cast<double>(step3.y())) - static_cast<double>(seg.fromValue)) / valueSpan;
    CHECK(nx > 0.0 && nx < 1.0); // the drag stayed inside the segment's time span: no clamp is in play
    CHECK(ny > 0.0 && ny < 1.0);
    CHECK(std::fabs(dragged.cubicBezier.x1 - nx) < 0.02);
    CHECK(std::fabs(dragged.cubicBezier.y1 - ny) < 0.02);
    CHECK(dragged.cubicBezier.x1 > kAuthored.cubicBezier.x1); // it moved the way it was dragged: right
    CHECK(dragged.cubicBezier.y1 > kAuthored.cubicBezier.y1); // and up, toward the next key's value

    // --- 6: exactly ONE command. One undo reaches the ORIGINAL param, and a
    // second undo finds nothing left: the three moves coalesced into the single
    // command the first move pushed, which is the whole point of a Gesture.
    bool undone = false;
    const QString dragName = stack.undo(&undone);
    CHECK(undone);
    CHECK(!dragName.isEmpty());
    CHECK(key0->data().easing() == kAuthored);

    bool undoneAgain = true;
    stack.undo(&undoneAgain);
    CHECK(!undoneAgain); // the drag was the whole history

    bool redone = false;
    const QString redoName = stack.redo(&redone);
    CHECK(redone);
    CHECK(redoName == dragName); // one command, undone and redone
    CHECK(key0->data().easing() == dragged);

    // --- 7: a press+release with NO move pushes nothing. The handle is grabbed
    // again (the drag holds the cursor even off the items while the button is
    // down), it edits nothing, and the history still ends at the one drag.
    //
    // The re-grab point is the handle's pixel under the LIVE scale: the focus's
    // pending fit landed when the section-4 drag released, so the window moved
    // and with it the handle's pixel. `still` is therefore the same handle at a
    // different pixel, not the released pixel - the released PIXEL identity is
    // not a contract here (the WINDOW is allowed to move at a release; the
    // PARAM is not, and `key0`'s easing is still `dragged`).
    const QPoint still = currentHandlePixel(editor, node, 0);
    CHECK(panel.contains(still)); // inside the viewport, where the panel covers

    const int eventsBefore = changeEvents;
    CHECK(sendPress(editor, camera, drag, still));
    CHECK(editor.ownsCursor(camera, outside)); // grabbed: a drag is in flight
    CHECK(key0->data().easing() == dragged);   // which has edited nothing
    CHECK(sendRelease(editor, camera, drag, still));
    CHECK(!editor.ownsCursor(camera, outside)); // closed again
    CHECK(key0->data().easing() == dragged);    // the press+release changed nothing
    CHECK(changeEvents == eventsBefore);        // and announced nothing

    bool undoneStill = false;
    stack.undo(&undoneStill);
    CHECK(undoneStill);
    CHECK(key0->data().easing() == kAuthored); // the history is still exactly the one drag
    bool undoneNone = true;
    stack.undo(&undoneNone);
    CHECK(!undoneNone);
    bool redoneStill = false;
    stack.redo(&redoneStill);
    CHECK(redoneStill);
    CHECK(key0->data().easing() == dragged);

    // --- 8: a second drag after an undo pushes exactly ONE MORE command. A press
    // closes whatever edit was open and the undo dropped the stack's modifiable
    // link, so the new drag opens a fresh command instead of rewriting the old
    // one - and its own moves still coalesce into that one command.
    bool undoneSecond = false;
    stack.undo(&undoneSecond);
    CHECK(undoneSecond);
    CHECK(key0->data().easing() == kAuthored);

    const QPoint grab2 = currentHandlePixel(editor, node, 0);
    CHECK(panel.contains(grab2));
    CHECK(sendPress(editor, camera, drag, grab2));
    CHECK(editor.ownsCursor(camera, outside)); // grabbed again

    const QPoint last2 = grab2 + QPoint(-6, 8); // left and down the value axis: both components fall
    CHECK(sendMove(editor, camera, drag, grab2 + QPoint(-2, 3)));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(sendMove(editor, camera, drag, grab2 + QPoint(-4, 6)));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(sendMove(editor, camera, drag, last2));
    CHECK(sendRelease(editor, camera, drag, last2));
    CHECK(!editor.ownsCursor(camera, outside));

    const util::Easing::Param second = key0->data().easing();
    CHECK(second != kAuthored);
    CHECK(second != dragged);                                    // a fresh edit, not a replay
    CHECK(second.cubicBezier.x1 < kAuthored.cubicBezier.x1);     // dragged left
    CHECK(second.cubicBezier.y1 < kAuthored.cubicBezier.y1);     // and down, below its own key's value

    const double nx2 = (scale.frameAtPixel(static_cast<double>(last2.x()))
                           - static_cast<double>(seg.from->frame()))
        / frameSpan;
    const double ny2 =
        (scale.valueAtPixel(static_cast<double>(last2.y())) - static_cast<double>(seg.fromValue)) / valueSpan;
    CHECK(nx2 > 0.0 && nx2 < 1.0);
    CHECK(ny2 > 0.0 && ny2 < 1.0);
    CHECK(std::fabs(second.cubicBezier.x1 - nx2) < 0.02);
    CHECK(std::fabs(second.cubicBezier.y1 - ny2) < 0.02);

    bool undoneOne = false;
    const QString secondName = stack.undo(&undoneOne);
    CHECK(undoneOne);
    CHECK(!secondName.isEmpty());
    CHECK(key0->data().easing() == kAuthored); // ONE undo restores the pre-drag value
    bool undoneNothing = true;
    stack.undo(&undoneNothing);
    CHECK(!undoneNothing); // and nothing sits behind it: the second drag was one command
    bool redoneSecond = false;
    stack.redo(&redoneSecond);
    CHECK(redoneSecond);
    CHECK(key0->data().easing() == second);

    // --- 9: HIT-ONLY ownership. The panel is fullscreen, so owning "inside the
    // rect" would be owning everything. It owns a point only when the hit-test
    // finds an item there, or while a drag is in flight (that half is pinned in
    // section 4, where a point with no item under it stays the graph's because
    // the button is down).
    {
        // A point inside the viewport (and so inside the fullscreen panel) that
        // is on no key and no handle: not the graph's.
        core::AbstractCursor hover;
        const QPoint kEmptyInView(700, 550);
        CHECK(panel.contains(kEmptyInView));
        sendMove(editor, camera, hover, kEmptyInView);
        CHECK(!editor.ownsCursor(camera, hover));

        // A key's OWN rect - the rect the renderer draws and the hit-test tests -
        // is the graph's, though: that is exactly the pointer it can act on.
        const QRect key1Rect = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        sendMove(editor, camera, hover, key1Rect.center());
        CHECK(editor.ownsCursor(camera, hover));
    }

    // --- 10: the press routing, and the fit's TIMING. A press that HITS a key
    // both selects it and focuses its channel; a press on EMPTY space does
    // NEITHER and is passed through. Flipping the `hitCurve && validItem` routing
    // would break one half of this either way.
    //
    // The timing half is the invariant a defect would hide: a press focuses (and
    // arms a move) BEFORE the gesture is interpreted, so the focus's fit is
    // PENDING and must not move the geometry under that press - the key's own
    // rect has to be byte-identical immediately before the press and immediately
    // before its release.
    {
        // Start from "no focus" so the press below is a REAL focus change: the
        // case where an immediate fit would move the key the press just selected.
        editor.clearFocus();
        core::AbstractCursor settle;
        sendMove(editor, camera, settle, QPoint(700, 550)); // pays the clearFocus fit
        CHECK(!editor.hasFocus());

        // A hand-set window, so the pending fit is OBSERVABLE: if the press
        // refitted, the release would be interpreted against the data window
        // instead of this one.
        editor.scale().setValueWindow(-1000.0f, 1000.0f);

        // The key's rect as the user sees it, immediately before the press.
        const QRect beforePress = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        core::AbstractCursor hitCur;
        sendMove(editor, camera, hitCur, beforePress.center());
        CHECK(editor.ownsCursor(camera, hitCur)); // on the key: the graph's

        // The press selects the key, focuses its channel, and ARMS the move.
        CHECK(sendPress(editor, camera, hitCur, beforePress.center()));

        core::TimeLineEvent sel;
        CHECK(editor.retrieveSelection(sel));
        CHECK(sel.targets().size() == 1);
        CHECK(sel.targets()[0].node == &node);
        CHECK(sel.targets()[0].pos.type() == core::TimeKeyType_Rotate);
        CHECK(sel.targets()[0].pos.index() == 24); // the key that was hit
        CHECK(editor.hasFocus());
        CHECK(editor.focusedNode() == &node);
        CHECK(editor.focusedChannel() == Channel(core::TimeKeyType_Rotate, 0));

        // The invariant the timing rule buys: the geometry the user pressed on is
        // UNCHANGED when the release arrives, and the panel still owns that point.
        const QRect beforeRelease = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        CHECK(beforeRelease == beforePress);          // the key did NOT slide under the pointer
        CHECK(editor.ownsCursor(camera, hitCur));     // so the release is still the panel's
        CHECK(sendRelease(editor, camera, hitCur, beforeRelease.center())); // closes the armed move

        // The gesture is over, so the pending fit has landed: the window answers
        // the channel the press focused, not the window that was hand-set.
        CHECK(editor.scale().valueMin() > -1000.0f);
        CHECK(editor.scale().valueMin() <= 0.0f);
        CHECK(editor.scale().valueMax() >= 90.0f);
        CHECK(editor.scale().valueMax() < 1000.0f);

        // Empty space: the cursor is passed through, the press is delivered but
        // inserts nothing (no curve band out there) and arms nothing - so the
        // release closes no drag and is NOT consumed - and neither the selection
        // nor the (sticky) focus changes.
        const QPoint kEmpty(700, 550);
        core::AbstractCursor emptyCur;
        sendMove(editor, camera, emptyCur, kEmpty);
        CHECK(panel.contains(kEmpty));                 // inside the fullscreen panel...
        CHECK(!editor.ownsCursor(camera, emptyCur));   // ...but on nothing: passed through
        CHECK(sendPress(editor, camera, emptyCur, kEmpty));
        CHECK(!sendRelease(editor, camera, emptyCur, kEmpty)); // armed nothing: closes no drag

        core::TimeLineEvent selAfter;
        CHECK(editor.retrieveSelection(selAfter));
        CHECK(selAfter.targets().size() == 1);
        CHECK(selAfter.targets()[0].pos.index() == 24); // the selection is untouched
        CHECK(editor.hasFocus());                       // and so is the focus
        CHECK(editor.focusedNode() == &node);
        CHECK(editor.focusedChannel() == Channel(core::TimeKeyType_Rotate, 0));
    }

    // --- 11: the key hit-radius BOUNDARY. The hit-test compares against
    // graph::kKeyHitRadius with `<=`, so a press EXACTLY that far from a key's
    // mapped centre still hits - changing it to `<` would make the outermost ring
    // dead. The centre is computed exactly the way the hit-test computes it
    // (QRectF of the rendered rect), and the probe sits on the axis through it, so
    // the distance is exactly the radius with no half-pixel slack.
    {
        const QRect key1Rect = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        const QPoint key1Center = QRectF(key1Rect).center().toPoint();
        const QPoint onBoundary(key1Center.x() + ctrl::graph::kKeyHitRadius, key1Center.y());

        core::AbstractCursor boundaryCur;
        sendMove(editor, camera, boundaryCur, onBoundary);
        CHECK(editor.ownsCursor(camera, boundaryCur)); // exactly on the radius: still a hit

        core::TimeLineEvent sel;
        CHECK(sendPress(editor, camera, boundaryCur, onBoundary));
        CHECK(editor.retrieveSelection(sel));
        CHECK(sel.targets().size() == 1);
        CHECK(sel.targets()[0].pos.index() == 24); // it selected the key it hit
        CHECK(sendRelease(editor, camera, boundaryCur, onBoundary));

        // ...and just past it the key is not hit (the radius is not larger than
        // advertised): 2 px beyond the key radius no key is in range, and the
        // nearest tangent handle is tens of pixels away, so nothing is.
        const QPoint beyond(key1Center.x() + ctrl::graph::kKeyHitRadius + 2, key1Center.y());
        core::AbstractCursor beyondCur;
        sendMove(editor, camera, beyondCur, beyond);
        CHECK(!editor.ownsCursor(camera, beyondCur));
    }

    // --- 12: the degenerate value window. A channel whose keys are ALL at the
    // same value has a zero span: the fit must widen it around the value (never
    // leave a collapsed window) so drawing it cannot divide by zero.
    {
        core::TestNode flatNode;
        core::RotateKey* f0 = new core::RotateKey();
        f0->setRotate(5.0f);
        setup.push(flatNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, f0));
        core::RotateKey* f1 = new core::RotateKey();
        f1->setRotate(5.0f);
        setup.push(flatNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 30, f1));

        editor.setTargets({ &flatNode });
        const Scaler& flat = editor.scale();
        CHECK(flat.valueMin() != flat.valueMax()); // the window did NOT collapse
        CHECK(flat.valueMin() < 5.0f);             // it straddles the constant value...
        CHECK(flat.valueMax() > 5.0f);

        // Drawing it must not divide by zero: map the constant value to a real
        // row inside the plot. (The paint path lives in gui::GraphPanelModel now,
        // whose own gate asserts the mapping; this pins the window.)
        const QPoint flatPixel(flat.pixelX(core::Frame(0)), flat.pixelY(5.0f));
        CHECK(flat.plotRect().contains(flatPixel));
        CHECK(flat.valueMin() != flat.valueMax());

        // The caller's obligation, stated where the node dies: GraphEditor holds
        // RAW target pointers and never owns them, so a target MUST be dropped
        // before it goes out of scope. The application does exactly this (the
        // object tree's deletion path re-emits the selection change, and the
        // window re-calls setTargets before any notification or deferred repaint
        // runs). A target left in mTargets past its owner's death is a dangling
        // pointer the next buildEntries() walks - a SIGSEGV, not a failed
        // assertion. Every block in this file that declares a node drops it.
        editor.setTargets({});
    }

    // --- 13: the KEYLESS-selection invariant. A selection whose shown curves
    // have NO keys must leave the value window byte-identical - before and after
    // a cursor pass, and even though the fit ran (applyValueFit's `any` guard is
    // the only thing that can hold this).
    {
        core::TestNode emptyNode; // a real TimeLine with no keys at all
        editor.setTargets({ &emptyNode });

        const float keptMin = editor.scale().valueMin();
        const float keptMax = editor.scale().valueMax();
        CHECK(keptMin != keptMax); // a usable window is in force, not a collapsed one
        CHECK(keptMin != 0.0f || keptMax != 1.0f); // and not the scaler's seed

        core::AbstractCursor idle;
        sendMove(editor, camera, idle, QPoint(700, 550));
        CHECK(editor.scale().valueMin() == keptMin); // byte-identical after a cursor pass
        CHECK(editor.scale().valueMax() == keptMax);

        editor.setTargets({}); // drop the target before its owner dies (see section 12)
    }

    // --- 14: FOCUS DRIVES THE FIT. Two shown channels at disjoint value ranges:
    // with no focus the window is their union; pressing a key of one of them
    // makes it the focused channel and the window narrows to that channel alone.
    //
    // nodeA and nodeB are declared at FUNCTION scope, outside this section's
    // block, because section 15 keeps using them: a target must outlive every
    // section that leaves it in mTargets (GraphEditor holds raw pointers and
    // never owns them), and the regression section after 15 is what drops them.
    core::TestNode nodeA;
    core::TestNode nodeB;
    {
        core::RotateKey* a0 = new core::RotateKey();
        a0->setRotate(0.0f);
        setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, a0));
        core::RotateKey* a1 = new core::RotateKey();
        a1->setRotate(90.0f);
        setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, a1));

        core::RotateKey* b0 = new core::RotateKey();
        b0->setRotate(1000.0f);
        setup.push(nodeB.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, b0));
        core::RotateKey* b1 = new core::RotateKey();
        b1->setRotate(1100.0f);
        setup.push(nodeB.timeLine()->createPusher(core::TimeKeyType_Rotate, 30, b1));

        editor.setTargets({ &nodeA, &nodeB });
        CHECK(!editor.hasFocus());                        // a fresh selection focuses nothing
        CHECK(editor.scale().valueMin() <= 0.0f);         // the union covers A...
        CHECK(editor.scale().valueMax() >= 1100.0f);      // ...and B

        // Press B's key at frame 0 (value 1000): the hit focuses channel B.
        const QRect bKeyRect = ctrl::graph::keyRect(editor.scale(), core::Frame(0), 1000.0f);
        core::AbstractCursor bCur;
        sendMove(editor, camera, bCur, bKeyRect.center());
        CHECK(editor.ownsCursor(camera, bCur));
        CHECK(sendPress(editor, camera, bCur, bKeyRect.center()));
        CHECK(sendRelease(editor, camera, bCur, bKeyRect.center()));

        CHECK(editor.hasFocus());
        CHECK(editor.focusedNode() == &nodeB);
        CHECK(editor.focusedChannel() == Channel(core::TimeKeyType_Rotate, 0));

        // Both bounds: the window covers B's range and NOTHING of A's (A tops out
        // at 90, so a window whose floor is above 90 excludes every A value).
        CHECK(editor.scale().valueMin() <= 1000.0f);
        CHECK(editor.scale().valueMax() >= 1100.0f);
        CHECK(editor.scale().valueMin() > 90.0f);
    }

    // --- 15: the FRAME-axis fit. fitAll() fits both axes; afterwards every shown
    // key's frame is inside the plot's frame window and the window is not
    // degenerate. The frame axis is the graph's OWN scaler instance, so this
    // moves only the graph's frame window.
    {
        editor.scale().setRect(panel); // the rect the cursor path would have written
        editor.fitAll();

        const Scaler& fitted = editor.scale();
        const QRect plot = fitted.plotRect();
        CHECK(plot.width() > 0);

        const double leftFrame = fitted.frameAtPixel(static_cast<double>(plot.left()));
        const double rightFrame = fitted.frameAtPixel(static_cast<double>(plot.right()));
        CHECK(leftFrame <= 0.0);  // the leftmost shown key (frame 0) is inside
        CHECK(rightFrame >= 30.0); // the rightmost shown key (frame 30) is inside
        CHECK(rightFrame > leftFrame); // the frame window is not degenerate

        // Every shown key's frame maps inside the plot's horizontal span.
        for (int frame : { 0, 24, 30 }) {
            const int x = fitted.pixelX(core::Frame(frame));
            CHECK(plot.left() <= x && x <= plot.right());
        }
    }

    // --- 15b: NO TARGETS is a SAFE state, and the regression that keeps it one.
    // This is the state the application reaches the moment the selection is
    // cleared, and the state every section here must leave behind before the node
    // it targeted dies: GraphEditor holds RAW target pointers and never owns
    // them, so a target left in mTargets past its owner's death is a dangling
    // pointer the next buildEntries() walks (a SIGSEGV, not a failed assertion).
    // Everything that walks the targets must therefore be safe with none.
    {
        editor.setTargets({}); // also drops nodeA/nodeB while they are still alive

        CHECK(editor.curves().isEmpty());           // nothing shown, nothing walked
        CHECK(!editor.hasFocus());                  // a focus cannot outlive the shown set
        CHECK(!editor.ownsCursor(camera, outside)); // nothing to own

        editor.fitAll();       // both fits over an empty entry list
        editor.fitValueAxis(); // and the value fit on its own

        const QPoint kNoTargets(700, 550);
        core::AbstractCursor idle;
        sendMove(editor, camera, idle, kNoTargets); // a cursor pass: safe
        CHECK(!editor.ownsCursor(camera, idle));
        CHECK(sendPress(editor, camera, idle, kNoTargets));    // nothing to hit, nothing to insert
        CHECK(!sendRelease(editor, camera, idle, kNoTargets)); // and nothing was armed

        CHECK(editor.curves().isEmpty());
    }

    // --- 16-18: the three key edits - INSERT, MOVE and REMOVE - on one fresh
    // layer, so every assertion below is about one gesture and one undo step.
    //
    // INSERT: a press on a curve's BAND - a point on no key and no handle, whose
    // frame the curve covers and whose value is within GraphEditor::kCurveBandRadius
    // of the value the curve evaluates at that frame - inserts the key the curve
    // already passes through. The value written is the curve's own sample at that
    // frame, so no sampled value moves AT THAT FRAME, and the key's payload (the
    // easing it carries, which is the split segment's own) comes from the
    // document as well. The panel OWNS the band, which is what makes the press
    // reach the editor at all: the Driver routes by ownsCursor, and a band point
    // is on no key and no handle. The insert is refused - nothing pushed, the
    // document untouched - on an occupied frame (the key there is the more
    // specific target of the same point, and TimeLine::createPusher would
    // REPLACE it), off the band, and outside the plot.
    //
    // MOVE: pressing a KEY (not a handle) arms a move; a horizontal drag moves
    // its FRAME through the repository's own TimeLineUtil::MoveFrameOfKey, the
    // whole gesture is ONE undo step, a destination another key owns is refused,
    // and the key's value and easing never change.
    //
    // REMOVE: acts on the key the panel's selection names (the key a press last
    // hit) through TimeLine::createRemover, ONE undo step whose undo restores the
    // SAME key object with its value and easing; with no key selected, or with a
    // selection naming a frame that holds no key, it pushes nothing.
    {
        core::TestNode editNode;
        const util::Easing::Param kEditEasing = customEasing();
        core::RotateKey* r0 = new core::RotateKey();
        r0->setRotate(0.0f);
        r0->data().easing() = kEditEasing;
        setup.push(editNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, r0));
        core::RotateKey* r1 = new core::RotateKey();
        r1->setRotate(90.0f);
        setup.push(editNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, r1));

        const util::Easing::Param kR1Easing = r1->data().easing();
        core::TimeLine* editLine = editNode.timeLine();
        const Channel editChannel(core::TimeKeyType_Rotate, 0);
        const Curve editCurve(*editLine, editChannel);

        editor.setTargets({ &editNode });
        editor.scale().setRect(panel); // the rect the cursor path writes
        project.commandStack().clear(); // this section's history is the graph's own edits
        CHECK(!editor.hasFocus());      // a fresh shown set focuses nothing
        CHECK(editCurve.keys().size() == 2);
        CHECK(editCurve.keys()[0] == r0);
        CHECK(editCurve.keys()[1] == r1);

        const core::Frame addFrame(12);
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        const float atEmptyFrame = editCurve.sample(addFrame);
        CHECK(atEmptyFrame > 0.0f && atEmptyFrame < 90.0f); // a real interpolation, not an endpoint

        // 16a: a press ON a key does not insert. The key is the more specific
        // target of that point - its value IS the curve's value there, and the
        // band is narrower than the key radius - so an occupied frame is never
        // replaced: the press selects the key that is there.
        const QPoint key24(editor.scale().pixelX(core::Frame(24)), editor.scale().pixelY(90.0f));
        CHECK(ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f).contains(key24));
        core::AbstractCursor editCur;
        sendMove(editor, camera, editCur, key24);
        CHECK(editor.ownsCursor(camera, editCur));
        CHECK(sendPress(editor, camera, editCur, key24));
        CHECK(sendRelease(editor, camera, editCur, key24)); // closes the armed move
        CHECK(editCurve.keys().size() == 2);                // nothing was inserted
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 24) == r1);
        core::TimeLineEvent selOfKey24;
        CHECK(editor.retrieveSelection(selOfKey24));
        CHECK(selOfKey24.targets().size() == 1);
        CHECK(selOfKey24.targets()[0].pos.index() == 24); // the key the point was on
        bool undoneOccupied = true;
        stack.undo(&undoneOccupied);
        CHECK(!undoneOccupied); // and that press+release pushed nothing at all

        // 16b: a point OFF the band is not the panel's and inserts nothing. The
        // band is a real radius, not "anywhere on the fullscreen panel".
        const QPoint aboveBand(
            editor.scale().pixelX(addFrame),
            editor.scale().pixelY(atEmptyFrame) - (ctrl::GraphEditor::kCurveBandRadius + 2)
        );
        CHECK(editor.scale().plotRect().contains(aboveBand));
        core::AbstractCursor offBandCur;
        sendMove(editor, camera, offBandCur, aboveBand);
        CHECK(!editor.ownsCursor(camera, offBandCur));
        CHECK(sendPress(editor, camera, offBandCur, aboveBand));    // delivered, but not an insert
        CHECK(!sendRelease(editor, camera, offBandCur, aboveBand)); // and it closed no drag
        CHECK(editCurve.keys().size() == 2);
        bool undoneOffBand = true;
        stack.undo(&undoneOffBand);
        CHECK(!undoneOffBand);

        // 16c: the band press inserts the key the curve already passes through.
        const QPoint inBand(editor.scale().pixelX(addFrame), editor.scale().pixelY(atEmptyFrame));
        CHECK(editor.scale().plotRect().contains(inBand));
        sendMove(editor, camera, editCur, inBand);
        CHECK(editor.ownsCursor(camera, editCur)); // the band IS the panel's: the press can reach it
        CHECK(sendPress(editor, camera, editCur, inBand));
        // The insert armed NOTHING - it is a press action, not a gesture - so this
        // release closes no drag and is not consumed, exactly like the off-band
        // one above. (A press on a KEY does arm a move, which is why the key-press
        // releases elsewhere in this file are consumed; the band is the other
        // case.)
        CHECK(!sendRelease(editor, camera, editCur, inBand));

        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        CHECK(editCurve.keys().size() == 3);
        const core::TimeKey* insertedKey = editLine->timeKey(core::TimeKeyType_Rotate, addFrame.get());
        const core::RotateKey* inserted = static_cast<const core::RotateKey*>(insertedKey);
        CHECK(insertedKey != nullptr && inserted != r0 && inserted != r1); // a new key, not a moved one
        CHECK(inserted->rotate() == atEmptyFrame);        // seeded with the curve's own value
        CHECK(inserted->data().easing() == kEditEasing);  // the easing of the segment it split
        CHECK(editCurve.sample(addFrame) == atEmptyFrame); // re-read through the document: nothing moved
        CHECK(r0->rotate() == 0.0f && r1->rotate() == 90.0f);
        CHECK(r0->data().easing() == kEditEasing);
        CHECK(r1->data().easing() == kR1Easing); // the neighbours are untouched

        // The inserted key is the panel's current key and its channel the focused
        // one, exactly as a pressed key would be.
        core::TimeLineEvent selAfterInsert;
        CHECK(editor.retrieveSelection(selAfterInsert));
        CHECK(selAfterInsert.targets().size() == 1);
        CHECK(selAfterInsert.targets()[0].node == &editNode);
        CHECK(selAfterInsert.targets()[0].pos.index() == addFrame.get());
        CHECK(editor.hasFocus());
        CHECK(editor.focusedNode() == &editNode);
        CHECK(editor.focusedChannel() == editChannel);

        // ONE command: one undo takes the key off, a second finds nothing behind
        // it, and a redo puts the very same key object back with its value.
        bool undoneInsert = false;
        const QString insertName = stack.undo(&undoneInsert);
        CHECK(undoneInsert);
        CHECK(!insertName.isEmpty());
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        CHECK(editCurve.keys().size() == 2);
        bool undoneInsertAgain = true;
        stack.undo(&undoneInsertAgain);
        CHECK(!undoneInsertAgain);
        bool redoneInsert = false;
        stack.redo(&redoneInsert);
        CHECK(redoneInsert);
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, addFrame.get()) == insertedKey);
        CHECK(inserted->rotate() == atEmptyFrame);
        CHECK(inserted->data().easing() == kEditEasing);

        // 17: the MOVE gesture, on the key the insert made (frame 12).
        //
        // The hit geometry first: the point the renderer DRAWS the key at is
        // inside the rect the hit-test TESTS (one shared graph::keyRect), and
        // that press is what makes the key the panel's current key.
        const QPoint key12Pixel(editor.scale().pixelX(addFrame), editor.scale().pixelY(inserted->rotate()));
        CHECK(ctrl::graph::keyRect(editor.scale(), addFrame, inserted->rotate()).contains(key12Pixel));
        core::AbstractCursor moveCur;
        sendMove(editor, camera, moveCur, key12Pixel);
        CHECK(editor.ownsCursor(camera, moveCur));
        CHECK(sendPress(editor, camera, moveCur, key12Pixel));
        core::TimeLineEvent moveSel;
        CHECK(editor.retrieveSelection(moveSel));
        CHECK(moveSel.targets().size() == 1);
        CHECK(moveSel.targets()[0].node == &editNode);
        CHECK(moveSel.targets()[0].pos.type() == core::TimeKeyType_Rotate);
        CHECK(moveSel.targets()[0].pos.index() == addFrame.get());

        // A press with no drag moves nothing, and the proof is that the next undo
        // rewinds the INSERT: this press+release pushed no command of its own.
        CHECK(sendRelease(editor, camera, moveCur, key12Pixel));
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        bool undoneReachedInsert = false;
        stack.undo(&undoneReachedInsert);
        CHECK(undoneReachedInsert);
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        bool redoneInsertAgain = false;
        stack.redo(&redoneInsertAgain);
        CHECK(redoneInsertAgain);
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, addFrame.get()) == insertedKey);

        // The drag: three moves right, frame 12 -> frame 20 (all free), release.
        // The whole gesture must be ONE command.
        const int keyY = key12Pixel.y();
        const QPoint to14(editor.scale().pixelX(core::Frame(14)), keyY);
        const QPoint to17(editor.scale().pixelX(core::Frame(17)), keyY);
        const QPoint to20(editor.scale().pixelX(core::Frame(20)), keyY);
        CHECK(sendPress(editor, camera, moveCur, key12Pixel));
        CHECK(sendMove(editor, camera, moveCur, to14));
        CHECK(sendMove(editor, camera, moveCur, to17));
        CHECK(sendMove(editor, camera, moveCur, to20));
        CHECK(sendRelease(editor, camera, moveCur, to20));

        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(editCurve.keys().size() == 3);
        CHECK(editCurve.keys()[1]->frame() == 20); // the moved key, in frame order
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 20) == insertedKey); // the SAME key object
        CHECK(inserted->rotate() == atEmptyFrame);       // the move changes the frame only...
        CHECK(inserted->data().easing() == kEditEasing); // ...never the value or the easing
        CHECK(r0->rotate() == 0.0f && r1->rotate() == 90.0f);
        CHECK(r1->data().easing() == kR1Easing);

        // ONE undo takes it back to frame 12 with its value intact, a redo
        // reapplies the move, and the entry BEHIND the move's undo is the insert:
        // exactly one command for the whole three-move gesture.
        bool undoneMove = false;
        const QString moveName = stack.undo(&undoneMove);
        CHECK(undoneMove);
        CHECK(!moveName.isEmpty());
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(inserted->rotate() == atEmptyFrame);
        CHECK(inserted->data().easing() == kEditEasing);
        bool redoneMove = false;
        stack.redo(&redoneMove);
        CHECK(redoneMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        bool undoneMoveAgain = false;
        stack.undo(&undoneMoveAgain);
        CHECK(undoneMoveAgain);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        bool undoneToInsert = false;
        stack.undo(&undoneToInsert);
        CHECK(undoneToInsert);
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        bool restoredInsert = false;
        stack.redo(&restoredInsert);
        CHECK(restoredInsert);
        bool restoredMove = false;
        stack.redo(&restoredMove);
        CHECK(restoredMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));

        // The OCCUPIED destination: dragging the key onto frame 24 (r1's frame) is
        // refused. The key keeps the frame it was accepted at, r1 is not
        // displaced, nothing is announced - the return is "nothing to repaint" -
        // and the gesture stays open, so the pointer can be dragged on to a free
        // frame and that move still lands.
        const QPoint key20Pixel(editor.scale().pixelX(core::Frame(20)), editor.scale().pixelY(inserted->rotate()));
        CHECK(ctrl::graph::keyRect(editor.scale(), core::Frame(20), inserted->rotate()).contains(key20Pixel));
        const QPoint occupied(editor.scale().pixelX(core::Frame(24)), key20Pixel.y());
        const QPoint free30(editor.scale().pixelX(core::Frame(30)), key20Pixel.y());
        CHECK(sendPress(editor, camera, moveCur, key20Pixel));
        CHECK(!sendMove(editor, camera, moveCur, occupied)); // refused
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20)); // the key stayed put
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 24) == r1);
        CHECK(r1->rotate() == 90.0f);
        CHECK(inserted->rotate() == atEmptyFrame);
        CHECK(inserted->data().easing() == kEditEasing);
        CHECK(sendMove(editor, camera, moveCur, free30)); // the gesture is still live
        CHECK(sendRelease(editor, camera, moveCur, free30));
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 30) == insertedKey);
        CHECK(inserted->rotate() == atEmptyFrame);

        // This gesture is ONE command as well, and the refused step left no entry
        // of its own: the FIRST undo already takes the key 30 -> 20.
        bool undoneLateMove = false;
        stack.undo(&undoneLateMove);
        CHECK(undoneLateMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        bool undoneFirstMove = false;
        stack.undo(&undoneFirstMove);
        CHECK(undoneFirstMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get())); // the first move, undone
        bool restoredFirstMove = false;
        stack.redo(&restoredFirstMove);
        CHECK(restoredFirstMove);
        bool restoredLateMove = false;
        stack.redo(&restoredLateMove);
        CHECK(restoredLateMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));

        // 18: the REMOVE. The key is at frame 30, and the selection still names
        // frame 20 - the frame the press that armed the last move found the key
        // at. That is a selection naming a frame with no key, which is refused:
        // nothing is removed and nothing is pushed.
        core::TimeLineEvent staleSelection;
        CHECK(editor.retrieveSelection(staleSelection));
        CHECK(staleSelection.targets().size() == 1);
        CHECK(staleSelection.targets()[0].pos.index() == 20);
        CHECK(!editor.removeSelectedKey());
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 30) == insertedKey);

        // ...and with nothing selected at all, likewise.
        editor.clearSelection();
        core::TimeLineEvent noSelection;
        CHECK(!editor.retrieveSelection(noSelection));
        CHECK(!editor.removeSelectedKey());
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));

        // Neither refusal pushed anything: the next undo takes the key 30 -> 20,
        // i.e. it reaches the gesture that moved it there.
        bool undoneRefusals = false;
        stack.undo(&undoneRefusals);
        CHECK(undoneRefusals);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));
        bool redoneRefusals = false;
        stack.redo(&redoneRefusals);
        CHECK(redoneRefusals);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));

        // The real remove: the key is pressed at the point the renderer draws it
        // (inside graph::keyRect again), which makes it the current key...
        const QPoint key30Pixel(editor.scale().pixelX(core::Frame(30)), editor.scale().pixelY(inserted->rotate()));
        CHECK(ctrl::graph::keyRect(editor.scale(), core::Frame(30), inserted->rotate()).contains(key30Pixel));
        core::AbstractCursor deleteCur;
        sendMove(editor, camera, deleteCur, key30Pixel);
        CHECK(editor.ownsCursor(camera, deleteCur));
        CHECK(sendPress(editor, camera, deleteCur, key30Pixel));
        CHECK(sendRelease(editor, camera, deleteCur, key30Pixel));
        core::TimeLineEvent deleteSelection;
        CHECK(editor.retrieveSelection(deleteSelection));
        CHECK(deleteSelection.targets().size() == 1);
        CHECK(deleteSelection.targets()[0].pos.index() == 30);

        // ...and removed. Exactly that key: its neighbours stay, and the
        // selection goes with it.
        CHECK(editor.removeSelectedKey());
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        CHECK(editCurve.keys().size() == 2);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 24));
        core::TimeLineEvent droppedSelection;
        CHECK(!editor.retrieveSelection(droppedSelection));

        // ONE undo restores the SAME key object, with its value and its easing;
        // a redo removes it again; and the entry behind the remove's undo is the
        // gesture's move, so the remove was one command.
        bool undoneRemove = false;
        const QString removeName = stack.undo(&undoneRemove);
        CHECK(undoneRemove);
        CHECK(!removeName.isEmpty());
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 30) == insertedKey);
        CHECK(inserted->rotate() == atEmptyFrame);
        CHECK(inserted->data().easing() == kEditEasing);
        bool redoneRemove = false;
        stack.redo(&redoneRemove);
        CHECK(redoneRemove);
        CHECK(!editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        bool undoRemoveAgain = false;
        stack.undo(&undoRemoveAgain);
        CHECK(undoRemoveAgain);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 30));
        bool undoReachedMove = false;
        stack.undo(&undoReachedMove);
        CHECK(undoReachedMove);
        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, 20));

        // Drop the target before its owner dies, like every other block here:
        // editNode goes out of scope with this brace, and mTargets must not still
        // name it (see 15b for the contract and the regression that pins it).
        editor.setTargets({});
    }

    // --- 19: THE EASING EDIT, on a layer of its own, so every assertion below is
    // about one action and one undo step.
    //
    // setSelectedKeysEasing() changes the easing TYPE of every key the panel's
    // selection names, and it has to be ONE undo step: the route is the timeline's
    // own (one cmnd::ScopedMacro, one TimeLineUtil::Notifier carrying the
    // Type_ChangeKeyValue event, one cmnd::Assign<util::Easing::Param> per key),
    // batched - the per-key timeline helper opens a macro of its own and
    // cmnd::Stack refuses a nested one. What is asserted here is therefore the
    // HISTORY SHAPE, which is invisible from inside the implementation: after the
    // change, one undo restores the previous easing and a second finds NOTHING
    // left, and a redo reapplies it. That catches a route that pushed a SECOND
    // real command - one per key, or an extra one outside the macro. It does NOT
    // catch a duplicate command INSIDE the one macro, and that is the right
    // answer rather than a gap: the macro is the single undo step, so a second
    // Assign inside it is still one entry (measured: a mutant that pushes the
    // command twice inside the macro leaves this section green).
    //
    // The refusals are asserted beside it, because each of them is a way to leave
    // that empty history entry behind: an EMPTY selection (nothing to write), a
    // STALE selection (a frame with no key), a key whose type carries no easing
    // playback consults (the image offset is step-held, which is why the graph
    // draws it as a step and the timeline's own easing menu skips it) and a
    // request for the type the key already has (a command whose exec and undo are
    // the same value is a history entry for a change that did not happen).
    //
    // NOT HERE: a multi-key selection. The panel cannot make one - selectKey sets
    // exactly one target and nothing else writes GraphEditor's own selection - so
    // there is no second key to put in it through the public API. The action
    // handles one anyway (one macro, one notifier, every target in the event), and
    // the timeline is where a multi-key selection is made today.
    {
        core::TestNode easeNode;
        // The first key's param is deliberately NOT the Param's own default (a range, a
        // weight and a bezier that differ from it): the assertions below say the action
        // keeps the key's own fields and only replaces the TYPE, and with a default param
        // that claim would be vacuous - a fresh util::Easing::Param() would pass it. The
        // authored values are asserted against the default just below, so the assertions
        // cannot quietly become vacuous again.
        util::Easing::Param authored;
        authored.type = util::Easing::Type_Linear;
        authored.range = util::Easing::Range_In;
        authored.weight = 0.5f;
        authored.cubicBezier.x1 = 0.2f;
        authored.cubicBezier.y1 = 0.1f;
        authored.cubicBezier.x2 = 0.8f;
        authored.cubicBezier.y2 = 0.9f;
        CHECK(authored != util::Easing::Param());

        core::RotateKey* e0 = new core::RotateKey();
        e0->setRotate(10.0f);
        e0->data().easing() = authored;
        setup.push(easeNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, e0));
        core::RotateKey* e1 = new core::RotateKey();
        e1->setRotate(70.0f);
        setup.push(easeNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 12, e1));

        core::TimeLine* easeLine = easeNode.timeLine();
        const Channel easeChannel(core::TimeKeyType_Rotate, 0);

        editor.setTargets({ &easeNode });
        editor.scale().setRect(panel); // the rect the cursor path writes
        project.commandStack().clear(); // this section's history is the graph's own edit
        CHECK(e0->data().easing().type == util::Easing::Type_Linear); // the authored type
        CHECK(e0->data().easing() == authored);                       // and the rest of it
        // What the edit must NOT touch: the second key's whole param, and every
        // field of the edited key's param except the type.
        const util::Easing::Param kSecondKeyEasing = e1->data().easing();
        const util::Easing::Range kFirstRange = e0->data().easing().range;
        const float kFirstWeight = e0->data().easing().weight;
        const util::Easing::CubicBezier kFirstBezier = e0->data().easing().cubicBezier;
        CHECK(kFirstRange != util::Easing::Param().range);
        CHECK(kFirstWeight != util::Easing::Param().weight);
        CHECK(!(kFirstBezier == util::Easing::Param().cubicBezier));

        // 19a: an EMPTY selection. Nothing is written and nothing is pushed -
        // CMND-MACRO: a macro is appended to the stack even when it holds no
        // command, so an action that opened one first would leave an undo step
        // that undoes nothing.
        editor.clearSelection();
        core::TimeLineEvent emptySelection;
        CHECK(!editor.retrieveSelection(emptySelection));
        CHECK(!editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        CHECK(e0->data().easing().type == util::Easing::Type_Linear);
        bool undoneAfterEmpty = true;
        CHECK(stack.undo(&undoneAfterEmpty).isEmpty());
        CHECK(!undoneAfterEmpty);

        // 19b: a STALE selection - a frame that holds no key of that type. Same
        // answer, for the same reason: the command would be an assignment against
        // a key that is not there.
        editor.selectKey(easeNode, easeChannel, core::Frame(5));
        core::TimeLineEvent staleSelection;
        CHECK(editor.retrieveSelection(staleSelection));
        CHECK(staleSelection.targets().size() == 1);
        CHECK(!easeLine->hasTimeKey(core::TimeKeyType_Rotate, 5));
        CHECK(!editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        CHECK(staleSelection.targets()[0].pos.index() == 5); // and the selection is left alone
        CHECK(e0->data().easing().type == util::Easing::Type_Linear);
        bool undoneAfterStale = true;
        CHECK(stack.undo(&undoneAfterStale).isEmpty());
        CHECK(!undoneAfterStale);

        // 19c: the change itself - the selected key, ONE undo step.
        editor.selectKey(easeNode, easeChannel, core::Frame(0));
        CHECK(editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        CHECK(e0->data().easing().type == util::Easing::Type_Sine);
        CHECK(e0->data().easing().range == kFirstRange);       // the key's own fields...
        CHECK(e0->data().easing().weight == kFirstWeight);
        CHECK(e0->data().easing().cubicBezier == kFirstBezier);
        CHECK(e1->data().easing() == kSecondKeyEasing);        // ...and its neighbour's param

        // ONE command: an undo restores the type, a SECOND undo finds nothing
        // left, a redo reapplies it and a second redo finds nothing either.
        bool undoneEasing = false;
        const QString easingCmd = stack.undo(&undoneEasing);
        CHECK(undoneEasing);
        CHECK(!easingCmd.isEmpty());
        CHECK(e0->data().easing().type == util::Easing::Type_Linear);
        bool nothingLeftToUndo = true;
        CHECK(stack.undo(&nothingLeftToUndo).isEmpty());
        CHECK(!nothingLeftToUndo);
        bool redoneEasing = false;
        CHECK(!stack.redo(&redoneEasing).isEmpty());
        CHECK(redoneEasing);
        CHECK(e0->data().easing().type == util::Easing::Type_Sine);
        bool nothingLeftToRedo = true;
        CHECK(stack.redo(&nothingLeftToRedo).isEmpty());
        CHECK(!nothingLeftToRedo);

        // 19d: a key type the GRAPH has no easing for. graph::Curve::easingOf is
        // the graph's own type switch - the one its channels are built from - and
        // it answers null for anything outside the graphable set (a pose key's
        // transform hierarchy, like a bone's or a mesh's, is not shaped by an
        // easing the graph draws). The action refuses it: nothing is written and
        // nothing is pushed. The panel cannot select such a key today (it has no
        // channel for the type), so this pins the action's own guard rather than a
        // state the UI can reach - the same reason the header names it.
        core::PoseKey* poseKey = new core::PoseKey();
        setup.push(easeNode.timeLine()->createPusher(core::TimeKeyType_Pose, 0, poseKey));
        project.commandStack().clear();
        editor.selectKey(easeNode, Channel(core::TimeKeyType_Pose, 0), core::Frame(0));
        CHECK(!editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        bool undoneAfterPose = true;
        CHECK(stack.undo(&undoneAfterPose).isEmpty());
        CHECK(!undoneAfterPose);

        // The OTHER guard - a key whose type carries an easing param that playback
        // never reads - cannot be driven here at all: the only such type is the
        // image offset, and core::ImageKey's data binds a GL mesh buffer
        // (core::GridMesh -> gl::Global), which needs a current GL context this
        // headless test has none of. What is asserted is the RULE that guard
        // reads, on the channel the panel would draw:
        CHECK(!Channel(core::TimeKeyType_Image, 0).isInterpolated());
        CHECK(Channel::componentCount(core::TimeKeyType_Image) == 2);
        CHECK(Channel(core::TimeKeyType_Rotate, 0).isInterpolated());

        // 19e: the type the key ALREADY has. Refused, and nothing pushed: a
        // command whose exec and undo write the same param is a history entry for
        // a change that did not happen, and an undo would then appear to do
        // nothing.
        editor.selectKey(easeNode, easeChannel, core::Frame(0));
        CHECK(e0->data().easing().type == util::Easing::Type_Sine); // 19c's redo
        CHECK(!editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        bool undoneAfterSame = true;
        CHECK(stack.undo(&undoneAfterSame).isEmpty());
        CHECK(!undoneAfterSame);

        // ...and a DIFFERENT one from there is a real edit again, so the refusal
        // above was about the value and not about a broken path.
        CHECK(editor.setSelectedKeysEasing(util::Easing::Type_Bounce));
        CHECK(e0->data().easing().type == util::Easing::Type_Bounce);
        bool undoneAfterBounce = false;
        CHECK(!stack.undo(&undoneAfterBounce).isEmpty());
        CHECK(undoneAfterBounce);
        CHECK(e0->data().easing().type == util::Easing::Type_Sine);

        // Drop the target before its owner dies, like every other block here:
        // easeNode goes out of scope with this brace.
        editor.setTargets({});
    }

    // --- 20: the switch's OTHER direction - the half the show at the top of this
    // file cannot cover - and it sits last so nothing above or below runs against
    // a panel that was hidden out from under it. Hiding is what hands the pointer
    // and the wheel back to the canvas (the Driver consults isVisible() before it
    // routes anything), so a setVisible(false) that did not take would leave the
    // panel owning a viewport the user asked it to leave. The round trip is
    // asserted as one - hidden really answers false, and shown really answers true
    // again - so the editor is left in the state every section above drove it in.
    editor.setVisible(false);
    CHECK(!editor.isVisible());
    editor.setVisible(true);
    CHECK(editor.isVisible());

    std::printf(
        "graph_drag: one handle drag is one undo step, and the drag edits the key's own easing param "
        "at the pixel it points to; the value axis is fitted to the shown curves where the panel is "
        "opened or re-selected, and a manual or wheeled window survives every cursor pass; "
        "the fullscreen panel owns the cursor only on a key, a handle or a curve's insert band (or "
        "mid-drag), a press on empty space leaves the selection and the sticky focus alone, the "
        "focused channel drives the fit, and fitAll() also fits the frame axis to the shown keys; "
        "and the three key edits are one undo step each through the timeline's own route: a press on "
        "the band inserts the key the curve already passes through (no sampled value moves at that "
        "frame), a press on a key plus a horizontal drag moves its frame and refuses an occupied "
        "destination, and removeSelectedKey() removes exactly the selected key and restores the same "
        "key object on undo; and setSelectedKeysEasing() changes the easing type of the selected "
        "key(s) as ONE undo step on that same route, pushing nothing for an empty or stale selection, "
        "for the step-held image channel or for the type a key already has\n"
    );
    return 0;
}
