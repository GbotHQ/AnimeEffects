// Contract test for the animation graph's edit path: ONE handle drag IS ONE
// undo step, and the drag edits the key's OWN easing param where the drag points.
//
// The graph panel (ctrl::GraphEditor) is the only place a user reshapes a
// Custom easing's tangent handle, and that edit carries two contracts that are
// invisible from inside the implementation:
//
//   * it is ONE command. The drag holds a cmnd::Gesture over a single
//     HandleDragCommand, which carries BOTH params a step writes (the grabbed
//     one and, when the key is ALIGNED, the opposite handle's param): the press
//     only records the hit, the FIRST move commits, and every later move
//     rewrites that same command. So one undo must restore BOTH pre-drag params
//     exactly, and a second undo must find nothing left - a drag that pushed one
//     command per move cannot satisfy that.
//   * it edits the easing at the pixel it points to, and preserves the key's
//     handle PAIR. Handle 0 is the `from` key's OUTGOING tangent and handle 1 the
//     `to` key's INCOMING one, so the opposite tangent of that same key lives in
//     the neighbouring segment: segment i-1's (x2,y2) for an outgoing drag,
//     segment i+1's (x1,y1) for an incoming one. Whether the two are LINKED is
//     the key's STORED `util::Easing::Param::aligned` (document state, default
//     true, serialized with the key); nothing infers it from the pixel angle, so
//     a key moved on the value axis cannot flip it. Dragging one handle of a
//     linked key keeps the other collinear at ITS OWN LENGTH (angle linked,
//     length independent). Alt on any step leaves the opposite untouched AND
//     writes the key FREE for good; Alt+click on a key links it again along the
//     CHORD from the previous key's anchor to the next key's anchor, each handle
//     keeping its own length. The FIRST and LAST key have one handle only and
//     nothing to link.
//
// Everything is observed through the public API only - the editor's own live
// scale for the pixels, the document for the values, the project's command stack
// for the history - so the test cannot drift from the geometry the hit-test
// uses, and no formula is re-stated here that the implementation could then
// change underneath it.
//
// The panel is FULLSCREEN, so it does not own the whole viewport: ownership is
// narrowed to the PLOT rect - the data area the curves are drawn in - for a LEFT
// PRESS (a key selects and drags, Ctrl on a curve's band inserts, anything else
// marquees) and for the MIDDLE button (a pan), plus the point under a key or
// handle wherever it is, plus every point while a gesture is in flight. Both
// halves are pinned below: a press outside the plot and a plain HOVER inside it
// are nobody's edit, and an in-flight drag keeps the cursor even at a point with
// no item under it (which is also what proves the grab in section 4 is a real one
// rather than a press that missed).
//
// The value axis is pinned here too, because the drag pixels would otherwise
// hide the defect: the window has to be FITTED to the shown curves on the paths
// the application uses - the panel is toggled on and the layer selected with no
// cursor event ever reaching the panel - or a channel whose values are 0..90
// draws its whole curve and its keys outside the plot, an empty panel. The fit is
// keyed to the SET of shown (layer, channel) pairs and to nothing else: a manual
// window (the wheel's own path) survives any number of cursor passes, only a
// change of shown curves refits, and the STICKY FOCUS does NOT refit at all - a
// click that zoomed the view was the reported defect (section 14). (The paint
// path is no longer a writer here at all: GraphEditor::renderQt was deleted with
// the ImGui cutover and the panel is drawn from gui::GraphPanelModel, whose own
// gate pins that building it writes nothing back.)
//
// The panel's SELECTION is pinned at the end of the file too, and it is a
// selection of MARKERS - one COMPONENT of one key, the dot on one curve. A plain
// press replaces the selection with the marker it hit - unless that marker is
// ALREADY selected, in which case the press grabs the selection and leaves it
// exactly as it is, because a drag armed by that press carries the WHOLE
// selection (so a two-key selection is dragged by any of its own keys). Shift-press
// adds one more (which is how both of a key's markers are selected together),
// Ctrl-press toggles one, and a MARQUEE - a PLAIN
// press in the plot (Ctrl on a curve's band inserts instead), dragged and
// released - replaces it with EVERY MARKER of every key inside the dragged
// rectangle (adding, under Shift), with the rectangle published live by
// marqueeRect(); a release still within 4px of the press on both axes is the
// plain CLICK instead, which clears the selection. Ctrl+A (selectAllMarkers) is
// the same bulk rule over the shown channels from the keyboard, Escape clears
// (clearSelection), and the set
// announces every change of its own (time::Selection::onChanged) - which is what
// the timeline header's chip reads instead of a reconcile timer. A KEY-LEVEL
// entry - what the timeline's own gestures, a marquee and ctrl+A write - is every
// marker its key owns, which is why the timeline keeps highlighting a key the
// panel selected one component of.
//
// The panel's three KEY EDITS are pinned the same way (sections 16-18), on one
// layer of their own:
//
//   * INSERT: a CTRL-press on a curve's BAND (a point on no key and no handle,
//     within GraphEditor::kCurveBandRadius of the value the curve evaluates at
//     the point's frame) writes the key the curve already passes through - the
//     value is the curve's own sample there, so no sampled value moves AT THAT
//     FRAME. The modifier decides which of the graph's two plot gestures a press
//     gets: with Ctrl on a curve's band it inserts, and every other press in the
//     plot - a PLAIN press on the band included, and a Ctrl-press off every band
//     - opens the MARQUEE (Blender's f-curve editor uses the same Ctrl-click). A
//     Ctrl-press on a key toggles it in the selection, and a press outside the
//     plot is neither - none of them insert and none of them push.
//   * MOVE: a press on a KEY arms a move and a drag changes BOTH its frame and its
//     value (Shift pins it to the dominant axis of the gesture), through the
//     repository's own ctrl::TimeLineUtil::MoveFrameOfKey for the frame and
//     ctrl::TimeLineUtil::AssignKeyValue for the value, side by side in ONE macro;
//     the easing never moves, a destination another key owns refuses the whole
//     step (and leaves no entry of its own), and the whole gesture is ONE undo
//     step.
//   * REMOVE: acts on every key the panel's selection names through
//     TimeLine::createRemover; ONE undo step, and the undo restores the SAME key
//     objects with their values and easings. With no key selected, or a selection
//     naming only frames with no key, nothing is pushed.
//   * EASING (section 19): setSelectedKeysEasing() changes the easing TYPE of
//     every COMPONENT the selection names (a whole-key entry covers all of the
//     key's components; a marker only its own), on the timeline's own route - the
//     command, the notifier and the event the timeline's "Change key(s) easing
//     to" action uses - as ONE undo step. An empty or stale selection, a type whose easing
//     playback never reads (the step-held image offset) and a request for the
//     type the key already has all push NOTHING.
//
// The WHEEL is pinned in section 0b (and its delegation in section 23). It ZOOMS
// and never scrolls: plain zooms BOTH axes about the cursor, Shift zooms the FRAME
// axis alone (the frame-only zoom the 2026-10-19 remap had moved to Ctrl+Shift),
// Ctrl zooms the VALUE axis alone, and Ctrl+Shift zooms BOTH - so every form moves
// at least one axis and no notch scrolls anything. Section 0b-iii pins the COUNT
// (four Shift notches = four x1.1 steps of the frame rate, with the value window
// bit-identical) and the Ctrl+Shift notch's change on both axes with both anchors
// held. THE SCROLL BARS are pinned in section 0d (their geometry, and the two
// gestures that move a window) and in section 23 with a timeline attached: a thumb
// drag pans by exactly the pixels dragged and a press on the lane beside it advances
// the window ONE PAGE toward the press, all of it through the same frame delegate the
// pan uses.
// The MIDDLE button is pinned in section 21: while the panel is up it pans the
// graph's window (both axes, a pure pixel offset), never the canvas camera.
//
// THE TIMELINE SEAM is pinned in section 23, on a real ctrl::TimeLineEditor:
// the panel's selection is the TIMELINE's own live set (setSelectionSource), so a
// key selected here is reported by the timeline's retrieveSelectionTargets and the
// other way round - one instance, asserted by pointer identity as well as by the
// round trip; and the panel's FRAME axis, while a window has been pushed in
// (setFrameWindow), is that window: the panel does not fit, zoom or pan it itself
// (the wheel's frame half and a middle-drag pan go to the delegates), and every
// path that used to refit X on its own - a setTargets, a cursor pass, a fitAll -
// leaves the pushed window exactly where it was. Sections 0-22 run with NO
// timeline attached, which is the other half of that contract: there the panel
// still owns its frame axis and fits, zooms and pans it exactly as it always did.
//
// Every one of the edits goes through the same TimeLineUtil notifier +
// ScopedMacro route the handle drag uses, so undo/redo and the timeline's event
// currency are the same for all of them - which is what the stack probes below
// measure: after each gesture, the FIRST undo must be that gesture.
//
// THE PANEL'S SWITCH is gone with the viewport overlay it belonged to: the editor
// has no shown/hidden state (the dock's Graph page IS the panel while its tab is
// current, so no flag inside the editor could describe it, and nothing in the
// application routes by one any more - gui::GraphPanelWidget owns its page's events
// and hands them straight to the editor). What a hide still does is the ONE side
// effect hiding ever had, through its own door: GraphEditor::panelHidden() drops a
// gesture in flight. The last section drives exactly that, with a key move open, and
// pins that the dropped gesture's command stays on the stack (dropping the gesture is
// NOT an undo) and that no later move can rewrite it. The SHOW half has no state to
// assert any more; the path that matters now is the one the application really uses -
// the object tree's selection handed in through setTargets(), which is also what fits
// the value window BEFORE the page is ever shown (section 0 asserts that fit).
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
//
// THE MULTI-KEY MOVE, the GHOST HANDLES and the MARKER are pinned in sections
// 23b-23f:
//
//   * 23b - the move carries the panel's WHOLE SELECTION: a two-key selection is
//     dragged and BOTH keys move by the same frame AND value delta, the whole
//     gesture is ONE command (KeyMoveCommand), ONE undo restores every frame and
//     every value, a second undo finds nothing, and ONE redo rebuilds both. The
//     single-key move is the one-member case of exactly this path.
//   * 23c - THE PRESS THAT GRABS A SELECTION: a PLAIN press on a key that is
//     already selected leaves the selection as it is (so the drag it arms moves
//     EVERY selected key, and a key left unselected does not move), while a plain
//     press on an unselected key still replaces the selection with it (so only
//     that one moves). It is the press, not the move, that used to collapse a
//     multi-key selection to the key under the pointer.
//   * 23d - a drag on an Auto segment's GHOST handle (Curve::ghostHandles, the
//     derived pair) WRITES Type_Custom carrying that pair at (1/3, y1) and
//     (2/3, y2), inside the drag's own Gesture: one undo brings the Auto easing
//     back whole, and the drag away from the dot then writes the grabbed handle
//     at the pointer. Its segment is the channel's first, so its `from` key has
//     no incoming handle: the pair's other control point (the `to` key's
//     incoming one) stays at the derived value. The hit
//     test DERIVES that pair only for a segment whose ghost x is within the grab
//     radius of the pointer (an exact cull on the segment's own arithmetic), so
//     the cursor path cannot pay a derivation per segment: 8px from the dot still
//     grabs it, 30px away does not.
//   * 23e - THE MARKER, on a Scale key (two components): a plain press selects ONE
//     component and leaves its sibling with the faint ring's predicate; with both
//     selected, ONE diagonal drag moves the key's FRAME ONCE and moves BOTH
//     components by the same delta, each in its own channel - and the unselected
//     sibling of an X-only selection comes out bit-identical. A whole-key entry
//     (the timeline's own, a marquee, ctrl+A) is every marker of its key, so a
//     vertical drag moves both components; ctrl+A selects the shown channels' keys
//     as one entry per key and Escape clears; and the set announces every change
//     of its own, and only the changes.
//   * 23f - THE HANDLE PAIR OF ONE KEY (link state, not a mirror): a three-key
//     channel, so key B owns both an incoming (segment A-B's x2,y2) and an
//     outgoing (segment B-C's x1,y1) handle. LINKED is the key's STORED
//     `util::Easing::Param::aligned` - document state, not an inference from
//     pixel collinearity - so a key moved on the value axis keeps it. On a LINKED
//     key a drag keeps the opposite handle collinear at ITS OWN LENGTH (angle
//     linked, length independent); on a FREE key the opposite is untouched. Alt
//     held on any step stops writing the opposite AND writes the key FREE for
//     good (the latest modifier state is read, and a later drag without Alt stays
//     free); Alt+click links the pair along the CHORD between the neighbouring
//     keys' anchors, each handle keeping its own length - the direction never
//     depends on which handle is longer. One gesture/command/undo (flag
//     included); the first and last keys have one handle and nothing to link; a
//     written Auto param converts to Custom carrying its derived pair.
#include <QApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector2D>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <initializer_list>

#include "XCAssert.h"
#include "cmnd/Stack.h"
#include "core/AbstractCursor.h"
#include "core/CameraInfo.h"
#include "core/Frame.h"
#include "core/MoveKey.h"
#include "core/ObjectNode.h"
#include "core/PoseKey.h"
#include "core/Project.h"
#include "core/RotateKey.h"
#include "core/ScaleKey.h"
#include "core/TimeKey.h"
#include "core/TimeKeyType.h"
#include "core/TimeLine.h"
#include "ctrl/GraphEditor.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/UILogger.h"
#include "ctrl/graph/graph_Channel.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Geometry.h"
#include "ctrl/graph/graph_Scaler.h"
#include "gui/CanvasMover.h"
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

// Section 23 drives a REAL ctrl::TimeLineEditor (the panel's selection source is
// the timeline's own live set, so the round trip has to go through the real
// thing). Pulling that object in pulls ctrl::time::Renderer with it - the editor's
// render() builds one - and the renderer's track painter reads these five colours
// from gui/theme/TimeLine, whose .cpp this target does not link (it links `ctrl`,
// not `gui`). This test never renders a timeline, so the values are inert: the
// same reason the two stubs above exist. The alternative - adding
// src/gui/theme/TimeLine.cpp to this target - is a tests/CMakeLists.txt change,
// which is another unit's file.
#include "gui/theme/TimeLine.h"
QColor theme::TimeLine::trackColor() const { return QColor(); }
QColor theme::TimeLine::trackEdgeColor() const { return QColor(); }
QColor theme::TimeLine::trackTextColor() const { return QColor(); }
QColor theme::TimeLine::trackSelectColor() const { return QColor(); }
QColor theme::TimeLine::trackSeperatorColor() const { return QColor(); }

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
//
// Every helper takes the modifier state as an argument and feeds it to the editor
// BEFORE the cursor, the way the widget does (core::AbstractCursor carries no
// modifiers, so the panel reads them from ctrl::GraphEditor::setModifiers). The
// default is "no modifier", which is what the plain gestures use.
bool sendPress(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos, Qt::KeyboardModifiers aModifiers = Qt::NoModifier
) {
    aEditor.setModifiers(aModifiers);
    QMouseEvent event(
        QEvent::MouseButtonPress, aPos, aPos, aPos, Qt::LeftButton, Qt::LeftButton, aModifiers
    );
    if (!aCursor.setMousePress(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendMove(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos, Qt::KeyboardModifiers aModifiers = Qt::NoModifier
) {
    aEditor.setModifiers(aModifiers);
    QMouseEvent event(QEvent::MouseMove, aPos, aPos, aPos, Qt::NoButton, Qt::LeftButton, aModifiers);
    if (!aCursor.setMouseMove(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendRelease(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor,
    const QPoint& aPos, Qt::KeyboardModifiers aModifiers = Qt::NoModifier
) {
    aEditor.setModifiers(aModifiers);
    QMouseEvent event(QEvent::MouseButtonRelease, aPos, aPos, aPos, Qt::LeftButton, Qt::NoButton, aModifiers);
    if (!aCursor.setMouseRelease(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

// The MIDDLE button, through the same three steps: the panel's own pan gesture
// (GraphEditor::updateCursor's middle branch), which is why a middle drag must be
// delivered with the button held - core::AbstractCursor::isPressedMiddle is what
// says the drag is one.
bool sendMiddlePress(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor, const QPoint& aPos
) {
    QMouseEvent event(
        QEvent::MouseButtonPress, aPos, aPos, aPos, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier
    );
    if (!aCursor.setMousePress(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendMiddleMove(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor, const QPoint& aPos
) {
    QMouseEvent event(QEvent::MouseMove, aPos, aPos, aPos, Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    if (!aCursor.setMouseMove(&event, aCamera))
        return false;
    return aEditor.updateCursor(aCamera, aCursor);
}

bool sendMiddleRelease(
    ctrl::GraphEditor& aEditor, const core::CameraInfo& aCamera, core::AbstractCursor& aCursor, const QPoint& aPos
) {
    QMouseEvent event(QEvent::MouseButtonRelease, aPos, aPos, aPos, Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
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
    key0->data().easing(0) = kAuthored;
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
    CHECK(key0->data().easing(0) == kAuthored);
    CHECK(key0->data().easing(0).type == util::Easing::Type_Custom);

    // The second key's own param: the drag must never write it, so it is read
    // once and compared after every phase.
    const util::Easing::Param kOtherKeyEasing = key1->data().easing(0);

    core::CameraInfo camera;
    camera.setScreenSize(QSize(800, 600));
    const QRect panel = ctrl::GraphEditor::panelRect(camera.screenSize());

    NullLogger logger;
    ctrl::GraphEditor editor(project, logger);
    // The show this file rides on is the surviving path the application uses: the
    // editor has no shown/hidden state any more (the dock's Graph page IS the panel
    // while its tab is current), so what used to be asserted here - the editor is
    // not inert - is asserted right below as the FIT: with the targets set and no
    // cursor event ever reaching the editor, the value window must leave the
    // scaler's seed 0..1 (section 0's own precondition, which is what a setVisible
    // that silently did not take used to make a run fail loudly for).
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
    // The fullscreen panel covers the whole viewport, so this point is INSIDE its
    // rect - and still not the panel's: it is OUTSIDE the plot (the label column
    // and the margin are not data), and ownership is plot-based for presses. A
    // hover there is nobody's, and a press there passes through to the tools.
    CHECK(panel.contains(kProbe));
    core::AbstractCursor probe;
    sendMove(editor, camera, probe, kProbe);
    CHECK(!editor.ownsCursor(camera, probe));
    CHECK(sendPress(editor, camera, probe, kProbe)); // delivered, though the panel does not own it
    CHECK(!sendRelease(editor, camera, probe, kProbe)); // and it closes no drag
    CHECK(editor.scale().valueMin() == -5.0f);
    CHECK(editor.scale().valueMax() == 5.0f);

    // The wheel's path too, since that is where a user really moves the window.
    // THE RESTORED TABLE. The 2026-10-19 round briefly made Shift a two-axis
    // SCROLL; the wheel is a ZOOM on every modifier form again - moving a window
    // without changing its scale is the SCROLL BARS' job (section 0d below) - and
    // the four forms are:
    //   * plain         -> zoom BOTH axes about the cursor;
    //   * Shift         -> zoom the FRAME axis ALONE (what Ctrl+Shift used to be);
    //   * Ctrl          -> zoom the VALUE axis ALONE (unchanged);
    //   * Ctrl + Shift  -> zoom BOTH axes, exactly as plain does.
    // Every form zooms at least one axis, so NO NOTCH SCROLLS ANYTHING and none is
    // consumed by a case that moved nothing - the two things that were wrong with
    // the round this rewrites. Each form is asserted on the MAPPING the two axes
    // produce (pixels between two frames; the value span; the anchored frame and
    // the value under the cursor), because a move that changed the stored numbers
    // without moving the pixels would prove nothing.
    const QPoint kWheelPoint = panel.center();
    const int beforeFrameSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const float beforeValueSpan = editor.scale().valueMax() - editor.scale().valueMin();
    CHECK(beforeFrameSpan > 0);
    CHECK(beforeValueSpan > 0.0f);

    // A NEGATIVE notch (the wheel rotated toward the user) is the zoom IN, and it
    // is the direction the panel has headroom in: the frame axis' ladder starts at
    // 2 px/frame, so a zoom-out notch would already be at its floor and prove
    // nothing.
    CHECK(editor.updateWheel(camera, kWheelPoint, -120, Qt::NoModifier, false)); // one notch: closer
    const int afterFrameSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const float afterValueSpan = editor.scale().valueMax() - editor.scale().valueMin();
    // The application's own wheel direction (ctrl::CanvasMover and
    // ctrl::TimeLineEditor both zoom OUT on a positive angleDelta, so a negative
    // one is the zoom IN) on BOTH axes.
    CHECK(afterFrameSpan > beforeFrameSpan);   // the FRAME axis zoomed in...
    CHECK(afterValueSpan < beforeValueSpan);   // ...and so did the VALUE axis

    // SHIFT ZOOMS THE FRAME AXIS ALONE - it is the frame-only zoom the round before
    // last had, where Ctrl+Shift now is. The VALUE window must come back
    // BIT-identical (a notch that also touched it would not be "alone"), the frame
    // axis must be a ZOOM (its pixels-per-frame changes and it holds the frame under
    // the cursor) and not the SCROLL this test used to pin: a scroll leaves the rate
    // alone and moves the origin, which `ppf` and the anchor together can tell apart.
    const double beforeShiftPpf = editor.scale().framePpf();
    const int beforeShiftSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const float beforeShiftMin = editor.scale().valueMin();
    const float beforeShiftMax = editor.scale().valueMax();
    const double beforeShiftValueY = editor.scale().pixelYOfValue(0.5f);
    const double shiftAnchorFrame = editor.scale().frameAtPixel(kWheelPoint.x());
    CHECK(editor.updateWheel(camera, kWheelPoint, -120, Qt::ShiftModifier, false));
    CHECK(editor.scale().framePpf() > beforeShiftPpf); // the frame axis ZOOMED (a notch in)...
    CHECK(editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0)) > beforeShiftSpan);
    CHECK(std::fabs(editor.scale().frameAtPixel(kWheelPoint.x()) - shiftAnchorFrame) < 1e-9); // ...about the cursor
    CHECK(editor.scale().valueMin() == beforeShiftMin); // and the value window is bit-identical
    CHECK(editor.scale().valueMax() == beforeShiftMax);
    CHECK(editor.scale().pixelYOfValue(0.5f) == beforeShiftValueY); // so the value CONTENT did not move either
    const int afterShiftSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));

    // CTRL+SHIFT: BOTH axes, exactly as the plain wheel does (the two modifiers name
    // the two axes). Both mappings move and neither origin is left as a plain
    // scroll would leave it: the frame axis keeps the frame under the cursor and the
    // value axis zooms about the cursor's row, so the anchored point is preserved on
    // both.
    const float beforeCtrlShiftMin = editor.scale().valueMin();
    const float beforeCtrlShiftMax = editor.scale().valueMax();
    const double beforeCtrlShiftPpf = editor.scale().framePpf();
    const int beforeCtrlShiftSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const double ctrlShiftAnchorFrame = editor.scale().frameAtPixel(kWheelPoint.x());
    const double ctrlShiftAnchorValue = editor.scale().valueAtPixel(kWheelPoint.y());
    CHECK(editor.updateWheel(camera, kWheelPoint, -120, Qt::ControlModifier | Qt::ShiftModifier, false));
    CHECK(editor.scale().framePpf() > beforeCtrlShiftPpf); // the FRAME axis zoomed...
    CHECK(editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0)) > beforeCtrlShiftSpan);
    CHECK(std::fabs(editor.scale().frameAtPixel(kWheelPoint.x()) - ctrlShiftAnchorFrame) < 1e-9); // ...anchored
    CHECK(editor.scale().valueMax() - editor.scale().valueMin() < beforeCtrlShiftMax - beforeCtrlShiftMin); // and so did the VALUE axis
    CHECK(std::fabs(editor.scale().valueAtPixel(kWheelPoint.y()) - ctrlShiftAnchorValue) < 1e-4); // anchored on its row too
    const int afterCtrlShiftSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));

    // Ctrl alone: the value axis alone, and the frame axis untouched this time.
    const double beforeCtrlPpf = editor.scale().framePpf();
    const int beforeCtrlSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const float beforeCtrlValueSpan = editor.scale().valueMax() - editor.scale().valueMin();
    CHECK(editor.updateWheel(camera, kWheelPoint, -120, Qt::ControlModifier, false));
    CHECK(editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0)) == beforeCtrlSpan);
    CHECK(editor.scale().framePpf() == beforeCtrlPpf); // the frame axis did not move at all...
    CHECK(editor.scale().valueMax() - editor.scale().valueMin() < beforeCtrlValueSpan); // ...and the value axis did
    const float afterCtrlValueSpan = editor.scale().valueMax() - editor.scale().valueMin();

    // A notch the OTHER way is the inverse zoom, on the same two axes: the wheel
    // is symmetric, so a wheel-away notch undoes what a wheel-toward notch did.
    // (The frame axis is a ladder of integer pixels-per-frame, so "the inverse" is
    // asserted as "the span shrank", not as "the same number came back".)
    const int beforeOutFrameSpan = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
    const float beforeOutValueSpan = editor.scale().valueMax() - editor.scale().valueMin();
    CHECK(editor.updateWheel(camera, kWheelPoint, 120, Qt::NoModifier, false));
    CHECK(editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0)) < beforeOutFrameSpan);
    CHECK(editor.scale().valueMax() - editor.scale().valueMin() > beforeOutValueSpan);

    std::printf(
        "[W] wheel (RESTORED: zoom only, no notch scrolls): plain -> frame px(0..24) %d -> %d and value span "
        "%.4f -> %.4f (BOTH axes); Shift -> frame-only zoom px(0..24) %d -> %d, ppf %.6f -> %.6f, value span "
        "%.4f bit-identical; Ctrl+Shift -> both axes, px(0..24) %d -> %d and value span shrinking from %.4f; "
        "Ctrl -> value-only, px(0..24) %d unchanged at %d, value span %.4f -> %.4f\n",
        beforeFrameSpan, afterFrameSpan, beforeValueSpan, afterValueSpan, beforeShiftSpan, afterShiftSpan,
        beforeShiftPpf, editor.scale().framePpf(), beforeShiftMax - beforeShiftMin, beforeCtrlShiftSpan,
        afterCtrlShiftSpan, beforeCtrlShiftMax - beforeCtrlShiftMin, beforeCtrlSpan, beforeCtrlSpan,
        beforeCtrlValueSpan, afterCtrlValueSpan
    );

    // The window the wheel left, against the cursor pass that follows: nothing is
    // supposed to write it, so nothing may round it either.
    const float wheeledMin = editor.scale().valueMin();
    const float wheeledMax = editor.scale().valueMax();
    CHECK(wheeledMax > wheeledMin); // a notch that moved nothing would prove nothing
    CHECK(wheeledMin != -5.0f || wheeledMax != 5.0f);
    CHECK(sendPress(editor, camera, probe, kProbe));
    CHECK(!sendRelease(editor, camera, probe, kProbe));
    CHECK(editor.scale().valueMin() == wheeledMin);
    CHECK(editor.scale().valueMax() == wheeledMax);

    // The wheel really moved the FRAME window about the cursor (that is the half
    // of it that is not about the value axis), and the sections below measure
    // pixels against it - so it is restored to the shown keys here, explicitly.
    // Without this the frame origin the three zoom-ins left behind puts the keys
    // off the left edge of the plot, and every later pixel assertion would be
    // about a key nobody can see.
    editor.fitFrameAxis();
    CHECK(editor.scale().plotRect().contains(
        QPoint(editor.scale().pixelX(core::Frame(0)), editor.scale().pixelY(0.0f))
    ));

    // --- 0b-ii: THE ZOOM DIRECTION IS THE USER'S SETTING, AND IT IS ONE DIRECTION
    // FOR ALL THREE SURFACES. The panel used to read the raw wheel and no setting
    // at all, so with "Inverse canvas scrolling" ON - which IS the setting the panel
    // takes its wheel under: it takes the wheel from the main view, see
    // MainDisplayWidget::wheelEvent - the canvas and the timeline zoomed IN on a
    // wheel-away notch while the panel zoomed OUT.
    //
    // The three are pinned AGAINST EACH OTHER here, on the real objects: the mover
    // is gui::CanvasMover and the timeline ctrl::TimeLineEditor, both of which this
    // test compiles/links (the mover because CanvasMover.cpp is in this target's
    // sources - see tests/CMakeLists.txt), so the comparison cannot drift from the
    // code that actually runs it. The panel's half is its VALUE axis: that is the
    // axis neither of the other two shows, and the delta the setting inverts is one
    // value for both halves (see GraphEditor::updateWheel).
    {
        gui::CanvasMover mover;
        core::CameraInfo moverCamera;
        mover.setCamera(&moverCamera);
        ctrl::TimeLineEditor tline;
        tline.setProject(&project);
        tline.setFrameRange(util::Range(1, 60));

        for (int i = 0; i < 2; ++i) {
            const bool invertScaling = i == 1;
            const float moverPreScale = moverCamera.scale();
            const double tlinePrePpf = tline.pixelsPerFrame();
            const float panelPreSpan = editor.scale().valueMax() - editor.scale().valueMin();

            // ONE wheel-away notch (Qt reports it as +120), the user's setting as it is
            CHECK(mover.updateByWheel(QVector2D(100.0f, 100.0f), 120, invertScaling));
            CHECK(tline.updateWheel(120, invertScaling));
            CHECK(editor.updateWheel(camera, kWheelPoint, 120, Qt::ControlModifier, invertScaling));

            const float panelSpan = editor.scale().valueMax() - editor.scale().valueMin();
            const bool closer = moverCamera.scale() > moverPreScale; // the canvas' own answer
            CHECK((tline.pixelsPerFrame() > tlinePrePpf) == closer); // the timeline agrees with it...
            CHECK((panelSpan < panelPreSpan) == closer);             // ...and the panel agrees with BOTH
            CHECK(closer == invertScaling);                          // and the setting is what decides
            std::printf(
                "[W] direction invert=%d: canvas scale %.4f -> %.4f, timeline %.4f -> %.4f px/frame, "
                "panel value span %.4f -> %.4f (all three %s)\n",
                invertScaling ? 1 : 0, moverPreScale, moverCamera.scale(), tlinePrePpf, tline.pixelsPerFrame(),
                panelPreSpan, panelSpan, closer ? "closer" : "further away"
            );
        }

        // Both axes of the window back on the shown keys, which is the state the
        // sections below measure their pixels in.
        editor.fitAll();
    }

    // --- 0b-iii: A COUNT OF SHIFT NOTCHES IS A COUNT OF FRAME-AXIS ZOOMS, AND
    // Ctrl+SHIFT MOVES BOTH AXES IN PLACE. The single-notch forms above pin the
    // direction and the axis split; these pin the ARITHMETIC at a count (four) and
    // measure the frame axis through its MAPPING - pixelXOfFrame() and framePpf() -
    // not through the rounded pixel between two frames, so a rate change cannot hide
    // inside the span. Everything is asserted THROUGH GraphEditor::updateWheel, the
    // one place a notch becomes a window move:
    //   * Shift: every one of the four notches is consumed; the frame axis' RATE
    //     moves (x1.1 a notch, whichever rung the ladder lands on) and holds the
    //     frame under the cursor, while the value window comes back BIT-identical -
    //     so four Shift notches move four times the frame axis and not one pixel of
    //     anything else. A SCROLL would have left the rate alone: this is the
    //     assertion the 2026-10-19 remap would fail.
    //   * Ctrl+Shift: one notch (a notch IN) raises the frame rate, moves the value
    //     axis' span too (both axes is the whole point of the combination), and holds
    //     the anchored frame and the anchored value.
    {
        const Scaler& scale = editor.scale();
        constexpr int kNotches = 4;
        const double frameX0 = scale.pixelXOfFrame(0.0);
        const double framePpf0 = scale.framePpf();
        const float valueMin0 = scale.valueMin();
        const float valueMax0 = scale.valueMax();
        const float valueSpan0 = valueMax0 - valueMin0;
        const double valueY0 = scale.pixelYOfValue(0.5f);
        const double anchorX = static_cast<double>(kWheelPoint.x());
        const double anchorFrame0 = scale.frameAtPixel(anchorX);
        bool taken = true;
        for (int i = 0; i < kNotches; ++i)
            taken = editor.updateWheel(camera, kWheelPoint, -120, Qt::ShiftModifier, false) && taken;
        CHECK(taken); // a Shift notch over the panel is consumed
        // x1.1 a notch, measured as a RATIO: the ladder computes 1.1^index with pow(),
        // so four rungs up is 1.1^4 of the rate and not necessarily the product of
        // four multiplications bit for bit.
        CHECK(std::fabs(scale.framePpf() / framePpf0 - 1.4641) < 1e-9);
        CHECK(scale.pixelXOfFrame(0.0) != frameX0);            // the frame content moved (a zoom does)...
        CHECK(std::fabs(scale.frameAtPixel(anchorX) - anchorFrame0) < 1e-9); // ...holding the anchored frame
        CHECK(scale.valueMin() == valueMin0); // Shift moves the VALUE axis not at all: bit for bit...
        CHECK(scale.valueMax() == valueMax0);
        CHECK(scale.valueMax() - scale.valueMin() == valueSpan0); // ...so its span is the same float too
        CHECK(scale.pixelYOfValue(0.5f) == valueY0);           // and the value content did not move either

        // CTRL+SHIFT: BOTH axes. The frame axis' rate rises and the value axis' span
        // shrinks (a notch in is a zoom in on both), with each axis keeping the point
        // under the cursor - the two halves of one notch, not a pan of either.
        const double bothPpf0 = scale.framePpf();
        const float bothMin0 = scale.valueMin();
        const float bothMax0 = scale.valueMax();
        const double bothAnchorValue = scale.valueAtPixel(kWheelPoint.y());
        CHECK(editor.updateWheel(camera, kWheelPoint, -120, Qt::ControlModifier | Qt::ShiftModifier, false));
        CHECK(scale.framePpf() > bothPpf0); // the frame axis zoomed
        CHECK(scale.valueMax() - scale.valueMin() < bothMax0 - bothMin0); // and the value axis did too
        CHECK(scale.valueMin() != bothMin0 || scale.valueMax() != bothMax0);
        CHECK(std::fabs(scale.frameAtPixel(anchorX) - anchorFrame0) < 1e-9); // the frame anchor held
        CHECK(std::fabs(scale.valueAtPixel(kWheelPoint.y()) - bothAnchorValue) < 1e-4); // and the value anchor

        std::printf(
            "[W] wheel steps: %d Shift notches -> ppf %.6f -> %.6f (x1.1 each) with the value span %.4f "
            "BIT-IDENTICAL; Ctrl+Shift -> ppf %.6f (up) and value span %.4f -> %.4f (down), both anchors held\n",
            kNotches, framePpf0, scale.framePpf(), valueSpan0, bothPpf0, bothMax0 - bothMin0,
            scale.valueMax() - scale.valueMin()
        );

        // Both axes back on the shown keys for the sections below.
        editor.fitAll();
    }

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

    // --- 0d: THE SCROLL BARS. The two bars are the panel's way of moving a WINDOW
    // without changing its scale (the wheel zooms, they scroll): the horizontal one
    // along the plot's bottom edge scrolls the FRAME axis - its content is the
    // project's frame range - and the vertical one along the plot's right edge
    // scrolls the VALUE axis, whose content is the shown curves' key extent
    // (shownValueExtent, the range fitValueAxis fits). This section pins:
    //   * WHERE THEY ARE: outside the plot, inside the panel, disjoint from each
    //     other, the application's 12px lane with the scaler's 2px gap - the reserve
    //     the plot gave up at its right and bottom edges (Scaler::kScrollBarReserve);
    //   * a press on a THUMB (its run) pans its axis by EXACTLY the pixels dragged;
    //   * a press on the LANE beside the run pages the window by its own span toward
    //     the press, and then continues as that same drag;
    //   * and NONE of it pushes a command: a scroll bar moves a window, and the
    //     window is not the document.
    // The geometry is the scaler's own scrollBars(), computed with the two contents
    // the EDITOR uses - the same call the editor's press routing makes and (in
    // tests/graph_panel_model.cpp) the one the model publishes its four rects from.
    {
        const Scaler& scale = editor.scale();
        const QRect plot = scale.plotRect();
        float valueLo = 0.0f;
        float valueHi = 0.0f;
        CHECK(editor.shownValueExtent(valueLo, valueHi)); // the node's rotate curve has keys
        CHECK(valueHi > valueLo);
        const util::Range frames = editor.frameRange();
        CHECK(frames.max() > frames.min());

        // A WINDOW WITH A PARTIAL THUMB, so a press can be told apart from a thumb
        // press: the frame axis asked for frames 0..10 of the project's range (a short
        // window at the START of the range, so the thumb sits at the track's left) and
        // the value axis for 0..10 of the curve's 0..90 (the thumb at the TOP, the
        // value axis being inverted along its track).
        editor.scale().fitFrameAxis(core::Frame(0), core::Frame(10));
        editor.scale().setValueWindow(0.0f, 10.0f);
        const Scaler::ScrollBars bars = scale.scrollBars(frames.min(), frames.max(), valueLo, valueHi);

        CHECK(!bars.isEmpty());
        CHECK(bars.hTrack.height() == Scaler::kScrollBarThickness);
        CHECK(bars.hTrack.width() == plot.width());
        CHECK(bars.hTrack.left() == plot.left());
        CHECK(bars.hTrack.top() == plot.bottom() + 1 + Scaler::kScrollBarGap);
        CHECK(bars.vTrack.width() == Scaler::kScrollBarThickness);
        CHECK(bars.vTrack.height() == plot.height());
        CHECK(bars.vTrack.top() == plot.top());
        CHECK(bars.vTrack.left() == plot.right() + 1 + Scaler::kScrollBarGap);
        CHECK(panel.contains(bars.hTrack));
        CHECK(panel.contains(bars.vTrack));
        CHECK(!bars.hTrack.intersects(plot));
        CHECK(!bars.vTrack.intersects(plot));
        CHECK(!bars.hTrack.intersects(bars.vTrack)); // the two bars cannot overlap
        // The value column shares the plot's rows and ends at plot.bottom(), so a
        // track below the plot is clear of it by construction, and the vertical track
        // starts right of the plot - where the column is not.
        CHECK(bars.hTrack.top() > plot.bottom());
        CHECK(bars.vTrack.left() > plot.right());
        CHECK(bars.hTrack.contains(bars.hThumb)); // a thumb never leaves its track
        CHECK(bars.vTrack.contains(bars.vThumb));
        CHECK(bars.hThumb.width() < bars.hTrack.width()); // the window is narrower than the content
        CHECK(bars.vThumb.height() < bars.vTrack.height());
        CHECK(bars.hThumb.left() == bars.hTrack.left()); // the window starts at the range's first frame

        // (1) A THUMB DRAG PANS BY EXACTLY THE PIXELS DRAGGED. The frame half moves
        // the mapping's origin by the pointer's delta - measured through
        // pixelXOfFrame, the mapping itself and not a rounded pixel - and leaves the
        // value window bit-identical; the value half does the same to the value
        // window and leaves the frame axis alone.
        const QPoint hGrab = bars.hThumb.center();
        const double frameOriginBefore = scale.pixelXOfFrame(0.0);
        const float valueMinBefore = scale.valueMin();
        const float valueMaxBefore = scale.valueMax();
        core::AbstractCursor hCur;
        {
            QMouseEvent down(
                QEvent::MouseButtonPress, hGrab, hGrab, hGrab, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
            );
            CHECK(hCur.setMousePress(&down, camera));
        }
        // THE PANEL OWNS THE PRESS: asked with the cursor's own press event set, the
        // way ctrl::Driver asks it - a bar press must not fall through to the plot's
        // gestures or to the tools.
        CHECK(editor.ownsCursor(camera, hCur));
        CHECK(editor.updateCursor(camera, hCur));
        const int selectionBefore = editor.selectionCount();
        CHECK(hGrab.y() > plot.bottom()); // the grab really is on the bar, outside the plot
        const QPoint hDelta(37, 0);
        CHECK(sendMove(editor, camera, hCur, hGrab + hDelta));
        CHECK(sendRelease(editor, camera, hCur, hGrab + hDelta));
        // Exactly the pixels dragged: the origin is a double the pan adds the integer
        // delta to, so the comparison is the mapping's own arithmetic to within the
        // double's rounding - an 11px drag is not 10 or 12.
        CHECK(std::fabs(scale.pixelXOfFrame(0.0) - frameOriginBefore - 37.0) < 1e-9);
        CHECK(scale.valueMin() == valueMinBefore); // and not one value in the process
        CHECK(scale.valueMax() == valueMaxBefore);
        CHECK(editor.selectionCount() == selectionBefore); // no plot gesture ran
        CHECK(editor.marqueeRect().isEmpty());            // and no marquee was opened

        // (2) A LANE PRESS IS ONE PAGE TOWARD THE PRESS, THEN A DRAG. A press on the
        // lane beside the run moves the window by its OWN span - a page is the visible
        // window, the unit a QScrollBar's PageStep uses - in the press's direction, and
        // the move that follows pans from the press position exactly as a thumb grab
        // does. The press here is to the RIGHT of a window sitting at the range's
        // start, so it pages FORWARD by exactly one window.
        editor.scale().fitFrameAxis(core::Frame(0), core::Frame(10)); // the thumb back to the left end
        const Scaler::ScrollBars jump = scale.scrollBars(frames.min(), frames.max(), valueLo, valueHi);
        const QPoint trackPress(jump.hTrack.right() - 2, jump.hTrack.center().y());
        CHECK(!jump.hThumb.contains(trackPress)); // a LANE press: off the run
        const QRect jumpPlot = scale.plotRect();
        const double leftFrameBeforePage = scale.frameAtPixel(jumpPlot.left());
        const double rightFrameBeforePage = scale.frameAtPixel(jumpPlot.left() + jumpPlot.width());
        core::AbstractCursor jumpCur;
        {
            QMouseEvent down(
                QEvent::MouseButtonPress, trackPress, trackPress, trackPress, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier
            );
            CHECK(jumpCur.setMousePress(&down, camera));
        }
        CHECK(editor.ownsCursor(camera, jumpCur));
        CHECK(editor.updateCursor(camera, jumpCur));
        // ONE PAGE: the frame that was at the window's right edge is now at its left.
        CHECK(std::fabs(scale.frameAtPixel(jumpPlot.left()) - rightFrameBeforePage) < 1e-9);
        CHECK(scale.frameAtPixel(jumpPlot.left()) > leftFrameBeforePage); // it really moved forward
        const double jumpedOrigin = scale.pixelXOfFrame(0.0);
        const QPoint jumpDelta(-11, 0);
        CHECK(sendMove(editor, camera, jumpCur, trackPress + jumpDelta));
        CHECK(sendRelease(editor, camera, jumpCur, trackPress + jumpDelta));
        CHECK(std::fabs(scale.pixelXOfFrame(0.0) - jumpedOrigin + 11.0) < 1e-9); // the drag after a page pans the same

        // (3) THE VERTICAL BAR, the same two gestures on the value axis: a thumb drag
        // moves the value window by the pixels dragged (and the frame axis not at
        // all), and a lane press pages the window by its own span toward the press.
        // The window is 0..10 of the key extent 0..90 - the content's LOW end - so,
        // the value axis being inverted along its track, its thumb sits at the track's
        // BOTTOM and the free lane is the top: a press there pages toward HIGHER
        // values, which is one full window up.
        editor.scale().setValueWindow(0.0f, 10.0f);
        const Scaler::ScrollBars vBars = scale.scrollBars(frames.min(), frames.max(), valueLo, valueHi);
        const QPoint vGrab = vBars.vThumb.center();
        const double vFrameOriginBefore = scale.pixelXOfFrame(0.0);
        const float vSpanBefore = scale.valueMax() - scale.valueMin();
        const double valueUnderGrab = scale.valueAtPixel(vGrab.y());
        core::AbstractCursor vCur;
        {
            QMouseEvent down(
                QEvent::MouseButtonPress, vGrab, vGrab, vGrab, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
            );
            CHECK(vCur.setMousePress(&down, camera));
        }
        CHECK(editor.ownsCursor(camera, vCur));
        CHECK(editor.updateCursor(camera, vCur));
        const QPoint vDelta(0, 24);
        CHECK(sendMove(editor, camera, vCur, vGrab + vDelta));
        CHECK(sendRelease(editor, camera, vCur, vGrab + vDelta));
        // The value that was under the pointer is now 24 px lower in the plot, with
        // the window's SPAN untouched: a pan, never a zoom.
        CHECK(std::fabs(scale.valueAtPixel(vGrab.y() + 24) - valueUnderGrab) < 1e-3);
        CHECK(std::fabs((scale.valueMax() - scale.valueMin()) - vSpanBefore) < 1e-5f);
        CHECK(scale.pixelXOfFrame(0.0) == vFrameOriginBefore); // the frame axis did not move

        editor.scale().setValueWindow(0.0f, 10.0f);
        const Scaler::ScrollBars vJump = scale.scrollBars(frames.min(), frames.max(), valueLo, valueHi);
        const QPoint vTrackPress(vJump.vTrack.center().x(), vJump.vTrack.top() + 2);
        CHECK(!vJump.vThumb.contains(vTrackPress)); // the thumb is at the bottom: this is off it
        const float vMinBeforePage = scale.valueMin();
        const float vSpanAtPage = scale.valueMax() - scale.valueMin();
        core::AbstractCursor vJumpCur;
        {
            QMouseEvent down(
                QEvent::MouseButtonPress, vTrackPress, vTrackPress, vTrackPress, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier
            );
            CHECK(vJumpCur.setMousePress(&down, camera));
        }
        CHECK(editor.ownsCursor(camera, vJumpCur));
        CHECK(editor.updateCursor(camera, vJumpCur));
        // ONE PAGE up: the window moved by its own span, and the span did not change.
        CHECK(std::fabs(scale.valueMin() - (vMinBeforePage + vSpanAtPage)) < 1e-3f);
        CHECK(std::fabs((scale.valueMax() - scale.valueMin()) - vSpanAtPage) < 1e-5f);
        CHECK(sendRelease(editor, camera, vJumpCur, vTrackPress));

        // (4) NOTHING WAS PUSHED, and nothing was announced: the two windows moved and
        // the document did not notice.
        bool barUndo = true;
        stack.undo(&barUndo);
        CHECK(!barUndo); // the bars pushed no command
        CHECK(changeEvents == 0);

        std::printf(
            "[B] scroll bars: plot (%d,%d %dx%d)  hTrack (%d,%d %dx%d) run %d px at +%d  vTrack (%d,%d %dx%d) "
            "run %d px at +%d  thumb drag = 37 px of frame origin, lane page moved the frame window one full "
            "span forward, value window one span up (span %.4f of 0..90), commands pushed 0\n",
            plot.x(), plot.y(), plot.width(), plot.height(), bars.hTrack.x(), bars.hTrack.y(),
            bars.hTrack.width(), bars.hTrack.height(), bars.hThumb.width(),
            bars.hThumb.left() - bars.hTrack.left(), bars.vTrack.x(), bars.vTrack.y(), bars.vTrack.width(),
            bars.vTrack.height(), bars.vThumb.height(), bars.vThumb.top() - bars.vTrack.top(), vSpanAtPage
        );

        // Both axes back on the shown keys: the sections below measure their pixels
        // against the fit.
        editor.fitAll();
        CHECK(editor.scale().plotRect().contains(
            QPoint(editor.scale().pixelX(core::Frame(0)), editor.scale().pixelY(0.0f))
        ));
    }

    // --- 0e: THE RULER BAND SEEKS. The timeline's ruler moves the document's
    // current frame on a press and keeps moving it on the drag (TimeRuler ->
    // TimeLineEditorWidget::updateCursor); the graph's band is the same ruler seen by
    // the other view, so its press must reach the same setter - the delegate the host
    // hands over. The frame is the PROJECT's, so nothing here may push a command, and
    // with no delegate the band is nobody's.
    {
        ctrl::graph::Scaler bandScale = editor.scale();
        const QRect plot = bandScale.plotRect();
        CHECK(!plot.isEmpty());
        const QRect band(
            plot.left(), plot.top() - ctrl::graph::Scaler::kMargin - ctrl::graph::Scaler::kRulerBandHeight,
            plot.width(), ctrl::graph::Scaler::kRulerBandHeight
        );
        CHECK(!band.intersects(plot)); // a section of its own, as the model publishes it

        // "would this press be the panel's" without delivering it: the cursor takes
        // the press event, then ownsCursor answers on that state (the same pair the
        // widget's own routing asks).
        const auto pressAt = [&](const QPoint& aPoint) {
            core::AbstractCursor cursor;
            QMouseEvent event(
                QEvent::MouseButtonPress, aPoint, aPoint, aPoint, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
            );
            cursor.setMousePress(&event, camera);
            return editor.ownsCursor(camera, cursor);
        };

        // THE BAND IS NOT THE PLOT: its rows are a section above the plot's (which is
        // where the ruler's own seek lives), and a press in the plot is the plot's
        // gesture - asserted where it belongs, in the marquee sections below.
        CHECK(plot.top() == band.bottom() + 1 + ctrl::graph::Scaler::kMargin);

        QVector<int> seeks;
        editor.setFrameSeekDelegate([&](core::Frame aFrame) { seeks.append(aFrame.get()); });
        CHECK(pressAt(band.center()));

        const int pressX = band.left() + band.width() / 3;
        const int moveX = band.left() + (2 * band.width()) / 3;
        core::AbstractCursor cursor;
        CHECK(sendPress(editor, camera, cursor, QPoint(pressX, band.center().y())));
        CHECK(seeks.size() == 1);
        CHECK(seeks.last() == bandScale.frameAt(pressX).get());
        CHECK(sendMove(editor, camera, cursor, QPoint(moveX, band.center().y())));
        CHECK(seeks.size() == 2);
        CHECK(seeks.last() == bandScale.frameAt(moveX).get());
        const int releaseX = band.left() + band.width() / 4;
        CHECK(sendRelease(editor, camera, cursor, QPoint(releaseX, band.center().y())));
        CHECK(seeks.size() == 3);
        CHECK(seeks.last() == bandScale.frameAt(releaseX).get());
        // ...and the seek pushed NOTHING: the frame is the project's, so the stack
        // answers "nothing to undo" and the document never heard of it.
        bool seekUndo = true;
        stack.undo(&seekUndo);
        CHECK(!seekUndo);
        CHECK(changeEvents == 0);
        // ...and the seek ended: a later press in the plot is the plot's, not a seek.
        CHECK(sendPress(editor, camera, cursor, plot.center()));
        CHECK(seeks.size() == 3);
        sendRelease(editor, camera, cursor, plot.center());

        // NO DELEGATE, NO CLAIM: the band is not the panel's to swallow.
        editor.setFrameSeekDelegate(nullptr);
        CHECK(!pressAt(band.center()));
        std::printf(
            "[B] ruler band: (%d,%d %dx%d) above the plot (%d,%d)  press+drag+release sought %d frames "
            "(first %d, last %d at x %d..%d)  commands pushed 0  band unclaimed without a delegate\n",
            band.x(), band.y(), band.width(), band.height(), plot.y(), plot.top(), int(seeks.size()),
            seeks.value(0), seeks.last(), pressX, releaseX
        );
    }

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

    CHECK(key0->data().easing(0) == kAuthored); // unchanged by the outside press+release
    CHECK(key1->data().easing(0) == kOtherKeyEasing);
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
    CHECK(key0->data().easing(0) == kAuthored); // a press alone commits nothing
    CHECK(changeEvents == 0);

    const QPoint step1 = grab + QPoint(4, -4);
    const QPoint step2 = grab + QPoint(8, -8);
    const QPoint step3 = grab + QPoint(12, -12); // right and up: a higher value

    CHECK(sendMove(editor, camera, drag, step1));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));
    CHECK(key0->data().easing(0) != kAuthored); // the FIRST move is what commits

    CHECK(sendMove(editor, camera, drag, step2));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));

    CHECK(sendMove(editor, camera, drag, step3));
    CHECK(editor.ownsCursor(camera, drag));
    CHECK(editor.ownsCursor(camera, outside));

    CHECK(key1->data().easing(0) == kOtherKeyEasing); // the other key's param is untouched
    CHECK(sendRelease(editor, camera, drag, step3));
    CHECK(!editor.ownsCursor(camera, outside)); // the drag is over: the outside point is nobody's again
    CHECK(changeEvents > 0); // the drag told the rest of the application the value changed

    // --- 5: the edit landed where the drag pointed. The released pixel maps
    // back through the SAME scale into the normalized space the handle lives in,
    // and the GRABBED handle is exactly where the pointer is.
    //
    // This fixture has ONE segment, and the press grabbed its handle 0 - the
    // OUTGOING handle of key0, the channel's FIRST key. A first key has no
    // incoming handle, so there is no neighbour segment and NO MIRROR: the
    // other control point of the segment is key1's INCOMING handle and this
    // drag must leave it exactly where it was. (The old claim asserted
    // `x2 == 1-x1` / `y2 == 1-y1`, "the OTHER one at its mirror image about the
    // segment"; that mirrored key1's handle whenever key0's was dragged, which
    // is the defect this contract replaces.)
    const util::Easing::Param dragged = key0->data().easing(0);
    CHECK(dragged != kAuthored);
    CHECK(dragged.type == kAuthored.type); // a handle drag shapes the curve; it never writes the type
    CHECK(dragged.range == kAuthored.range);
    CHECK(dragged.weight == kAuthored.weight);
    // key1's handle - the segment's other control point - is untouched.
    CHECK(dragged.cubicBezier.x2 == kAuthored.cubicBezier.x2);
    CHECK(dragged.cubicBezier.y2 == kAuthored.cubicBezier.y2);

    CHECK(key0->rotate() == 0.0f); // the drag edits the easing, not the key's value
    CHECK(key1->rotate() == 90.0f);
    CHECK(key1->data().easing(0) == kOtherKeyEasing);

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
    CHECK(key0->data().easing(0) == kAuthored);

    bool undoneAgain = true;
    stack.undo(&undoneAgain);
    CHECK(!undoneAgain); // the drag was the whole history

    bool redone = false;
    const QString redoName = stack.redo(&redone);
    CHECK(redone);
    CHECK(redoName == dragName); // one command, undone and redone
    CHECK(key0->data().easing(0) == dragged);

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
    CHECK(key0->data().easing(0) == dragged);   // which has edited nothing
    CHECK(sendRelease(editor, camera, drag, still));
    CHECK(!editor.ownsCursor(camera, outside)); // closed again
    CHECK(key0->data().easing(0) == dragged);    // the press+release changed nothing
    CHECK(changeEvents == eventsBefore);        // and announced nothing

    bool undoneStill = false;
    stack.undo(&undoneStill);
    CHECK(undoneStill);
    CHECK(key0->data().easing(0) == kAuthored); // the history is still exactly the one drag
    bool undoneNone = true;
    stack.undo(&undoneNone);
    CHECK(!undoneNone);
    bool redoneStill = false;
    stack.redo(&redoneStill);
    CHECK(redoneStill);
    CHECK(key0->data().easing(0) == dragged);

    // --- 8: a second drag after an undo pushes exactly ONE MORE command. A press
    // closes whatever edit was open and the undo dropped the stack's modifiable
    // link, so the new drag opens a fresh command instead of rewriting the old
    // one - and its own moves still coalesce into that one command.
    bool undoneSecond = false;
    stack.undo(&undoneSecond);
    CHECK(undoneSecond);
    CHECK(key0->data().easing(0) == kAuthored);

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

    const util::Easing::Param second = key0->data().easing(0);
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
    CHECK(key0->data().easing(0) == kAuthored); // ONE undo restores the pre-drag value
    bool undoneNothing = true;
    stack.undo(&undoneNothing);
    CHECK(!undoneNothing); // and nothing sits behind it: the second drag was one command
    bool redoneSecond = false;
    stack.redo(&redoneSecond);
    CHECK(redoneSecond);
    CHECK(key0->data().easing(0) == second);

    // --- 9: PLOT-BASED ownership for presses, hit-based otherwise. The panel
    // covers the whole viewport, so owning "inside the fullscreen rect" would be
    // owning everything. What it owns is: a LEFT PRESS inside the PLOT (a key
    // selects and drags, Ctrl on a curve's band inserts, anything else marquees),
    // the point under a key or handle, the middle button, and everything while a
    // gesture is in flight.
    {
        // A plain HOVER inside the plot, on no key and no handle: not the graph's,
        // so the viewport tools keep their hover feedback.
        core::AbstractCursor hover;
        const QPoint kEmptyInView(700, 550);
        CHECK(panel.contains(kEmptyInView));
        CHECK(editor.scale().plotRect().contains(kEmptyInView)); // inside the PLOT: a press would be the panel's
        sendMove(editor, camera, hover, kEmptyInView);
        CHECK(!editor.ownsCursor(camera, hover));

        // A key's OWN rect - the rect the renderer draws and the hit-test tests -
        // is the graph's, though: that is exactly the pointer it can act on.
        const QRect key1Rect = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        sendMove(editor, camera, hover, key1Rect.center());
        CHECK(editor.ownsCursor(camera, hover));

        // ...and a point OUTSIDE the plot is nobody's even for a press, which is
        // what keeps the label column and the margins usable by the tools.
        core::AbstractCursor chrome;
        const QPoint kChrome(20, 300);
        CHECK(panel.contains(kChrome));
        CHECK(!editor.scale().plotRect().contains(kChrome));
        QMouseEvent chromePress(
            QEvent::MouseButtonPress, kChrome, kChrome, kChrome, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
        );
        CHECK(chrome.setMousePress(&chromePress, camera));
        CHECK(!editor.ownsCursor(camera, chrome)); // a press outside the plot is the tools'
    }

    // --- 10: the press routing, and the fit that is NOT there. A press that HITS
    // a key both selects it and focuses its channel; a press on EMPTY plot space
    // begins a MARQUEE (see section 20) and leaves the focus alone. Flipping the
    // `hitCurve && validItem` routing would break one half of this either way.
    //
    // The window half is the invariant the reported defect was about: focusing a
    // channel used to REFIT the value axis, so merely clicking a curve zoomed the
    // view. The focus now moves no window at all, so the hand-set window below is
    // byte-identical before the press, before its release, and after it.
    {
        // Start from "no focus" so the press below is a REAL focus change - the
        // case that used to refit.
        editor.clearFocus();
        core::AbstractCursor settle;
        sendMove(editor, camera, settle, QPoint(700, 550));
        CHECK(!editor.hasFocus());

        // A hand-set window, so a refit would be impossible to miss.
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

        // The geometry the user pressed on is UNCHANGED when the release arrives
        // (no window moved under the pointer), and the panel still owns the point.
        const QRect beforeRelease = ctrl::graph::keyRect(editor.scale(), core::Frame(24), 90.0f);
        CHECK(beforeRelease == beforePress);          // the key did NOT slide under the pointer
        CHECK(editor.ownsCursor(camera, hitCur));     // so the release is still the panel's
        CHECK(sendRelease(editor, camera, hitCur, beforeRelease.center())); // closes the armed move

        // NO FIT. The window the user set is still the window - the focus is
        // emphasis, and a click is not a fit request.
        CHECK(editor.scale().valueMin() == -1000.0f);
        CHECK(editor.scale().valueMax() == 1000.0f);

        // Empty plot space, PLAIN press: the press is the PANEL's - a plain press
        // in the plot is where the marquee begins - so it owns the cursor and
        // opens the rectangle. Nothing is written by the press itself; the
        // rectangle is applied on the release.
        const QPoint kEmpty(700, 550);
        core::AbstractCursor emptyCur;
        sendMove(editor, camera, emptyCur, kEmpty);
        CHECK(panel.contains(kEmpty));                 // inside the fullscreen panel...
        CHECK(!editor.ownsCursor(camera, emptyCur));   // ...a hover owns nothing...
        CHECK(sendPress(editor, camera, emptyCur, kEmpty)); // a plain press in the plot
        CHECK(editor.hasMarquee());                    // opens the marquee...
        CHECK(editor.marqueeRect().isEmpty());         // ...with nothing dragged yet
        CHECK(editor.ownsCursor(camera, emptyCur));    // and the panel owns the point
        CHECK(sendRelease(editor, camera, emptyCur, kEmpty)); // the release consumes it
        CHECK(!editor.hasMarquee());
        CHECK(!editor.ownsCursor(camera, emptyCur));   // and the gesture is closed
        // The same point WITH Ctrl is the graph's too - off every band, Ctrl keeps
        // its marquee meaning (only a band turns it into the insert) - and the
        // marquee that never moved selects nothing, which is the "clear the
        // selection" gesture. The sticky FOCUS is untouched by it (empty space is
        // not a command to drop what the user was working on). The modifier goes
        // THROUGH the helper: the helper is what sets it on the editor.
        CHECK(sendPress(editor, camera, emptyCur, kEmpty, Qt::ControlModifier)); // the Ctrl-press is the panel's
        CHECK(editor.hasMarquee());                    // it began the marquee...
        CHECK(editor.marqueeRect().isEmpty());         // ...with nothing dragged yet
        CHECK(sendRelease(editor, camera, emptyCur, kEmpty));
        CHECK(!editor.hasMarquee());
        CHECK(!editor.ownsCursor(camera, emptyCur));   // and the gesture is closed

        core::TimeLineEvent selAfter;
        CHECK(!editor.retrieveSelection(selAfter)); // the click deselected
        CHECK(editor.hasFocus());                   // but the focus is untouched
        CHECK(editor.focusedNode() == &node);
        CHECK(editor.focusedChannel() == Channel(core::TimeKeyType_Rotate, 0));

        // ...and it moved no window either: the same byte-identical hand-set one.
        CHECK(editor.scale().valueMin() == -1000.0f);
        CHECK(editor.scale().valueMax() == 1000.0f);
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

    // --- 14: the FOCUS IS EMPHASIS, NOT A FIT. Two shown channels at disjoint
    // value ranges: with no focus the window is their UNION, and pressing a key of
    // one of them focuses that channel - but the WINDOW DOES NOT MOVE. The
    // narrowing fit this section used to pin is exactly the defect the user
    // reported ("clicking a curve seems to zoom it in"): a click must not zoom.
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

        // The window as the user has it, SNAPSHOTTED: it must survive everything
        // below. (Fitted, so it is a real window and not a collapsed one.)
        const float unionMin = editor.scale().valueMin();
        const float unionMax = editor.scale().valueMax();

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

        // THE POINT OF THE SECTION: the focus changed and the window did NOT, so
        // the union of BOTH channels is still on screen. (An immediate fit would
        // have narrowed it to B and dropped every A value off the axis, which is
        // what the old section asserted - deliberately, and wrongly.)
        CHECK(editor.scale().valueMin() == unionMin);
        CHECK(editor.scale().valueMax() == unionMax);
        CHECK(editor.scale().valueMin() <= 0.0f);  // A is still on the axis...
        CHECK(editor.scale().valueMax() >= 1100.0f); // ...and so is B

        // ...and CLEARING the focus moves no window either. Both directions of the
        // focus are pinned, so a refit reintroduced on either side fails here.
        editor.clearFocus();
        CHECK(!editor.hasFocus());
        CHECK(editor.scale().valueMin() == unionMin);
        CHECK(editor.scale().valueMax() == unionMax);

        // The explicit fit still does what it says, so "no side effect" is not
        // "the fit was removed": fitValueAxis() puts the axis back on the union
        // (and here the union IS the window, so the window is unchanged), while
        // fitAll() is the double-click gesture.
        editor.fitValueAxis();
        CHECK(editor.scale().valueMin() <= 0.0f);
        CHECK(editor.scale().valueMax() >= 1100.0f);
        editor.fitAll();
        CHECK(editor.scale().valueMin() <= 0.0f);
        CHECK(editor.scale().valueMax() >= 1100.0f);

        // A hand-set window survives the fit path that follows it too, as long as
        // the shown SET does not change.
        editor.scale().setValueWindow(-7.0f, 7.0f);
        core::AbstractCursor idle;
        sendMove(editor, camera, idle, QPoint(700, 550));
        CHECK(editor.scale().valueMin() == -7.0f);
        CHECK(editor.scale().valueMax() == 7.0f);
        sendPress(editor, camera, idle, QPoint(700, 550));
        sendRelease(editor, camera, idle, QPoint(700, 550));
        CHECK(editor.scale().valueMin() == -7.0f);
        CHECK(editor.scale().valueMax() == 7.0f);
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
        CHECK(!editor.ownsCursor(camera, idle)); // a hover owns nothing
        CHECK(sendPress(editor, camera, idle, kNoTargets, Qt::ControlModifier)); // the Ctrl-press is the panel's...
        CHECK(editor.hasMarquee());                            // ...it began a marquee over no curves
        CHECK(sendRelease(editor, camera, idle, kNoTargets));  // and the release closes it
        CHECK(!editor.hasMarquee());
        CHECK(!editor.ownsCursor(camera, idle));

        CHECK(editor.curves().isEmpty());
    }

    // --- 16-18: the three key edits - INSERT, MOVE and REMOVE - on one fresh
    // layer, so every assertion below is about one gesture and one undo step.
    //
    // INSERT: a CTRL-press on a curve's BAND - a point on no key and no handle,
    // whose frame the curve covers and whose value is within
    // GraphEditor::kCurveBandRadius of the value the curve evaluates at that frame
    // - inserts the key the curve already passes through. The value written is the
    // curve's own sample at that frame, so no sampled value moves AT THAT FRAME,
    // and the key's payload (the easing it carries, which is the split segment's
    // own) comes from the document as well. The MODIFIER is what keeps the insert
    // from eating the marquee: a PLAIN press on the band begins a marquee like any
    // other empty plot space, so a rubber band dragged across a curve never writes
    // a key nobody asked for. The insert is refused - nothing pushed, the document
    // untouched - on an occupied frame (the key there is the more specific target
    // of the same point, and TimeLine::createPusher would REPLACE it), off the
    // band, and outside the plot.
    //
    // MOVE: pressing a KEY (not a handle) arms a move; a drag moves BOTH its FRAME
    // (through the repository's own TimeLineUtil::MoveFrameOfKey) and its VALUE
    // (through TimeLineUtil::AssignKeyValue), the whole gesture is ONE undo step, a
    // destination another key owns refuses the step, the easing never changes, and
    // Shift pins the gesture to the dominant axis.
    //
    // REMOVE: acts on every key the panel's selection names through
    // TimeLine::createRemover, ONE undo step whose undo restores the SAME key
    // objects with their values and easings; with no key selected, or with a
    // selection naming only frames that hold no key, it pushes nothing.
    {
        core::TestNode editNode;
        const util::Easing::Param kEditEasing = customEasing();
        core::RotateKey* r0 = new core::RotateKey();
        r0->setRotate(0.0f);
        r0->data().easing(0) = kEditEasing;
        setup.push(editNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, r0));
        core::RotateKey* r1 = new core::RotateKey();
        r1->setRotate(90.0f);
        setup.push(editNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, r1));

        const util::Easing::Param kR1Easing = r1->data().easing(0);
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

        // 16b: a point OFF the band, inside the plot, inserts nothing - the band is
        // a real radius, not "anywhere in the plot". A PLAIN press there is the
        // MARQUEE's (a plain plot press marquees anywhere off a band) and a
        // CTRL-press there is the marquee's too: the modifier only means "insert"
        // ON a band. A press that never moves drags no rectangle, so nothing falls
        // inside it and the marquee DESELECTS.
        const QPoint aboveBand(
            editor.scale().pixelX(addFrame),
            editor.scale().pixelY(atEmptyFrame) - (ctrl::GraphEditor::kCurveBandRadius + 2)
        );
        CHECK(editor.scale().plotRect().contains(aboveBand));
        core::AbstractCursor offBandCur;
        sendMove(editor, camera, offBandCur, aboveBand);
        CHECK(!editor.ownsCursor(camera, offBandCur)); // a hover owns nothing
        CHECK(sendPress(editor, camera, offBandCur, aboveBand)); // a PLAIN press here is the panel's...
        CHECK(editor.hasMarquee());                              // ...so the graph begins a marquee
        CHECK(editor.ownsCursor(camera, offBandCur));            // and owns the point
        CHECK(sendRelease(editor, camera, offBandCur, aboveBand)); // which the release closes
        CHECK(!editor.hasMarquee());
        CHECK(sendPress(editor, camera, offBandCur, aboveBand, Qt::ControlModifier)); // the CTRL-press is the panel's too
        CHECK(editor.hasMarquee());                              // ...and it began a marquee
        CHECK(editor.marqueeRect().isEmpty());                   // nothing dragged: nothing to select
        CHECK(sendRelease(editor, camera, offBandCur, aboveBand)); // the release consumes it
        CHECK(!editor.hasMarquee());
        CHECK(editCurve.keys().size() == 2); // and nothing was inserted
        core::TimeLineEvent selOffBand;
        CHECK(!editor.retrieveSelection(selOffBand)); // the empty marquee deselected
        bool undoneOffBand = true;
        stack.undo(&undoneOffBand);
        CHECK(!undoneOffBand);

        // 16c: the CTRL-press on the band inserts the key the curve already passes
        // through.
        const QPoint inBand(editor.scale().pixelX(addFrame), editor.scale().pixelY(atEmptyFrame));
        CHECK(editor.scale().plotRect().contains(inBand));
        sendMove(editor, camera, editCur, inBand);
        CHECK(!editor.ownsCursor(camera, editCur)); // the band is not owned on a HOVER...
        CHECK(sendPress(editor, camera, editCur, inBand, Qt::ControlModifier)); // ...but the Ctrl-press is
        // The insert armed NOTHING - it is a press action, not a gesture - so this
        // release closes no drag and is not consumed. (A press on a KEY does arm a
        // move, which is why the key-press releases elsewhere in this file are
        // consumed; the band is the other case.)
        CHECK(!sendRelease(editor, camera, editCur, inBand));

        // The insert's own API refuses the same two points the press routing does,
        // stated beside the successful one: a point ON a key (the key is the more
        // specific target, and replacing it is a different edit with a different
        // undo story) and a point outside the plot (no band out there). Neither
        // pushes anything, and the document is untouched.
        const QPoint onKey24(editor.scale().pixelX(core::Frame(24)), editor.scale().pixelY(90.0f));
        CHECK(!editor.insertKeyAt(camera, onKey24));
        CHECK(!editor.insertKeyAt(camera, QPoint(5, 5)));
        CHECK(editLine->timeKey(core::TimeKeyType_Rotate, 24) == r1);
        CHECK(editCurve.keys().size() == 3);

        CHECK(editLine->hasTimeKey(core::TimeKeyType_Rotate, addFrame.get()));
        CHECK(editCurve.keys().size() == 3);
        const core::TimeKey* insertedKey = editLine->timeKey(core::TimeKeyType_Rotate, addFrame.get());
        const core::RotateKey* inserted = static_cast<const core::RotateKey*>(insertedKey);
        CHECK(insertedKey != nullptr && inserted != r0 && inserted != r1); // a new key, not a moved one
        CHECK(inserted->rotate() == atEmptyFrame);        // seeded with the curve's own value
        CHECK(inserted->data().easing(0) == kEditEasing);  // the easing of the segment it split
        CHECK(editCurve.sample(addFrame) == atEmptyFrame); // re-read through the document: nothing moved
        CHECK(r0->rotate() == 0.0f && r1->rotate() == 90.0f);
        CHECK(r0->data().easing(0) == kEditEasing);
        CHECK(r1->data().easing(0) == kR1Easing); // the neighbours are untouched

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
        CHECK(inserted->data().easing(0) == kEditEasing);

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
        // This drag is purely HORIZONTAL (every move keeps the press row), so the
        // value axis' delta is exactly zero and the key's value and easing are
        // untouched - which is what the 2-axis rule means at dy == 0. The value
        // axis is exercised on its own in section 21.
        CHECK(inserted->rotate() == atEmptyFrame);       // no vertical movement: the value is untouched
        CHECK(inserted->data().easing(0) == kEditEasing); // ...and so is the easing
        CHECK(r0->rotate() == 0.0f && r1->rotate() == 90.0f);
        CHECK(r1->data().easing(0) == kR1Easing);

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
        CHECK(inserted->data().easing(0) == kEditEasing);
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
        CHECK(inserted->data().easing(0) == kEditEasing);
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
        CHECK(inserted->data().easing(0) == kEditEasing);
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
    // Type_ChangeKeyValue event, one cmnd::Assign<util::Easing::Param> per
    // written component - one per key for the Rotate used here), batched - the per-key timeline helper opens a macro of its own and
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
    // NOT HERE: a multi-key selection made by the panel's own POINTER gestures.
    // Those write the same set - a Shift-press, a marquee and Ctrl+A are the
    // routes sections 20, 23b and 23c pin - and this section drives the action
    // through selectKey, which writes exactly one whole-key target, because what
    // is asserted here is the ACTION's shape (one macro, one notifier, every
    // target in the event) and not the gesture that filled the set. The action
    // handles every target the selection names.
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
        e0->data().easing(0) = authored;
        setup.push(easeNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, e0));
        core::RotateKey* e1 = new core::RotateKey();
        e1->setRotate(70.0f);
        setup.push(easeNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 12, e1));

        core::TimeLine* easeLine = easeNode.timeLine();
        const Channel easeChannel(core::TimeKeyType_Rotate, 0);

        editor.setTargets({ &easeNode });
        editor.scale().setRect(panel); // the rect the cursor path writes
        project.commandStack().clear(); // this section's history is the graph's own edit
        CHECK(e0->data().easing(0).type == util::Easing::Type_Linear); // the authored type
        CHECK(e0->data().easing(0) == authored);                       // and the rest of it
        // What the edit must NOT touch: the second key's whole param, and every
        // field of the edited key's param except the type.
        const util::Easing::Param kSecondKeyEasing = e1->data().easing(0);
        const util::Easing::Range kFirstRange = e0->data().easing(0).range;
        const float kFirstWeight = e0->data().easing(0).weight;
        const util::Easing::CubicBezier kFirstBezier = e0->data().easing(0).cubicBezier;
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
        CHECK(e0->data().easing(0).type == util::Easing::Type_Linear);
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
        CHECK(e0->data().easing(0).type == util::Easing::Type_Linear);
        bool undoneAfterStale = true;
        CHECK(stack.undo(&undoneAfterStale).isEmpty());
        CHECK(!undoneAfterStale);

        // 19c: the change itself - the selected key, ONE undo step.
        editor.selectKey(easeNode, easeChannel, core::Frame(0));
        CHECK(editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        CHECK(e0->data().easing(0).type == util::Easing::Type_Sine);
        CHECK(e0->data().easing(0).range == kFirstRange);       // the key's own fields...
        CHECK(e0->data().easing(0).weight == kFirstWeight);
        CHECK(e0->data().easing(0).cubicBezier == kFirstBezier);
        CHECK(e1->data().easing(0) == kSecondKeyEasing);        // ...and its neighbour's param

        // ONE command: an undo restores the type, a SECOND undo finds nothing
        // left, a redo reapplies it and a second redo finds nothing either.
        bool undoneEasing = false;
        const QString easingCmd = stack.undo(&undoneEasing);
        CHECK(undoneEasing);
        CHECK(!easingCmd.isEmpty());
        CHECK(e0->data().easing(0).type == util::Easing::Type_Linear);
        bool nothingLeftToUndo = true;
        CHECK(stack.undo(&nothingLeftToUndo).isEmpty());
        CHECK(!nothingLeftToUndo);
        bool redoneEasing = false;
        CHECK(!stack.redo(&redoneEasing).isEmpty());
        CHECK(redoneEasing);
        CHECK(e0->data().easing(0).type == util::Easing::Type_Sine);
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
        CHECK(e0->data().easing(0).type == util::Easing::Type_Sine); // 19c's redo
        CHECK(!editor.setSelectedKeysEasing(util::Easing::Type_Sine));
        bool undoneAfterSame = true;
        CHECK(stack.undo(&undoneAfterSame).isEmpty());
        CHECK(!undoneAfterSame);

        // ...and a DIFFERENT one from there is a real edit again, so the refusal
        // above was about the value and not about a broken path.
        CHECK(editor.setSelectedKeysEasing(util::Easing::Type_Bounce));
        CHECK(e0->data().easing(0).type == util::Easing::Type_Bounce);
        bool undoneAfterBounce = false;
        CHECK(!stack.undo(&undoneAfterBounce).isEmpty());
        CHECK(undoneAfterBounce);
        CHECK(e0->data().easing(0).type == util::Easing::Type_Sine);

        // Drop the target before its owner dies, like every other block here:
        // easeNode goes out of scope with this brace.
        editor.setTargets({});
    }

    // --- 20: the SELECTION: one key by a plain press, extending, toggling, and a
    // MARQUEE over empty plot space. The graph's selection is the timeline's own
    // currency (a core::TimeLineEvent with one target per key), so a multi-key
    // selection is expressible and every one of these is observable through
    // retrieveSelection().
    {
        core::TestNode selNode;
        for (int i = 0; i < 4; ++i) {
            core::RotateKey* key = new core::RotateKey();
            key->setRotate(static_cast<float>(i) * 10.0f); // values 0, 10, 20, 30
            setup.push(selNode.timeLine()->createPusher(core::TimeKeyType_Rotate, i * 10, key));
        }
        CHECK(selNode.timeLine()->map(core::TimeKeyType_Rotate).size() == 4);

        editor.setTargets({ &selNode });
        editor.scale().setRect(panel);
        editor.clearSelection();
        project.commandStack().clear(); // no edit below may push anything

        // The four keys' pixels, from the editor's own live scale.
        const Scaler& selScale = editor.scale();
        const QPoint k0(selScale.pixelX(core::Frame(0)), selScale.pixelY(0.0f));
        const QPoint k1(selScale.pixelX(core::Frame(10)), selScale.pixelY(10.0f));
        const QPoint k2(selScale.pixelX(core::Frame(20)), selScale.pixelY(20.0f));
        const QPoint k3(selScale.pixelX(core::Frame(30)), selScale.pixelY(30.0f));
        for (const QPoint& pixel : { k0, k1, k2, k3 }) {
            CHECK(selScale.plotRect().contains(pixel));
        }

        core::AbstractCursor selCur;
        auto selectedFrames = [&editor]() {
            core::TimeLineEvent event;
            QVector<int> frames;
            if (editor.retrieveSelection(event)) {
                for (const core::TimeLineEvent::Target& target : event.targets()) {
                    frames.append(target.pos.index());
                }
            }
            std::sort(frames.begin(), frames.end());
            return frames;
        };
        auto framesAre = [&selectedFrames](std::initializer_list<int> aExpected) {
            const QVector<int> expected(aExpected);
            const QVector<int> actual = selectedFrames();
            return actual == expected;
        };

        // A plain press REPLACES the selection with the one key it hit.
        CHECK(sendPress(editor, camera, selCur, k2));
        CHECK(sendRelease(editor, camera, selCur, k2));
        CHECK(framesAre({ 20 }));

        // Shift-press EXTENDS. Pressing the same key again adds nothing (there is
        // one entry per key, not one per click).
        CHECK(sendPress(editor, camera, selCur, k0, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, selCur, k0, Qt::ShiftModifier));
        CHECK(framesAre({ 0, 20 }));
        CHECK(sendPress(editor, camera, selCur, k0, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, selCur, k0, Qt::ShiftModifier));
        CHECK(framesAre({ 0, 20 })); // no duplicate

        // Ctrl-press TOGGLES: in when out, out when in.
        CHECK(sendPress(editor, camera, selCur, k1, Qt::ControlModifier));
        CHECK(sendRelease(editor, camera, selCur, k1, Qt::ControlModifier));
        CHECK(framesAre({ 0, 10, 20 }));
        CHECK(sendPress(editor, camera, selCur, k1, Qt::ControlModifier));
        CHECK(sendRelease(editor, camera, selCur, k1, Qt::ControlModifier));
        CHECK(framesAre({ 0, 20 }));

        // A plain press on a key replaces again (the modifiers are the ONLY thing
        // that makes a key press anything but a replace).
        CHECK(sendPress(editor, camera, selCur, k3));
        CHECK(sendRelease(editor, camera, selCur, k3));
        CHECK(framesAre({ 30 }));

        // THE MARQUEE. A PLAIN press on empty plot space at the plot's bottom-left
        // corner (no key, and - the curve only being stroked between its first and
        // last key - on no band either), dragged up and right to frame 15 /
        // value 15 and released: every key whose DRAWN square is inside the
        // rectangle is selected, which here is the keys at frames 0 and 10. A
        // plain press is what starts it (the Ctrl the marquee used to require is
        // now the INSERT's alone, see the ownership note at the top and section
        // 16b); the owner check below is the press's own proof.
        const QPoint marqueeOrigin(selScale.plotRect().left() + 2, selScale.plotRect().bottom() - 2);
        const QPoint marqueeEnd(selScale.pixelX(core::Frame(15)), selScale.pixelY(15.0f));
        CHECK(selScale.plotRect().contains(marqueeOrigin));
        CHECK(selScale.plotRect().contains(marqueeEnd));

        CHECK(sendPress(editor, camera, selCur, marqueeOrigin));
        CHECK(editor.ownsCursor(camera, selCur)); // the plain press in the plot is the panel's
        CHECK(framesAre({ 30 })); // the press alone changes nothing...
        CHECK(editor.hasMarquee()); // ...it only opens the rectangle
        CHECK(editor.marqueeRect().isEmpty()); // with nothing dragged yet
        CHECK(sendMove(editor, camera, selCur, marqueeEnd));
        // The LIVE rectangle: normalized (press at the bottom-left, pointer up and
        // right) and clipped to the plot, so it is the box the user sees.
        CHECK(!editor.marqueeRect().isEmpty());
        CHECK(editor.marqueeRect().contains(k0));
        CHECK(editor.marqueeRect().contains(k1));
        CHECK(!editor.marqueeRect().contains(k2)); // frame 20 is past the box's right edge
        CHECK(sendRelease(editor, camera, selCur, marqueeEnd));
        CHECK(!editor.hasMarquee());
        CHECK(framesAre({ 0, 10 })); // the marquee REPLACED the selection
        const int marqueeCount = selectedFrames().size();

        // A SHIFT-marquee ADDS to the selection instead of replacing it: the same
        // kind of box, dragged over the key at frame 20, or-ed with what is
        // already selected. The origin sits on empty space (frame 15's row is far
        // below its curve) and the box is closed before frame 30's key.
        const QPoint addOrigin(selScale.pixelX(core::Frame(15)), selScale.pixelY(5.0f));
        const QPoint addEnd(selScale.pixelX(core::Frame(25)), selScale.plotRect().top() + 2);
        CHECK(selScale.plotRect().contains(addOrigin));
        CHECK(selScale.plotRect().contains(addEnd));
        CHECK(sendPress(editor, camera, selCur, addOrigin, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, selCur, addEnd, Qt::ShiftModifier));
        CHECK(editor.marqueeRect().contains(k2)); // the box is over the frame-20 key
        CHECK(!editor.marqueeRect().contains(k1)); // and not over the frame-10 one
        CHECK(sendRelease(editor, camera, selCur, addEnd, Qt::ShiftModifier));
        CHECK(framesAre({ 0, 10, 20 })); // added, not replaced
        const int shiftMarqueeCount = selectedFrames().size();

        // A marquee that selects nothing replaces with nothing - the "clear the
        // selection" gesture - while a Shift-marquee that selects nothing leaves
        // the selection alone.
        CHECK(sendPress(editor, camera, selCur, addOrigin));
        CHECK(sendRelease(editor, camera, selCur, addOrigin));
        CHECK(selectedFrames().isEmpty());
        CHECK(sendPress(editor, camera, selCur, k1));
        CHECK(sendRelease(editor, camera, selCur, k1));
        CHECK(framesAre({ 10 }));
        CHECK(sendPress(editor, camera, selCur, addOrigin, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, selCur, addOrigin, Qt::ShiftModifier));
        CHECK(framesAre({ 10 })); // still there: a Shift-marquee adds, so an empty one changes nothing

        // A press that SHOOK before it was released is still that CLICK, not the
        // marquee over the few pixels it covered: the design's slop is 4px on both
        // axes (mock-graph.js: `r.w > 4 || r.h > 4`). The grab radius keeps a
        // 4px box from covering a key the press did not already hit, so this pins
        // the GESTURE's meaning - a hand-shake on empty plot stays the click that
        // clears, and would not become a selection gesture if that radius ever
        // shrank under the slop. The box is still PUBLISHED while the button is
        // down (the rectangle the user sees), but the release treats it as the
        // click: the selection is cleared, exactly as the never-moved click above
        // clears it, and nothing is selected.
        const QPoint shake(addOrigin.x() + 3, addOrigin.y() - 2);
        CHECK(selScale.plotRect().contains(shake));
        CHECK(sendPress(editor, camera, selCur, addOrigin));
        CHECK(editor.hasMarquee());
        CHECK(sendMove(editor, camera, selCur, shake));
        CHECK(!editor.marqueeRect().isEmpty()); // the live box is drawn...
        CHECK(editor.marqueeRect().width() <= 5); // ...and it is the few pixels that were dragged
        CHECK(sendRelease(editor, camera, selCur, shake));
        CHECK(!editor.hasMarquee());
        CHECK(selectedFrames().isEmpty()); // the shake is a click: it cleared, it selected nothing

        // ...and with Shift the same shake changes nothing at all: a Shift-press
        // never clears, which is the other half of the click's rule.
        CHECK(sendPress(editor, camera, selCur, k1));
        CHECK(sendRelease(editor, camera, selCur, k1));
        CHECK(framesAre({ 10 }));
        CHECK(sendPress(editor, camera, selCur, addOrigin, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, selCur, shake, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, selCur, shake, Qt::ShiftModifier));
        CHECK(framesAre({ 10 })); // a Shift-click leaves the selection alone, shake or no shake

        std::printf(
            "[S] selection: marquee box (%d,%d)-(%d,%d) over empty plot selects %d keys; a shift-marquee "
            "over the frame-20 key makes %d\n",
            marqueeOrigin.x(), marqueeOrigin.y(), marqueeEnd.x(), marqueeEnd.y(), marqueeCount, shiftMarqueeCount
        );

        // NOTHING ABOVE IS AN EDIT: every press here - the plain replacement, the
        // two extensions, the toggle, the marquees, the shakes and the clicks -
        // pushed no command at all.
        bool undoneAfterSelection = true;
        CHECK(stack.undo(&undoneAfterSelection).isEmpty());
        CHECK(!undoneAfterSelection);

        // REMOVE is multi-key too: two keys selected, ONE undo step that takes both
        // off, and one undo that brings both back with their own values.
        CHECK(sendPress(editor, camera, selCur, k0));
        CHECK(sendRelease(editor, camera, selCur, k0));
        CHECK(sendPress(editor, camera, selCur, k1, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, selCur, k1, Qt::ShiftModifier));
        CHECK(framesAre({ 0, 10 }));
        CHECK(editor.removeSelectedKey());
        CHECK(!selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(!selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 10));
        CHECK(selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 30));
        core::TimeLineEvent afterRemove;
        CHECK(!editor.retrieveSelection(afterRemove)); // the selection went with the keys
        bool undoneTwo = false;
        CHECK(!stack.undo(&undoneTwo).isEmpty()); // ONE entry for both keys
        CHECK(undoneTwo);
        CHECK(selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(selNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 10));
        CHECK(static_cast<core::RotateKey*>(selNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 0))->rotate() == 0.0f);
        CHECK(static_cast<core::RotateKey*>(selNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 10))->rotate() == 10.0f);
        bool undoneTwoAgain = true;
        CHECK(stack.undo(&undoneTwoAgain).isEmpty()); // and nothing sits behind it

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 21: the 2-AXIS key move, and Shift's axis snap. A key drag follows the
    // pointer in BOTH axes - its frame by the pointer's frame delta and its value
    // by the pointer's value delta - and the whole gesture is ONE undo step, so
    // ONE undo must restore both the frame AND the value.
    {
        core::TestNode dragNode;
        core::RotateKey* d0 = new core::RotateKey();
        d0->setRotate(0.0f);
        setup.push(dragNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, d0));
        core::RotateKey* d1 = new core::RotateKey();
        d1->setRotate(90.0f);
        setup.push(dragNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, d1));
        const util::Easing::Param kD0Easing = d0->data().easing(0);

        editor.setTargets({ &dragNode });
        editor.scale().setRect(panel);
        project.commandStack().clear();

        const Scaler& dragMoveScale = editor.scale();
        const QPoint from0(dragMoveScale.pixelX(core::Frame(0)), dragMoveScale.pixelY(0.0f));
        CHECK(ctrl::graph::keyRect(dragMoveScale, core::Frame(0), 0.0f).contains(from0));

        // The destination: frame 5, and the value the pointer's ROW means. The
        // expectation is computed through the SAME live scale the editor drags
        // through, so the assertion is about "the key went where the pointer
        // pointed", not about a formula copied out of the implementation.
        const int toX = dragMoveScale.pixelX(core::Frame(5));
        const int toY = dragMoveScale.pixelY(30.0f);
        const float expectedValue = 0.0f + (dragMoveScale.valueAt(toY) - dragMoveScale.valueAt(from0.y()));
        CHECK(expectedValue > 20.0f && expectedValue < 40.0f); // a real value move, not a rounding

        core::AbstractCursor dragCur;
        CHECK(sendPress(editor, camera, dragCur, from0));
        CHECK(sendMove(editor, camera, dragCur, QPoint(toX, toY)));
        CHECK(sendRelease(editor, camera, dragCur, QPoint(toX, toY)));

        CHECK(!dragNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(dragNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 5));
        CHECK(dragNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 5) == d0); // the SAME key object
        CHECK(d0->frame() == 5);              // the FRAME followed the pointer...
        CHECK(d0->rotate() == expectedValue); // ...and so did the VALUE
        CHECK(d0->data().easing(0) == kD0Easing); // the easing never moves

        // ONE undo restores BOTH axes; a second finds nothing behind it.
        bool undoneBoth = false;
        CHECK(!stack.undo(&undoneBoth).isEmpty());
        CHECK(undoneBoth);
        CHECK(dragNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(!dragNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 5));
        CHECK(d0->rotate() == 0.0f); // the value came back too
        bool undoneBothAgain = true;
        CHECK(stack.undo(&undoneBothAgain).isEmpty());
        CHECK(!undoneBothAgain);
        bool redoneBoth = false;
        CHECK(!stack.redo(&redoneBoth).isEmpty());
        CHECK(redoneBoth);
        CHECK(d0->frame() == 5);
        CHECK(d0->rotate() == expectedValue);

        // SHIFT: the DOMINANT axis only. A drag that is overwhelmingly horizontal
        // moves the frame and leaves the value BIT-identical (not merely close).
        const QPoint from5(dragMoveScale.pixelX(core::Frame(5)), dragMoveScale.pixelY(expectedValue));
        const float beforeValue = d0->rotate();
        const QPoint mostlyRight(dragMoveScale.pixelX(core::Frame(12)), from5.y() + 3);
        CHECK(sendPress(editor, camera, dragCur, from5, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, dragCur, mostlyRight, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, dragCur, mostlyRight, Qt::ShiftModifier));
        CHECK(d0->frame() == 12);            // the dominant axis moved
        CHECK(d0->rotate() == beforeValue);  // the other one did not

        // ...and a drag that is overwhelmingly vertical moves the value and leaves
        // the FRAME exactly where it was.
        const QPoint from12(dragMoveScale.pixelX(core::Frame(12)), dragMoveScale.pixelY(beforeValue));
        const float beforeDownValue = d0->rotate();
        const QPoint mostlyDown(from12.x() + 3, dragMoveScale.pixelY(beforeValue - 25.0f));
        CHECK(sendPress(editor, camera, dragCur, from12, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, dragCur, mostlyDown, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, dragCur, mostlyDown, Qt::ShiftModifier));
        CHECK(d0->frame() == 12);                        // the frame is pinned
        CHECK(d0->rotate() < beforeDownValue);           // and the value followed the pointer down
        CHECK(d0->rotate() == beforeDownValue + (dragMoveScale.valueAt(mostlyDown.y())
                                                - dragMoveScale.valueAt(from12.y())));
        const float afterVerticalValue = d0->rotate();

        // A drag that crosses NO axis (no frame boundary, no value row) pushes
        // nothing at all: the history still holds exactly the three real gestures
        // above, so three undos exhaust it and a fourth finds nothing - which is
        // the observable form of "the empty press wrote no history entry".
        const QPoint still(dragMoveScale.pixelX(core::Frame(12)), dragMoveScale.pixelY(d0->rotate()));
        CHECK(sendPress(editor, camera, dragCur, still));
        CHECK(!sendMove(editor, camera, dragCur, still)); // nothing moved: nothing to rewrite
        CHECK(sendRelease(editor, camera, dragCur, still));
        for (int i = 0; i < 3; ++i) {
            bool undoneReal = false;
            CHECK(!stack.undo(&undoneReal).isEmpty()); // rewind one of the three real gestures
            CHECK(undoneReal);
        }
        bool undoneFourth = true;
        CHECK(stack.undo(&undoneFourth).isEmpty()); // and nothing behind them
        CHECK(!undoneFourth);

        std::printf(
            "[M] 2-axis drag: frame 0 -> 5 and value 0 -> %.4f in ONE undo step; shift-horizontal moved "
            "frame 5 -> 12 with the value bit-identical (%.4f); shift-vertical pinned the frame at 12 and "
            "moved the value %.4f -> %.4f\n",
            expectedValue, beforeValue, beforeDownValue, afterVerticalValue
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 22: the MIDDLE button pans the GRAPH. The middle drag moves the panel's
    // window on both axes and never a canvas camera: the panel's page hands the event
    // to this editor (gui::GraphPanelWidget) and the viewport no longer has a panel in
    // it - ctrl::MainDisplayWidget has nothing to withhold from ctrl::CanvasMover any
    // more, because the middle drag over the dock page never reaches the viewport. A
    // pan is a pure pixel offset: the window SPAN is untouched, so a pan cannot zoom.
    {
        core::TestNode panNode;
        core::RotateKey* p0 = new core::RotateKey();
        p0->setRotate(0.0f);
        setup.push(panNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, p0));
        core::RotateKey* p1 = new core::RotateKey();
        p1->setRotate(90.0f);
        setup.push(panNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, p1));

        editor.setTargets({ &panNode });
        editor.scale().setRect(panel);
        project.commandStack().clear();

        const Scaler& panScale = editor.scale();
        const QPoint panStart = panScale.plotRect().center();
        const QPoint panDelta(40, 25);

        const int beforeFramePixel = panScale.pixelX(core::Frame(0));
        const double beforeLeftEdge = panScale.frameAtPixel(static_cast<double>(panScale.plotRect().left()));
        const double beforePpf = panScale.frameScale().pixelsPerFrame();
        const float beforePanMin = panScale.valueMin();
        const float beforePanMax = panScale.valueMax();

        // The middle press is the panel's even on a point with no item under it,
        // and even OUTSIDE the plot: while the panel is up the whole viewport's
        // middle button is the graph's.
        core::AbstractCursor panCur;
        QMouseEvent outsideMiddlePress(
            QEvent::MouseButtonPress, QPoint(5, 5), QPoint(5, 5), QPoint(5, 5), Qt::MiddleButton, Qt::MiddleButton,
            Qt::NoModifier
        );
        CHECK(panCur.setMousePress(&outsideMiddlePress, camera));
        CHECK(editor.ownsCursor(camera, panCur));
        CHECK(panCur.setMouseRelease(&outsideMiddlePress, camera)); // drop it again (no pan ran)

        CHECK(sendMiddlePress(editor, camera, panCur, panStart));
        CHECK(editor.isPanning());
        CHECK(editor.ownsCursor(camera, panCur)); // the gesture owns the cursor wherever it goes
        CHECK(sendMiddleMove(editor, camera, panCur, panStart + panDelta));
        CHECK(sendMiddleRelease(editor, camera, panCur, panStart + panDelta));
        CHECK(!editor.isPanning());

        // The frame axis moved by EXACTLY the pointer's own x delta (panFrame adds
        // a pixel offset), and the value window by the rows it shifted: the span is
        // untouched, so this was a pan and not a zoom.
        //
        // Read PLOT-RELATIVE, because this very pan can move the plot: the value
        // labels are MEASURED (graph::Scaler::axisLabelWidth), so panning the value
        // window onto wider numbers widens the label column and moves
        // plotRect().left() with it - an absolute pixel comparison would then be
        // part pan and part label column (measured: this pan moved plot.left() by
        // 1px, so px(0) moved 41 where the pan owes 40). What the pan owes is the
        // WINDOW: the frame at the plot's left edge moves by the pointer's x delta
        // worth of frames, and the rung does not change.
        const double panPpf = panScale.frameScale().pixelsPerFrame();
        const double leftEdgeAfter = panScale.frameAtPixel(static_cast<double>(panScale.plotRect().left()));
        CHECK(std::fabs((leftEdgeAfter - beforeLeftEdge) * panPpf + panDelta.x()) < 1e-6);
        CHECK(panScale.frameScale().pixelsPerFrame() == beforePpf);
        const float afterSpan = panScale.valueMax() - panScale.valueMin();
        const float beforeSpan = beforePanMax - beforePanMin;
        CHECK(std::fabs(afterSpan - beforeSpan) < beforeSpan * 1e-5f); // the span survived
        CHECK(panScale.valueMin() > beforePanMin); // dragging DOWN reveals higher values
        CHECK(!editor.hasFocus());                 // a pan selects nothing...

        // ...and it is not an EDIT: no command, no notification, no changed key.
        CHECK(p0->rotate() == 0.0f);
        CHECK(p1->rotate() == 90.0f);
        bool undoneAfterPan = true;
        CHECK(stack.undo(&undoneAfterPan).isEmpty());
        CHECK(!undoneAfterPan);

        // The pan is NOT a fit either: a cursor pass after it leaves the panned
        // window exactly where the pan left it.
        const float pannedMin = panScale.valueMin();
        const float pannedMax = panScale.valueMax();
        const int pannedFramePixel = panScale.pixelX(core::Frame(0));
        core::AbstractCursor idleAfterPan;
        sendMove(editor, camera, idleAfterPan, panScale.plotRect().center());
        CHECK(editor.scale().valueMin() == pannedMin);
        CHECK(editor.scale().valueMax() == pannedMax);
        CHECK(editor.scale().pixelX(core::Frame(0)) == pannedFramePixel);

        std::printf(
            "[P] middle-button pan: (%d,%d) px moved the frame WINDOW by the x delta (frame at the plot's "
            "left edge %.4f -> %.4f at %.4f px/frame; absolute px(0) %d -> %d, the rest of that being the "
            "measured label column) and the "
            "value window %+.4f..%+.4f -> %+.4f..%+.4f (span %.6f vs %.6f, a pan and not a zoom); no command pushed\n",
            panDelta.x(), panDelta.y(), beforeLeftEdge, leftEdgeAfter, beforePpf, beforeFramePixel,
            pannedFramePixel, beforePanMin, beforePanMax, pannedMin, pannedMax, beforePanMax - beforePanMin,
            pannedMax - pannedMin
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 23: THE TIMELINE SEAM - ONE selection and ONE frame window, on a real
    // ctrl::TimeLineEditor.
    //
    // The panel is a SECOND VIEW of the timeline, not a tool beside it, and this
    // section pins both halves of what that means in ctrl:
    //
    //   * SELECTION: GraphEditor::setSelectionSource points the panel at the
    //     timeline's OWN live set (ctrl::TimeLineEditor::selection()), so a key
    //     selected in the panel is reported by the timeline's own accessor
    //     (retrieveSelectionTargets), carries the focus the timeline's renderer
    //     highlights (TimeKey::isFocused, which time_Renderer reads), and a
    //     selection made through the timeline's own path is what the panel reads.
    //     The round trip is asserted in BOTH directions, and so is the instance
    //     identity - two sets that must agree are two sets that eventually do not.
    //   * FRAME WINDOW: setFrameWindow pushes the frames the timeline is showing
    //     (TimeLineWidget::visibleFrameWindow) in as VALUES, and while it is set the
    //     panel does not fit, zoom or pan its own X; the wheel's frame half and a
    //     middle-drag pan go to the delegates, which the application wires to the
    //     timeline's own scrollFramesBy/zoomFramesAbout. The pushed window is mapped
    //     ACROSS THE PANEL, so a frame's x here is its x in the timeline: the plot's
    //     own left and right strips are chrome (the value column and the bar lane),
    //     not a margin the mapping subtracts.
    //
    // The timeline is a REAL ctrl::TimeLineEditor: the seam's whole point is that
    // the panel writes the set the timeline highlights and shows the window the
    // timeline is looking at, so a stand-in would only prove two stand-ins agree.
    // Sections 0-22 run with NO timeline attached, which is the other half of the
    // contract: there the panel owns its frame axis and its selection, and fits,
    // zooms and pans X exactly as it always has.
    {
        ctrl::TimeLineEditor timeline;
        timeline.setProject(&project);
        timeline.setFrameRange(util::Range(1, 60));

        core::TestNode seamNode;
        core::RotateKey* s0 = new core::RotateKey();
        s0->setRotate(0.0f);
        setup.push(seamNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 2, s0));
        core::RotateKey* s1 = new core::RotateKey();
        s1->setRotate(90.0f);
        setup.push(seamNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 6, s1));
        core::TimeLine* seamLine = seamNode.timeLine();
        const Channel seamChannel(core::TimeKeyType_Rotate, 0);
        CHECK(seamLine->map(core::TimeKeyType_Rotate).size() == 2);

        editor.setTargets({ &seamNode });
        editor.scale().setRect(panel);
        project.commandStack().clear();

        // --- (a) ATTACH: the panel's selection IS the timeline's live set. ---
        editor.setSelectionSource(&timeline.selection());
        CHECK(&editor.selection() == &timeline.selection()); // ONE instance, not two

        // --- (b) PANEL -> TIMELINE. A plain press on a key in the panel... ---
        const QPoint s0Pixel(editor.scale().pixelX(core::Frame(2)), editor.scale().pixelY(0.0f));
        CHECK(editor.scale().plotRect().contains(s0Pixel)); // the precondition of the press
        core::AbstractCursor seamCur;
        sendMove(editor, camera, seamCur, s0Pixel);
        CHECK(editor.ownsCursor(camera, seamCur));
        CHECK(sendPress(editor, camera, seamCur, s0Pixel));
        CHECK(sendRelease(editor, camera, seamCur, s0Pixel)); // closes the armed move (no move: no command)
        core::TimeLineEvent selected;
        CHECK(editor.retrieveSelection(selected));
        CHECK(selected.targets().size() == 1);
        CHECK(selected.targets()[0].node == &seamNode);
        CHECK(selected.targets()[0].pos.index() == 2);

        // ...is what the TIMELINE reports as its own selection: the panel wrote the
        // very set the timeline's accessor hands out, so a consumer on the timeline
        // side sees exactly the keys the panel selected.
        core::TimeLineEvent fromTimeline;
        CHECK(timeline.retrieveSelectionTargets(fromTimeline));
        CHECK(fromTimeline.targets().size() == 1);
        CHECK(fromTimeline.targets()[0].node == &seamNode);
        CHECK(fromTimeline.targets()[0].pos.type() == core::TimeKeyType_Rotate);
        CHECK(fromTimeline.targets()[0].pos.index() == 2);
        // ...and the HIGHLIGHT the timeline's renderer reads is that same write: the
        // key the panel selected carries the focus time::Selection rebuilt for it
        // (time_Renderer's key body reads exactly TimeKey::isFocused), and the other
        // key does not.
        CHECK(seamLine->timeKey(core::TimeKeyType_Rotate, 2)->isFocused());
        CHECK(!seamLine->timeKey(core::TimeKeyType_Rotate, 6)->isFocused());

        // --- (c) TIMELINE -> PANEL. A selection the timeline makes through its own
        // public path (the "creation selects its output" rule) is what the panel
        // reads and acts on, and the panel's own mutators write the same set. ---
        core::TimeLineEvent created;
        created.setType(core::TimeLineEvent::Type_PushKey);
        created.pushTarget(seamNode, core::TimeKeyType_Rotate, 6);
        timeline.selectCreatedKeys(created);
        core::TimeLineEvent panelSees;
        CHECK(editor.retrieveSelection(panelSees));
        CHECK(panelSees.targets().size() == 1);
        CHECK(panelSees.targets()[0].pos.index() == 6);
        CHECK(seamLine->timeKey(core::TimeKeyType_Rotate, 6)->isFocused());
        CHECK(!seamLine->timeKey(core::TimeKeyType_Rotate, 2)->isFocused());
        editor.modifySelection(seamNode, seamChannel, core::Frame(2), true); // Shift-extend, in the panel
        fromTimeline = core::TimeLineEvent();
        CHECK(timeline.retrieveSelectionTargets(fromTimeline));
        CHECK(fromTimeline.targets().size() == 2); // the timeline's set grew with it

        // DETACHED, the panel is its own again: a write there is no longer the
        // timeline's selection.
        editor.setSelectionSource(nullptr);
        CHECK(&editor.selection() != &timeline.selection());
        editor.clearSelection();
        fromTimeline = core::TimeLineEvent();
        CHECK(timeline.retrieveSelectionTargets(fromTimeline)); // the timeline's set is untouched
        CHECK(fromTimeline.targets().size() == 2);
        editor.setSelectionSource(&timeline.selection()); // re-attach for the window half

        // --- (d0) THE PUSH IS NOT THE MAPPING, and that split is load-bearing: the gui
        // pushes the window the moment the timeline's view or the driver changes,
        // which in the running application is BEFORE the viewport has been laid out -
        // so the panel's plot rect is still degenerate and the mapping has no width to
        // choose a rung from (MEASURED in the running app: the first pushes arrive with
        // a plot of width -79). The editor's own rect is otherwise written only on the
        // cursor path, which the panel reaches only when it OWNS an event - so a window
        // pushed early would never be applied at all and the panel would keep the frame
        // axis it was born with. syncFrameWindowFor() is the second step, called from
        // the path that is about to DRAW the panel. Both steps are pinned here, in the
        // application's own order.
        editor.setFrameWindow(50.0, 150.0);
        const int beforeDegeneratePush = editor.scale().pixelX(core::Frame(0));
        editor.scale().setRect(QRect()); // the viewport has no layout yet
        editor.setFrameWindow(50.0, 150.0);
        CHECK(editor.hasTimelineFrameAxis());          // the window is REMEMBERED...
        CHECK(editor.scale().plotRect().width() <= 0); // ...but there is nothing to map into
        CHECK(editor.scale().pixelX(core::Frame(0)) == beforeDegeneratePush); // so nothing moved

        // ...and the gui derives it for the geometry the panel will be drawn with, which
        // is the same size the drawing layer builds its model with.
        editor.syncFrameWindowFor(panel.size());
        CHECK(editor.scale().plotRect().width() > 0);
        // The window spans the PANEL, so its first frame is the panel's first column -
        // the timeline's own, which is left of the plot (the value column).
        CHECK(std::abs(editor.scale().pixelX(core::Frame(50)) - panel.left()) <= 1);

        // --- (d) THE SHARED FRAME WINDOW: the gui pushes the timeline's visible window
        // (TimeLineWidget::visibleFrameWindow, FRACTIONAL frames) as VALUES and the panel
        // maps it into its plot exactly, then never writes its own X again. ---
        const double pushedLo = 50.0;
        const double pushedHi = 150.0;
        editor.setFrameWindow(pushedLo, pushedHi);
        CHECK(editor.hasTimelineFrameAxis());

        const QRect sharedPanel = editor.scale().rect();
        const QRect sharedPlot = editor.scale().plotRect();
        const double sharedPpf = editor.scale().framePpf();
        // THE IDENTITY: the panel covers the timeline's own columns, so the graph's
        // pixel for a frame IS the timeline's. Stated as the formula both views
        // evaluate, then checked across the window and at BOTH edges:
        //
        //   graphPixelX(frame) == panel.left() + (frame - lo) * panel.width() / (hi - lo)
        const auto timelinePixelX = [&sharedPanel, pushedLo, sharedPpf](double aFrame) {
            return static_cast<double>(sharedPanel.left()) + (aFrame - pushedLo) * sharedPpf;
        };
        CHECK(std::fabs(sharedPpf - static_cast<double>(sharedPanel.width()) / (pushedHi - pushedLo)) < 1e-9);
        CHECK(editor.scale().pixelX(core::Frame(50)) == sharedPanel.left());
        CHECK(editor.scale().pixelX(core::Frame(150)) == sharedPanel.left() + sharedPanel.width());
        for (const double frame : { pushedLo, pushedLo + 12.5, (pushedLo + pushedHi) / 2.0, pushedHi - 0.5, pushedHi })
            CHECK(std::fabs(editor.scale().pixelXOfFrame(frame) - timelinePixelX(frame)) <= 1e-9);
        // The plot is INSET inside that window by the value column and the bar
        // reserve, so the frames at the panel's edges are the timeline's columns but
        // not the plot's - no margin was ever subtracted from the window itself.
        CHECK(editor.scale().pixelX(core::Frame(50)) < sharedPlot.left());
        CHECK(editor.scale().pixelX(core::Frame(150)) > sharedPlot.right());
        CHECK(editor.scale().frameAt(sharedPlot.left()) > pushedLo);
        CHECK(editor.scale().frameAt(sharedPlot.right()) < pushedHi);

        // --- (d2) THE MAPPING IS AFFINE IN FRAMES, so neither horizontal gesture can
        // snap. A push half a frame further along (what the timeline's own content-pixel
        // viewport produces when it pans by half a frame) moves every frame's pixel by
        // exactly half a frame's worth - not by a whole frame, and not by nothing. ---
        {
            const int xBefore = editor.scale().pixelX(core::Frame(100));
            const double subFrame = 0.5;
            editor.setFrameWindow(pushedLo + subFrame, pushedHi + subFrame);
            const double expected = xBefore - subFrame * sharedPpf;
            CHECK(std::abs(static_cast<double>(editor.scale().pixelX(core::Frame(100))) - expected) <= 1.0);
            CHECK(editor.scale().pixelX(core::Frame(100)) != xBefore); // a half frame is not a no-op
            editor.setFrameWindow(pushedLo, pushedHi);
        }

        // --- (d3) A ZOOM KEEPS ITS ANCHOR. The timeline zooms about the frame under the
        // cursor in its own content pixels, so the window it pushes back keeps that
        // frame at the same FRACTION of the window; the panel's mapping therefore puts
        // it back on the same pixel, with no second anchoring step. (Measured on the
        // anchor's own sub-pixel position: the anchor is not a whole frame, and a whole
        // frame below it would move by up to a frame's worth of pixels under the zoom,
        // which is exactly the drift this pins out.) ---
        {
            const double anchorPixel = sharedPlot.center().x();
            const double anchorFrame = editor.scale().frameAtPixel(anchorPixel);
            const double xBefore = editor.scale().pixelXOfFrame(anchorFrame);
            // The window the same anchor at the same fraction implies, at half the span:
            const double lo = anchorFrame - (anchorFrame - pushedLo) / 2.0;
            const double hi = anchorFrame + (pushedHi - anchorFrame) / 2.0;
            editor.setFrameWindow(lo, hi);
            CHECK(editor.scale().framePpf() > sharedPpf); // it really zoomed in...
            CHECK(std::fabs(editor.scale().pixelXOfFrame(anchorFrame) - xBefore) <= 0.5); // ...about the anchor
            editor.setFrameWindow(pushedLo, pushedHi);
        }

        // NOTHING REFITS X WHILE THE TIMELINE OWNS IT: a cursor pass, a change of the
        // shown curves (which IS the value fit's trigger), the panel's own
        // double-click fit, the value wheel and the value pan all leave the pushed
        // window exactly where the timeline put it.
        //
        // The measure is the FRAME AT THE PLOT'S LEFT EDGE, not a pixel: the value
        // labels are measured (graph::Scaler::axisLabelWidth), so a change of the
        // VALUE window can move plotRect().left() - and a pixel comparison would
        // then be about the label column, not about X. "The frame the plot starts
        // at" is the same number either way, and a refit, a zoom or a self-pan would
        // move it by frames, not by the pixel the origin write rounds to.
        auto leftEdgeFrame = [&editor]() {
            const QRect livePlot = editor.scale().plotRect();
            return editor.scale().frameAtPixel(static_cast<double>(livePlot.left()));
        };
        const double windowLeftFrame = leftEdgeFrame();
        // The baseline is the mapping's own consequence: the plot starts to the RIGHT
        // of the window's first column (the value column is between them), so the
        // plot's left edge shows that offset's worth of LATER frames:
        // (windowLeftFrame - lo) * ppf == plot.left - panel.left. A margin would show
        // up here as a different constant, which is why this is stated as the offset
        // rather than as "== lo".
        CHECK(
            std::abs(
                (windowLeftFrame - pushedLo) * sharedPpf
                - (sharedPlot.left() - sharedPanel.left())
            ) <= 1.5
        );

        core::AbstractCursor idleSeam;
        sendMove(editor, camera, idleSeam, sharedPlot.center());
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);
        editor.setTargets({});
        editor.setTargets({ &seamNode });
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);
        CHECK(std::fabs(editor.scale().framePpf() - sharedPpf) < 1e-9);
        editor.fitAll();
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);
        CHECK(std::fabs(editor.scale().framePpf() - sharedPpf) < 1e-9);

        // --- (d2) THE WINDOW'S EDGES ARE THE PANEL'S COLUMNS, for ANY span. A
        // narrower window (the timeline zoomed in) keeps the same identity: its first
        // frame is the panel's first column and its last is one past the panel's last.
        // There is no shared-window edge margin any more - that existed because the
        // old mapping fitted the PLOT, and the pixel-for-pixel identity with the
        // timeline is what replaced it.
        {
            const double narrowHi = pushedLo + 10.0;
            editor.setFrameWindow(pushedLo, narrowHi);
            const QRect narrowPanel = editor.scale().rect();
            const double narrowPpf = editor.scale().framePpf();
            CHECK(std::fabs(narrowPpf - static_cast<double>(narrowPanel.width()) / (narrowHi - pushedLo)) < 1e-9);
            CHECK(editor.scale().pixelX(core::Frame(int(pushedLo))) == narrowPanel.left());
            CHECK(
                editor.scale().pixelX(core::Frame(int(narrowHi)))
                == narrowPanel.left() + narrowPanel.width()
            );
            editor.setFrameWindow(pushedLo, pushedHi);
            CHECK(std::fabs(editor.scale().framePpf() - sharedPpf) < 1e-9);
        }

        // The section's own two keys stay the selection the gesture half below reads,
        // and the target and window it works on are set back explicitly.
        editor.setTargets({ &seamNode });
        editor.setFrameWindow(pushedLo, pushedHi);
        CHECK(std::fabs(editor.scale().framePpf() - sharedPpf) < 1e-9);
        {
            core::TimeLineEvent both;
            both.pushTarget(seamNode, core::TimeKeyType_Rotate, 2);
            both.pushTarget(seamNode, core::TimeKeyType_Rotate, 6);
            timeline.selectCreatedKeys(both);
        }

        // --- (e) THE HORIZONTAL GESTURES GO TO THE TIMELINE. ---
        double scrolledFrames = 0.0;
        int scrollCalls = 0;
        int zoomedDelta = 0;
        double zoomedAnchor = 0.0;
        int zoomCalls = 0;
        editor.setFrameGestureDelegates(
            [&scrolledFrames, &scrollCalls](double aDelta) { scrolledFrames += aDelta; ++scrollCalls; },
            [&zoomedDelta, &zoomedAnchor, &zoomCalls](int aDelta, double aAnchor) {
                zoomedDelta = aDelta;
                zoomedAnchor = aAnchor;
                ++zoomCalls;
            }
        );

        // SHIFT IS THE FRAME-AXIS-ONLY ZOOM (it is what Ctrl+Shift was in the
        // 2026-10-19 remap, which made Shift a SCROLL and is undone here). The notch
        // has to reach the timeline's zoomFramesAbout through the ZOOM delegate - in
        // the unit both views share, the frame under the panel's cursor, SUB-PIXEL so
        // the timeline can put it back under the cursor instead of rounding it - and
        // the SCROLL delegate must NOT hear it: a scroll here is a window move with no
        // rate change, which is the one thing this notch is not.
        const double anchorFrameAtCursor = editor.scale().frameAtPixel(sharedPlot.center().x());
        const float beforeShiftSeamMin = editor.scale().valueMin();
        const float beforeShiftSeamMax = editor.scale().valueMax();
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::ShiftModifier, false));
        CHECK(zoomCalls == 1);
        CHECK(scrollCalls == 0); // the SCROLL delegate did not hear it
        CHECK(zoomedDelta == -120);
        CHECK(std::fabs(zoomedAnchor - anchorFrameAtCursor) < 1e-9);
        CHECK(zoomedAnchor != std::floor(zoomedAnchor)); // a real sub-pixel anchor, not a coincidence
        CHECK(editor.scale().valueMin() == beforeShiftSeamMin); // the frame-only zoom moved no value
        CHECK(editor.scale().valueMax() == beforeShiftSeamMax);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0); // the panel's X stayed put

        // Ctrl+Shift is the OTHER half of the same table: BOTH axes, so it reaches the
        // ZOOM delegate like plain does and moves the value axis as well - and the
        // scroll delegate still hears nothing, because neither form scrolls.
        const int scrollCallsBeforeCtrlShift = scrollCalls;
        const double anchorFrameAtCtrlShift = editor.scale().frameAtPixel(sharedPlot.center().x());
        const float beforeCtrlShiftSeamMin = editor.scale().valueMin();
        const float beforeCtrlShiftSeamMax = editor.scale().valueMax();
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::ControlModifier | Qt::ShiftModifier, false));
        CHECK(zoomCalls == 2);
        CHECK(zoomedDelta == -120);
        CHECK(std::fabs(zoomedAnchor - anchorFrameAtCtrlShift) < 1e-9);
        CHECK(scrollCalls == scrollCallsBeforeCtrlShift); // the scroll delegate did not hear it
        CHECK(editor.scale().valueMax() - editor.scale().valueMin() < beforeCtrlShiftSeamMax - beforeCtrlShiftSeamMin);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);

        // The plain wheel zooms BOTH axes too: the frame half still goes to the
        // timeline, while the VALUE half - the axis the timeline does not show - is the
        // panel's own and really moves.
        const float beforeSeamValueSpan = editor.scale().valueMax() - editor.scale().valueMin();
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::NoModifier, false));
        CHECK(zoomCalls == 3);
        CHECK(zoomedDelta == -120);
        CHECK(scrollCalls == scrollCallsBeforeCtrlShift); // a zoom is not a scroll
        CHECK(editor.scale().valueMax() - editor.scale().valueMin() < beforeSeamValueSpan);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);

        // Ctrl = the value axis alone: neither frame delegate hears it.
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::ControlModifier, false));
        CHECK(zoomCalls == 3); // (the count after the plain notch above: Ctrl adds nothing)
        CHECK(scrollCalls == scrollCallsBeforeCtrlShift);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);

        // ...and the INVERTED setting is applied BEFORE the delegation, so the
        // timeline is handed a notch in the panel's own sense and the panel's two
        // axes keep moving together: with the setting on, the same wheel-away notch
        // that arrived as -120 above arrives as +120 here - still a ZOOM, on the frame
        // axis alone, and still nothing for the scroll delegate. (The timeline's own
        // invert setting is its own wheel's; that is the gesture
        // gui::TimeLineEditorWidget::updateWheel drives, not this one.)
        const int scrollCallsBeforeInvert = scrollCalls;
        const float beforeInvertMin = editor.scale().valueMin();
        const float beforeInvertMax = editor.scale().valueMax();
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::ShiftModifier, true));
        CHECK(zoomCalls == 4);
        CHECK(zoomedDelta == 120); // the delegated notch, in the panel's own sense
        CHECK(scrollCalls == scrollCallsBeforeInvert); // still not a scroll
        CHECK(editor.scale().valueMin() == beforeInvertMin); // and still the frame axis ALONE
        CHECK(editor.scale().valueMax() == beforeInvertMax);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);
        const float beforeInvertSpan = editor.scale().valueMax() - editor.scale().valueMin();
        CHECK(editor.updateWheel(camera, sharedPlot.center(), -120, Qt::NoModifier, true));
        CHECK(zoomCalls == 5);
        CHECK(zoomedDelta == 120); // the plain wheel's frame half too
        // The value half moves the way the canvas moves on that same notch with the
        // setting on: a notch TOWARD the user is the zoom OUT when the wheel is
        // inverted (gui::CanvasMover scales by 0.9 on it), so the span grows - the
        // two halves agreeing is the point.
        CHECK(editor.scale().valueMax() - editor.scale().valueMin() > beforeInvertSpan);

        // The MIDDLE drag: its horizontal half becomes a FRAME delta handed to the
        // timeline (dragging right reveals EARLIER frames, so the delta is negative
        // and its size is the pixel delta over the panel's own rung - frames are the
        // one unit both views share), and its vertical half stays the panel's.
        const float beforeSeamPanMin = editor.scale().valueMin();
        const float beforeSeamPanMax = editor.scale().valueMax();
        const QPoint seamPanStart = sharedPlot.center();
        const QPoint seamPanDelta(40, 25);
        const double scrolledBeforePan = scrolledFrames;
        core::AbstractCursor seamPanCur;
        CHECK(sendMiddlePress(editor, camera, seamPanCur, seamPanStart));
        CHECK(sendMiddleMove(editor, camera, seamPanCur, seamPanStart + seamPanDelta));
        CHECK(sendMiddleRelease(editor, camera, seamPanCur, seamPanStart + seamPanDelta));
        CHECK(zoomCalls == 5); // a pan is not a zoom
        CHECK(std::fabs((scrolledFrames - scrolledBeforePan) + 40.0 / static_cast<double>(sharedPpf)) < 1e-9);
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0); // the panel's X still did not move
        const float afterSeamPanSpan = editor.scale().valueMax() - editor.scale().valueMin();
        const float beforeSeamPanSpan = beforeSeamPanMax - beforeSeamPanMin;
        CHECK(std::fabs(afterSeamPanSpan - beforeSeamPanSpan) < beforeSeamPanSpan * 1e-5f); // a pan, not a zoom
        CHECK(editor.scale().valueMin() > beforeSeamPanMin); // dragging DOWN reveals higher values

        // THE HORIZONTAL SCROLL BAR, on the SHARED axis: a thumb drag moves the frame
        // window by exactly the pixels dragged, and it does it through the SAME scroll
        // delegate the middle drag uses - the panel's own mapping is not written at all,
        // so the timeline's window is the one that moves and both views stay one window.
        float seamValueLo = 0.0f;
        float seamValueHi = 0.0f;
        CHECK(editor.shownValueExtent(seamValueLo, seamValueHi));
        const util::Range seamFrames = editor.frameRange();
        const Scaler::ScrollBars seamBars =
            editor.scale().scrollBars(seamFrames.min(), seamFrames.max(), seamValueLo, seamValueHi);
        CHECK(!seamBars.isEmpty() && !seamBars.hThumb.isEmpty());
        const QPoint seamThumbGrab = seamBars.hThumb.center();
        CHECK(seamThumbGrab.y() > sharedPlot.bottom()); // the grab is on the bar, outside the plot
        const double seamPpfAtBarDrag = editor.scale().framePpf();
        const double seamOriginBeforeBar = editor.scale().pixelXOfFrame(0.0);
        const double scrolledBeforeBar = scrolledFrames;
        const int scrollCallsBeforeBar = scrollCalls;
        const QPoint seamBarDelta(30, 0);
        core::AbstractCursor seamBarCur;
        CHECK(sendPress(editor, camera, seamBarCur, seamThumbGrab));
        CHECK(sendMove(editor, camera, seamBarCur, seamThumbGrab + seamBarDelta));
        CHECK(sendRelease(editor, camera, seamBarCur, seamThumbGrab + seamBarDelta));
        CHECK(scrollCalls == scrollCallsBeforeBar + 1); // the timeline's SCROLL delegate heard it...
        CHECK(std::fabs((scrolledFrames - scrolledBeforeBar) + 30.0 / seamPpfAtBarDrag) < 1e-9); // ...by 30 px of frames
        CHECK(editor.scale().pixelXOfFrame(0.0) == seamOriginBeforeBar); // and the panel's own X was not written
        CHECK(zoomCalls == 5); // a scroll bar is not a wheel
        CHECK(std::abs(leftEdgeFrame() - windowLeftFrame) <= 1.0);

        std::printf(
            "[T] timeline seam: the panel writes the timeline's own selection (1 key selected in the panel "
            "is 1 in the timeline, focused), a pushed window of %.2f..%.2f maps across the panel at "
            "%.3f px/frame "
            "(panel %dx%d, plot %dx%d) and survives a cursor pass, a target-set change and fitAll; a Shift notch and a "
            "Ctrl+Shift notch both reach the frame ZOOM delegate (the frame axis alone, then both axes) and "
            "NEVER the scroll one, a 40px middle-drag pan and a 30px scroll-bar thumb drag reach the SCROLL "
            "delegate with 40/framePpf and 30/framePpf frames - all leaving the panel's own X exactly where "
            "the timeline put it, while a Ctrl wheel moves only the value axis\n",
            pushedLo, pushedHi, sharedPpf, sharedPanel.width(), sharedPanel.height(), sharedPlot.width(),
            sharedPlot.height()
        );

        // --- (f) DETACHED, the panel owns its frame axis again: the same fit and the
        // same wheel write it, which is the behaviour sections 0-22 pin with no
        // timeline at all. ---
        editor.clearFrameAxis();
        CHECK(!editor.hasTimelineFrameAxis());
        editor.fitAll();
        const QRect ownPlot = editor.scale().plotRect();
        CHECK(editor.scale().frameAtPixel(static_cast<double>(ownPlot.left())) <= 2.0);   // the shown keys came back
        CHECK(editor.scale().frameAtPixel(static_cast<double>(ownPlot.right())) >= 6.0);
        const double ownPpfBefore = editor.scale().frameScale().pixelsPerFrame();
        const int ownSpanBefore = editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0));
        // SHIFT IS THE FRAME-AXIS-ONLY ZOOM here too (it was the SCROLL for one
        // round): with no timeline the panel's OWN frame axis zooms and its value axis
        // does not move a bit. A notch AWAY (+120) is the zoom OUT, so the panel's own
        // rung drops - and a SCROLL would have left it alone and moved the offset with
        // no anchor, which the anchored frame below rules out.
        const double ownAnchorFrame = editor.scale().frameAtPixel(ownPlot.center().x());
        const float ownValueMinBefore = editor.scale().valueMin();
        const float ownValueMaxBefore = editor.scale().valueMax();
        CHECK(editor.updateWheel(camera, ownPlot.center(), 120, Qt::ShiftModifier, false));
        CHECK(editor.scale().frameScale().pixelsPerFrame() < ownPpfBefore); // the rate moved (zoom OUT)...
        CHECK(editor.scale().pixelX(core::Frame(24)) - editor.scale().pixelX(core::Frame(0)) < ownSpanBefore);
        CHECK(std::fabs(editor.scale().frameAtPixel(ownPlot.center().x()) - ownAnchorFrame) < 1e-9); // ...anchored
        CHECK(editor.scale().valueMin() == ownValueMinBefore); // and the value axis did not move at all
        CHECK(editor.scale().valueMax() == ownValueMaxBefore);
        CHECK(zoomCalls == 5);   // the ZOOM delegate heard nothing: this axis is the panel's own
        CHECK(scrollCalls == 2); // and the scroll delegate did not either

        // CTRL+SHIFT: BOTH axes on the panel's own axis too. The frame rate drops
        // further, the value window grows with it (both halves of one notch), the
        // anchored frame stays under the cursor, and no delegate hears either half.
        const float ownValueSpanBefore = editor.scale().valueMax() - editor.scale().valueMin();
        CHECK(editor.updateWheel(camera, ownPlot.center(), 120, Qt::ControlModifier | Qt::ShiftModifier, false));
        CHECK(editor.scale().frameScale().pixelsPerFrame() < ownPpfBefore); // the panel's OWN frame axis moved
        CHECK(editor.scale().valueMax() - editor.scale().valueMin() > ownValueSpanBefore); // and its value axis did
        CHECK(std::fabs(editor.scale().frameAtPixel(ownPlot.center().x()) - ownAnchorFrame) < 1e-9);
        CHECK(zoomCalls == 5);   // still no delegate: the panel owns this axis
        CHECK(scrollCalls == 2);

        // The shared end must go before the timeline it points at does: a dangling
        // selection source is the one thing this seam can get catastrophically wrong.
        editor.setTargets({}); // drop the target before its owner dies
        editor.setSelectionSource(nullptr);
        editor.clearFrameAxis();
        CHECK(&editor.selection() != &timeline.selection());

        // The timeline's own selection outlived the panel's attachment - it is the
        // timeline's data, and the panel was only ever a second view of it.
        fromTimeline = core::TimeLineEvent();
        CHECK(timeline.retrieveSelectionTargets(fromTimeline));
        CHECK(fromTimeline.targets().size() == 2);
    }

    // --- 23b: THE MULTI-KEY MOVE. A move carries the panel's WHOLE SELECTION:
    // every selected key moves by the same delta, and the whole gesture is ONE
    // command (KeyMoveCommand) - so ONE undo puts every frame AND every value
    // back. The behaviour this replaced moved only the PRESSED key, which fails
    // both halves of what is asserted below.
    {
        core::TestNode groupNode;
        core::RotateKey* g0 = new core::RotateKey();
        g0->setRotate(5.0f);
        setup.push(groupNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, g0));
        core::RotateKey* g1 = new core::RotateKey();
        g1->setRotate(25.0f);
        setup.push(groupNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 20, g1));
        CHECK(groupNode.timeLine()->map(core::TimeKeyType_Rotate).size() == 2);

        editor.setTargets({ &groupNode });
        editor.scale().setRect(panel);
        editor.fitAll();
        project.commandStack().clear();

        const Scaler& groupScale = editor.scale();
        const QPoint pG0(groupScale.pixelX(core::Frame(0)), groupScale.pixelY(5.0f));
        const QPoint pG1(groupScale.pixelX(core::Frame(20)), groupScale.pixelY(25.0f));
        CHECK(groupScale.plotRect().contains(pG0));
        CHECK(groupScale.plotRect().contains(pG1));

        core::AbstractCursor groupCur;

        // The selection: a plain press replaces it with the frame-20 key, then a
        // SHIFT-press ADDS the frame-0 key and arms the move - so the drag that
        // follows starts from a two-key selection. (A plain press would replace
        // the selection with one key again, which is the single-key case.)
        CHECK(sendPress(editor, camera, groupCur, pG1));
        CHECK(sendRelease(editor, camera, groupCur, pG1));
        CHECK(editor.selectionCount() == 1);
        CHECK(sendPress(editor, camera, groupCur, pG0, Qt::ShiftModifier));
        CHECK(editor.selectionCount() == 2);

        const QPoint groupTo(groupScale.pixelX(core::Frame(5)), groupScale.pixelY(15.0f));
        const int expectedFrameStep = groupScale.frameAt(groupTo.x()).get() - groupScale.frameAt(pG0.x()).get();
        const float expectedValueDelta = groupScale.valueAt(groupTo.y()) - groupScale.valueAt(pG0.y());
        CHECK(expectedFrameStep == 5); // the pixels name the frames they map to
        CHECK(expectedValueDelta > 5.0f && expectedValueDelta < 15.0f); // a real value move

        // The drag: NO modifier, so both axes move (the Shift above was the
        // selection's; the modifiers are re-read per event).
        CHECK(sendMove(editor, camera, groupCur, groupTo));
        CHECK(sendRelease(editor, camera, groupCur, groupTo));

        // Every selected key went where the pointer pointed, by the SAME delta,
        // and they are the SAME key objects (TimeLine::move re-keys, it never
        // rebuilds).
        CHECK(!groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(!groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, expectedFrameStep));
        CHECK(groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20 + expectedFrameStep));
        CHECK(groupNode.timeLine()->timeKey(core::TimeKeyType_Rotate, expectedFrameStep) == g0);
        CHECK(groupNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 20 + expectedFrameStep) == g1);
        CHECK(g0->rotate() == 5.0f + expectedValueDelta);
        CHECK(g1->rotate() == 25.0f + expectedValueDelta);

        // ONE command for the WHOLE multi-key drag: one undo restores both frames
        // and both values, and a second finds nothing behind it.
        bool undoneGroup = false;
        CHECK(!stack.undo(&undoneGroup).isEmpty());
        CHECK(undoneGroup);
        CHECK(groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(!groupNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, expectedFrameStep));
        CHECK(g0->rotate() == 5.0f);
        CHECK(g1->rotate() == 25.0f);
        bool undoneGroupAgain = true;
        CHECK(stack.undo(&undoneGroupAgain).isEmpty());
        CHECK(!undoneGroupAgain);

        // ...and that ONE command redoes whole: both axes are rebuilt from the
        // values the command itself carries.
        bool redoneGroup = false;
        CHECK(!stack.redo(&redoneGroup).isEmpty());
        CHECK(redoneGroup);
        CHECK(g0->frame() == expectedFrameStep);
        CHECK(g1->frame() == 20 + expectedFrameStep);
        CHECK(g0->rotate() == 5.0f + expectedValueDelta);
        CHECK(g1->rotate() == 25.0f + expectedValueDelta);

        std::printf(
            "[G] multi-key move: 2 selected keys moved by frame %+d / value %+.3f in ONE command; one undo "
            "restored both frames and both values, a second found nothing, and one redo rebuilt both\n",
            expectedFrameStep, expectedValueDelta
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 23c: THE PRESS THAT GRABS A SELECTION. Which key a drag STARTS on must
    // not decide what it moves: a press on a key the selection already covers
    // GRABS that selection and leaves it exactly as it is - the design's rule for
    // a press on something selected - so the drag it arms carries the whole
    // selection. A plain press on a key that is NOT selected still replaces the
    // selection with that one key, which is the other half of the same rule and
    // what makes a drag on a fresh key an edit of that key alone. Three keys: two
    // selected (the halves) and one bystander, left unselected, that must not move
    // a pixel in either half.
    //
    // This is the press, not the move, that the report was about: the move path
    // already carried every member (23b above), but the press had already
    // collapsed the selection to the marker under the pointer, so the move had
    // nothing else to carry.
    {
        core::TestNode grabNode;
        core::RotateKey* q0 = new core::RotateKey();
        q0->setRotate(5.0f);
        setup.push(grabNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, q0));
        core::RotateKey* q1 = new core::RotateKey();
        q1->setRotate(25.0f);
        setup.push(grabNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 20, q1));
        core::RotateKey* q2 = new core::RotateKey();
        q2->setRotate(45.0f);
        setup.push(grabNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 40, q2));
        CHECK(grabNode.timeLine()->map(core::TimeKeyType_Rotate).size() == 3);

        editor.setTargets({ &grabNode });
        editor.scale().setRect(panel);
        editor.fitAll(); // so the keys are on screen and the drags land where they point
        editor.clearSelection();
        project.commandStack().clear();

        const Channel grabChannel(core::TimeKeyType_Rotate, 0);
        // Every pixel below is read from the LIVE scale at the moment it is used,
        // so each press and each expected delta are read off the same mapping the
        // gesture will run on, whatever the window did between two of them.
        auto keyPixel = [&editor](core::Frame aFrame, float aValue) {
            const Scaler& live = editor.scale();
            return QPoint(live.pixelX(aFrame), live.pixelY(aValue));
        };
        core::AbstractCursor grabCur;

        // THE SELECTION: the plain press that replaces (nothing is selected, so the
        // frame-0 key is what it replaces with) and the Shift-press that adds the
        // frame-20 key. Two keys - and the frame-40 key is not one of them.
        const QPoint gQ0 = keyPixel(core::Frame(0), q0->rotate());
        const QPoint gQ1 = keyPixel(core::Frame(20), q1->rotate());
        CHECK(editor.scale().plotRect().contains(gQ0));
        CHECK(editor.scale().plotRect().contains(gQ1));
        CHECK(sendPress(editor, camera, grabCur, gQ0));
        CHECK(sendRelease(editor, camera, grabCur, gQ0));
        CHECK(editor.selectionCount() == 1);
        CHECK(sendPress(editor, camera, grabCur, gQ1, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, grabCur, gQ1, Qt::ShiftModifier));
        CHECK(editor.selectionCount() == 2);
        CHECK(editor.isMarkerSelected(grabNode, grabChannel, core::Frame(20)));
        CHECK(!editor.isMarkerSelected(grabNode, grabChannel, core::Frame(40)));

        // (a) A PLAIN PRESS ON THE KEY THAT IS ALREADY SELECTED. It grabs the
        // selection: the two assertions on the selection are the ones that fail
        // today, when the press replaced the selection with the marker under the
        // pointer and the drag below then moved ONE key.
        const QPoint armQ1 = keyPixel(core::Frame(20), q1->rotate());
        CHECK(sendPress(editor, camera, grabCur, armQ1));
        const int grabbedCount = editor.selectionCount(); // how many keys the press left selected
        CHECK(grabbedCount == 2); // the press grabbed the selection...
        CHECK(editor.isMarkerSelected(grabNode, grabChannel, core::Frame(0))); // ...both keys are still in
        CHECK(!editor.isMarkerSelected(grabNode, grabChannel, core::Frame(40))); // and the third still is not

        const QPoint grabTo(editor.scale().pixelX(core::Frame(25)), editor.scale().pixelY(15.0f));
        const Scaler& grabScale = editor.scale(); // the window the moves are interpreted against
        const int grabStep = grabScale.frameAt(grabTo.x()).get() - grabScale.frameAt(armQ1.x()).get();
        const float grabDelta = grabScale.valueAt(grabTo.y()) - grabScale.valueAt(armQ1.y());
        CHECK(grabStep == 5); // the pixels name the frames they map to (and the window did not move)
        CHECK(grabDelta < -5.0f && grabDelta > -15.0f); // a real, downward value move

        CHECK(sendMove(editor, camera, grabCur, grabTo));
        CHECK(sendRelease(editor, camera, grabCur, grabTo));

        // The WHOLE selection moved by that one delta, and they are the SAME key
        // objects (TimeLine::move re-keys, it never rebuilds).
        CHECK(!grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(!grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, grabStep));
        CHECK(grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20 + grabStep));
        CHECK(grabNode.timeLine()->timeKey(core::TimeKeyType_Rotate, grabStep) == q0);
        CHECK(grabNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 20 + grabStep) == q1);
        CHECK(q0->rotate() == 5.0f + grabDelta);
        CHECK(q1->rotate() == 25.0f + grabDelta);
        // ...while the key that was never selected did not move - neither its
        // frame nor its value, on its own object.
        CHECK(grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 40));
        CHECK(grabNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 40) == q2);
        CHECK(q2->frame() == 40);
        CHECK(q2->rotate() == 45.0f);

        // The whole gesture is ONE command, as for every move here.
        bool undoneGrab = false;
        CHECK(!stack.undo(&undoneGrab).isEmpty());
        CHECK(undoneGrab);
        CHECK(q0->frame() == 0 && q0->rotate() == 5.0f);
        CHECK(q1->frame() == 20 && q1->rotate() == 25.0f);
        CHECK(q2->frame() == 40 && q2->rotate() == 45.0f);

        // (b) THE COMPLEMENT: a plain press on a key that is NOT selected still
        // replaces the selection with it, so a drag from there moves that key
        // alone. (Without this half the section would also pass on a press that
        // never changed the selection at all.)
        const QPoint gQ2 = keyPixel(core::Frame(40), q2->rotate());
        CHECK(editor.scale().plotRect().contains(gQ2));
        CHECK(sendPress(editor, camera, grabCur, gQ2));
        CHECK(editor.selectionCount() == 1);
        CHECK(editor.isMarkerSelected(grabNode, grabChannel, core::Frame(40)));
        CHECK(!editor.isMarkerSelected(grabNode, grabChannel, core::Frame(0)));

        const QPoint aloneTo(editor.scale().pixelX(core::Frame(35)), editor.scale().pixelY(35.0f));
        const Scaler& aloneScale = editor.scale();
        const int aloneStep = aloneScale.frameAt(aloneTo.x()).get() - aloneScale.frameAt(gQ2.x()).get();
        const float aloneDelta = aloneScale.valueAt(aloneTo.y()) - aloneScale.valueAt(gQ2.y());
        CHECK(aloneStep == -5);
        CHECK(aloneDelta < -5.0f && aloneDelta > -15.0f);
        CHECK(sendMove(editor, camera, grabCur, aloneTo));
        CHECK(sendRelease(editor, camera, grabCur, aloneTo));

        CHECK(grabNode.timeLine()->timeKey(core::TimeKeyType_Rotate, 40 + aloneStep) == q2);
        CHECK(q2->rotate() == 45.0f + aloneDelta);
        CHECK(grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0)); // the dropped keys did not move
        CHECK(grabNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 20));
        CHECK(q0->frame() == 0 && q0->rotate() == 5.0f);
        CHECK(q1->frame() == 20 && q1->rotate() == 25.0f);

        std::printf(
            "[G2] press that grabs a selection: a plain press on an already-selected key left %d keys "
            "selected and the drag moved both by frame %+d / value %+.3f while the third key stayed at "
            "frame 40 / %.1f; a plain press on the unselected key replaced the selection with it, and the "
            "drag moved that one by frame %+d / value %+.3f alone\n",
            grabbedCount, grabStep, grabDelta, q2->rotate() - aloneDelta, aloneStep, aloneDelta
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 23d: a GHOST-handle drag writes Type_Custom at the third points. An
    // Auto segment's control points are DERIVED (Curve::ghostHandles), not
    // stored, so a press must FIND them (the hit-test does now) and a drag must
    // CONVERT: Type_Custom carrying the derived pair at (1/3, y1) and (2/3, y2),
    // inside the drag's own cmnd::Gesture - so the conversion is one undo step.
    {
        core::TestNode ghostNode;
        core::RotateKey* a0 = new core::RotateKey();
        a0->setRotate(0.0f);
        setup.push(ghostNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, a0));
        core::RotateKey* a1 = new core::RotateKey();
        a1->setRotate(100.0f);
        setup.push(ghostNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 10, a1));
        core::RotateKey* a2 = new core::RotateKey();
        a2->setRotate(40.0f);
        setup.push(ghostNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 20, a2));
        a0->data().easing(0).type = util::Easing::Type_Auto;
        a1->data().easing(0).type = util::Easing::Type_Auto;
        const util::Easing::Param kAutoAuthored = a0->data().easing(0);

        editor.setTargets({ &ghostNode });
        editor.scale().setRect(panel);
        editor.fitAll();
        project.commandStack().clear();

        const Curve ghostCurve(*ghostNode.timeLine(), Channel(core::TimeKeyType_Rotate, 0));
        CHECK(ghostCurve.segmentCount() == 2);
        const Curve::Segment ghostSeg = ghostCurve.segment(0);
        CHECK(ghostSeg.from == a0 && ghostSeg.to == a1);
        CHECK(ghostSeg.fromValue == 0.0f && ghostSeg.toValue == 100.0f);

        const QVector<QPointF> ghosts = ghostCurve.ghostHandles(*ghostSeg.from);
        CHECK(ghosts.size() == 2);
        CHECK(std::fabs(ghosts[0].x() - 1.0 / 3.0) < 1e-9);
        CHECK(std::fabs(ghosts[1].x() - 2.0 / 3.0) < 1e-9);

        // The ghost DOT, mapped exactly the way the panel's own geometry maps a
        // handle (graph::handlePointAt), rounded to the pixel a press can name.
        const QPointF ghostDot = ctrl::graph::handlePointAt(editor.scale(), ghostSeg, ghosts[0]);
        const QPoint ghostGrab(
            static_cast<int>(std::lround(ghostDot.x())), static_cast<int>(std::lround(ghostDot.y()))
        );
        CHECK(editor.scale().plotRect().contains(ghostGrab));

        core::AbstractCursor ghostCur;

        // (a) A drag whose move lands back ON the ghost dot: the conversion is
        // the whole edit, so what was written is the derived pair's GEOMETRY at
        // the thirds - the grabbed handle at its own third, and the pair's
        // other control point left at the second third. That other point is the
        // `to` key's INCOMING handle, NOT the dragged key's opposite handle: the
        // grabbed handle is this segment's `from` key's OUTGOING one, that key
        // is the channel's FIRST, and a first key has no incoming handle - so
        // there is no neighbour to mirror onto and the derived (2/3, y2) stays.
        CHECK(sendPress(editor, camera, ghostCur, ghostGrab));
        CHECK(editor.ownsCursor(camera, ghostCur)); // the ghost dot is a grabbable handle
        CHECK(a0->data().easing(0).type == util::Easing::Type_Auto); // the press alone edits nothing
        CHECK(sendMove(editor, camera, ghostCur, ghostGrab));
        CHECK(sendRelease(editor, camera, ghostCur, ghostGrab));

        CHECK(a0->data().easing(0).type == util::Easing::Type_Custom);
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.x1 - 1.0f / 3.0f) < 0.01f);
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.x2 - 2.0f / 3.0f) < 0.01f);
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.y1 - static_cast<float>(ghosts[0].y())) < 0.02f);
        // The other control point is the conversion's own derived y, untouched
        // by the drag: no mirror exists for the first key.
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.y2 - static_cast<float>(ghosts[1].y())) < 1e-6f);

        // ONE undo brings the Auto easing back whole (the conversion and the
        // handle write are one command), and a second finds nothing behind it.
        bool undoneGhost = false;
        CHECK(!stack.undo(&undoneGhost).isEmpty());
        CHECK(undoneGhost);
        CHECK(a0->data().easing(0) == kAutoAuthored);
        bool undoneGhostAgain = true;
        CHECK(stack.undo(&undoneGhostAgain).isEmpty());
        CHECK(!undoneGhostAgain);

        // (b) A real drag AWAY from the dot: the conversion still happens (the
        // key becomes Custom, riding the drag's own gesture), and the drag then
        // writes the GRABBED handle where the pointer is. The pair's other
        // control point belongs to the `to` key, not to the dragged one, and has
        // no mirror rule of its own: it stays at the derived second third.
        const QPoint ghostMoved(ghostGrab.x() + 6, ghostGrab.y() + 5);
        CHECK(sendPress(editor, camera, ghostCur, ghostGrab));
        CHECK(sendMove(editor, camera, ghostCur, ghostMoved));
        CHECK(sendRelease(editor, camera, ghostCur, ghostMoved));

        CHECK(a0->data().easing(0).type == util::Easing::Type_Custom);
        const double ghostFrameSpan = static_cast<double>(ghostSeg.to->frame() - ghostSeg.from->frame());
        const double ghostValueSpan = static_cast<double>(ghostSeg.toValue - ghostSeg.fromValue);
        const double expectedNx = (editor.scale().frameAtPixel(static_cast<double>(ghostMoved.x()))
                                   - static_cast<double>(ghostSeg.from->frame()))
                                  / ghostFrameSpan;
        const double expectedNy = (editor.scale().valueAtPixel(static_cast<double>(ghostMoved.y()))
                                   - static_cast<double>(ghostSeg.fromValue))
                                  / ghostValueSpan;
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.x1 - static_cast<float>(expectedNx)) < 1e-4f);
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.y1 - static_cast<float>(expectedNy)) < 1e-4f);
        // ...and the other control point is still the derived pair's own second
        // third, in BOTH components: the drag did not touch it.
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.x2 - static_cast<float>(ghosts[1].x())) < 1e-6f);
        CHECK(std::fabs(a0->data().easing(0).cubicBezier.y2 - static_cast<float>(ghosts[1].y())) < 1e-6f);

        std::printf(
            "[A] ghost-handle drag: an Auto segment's derived pair (1/3,%.3f)/(2/3,%.3f) was written as "
            "Type_Custom at the thirds and undone by ONE undo; a drag away moved the grabbed handle to "
            "(%.3f,%.3f) and left the other control point at its derived (%.3f,%.3f) - the first key has "
            "no mirror\n",
            ghosts[0].y(), ghosts[1].y(), a0->data().easing(0).cubicBezier.x1,
            a0->data().easing(0).cubicBezier.y1, a0->data().easing(0).cubicBezier.x2,
            a0->data().easing(0).cubicBezier.y2
        );

        // (c) THE CULL. The hit test derives a segment's ghost pair only when one
        // of the pair's x-pixels is within the grab radius of the pointer (see
        // GraphEditor::hitTest: the x is the segment's own arithmetic, so the cull
        // is exact and costs no map lookup). Both sides of that decision must
        // behave exactly as they did before it existed: a ghost dot 8px away is
        // STILL grabbed - an inexact cull would silently drop a handle the pointer
        // is on - and a point 30px away grabs nothing at all, so it opens the
        // marquee like any empty plot press.
        a0->data().easing(0).type = util::Easing::Type_Auto; // the ghost state again: a document write, not a gesture

        const QPoint ghostNear(ghostGrab.x() + 8, ghostGrab.y());
        // The precondition the grab below rests on: 8px in x, plus the dot's own
        // sub-pixel rounding, is inside kHandleHitRadius - so it MUST be grabbed.
        CHECK(std::fabs(ghostDot.x() - static_cast<double>(ghostNear.x())) <= 9.0);
        CHECK(sendPress(editor, camera, ghostCur, ghostNear));
        CHECK(editor.ownsCursor(camera, ghostCur));
        CHECK(a0->data().easing(0).type == util::Easing::Type_Auto); // the press alone still edits nothing
        CHECK(sendMove(editor, camera, ghostCur, ghostNear));
        CHECK(sendRelease(editor, camera, ghostCur, ghostNear));
        CHECK(a0->data().easing(0).type == util::Easing::Type_Custom); // the reachable ghost WAS grabbed
        CHECK(!editor.hasMarquee());

        a0->data().easing(0).type = util::Easing::Type_Auto; // ...and once more for the far case
        const QPoint ghostFar(ghostGrab.x() + 30, ghostGrab.y());
        CHECK(sendPress(editor, camera, ghostCur, ghostFar));
        CHECK(editor.hasMarquee()); // no handle within reach: the plain plot press marquees
        CHECK(editor.marqueeRect().isEmpty()); // with nothing dragged yet
        CHECK(a0->data().easing(0).type == util::Easing::Type_Auto); // nothing grabbed, nothing written
        CHECK(sendRelease(editor, camera, ghostCur, ghostFar));
        CHECK(!editor.hasMarquee());

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 23e: THE MARKER. A key's COMPONENTS are things a user selects one at a
    // time - "The selection is the channel marker, not the key" - so the panel's
    // selection names (key, component) and the drag follows it: HORIZONTAL moves
    // the frame, which one key shares across its components, so each selected key
    // moves ONCE; VERTICAL moves every selected MARKER in its OWN channel, by the
    // same delta. A Scale key has exactly two components, so "X alone" and "X and
    // Y" are two different selections with two different drags - which is what
    // this section pins, on the public surface only:
    //   * what a press selects (the panel's isMarkerSelected/hasSelectedSiblingMarker
    //     per component, and selectionCount/the key-level retrieveSelection for the
    //     other reading),
    //   * what a drag does to the SELECTED marker and to the sibling it did not
    //     select (the document: the key's own scale() pair),
    //   * ONE command per gesture, whatever it moved,
    //   * the bulk rules: a marquee and ctrl+A write WHOLE-KEY entries (every marker
    //     of those keys), Escape clears, and the set announces every change of its
    //     own and only the changes.
    {
        core::TestNode markerNode;
        core::ScaleKey* mk0 = new core::ScaleKey();
        mk0->setScale(QVector2D(20.0f, 30.0f));
        setup.push(markerNode.timeLine()->createPusher(core::TimeKeyType_Scale, 5, mk0));
        core::ScaleKey* mk1 = new core::ScaleKey();
        mk1->setScale(QVector2D(60.0f, 80.0f));
        setup.push(markerNode.timeLine()->createPusher(core::TimeKeyType_Scale, 25, mk1));
        CHECK(markerNode.timeLine()->map(core::TimeKeyType_Scale).size() == 2);

        // The pair the whole section rests on: two components, so "X alone" is a
        // selection that leaves something out.
        const Channel markerX(core::TimeKeyType_Scale, 0);
        const Channel markerY(core::TimeKeyType_Scale, 1);
        CHECK(Channel::componentCount(core::TimeKeyType_Scale) == 2);
        CHECK(markerX.value(*mk0) == 20.0f && markerY.value(*mk0) == 30.0f);

        editor.setTargets({ &markerNode });
        editor.scale().setRect(panel);
        editor.fitAll();
        editor.clearSelection();
        project.commandStack().clear();

        // Every pixel below comes from the editor's OWN live scale, so the press
        // lands on the marker the drawing layer draws (the two are ~80px apart at
        // this window's span, far outside the 10px hit radius - a press can only
        // mean the marker it is on).
        const Scaler& markerScale = editor.scale();
        const QPoint px05(markerScale.pixelX(core::Frame(5)), markerScale.pixelY(20.0f));
        const QPoint py05(markerScale.pixelX(core::Frame(5)), markerScale.pixelY(30.0f));
        const QPoint px25(markerScale.pixelX(core::Frame(25)), markerScale.pixelY(60.0f));
        for (const QPoint& pixel : { px05, py05, px25 }) {
            CHECK(markerScale.plotRect().contains(pixel));
        }
        CHECK(std::abs(px05.y() - py05.y()) > int(ctrl::graph::kKeyHitRadius));

        // THE SET ANNOUNCES ITSELF: what the timeline header's chip reads instead
        // of the reconcile timer it used to run. The slot goes with this block (see
        // the disconnect below): the editor and its set outlive every section here.
        int selectionChanges = 0;
        const util::SlotId changeSlot =
            editor.selection().onChanged.connect([&selectionChanges]() { ++selectionChanges; });

        // (a) A PLAIN PRESS SELECTS THE MARKER UNDER THE POINTER, not the key: the
        // other component is left out - unselected, but the SIBLING of one that is
        // (which is what the drawing layer's faint ring is for).
        core::AbstractCursor markerCur;
        CHECK(sendPress(editor, camera, markerCur, px05));
        CHECK(sendRelease(editor, camera, markerCur, px05));
        CHECK(selectionChanges > 0); // the press changed the selection, so it said so
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(!editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(!editor.hasSelectedSiblingMarker(markerNode, markerX, core::Frame(5)));
        CHECK(editor.hasSelectedSiblingMarker(markerNode, markerY, core::Frame(5)));
        CHECK(editor.selection().count() == 1); // ONE ENTRY: the marker the press hit
        CHECK(editor.selectionCount() == 1);    // ...which is ONE KEY (a marker is not one)
        // The KEY still carries the timeline's highlight: a marker entry focuses its
        // key, so the panel's finer selection is visible in the coarse view too.
        CHECK(markerNode.timeLine()->timeKey(core::TimeKeyType_Scale, 5)->isFocused());
        CHECK(!markerNode.timeLine()->timeKey(core::TimeKeyType_Scale, 25)->isFocused());
        // The timeline's own reading of that selection: the key it belongs to.
        core::TimeLineEvent markerTargets;
        CHECK(editor.retrieveSelection(markerTargets));
        CHECK(markerTargets.targets().size() == 1);
        CHECK(markerTargets.targets()[0].node == &markerNode);
        CHECK(markerTargets.targets()[0].pos.type() == core::TimeKeyType_Scale);
        CHECK(markerTargets.targets()[0].pos.index() == 5);

        // (b) SHIFT ADDS THE SECOND MARKER of the same key: two entries, ONE key.
        const int changesBeforeShift = selectionChanges;
        CHECK(sendPress(editor, camera, markerCur, py05, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, markerCur, py05, Qt::ShiftModifier));
        CHECK(selectionChanges > changesBeforeShift);
        CHECK(editor.selection().count() == 2); // two markers...
        CHECK(editor.selectionCount() == 1);    // ...of one key
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(editor.hasSelectedSiblingMarker(markerNode, markerX, core::Frame(5)));
        CHECK(editor.hasSelectedSiblingMarker(markerNode, markerY, core::Frame(5)));
        // The key-level reading is still ONE target: the two entries name one key.
        CHECK(editor.retrieveSelection(markerTargets = core::TimeLineEvent()));
        CHECK(markerTargets.targets().size() == 1);
        CHECK(markerTargets.targets()[0].pos.index() == 5);

        // (c) THE DRAG WITH BOTH MARKERS SELECTED. The grab is a SHIFT-press on a
        // marker that is already covered (Shift is also the axis lock for the drag
        // below, where it is re-read per move), so the selection the gesture carries
        // is the two markers; then the pointer goes up and right.
        const QPoint bothFrom(markerScale.pixelX(core::Frame(5)), markerScale.pixelY(30.0f));
        const QPoint bothTo(markerScale.pixelX(core::Frame(9)), markerScale.pixelY(30.0f) - 24);
        const int bothStep = markerScale.frameAt(bothTo.x()).get() - markerScale.frameAt(bothFrom.x()).get();
        const float bothDelta = markerScale.valueAt(bothTo.y()) - markerScale.valueAt(bothFrom.y());
        CHECK(bothStep == 4);            // the pixels name the frames they map to
        CHECK(bothDelta > 1.0f);         // and a real, upward value move
        CHECK(sendPress(editor, camera, markerCur, bothFrom, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, markerCur, bothTo));
        CHECK(sendRelease(editor, camera, markerCur, bothTo));

        // HORIZONTAL: the key's frame moves ONCE - the frame is one thing the two
        // selected markers share, so it cannot move by two steps' worth.
        CHECK(!markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 5));
        CHECK(!markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 5 + bothStep * 2));
        CHECK(markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 5 + bothStep));
        CHECK(markerNode.timeLine()->timeKey(core::TimeKeyType_Scale, 5 + bothStep) == mk0);
        // VERTICAL: BOTH components move by the same delta, each in its own channel.
        CHECK(mk0->scale().x() == 20.0f + bothDelta);
        CHECK(mk0->scale().y() == 30.0f + bothDelta);
        // ...and the key that was not selected is untouched, both components of it.
        CHECK(markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 25));
        CHECK(mk1->scale().x() == 60.0f && mk1->scale().y() == 80.0f);

        // ONE COMMAND for the whole two-marker drag: one undo restores the frame and
        // BOTH components, and a second finds nothing behind it.
        bool undoneBoth = false;
        CHECK(!stack.undo(&undoneBoth).isEmpty());
        CHECK(undoneBoth);
        CHECK(markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 5));
        CHECK(!markerNode.timeLine()->hasTimeKey(core::TimeKeyType_Scale, 5 + bothStep));
        CHECK(mk0->scale().x() == 20.0f && mk0->scale().y() == 30.0f);
        bool undoneBothAgain = true;
        CHECK(stack.undo(&undoneBothAgain).isEmpty());
        CHECK(!undoneBothAgain);
        std::printf(
            "[K] marker drag: both markers of one Scale key moved frame %+d / value %+.3f in ONE command "
            "(X 20 -> %.3f, Y 30 -> %.3f); one undo restored frame and both components, a second found "
            "nothing\n",
            bothStep, bothDelta, 20.0f + bothDelta, 30.0f + bothDelta
        );

        // (d) X ALONE, DRAGGED UP: X follows the pointer and the Y marker - not
        // selected, but the sibling of one that is - keeps its value BIT-identical.
        // The pointer moves in x by nothing at all, so no frame can be owed either.
        //
        // The selection is narrowed to X by TOGGLING Y out with a Ctrl-press: a
        // PLAIN press on the X marker cannot do it any more, because X is already
        // selected and a press on something selected grabs the selection as it is
        // (section 23c) - which is exactly why the design gives Ctrl that job.
        // Both markers are selected here (the drag above, undone, left them), so
        // the toggle has something to take out: asserted before and after, so the
        // press cannot pass by doing nothing.
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(sendPress(editor, camera, markerCur, py05, Qt::ControlModifier));
        CHECK(sendRelease(editor, camera, markerCur, py05, Qt::ControlModifier));
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(!editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(editor.hasSelectedSiblingMarker(markerNode, markerY, core::Frame(5)));
        // ...and the plain press that GRABS X (already selected) leaves it alone,
        // which is the press this section's drag is armed by.
        CHECK(sendPress(editor, camera, markerCur, px05));
        CHECK(sendRelease(editor, camera, markerCur, px05));
        CHECK(editor.selection().count() == 1); // still X alone: the plain press did not re-add Y
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(!editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));

        const QPoint xFrom(markerScale.pixelX(core::Frame(5)), markerScale.pixelY(20.0f));
        const QPoint xTo(xFrom.x(), xFrom.y() - 30);
        const float xDelta = markerScale.valueAt(xTo.y()) - markerScale.valueAt(xFrom.y());
        CHECK(xDelta > 1.0f);
        CHECK(sendPress(editor, camera, markerCur, xFrom));
        CHECK(sendMove(editor, camera, markerCur, xTo));
        CHECK(sendRelease(editor, camera, markerCur, xTo));
        CHECK(mk0->frame() == 5);               // a vertical-only drag moves no frame
        CHECK(mk0->scale().x() == 20.0f + xDelta); // the SELECTED marker followed the pointer
        CHECK(mk0->scale().y() == 30.0f);          // the unselected sibling: untouched, bit-identical
        bool undoneAlone = false;
        CHECK(!stack.undo(&undoneAlone).isEmpty());
        CHECK(undoneAlone);
        CHECK(mk0->scale().x() == 20.0f && mk0->scale().y() == 30.0f);
        bool undoneAloneAgain = true;
        CHECK(stack.undo(&undoneAloneAgain).isEmpty());
        CHECK(!undoneAloneAgain);

        // (e) THE BULK RULES WRITE WHOLE-KEY ENTRIES, and such an entry IS every
        // marker of its key - which is the other half of the design: a key the
        // timeline selected (or a marquee enclosed, or ctrl+A took) lights all of
        // its components. The marquee first, because it is the pointer gesture.
        editor.clearSelection();
        const QPoint boxOrigin(markerScale.plotRect().left() + 2, markerScale.plotRect().bottom() - 2);
        const QPoint boxEnd(markerScale.plotRect().right() - 2, markerScale.plotRect().top() + 2);
        CHECK(sendPress(editor, camera, markerCur, boxOrigin));
        CHECK(editor.hasMarquee());
        CHECK(sendMove(editor, camera, markerCur, boxEnd));
        CHECK(sendRelease(editor, camera, markerCur, boxEnd));
        CHECK(!editor.hasMarquee());
        CHECK(editor.selection().count() == 2); // ONE ENTRY PER KEY, not per marker
        CHECK(editor.selectionCount() == 2);
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(25)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(25)));
        CHECK(editor.hasSelectedSiblingMarker(markerNode, markerY, core::Frame(5)));

        // A vertical drag on that selection moves EVERY component of every key it
        // names, each in its own channel - the whole key is selected, and a whole
        // key's value is every channel it has.
        const QPoint allFrom(markerScale.pixelX(core::Frame(25)), markerScale.pixelY(60.0f));
        const QPoint allTo(allFrom.x(), allFrom.y() - 20);
        const float allDelta = markerScale.valueAt(allTo.y()) - markerScale.valueAt(allFrom.y());
        CHECK(allDelta > 1.0f);
        CHECK(sendPress(editor, camera, markerCur, allFrom, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, markerCur, allTo));
        CHECK(sendRelease(editor, camera, markerCur, allTo));
        CHECK(mk0->frame() == 5 && mk1->frame() == 25); // no frame moved: the pointer did not move in x
        CHECK(mk0->scale().x() == 20.0f + allDelta);
        CHECK(mk0->scale().y() == 30.0f + allDelta);
        CHECK(mk1->scale().x() == 60.0f + allDelta);
        CHECK(mk1->scale().y() == 80.0f + allDelta);
        // Two keys, four components, ONE command.
        bool undoneAll = false;
        CHECK(!stack.undo(&undoneAll).isEmpty());
        CHECK(undoneAll);
        CHECK(mk0->scale().x() == 20.0f && mk0->scale().y() == 30.0f);
        CHECK(mk1->scale().x() == 60.0f && mk1->scale().y() == 80.0f);
        bool undoneAllAgain = true;
        CHECK(stack.undo(&undoneAllAgain).isEmpty());
        CHECK(!undoneAllAgain);

        // A SHIFT-MARQUEE OVER A PARTIALLY SELECTED KEY MAKES IT WHOLE: the box names
        // every marker of every key it encloses, so the marker entry of an enclosed
        // key is replaced by a whole-key one rather than left beside it.
        editor.clearSelection();
        CHECK(sendPress(editor, camera, markerCur, px05)); // the frame-5 key's X alone
        CHECK(sendRelease(editor, camera, markerCur, px05));
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(!editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(sendPress(editor, camera, markerCur, boxOrigin, Qt::ShiftModifier));
        CHECK(sendMove(editor, camera, markerCur, boxEnd, Qt::ShiftModifier));
        CHECK(sendRelease(editor, camera, markerCur, boxEnd, Qt::ShiftModifier));
        CHECK(editor.selection().count() == 2); // both keys, each as one whole-key entry
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5))); // the sibling joined

        // (f) CTRL+A: every marker of every key of the SHOWN channels, as ONE ENTRY
        // PER KEY (a type whose two components are both drawn is not doubled), and
        // Escape clears. Both actions answer what they did, which is what a key
        // handler needs to know whether to consume the key.
        editor.clearSelection();
        CHECK(editor.selectAllMarkers());
        CHECK(editor.selection().count() == 2); // the two keys, one entry each
        CHECK(editor.selectionCount() == 2);
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(25)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(25)));
        CHECK(!editor.selectAllMarkers()); // already exactly that selection: nothing changed
        const int changesBeforeEscape = selectionChanges;
        CHECK(editor.clearSelection());    // ESCAPE
        CHECK(editor.selection().empty());
        CHECK(editor.selectionCount() == 0);
        CHECK(selectionChanges > changesBeforeEscape);
        CHECK(!editor.clearSelection());   // nothing left to clear, and the action says so

        // (g) THE TIMELINE'S OWN FORM, on the same set: a KEY-LEVEL entry (what the
        // timeline's gestures write - the panel's own modifySelection is that
        // currency) carries NO component, and it covers both of the key's markers.
        // An empty set is not changed by removing a key from it, and the set says so.
        CHECK(!editor.modifySelection(markerNode, markerX, core::Frame(5), false));
        CHECK(editor.modifySelection(markerNode, markerX, core::Frame(5), true));
        CHECK(editor.selection().count() == 1);
        CHECK(editor.selection().items().front().component == ctrl::time::Selection::kKeyLevel);
        CHECK(editor.isMarkerSelected(markerNode, markerX, core::Frame(5)));
        CHECK(editor.isMarkerSelected(markerNode, markerY, core::Frame(5)));
        CHECK(editor.selectionCount() == 1); // one key, at its own frame
        CHECK(editor.modifySelection(markerNode, markerX, core::Frame(5), false));
        CHECK(editor.selection().empty());

        std::printf(
            "[X] markers: a plain press selected Scale X alone (Y left with the sibling ring), Shift added "
            "Scale Y, and the set announced %d changes; ctrl+A took the shown channels' keys as 2 whole-key "
            "entries and Escape cleared it\n",
            selectionChanges
        );

        // The counter's slot goes with the counter: it captures this block's local,
        // and the editor's set outlives the block.
        editor.selection().onChanged.disconnect(changeSlot);

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 23f: THE HANDLE PAIR OF ONE KEY (link state, not a mirror). A
    // segment's two control points are NOT a symmetric pair about the segment:
    // (x1,y1) is the `from` key's OUTGOING tangent and (x2,y2) the `to` key's
    // INCOMING one, so each belongs to a different key. The opposite tangent of
    // a given key lives in the NEIGHBOURING segment - segment i-1's (x2,y2) for
    // the `from` key's outgoing handle, segment i+1's (x1,y1) for the `to` key's
    // incoming one.
    //
    // Whether the two are LINKED is the key's STORED
    // `util::Easing::Param::aligned` - document state (default true) carried in
    // the key's own param, NOT an inference from the pixel angle. The rules
    // driven below:
    //   (a) dragging a handle of a LINKED key keeps the other handle collinear
    //       and KEEPS ITS OWN LENGTH (angle linked, length independent -
    //       deliberately not a reflection);
    //   (b) Alt held on any step stops writing the other handle (byte-identical
    //       afterwards) AND writes the key FREE for good, so a later drag
    //       without Alt leaves the partner alone too; the latest modifier state
    //       is read, not the press's;
    //   (c) a FREE key's other handle is untouched without Alt;
    //   (d) a key MOVED ON THE VALUE AXIS keeps its link and both handles byte
    //       for byte, even though the pair's pixel angle changed under it - the
    //       user's report: the old inference flipped the state there;
    //   (e) Alt+click on a key links the pair along the CHORD from the previous
    //       key's anchor to the next key's anchor, each handle keeping its own
    //       length; the direction is independent of which handle is longer (the
    //       case below is built so the old longer-handle rule would have kept a
    //       direction clearly off that chord);
    //   (f) one undo restores both params AND the flag of every gesture/command;
    //   (g) the first (and last) key has one handle, so a drag has nothing to
    //       pair and Alt+click is inert - neither crashes;
    //   (h) an Auto neighbour is still converted to Custom when it is WRITTEN.
    //
    // The arithmetic is in PIXELS because each segment divides y by ITS OWN
    // value span: the two spans below are 90 and 60, so a normalized
    // construction would bend the tangent away from the line the eye expects
    // through the key. graph::Curve::setHandle clamps a written x into [0,1]
    // (a cubic's own domain) and leaves y free.
    {
        core::TestNode pairNode;
        core::RotateKey* fA = new core::RotateKey();
        fA->setRotate(0.0f);
        setup.push(pairNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, fA));
        core::RotateKey* fB = new core::RotateKey();
        fB->setRotate(90.0f);
        setup.push(pairNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 30, fB));
        core::RotateKey* fC = new core::RotateKey();
        fC->setRotate(150.0f);
        setup.push(pairNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 40, fC));

        editor.setTargets({ &pairNode });
        editor.scale().setRect(panel);
        editor.fitAll();
        editor.clearSelection();
        project.commandStack().clear();

        const Channel pairChannel(core::TimeKeyType_Rotate, 0);
        const Curve pairCurve(*pairNode.timeLine(), pairChannel);
        CHECK(pairCurve.segmentCount() == 2);
        CHECK(pairCurve.segment(0).from == fA && pairCurve.segment(0).to == fB);
        CHECK(pairCurve.segment(1).from == fB && pairCurve.segment(1).to == fC);
        CHECK(pairCurve.segment(0).fromValue == 0.0f && pairCurve.segment(0).toValue == 90.0f);
        CHECK(pairCurve.segment(1).fromValue == 90.0f && pairCurve.segment(1).toValue == 150.0f);
        // The point of pixel arithmetic: the two segments' value spans differ.
        CHECK(pairCurve.segment(0).toValue - pairCurve.segment(0).fromValue
              != pairCurve.segment(1).toValue - pairCurve.segment(1).fromValue);

        const Scaler pairScale = editor.scale();

        auto custom = [](float x1, float y1, float x2, float y2) {
            util::Easing::Param param;
            param.type = util::Easing::Type_Custom;
            param.cubicBezier.x1 = x1;
            param.cubicBezier.y1 = y1;
            param.cubicBezier.x2 = x2;
            param.cubicBezier.y2 = y2;
            return param;
        };

        // A pixel back into one segment's own normalized space (the inverse of
        // graph::handlePointAt), for the "the grabbed handle is at the pointer"
        // expectations.
        auto normalizedAt = [](const Scaler& aScale, const Curve::Segment& aSeg, const QPoint& aPos) {
            const double frameSpan = static_cast<double>(aSeg.to->frame() - aSeg.from->frame());
            const double valueSpan = static_cast<double>(aSeg.toValue - aSeg.fromValue);
            return QPointF(
                (aScale.frameAtPixel(static_cast<double>(aPos.x())) - static_cast<double>(aSeg.from->frame()))
                    / frameSpan,
                (aScale.valueAtPixel(static_cast<double>(aPos.y())) - static_cast<double>(aSeg.fromValue))
                    / valueSpan
            );
        };

        // The dot a handle is DRAWN at: a Custom pair is stored, an Auto pair is
        // derived - and the derivation is what the user sees and grabs.
        auto handlePx = [](const Scaler& aScale, const Curve& aCurve, const Curve::Segment& aSeg, int aHandle) {
            QPointF normalized = Curve::handle(aSeg, aHandle);
            if (aSeg.easing.type == util::Easing::Type_Auto) {
                const QVector<QPointF> ghosts = aCurve.ghostHandles(*aSeg.from);
                if (ghosts.size() == 2)
                    normalized = ghosts[aHandle];
            }
            return ctrl::graph::handlePointAt(aScale, aSeg, normalized);
        };

        auto lengthPx = [](const QPointF& aKey, const QPointF& aPoint) {
            return std::hypot(aPoint.x() - aKey.x(), aPoint.y() - aKey.y());
        };

        // The angle between the two pixel vectors key->outgoing and
        // key->incoming. 180 degrees means collinear through the key on
        // opposite sides. This is a MEASUREMENT the assertions use; the editor
        // no longer classifies by it (the link is the stored flag).
        auto pairAngle = [](const QPointF& aKey, const QPointF& aOutgoing, const QPointF& aIncoming) {
            const double outX = aOutgoing.x() - aKey.x();
            const double outY = aOutgoing.y() - aKey.y();
            const double inX = aIncoming.x() - aKey.x();
            const double inY = aIncoming.y() - aKey.y();
            const double outLength = std::hypot(outX, outY);
            const double inLength = std::hypot(inX, inY);
            if (!(outLength > 0.0) || !(inLength > 0.0))
                return 0.0;
            const double cosAngle = std::max(-1.0, std::min(1.0,
                (outX * inX + outY * inY) / (outLength * inLength)));
            return std::acos(cosAngle) * 180.0 / 3.14159265358979323846;
        };
        // The angle between two free pixel vectors, 0 when they point the same
        // way; how far a handle's direction is from the chord's.
        auto directionAngle = [](const QPointF& aLhs, const QPointF& aRhs) {
            const double lhsLength = std::hypot(aLhs.x(), aLhs.y());
            const double rhsLength = std::hypot(aRhs.x(), aRhs.y());
            if (!(lhsLength > 0.0) || !(rhsLength > 0.0))
                return 180.0;
            const double cosAngle = std::max(-1.0, std::min(1.0,
                (aLhs.x() * aRhs.x() + aLhs.y() * aRhs.y()) / (lhsLength * rhsLength)));
            return std::acos(cosAngle) * 180.0 / 3.14159265358979323846;
        };
        // What "visibly off the through-line" means in these assertions (a
        // measurement threshold, not editor state).
        const double kPairOffLine = 2.0;

        const QPointF keyB = ctrl::graph::keyDot(pairScale, core::Frame(30), 90.0f);

        // --- the LINKED authoring: B's incoming handle points left/down at 40
        // px, its outgoing points right/up at ~67 px - collinear, different
        // lengths, and the two segments' spans differ. A's outgoing and C's
        // incoming are authored to arbitrary values and must never move. The
        // link itself is B's STORED flag, not the geometry: set it explicitly.
        const QPointF wantIn = keyB + QPointF(-36.0, 18.0);
        const QPointF wantOut = keyB + QPointF(60.0, -30.0);
        const QPointF inNorm = normalizedAt(
            pairScale, pairCurve.segment(0), QPoint(static_cast<int>(wantIn.x()), static_cast<int>(wantIn.y()))
        );
        const QPointF outNorm = normalizedAt(
            pairScale, pairCurve.segment(1), QPoint(static_cast<int>(wantOut.x()), static_cast<int>(wantOut.y()))
        );
        const util::Easing::Param kAB = custom(0.4f, 0.1f, inNorm.x(), inNorm.y());
        const util::Easing::Param kBC = custom(outNorm.x(), outNorm.y(), 0.7f, 0.9f);
        CHECK(kBC.aligned); // a new key starts linked

        // --- the FREE authoring: B's incoming (segment A-B's x2,y2) and its
        // outgoing (segment B-C's x1,y1) point in unrelated directions AND the
        // key is stored FREE - that flag, not the angle, is what the editor
        // reads.
        util::Easing::Param kFreeAB = custom(0.4f, 0.1f, 0.8f, 0.9f);
        util::Easing::Param kFreeBC = custom(0.2f, -0.3f, 0.7f, 0.9f);
        kFreeAB.aligned = false;
        kFreeBC.aligned = false;

        auto setAligned = [&]() {
            fA->data().easing(0) = kAB;
            fB->data().easing(0) = kBC;
        };
        auto setFree = [&]() {
            fA->data().easing(0) = kFreeAB;
            fB->data().easing(0) = kFreeBC;
        };
        setAligned();

        {
            const Curve authored(*pairNode.timeLine(), pairChannel);
            const QPointF outPx = handlePx(pairScale, authored, authored.segment(1), 0);
            const QPointF inPx = handlePx(pairScale, authored, authored.segment(0), 1);
            const double angle = pairAngle(keyB, outPx, inPx);
            CHECK(std::fabs(angle - 180.0) <= kPairOffLine); // the key IS aligned
            CHECK(std::fabs(lengthPx(keyB, inPx) - lengthPx(keyB, outPx)) > 5.0); // and not a reflection
        }

        core::AbstractCursor pairCur;

        // (a) A LINKED KEY KEEPS ITS PAIR COLLINEAR, AT THE OTHER HANDLE'S OWN
        // LENGTH. Grab B's INCOMING handle (segment A-B's handle 1, stored in
        // A's param); B's OUTGOING one (segment B-C's handle 0, stored in B's
        // param) must end on the opposite ray at its PRE-DRAG pixel length -
        // deliberately not at the reflection, which would force the lengths
        // equal. B's stored flag is what decides this; it stays true.
        {
            CHECK(fB->data().easing(0).aligned); // the document says LINKED
            const Curve beforeA(*pairNode.timeLine(), pairChannel);
            const Curve::Segment abA = beforeA.segment(0);
            const Curve::Segment bcA = beforeA.segment(1);
            const QPoint grab = handlePixel(pairScale, abA, 1);
            const QPoint to = grab + QPoint(-10, 6);
            CHECK(panel.contains(grab));
            CHECK(panel.contains(to));
            const QPoint otherGrab = handlePixel(pairScale, abA, 0);
            CHECK(std::hypot(otherGrab.x() - grab.x(), otherGrab.y() - grab.y())
                > 2.0 * ctrl::graph::kHandleHitRadius); // unambiguous grab

            // PRECONDITION: the pair is collinear and its lengths differ.
            const QPointF outBefore = handlePx(pairScale, beforeA, bcA, 0);
            const QPointF inBefore = handlePx(pairScale, beforeA, abA, 1);
            const double otherLengthBefore = lengthPx(keyB, outBefore);
            CHECK(std::fabs(pairAngle(keyB, outBefore, inBefore) - 180.0) <= kPairOffLine);
            CHECK(std::fabs(otherLengthBefore - lengthPx(keyB, inBefore)) > 5.0);

            CHECK(sendPress(editor, camera, pairCur, grab));
            CHECK(sendMove(editor, camera, pairCur, to));
            CHECK(sendRelease(editor, camera, pairCur, to));

            const util::Easing::Param wroteAB = fA->data().easing(0);
            const util::Easing::Param wroteBC = fB->data().easing(0);
            const QPointF want = normalizedAt(pairScale, abA, to);
            CHECK(std::fabs(wroteAB.cubicBezier.x2 - want.x()) < 0.02); // grabbed at pointer
            CHECK(std::fabs(wroteAB.cubicBezier.y2 - want.y()) < 0.02);
            CHECK(wroteAB.cubicBezier.x1 == kAB.cubicBezier.x1); // A's outgoing untouched
            CHECK(wroteAB.cubicBezier.y1 == kAB.cubicBezier.y1);
            CHECK(wroteBC.cubicBezier.x2 == kBC.cubicBezier.x2); // C's incoming untouched
            CHECK(wroteBC.cubicBezier.y2 == kBC.cubicBezier.y2);
            CHECK(wroteBC.aligned); // the link survived the drag
            CHECK(wroteAB.aligned); // and A's own flag was not touched

            const Curve afterA(*pairNode.timeLine(), pairChannel);
            const QPointF outAfter = handlePx(pairScale, afterA, afterA.segment(1), 0);
            const QPointF inAfter = handlePx(pairScale, afterA, afterA.segment(0), 1);
            const double otherLengthAfter = lengthPx(keyB, outAfter);
            const double angle = pairAngle(keyB, outAfter, inAfter);
            CHECK(std::fabs(angle - 180.0) <= kPairOffLine); // collinear through B
            CHECK(std::fabs(otherLengthAfter - otherLengthBefore) < 0.75); // own length kept
            const double draggedLength = lengthPx(keyB, inAfter);
            CHECK(std::fabs(otherLengthAfter - draggedLength) > 5.0); // NOT a reflection

            bool undoneA = false; // ONE undo restores both params ...
            CHECK(!stack.undo(&undoneA).isEmpty());
            CHECK(undoneA);
            CHECK(fA->data().easing(0) == kAB);
            CHECK(fB->data().easing(0) == kBC);
            bool undoneAAgain = true;
            CHECK(stack.undo(&undoneAAgain).isEmpty()); // ... and nothing more
            CHECK(!undoneAAgain);

            std::printf(
                "[M] linked drag: B's incoming handle moved to (%.3f,%.3f); its outgoing stayed "
                "collinear at its own length %.2f px (was %.2f), %.3f deg from the through-line "
                "- not the %.2f px reflection; the stored link survived; one undo restored A and B\n",
                wroteAB.cubicBezier.x2, wroteAB.cubicBezier.y2, otherLengthAfter, otherLengthBefore,
                std::fabs(angle - 180.0), draggedLength
            );
        }

        // (b) ALT UNLINKS THE KEY FOR GOOD, AND THE LATEST MODIFIER STATE COUNTS.
        // The first move (no Alt) still carries B's opposite handle with it; Alt
        // pressed on the SECOND move must freeze that handle exactly where the
        // first move left it (byte-identical), even while the dragged handle
        // keeps following the pointer - AND write B FREE in the document. A LATER
        // DRAG, with no Alt at all, must then leave the partner alone too: that
        // sticky state is the user's report ("once it diverges with alt, the way
        // to get it back should be an alt click"). The gesture still has ONE
        // command, so one undo restores both params AND the flag.
        {
            setAligned();
            const Curve beforeB(*pairNode.timeLine(), pairChannel);
            const Curve::Segment abB = beforeB.segment(0);
            const Curve::Segment bcB = beforeB.segment(1);
            const QPoint grab = handlePixel(pairScale, abB, 1);
            const QPoint step1 = grab + QPoint(-12, 8);
            const QPoint step2(step1.x(), step1.y() - 40); // off the original ray
            CHECK(panel.contains(grab));
            CHECK(panel.contains(step1));
            CHECK(panel.contains(step2));

            // PRECONDITION: linked before the act.
            CHECK(fB->data().easing(0).aligned);
            const QPointF outBefore = handlePx(pairScale, beforeB, bcB, 0);
            const QPointF inBefore = handlePx(pairScale, beforeB, abB, 1);
            CHECK(std::fabs(pairAngle(keyB, outBefore, inBefore) - 180.0) <= kPairOffLine);

            CHECK(sendPress(editor, camera, pairCur, grab));
            CHECK(sendMove(editor, camera, pairCur, step1)); // no Alt: the pair follows
            const util::Easing::Param mirrored = fB->data().easing(0);
            CHECK(mirrored != kBC); // the opposite handle was written ...
            CHECK(mirrored.aligned); // ... and the link is still on
            CHECK(fB->data().easing(0) == mirrored);

            CHECK(sendMove(editor, camera, pairCur, step2, Qt::AltModifier)); // Alt now ...
            CHECK(fB->data().easing(0).cubicBezier == mirrored.cubicBezier); // ... the opposite handle is untouched
            CHECK(!fB->data().easing(0).aligned); // ... and the document is FREE for good
            const QPointF want2 = normalizedAt(pairScale, abB, step2);
            CHECK(std::fabs(fA->data().easing(0).cubicBezier.x2 - want2.x()) < 0.02); // the grabbed one still moves
            CHECK(std::fabs(fA->data().easing(0).cubicBezier.y2 - want2.y()) < 0.02);

            CHECK(sendMove(editor, camera, pairCur, step2 + QPoint(-6, 4), Qt::AltModifier));
            CHECK(fB->data().easing(0).cubicBezier == mirrored.cubicBezier); // every further Alt step leaves it alone
            CHECK(!fB->data().easing(0).aligned);
            CHECK(sendRelease(editor, camera, pairCur, step2, Qt::AltModifier));
            const util::Easing::Param afterAltB = fB->data().easing(0);
            CHECK(afterAltB.cubicBezier == mirrored.cubicBezier);
            CHECK(!afterAltB.aligned);

            const Curve afterB(*pairNode.timeLine(), pairChannel);
            const QPointF outAfter = handlePx(pairScale, afterB, afterB.segment(1), 0);
            const QPointF inAfter = handlePx(pairScale, afterB, afterB.segment(0), 1);
            const double angle = pairAngle(keyB, outAfter, inAfter);
            CHECK(std::fabs(angle - 180.0) > kPairOffLine); // off-line, and that is allowed

            // (b2) THE STICKY PART: a NEW drag with no Alt must not move the
            // partner. Before the stored flag existed the next drag re-inferred
            // the state from the pixel angle and could snap the partner back;
            // now the document says FREE and stays free.
            {
                const QPoint grab2 = handlePixel(pairScale, afterB.segment(0), 1);
                const QPoint to2 = grab2 + QPoint(-9, 11);
                CHECK(panel.contains(grab2));
                CHECK(panel.contains(to2));
                CHECK(sendPress(editor, camera, pairCur, grab2));
                CHECK(sendMove(editor, camera, pairCur, to2)); // NO Alt
                CHECK(sendRelease(editor, camera, pairCur, to2));
                CHECK(fB->data().easing(0) == afterAltB); // the partner is byte-identical
                const QPointF want3 = normalizedAt(pairScale, afterB.segment(0), to2);
                CHECK(std::fabs(fA->data().easing(0).cubicBezier.x2 - want3.x()) < 0.02); // only the grabbed one moved
                CHECK(std::fabs(fA->data().easing(0).cubicBezier.y2 - want3.y()) < 0.02);

                bool undoneB2 = false;
                CHECK(!stack.undo(&undoneB2).isEmpty()); // close the second drag ...
                CHECK(undoneB2);
                CHECK(fB->data().easing(0) == afterAltB);
            }

            bool undoneB = false; // the first step's mirror AND the flag are one command
            CHECK(!stack.undo(&undoneB).isEmpty());
            CHECK(undoneB);
            CHECK(fA->data().easing(0) == kAB);
            CHECK(fB->data().easing(0) == kBC); // flag true again
            CHECK(fB->data().easing(0).aligned);
            bool undoneBAgain = true;
            CHECK(stack.undo(&undoneBAgain).isEmpty());
            CHECK(!undoneBAgain);

            std::printf(
                "[M] alt: the opposite handle was written by step 1 and stayed byte-identical through "
                "two Alt steps; the document went FREE, so a LATER drag without Alt left the partner "
                "alone too; the pair ended %.3f deg off the through-line; one undo restored both params "
                "AND the link\n",
                std::fabs(angle - 180.0)
            );
        }

        // (c) A FREE KEY DOES NOT ACQUIRE A PAIR. B's stored flag is false, so
        // dragging either handle without Alt must leave the OTHER param
        // byte-identical - the pair is only preserved when the DOCUMENT says it
        // is linked. (The geometry here is also off-line, but that is not what
        // the editor reads.)
        {
            setFree();
            const Curve beforeC(*pairNode.timeLine(), pairChannel);
            const Curve::Segment abC = beforeC.segment(0);
            const Curve::Segment bcC = beforeC.segment(1);

            // PRECONDITION: B is stored FREE before the act.
            CHECK(!fB->data().easing(0).aligned);
            const QPointF outC = handlePx(pairScale, beforeC, bcC, 0);
            const QPointF inC = handlePx(pairScale, beforeC, abC, 1);
            CHECK(std::fabs(pairAngle(keyB, outC, inC) - 180.0) > kPairOffLine);

            // (c1) B's INCOMING handle (stored in A's param): B's param is the
            // other half and must not move.
            {
                const QPoint grab = handlePixel(pairScale, abC, 1);
                const QPoint to = grab + QPoint(-24, 14);
                CHECK(panel.contains(grab));
                CHECK(panel.contains(to));
                CHECK(sendPress(editor, camera, pairCur, grab));
                CHECK(sendMove(editor, camera, pairCur, to));
                CHECK(sendRelease(editor, camera, pairCur, to));

                CHECK(fB->data().easing(0) == kFreeBC); // the free opposite, untouched
                CHECK(fA->data().easing(0).cubicBezier.x2
                        != kFreeAB.cubicBezier.x2
                      || fA->data().easing(0).cubicBezier.y2 != kFreeAB.cubicBezier.y2);
                bool undoneC1 = false;
                CHECK(!stack.undo(&undoneC1).isEmpty());
                CHECK(undoneC1);
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
            }

            // (c2) B's OUTGOING handle (stored in B's param): A's param is the
            // other half and must not move.
            {
                const QPoint grab = handlePixel(pairScale, bcC, 0);
                const QPoint to = grab + QPoint(24, -16);
                CHECK(panel.contains(grab));
                CHECK(panel.contains(to));
                CHECK(sendPress(editor, camera, pairCur, grab));
                CHECK(sendMove(editor, camera, pairCur, to));
                CHECK(sendRelease(editor, camera, pairCur, to));

                CHECK(fA->data().easing(0) == kFreeAB); // the free opposite, untouched
                CHECK(fB->data().easing(0).cubicBezier.x1
                        != kFreeBC.cubicBezier.x1
                      || fB->data().easing(0).cubicBezier.y1 != kFreeBC.cubicBezier.y1);
                bool undoneC2 = false;
                CHECK(!stack.undo(&undoneC2).isEmpty());
                CHECK(undoneC2);
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
            }

            std::printf(
                "[M] free key: both drags left the opposite param byte-identical (the stored flag was "
                "false; B's pair was %.3f deg from the through-line)\n",
                std::fabs(pairAngle(keyB, outC, inC) - 180.0)
            );
        }

        // (d) A KEY MOVED ON THE VALUE AXIS KEEPS ITS LINK AND BOTH HANDLES.
        // This is the user's report: with the old geometric inference, moving B
        // on the value axis changed the pair's PIXEL angle and the state flipped
        // under the user's hands, so the next handle drag snapped the partner
        // onto a line the pair no longer was on. The move must write neither
        // param, the flag must stay true, and a HANDLE drag afterwards must still
        // carry the partner (at its own length).
        {
            setAligned();
            const util::Easing::Param keepAB = fA->data().easing(0);
            const util::Easing::Param keepBC = fB->data().easing(0);
            CHECK(keepBC.aligned);
            const float beforeValue = fB->rotate();

            const QPoint grab = keyB.toPoint();
            const QPoint to = grab + QPoint(0, 36); // value axis only
            CHECK(panel.contains(grab));
            CHECK(panel.contains(to));
            CHECK(sendPress(editor, camera, pairCur, grab));
            CHECK(sendMove(editor, camera, pairCur, to));
            CHECK(sendRelease(editor, camera, pairCur, to));

            CHECK(fB->rotate() != beforeValue); // the key moved ...
            const float movedValue = fB->rotate();
            CHECK(fA->data().easing(0) == keepAB); // ... and neither param was rewritten ...
            CHECK(fB->data().easing(0) == keepBC); // ... flag and handles included
            CHECK(fB->data().easing(0).aligned);

            // The pair is now visibly OFF the through-line in pixels (the same
            // stored handles map through different value spans) - exactly the
            // state the inference used to misread. The link is still true.
            const QPointF keyMoved =
                ctrl::graph::keyDot(pairScale, core::Frame(30), fB->rotate());
            const Curve moved(*pairNode.timeLine(), pairChannel);
            const QPointF outMoved = handlePx(pairScale, moved, moved.segment(1), 0);
            const QPointF inMoved = handlePx(pairScale, moved, moved.segment(0), 1);
            CHECK(std::fabs(pairAngle(keyMoved, outMoved, inMoved) - 180.0) > kPairOffLine);
            const double partnerLength = lengthPx(keyMoved, inMoved);

            // A drag on B's OUTGOING handle, NO Alt: the incoming partner (A's
            // param) must follow, collinear, at its own length.
            const QPoint grabH = handlePixel(pairScale, moved.segment(1), 0);
            const QPoint toH = grabH + QPoint(14, -9);
            CHECK(panel.contains(grabH));
            CHECK(panel.contains(toH));
            CHECK(sendPress(editor, camera, pairCur, grabH));
            CHECK(sendMove(editor, camera, pairCur, toH));
            CHECK(sendRelease(editor, camera, pairCur, toH));

            CHECK(fB->data().easing(0).aligned); // the move did not unlink it
            CHECK(fA->data().easing(0).cubicBezier.x1 == keepAB.cubicBezier.x1); // A's own outgoing untouched
            CHECK(fA->data().easing(0).cubicBezier.y1 == keepAB.cubicBezier.y1);
            CHECK(fA->data().easing(0).cubicBezier.x2 != keepAB.cubicBezier.x2
                  || fA->data().easing(0).cubicBezier.y2 != keepAB.cubicBezier.y2); // the partner DID move
            const Curve redrawn(*pairNode.timeLine(), pairChannel);
            const QPointF outRedrawn = handlePx(pairScale, redrawn, redrawn.segment(1), 0);
            const QPointF inRedrawn = handlePx(pairScale, redrawn, redrawn.segment(0), 1);
            CHECK(std::fabs(pairAngle(keyMoved, outRedrawn, inRedrawn) - 180.0) <= 2.0); // still through B
            CHECK(std::fabs(lengthPx(keyMoved, inRedrawn) - partnerLength) < 1.0); // own length kept

            bool undoneH = false; // the handle drag, one undo ...
            CHECK(!stack.undo(&undoneH).isEmpty());
            CHECK(undoneH);
            CHECK(fA->data().easing(0) == keepAB);
            CHECK(fB->data().easing(0) == keepBC);
            bool undoneMove = false; // ... then the key move
            CHECK(!stack.undo(&undoneMove).isEmpty());
            CHECK(undoneMove);
            CHECK(fB->rotate() == beforeValue);
            CHECK(fA->data().easing(0) == keepAB); // the move never touched the handles
            CHECK(fB->data().easing(0) == keepBC);

            std::printf(
                "[M] value move: B went %.1f -> %.1f (its pair %.3f deg off the through-line "
                "afterwards) and BOTH params stayed byte-identical; a following drag still carried "
                "the partner, at its own length\n",
                beforeValue, movedValue,
                std::fabs(pairAngle(keyMoved, outMoved, inMoved) - 180.0)
            );
        }

        // (e) ALT+CLICK LINKS A FREE KEY ALONG THE NEIGHBOUR CHORD. Each handle
        // keeps its own length, and the direction comes from the CHORD from the
        // PREVIOUS key's anchor to the NEXT key's anchor - never from which
        // handle is longer. The case is built so the OLD rule (longer handle
        // wins) would have kept the OUTGOING handle's horizontal direction,
        // clearly off the chord: the assertions below would fail under it.
        // One command writes both params AND the flag, so one undo brings the
        // free pair back whole.
        {
            const QPointF keyE = ctrl::graph::keyDot(pairScale, core::Frame(30), 90.0f);

            // Author the free pair by PIXEL, then read it back the way the editor
            // draws it (the authored pixels are quantized to ints by normalizedAt,
            // so the read-back is the truth the assertions compare against).
            const QPointF wantOut = keyE + QPointF(55.0, 0.0); // long, horizontal
            const QPointF wantIn = keyE + QPointF(-20.0, 11.0); // short, opposite-ish
            const QPointF inNormE = normalizedAt(
                pairScale, pairCurve.segment(0),
                QPoint(static_cast<int>(wantIn.x()), static_cast<int>(wantIn.y()))
            );
            const QPointF outNormE = normalizedAt(
                pairScale, pairCurve.segment(1),
                QPoint(static_cast<int>(wantOut.x()), static_cast<int>(wantOut.y()))
            );
            util::Easing::Param freeAB = custom(0.4f, 0.1f, inNormE.x(), inNormE.y());
            util::Easing::Param freeBC = custom(outNormE.x(), outNormE.y(), 0.7f, 0.9f);
            freeAB.aligned = false;
            freeBC.aligned = false;
            fA->data().easing(0) = freeAB;
            fB->data().easing(0) = freeBC;

            const Curve beforeE(*pairNode.timeLine(), pairChannel);
            const QPointF outBefore = handlePx(pairScale, beforeE, beforeE.segment(1), 0);
            const QPointF inBefore = handlePx(pairScale, beforeE, beforeE.segment(0), 1);
            const double outLength = lengthPx(keyE, outBefore);
            const double inLength = lengthPx(keyE, inBefore);

            // The chord from A's anchor to C's anchor, in pixels.
            const QPointF prevPixel =
                ctrl::graph::keyDot(pairScale, core::Frame(0), pairCurve.segment(0).fromValue);
            const QPointF nextPixel =
                ctrl::graph::keyDot(pairScale, core::Frame(40), pairCurve.segment(1).toValue);
            const QPointF chord(nextPixel.x() - prevPixel.x(), nextPixel.y() - prevPixel.y());
            const QPointF outVector(outBefore.x() - keyE.x(), outBefore.y() - keyE.y());

            // PRECONDITION: free, the OUTGOING handle is the longer one, and the
            // old rule's direction (its own) is clearly off the chord.
            CHECK(!fB->data().easing(0).aligned);
            CHECK(outLength > inLength + 1.0);
            const double oldRuleOffChord = directionAngle(outVector, chord);
            CHECK(oldRuleOffChord > 5.0);

            const QPoint click = keyE.toPoint();
            CHECK(panel.contains(click));
            CHECK(sendPress(editor, camera, pairCur, click, Qt::AltModifier)); // press+release = click
            CHECK(!sendRelease(editor, camera, pairCur, click, Qt::AltModifier)); // no drag to close

            const util::Easing::Param afterAB = fA->data().easing(0);
            const util::Easing::Param afterBC = fB->data().easing(0);
            CHECK(afterBC.aligned); // the DOCUMENT is linked now
            CHECK(!afterAB.aligned); // ... and only the clicked key's flag was written
            const Curve afterE(*pairNode.timeLine(), pairChannel);
            const QPointF outAfter = handlePx(pairScale, afterE, afterE.segment(1), 0);
            const QPointF inAfter = handlePx(pairScale, afterE, afterE.segment(0), 1);
            const QPointF outAfterVector(outAfter.x() - keyE.x(), outAfter.y() - keyE.y());
            const QPointF inAfterVector(inAfter.x() - keyE.x(), inAfter.y() - keyE.y());

            // THE CHORD WINS, on both sides, at the handles' own lengths.
            CHECK(directionAngle(outAfterVector, chord) <= 3.0); // outgoing along the chord
            const QPointF oppositeChord(-chord.x(), -chord.y());
            CHECK(directionAngle(inAfterVector, oppositeChord) <= 3.0); // incoming on its other side
            const double pairOff = std::fabs(pairAngle(keyE, outAfter, inAfter) - 180.0);
            CHECK(pairOff <= 2.5); // one straight line through the key
            CHECK(std::fabs(lengthPx(keyE, outAfter) - outLength) < 1.0); // lengths preserved
            CHECK(std::fabs(lengthPx(keyE, inAfter) - inLength) < 1.0);

            bool undoneE = false;
            CHECK(!stack.undo(&undoneE).isEmpty());
            CHECK(undoneE);
            CHECK(fA->data().easing(0) == freeAB); // both params AND the flag, one undo
            CHECK(fB->data().easing(0) == freeBC);
            CHECK(!fB->data().easing(0).aligned);
            bool undoneEAgain = true;
            CHECK(stack.undo(&undoneEAgain).isEmpty());
            CHECK(!undoneEAgain);

            std::printf(
                "[M] alt+click link: the pair was authored free with the OUTGOING handle longest "
                "(%.2f px vs %.2f px) and %.1f deg off the neighbour chord; the click put BOTH "
                "handles on the chord (out %.2f deg, in %.2f deg off) at their own lengths and set "
                "the flag; one undo restored the free pair\n",
                outLength, inLength, oldRuleOffChord,
                directionAngle(outAfterVector, chord),
                directionAngle(inAfterVector, oppositeChord)
            );
        }

        // (g) THE FIRST AND LAST KEYS HAVE ONE HANDLE EACH. A drag writes that
        // handle and leaves the other param bit-identical (no opposite exists),
        // and Alt+click is inert: nothing to link, no command, no crash.
        {
            setFree();
            const Curve beforeF(*pairNode.timeLine(), pairChannel);
            const Curve::Segment abF = beforeF.segment(0);
            const Curve::Segment bcF = beforeF.segment(1);

            // (g1) FIRST KEY A: drag its only (outgoing) handle.
            {
                const QPoint grab = handlePixel(pairScale, abF, 0);
                const QPoint to = grab + QPoint(24, -16);
                CHECK(panel.contains(grab));
                CHECK(panel.contains(to));
                CHECK(sendPress(editor, camera, pairCur, grab));
                CHECK(sendMove(editor, camera, pairCur, to));
                CHECK(sendRelease(editor, camera, pairCur, to));

                const util::Easing::Param wroteAB = fA->data().easing(0);
                const QPointF want = normalizedAt(pairScale, abF, to);
                CHECK(std::fabs(wroteAB.cubicBezier.x1 - want.x()) < 0.02);
                CHECK(std::fabs(wroteAB.cubicBezier.y1 - want.y()) < 0.02);
                CHECK(wroteAB.cubicBezier.x2 == kFreeAB.cubicBezier.x2); // the segment's other point
                CHECK(wroteAB.cubicBezier.y2 == kFreeAB.cubicBezier.y2);
                CHECK(fB->data().easing(0) == kFreeBC); // the next key's param

                bool undoneF1 = false;
                CHECK(!stack.undo(&undoneF1).isEmpty());
                CHECK(undoneF1);
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
            }

            // (g2) ALT+CLICK ON THE FIRST KEY: inert.
            {
                const QPoint keyA =
                    ctrl::graph::keyDot(pairScale, core::Frame(0), 0.0f).toPoint();
                CHECK(panel.contains(keyA));
                CHECK(sendPress(editor, camera, pairCur, keyA, Qt::AltModifier));
                CHECK(!sendRelease(editor, camera, pairCur, keyA, Qt::AltModifier)); // inert: no drag
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
                bool nothing = true; // no command was pushed
                CHECK(stack.undo(&nothing).isEmpty());
                CHECK(!nothing);
            }

            // (g3) LAST KEY C: drag its only (incoming) handle.
            {
                const QPoint grab = handlePixel(pairScale, bcF, 1);
                const QPoint to = grab + QPoint(-20, 14);
                CHECK(panel.contains(grab));
                CHECK(panel.contains(to));
                CHECK(sendPress(editor, camera, pairCur, grab));
                CHECK(sendMove(editor, camera, pairCur, to));
                CHECK(sendRelease(editor, camera, pairCur, to));

                const util::Easing::Param wroteBC = fB->data().easing(0);
                const QPointF want = normalizedAt(pairScale, bcF, to);
                CHECK(std::fabs(wroteBC.cubicBezier.x2 - want.x()) < 0.02);
                CHECK(std::fabs(wroteBC.cubicBezier.y2 - want.y()) < 0.02);
                CHECK(wroteBC.cubicBezier.x1 == kFreeBC.cubicBezier.x1); // the segment's other point
                CHECK(wroteBC.cubicBezier.y1 == kFreeBC.cubicBezier.y1);
                CHECK(fA->data().easing(0) == kFreeAB); // the previous key's param

                bool undoneF3 = false;
                CHECK(!stack.undo(&undoneF3).isEmpty());
                CHECK(undoneF3);
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
            }

            // (g4) ALT+CLICK ON THE LAST KEY: inert.
            {
                const QPoint keyC =
                    ctrl::graph::keyDot(pairScale, core::Frame(40), 150.0f).toPoint();
                CHECK(panel.contains(keyC));
                CHECK(sendPress(editor, camera, pairCur, keyC, Qt::AltModifier));
                CHECK(!sendRelease(editor, camera, pairCur, keyC, Qt::AltModifier)); // inert: no drag
                CHECK(fA->data().easing(0) == kFreeAB);
                CHECK(fB->data().easing(0) == kFreeBC);
                bool nothing = true;
                CHECK(stack.undo(&nothing).isEmpty());
                CHECK(!nothing);
            }

            std::printf(
                "[M] ends: the first and last keys wrote only their one handle, left the neighbouring "
                "param byte-identical, and Alt+click on either was inert (no command, no crash)\n"
            );
        }

        // (h) AN AUTO NEIGHBOUR IS STILL CONVERTED TO CUSTOM WHEN IT IS WRITTEN.
        // Switch segment B-C to Type_Auto (its outgoing pair is DERIVED), store B
        // LINKED, author B's incoming handle to be exactly opposite the derived
        // OUTGOING one, then drag the incoming handle: the written opposite must
        // become Custom carrying the derived pair, keep its derived LENGTH, stay
        // collinear - and one undo must bring the Auto param back whole.
        {
            fB->data().easing(0).type = util::Easing::Type_Auto; // a document write, not a gesture
            fB->data().easing(0).aligned = true; // the link is the stored flag
            const util::Easing::Param kAutoBC = fB->data().easing(0);

            const Curve autoCurve(*pairNode.timeLine(), pairChannel);
            const Curve::Segment abG = autoCurve.segment(0);
            const Curve::Segment bcG = autoCurve.segment(1);
            const QVector<QPointF> ghosts = autoCurve.ghostHandles(*bcG.from);
            CHECK(ghosts.size() == 2);
            const QPointF ghostOut = handlePx(pairScale, autoCurve, bcG, 0);
            const double ghostLength = lengthPx(keyB, ghostOut);
            CHECK(ghostLength > 0.0);

            const QPointF ghostDir(ghostOut.x() - keyB.x(), ghostOut.y() - keyB.y());
            const QPointF wantIn(
                keyB.x() - ghostDir.x() / ghostLength * 60.0,
                keyB.y() - ghostDir.y() / ghostLength * 60.0
            );
            const QPointF inNormG = normalizedAt(pairScale, abG, wantIn.toPoint());
            util::Easing::Param authoredAB = kFreeAB;
            authoredAB.cubicBezier.x2 = static_cast<float>(inNormG.x());
            authoredAB.cubicBezier.y2 = static_cast<float>(inNormG.y());
            fA->data().easing(0) = authoredAB;

            // PRECONDITION: aligned against the DERIVED outgoing handle. The
            // segment copies above predate the authoring, so read them again.
            const Curve beforeG(*pairNode.timeLine(), pairChannel);
            const QPointF outBefore = handlePx(pairScale, beforeG, beforeG.segment(1), 0);
            const QPointF inBefore = handlePx(pairScale, beforeG, beforeG.segment(0), 1);
            CHECK(std::fabs(pairAngle(keyB, outBefore, inBefore) - 180.0) <= kPairOffLine);

            const QPoint grab = handlePixel(pairScale, beforeG.segment(0), 1);
            const QPoint to = grab + QPoint(-10, 8);
            CHECK(panel.contains(grab));
            CHECK(panel.contains(to));

            CHECK(sendPress(editor, camera, pairCur, grab));
            CHECK(fB->data().easing(0).type == util::Easing::Type_Auto); // the press alone edits nothing
            CHECK(sendMove(editor, camera, pairCur, to));
            CHECK(sendRelease(editor, camera, pairCur, to));

            const util::Easing::Param wroteAB = fA->data().easing(0);
            const util::Easing::Param wroteBC = fB->data().easing(0);
            CHECK(wroteBC.type == util::Easing::Type_Custom); // written => the derived pair was made storable
            // The pair's OTHER control point is key C's incoming handle and
            // stays at its derived third.
            CHECK(std::fabs(wroteBC.cubicBezier.x2 - static_cast<float>(ghosts[1].x())) < 1e-6f);
            CHECK(std::fabs(wroteBC.cubicBezier.y2 - static_cast<float>(ghosts[1].y())) < 1e-6f);
            CHECK(wroteAB.cubicBezier.x1 == authoredAB.cubicBezier.x1); // A's outgoing untouched
            CHECK(wroteAB.cubicBezier.y1 == authoredAB.cubicBezier.y1);

            const Curve afterG(*pairNode.timeLine(), pairChannel);
            const QPointF outAfter = handlePx(pairScale, afterG, afterG.segment(1), 0);
            const QPointF inAfter = handlePx(pairScale, afterG, afterG.segment(0), 1);
            CHECK(std::fabs(lengthPx(keyB, outAfter) - ghostLength) < 0.75); // derived length kept
            const double angle = pairAngle(keyB, outAfter, inAfter);
            CHECK(std::fabs(angle - 180.0) <= kPairOffLine); // collinear through B

            bool undoneG = false;
            CHECK(!stack.undo(&undoneG).isEmpty());
            CHECK(undoneG);
            CHECK(fA->data().easing(0) == authoredAB);
            CHECK(fB->data().easing(0) == kAutoBC); // the Auto easing, whole

            std::printf(
                "[M] auto neighbour: a written Type_Auto became Custom carrying the derived pair, kept "
                "its %.2f px length and stayed %.3f deg from the through-line; one undo restored the "
                "Auto easing and A\n",
                ghostLength, std::fabs(angle - 180.0)
            );
        }

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 24: THE PANEL GOING AWAY. The editor has no shown/hidden state any more -
    // the dock's Graph page IS the panel while its tab is current, so nothing in
    // this class could describe it, and the Driver's viewport routing no longer asks
    // - and this sits last so nothing above or below runs against a panel that went
    // away. What a hide still has to do is the ONE side effect hiding ever had,
    // through its own door: GraphEditor::panelHidden(), which
    // gui::GraphPanelWidget::hideEvent calls. It DROPS the gesture in flight, and
    // this section drives it with a key move actually open, then pins what the drop
    // must NOT do: undo it (the command stays on the stack, the keys stay where the
    // drag left them) or leave it rewritable (a further move, with the button still
    // down as far as this synthetic cursor knows, changes nothing).
    {
        core::TestNode awayNode;
        core::RotateKey* aw0 = new core::RotateKey();
        aw0->setRotate(0.0f);
        setup.push(awayNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, aw0));
        // TWO keys, and the second one is not decoration: fitAll() fits the FRAME
        // axis to the SHOWN keys (fitFrameAxis: the span of their frames, plus a
        // tenth of it, at least one frame each side), and a ONE-key channel fits
        // frames -1..1 - so the frames this drag crosses (0 -> 4 -> 9) would not be
        // in the plot at all and the preconditions below would be about a pixel the
        // section never draws. The second key gives the fit the span the section
        // needs, and it doubles as the "the key the press did not select is
        // untouched" check.
        core::RotateKey* aw1 = new core::RotateKey();
        aw1->setRotate(60.0f);
        setup.push(awayNode.timeLine()->createPusher(core::TimeKeyType_Rotate, 12, aw1));
        CHECK(awayNode.timeLine()->map(core::TimeKeyType_Rotate).size() == 2);

        editor.setTargets({ &awayNode });
        editor.scale().setRect(panel);
        editor.fitAll();
        project.commandStack().clear();

        const Scaler& awayScale = editor.scale();
        const QPoint awayFrom(awayScale.pixelX(core::Frame(0)), awayScale.pixelY(0.0f));
        const QPoint awayTo(awayScale.pixelX(core::Frame(4)), awayScale.pixelY(0.0f));
        const QPoint awayFurther(awayScale.pixelX(core::Frame(9)), awayScale.pixelY(0.0f));
        for (const QPoint& pixel : { awayFrom, awayTo, awayFurther }) {
            CHECK(awayScale.plotRect().contains(pixel));
        }
        const int awayStep = awayScale.frameAt(awayTo.x()).get() - awayScale.frameAt(awayFrom.x()).get();
        CHECK(awayStep == 4);

        core::AbstractCursor awayCur;
        CHECK(sendPress(editor, camera, awayCur, awayFrom));
        CHECK(sendMove(editor, camera, awayCur, awayTo));
        CHECK(editor.isMovingKeys()); // a key move is in flight ...
        CHECK(aw0->frame() == awayStep);
        CHECK(!awayNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));

        // ... and the page going away drops it.
        editor.panelHidden();
        CHECK(!editor.isMovingKeys());
        CHECK(!editor.hasMarquee());
        CHECK(!editor.isPanning());

        // The drop is NOT an undo: the keys are where the drag left them, the
        // command is still the stack's one entry, and no later move can rewrite it.
        sendMove(editor, camera, awayCur, awayFurther);
        CHECK(aw0->frame() == awayStep);
        CHECK(awayNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, awayStep));
        CHECK(!editor.isMovingKeys());

        bool undoneAway = false;
        CHECK(!stack.undo(&undoneAway).isEmpty()); // the whole gesture, in ONE undo
        CHECK(undoneAway);
        CHECK(awayNode.timeLine()->hasTimeKey(core::TimeKeyType_Rotate, 0));
        CHECK(aw0->frame() == 0);
        CHECK(aw1->frame() == 12); // the key the press did not select never moved
        bool undoneAwayAgain = true;
        CHECK(stack.undo(&undoneAwayAgain).isEmpty()); // and nothing sits behind it
        CHECK(!undoneAwayAgain);

        std::printf(
            "[Z] panelHidden(): a key move open (frame 0 -> %d) was dropped - not moving, the "
            "keys left where the drag put them - and its ONE command still undid the whole gesture\n",
            awayStep
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    // --- 26: PER-CHANNEL HANDLES. A Move key's x and y are independent curves.
    // Dragging the X handle must edit ONLY component 0's param; the Y param is
    // the user's report, and it must come out of the gesture byte-identical.
    {
        core::TestNode moveNode;
        core::MoveKey* m0 = new core::MoveKey();
        m0->setPos(QVector2D(0.0f, 0.0f));
        core::MoveKey* m1 = new core::MoveKey();
        m1->setPos(QVector2D(80.0f, 40.0f));

        // The two components start on DIFFERENT authored curves, so an edit that
        // leaked across components could not hide behind equal values.
        util::Easing::Param xAuthored = customEasing();
        xAuthored.cubicBezier.x1 = 0.4f;
        xAuthored.cubicBezier.y1 = 0.1f;
        xAuthored.cubicBezier.x2 = 0.6f;
        xAuthored.cubicBezier.y2 = 0.9f;
        util::Easing::Param yAuthored = customEasing();
        yAuthored.cubicBezier.x1 = 0.2f;
        yAuthored.cubicBezier.y1 = 0.3f;
        yAuthored.cubicBezier.x2 = 0.7f;
        yAuthored.cubicBezier.y2 = 0.8f;
        CHECK(xAuthored != yAuthored);
        m0->data().easing(0) = xAuthored;
        m0->data().easing(1) = yAuthored;

        setup.push(moveNode.timeLine()->createPusher(core::TimeKeyType_Move, 0, m0));
        setup.push(moveNode.timeLine()->createPusher(core::TimeKeyType_Move, 24, m1));

        editor.setTargets({ &moveNode });
        editor.scale().setRect(panel);
        project.commandStack().clear();

        const Channel moveX(core::TimeKeyType_Move, 0);
        const Channel moveY(core::TimeKeyType_Move, 1);
        const Curve authoredX(*moveNode.timeLine(), moveX);
        const Curve authoredY(*moveNode.timeLine(), moveY);
        CHECK(authoredX.segmentCount() == 1 && authoredY.segmentCount() == 1);
        CHECK(authoredX.segment(0).easing == xAuthored);
        CHECK(authoredY.segment(0).easing == yAuthored);

        const QPoint grab = handlePixel(editor.scale(), authoredX.segment(0), 0);
        const QPoint want(grab.x() + 40, grab.y() + 25);
        core::AbstractCursor curveCur;
        CHECK(sendPress(editor, camera, curveCur, grab));
        CHECK(sendMove(editor, camera, curveCur, want));
        CHECK(sendRelease(editor, camera, curveCur, want));

        // X moved, Y did not - param for param and field for field.
        CHECK(m0->data().easing(0) != xAuthored);
        CHECK(m0->data().easing(1) == yAuthored);
        CHECK(!(m0->data().easing(0).cubicBezier == yAuthored.cubicBezier));

        // The drawn curves agree, and playback reads the same split: the Y
        // channel's segment still carries the untouched authored pair.
        const Curve afterX(*moveNode.timeLine(), moveX);
        const Curve afterY(*moveNode.timeLine(), moveY);
        CHECK(afterX.segment(0).easing == m0->data().easing(0));
        CHECK(afterY.segment(0).easing == yAuthored);

        // ONE undo puts X back and leaves Y where it always was.
        bool undone = false;
        CHECK(!project.commandStack().undo(&undone).isEmpty());
        CHECK(undone);
        CHECK(m0->data().easing(0) == xAuthored);
        CHECK(m0->data().easing(1) == yAuthored);
        CHECK(project.commandStack().undo(&undone).isEmpty()); // exactly one step was pushed

        std::printf(
            "[26] per-channel handle drag: Move x edited alone, Move y byte-identical, one undo\n"
        );

        editor.setTargets({}); // drop the target before its owner dies
    }

    std::printf(
        "graph_drag: one handle drag is one undo step, and the drag edits the key's own easing param "
        "at the pixel it points to - the grabbed handle where the pointer is, and the OPPOSITE handle "
        "of the SAME key in the neighbouring segment (outgoing -> the previous segment's x2,y2, "
        "incoming -> the next segment's x1,y1) kept collinear through the key in pixels at its OWN "
        "length when the key's STORED flag says linked, untouched when free, and untouched on any Alt "
        "step - which also writes the key FREE for good, so a later drag without Alt stays free; a key "
        "moved on the value axis keeps the flag and both handles byte-identical; Alt+click links the "
        "pair along the neighbour keys' chord, each handle keeping its own length; no pair for a "
        "channel's first or last key; the value axis is fitted to the shown curves where the panel is "
        "opened or re-selected, and a manual, wheeled or panned window survives every cursor pass; "
        "the panel owns the cursor on a key, a handle, a LEFT PRESS inside the plot (a key "
        "selects and drags, Ctrl on a curve's band inserts, everything else marquees) and the middle "
        "button (a pan), or mid-gesture - a press outside the plot, and every hover, stays the "
        "viewport tools'; the selection is a set of MARKERS - one press selects the one component "
        "under the pointer (Scale X alone, its Y sibling left with the faint-ring predicate) unless "
        "that marker is already selected, in which case the press grabs the selection as it stands, "
        "Shift adds a second marker, Ctrl toggles one, a marquee and ctrl+A take every marker of every "
        "key they enclose as one whole-key entry (a release within 4px of the press on both axes being "
        "the CLICK that clears instead), Escape clears, and the set announces every change of its "
        "own and only the changes; a drag of that selection moves one key's frame ONCE and every "
        "selected marker in its OWN channel by the same delta, so a key's unselected component comes "
        "out of the gesture bit-identical, all inside ONE command, and the drag starts on ANY of the "
        "selected keys - a plain press on one of them keeps the selection, so the whole selection "
        "moves; the focus moves NO window (a click "
        "does not zoom); the wheel ZOOMS only - both axes plain, the frame axis alone under Shift, the value "
        "axis alone under Ctrl, both under Ctrl+Shift, so no notch ever scrolls and none is dead - and its "
        "DIRECTION is the caller's invert setting - the one the canvas takes - applied to both axes before "
        "the frame half is delegated, so the canvas, the timeline and the panel never zoom against each "
        "other; the "
        "three key edits are one undo step each through the timeline's own route: a Ctrl-press on the "
        "band inserts the key the curve already passes through (no sampled value moves at that "
        "frame), a press on a key plus a 2-axis drag moves its frame AND its value (Shift pins the "
        "dominant axis) and refuses an occupied destination, and removeSelectedKey() removes every "
        "selected key and restores the same key objects on undo; a key drag moves the WHOLE SELECTION "
        "by one delta as ONE command (one undo, one redo), selectionCount() reports how many keys the "
        "selection names, and a drag on an Auto segment's ghost handle writes Type_Custom carrying the "
        "derived pair at the third points as one undo step; "
        "setSelectedKeysEasing() changes the "
        "easing type of the selected key(s) as ONE undo step on that same route, pushing nothing for "
        "an empty or stale selection, for the step-held image channel or for the type a key already "
        "has; and with a timeline attached the panel shares its selection (one instance, both "
        "directions) and its frame window (pushed as values, its own X written by nothing else, the "
        "wheel's frame half and the middle-drag pan delegated to the timeline)\n"
    );
    return 0;
}
