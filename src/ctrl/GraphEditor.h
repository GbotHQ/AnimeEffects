#ifndef CTRL_GRAPHEDITOR_H
#define CTRL_GRAPHEDITOR_H

#include <QRect>
#include <QVector>
#include <QtCore/qnamespace.h>
#include <functional>
#include "cmnd/BasicCommands.h"
#include "cmnd/Gesture.h"
#include "core/AbstractCursor.h"
#include "core/ObjectNode.h"
#include "core/Project.h"
#include "core/CameraInfo.h"
#include "core/TimeLineEvent.h"
#include "core/TimeFormat.h"
#include "ctrl/UILogger.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Scaler.h"
// The selection the panel writes is the TIMELINE's own currency (see the
// SELECTION note below): time::Selection is what carries the focus the
// timeline's renderer highlights, so the panel holds a reference to the very
// instance the timeline owns rather than a second set of its own.
#include "ctrl/time/time_Selection.h"
// The temporary playback window's own type: the graph's ruler draws and dims by the
// timeline's rules, so the GUI pushes the timeline editor's PlayWindow VALUE into this
// editor rather than a second copy of the struct (see setPlayWindow/activeRange below).
#include "ctrl/TimeLineEditor.h"
#include "util/Range.h"

namespace ctrl {

// The two document commands a key-move gesture drives (defined in
// ctrl/TimeLineUtil.h): the frame half and the value half. Declared here so this
// header does not drag the timeline utilities - and every key type they include
// - into every consumer, the same reason the Entry list below is built from core
// headers alone.
namespace TimeLineUtil {
    class MoveFrameOfKey;
    class AssignKeyValue;
}

// THE WHOLE KEY MOVE AS ONE COMMAND, so a drag that moves N keys is ONE undo
// step - the design's "Multi-key drag: a per-selection delta, one command per
// gesture".
//
// A key move is two kinds of edit: a FRAME (a map operation on the owning
// TimeLine) and a VALUE (a field write on one component of one key). The panel
// already drives the timeline's own commands for both - ctrl::TimeLineUtil's
// MoveFrameOfKey (which moves a WHOLE core::TimeLineEvent, i.e. every selected
// key, by one delta with its own conflict rule) and one AssignKeyValue per key's
// value. What this class adds is the "one command" half: it OWNS those commands,
// is the single command a cmnd::Gesture<KeyMoveCommand> pushes, and every later
// step of the drag REWRITES it (modifyMove / modifyValues) instead of pushing
// another - so the stack grows by one entry per gesture, not one per move, and
// the whole gesture is one undo step.
//
// The sub-commands are NOT stack entries (undo/redo delegate to them), which is
// exactly what makes replay work: each carries the values it restores, so
// undoing this command puts every frame and every value back, as cmnd/Stable.h
// requires of an interactive edit.
class KeyMoveCommand: public cmnd::Stable {
public:
    KeyMoveCommand();
    virtual ~KeyMoveCommand();

    // The FRAME half, over every target of aTargets (one per selected key, at the
    // frame it held when the gesture began).
    void buildFrames(const core::TimeLineEvent& aTargets);
    // The frame step. Before the command is on the stack this only validates and
    // records it; afterwards it moves the live keys and fills aModEvent with each
    // target's from/to frames (Type_MoveKey). False when the step is refused - a
    // destination another key owns - in which case NOTHING moved and nothing is
    // recorded, so a refused step can be dropped without leaving a history entry.
    bool modifyMove(core::TimeLineEvent& aModEvent, int aStep, const util::Range& aFrame, int* aClampedStep);

    // One VALUE half: aKey's (aType, aComponent) follows the gesture's value
    // delta, from the aBaseValue it held when the gesture began.
    void addValue(core::TimeKey& aKey, core::TimeKeyType aType, int aComponent, float aBaseValue);
    // Rewrites every value half to aBaseValue + aValueDelta (an absolute function
    // of the pointer, so it cannot accumulate a key's own clamp).
    void modifyValues(float aValueDelta);

    virtual void exec();
    virtual void undo();
    virtual void redo();

private:
    TimeLineUtil::MoveFrameOfKey* mFrames;
    struct Value {
        TimeLineUtil::AssignKeyValue* command;
        float base;
    };
    QVector<Value> mValues;
};

// ONE HANDLE DRAG'S COMMAND: the two easing params a handle drag writes - the
// param the GRABBED handle is stored in, and, when the key that handle belongs
// to has a neighbour on its other side, the NEIGHBOUR segment's param that
// receives the MIRRORED handle (see GraphEditor::updateDrag). One command for
// both halves is what keeps the whole drag ONE undo step; modifyValues()
// rewrites both on every later move instead of pushing a second command.
//
// The two halves are separate Param objects even when the grabbed handle is the
// `to` key's incoming one, whose value still lives in the segment's `from`
// param: the mirror always lives in the NEXT/PREVIOUS segment's `from` param,
// which is a different key's easing. aMirror is null when the key has only one
// handle (the first and last key of a channel): there is no neighbour segment
// to hold the opposite tangent.
//
// A whole Param moves through each half, so the key's `aligned` flag rides the
// same command: Alt writes it FREE inside the NEXT value (one undo restores the
// link with both handles), and Alt+CLICK writes it LINKED while relocating both
// handles along the neighbour chord.
class HandleDragCommand: public cmnd::Stable {
public:
    HandleDragCommand(
        util::Easing::Param* aMain, const util::Easing::Param& aMainValue, util::Easing::Param* aMirror,
        const util::Easing::Param* aMirrorValue
    );
    // The values the drag holds NOW: the grabbed param and, when this command
    // owns one, the neighbour's. A null aMirror leaves the mirror half alone.
    void modifyValues(const util::Easing::Param& aMain, const util::Easing::Param* aMirror);

    virtual void exec();
    virtual void undo();
    virtual void redo();

private:
    struct Half {
        util::Easing::Param* target;
        util::Easing::Param prev;
        util::Easing::Param next;
    };
    Half mMain;
    Half mMirror;
    bool mHasMirror;
    bool mDone;
};

// The animation graph editor: the value curves of the selected layers'
// channels, drawn as a fullscreen overlay over the animation.
//
// The panel covers the WHOLE viewport and has no background of its own - it is
// an overlay over the artwork, not a band beside it. That is a change with two
// consequences the rest of this header spells out:
//   * it must not own the whole viewport by rectangle (a fullscreen rect would
//     swallow every event the viewport tools need): ownership is narrowed to the
//     panel's PLOT - the data area the curves are drawn in - for a press, for a
//     drag it began, and for the middle button, and is otherwise HIT-ONLY, see
//     ownsCursor;
//   * the cursor is the only thing that marks a channel, so the panel keeps a
//     STICKY FOCUS - one shown channel the panel EMPHASISES (the drawing layer
//     paints every other curve dimmed) - which a press on a curve sets and a
//     press on empty space deliberately leaves alone. The focus does NOT move
//     the value window: fitting is deliberate (fitAll or the axes' own fit),
//     never a side effect of selecting.
//
// The layering is the timeline's, deliberately:
//   * geometry, scales, sampling and hit-testing live here and in ctrl::graph -
//     widget-free, no GL, no camera;
//   * painting is NOT here: the panel is drawn by gui::drawPanel (Dear ImGui)
//     from gui::GraphPanelModel, in the ImGui pass at the end of
//     MainDisplayWidget::paintEvent. The geometry this class picks through is
//     the model's geometry too - both read ctrl/graph/graph_Geometry.h - so the
//     drawn item and the picked item are one;
//   * the widget only feeds the cursor, so this class owns the mouse semantics
//     for the panel the same way ctrl::Driver's editors own them elsewhere.
//
// Every edit is ONE undo step, and every edit announces itself on the timeline's
// own wire: a handle drag is a cmnd::Gesture over a single
// HandleDragCommand (the dragged param AND the neighbour segment's mirrored
// one, see updateDrag), the key move is a
// cmnd::Gesture over a single KeyMoveCommand (the whole selection, one step of
// the drag rewritten - see that class above), and the three key edits - insert,
// move and remove (see insertKeyAt, the MOVE note next to it and
// removeSelectedKey) - go through a ctrl::TimeLineUtil::Notifier inside a
// cmnd::ScopedMacro, carrying the same core::TimeLineEvent the timeline emits.
// So the two views share one selection currency and one change currency, and
// undo/redo is one command per gesture for every edit this panel can make.
class GraphEditor {
public:
    GraphEditor(core::Project& aProject, UILogger& aLogger);
    ~GraphEditor();

    void setProject(core::Project* aProject);

    // The layers whose curves are drawn: the object tree selection.
    void setTargets(const QVector<core::ObjectNode*>& aTargets);
    const QVector<core::ObjectNode*>& targets() const { return mTargets; }

    // THE PANEL GOING AWAY (its dock page is no longer current): drop whatever
    // gesture is in flight - a key move, a handle drag, a marquee, a pan.
    //
    // This is the ONE side effect hiding ever had, and it is still wanted: the
    // panel can be switched away from with a gesture open (the release usually beats
    // the tab click, but not always), and a gesture left open would keep coalescing
    // the next edit into a command whose geometry nobody can see any more. The
    // selection, the scale and the hidden channels are kept, so the page coming back
    // restores the panel as it was - and the editor has NO other shown/hidden state:
    // the dock's page IS the panel while its tab is current, so no flag here could
    // describe it, and nothing in this class may gate itself on one (the viewport's
    // routing no longer asks; gui::GraphPanelWidget owns its page's events).
    void panelHidden();

    // The panel rect for a viewport of this size, in viewport pixels: the WHOLE
    // surface the panel is handed - the dock page's viewport, which is what the
    // panel's host measures and passes in. There is no band, no inset and no height
    // to clamp, because the panel's own surface is opaque in the dock: it is not an
    // overlay over the animation any more. Still a pure function of the size: no
    // camera, no layout pass.
    static QRect panelRect(const QSize& aViewportSize);

    // Cursor handling, the IEditor::updateCursor shape: returns true when the
    // graph needs a repaint.
    //
    // ownsCursor is the arbitration. The panel covers the whole viewport, so
    // "inside the panel" cannot decide it - that would mean "inside the
    // viewport" and the graph would swallow every event the tools need. Instead:
    //   * an in-flight gesture (a handle drag, a key move, a marquee, a
    //     middle-button pan) owns the cursor wherever it goes, so the release
    //     that ends it reaches this class;
    //   * a key or a tangent handle owns the point it is under, wherever it is;
    //   * a LEFT PRESS inside the panel's PLOT rect is the panel's, because that
    //     is where its own plot gestures begin: on a key it selects (and arms the
    //     move), on a curve's band WITH CTRL it inserts (see insertKeyAt), and
    //     anywhere else - including a plain press on a band - it opens the
    //     MARQUEE (see the SELECTION note);
    //   * a MIDDLE-button event inside the viewport is the panel's too - the
    //     middle drag pans the graph while the panel is up, never the canvas
    //     camera (see updateCursor);
    //   * a LEFT PRESS on one of the panel's two SCROLL BARS - its track or its
    //     thumb - is the panel's as well, and it is tested BEFORE every other hit:
    //     a bar is chrome the user drags, and the press must not become a plot
    //     gesture (a handle can hang over the bar's rows, see THE SCROLL BARS).
    // A plain hover, a press outside the plot and everything that is not a bar and
    // has nothing to act on pass through: the tools keep their feedback over the
    // whole viewport, their presses in the label column and the margins the panel
    // does not use for its bars, and the camera gestures the panel does not own.
    //
    // WHAT THE PLOT COSTS THE TOOLS. The plot is most of the canvas, so claiming
    // every press in it does take those presses from the viewport tools while the
    // panel is up - there is no modifier left to let a rubber band start
    // anywhere in the data area otherwise (a band that had to be started with Ctrl
    // was a band nobody draws by accident, but also one nobody finds). Showing the
    // panel is the user saying they are working on curves; hiding it (or putting
    // the pointer outside the plot) is how the tools get the canvas back. The
    // events this does NOT take are the ones above: hovers, the chrome outside the
    // plot, and a right-press, which is nobody's here.
    //
    // The Driver asks ownsCursor first and routes the event to exactly one owner
    // - the viewport has no accept()/reject() seam, and adding one would change
    // every mode's contract.
    bool ownsCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor) const;
    bool updateCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor);

    // Wheel over the panel: ZOOM, one or both axes. Returns true when the graph
    // took the event, so the viewport does not also zoom the canvas.
    //
    // THE MAPPING, axis by axis:
    //   * plain         - zoom BOTH axes about the cursor by the same notch factor,
    //                     so the point under the pointer stays under it;
    //   * Shift         - zoom the FRAME axis alone (horizontal), about the
    //                     cursor's column;
    //   * Ctrl          - zoom the VALUE axis alone (vertical), about the cursor's
    //                     row;
    //   * Ctrl + Shift  - zoom BOTH axes, exactly as plain does: the two
    //                     modifiers name the two axes, so both of them name both.
    // Every combination therefore zooms at least one axis, and no notch is ever
    // consumed by a case that moved nothing. THIS GESTURE NEVER SCROLLS: moving a
    // window without changing its scale is the SCROLL BARS' job (and the middle
    // drag's), so a notch can be neither a scroll nor a dead key. The wheel's Shift
    // branch is the frame-axis ZOOM it had before the 2026-10-19 round briefly made
    // it a scroll, and the timeline's own wheel (Shift = scroll, plain = zoom) is
    // untouched by this: it is that widget's gesture, not this editor's.
    //
    // The zooms use the canvas mover's and the timeline scaler's own per-notch
    // factor (1.1 per notch) and their default direction (a positive notch zooms
    // OUT), so one wheel motion does the same thing over the panel and over the
    // viewport.
    //
    // aInvertScaling is the user's zoom direction, and it inverts EVERY branch here
    // - before the frame half is delegated - so one notch cannot move the two axes
    // against each other. It is gui::MouseSetting::invertMainViewScaling: the panel
    // takes its wheel from the main view (hiding it gives the wheel back to the
    // canvas), so it takes that view's setting.
    //
    // The FRAME half of a zoom is the TIMELINE's while a timeline owns the frame
    // axis (see setFrameWindow): the notch goes to the timeline through its
    // delegate, about the frame under the cursor, and the panel's own X moves only
    // because the timeline pushed its new window back. The VALUE half is always
    // this panel's.
    bool updateWheel(
        const core::CameraInfo& aCamera, const QPoint& aPos, int aWheelDelta, Qt::KeyboardModifiers aModifiers,
        bool aInvertScaling
    );

    // --- THE SCROLL BARS ---
    //
    // The panel's two scroll bars are how a window MOVES without changing its scale:
    // the horizontal one along the plot's bottom edge is the frame axis' window (the
    // timeline's, while a timeline owns the axis - see setFrameWindow), the vertical
    // one along its right edge is this panel's value window. They are drawn by
    // gui::drawPanel from the rects the MODEL publishes
    // (GraphPanelModel::hTrack/hThumb/vTrack/vThumb), and their geometry is
    // ctrl::graph::Scaler::scrollBars' - the one formula the editor's own hit-test
    // below reads too, so the bar a press grabs is the bar the panel paints.
    //
    // WHERE THEY ARE. The bars live in the panel's margin OUTSIDE the plot, and the
    // scaler reserves their 14px (2px of gap + a 12px lane) at the plot's right and
    // bottom edges - see THE SCROLL BARS' RESERVE in graph_Scaler.h for the numbers
    // and for what that costs the plot. An empty plot publishes no bars at all, and a
    // bar with nothing to scroll is not published either.
    //
    // WHAT THEY LOOK LIKE. They are the application's own scroll bars: an invisible
    // 12px trough and a 4px, radius-2 pill across the slider's run
    // (src/gui/AppStyle.cpp's CC_ScrollBar). hThumb/vThumb name that RUN, so the pill
    // the drawing layer paints and the span a press drags are one geometry.
    //
    // WHAT THE THUMB MEANS. Its length is the visible window's fraction of the
    // CONTENT (the project's frame range on the frame axis, the value extent of the
    // shown channels' keys - shownValueExtent below - on the value axis), so a window
    // that covers the content fills its track and a zoomed-in one draws a short
    // thumb. It is a VIEW of the window, never a command: nothing here writes the
    // document, and no gesture below pushes anything.
    //
    // THE PRESSES (see updateCursor, and ownsCursor, which claims them before every
    // plot hit):
    //   * a press ON A THUMB (its run) starts a pan of that axis by the pointer's
    //     pixel delta - the frame half through the delegate while the timeline owns
    //     the axis, so both views keep one window; the value half moves this panel's
    //     own window;
    //   * a press on the LANE beside the run moves the window ONE PAGE toward the
    //     press (a page is the visible window, the unit a QScrollBar's PageStep uses)
    //     and then continues as that same drag;
    //   * SHIFT held while dragging changes nothing here - the modifier means
    //     "one axis, the dominant one" to a KEY drag, and a scroll bar is already one
    //     axis, so it is deliberately not consulted;
    //   * none of it pushes a command: only the middle-drag pan's and the wheel's
    //     window moves are already outside the history, and a scroll bar is one of
    //     those.
    // The press does not require the pointer to stay on the bar: the drag follows the
    // pointer anywhere (ownsCursor keeps the cursor while mDragging is set), exactly
    // as a marquee or a pan does.
    //
    // The VALUE EXTENT the vertical thumb is measured against: the union of every
    // shown channel's key values and of kFitSamplesPerSegment samples per segment
    // (the overshoot a Custom bezier or Back/Elastic reaches) - the SAME range
    // fitValueAxis fits, computed by the same walk, so the bar and the fit cannot
    // disagree. False when no shown channel has a key, i.e. when there is no extent
    // at all and the thumb must fill its track.
    bool shownValueExtent(float& aLo, float& aHi) const;

    // The modifier state the panel's own gestures read, fed by the widget before
    // it routes a cursor or wheel event (core::AbstractCursor carries no
    // modifiers). It is plain state, not a gesture: a press reads it and a drag
    // re-reads it on every move, so releasing Shift mid-drag un-constrains the
    // axis at once, like the viewport's own modifier-driven motions.
    //
    //   * Shift - a key drag moves on ONE axis: the dominant one of the gesture
    //     so far (see the MOVE note); a marquee ADDS its keys to the selection
    //     instead of replacing it (see the SELECTION note);
    //   * Ctrl  - a press ON A CURVE'S BAND inserts a key there (see insertKeyAt).
    //     It is the ONLY thing Ctrl does with a press now: every other press in
    //     the plot - on a key it selects and arms the move, everywhere else it
    //     opens the marquee - has the same meaning with or without it.
    void setModifiers(Qt::KeyboardModifiers aModifiers);
    Qt::KeyboardModifiers modifiers() const { return mModifiers; }

    // The curves currently drawn: one per visible channel of every target.
    // Rebuilt from the document on demand, so an edit anywhere is reflected.
    QVector<graph::Curve> curves() const;

    // Channel visibility, keyed by (target, channel). A channel is visible
    // unless it has been hidden.
    void setChannelVisible(core::ObjectNode& aTarget, const graph::Channel& aChannel, bool aVisible);
    bool isChannelVisible(const core::ObjectNode& aTarget, const graph::Channel& aChannel) const;

    // --- the selection, in the timeline's own currency ---
    //
    // SELECTION. The panel selects MARKERS - one COMPONENT of one key, the dot on
    // one curve - and it can select more than one: the selection is the timeline's
    // own time::Selection, whose entries name either a whole KEY (what every
    // timeline gesture writes: such an entry covers every component the key owns)
    // or one component of it (what a press here writes). A press chooses between
    // the three plain behaviours by the modifier the widget fed in:
    //   * a plain press on a key REPLACES the selection with the one marker it
    //     hit, so the OTHER component of that key is not selected (its curve stays
    //     at full strength and the drawing layer gives it the faint ring - see
    //     isMarkerSelected and hasSelectedSiblingMarker) - UNLESS that marker is
    //     already covered by the selection, in which case the selection is left
    //     exactly as it is: pressing something already selected GRABS it, it does
    //     not collapse the selection to it. That is what makes a multi-marker
    //     selection draggable by any of its own markers (the drag is armed over
    //     the whole selection, see the MOVE note), and a whole-key entry counts as
    //     covering every component of its key;
    //   * Shift-press on a key ADDS that one marker if it is not already covered
    //     (an entry that is there is left alone - Shift extends, it does not
    //     toggle), which is how a key's two markers are selected together;
    //   * Ctrl-press on a key TOGGLES it: in when out, out when in, where "in"
    //     means the marker is covered by an entry - its own, or a whole-key entry.
    // A MARQUEE - a PLAIN press in the plot on a point that is neither a key nor
    // a handle (Ctrl on a curve's band inserts instead, see insertKeyAt) -
    // dragged before it is released, replaces the selection with EVERY MARKER of
    // every key whose drawn square falls inside the dragged rectangle, or, with
    // Shift held at the press, adds those keys to the selection. It is drawn as a
    // live rectangle while the drag is in flight (marqueeRect), and a release
    // within 4px of the press on BOTH axes is not a drag at all: that press is the
    // plain click, which clears the selection (see endMarquee). Ctrl+A
    // (selectAllMarkers) is the keyboard form of the same bulk rule over the shown
    // channels, and Escape clears (clearSelection).
    //
    // ONE SET, ONE HIGHLIGHT. The set an edit here writes is the live selection
    // of the timeline the panel is a view of (see setSelectionSource): writing it
    // is what makes the timeline's renderer highlight those keys (time::Selection
    // rebuilds every member key's focus on every change - a marker entry focuses
    // its KEY - and that focus IS the highlight), and a selection made in the
    // timeline is what this panel reads and acts on. With no timeline attached -
    // the unit tests, and the frame before the window wires one in - the panel
    // uses a private instance of the same type, so there is exactly one set either
    // way and the code below never branches on which it is.
    //
    // The selection is what removeSelectedKey and setSelectedKeysEasing act on,
    // and it is what the drawing layer highlights: the MODEL classifies each
    // marker it publishes against this ONE set (gui::PanelCurve::keySelected and
    // keySiblingSelected, filled from isMarkerSelected / hasSelectedSiblingMarker
    // below), so the ring a marker is drawn with is this set's answer and not a
    // second copy of it. It is NOT touched by anything else: a plain hover and
    // every press outside the plot leave it alone.
    bool retrieveSelection(core::TimeLineEvent& aOut) const;
    // --- the panel's two keyboard actions ---
    //
    // WHERE THE KEYS COME FROM. This class is fed cursor and wheel events only, and
    // its KEYS are routed by whatever owns the application's key map - the same
    // split removeSelectedKey documents ("wiring a key press to it belongs to
    // whatever owns the key map, not to this class"). In the running application
    // that is gui::MainWindow's key map: gui::GraphPanelWidget's key handler hands
    // an event it does not consume on to the main window, which is where the
    // application's keys have always been routed. So these two are the ACTIONS, and
    // the surface that receives the key calls them.
    //
    // ESCAPE BELONGS TO WHOEVER CONSUMED IT. The key is only this panel's when
    // nothing else took it: a picker or a menu that is up owns its own dismissal
    // (the timeline's easing picker is a Qt::Popup and holds the keyboard while it
    // is open - a Qt::ApplicationShortcut does not even fire then), and so does an
    // ImGui text field (gui::ImGuiInputBridge::keyboardCaptured gates the
    // application's own commands on exactly that). The rule cannot be decided HERE -
    // it is decided by the consumer that receives the QKeyEvent, after every other
    // consumer has had it - so the action ANSWERS what it did and the caller calls
    // it only once the event reached it unconsumed.
    //
    // ESCAPE: empties the selection. True when there was something to clear (and
    // false when the set was already empty, so a caller can tell "I acted" from "I
    // did not").
    bool clearSelection();
    // SELECT ALL (ctrl+A): every marker of every key of the SHOWN channels - every
    // visible channel of every target (buildEntries), each of its curves' keys -
    // replaces the selection, which is exactly the set a marquee over the whole plot
    // would leave. Written as one whole-key entry per key: such an entry IS every
    // marker its key owns, so a channel that appears twice (two visible components
    // of one type) cannot double it, and the timeline's own reading of the set is
    // the same keys it would have had. True when the selection changed; with no
    // shown channel there is nothing to select, so the set is emptied (and only then
    // does this answer true).
    // THE TIMELINE HAS ITS OWN Ctrl+A (ctrl::TimeLineEditor::selectAllKeys), and the
    // two do NOT share an implementation: both write this same shared set, but this
    // method enumerates the PANEL's universe (every marker of every visible channel
    // of the targets) while the timeline's enumerates the TIMELINE's (every key of
    // every row it shows, closed folders' descendants included). They share the
    // selection, not the walk.
    bool selectAllMarkers();
    // Replaces the whole selection with this one key: the KEY, not a marker of it,
    // so every component it owns is selected (the timeline's own unit - a caller
    // that names a whole key).
    bool selectKey(core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame);
    // Replaces the whole selection with this one MARKER: the component of the key
    // aChannel names, so the key's other components are left out (the press's own
    // gesture).
    bool selectMarker(core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame);
    // Adds (aAdd == true) or removes (aAdd == false) this one marker. Adding a
    // marker the selection already covers, or removing one it does not, changes
    // nothing. Removing takes out what covers the marker - its own entry, or a
    // whole-key entry, which names it among every other component of its key.
    bool modifyMarkerSelection(
        core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame, bool aAdd
    );
    // Adds (aAdd == true) or removes (aAdd == false) this whole KEY. Adding a key
    // already in the selection, or removing one that is not, changes nothing.
    bool modifySelection(core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame, bool aAdd);
    // Attaches the selection this editor writes to aSelection - the timeline's
    // own live set (ctrl::TimeLineEditor::selection()) - or, with nullptr, back
    // to the private instance this editor starts on. The caller owns the
    // lifetime: whoever attaches a selection must detach (or die) before it goes
    // away. Nothing else about the selection changes: the entries already in it
    // are the entries the panel then reads.
    void setSelectionSource(time::Selection* aSelection);
    // The live selection this editor writes. Never null.
    time::Selection& selection() { return *mSelection; }
    const time::Selection& selection() const { return *mSelection; }

    // --- the selection's COUNT and MARKER STATES ---
    //
    // The number of KEYS the selection names that the document still holds. A stale
    // entry is not a key the user sees selected, so it is not counted, and two
    // markers of one key are ONE key. It is a FACT about the live selection and not
    // a drawing: the panel publishes it to the accessibility tree (", %n key(s)
    // selected" on its summary), and nothing inks it.
    int selectionCount() const;

    // --- the marker states the drawing layer reads ---
    //
    // Both are pure reads of the live selection: const, no allocation, and linear
    // in the (small) selection. Neither dereferences a stored selection pointer -
    // the node is compared by address, the frame and type by value, exactly the way
    // every other reader here resolves a key.
    //
    // A key's markers are its graphable components; each is drawn as the dot on
    // that component's curve. "Selected" has two sources, and they must agree with
    // the set's own rule (time::Selection): an entry that names that exact
    // component, or a whole-key entry - the timeline selects keys, and a selected
    // key lights EVERY marker it owns (see the design's "a key owns every
    // component's marker").
    bool isMarkerSelected(
        const core::ObjectNode& aNode, core::TimeKeyType aType, core::Frame aFrame, int aComponent
    ) const;
    // The channel form of the same question: aChannel's own component of that key.
    bool isMarkerSelected(const core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame) const;
    // True when the selection names A DIFFERENT MARKER of the same key than the one
    // being asked about: another component of it, or the key as a whole (a key-level
    // entry is all of them). It answers "is this key's other channel in play?" -
    // precisely what the design's faint ring marks, so a caller draws that ring for
    // `!isMarkerSelected(...) && hasSelectedSiblingMarker(...)` and for nothing
    // else: with the asked marker unselected and a sibling selected, the key is
    // half in play (that marker's curve stays at full strength but never takes the
    // selection ring), while both selected means no faint ring remains. False when
    // no other component of that key is selected - which is every key of a
    // single-component type, so a Rotate key can never have a sibling.
    bool hasSelectedSiblingMarker(
        const core::ObjectNode& aNode, core::TimeKeyType aType, core::Frame aFrame, int aComponent
    ) const;
    // The channel form of the same question.
    bool hasSelectedSiblingMarker(
        const core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame
    ) const;

    // True while a marquee drag is in flight.
    bool hasMarquee() const { return mMarqueeArmed; }
    // The marquee's rectangle in viewport pixels, normalized (a positive width
    // and height) and clipped to the plot, or an EMPTY rect while no marquee drag
    // is in flight. A press and release with no movement gives an empty rect:
    // nothing falls inside it, so the selection becomes empty, which is the
    // "click empty space to deselect" gesture.
    QRect marqueeRect() const;
    // True while a middle-button pan is in flight.
    bool isPanning() const { return mPanning; }

    // --- is a key move in flight? ---
    //
    // "IS A MOVE IN FLIGHT" IS ANSWERED BY THE GESTURE'S COMMAND: true exactly while
    // the stack still holds the command this drag pushed as its MODIFIABLE one. An
    // armed press that has not crossed a pixel yet (no command yet) and a drag
    // whose command an undo dropped (the keys are back at the press) therefore
    // both read NOT MOVING - the honest answer, because in neither state does a
    // step describe the document. The move is one cmnd::Gesture<KeyMoveCommand>,
    // so the whole question is one pointer comparison. Pure, no allocation, no
    // document walk.
    bool isMovingKeys() const;

    // --- the three key edits: add, move, remove ---
    //
    // The panel's document-mutating gestures, next to the handle drag below.
    // Each is ONE undo step, and each goes through the same
    // ctrl::TimeLineUtil::Notifier + cmnd::ScopedMacro route the drag (and the
    // timeline) uses: one core::TimeLineEvent in the timeline's own currency
    // reaches the rest of the application, so the timeline and the object tree
    // see an edit made here exactly as they see one made there.
    //
    // ADD. A CTRL-press on a curve's BAND - a point on no key and no handle,
    // whose frame the curve covers and whose value is within kCurveBandRadius
    // pixels of the value the curve evaluates there - inserts the key the curve
    // already passes through: the value written is the curve's own sample at that
    // frame, so inserting it changes no sampled value AT THAT FRAME. The new
    // key's payload - the components the graph does not draw, and the key's
    // easing - is taken from the key playback is on at that frame (the segment's
    // own `from` key, which owns that segment's easing), so the insert invents
    // neither a value nor a payload.
    //
    // WHY THE MODIFIER. The band lies on the curve, i.e. in the middle of the
    // plot, and the plot is where a MARQUEE begins (see the SELECTION note). A
    // plain press on the band would insert a key whenever a user started a
    // rubber band that happened to cross a curve - an edit nobody asked for, from
    // a gesture that means "select". Ctrl-press is the graph-editor convention
    // for "add a key here" (Blender's f-curve editor does exactly this), and it is
    // unambiguous here because Ctrl on a KEY also means "toggle this key in the
    // selection": the two never overlap, since a band point has no key on it. So
    // one press has one meaning decided by what is under the pointer: a key
    // selects (Ctrl toggles it), a band point with Ctrl inserts, and every other
    // point in the plot - a band point WITHOUT Ctrl included - opens the marquee.
    //
    // The modifier is therefore the INSERT's alone now, and the marquee's cost is
    // the plain press it takes: see ownsCursor for what that costs the viewport
    // tools inside the plot.
    //
    // The inserted key becomes the panel's whole selection and its channel the
    // focused channel, exactly as a pressed key would (so a following
    // removeSelectedKey() removes it). The focus no longer moves the value
    // window (see the class comment), so an insert press moves nothing under its
    // own release.
    //
    // One caveat the key's own setters impose: the value is written through the
    // concrete key's setter, which clamps to that type's own bound - a
    // Move/Scale/Rotate/Depth value is bounded far outside any real animation (±
    // 10000), opacity is 0..1 and an HSV component is an int, so a sampled value
    // beyond those bounds is seeded as the bound. That is the one case where an
    // insert can move a sampled value, and the graph does not invent a bound of
    // its own to avoid it (the bounds belong to the key types).
    //
    // The insert is REFUSED, with nothing pushed and the document untouched,
    // when the hit-test finds a key or a handle at the point (the more specific
    // target wins - this is what makes an occupied frame safe, since
    // TimeLine::createPusher REPLACES a key at its own frame and replacing is a
    // different edit with a different undo story), when that frame already holds
    // a key of the channel's type (the same rule, as the backstop for a pointer
    // on another channel's band), and when the point is outside the plot or off
    // every shown curve.
    bool insertKeyAt(const core::CameraInfo& aCamera, const QPoint& aPos);

    // MOVE. Pressing a KEY (not a handle) arms a move over the panel's SELECTION:
    // while the button is down the selection follows the pointer - and WHO follows
    // it in which direction is the design's marker rule, not a whole-key copy:
    //   * HORIZONTAL moves the FRAME, which one key shares across its components,
    //     so each selected KEY moves once however many of its markers are
    //     selected (two markers of one key are one frame, and moving it twice
    //     would be a double shift). The frame delta is measured from where the
    //     button went down, so grabbing a key off its centre does not make it jump
    //     by the offset;
    //   * VERTICAL moves only the SELECTED MARKERS, each in its OWN channel, by
    //     the same delta - a marker of another TYPE moves too (its own component,
    //     its own curve), because what is selected is markers and not one channel.
    //     So selecting a Scale key's X alone and dragging up changes X and leaves
    //     Y where it is, and selecting both moves both by the same delta.
    // The whole gesture is ONE undo step AND one command (KeyMoveCommand). It is
    // driven by updateCursor, like the handle drag, and the rules are:
    //   * the members are the markers the selection named when the press armed the
    //     move (the press wrote that selection: a plain press replaces it with the
    //     pressed marker, or leaves it as it is when the press landed on a marker
    //     it already covers, Shift adds one, Ctrl toggles it), resolved against the
    //     document so a stale entry is dropped. A whole-key entry (the timeline's
    //     own selection) names every component its key owns, so it becomes one
    //     member per graphable component of its type; a marker that is NOT
    //     selected is not a member, and comes out of the gesture untouched;
    //   * the first ACCEPTED move builds ONE commit: a TimeLineUtil::MoveFrameOfKey
    //     over every member's FRAME (it already moves a whole core::TimeLineEvent,
    //     so the selection moves as one unit by one delta with its own conflict
    //     rule - the frame half therefore carries each KEY once) and one
    //     cmnd::AssignKeyValue<T> per VALUE the drag writes, wrapped in ONE
    //     KeyMoveCommand pushed by ONE cmnd::Gesture inside ONE
    //     cmnd::ScopedMacro carrying ONE TimeLineUtil::Notifier. Every later move
    //     rewrites that command (modifyMove/modifyValues), so a drag cannot push
    //     one command per move;
    //   * WHICH VALUES MOVE. Every member is a (key, component) pair, so every
    //     member's value half is written on its own component - the pressed one and
    //     the ones Shift added alike - and a member's value is always the base it
    //     held at the press plus the gesture's value delta. A key whose type is not
    //     graphable has no component, so it is a frame-half member only;
    //   * the value command writes that ONE component through graph::Channel - a
    //     copy with one field replaced - so every component the graph does not
    //     draw (a Move key's centroid, an HSV key's absolute flag, an Image key's
    //     resource and blend mode) is carried through unchanged, and the key's own
    //     setter still clamps the value to its type's bound;
    //   * SHIFT constrains the gesture to the DOMINANT axis: the pointer's own
    //     pixel displacement decides (|dx| >= |dy| keeps the frame, otherwise the
    //     value), so a drag that is overwhelmingly horizontal does not smear a
    //     stray pixel of vertical movement into the value. Shift is re-read on
    //     every move, so it can be pressed and released mid-drag;
    //   * a destination frame that already holds a key of that type is REFUSED
    //     (MoveFrameOfKey's own conflict rule, applied to the whole selection: a
    //     move that would land any member on another key's frame refuses the WHOLE
    //     step), and a refusal refuses the whole step - frame AND value stay where
    //     the gesture last had them, nothing is announced, and the gesture stays
    //     open so the pointer can be dragged on to a free frame. Nothing is pushed
    //     before a step is accepted, so a refused drag leaves no empty history
    //     entry behind (the timeline's own move pushes its command on the first
    //     drag and can then be refused, which does leave one; the graph does not
    //     repeat that);
    //   * a step that moves neither frame nor value pushes nothing and opens
    //     nothing, so a press that never moves leaves the history untouched;
    //   * an undo mid-drag drops the command's modifiable link, so the next move
    //     opens a fresh commit from the frames and values the keys are at then;
    //   * the PRESSED key is held in the timeline's currency (the layer that owns
    //     the map, the node the event names, the type and the frame it is at
    //     now), not as an index into a rebuilt entry list, so the move follows
    //     the key the press hit; if that key is gone (an undo or another view
    //     removed it) the gesture stops and says so, like the handle drag's
    //     anchor check.
    //
    // WHETHER A MOVE IS IN FLIGHT is read off the gesture itself while the drag is
    // open - see isMovingKeys above - so a caller can tell a held key move from a
    // finished one, and a refused step leaves that answer untouched (the command is
    // still the stack's modifiable one; only the KEYS did not move).
    //
    // REMOVE. Removes every key the panel's CURRENT selection names through
    // TimeLine::createRemover, inside ONE notifier + macro route, so a
    // multi-key selection is ONE undo step. An undo restores the SAME key
    // objects with their values and easings, because the removal is a map
    // removal and not a rewrite. Keys the selection names that are no longer in
    // the document (a stale selection) are skipped; when nothing survives, the
    // selection is dropped and nothing is pushed, so no empty entry is left
    // behind. The selection is dropped with the keys.
    //
    // The application's Delete key is routed by its key map to the timeline
    // ("Delete selected key(s)"), and this panel is fed only cursor events - so
    // THIS call is the panel's remove action, and wiring a key press to it
    // belongs to whatever owns the key map, not to this class.
    bool removeSelectedKey();

    // EASING. Changes the easing TYPE of every key the panel's SELECTION names,
    // as ONE undo step, and returns whether it pushed anything.
    //
    // The route is the timeline's own, so the two views cannot disagree about
    // undo or notifications: each key is assigned through the same
    // cmnd::Assign<util::Easing::Param> command ctrl::TimeLineUtil's
    // assignKeyEasing pushes, under ONE cmnd::ScopedMacro carrying ONE
    // TimeLineUtil::Notifier whose core::TimeLineEvent states Type_ChangeKeyValue
    // and names every target that was written. That is the timeline's
    // "Change key(s) easing to" action, batched: the timeline opens one macro per
    // key (its own assignKeyEasing does, and cmnd::Stack::beginMacro asserts on a
    // nested one), so it takes one undo step per key; here the whole selection is
    // one macro and therefore one entry in the history.
    //
    // WHICH KEYS IT WRITES. The selection is the timeline's own currency
    // (retrieveSelection; selectKey sets it, and a right-click on a key in the
    // panel sets it the same way the timeline's key menu does). Every entry is
    // written at the component it names - a marker edits ITS OWN curve, so
    // choosing an easing for Move X leaves Move Y alone - while a key-level entry
    // (the timeline's unit, and the panel's marquee/Ctrl+A) covers every
    // component its key owns. A target whose frame holds no key any more is
    // skipped - a stale selection names a key that is not in the document - and
    // so is a key whose type carries no easing (graph::Curve::easingOf answers
    // null) and a component whose easing type is ALREADY aType, so a menu entry
    // that changes nothing leaves no history entry behind. Nothing else is
    // dropped silently: the ones that are written all appear in the notifier's
    // event, once per key.
    //
    // EMPTY SELECTION (or a selection of which nothing survives the rules above):
    // returns false and pushes NOTHING. That is not a micro-optimisation -
    // cmnd::Stack appends a macro even when it holds no command and counts it as
    // an edit, so an action that opened the macro first would leave an empty undo
    // step, exactly the defect removeSelectedKey's own guard avoids.
    //
    // SEVERAL KEYS: every target that survives is written, and the whole batch
    // stays ONE undo step, because it is one macro. The panel makes such a
    // selection itself (a marquee, or Shift-presses, see the SELECTION note), and
    // so does the timeline; because the two share ONE set, this action writes
    // exactly the keys the user sees highlighted in either view.
    //
    // The Image channel is deliberately NOT written: its keys carry an easing
    // param, but playback step-holds image offsets (graph::Channel::isInterpolated
    // is false for it), so the param is never consulted and the graph refuses the
    // edit - which is also what the timeline's own easing menu does with Image.
    // The panel's easing action therefore reports false for a selection on an
    // image channel rather than recording a change nothing can see.
    bool setSelectedKeysEasing(util::Easing::Type aType);

    // The half-width, in pixels, of the band around a curve the insert gesture
    // edits (see insertKeyAt's ADD note): a point whose frame the curve covers
    // and whose value is within this distance of the value the curve evaluates
    // at that frame is a point on the curve. Deliberately smaller than
    // graph::kKeyHitRadius (10) and graph::kHandleHitRadius (12), so a press
    // near an existing key or handle always hits that item first and only a
    // genuinely empty stretch of a curve inserts.
    static constexpr int kCurveBandRadius = 6;

    // There is NO shared-window edge margin: the shared mapping is the timeline's
    // pixel-for-pixel mapping across the panel (graph::Scaler's THE SHARED WINDOW'S
    // MAPPING), so a frame's x here is the frame's x there. The graph's own fit
    // keeps its content off the plot's edges with fitFrameAxis's pad instead.

    // --- sticky focus ---
    // The panel marks ONE shown channel as focused: the channel a press last hit
    // on a curve. The focus is the panel's EMPHASIS - the drawing layer paints
    // the focused curve and its keys at full colour and every other curve dimmed
    // - and it is reported to the accessibility layer as what the panel is
    // working on. It is NOT a fit trigger: see "deliberate fit" below.
    //
    // The policy is STICKY by design:
    //   * a press that HITS a curve (a key or a tangent handle) makes that
    //     entry's (node, channel) the focus;
    //   * a press that hits nothing leaves the focus - and the selection -
    //     untouched, because empty space is not a command to drop what the user
    //     was working on;
    //   * the focus survives every cursor pass and every paint: it is not a
    //     hover, it is a choice, so moving the pointer off the curve does not
    //     change it;
    //   * setTargets and setProject drop a focus that is no longer among the
    //     shown entries (hiding the focused channel drops it on the next sync,
    //     which a cursor pass runs), since a channel the panel cannot draw
    //     cannot be what the panel emphasises.
    //
    // DELIBERATE FIT. The value window changes only when the SHOWN SET changes
    // (a new layer selection, a channel that appears or disappears) or when a
    // caller asks for a fit (fitValueAxis, fitAll, the double-click). It never
    // changes because the focus moved, because a key was selected or dragged, or
    // because the wheel or a pan was used - a user who has framed a curve keeps
    // that frame while working on it. The fit the focus used to imply was
    // removed for exactly that reason: clicking a curve zoomed the view, which
    // is not what a click means.
    bool hasFocus() const { return mFocusNode != nullptr; }
    // The focused layer, or nullptr when nothing is focused.
    const core::ObjectNode* focusedNode() const { return mFocusNode; }
    // The focused channel; meaningful only while hasFocus() is true.
    graph::Channel focusedChannel() const { return mFocusChannel; }
    // Makes (aNode, aChannel) the focused channel. The pair is expected to be
    // one of the shown entries. Re-focusing the channel that is already focused
    // is a no-op. It moves NO window: the value axis is the user's, and the
    // focus is emphasis only (see DELIBERATE FIT above).
    void setFocusedChannel(core::ObjectNode& aNode, const graph::Channel& aChannel);
    // Drops the focus. Like setFocusedChannel it moves no window.
    void clearFocus();

    // The document's current frame, for views that draw the playhead. -1 when no
    // project is set (core frame numbers start at 1, so -1 cannot be a real
    // frame).
    core::Frame currentFrame() const;

    // THE FRAME-NUMBER FORMAT: the setting the timeline's editor reads
    // ("generalsettings/ui/timeformat", ctrl/TimeLineEditor.h:203) so the graph's
    // ruler writes a frame exactly as the timeline's ruler does. Read once when the
    // editor is built (the timeline's own rule) and applied live by the setter, so a
    // settings change can be pushed without an app restart.
    core::TimeFormatType timeFormatType() const { return mTimeFormat; }
    void setTimeFormatType(core::TimeFormatType aType) { mTimeFormat = aType; }

    // The project's playback rate, for the formats that need it (seconds and
    // timecode). The timeline's own default before a project exists is 60
    // (ctrl::TimeLineEditor's mFps), so the two rulers format alike in every state.
    int fps() const;

    // The scale, so the view can zoom and pan (wheel) and so a caller can query
    // the window it is showing.
    graph::Scaler& scale() { return mScale; }
    const graph::Scaler& scale() const { return mScale; }

    // Fits the value axis to the values the SHOWN curves occupy now: the min/max
    // of every shown curve's key values AND of a walk through its segments, since
    // a Custom easing (Back/Elastic) reaches values beyond the two keys' values.
    // It is the UNION of every shown curve; the focus does not narrow it (see
    // DELIBERATE FIT).
    // A curve with no keys contributes nothing, and when NO shown curve has keys
    // the window is left untouched, so an empty panel cannot blank a window the
    // user zoomed.
    //
    // This is the fit itself; the non-const paths run it through syncValueAxis(),
    // which skips it while the shown curves are the ones the last fit was
    // computed from - that is what keeps a wheel zoom. A caller wanting the axis
    // back on the data unconditionally calls this.
    void fitValueAxis();

    // Fits the FRAME axis to the shown curves' key frames, with a small pad at
    // both ends. The graph's frame axis is its own ctrl::time::Scaler instance
    // (see graph::Scaler), so this moves the graph's frame window only; the
    // timeline's zoom is untouched. Uses the panel rect the cursor path last
    // wrote into scale(), like the value fit.
    //
    // While a timeline owns the frame axis (see setFrameWindow) this does
    // NOTHING: the panel's X window is the timeline's, and a fit that moved it
    // would be the panel deciding, on its own, which frames are on screen - the
    // one thing the shared window exists to prevent. The value half of a fit
    // still happens, which is what fitAll below does there.
    void fitFrameAxis();
    // Both axes at once: the panel's double-click fit (and the accessibility
    // layer's own "fit" action). Fits the value axis to the union of the shown
    // curves and the frame axis to every shown key's frame, so the whole of the
    // shown data is on screen. This is the one explicit fit that is wired to a
    // pointer gesture, and it is the gesture a user reaches for when the view has
    // drifted away from the data. While a timeline owns the frame axis, only the
    // VALUE half happens - the frames on screen are the timeline's to choose (see
    // fitFrameAxis).
    void fitAll();

    // --- the shared FRAME window ---
    //
    // X is the TIMELINE's while the panel is a second view of one: the gui pushes the
    // frames the timeline is showing (setFrameWindow) and every horizontal gesture -
    // the wheel's and the horizontal scroll bar's frame half, of every modifier form,
    // and the pan's horizontal half - goes back to the timeline through the delegates
    // below. The panel's own X is written by setFrameWindow and by nothing else (see
    // fitFrameAxis, updateWheel, updatePan, the SCROLL BARS note).
    //
    // The window is FRACTIONAL frames, mapped ACROSS THE PANEL exactly
    // (graph::Scaler::applyFrameWindow): the timeline's viewport is expressed in content
    // pixels, so its frame edges are not whole numbers, and a camera carried as whole
    // frames cannot hold a point still - it snaps. The panel covers the timeline's own
    // columns, so this is the timeline's pixel-for-pixel mapping: a frame's x here is
    // its x there, and switching views moves nothing. The panel's pixels-per-frame
    // follows from the fit (fractional too), so one notch moves both views by the same
    // factor.
    void setFrameWindow(double aFramesLo, double aFramesHi);
    // Re-applies the pushed window for a panel covering aViewportSize, for the pushes
    // that arrive before the viewport is laid out (the plot has no width to map into
    // yet). Called from the draw path, where the geometry is known.
    void syncFrameWindowFor(const QSize& aViewportSize);
    // The two horizontal gestures, in the units each side owns: a scroll (the pan,
    // the vertical scroll bar's value half being this panel's own, and the HORIZONTAL
    // scroll bar's thumb drag and lane page) by aFrameDelta FRAMES - so 50 px of
    // pointer travel is 50 / framePpf() frames - and a wheel notch about
    // aAnchorFrame, the frame under the panel's cursor (fractional, so the timeline
    // can anchor on it exactly).
    using FrameScrollFunc = std::function<void(double aFrameDelta)>;
    using FrameZoomFunc = std::function<void(int aWheelDelta, double aAnchorFrame)>;
    void setFrameGestureDelegates(FrameScrollFunc aScrollFrames, FrameZoomFunc aZoomFrames);

    // THE RULER'S SEEK, on the TIMELINE's own route: a left press in the ruler band
    // above the plot is a seek, and a drag keeps seeking - the gesture
    // gui::TimeRuler drives for the timeline (TimeRuler::mousePressEvent ->
    // TimeLineEditorWidget::updateCursor -> the frame moves). The panel cannot move
    // the frame itself: the current frame is the DOCUMENT's (core::Project's
    // currentTimeInfo), owned by the timeline widget, so the host hands this editor
    // the timeline's own setter. With no delegate the band is not claimed at all -
    // nothing here invents a second frame authority.
    using FrameSeekFunc = std::function<void(core::Frame aFrame)>;
    void setFrameSeekDelegate(FrameSeekFunc aSeek) { mSeekFrame = std::move(aSeek); }
    // Detaches: the panel's frame axis is its own again (fit, zoom and pan work
    // as they did before a timeline was ever attached).
    void clearFrameAxis();
    // True while a timeline owns the frame axis.
    bool hasTimelineFrameAxis() const { return mFrameWindowShared; }

    // The project's own rule for the frames OUTSIDE the keys (see graph::Curve::setLoop): a looping
    // project continues the easing from the last key back to the first, wrapped by the range's span.
    // ONE source for it - the editor's curves and the model's own rebuild both read these.
    bool isLooping() const { return mProject && mProject->attribute().loop(); }
    util::Range frameRange() const { return mProject ? mProject->attribute().frameRange() : util::Range(); }

    // --- the temporary playback window ---
    //
    // The timeline's ruler gesture (shift-drag) owns this window; the graph's ruler
    // overlays the SAME window (TimeRuler::renderPlayWindow's translucent band, 2px
    // bracket stems and label pills) and recesses the frames outside it exactly as the
    // timeline recesses the frames outside its ACTIVE range. It is pushed in as a
    // VALUE by the GUI (gui::MainDisplayWidget::syncGraphPanelSharedState) when a
    // timeline is attached, so both views read one window and there is no second
    // gesture state here.
    void setPlayWindow(const TimeLineEditor::PlayWindow& aWindow) { mPlayWindow = aWindow; }
    const TimeLineEditor::PlayWindow& playWindow() const { return mPlayWindow; }
    // The range the ruler treats as ACTIVE: the temporary window while it is active,
    // the project's frame range otherwise - ctrl::TimeLineEditor::playbackRange()'s
    // own rule, which is the timeline ruler's fade basis. The model publishes it as a
    // per-frame classification (inside/outside), never as ink.
    util::Range activeRange() const {
        return mPlayWindow.active() ? util::Range(mPlayWindow.start, mPlayWindow.end) : frameRange();
    }

private:
    // The curve under a viewport point: which curve, which segment and - when
    // the hit is a tangent handle - which handle.
    struct Hit {
        Hit();
        bool hitCurve;
        bool hitHandle;
        int item;      // index into curves()
        int segment;   // segment index inside the curve
        int handle;    // 0 = leaving `from`, 1 = arriving at `to`
        core::Frame keyFrame; // the key hit when it is a key, not a handle
    };
    // What the graph draws: one curve per visible channel of one target, with
    // the target and channel the hit-test and the edit path need.
    struct Entry {
        core::ObjectNode* node;
        core::ObjectNode* line; // the layer that owns the key map (== node for now)
        graph::Channel channel;
        graph::Curve curve;
    };
    QVector<Entry> buildEntries() const;
    QRect currentPanelRect(const core::CameraInfo& aCamera) const;
    // Hands the project's fps and frame-range end to mScale's frame axis. The
    // two setters rebuild the tick ladder, so this is called from the paths that
    // already have a camera (they run on every cursor event).
    void syncFrameAxis();
    // A curve of this project's channel, carrying the project's loop rule for the frames outside the
    // keys (see graph::Curve::setLoop), and the value of one component at a frame through it.
    graph::Curve curveFor(const core::TimeLine& aLine, const graph::Channel& aChannel) const;
    float sampledChannelValue(
        const core::TimeLine& aLine, core::TimeKeyType aType, int aComponent, core::Frame aFrame
    ) const;

    // Re-applies the frame window the timeline pushed, so a panel rect that moved
    // (a resize, the first cursor pass after a setTargets) still shows exactly that
    // window. A no-op while no timeline owns the frame axis, and while the mapping is
    // already the one for this window and plot.
    void syncFrameWindow();

    // Fits the value axis only when the shown curves CHANGED since the last fit,
    // by comparing the (node, channel) basis below; it also drops a focus that
    // left the shown set. This is the fit the non-const paths call: setTargets,
    // setProject and updateCursor - and setTargets is also the path that covers
    // what the deleted setVisible(true) used to fit for: the application hands the
    // panel its targets when the document opens and whenever the tree selection
    // changes, so the window is fitted BEFORE the dock's page is ever shown, and a
    // page switch alone shows curves whose fit is already the one they got.
    // The paths that must NOT run it: a paint no longer exists in this class
    // (the panel is drawn from the model), and updateWheel is not one either:
    // zooming changes no curve, so the user's window has to survive it.
    void syncValueAxis();
    // True when (mFocusNode, mFocusChannel) is one of aItems, i.e. when the
    // focused channel is still something the panel draws.
    bool focusIsShown(const QVector<Entry>& aItems) const;
    // The (node, channel) list buildEntries() is drawing right now, in its own
    // order: the identity of a shown curve SET, which is the granularity the fit
    // is keyed to. Two selections that show the same (layer, channel) pairs are
    // the same basis even if a key moved between them - only a channel APPEARING
    // (which needs its first key, since buildEntries skips an empty map) or
    // leaving is a new one. A key added to an already-shown channel therefore
    // does not refit: the fit follows what is DRAWN, and a window the user set
    // outlives every edit that is not a change of the drawn set.
    static QVector<QPair<core::ObjectNode*, graph::Channel> > basisOf(const QVector<Entry>& aItems);
    // The fit itself, over entries the caller already built: the min/max of the
    // key values and of kFitSamplesPerSegment samples per segment (overshoot
    // included), over EVERY shown curve (the focus does not narrow it). Leaves
    // the window alone when nothing it covers has a key.
    void applyValueFit(const QVector<Entry>& aItems);

    // The index of the selection entry that COVERS (aNode, aFrame) of aChannel's
    // type at aChannel's component - the entry naming that exact marker, or a
    // whole-key entry, which covers every one of them - or -1. Pointer identity
    // plus the type, the frame and the component, which is the quadruple a marker
    // is named by: the same quadruple selectMarker writes, and the same rule
    // isMarkerSelected publishes.
    int markerIndexOf(
        const core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame
    ) const;
    // The target naming (aNode, aChannel's type, aFrame): the KEY half of a marker's
    // name, in the timeline's own currency. Its pos is null when the node owns no
    // timeline (a folder, or a node kind that holds no keys), which is the "nothing
    // to name" answer every caller tests before it writes a selection.
    static core::TimeLineEvent::Target markerTarget(
        core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame
    );

    // --- marquee ---
    // Begins the marquee a PLAIN press in the plot opens on a point that is no key,
    // no handle and no curve's band: only the origin is recorded, so a press that
    // never moves selects nothing (an empty rectangle contains no key) - which is
    // the "clear the selection" gesture for that press.
    void beginMarquee(const QPoint& aPos);
    void updateMarquee(const QPoint& aPos);
    // Applies the marquee to the selection: every MARKER of every key whose drawn
    // square's centre falls inside the rectangle - written as one whole-key entry
    // per key, which IS all of its markers - replacing the selection (or added to
    // it, when mAddToSelection - the Shift state the press recorded). A Shift-marquee
    // over a key that was selected in ONE component therefore makes it WHOLE: the
    // partial entry is taken out and the whole-key one put in its place. A gesture
    // that never got 4px away from the press on either axis is the CLICK instead
    // (it clears the selection and selects nothing): that is where the design puts
    // the line between a click and a drag, and the live box is still drawn either
    // way. Then clears the marquee state.
    void endMarquee();

    // --- middle-button pan ---
    // The middle drag moves the graph's window, not the canvas camera: the panel
    // is up, so the middle button belongs to it. The pan is a plain pixel offset
    // on both axes (Scaler::panFrame / panValue), so a pan cannot zoom and a zoom
    // cannot pan. aPos is the pointer the pan follows.
    void beginPan(const QPoint& aPos);
    void updatePan(const QPoint& aPos);
    void endPan();

    // Pans the FRAME axis by aPixelDelta pixels of pointer travel: this panel's
    // own mapping when the frame axis is its own, or the timeline's delegate in the
    // frames both views share when the timeline owns the axis (the pixel-to-frame
    // sign lives HERE and nowhere else - see the SCROLL BARS note and updatePan).
    void panFramesByPixels(graph::Scaler& aScale, int aPixelDelta);

    // --- the scroll bars (see the SCROLL BARS note in the public section) ---
    // The bar a press grabbed: none, the horizontal one (the frame axis) or the
    // vertical one (the value axis). It distinguishes nothing once the drag is in
    // flight - both pan their axis by the pointer's delta - but the axis is what
    // the drag names.
    enum ScrollBarAxis { ScrollBar_None = -1, ScrollBar_Horizontal = 0, ScrollBar_Vertical = 1 };

    // The bars for aScale's plot rect, with the two axes' content: the project's
    // frame range and the shown curves' value extent (valueExtentOf below; a
    // degenerate extent - no key - fills the vertical thumb's track). aItems are the
    // built entries, so a caller that already has them pays no second build.
    graph::Scaler::ScrollBars scrollBarsFor(const graph::Scaler& aScale, const QVector<Entry>& aItems) const;

    // The press routing's own bar test: true when aPos is on a bar WITH TRAVEL and a
    // gesture was armed for it. A press on the RUN (the slider's span) starts the drag
    // where it is; a press on the LANE beside the run first moves the window ONE PAGE
    // toward it (a page = the visible window) and then continues as that same drag.
    // Sets mDragging (so ownsCursor keeps the cursor for every later move and the
    // release) and the axis, and pushes nothing.
    bool beginScrollBarPress(const graph::Scaler::ScrollBars& aBars, const QPoint& aPos);

    // The value extent of a built entry list: the union of every curve's key values
    // and of kFitSamplesPerSegment samples per segment. False when no entry has a
    // key. ONE walk for the fit (applyValueFit) and for the vertical scroll bar's
    // content (shownValueExtent), so the two cannot drift.
    static bool valueExtentOf(const QVector<Entry>& aItems, float& aLo, float& aHi);

    Hit hitTest(const QVector<Entry>& aItems, const graph::Scaler& aScale, const QPoint& aPos) const;

    // The entry whose curve band holds aPos, or -1 when none does. The band is
    // kCurveBandRadius wide, measured on the value axis between the point and
    // the value the curve evaluates at the point's own (integral) frame, and it
    // only exists where the curve is DRAWN: inside the plot, at a frame between
    // the curve's first and last key (the stroke goes no further). Nearest
    // wins, so two curves crossing cannot make the hit ambiguous.
    int curveBandItem(const QVector<Entry>& aItems, const graph::Scaler& aScale, const QPoint& aPos) const;

    void beginDrag(const Hit& aHit, const QVector<Entry>& aItems);
    // Arms the key move a press on a KEY opens: no command yet - the first
    // ACCEPTED step is what commits, so a press that never moves leaves the
    // history untouched. aPressPos is where the button went down; the frame and
    // the value there are the origins of the two pointer deltas the move follows.
    void beginKeyMove(const Hit& aHit, const QVector<Entry>& aItems, const QPoint& aPressPos);
    void updateDrag(const QPoint& aPos);
    // Steps the armed key move to aPos; true when the selection moved (i.e. when
    // the caller has something to repaint). Both axes move - every member's frame
    // by the pointer's frame delta, the pressed component of the pressed type by
    // its value delta - unless the shift modifier constrains the gesture to the
    // dominant axis. A destination frame another key of that type owns refuses the
    // WHOLE step: nothing moves, the history is untouched and the gesture stays
    // open. Stops the gesture (with a log line) when the PRESSED key is no longer
    // in the document.
    bool updateKeyMove(const QPoint& aPos);
    // Closes whatever pointer gesture is in flight (the easing gesture, the key
    // move, the marquee, the pan) and clears every flag it set. The commands
    // already pushed stay on the stack - they ARE the gesture's undo step - so
    // this only drops the handles to them.
    void endDrag();

    // Pushes the key the insert writes: a clone of the key playback is on at
    // aFrame - the payload and the easing the graph does not draw, i.e. the
    // easing of the very segment the insert splits - with every component the
    // graph draws overwritten by the value the curve of that component samples
    // at aFrame. Pushed by the timeline's own TimeLineUtil::pushNew*Key helper,
    // so the notifier, the macro, the new-key command and the key's frame are
    // that path's, exactly as if the timeline had added the key. False when
    // there is no key to clone from (a shown curve always has one, so this is
    // the guard for a caller that hands in an empty map).
    bool pushSeededKey(
        core::TimeLine& aLine, core::ObjectNode& aTarget, const graph::Channel& aChannel, core::Frame aFrame
    );

    core::Project* mProject;
    UILogger& mLogger;
    QVector<core::ObjectNode*> mTargets;
    QVector<QPair<core::ObjectNode*, graph::Channel> > mHiddenChannels;
    // The fallback selection, used while no timeline is attached, and the pointer
    // every selection method goes through (see setSelectionSource): it starts on
    // the fallback and is pointed at the attached timeline's own live set when
    // one is wired in. The pointer is never null, so no method has a second path
    // for "there is no selection", and there is only ever ONE selection - the
    // panel's own until the timeline's replaces it, never both.
    time::Selection mOwnSelection;
    time::Selection* mSelection;
    graph::Scaler mScale;

    // The shared frame window (see setFrameWindow): the flag says a timeline owns the
    // panel's frame axis, the window is what it pushed (kept so the mapping can be
    // re-derived when the panel rect moves), and the two delegates are the gestures
    // that go back to the timeline. Cleared together by clearFrameAxis.
    //
    // mAppliedLo/mAppliedHi/mAppliedPanelWidth are what the mapping was last computed
    // from (the panel's width, which the left-to-right mapping spans), so
    // syncFrameWindow() maps only on a real change.
    bool mFrameWindowShared;
    double mFrameWindowLo;
    double mFrameWindowHi;
    double mAppliedLo;
    double mAppliedHi;
    int mAppliedPanelWidth;
    FrameScrollFunc mScrollFrames;
    FrameZoomFunc mZoomFrames;
    FrameSeekFunc mSeekFrame;
    // True from a band press to its release: the seek's own drag state (the button
    // stays down while the pointer leaves the band, as every gesture here does).
    bool mSeeking = false;

    // The temporary playback window pushed by the GUI (see setPlayWindow). Inactive by
    // default, so an editor with no timeline attached recesses by its project range -
    // the same basis the timeline's own ruler uses when no window is drawn.
    TimeLineEditor::PlayWindow mPlayWindow;

    // The modifier state the widget fed in on the last event (see setModifiers).
    // Read by the press routing (Ctrl = insert) and by every step of a key move
    // (Shift = axis snap, Shift = add-to-selection on a marquee press).
    Qt::KeyboardModifiers mModifiers;

    // The sticky focus: the one shown channel the panel EMPHASISES. A nullptr
    // mFocusNode is "no focus" (mFocusChannel is then unused). Held by pointer
    // identity like mHiddenChannels - never dereferenced to decide anything but
    // identity - and dropped by syncValueAxis when the channel leaves the shown
    // set, with setProject clearing it through the same path as mTargets. It
    // moves NO window: see DELIBERATE FIT in the public section.
    core::ObjectNode* mFocusNode;
    graph::Channel mFocusChannel;

    // The (node, channel) basis the last value fit was computed from. Empty is a
    // real value ("nothing shown"), so a fresh editor fits on the first sync that
    // has data; a wheel zoom never touches it, which is what lets the user's
    // window survive every later cursor pass and paint. Compared by pointer
    // identity and never dereferenced, like mHiddenChannels: a deleted target can
    // leave an entry nothing reaches, and setProject clears the basis with the
    // rest of the old document's state.
    QVector<QPair<core::ObjectNode*, graph::Channel> > mValueBasis;

    // Drag state, and the flags that say WHICH drag. mDragging is true while ANY
    // of the panel's pointer gestures is in flight (a handle drag, a key move, a
    // marquee, a middle-button pan, a scroll-bar drag - a press that begins one
    // sets it, and ownsCursor keeps the cursor while it is true); mKeyMoveArmed,
    // mMarqueeArmed, mPanning and mScrollBarAxis then distinguish them. They are
    // mutually exclusive by construction: each press entry point clears the others
    // (the press routing's endDrag runs before any of them arms), and endDrag clears
    // them all.
    //
    // Handle drag: the segment's `from` key (the anchor the segment index is
    // checked against), the segment, and which of its two handles was grabbed.
    // The grabbed handle is written where the pointer is; the OPPOSITE handle of
    // the KEY it belongs to - which lives in the neighbouring segment's param -
    // is written at its mirror about that key in PIXELS (see updateDrag), so
    // which of the two the press grabbed decides both what follows the pointer
    // and which neighbour is the mirror's home.
    bool mDragging;
    bool mKeyMoveArmed;
    bool mMarqueeArmed;
    bool mPanning;
    const core::TimeKey* mDragKey;
    int mDragItem;
    int mDragSegment;
    int mDragHandle;
    cmnd::Gesture<HandleDragCommand> mGesture;
    // The KEY MOVE's own gesture, over the one command that carries the whole
    // selection (see KeyMoveCommand). A separate Gesture from the handle drag's
    // because the kinds never coexist: each press clears the other, and the two
    // edits have different command types.
    cmnd::Gesture<KeyMoveCommand> mKeyMoveGesture;

    // The marquee: where the press went down and where the pointer is now, both
    // in viewport pixels. The rectangle is the two corners - the box the pointer
    // dragged, normalized when it is read (marqueeRect). mAddToSelection is the
    // shift state the press recorded, so a Shift-press that becomes a marquee
    // adds to the selection rather than replacing it.
    QPoint mMarqueeOrigin;
    QPoint mMarqueeCurrent;
    bool mAddToSelection;

    // The middle-button pan: the pointer position the pan last moved to, so each
    // step is a pixel delta (there is no press-time origin to drift from).
    QPoint mPanPointer;

    // The scroll-bar drag: which bar (ScrollBar_None when no bar drag is in flight -
    // a plain member rather than a flag beside mDragging, because the axis IS the
    // state) and the pointer position it last moved to, for the same pixel-delta
    // reason mPanPointer keeps one. Cleared by endDrag with every other gesture, and
    // pushed nowhere: a scroll bar moves a window and never the document.
    int mScrollBarAxis;
    QPoint mScrollBarPointer;

    // The key-move gesture's armed PRESSED key, in the timeline's own currency:
    // the layer that owns the key map, the node the TimeLineEvent names, the key
    // type, and the frame the key is at NOW (advanced by every accepted move).
    // Held instead of an index into buildEntries() so the move follows the key
    // the press hit rather than the position of a rebuilt list. The frame and
    // value DELTAS the whole gesture applies are measured from this key's press
    // (mMoveFrame here is the anchor the anchor-check and the notification name).
    // The COMPONENT the press landed on is not kept here: it is the marker's own
    // half of the name, and every member carries its own (MoveMember::component),
    // the pressed one included.
    core::ObjectNode* mMoveLine;
    core::ObjectNode* mMoveNode;
    core::TimeKeyType mMoveType;
    core::Frame mMoveFrame;
    // The pointer's frame as of the last accepted step, and the pointer pixels
    // the press went down at: every key follows the pointer's frame DELTA from the
    // last ACCEPTED frame, so a refused step does not accumulate, and the
    // dominant-axis rule under Shift is decided on the pixel displacement from
    // the press - which is the movement the user actually made.
    core::Frame mMovePointerFrame;
    QPoint mMovePressPos;
    // THE FRAME HALF'S units: ONE entry per KEY the panel's SELECTION named when
    // the press armed the move, deduplicated by the key itself - two selected
    // markers of one key are one frame, and moving it twice would be a double
    // shift. TimeLineUtil::MoveFrameOfKey is handed exactly these targets, one per
    // key, so its own conflict rule and its own sort stay valid. A key whose type
    // is not graphable appears here and in no member below: a frame is the only
    // thing the panel can move for it.
    struct MoveKey {
        core::TimeLineEvent::Target target; // node + (line, type, frame) at the press
        core::TimeKey* key;                 // the key, compared only (never dereferenced)
    };
    QVector<MoveKey> mMoveKeys;

    // THE VALUE HALF'S units: ONE entry per MARKER the panel's SELECTION named when
    // the press armed the move - a (key, component) pair, so a whole-key entry (the
    // timeline's own selection) contributes one member per graphable component of
    // its type. Every member is written on its own component; the frame it held at
    // the press is the base the gesture's one value delta is added to, so the
    // gesture is one absolute function of the pointer and cannot accumulate a key's
    // clamp. A marker that was NOT selected has no member here and comes out of the
    // gesture untouched.
    struct MoveMember {
        core::TimeLineEvent::Target target; // node + (line, type, frame) at the press
        core::TimeKey* key;                 // the marker's own key
        int component;                      // the component of that key this marker is
        float baseValue;                    // the value it held at the press
    };
    QVector<MoveMember> mMoveMembers;

    // The FPS last handed to mScale.frameScale(). It is what the axis was BUILT
    // with, not document state the editor draws from: setFps() rebuilds the tick
    // ladder, so the seed is skipped while the project's fps is unchanged (the
    // frame end is compared through the scaler's own getter).
    int mAxisFps;

    // The frame-number format the panel's ruler writes (see timeFormatType). Seeded
    // from the same QSettings key the timeline's editor reads, in the constructor.
    core::TimeFormatType mTimeFormat = core::TimeFormatType_Frames_From0;
};

} // namespace ctrl

#endif // CTRL_GRAPHEDITOR_H
