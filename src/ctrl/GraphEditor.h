#ifndef CTRL_GRAPHEDITOR_H
#define CTRL_GRAPHEDITOR_H

#include <QRect>
#include <QVector>
#include "cmnd/BasicCommands.h"
#include "cmnd/Gesture.h"
#include "core/AbstractCursor.h"
#include "core/ObjectNode.h"
#include "core/Project.h"
#include "core/CameraInfo.h"
#include "core/TimeLineEvent.h"
#include "ctrl/UILogger.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Scaler.h"

namespace ctrl {

// The frame-move command the key-move gesture drives (defined in
// ctrl/TimeLineUtil.h). Declared here so this header does not drag the timeline
// utilities - and every key type they include - into every consumer, the same
// reason the Entry list below is built from core headers alone.
namespace TimeLineUtil {
    class MoveFrameOfKey;
}

// The animation graph editor: the value curves of the selected layers'
// channels, drawn as a fullscreen overlay over the animation.
//
// The panel covers the WHOLE viewport and has no background of its own - it is
// an overlay over the artwork, not a band beside it. That is a change with two
// consequences the rest of this header spells out:
//   * it must NOT own the pointer by rectangle (a fullscreen rect contains
//     everything, so a rect test would swallow every event the viewport tools
//     need): ownership is HIT-ONLY, see ownsCursor;
//   * the cursor is the only thing that marks a channel, so the panel keeps a
//     STICKY FOCUS - one shown channel the value axis follows - which a press
//     on a curve sets and a press on empty space deliberately leaves alone.
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
// cmnd::ModifiableAssign<util::Easing::Param>, and the three key edits - insert,
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

    // Shown/hidden. Hidden keeps every state (selection, scale, hidden channels)
    // so the panel comes back as it was.
    void setVisible(bool aVisible);
    bool isVisible() const { return mVisible; }

    // The panel rect for a viewport of this size, in viewport pixels: the WHOLE
    // viewport. The panel is a fullscreen overlay drawn over the animation with
    // no background of its own, so this is not a band and there is no inset or
    // height to clamp. Because it covers everything, it cannot own the pointer
    // by rectangle either - ownership is hit-based, see ownsCursor - and the
    // pointer passes through to the viewport tools wherever the graph has
    // nothing to act on, which is what keeps the canvas usable while the graph
    // is up. Still a pure function of the size: no camera, no layout pass.
    static QRect panelRect(const QSize& aViewportSize);

    // Cursor handling, the IEditor::updateCursor shape: returns true when the
    // graph needs a repaint.
    //
    // ownsCursor is the arbitration, and it is HIT-BASED, not rect-based: the
    // panel is fullscreen, so "inside the panel" would mean "inside the
    // viewport" and the graph would swallow every event the tools need. The
    // graph owns the cursor only when the pointer is on something it can act on
    // - a key, a tangent handle, or the band around a curve the insert gesture
    // edits (kCurveBandRadius; that band has to be owned, or the press that
    // inserts could never reach this class at all, since the Driver routes by
    // this test alone) - or while a drag is in flight; everywhere else it passes
    // the event through and the viewport tools keep working. The Driver asks
    // ownsCursor first and routes the event to exactly one owner - the viewport
    // has no accept()/reject() seam, and adding one would change every mode's
    // contract.
    bool ownsCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor) const;
    bool updateCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor);

    // Wheel over the panel: zoom. Returns true when the graph took the event, so
    // the viewport does not also zoom the canvas. aZoomValue picks the value
    // axis; otherwise the frame axis zooms about the cursor.
    bool updateWheel(const core::CameraInfo& aCamera, const QPoint& aPos, int aWheelDelta, bool aZoomValue);

    // The curves currently drawn: one per visible channel of every target.
    // Rebuilt from the document on demand, so an edit anywhere is reflected.
    QVector<graph::Curve> curves() const;

    // Channel visibility, keyed by (target, channel). A channel is visible
    // unless it has been hidden.
    void setChannelVisible(core::ObjectNode& aTarget, const graph::Channel& aChannel, bool aVisible);
    bool isChannelVisible(const core::ObjectNode& aTarget, const graph::Channel& aChannel) const;

    // The selection, in the timeline's own currency.
    bool retrieveSelection(core::TimeLineEvent& aOut) const;
    void clearSelection();
    void selectKey(core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame);

    // --- the three key edits: add, move, remove ---
    //
    // The panel's document-mutating gestures, next to the handle drag below.
    // Each is ONE undo step, and each goes through the same
    // ctrl::TimeLineUtil::Notifier + cmnd::ScopedMacro route the drag (and the
    // timeline) uses: one core::TimeLineEvent in the timeline's own currency
    // reaches the rest of the application, so the timeline and the object tree
    // see an edit made here exactly as they see one made there.
    //
    // ADD. A press on a curve's BAND - a point on no key and no handle, whose
    // frame the curve covers and whose value is within kCurveBandRadius pixels
    // of the value the curve evaluates there - inserts the key the curve already
    // passes through: the value written is the curve's own sample at that frame,
    // so inserting it changes no sampled value AT THAT FRAME. The new key's
    // payload - the components the graph does not draw, and the key's easing -
    // is taken from the key playback is on at that frame (the segment's own
    // `from` key, which owns that segment's easing), so the insert invents
    // neither a value nor a payload. The inserted key becomes the panel's
    // selection and its channel the focused channel, exactly as a pressed key
    // would (so a following removeSelectedKey() removes it; and a REAL change of
    // focus moves the value window, as the focus always does - re-focusing the
    // channel the press was already working on refits nothing, and the refit
    // itself lands when the gesture is over, per the focus policy's PENDING
    // rule: an insert press never moves the band under its own release).
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
    //
    // On the press it listens to: the panel is fed core::AbstractCursor events,
    // and a double-click is not one of them - the viewport path never calls
    // setMouseDoubleClick, and the flag it would set is private anyway - so the
    // insert is a PLAIN press, and a double-click reads as an insert followed by
    // a press on the key just inserted, i.e. exactly like a double-click
    // anywhere else: the first click creates, the second selects what it
    // created.
    bool insertKeyAt(const core::CameraInfo& aCamera, const QPoint& aPos);

    // MOVE. Pressing a KEY (not a handle) arms a move: while the button is down
    // the key follows the pointer's frame delta (the timeline's own rule, so
    // grabbing a key off its centre does not make it jump by the offset), and
    // the whole gesture is ONE undo step. It is driven by updateCursor, like the
    // handle drag, and the rules are:
    //   * the first ACCEPTED move opens one TimeLineUtil::MoveFrameOfKey through
    //     the notifier + macro route; every later move of the gesture rewrites
    //     that same command through its own modifyMove, so a drag cannot push
    //     one command per move;
    //   * a destination frame that already holds a key of that type is REFUSED
    //     (MoveFrameOfKey's own conflict rule): the key keeps the last frame it
    //     was accepted at, nothing is announced, and the gesture stays open so
    //     the pointer can be dragged on to a free frame. Nothing is pushed
    //     before a move is accepted, so a refused drag leaves no empty history
    //     entry behind (the timeline's own move pushes its command on the first
    //     drag and can then be refused, which does leave one; the graph does not
    //     repeat that);
    //   * a move changes the FRAME only: the key's value, its easing and its
    //     payload are untouched;
    //   * only the pointer's x is read, so a vertical drag moves nothing (a
    //     value edit is a design of its own and deliberately not here);
    //   * an undo mid-drag drops the command's modifiable link, so the next move
    //     opens a fresh command from the frame the key is at then; a press that
    //     never crosses a frame boundary pushes nothing;
    //   * the armed key is held in the timeline's currency (the layer that owns
    //     the map, the node the event names, the type and the frame it is at
    //     now), not as an index into a rebuilt entry list, so the move follows
    //     the key the press hit; if that key is gone (an undo or another view
    //     removed it) the gesture stops and says so, like the handle drag's
    //     anchor check.
    //
    // REMOVE. Removes exactly the key the panel's CURRENT selection names - the
    // key a press last hit, since a press on a key both selects and focuses it -
    // through TimeLine::createRemover inside the notifier + macro route. One
    // undo step, and an undo restores the SAME key object with its value and
    // easing, because the removal is a map removal and not a rewrite. With no
    // key selected, or with a selection naming a frame that holds no key of that
    // type, nothing is pushed and no empty entry is left behind. The selection
    // is dropped with the key.
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
    // panel sets it the same way the timeline's key menu does). A target whose
    // frame holds no key any more is skipped - a stale selection names a key that
    // is not in the document - and so is a key whose type carries no easing
    // (graph::Curve::easingOf answers null) and a key whose easing type is
    // ALREADY aType, so a menu entry that changes nothing leaves no history entry
    // behind. Nothing else is dropped silently: the ones that are written all
    // appear in the notifier's event.
    //
    // EMPTY SELECTION (or a selection of which nothing survives the rules above):
    // returns false and pushes NOTHING. That is not a micro-optimisation -
    // cmnd::Stack appends a macro even when it holds no command and counts it as
    // an edit, so an action that opened the macro first would leave an empty undo
    // step, exactly the defect removeSelectedKey's own guard avoids.
    //
    // SEVERAL KEYS: every target that survives is written, and the whole batch
    // stays ONE undo step, because it is one macro. Today's panel cannot make
    // such a selection - selectKey sets a single target, and nothing else writes
    // mSelection - so this is the contract the implementation honours rather than
    // a state the UI can reach; the timeline is where a multi-key selection is
    // made today.
    //
    // The Image channel is deliberately NOT written: its keys carry an easing
    // param, but playback step-holds image offsets (graph::Channel::isInterpolated
    // is false for it), so the param is never consulted and the graph refuses the
    // edit - which is also what the timeline's own easing menu does with Image.
    // The panel's easing action therefore reports false for a selection on an
    // image channel rather than recording a change nothing can see.
    bool setSelectedKeysEasing(util::Easing::Type aType);

    // The half-width, in pixels, of the band around a curve that the panel OWNS
    // for the insert gesture (see insertKeyAt's ADD note and ownsCursor): a
    // point whose frame the curve covers and whose value is within this distance
    // of the value the curve evaluates at that frame is a point on the curve.
    // Deliberately smaller than graph::kKeyHitRadius (10) and
    // graph::kHandleHitRadius (12), so a press near an existing key or handle
    // always hits that item first and only a genuinely empty stretch of a curve
    // inserts.
    static constexpr int kCurveBandRadius = 6;

    // --- sticky focus ---
    // The panel marks ONE shown channel as focused: the channel a press last hit
    // on a curve, and the one the value axis follows.
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
    //     cannot be what the axis follows;
    //   * the fit the focus implies is PENDING, never applied between a press
    //     and its release. Setting or clearing the focus only raises a flag; the
    //     window is refitted once the gesture that arrived with it is over - at
    //     the end of the release handling, or on a plain hover pass - and never
    //     during a press, during a release, or while a drag is in flight. That
    //     is not a detail of the fit but a correctness rule of the pointer: a
    //     press focuses (and may arm a move or a handle drag) before the gesture
    //     it opens is interpreted, so a fit landing there would move the key or
    //     handle the user just grabbed out from under the pointer, and the drag
    //     would then run against geometry the fit had already invalidated.
    bool hasFocus() const { return mFocusNode != nullptr; }
    // The focused layer, or nullptr when nothing is focused.
    const core::ObjectNode* focusedNode() const { return mFocusNode; }
    // The focused channel; meaningful only while hasFocus() is true.
    graph::Channel focusedChannel() const { return mFocusChannel; }
    // Makes (aNode, aChannel) the focused channel and fits the value axis to it.
    // The pair is expected to be one of the shown entries. Re-focusing the
    // channel that is already focused is a no-op, so a handle grab on the
    // focused channel does not move a window the user set, and the shown-curve
    // basis is never touched - only a real change of focus refits. The fit is
    // PENDING: see the timing rule in the policy above.
    void setFocusedChannel(core::ObjectNode& aNode, const graph::Channel& aChannel);
    // Drops the focus; the value axis goes back to the union of the shown curves
    // (through the same guard: a keyless panel leaves the window alone), on the
    // same PENDING timing as setFocusedChannel.
    void clearFocus();

    // The document's current frame, for views that draw the playhead. -1 when no
    // project is set (core frame numbers start at 1, so -1 cannot be a real
    // frame).
    core::Frame currentFrame() const;

    // The scale, so the view can zoom and pan (wheel) and so a caller can query
    // the window it is showing.
    graph::Scaler& scale() { return mScale; }
    const graph::Scaler& scale() const { return mScale; }

    // Fits the value axis to the values the SHOWN curves occupy now: the min/max
    // of every shown curve's key values AND of a walk through its segments, since
    // a Custom easing (Back/Elastic) reaches values beyond the two keys' values.
    // When a channel is FOCUSED the fit covers that channel ALONE (the focus
    // drives the fit); otherwise it is the union of every shown curve.
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
    void fitFrameAxis();
    // Both axes at once: the panel's double-click fit. Fits the value axis to
    // the focused channel (or the union of the shown curves) and the frame axis
    // to every shown key's frame, so the whole of the shown data is on screen.
    void fitAll();

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

    // Fits the value axis only when the shown curves CHANGED since the last fit,
    // by comparing the (node, channel) basis below; it also drops a focus that
    // left the shown set. This is the fit the non-const paths call: setTargets,
    // setVisible(true), setProject and updateCursor.
    // The paths that must NOT run it: a paint no longer exists in this class
    // (the panel is drawn from the model), and updateWheel is not one either:
    // zooming changes no curve, so the user's window has to survive it.
    void syncValueAxis();
    // True when (mFocusNode, mFocusChannel) is one of aItems, i.e. when the
    // focused channel is still something the panel draws.
    bool focusIsShown(const QVector<Entry>& aItems) const;
    // Applies the focus-driven value fit if one is pending, and clears the flag.
    // Called only where a gesture is NOT being interpreted - the end of a release,
    // or a plain hover pass - so the window cannot move between a press and its
    // release; deliberately NOT called from endDrag(), which the press branch
    // runs BEFORE it interprets the press. A no-op when nothing is pending.
    void applyPendingFocusFit();
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
    // included). Narrowed to the focused channel when the focus is one of
    // aItems, otherwise the union. Leaves the window alone when nothing it
    // covers has a key.
    void applyValueFit(const QVector<Entry>& aItems);

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
    // ACCEPTED move is what commits, so a press that never drags leaves the
    // history untouched. aPressPos is where the button went down; the frame
    // there is the origin of the pointer-delta the move follows.
    void beginKeyMove(const Hit& aHit, const QVector<Entry>& aItems, const QPoint& aPressPos);
    void updateDrag(const QPoint& aPos);
    // Steps the armed key move to the frame under aPos; true when the key moved
    // (i.e. when the caller has something to repaint). A destination another key
    // of that type owns is refused, and a refused step changes nothing - not the
    // document, not the history, not the gesture. Stops the gesture (with a log
    // line) when the armed key is no longer in the document.
    bool updateKeyMove(const QPoint& aPos);
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
    core::TimeLineEvent mSelection;
    graph::Scaler mScale;
    bool mVisible;

    // The sticky focus: the one shown channel the value axis follows. A nullptr
    // mFocusNode is "no focus" (mFocusChannel is then unused). Held by pointer
    // identity like mHiddenChannels - never dereferenced to decide anything but
    // identity - and dropped by syncValueAxis when the channel leaves the shown
    // set, with setProject clearing it through the same path as mTargets.
    core::ObjectNode* mFocusNode;
    graph::Channel mFocusChannel;

    // The focus-driven fit is PENDING, not immediate: setFocusedChannel and
    // clearFocus raise this instead of writing the window, and applyPendingFocusFit
    // consumes it once no press, release or drag is being interpreted. It exists
    // so a press can focus (and arm a gesture) without the value window moving
    // under the pointer that pressed - see the policy note in the public section.
    bool mFocusFitPending;

    // The (node, channel) basis the last value fit was computed from. Empty is a
    // real value ("nothing shown"), so a fresh editor fits on the first sync that
    // has data; a wheel zoom never touches it, which is what lets the user's
    // window survive every later cursor pass and paint. Compared by pointer
    // identity and never dereferenced, like mHiddenChannels: a deleted target can
    // leave an entry nothing reaches, and setProject clears the basis with the
    // rest of the old document's state.
    QVector<QPair<core::ObjectNode*, graph::Channel> > mValueBasis;

    // Drag state, and the one flag that says which drag: mDragging is true while
    // EITHER a handle drag or a key move is in flight (a press on a handle or on
    // a key sets it, and ownsCursor keeps the cursor while it is true), and
    // mKeyMoveArmed distinguishes them. They are mutually exclusive by
    // construction: a handle press calls beginDrag (which clears the arm), a key
    // press calls beginKeyMove (which clears the handle state), and endDrag
    // clears both.
    //
    // Handle drag: the key whose easing is being edited, the segment that owns it
    // and which of its two handles.
    bool mDragging;
    bool mKeyMoveArmed;
    const core::TimeKey* mDragKey;
    int mDragItem;
    int mDragSegment;
    int mDragHandle;
    cmnd::Gesture<cmnd::ModifiableAssign<util::Easing::Param> > mGesture;

    // The key-move gesture's armed key, in the timeline's own currency: the
    // layer that owns the key map, the node the TimeLineEvent names, the key
    // type, and the frame the key is at NOW (advanced by every accepted move).
    // Held instead of an index into buildEntries() so the move follows the key
    // the press hit rather than the position of a rebuilt list.
    core::ObjectNode* mMoveLine;
    core::ObjectNode* mMoveNode;
    core::TimeKeyType mMoveType;
    core::Frame mMoveFrame;
    // The pointer's frame as of the last accepted move: the key follows the
    // pointer's frame DELTA from the press, so grabbing a key off its centre
    // does not make it jump by the offset.
    core::Frame mMovePointerFrame;
    // The command the gesture opened, or null before its first accepted move.
    // The stack owns the command once pushed, so this is only a handle to it,
    // used to ask whether a further move may rewrite it (isModifiable) - the
    // same question the easing drag's Gesture asks. A frame destination another
    // key owns is refused BEFORE a command is opened, so a refused drag never
    // leaves an empty history entry behind.
    TimeLineUtil::MoveFrameOfKey* mMoveCommand;

    // The FPS last handed to mScale.frameScale(). It is what the axis was BUILT
    // with, not document state the editor draws from: setFps() rebuilds the tick
    // ladder, so the seed is skipped while the project's fps is unchanged (the
    // frame end is compared through the scaler's own getter).
    int mAxisFps;
};

} // namespace ctrl

#endif // CTRL_GRAPHEDITOR_H
