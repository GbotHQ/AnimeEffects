#include "ctrl/GraphEditor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#include <QObject>
#include <memory>

#include "cmnd/ScopedMacro.h"
#include "core/BlurKey.h"
#include "core/DepthKey.h"
#include "core/HsvKey.h"
#include "core/ImageKey.h"
#include "core/MoveKey.h"
#include "core/ObjectNode.h"
#include "core/OpaKey.h"
#include "core/Project.h"
#include "core/RotateKey.h"
#include "core/ScaleKey.h"
#include "core/TimeLine.h"
#include "ctrl/CmndName.h"
#include "ctrl/TimeLineUtil.h"
#include "ctrl/graph/graph_Geometry.h"

namespace {

// The panel has no geometry constants of its own any more. It used to be a HUD
// band with an inset and a clamped height; it is now the whole viewport, so
// panelRect() needs neither - and the band constants that only fed that
// geometry (the inset, the height ratio, the min/max band height and the band
// ceiling) went with it.

// The value axis' wheel sensitivity: one physical notch (Qt's 120 units of
// angleDelta) scales the window by 1.1, which is the canvas mover's own
// per-notch zoom factor - so the wheel feels the same over the panel and over
// the viewport, and a wheel up always means "closer".
constexpr double kValueZoomPerNotch = 1.1;
constexpr double kWheelNotch = 120.0;

// How many points a segment is walked through when the value axis is fitted.
// The fit cannot read the key values alone: a Custom easing's cubic bezier (and
// Back/Elastic) reaches values beyond BOTH of the keys it sits between, and a
// window fitted to the key values would map that overshoot outside the plot,
// where the panel's plot clip removes it. 16 is the order of
// Curve::buildPolyline's drawing rate, so the fit sees what is drawn.
constexpr int kFitSamplesPerSegment = 16;

double notchZoomFactor(int aWheelDelta) {
    return std::pow(kValueZoomPerNotch, static_cast<double>(aWheelDelta) / kWheelNotch);
}

double pixelDistance(const QPointF& aLhs, const QPointF& aRhs) {
    return std::hypot(aLhs.x() - aRhs.x(), aLhs.y() - aRhs.y());
}

// The inverse of graph::handlePoint for one drag position. aFlatY is what the y
// component falls back to when the segment's value span is zero: a constant
// segment has no value axis to normalize across, so the vertical component of
// the drag has nothing to write and the handle keeps the y it had.
QPointF
handleNormalized(const ctrl::graph::Scaler& aScale, const ctrl::graph::Curve::Segment& aSegment, const QPoint& aPos,
    double aFlatY) {
    // Keys are unique per frame, so a segment always spans at least one frame;
    // a degenerate span could not be inverted anyway.
    const double frameSpan = static_cast<double>(aSegment.to->frame() - aSegment.from->frame());
    if (!(frameSpan > 0.0))
        return QPointF(0.0, aFlatY);

    const double nx = (aScale.frameAtPixel(aPos.x()) - static_cast<double>(aSegment.from->frame())) / frameSpan;

    const double valueSpan = static_cast<double>(aSegment.toValue - aSegment.fromValue);
    if (valueSpan == 0.0) {
        return QPointF(nx, aFlatY);
    }
    const double ny =
        (static_cast<double>(aScale.valueAtPixel(static_cast<double>(aPos.y()))) - aSegment.fromValue) / valueSpan;
    return QPointF(nx, ny);
}

// The value the graph draws for one component of one key type at aFrame: what
// Curve::sample() returns for that (type, component) channel, i.e. the number
// playback evaluates there through the `from` key's easing.
//
// The insert seeds EVERY component of the new key through this, not just the
// one the user's curve shows: a key holds all of its type's components at once,
// so adding a key from a Move X curve must carry the evaluated Y as well or the
// insert would change the animation it is meant to leave alone.
float sampledChannelValue(const core::TimeLine& aLine, core::TimeKeyType aType, int aComponent, core::Frame aFrame) {
    return ctrl::graph::Curve(aLine, ctrl::graph::Channel(aType, aComponent)).sample(aFrame);
}

} // namespace

namespace ctrl {

    //-------------------------------------------------------------------------------------------------
    GraphEditor::Hit::Hit(): hitCurve(false), hitHandle(false), item(-1), segment(-1), handle(-1), keyFrame() {}

    //-------------------------------------------------------------------------------------------------
    GraphEditor::GraphEditor(core::Project& aProject, UILogger& aLogger):
        mProject(&aProject),
        mLogger(aLogger),
        mTargets(),
        mHiddenChannels(),
        mSelection(),
        mScale(),
        mVisible(false),
        mFocusNode(nullptr),
        mFocusChannel(),
        mFocusFitPending(false),
        mValueBasis(),
        mDragging(false),
        mKeyMoveArmed(false),
        mDragKey(),
        mDragItem(-1),
        mDragSegment(-1),
        mDragHandle(-1),
        mGesture(aProject.commandStack()),
        mMoveLine(nullptr),
        mMoveNode(nullptr),
        mMoveType(core::TimeKeyType_TERM),
        mMoveFrame(),
        mMovePointerFrame(),
        mMoveCommand(nullptr),
        mAxisFps(0) {
        // The graph starts HIDDEN. The Driver routes every cursor and wheel
        // event to the tools while isVisible() is false, so a panel nobody asked
        // for cannot take the cursor on the first frame; a user shows it and the
        // state below comes back with it.
        syncFrameAxis();
    }

    GraphEditor::~GraphEditor() {
        // Nothing here is owned, and an edit that was never committed pushes
        // nothing (cmnd::Gesture's contract), so teardown needs neither a commit
        // nor a rollback.
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::setProject(core::Project* aProject) {
        // Everything this editor holds belongs to the previous document: the
        // target pointers, the hidden-channel keys and the selection are all
        // pointers into it (or copies naming it), and an open gesture belongs to
        // its command stack.
        endDrag();
        mTargets.clear();
        mHiddenChannels.clear();
        clearSelection();
        mProject = aProject;

        if (aProject) {
            // mGesture holds its command stack by reference, so a different
            // document needs a different stack and the reference cannot be
            // reassigned. The gesture is inert at this point (endDrag() above),
            // so it is reconstructed in place - the only way to move it - and an
            // edit can never be pushed into a stack that no longer belongs to the
            // document being edited.
            mGesture.~Gesture();
            ::new (static_cast<void*>(&mGesture))
                cmnd::Gesture<cmnd::ModifiableAssign<util::Easing::Param> >(aProject->commandStack());
            syncFrameAxis();
        }

        // The pending focus fit (if any) belonged to the old document too: the
        // next sync here refits from scratch if the shown set changed, and a fit
        // left pending across a document swap is a fit against data that no
        // longer exists.
        mFocusFitPending = false;

        // A different document is different data: the remembered basis, and any
        // focus pointing into the old document, belong to it. setProject cleared
        // the targets above, so the next sync either has nothing to fit (window
        // kept) or new data to fit, and it drops the (now absent) focus.
        syncValueAxis();
    }

    void GraphEditor::setTargets(const QVector<core::ObjectNode*>& aTargets) {
        // Stored as handed in: the target list is the object tree selection's
        // own currency, and nothing is resolved here - the entries are rebuilt
        // from the document on every use, so a target whose timeline changes
        // later is still drawn correctly.
        mTargets = aTargets;

        // A new selection is a new set of shown curves, so it is a new basis and
        // the axis is fitted to it (and a focus that was on a channel the new
        // selection no longer shows is dropped by the same sync). Setting the
        // SAME selection again changes nothing and so refits nothing, which keeps
        // a wheel zoom through a redundant re-selection.
        syncValueAxis();
    }

    void GraphEditor::setVisible(bool aVisible) {
        if (mVisible == aVisible)
            return;

        mVisible = aVisible;

        if (!aVisible) {
            // Hiding drops a drag in flight. The panel is gone, so the pixels a
            // further move would be measured against are gone too, and a gesture
            // left open would keep coalescing the next edit into a command whose
            // geometry nobody can see. The selection, the scale and the hidden
            // channels are kept, so showing the panel again restores it as it
            // was.
            endDrag();
            // Nothing is in flight now, so a fit the focus still owes may land,
            // or the panel would come back showing the window of a channel it is
            // focused away from.
            applyPendingFocusFit();
        } else {
            // Becoming visible SHOWS the data, and the application toggles the
            // panel on without a cursor event ever reaching it - so the fit
            // cannot be left to updateCursor alone: this is the one path that
            // must fit on its own, or the first paint of an opened panel would
            // draw the seed window around a layer whose values live elsewhere.
            syncValueAxis();
        }
    }

    //-------------------------------------------------------------------------------------------------
    QRect GraphEditor::panelRect(const QSize& aViewportSize) {
        // The panel is a fullscreen overlay drawn over the animation with no
        // background of its own: the WHOLE viewport, with no band, no inset and
        // no clamped height. It cannot own the pointer by rectangle for exactly
        // that reason - a rect that contains everything would swallow every
        // event the viewport tools need - so ownership is hit-based instead, see
        // ownsCursor. Still a pure function of the size: no camera, no layout
        // pass.
        return QRect(QPoint(0, 0), aViewportSize);
    }

    QRect GraphEditor::currentPanelRect(const core::CameraInfo& aCamera) const {
        // Pure: the panel is pinned over the canvas, so it never depends on the
        // camera's transform, only on the viewport it covers (the whole of it).
        // The callers that are allowed to write (updateCursor/updateWheel) copy
        // this into mScale; ownsCursor re-derives it into a local copy, because a
        // hit query may not mutate the editor.
        return panelRect(aCamera.screenSize());
    }

    //-------------------------------------------------------------------------------------------------
    // The frame axis' tick ladder belongs to the timeline's scaler, so the graph
    // hands it the same pair ctrl::TimeLineEditor does: the project's fps and the
    // end of its frame range. Both setters rebuild the ladder, so they are called
    // only when the attribute actually moved - this runs on every cursor event,
    // and the project's attribute can change while the panel is open.
    void GraphEditor::syncFrameAxis() {
        if (!mProject)
            return;

        const core::Project::Attribute& attribute = mProject->attribute();
        const int fps = attribute.fps();
        const int frameEnd = attribute.frameRange().max();

        ctrl::time::Scaler& axis = mScale.frameScale();
        if (mAxisFps == fps && axis.frameEnd() == frameEnd)
            return;

        axis.setFps(fps);
        axis.setFrameEnd(frameEnd);
        mAxisFps = fps;
    }

    //-------------------------------------------------------------------------------------------------
    // The value axis' fit. The window is where the user's data is, and the only
    // other writer is the wheel: with no fit the axis keeps its seed 0..1, so a
    // channel whose values are 0..90 (a rotate curve) maps its whole curve and
    // its keys outside plotRect(), where the panel's plot clip removes them and the
    // panel draws an empty grid.
    void GraphEditor::fitValueAxis() {
        const QVector<Entry> entries = buildEntries();
        applyValueFit(entries);
        // The basis this fit ran on, so the next syncValueAxis() compares against
        // it instead of repeating the fit - which would also overrule a window a
        // caller set right after fitting.
        mValueBasis = basisOf(entries);
        // This fit already covers whatever focus was pending, so nothing is owed.
        mFocusFitPending = false;
    }

    void GraphEditor::fitFrameAxis() {
        // The frame window follows the shown key FRAMES, the same way the value
        // window follows the shown key values. Collected here rather than inside
        // graph::Scaler because "which frames are shown" is this class' question;
        // the scaler is only handed the interval.
        bool any = false;
        core::Frame lo;
        core::Frame hi;

        for (const Entry& entry : buildEntries()) {
            for (const core::TimeKey* key : entry.curve.keys()) {
                const core::Frame frame(key->frame());
                if (!any) {
                    lo = frame;
                    hi = frame;
                    any = true;
                } else {
                    lo = std::min(lo, frame);
                    hi = std::max(hi, frame);
                }
            }
        }

        // No shown key: exactly the empty-panel rule the value fit follows - the
        // frame window the user set is left alone rather than collapsed.
        if (!any)
            return;

        mScale.fitFrameAxis(lo, hi);
    }

    void GraphEditor::fitAll() {
        // Both axes at once, which is what the panel's double-click does. The
        // value side goes through the same applyValueFit the cursor path uses, so
        // a focused channel still wins; the frame side covers every shown key.
        const QVector<Entry> entries = buildEntries();
        applyValueFit(entries);
        mValueBasis = basisOf(entries);
        // fitAll is an explicit user fit: it satisfies the focus's own fit, so
        // nothing stays pending behind it.
        mFocusFitPending = false;
        fitFrameAxis();
    }

    void GraphEditor::syncValueAxis() {
        const QVector<Entry> entries = buildEntries();

        // Sticky-focus pruning: a focused channel that is no longer among the
        // shown entries cannot be what the axis follows, so it is dropped here.
        // setTargets and setProject reach this through their own call, and a
        // channel the user hides drops its focus the same way. Whenever this
        // fires the shown SET changed, so the basis test below refits the window
        // (to the union, there being no focus left) rather than leaving it on a
        // channel that is gone.
        if (mFocusNode && !focusIsShown(entries)) {
            mFocusNode = nullptr;
            mFocusChannel = graph::Channel();
        }

        const QVector<QPair<core::ObjectNode*, graph::Channel> > basis = basisOf(entries);

        // The basis is the set of (layer, channel) pairs on screen, and that is
        // exactly what a refit is FOR: a selection change, or a channel whose
        // first key appeared, changes the data the window has to contain. Nothing
        // else does - a wheel zoom, a handle drag and a paint all leave this list
        // alone, so the window they were looking at is not re-fitted from under
        // them. Comparing the list (and not the values) is what makes the policy
        // "the fit follows the SHOWN CURVES, the user owns the window".
        if (basis == mValueBasis)
            return;

        mValueBasis = basis;
        applyValueFit(entries);
        // The basis refit already covers whatever focus was pending (the focus
        // is either shown - and fitted - or was pruned just above), so nothing is
        // owed. A sync that early-returns leaves the flag alone, which is what
        // keeps a still-owed focus fit owed until the gesture that raised it
        // ends.
        mFocusFitPending = false;
    }

    void GraphEditor::applyPendingFocusFit() {
        if (!mFocusFitPending)
            return;
        mFocusFitPending = false;
        applyValueFit(buildEntries());
    }

    bool GraphEditor::focusIsShown(const QVector<Entry>& aItems) const {
        for (const Entry& entry : aItems) {
            if (entry.node == mFocusNode && entry.channel == mFocusChannel)
                return true;
        }
        return false;
    }

    QVector<QPair<core::ObjectNode*, graph::Channel> > GraphEditor::basisOf(const QVector<Entry>& aItems) {
        QVector<QPair<core::ObjectNode*, graph::Channel> > basis;
        basis.reserve(aItems.size());
        for (const Entry& entry : aItems) {
            basis.append(QPair<core::ObjectNode*, graph::Channel>(entry.node, entry.channel));
        }
        return basis;
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::applyValueFit(const QVector<Entry>& aItems) {
        // Focus narrowing. When a channel is focused and is one of the shown
        // entries, the window is fitted to THAT channel alone; otherwise (no
        // focus, or a focus the shown set no longer holds) it is the union of
        // every shown curve, as it always was. This is what makes the focus
        // drive the fit: the value axis answers "what am I working on", not
        // "what is on screen".
        bool focusNarrow = false;
        if (mFocusNode) {
            for (const Entry& entry : aItems) {
                if (entry.node == mFocusNode && entry.channel == mFocusChannel) {
                    focusNarrow = true;
                    break;
                }
            }
        }

        bool any = false;
        float lo = 0.0f;
        float hi = 0.0f;

        for (const Entry& entry : aItems) {
            if (focusNarrow && !(entry.node == mFocusNode && entry.channel == mFocusChannel))
                continue; // focused: only the focused channel's values matter

            const graph::Curve& curve = entry.curve;

            const QVector<const core::TimeKey*> keys = curve.keys();
            if (keys.isEmpty())
                continue; // no keys: the curve has no values of its own to fit

            // The key values first, so a channel whose keys never interpolate
            // (one key, or a step-held channel) still bounds the window.
            for (const core::TimeKey* key : keys) {
                const float value = curve.valueOf(*key);
                lo = any ? std::min(lo, value) : value;
                hi = any ? std::max(hi, value) : value;
                any = true;
            }

            // Then the overshoot between them. The samples go through
            // Curve::sample(), so the fit sees the value PLAYBACK reaches inside
            // the segment - the Custom bezier and the Move spline included - and
            // the segment endpoints are samples too (i = 0 and i = n), so a curve
            // is never fitted to a range narrower than the polyline drawn from it.
            for (int s = 0; s < curve.segmentCount(); ++s) {
                const graph::Curve::Segment segment = curve.segment(s);
                const float first = static_cast<float>(segment.from->frame());
                const float last = static_cast<float>(segment.to->frame());
                for (int i = 0; i <= kFitSamplesPerSegment; ++i) {
                    const float frame = first + (last - first) * (static_cast<float>(i) / kFitSamplesPerSegment);
                    const float value = curve.sample(core::Frame::fromDecimal(frame));
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
            }
        }

        // No shown curve has a key: the panel is empty (or every channel shown is
        // keyless), and blanking the axis here would throw away a window the user
        // zoomed. Leaving it is the only harmless answer.
        if (!any)
            return;

        mScale.fitValueWindow(lo, hi);
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::ownsCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor) const {
        // A drag keeps the cursor even when the pointer is nowhere near an item:
        // the button is still down on a handle this editor is moving, and the
        // release that ends it (and every move in between) may land anywhere in
        // the viewport.
        if (mDragging)
            return true;

        // HIT-ONLY ownership. The panel is fullscreen, so a rect test would be
        // true everywhere and the graph would swallow every event the viewport
        // tools need; what the graph may take is exactly the pointer that is on
        // something it can act on - a key or a tangent handle, i.e. a point on a
        // curve. The hit-test runs on a LOCAL copy of the scale pinned to the
        // current panel rect, so this stays const and never mutates the live
        // view.
        graph::Scaler scale = mScale;
        scale.setRect(currentPanelRect(aCamera));

        const QVector<Entry> entries = buildEntries();
        const QPoint point = aCursor.screenPoint();
        const Hit hit = hitTest(entries, scale, point);
        if (hit.hitCurve && 0 <= hit.item && hit.item < entries.size())
            return true;

        // The insert gesture's own hit area. A band point is on no key and no
        // handle, so without this the press that inserts could never reach this
        // class: the Driver routes cursor events by this test and by nothing
        // else, and the panel would show an affordance it cannot act on.
        return curveBandItem(entries, scale, point) >= 0;
    }

    bool GraphEditor::updateCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor) {
        // The Driver calls this before the paint it may trigger, with the camera
        // that paint will use, so syncing here is what keeps the hit-test and the
        // drawing on one geometry.
        mScale.setRect(currentPanelRect(aCamera));
        syncFrameAxis();
        // A channel appearing in (or leaving) buildEntries() is a change of the
        // shown curves, so the window is fitted again here - BEFORE the hit-test
        // below, so the pixels tested and the pixels painted agree. A cursor pass
        // whose curves are the ones the last fit saw (the common case, and every
        // pass after a wheel zoom) leaves the user's window exactly as it is.
        syncValueAxis();

        if (aCursor.emitsLeftPressedEvent()) {
            // A press closes whatever edit was open. BOTH kinds, which is why
            // this is endDrag() and not just mGesture.end(): a gesture still
            // continuing would coalesce the next drag into the previous undo
            // step, and a key-move command left open would be rewritten by the
            // next drag - updateKeyMove asks isModifiable, and it would still be
            // the stack's modifiable command, now aiming at the key this press
            // armed instead.
            endDrag();

            const QVector<Entry> entries = buildEntries();
            const QPoint point = aCursor.screenPoint();
            const Hit hit = hitTest(entries, mScale, point);
            const bool validItem = 0 <= hit.item && hit.item < entries.size();

            // The two branches are exactly "did the hit-test find something":
            // `&& validItem` is the routing decision the whole ownership model
            // rests on, so it must not be relaxed to `||` (a hit with an invalid
            // item would index past entries and grab a handle that is not there).
            if (hit.hitHandle && validItem) {
                // A handle is a point ON a curve, so grabbing it both focuses the
                // channel it belongs to and starts the drag.
                setFocusedChannel(*entries[hit.item].node, entries[hit.item].channel);
                beginDrag(hit, entries);
            } else if (hit.hitCurve && validItem) {
                // Selecting the key IS the whole update: the graph keeps its own
                // selection (the two views can show different layers), and it is
                // held in the timeline's currency so no second type is needed.
                // The same hit also makes this entry the focused channel, which
                // is what the value axis follows.
                selectKey(*entries[hit.item].node, entries[hit.item].channel, hit.keyFrame);
                setFocusedChannel(*entries[hit.item].node, entries[hit.item].channel);
                // ...and ARMS the move: a horizontal drag from this press changes
                // the key's frame. Nothing is committed by the press itself, so a
                // click that only selects leaves the history untouched.
                beginKeyMove(hit, entries, point);
            } else {
                // Nothing under the pointer. The one thing a press can still mean
                // on the panel is the insert gesture: a point on a curve's band
                // (which is why ownsCursor owns it) with no key and no handle
                // there inserts the key the curve already passes through. Off the
                // band this does nothing at all, so a press on empty space leaves
                // the selection and the sticky focus exactly as they were.
                insertKeyAt(aCamera, point);
            }
            // The press is reported as consumed either way (the panel wants its
            // repaint), but ownsCursor is false off the items and the band, so
            // the Driver passes it through and the viewport tools act on it too.
            return true;
        }

        if (aCursor.emitsLeftDraggedEvent()) {
            if (!mDragging)
                return false; // a press that took nothing leaves the drag to nobody

            // The button is still down, so any fit the press's focus owes stays
            // PENDING: the window must not move under the pointer that is holding
            // the gesture open.
            if (mKeyMoveArmed) {
                // A key move answers with whether it moved the key: the caller
                // only has a repaint to make when the document changed, and a
                // refused step (an occupied frame) changed nothing.
                return updateKeyMove(aCursor.screenPoint());
            }

            updateDrag(aCursor.screenPoint());
            return true;
        }

        if (aCursor.emitsLeftReleasedEvent()) {
            if (mDragging) {
                endDrag();
                // The gesture is over and has been interpreted against the
                // geometry the user actually saw, so this is the first moment the
                // fit the press's focus owes may land.
                applyPendingFocusFit();
                return true;
            }
            // A release that closed no drag - a press that armed nothing, an
            // insert or a press on empty space - is still the END of the gesture
            // the press began, so the pending fit lands here for the same reason.
            applyPendingFocusFit();
            return false;
        }

        // A plain move with no button down, or an event this editor takes no
        // action on: outside any gesture, so whatever the focus still owes is
        // paid here.
        applyPendingFocusFit();
        return false;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::updateWheel(
        const core::CameraInfo& aCamera, const QPoint& aPos, int aWheelDelta, bool aZoomValue
    ) {
        // The panel owns the wheel inside its rect and nowhere else. The rect is
        // now the whole viewport (the panel is fullscreen), so the wheel over the
        // animation always zooms the graph while it is up; hiding the graph is
        // how the canvas gets the wheel back. (Ownership stayed rect-based here
        // on purpose: the wheel is not a pointer the tools need to track.)
        const QRect panel = currentPanelRect(aCamera);
        if (!panel.contains(aPos))
            return false;

        mScale.setRect(panel);
        syncFrameAxis();
        // Deliberately NO syncValueAxis() here: the wheel changes no curve, so
        // there is no basis change to refit for - and running the sync would be
        // harmless only because of that. The wheel is the user's own movement of
        // the window, and this path is where it is made.

        if (aZoomValue) {
            // The value axis about the cursor's row, so the number under the
            // cursor stays there.
            mScale.updateValueZoom(static_cast<float>(notchZoomFactor(aWheelDelta)), aPos.y());
        } else {
            // The frame axis delegates its ladder and its anchoring rule to the
            // timeline's scaler.
            mScale.updateFrameZoom(aWheelDelta, aPos.x());
        }
        // Taken even when the axis was already clamped at its floor or ceiling:
        // the pointer is over the panel, so the canvas must not also zoom.
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    QVector<graph::Curve> GraphEditor::curves() const {
        const QVector<Entry> entries = buildEntries();
        QVector<graph::Curve> curves;
        curves.reserve(entries.size());
        for (const Entry& entry : entries) {
            curves.append(entry.curve);
        }
        return curves;
    }

    QVector<GraphEditor::Entry> GraphEditor::buildEntries() const {
        QVector<Entry> items;

        for (core::ObjectNode* node : mTargets) {
            if (!node)
                continue;
            core::TimeLine* timeLine = node->timeLine();
            if (!timeLine) // a folder, or a node kind that holds no keys at all
                continue;

            for (core::TimeKeyType type : graph::Channel::graphableTypes()) {
                // A type whose map is empty has no curve: every sample would come
                // from the default key, i.e. a value no key ever wrote, drawn as
                // a dead flat line through the panel.
                if (timeLine->map(type).isEmpty())
                    continue;

                for (const graph::Channel& channel : graph::Channel::channelsOf(type)) {
                    if (!isChannelVisible(*node, channel))
                        continue;

                    Entry entry{};
                    entry.node = node;
                    entry.line = node; // the layer that owns the key map
                    entry.channel = channel;
                    entry.curve = graph::Curve(*timeLine, channel);
                    items.append(entry);
                }
            }
        }

        return items;
    }

    //-------------------------------------------------------------------------------------------------
    GraphEditor::Hit
    GraphEditor::hitTest(const QVector<Entry>& aItems, const graph::Scaler& aScale, const QPoint& aPos) const {
        Hit hit;
        const QPointF pos(aPos);

        // Handles first, nearest wins. A handle sits within its key's
        // neighbourhood and its drag edits the easing instead of the key, so it
        // is the more specific target of the two.
        double bestHandle = graph::kHandleHitRadius;
        for (int i = 0; i < aItems.size(); ++i) {
            const graph::Curve& curve = aItems[i].curve;
            const int segments = curve.segmentCount();
            for (int s = 0; s < segments; ++s) {
                const graph::Curve::Segment segment = curve.segment(s);
                if (!curve.isCustom(segment))
                    continue; // only a Custom easing exposes tangent handles at all

                for (int h = 0; h < 2; ++h) {
                    const double distance = pixelDistance(graph::handlePoint(aScale, segment, h), pos);
                    if (distance <= bestHandle) {
                        bestHandle = distance;
                        hit.hitCurve = true;
                        hit.hitHandle = true;
                        hit.item = i;
                        hit.segment = s;
                        hit.handle = h;
                    }
                }
            }
        }
        if (hit.hitHandle)
            return hit;

        // Then the keys, nearest wins. The rect comes from graph::keyRect, so a
        // click lands on the key as it is drawn. The comparison is `<=`: a press
        // EXACTLY on the hit radius still hits, which is the boundary the test
        // pins (changing it to `<` would make the outermost ring dead).
        double bestKey = graph::kKeyHitRadius;
        for (int i = 0; i < aItems.size(); ++i) {
            const graph::Curve& curve = aItems[i].curve;
            for (const core::TimeKey* key : curve.keys()) {
                const core::Frame frame(key->frame());
                const QRect rect = graph::keyRect(aScale, frame, curve.valueOf(*key));
                const double distance = pixelDistance(QRectF(rect).center(), pos);
                if (distance <= bestKey) {
                    bestKey = distance;
                    hit.hitCurve = true;
                    hit.hitHandle = false;
                    hit.item = i;
                    hit.segment = -1;
                    hit.handle = -1;
                    hit.keyFrame = frame;
                }
            }
        }

        return hit;
    }

    //-------------------------------------------------------------------------------------------------
    int GraphEditor::curveBandItem(
        const QVector<Entry>& aItems, const graph::Scaler& aScale, const QPoint& aPos
    ) const {
        const QRect plot = aScale.plotRect();
        if (plot.isEmpty() || !plot.contains(aPos))
            return -1; // the label column, the margins and the panel's edges are not data

        // The frame the pointer points at, at the same integral rounding the
        // insert will use: the point must be "at" the curve for the frame the key
        // would land on, so the value tested and the value written are one number.
        const core::Frame frame = aScale.frameAt(aPos.x());
        const double y = static_cast<double>(aPos.y());

        int best = -1;
        double bestDistance = static_cast<double>(kCurveBandRadius);
        for (int i = 0; i < aItems.size(); ++i) {
            const graph::Curve& curve = aItems[i].curve;
            const QVector<const core::TimeKey*> keys = curve.keys();
            if (keys.isEmpty())
                continue;

            // A curve is stroked between its first and last key and no further
            // (buildPolyline is handed that span), so the band stops there
            // too: owning a stretch of curve that is not drawn would be owning an
            // invisible thing, and the panel would take presses on nothing.
            const core::Frame firstFrame(keys.first()->frame());
            const core::Frame lastFrame(keys.last()->frame());
            if (frame < firstFrame || lastFrame < frame)
                continue;

            const double distance = std::fabs(static_cast<double>(aScale.pixelY(curve.sample(frame))) - y);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::insertKeyAt(const core::CameraInfo& aCamera, const QPoint& aPos) {
        if (!mProject)
            return false;

        // The geometry the hit-test and the paint share: the live window and
        // ladder on a LOCAL copy of the scale pinned to this camera's panel rect.
        // A local copy because this is a public entry point - it cannot assume a
        // cursor pass has already written mScale, and it must not write it itself.
        graph::Scaler scale = mScale;
        scale.setRect(currentPanelRect(aCamera));

        const QVector<Entry> entries = buildEntries();

        // A key or a handle at the point is the more specific target: the press
        // that means to grab or to select one must not insert instead, and it
        // must not be REPLACED either (TimeLine::createPusher removes the key it
        // lands on).
        if (hitTest(entries, scale, aPos).hitCurve)
            return false;

        const int item = curveBandItem(entries, scale, aPos);
        if (item < 0)
            return false; // not on any shown curve: nothing to insert into

        const Entry& entry = entries[item];
        core::TimeLine* line = entry.line ? entry.line->timeLine() : nullptr;
        if (!line)
            return false; // a folder, or a node kind that holds no keys

        const core::Frame frame = scale.frameAt(aPos.x());

        // An occupied frame is REFUSED, not replaced: the key already there is the
        // user's, and an insert is not an overwrite (see the header). In practice
        // the key radius catches this press first - a key sits on its own curve,
        // inside the band - so this is the backstop that keeps the rule true even
        // when the pointer is on ANOTHER channel's band.
        if (line->hasTimeKey(entry.channel.type(), frame.get()))
            return false;

        if (!pushSeededKey(*line, *entry.line, entry.channel, frame))
            return false;

        // The inserted key is the panel's current key now: the press landed on its
        // curve, so it also focuses that channel, exactly as a press on a key
        // does. (A real change of focus moves the value window - the focus's own
        // documented contract, not an insert side effect: re-focusing the channel
        // that is already focused is a no-op and refits nothing.)
        selectKey(*entry.node, entry.channel, frame);
        setFocusedChannel(*entry.node, entry.channel);
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::pushSeededKey(
        core::TimeLine& aLine, core::ObjectNode& aTarget, const graph::Channel& aChannel, core::Frame aFrame
    ) {
        const core::TimeKeyType type = aChannel.type();
        const int frame = aFrame.get();

        // The key playback is on at this frame, and where the payload and the
        // easing come from: the last key at or before the frame, else the first
        // key of the map (playback holds the first key before the first key), else
        // the timeline's default key. A shown curve always has at least one key -
        // buildEntries skips a type whose map is empty - so the last fallback is
        // the guard for a caller that reached here some other way.
        core::TimeKey* played = nullptr;
        {
            const core::TimeLine::MapType& map = aLine.map(type);
            auto itr = map.upperBound(frame);
            if (itr != map.begin()) {
                --itr;
                played = itr.value();
            } else if (!map.isEmpty()) {
                played = map.first();
            }
        }
        if (!played)
            played = aLine.defaultKey(type);
        if (!played)
            return false;

        // Cloned, not constructed: everything the graph does not draw - a Move
        // key's centroid, an HSV key's absolute flag, an Image key's resource and
        // blend mode - is the document's own value for this frame instead of an
        // invented default. That is also why the key playback is ON is the right
        // template: its easing is the param playback applies across the very
        // segment this insert splits.
        std::unique_ptr<core::TimeKey> seed(played->createClone());
        if (!seed)
            return false;

        // Each case overwrites the components the graph DRAWS with the value the
        // curve samples at that frame - all of them, so the insert is invisible on
        // every channel of this type, not just the one it was made from - and
        // pushes the key through the timeline's own TimeLineUtil helper, which owns
        // the notifier, the macro and the new-key command (one undo step, the same
        // Type_PushKey event the timeline's add-key buttons emit).
        switch (type) {
        case core::TimeKeyType_Move: {
            auto* key = static_cast<core::MoveKey*>(seed.get());
            key->setPos(QVector2D(
                sampledChannelValue(aLine, type, 0, aFrame), sampledChannelValue(aLine, type, 1, aFrame)
            ));
            TimeLineUtil::pushNewMoveKey(*mProject, aTarget, frame, static_cast<core::MoveKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Rotate: {
            auto* key = static_cast<core::RotateKey*>(seed.get());
            key->setRotate(sampledChannelValue(aLine, type, 0, aFrame));
            TimeLineUtil::pushNewRotateKey(*mProject, aTarget, frame, static_cast<core::RotateKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Scale: {
            auto* key = static_cast<core::ScaleKey*>(seed.get());
            key->setScale(QVector2D(
                sampledChannelValue(aLine, type, 0, aFrame), sampledChannelValue(aLine, type, 1, aFrame)
            ));
            TimeLineUtil::pushNewScaleKey(*mProject, aTarget, frame, static_cast<core::ScaleKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Depth: {
            auto* key = static_cast<core::DepthKey*>(seed.get());
            key->setDepth(sampledChannelValue(aLine, type, 0, aFrame));
            TimeLineUtil::pushNewDepthKey(*mProject, aTarget, frame, static_cast<core::DepthKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Opa: {
            auto* key = static_cast<core::OpaKey*>(seed.get());
            key->setOpacity(sampledChannelValue(aLine, type, 0, aFrame));
            TimeLineUtil::pushNewOpaKey(*mProject, aTarget, frame, static_cast<core::OpaKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Blur: {
            auto* key = static_cast<core::BlurKey*>(seed.get());
            key->setBlurX(sampledChannelValue(aLine, type, 0, aFrame));
            key->setBlurY(sampledChannelValue(aLine, type, 1, aFrame));
            key->setAngleDeg(sampledChannelValue(aLine, type, 2, aFrame));
            TimeLineUtil::pushNewBlurKey(*mProject, aTarget, frame, static_cast<core::BlurKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_HSV: {
            auto* key = static_cast<core::HSVKey*>(seed.get());
            // The stored channel is an int and the graph READS that int, so the
            // sample is truncated exactly as the document stores it: the seeded
            // value is the value the channel reads back. The fourth slot
            // ("absolute") is not a graphable component and keeps the template's.
            QList<int> hsv = key->hsv();
            hsv[0] = static_cast<int>(sampledChannelValue(aLine, type, 0, aFrame));
            hsv[1] = static_cast<int>(sampledChannelValue(aLine, type, 1, aFrame));
            hsv[2] = static_cast<int>(sampledChannelValue(aLine, type, 2, aFrame));
            key->setHSV(hsv);
            TimeLineUtil::pushNewHSVKey(*mProject, aTarget, frame, static_cast<core::HSVKey*>(seed.release()));
            break;
        }
        case core::TimeKeyType_Image: {
            auto* key = static_cast<core::ImageKey*>(seed.get());
            key->setImageOffset(QVector2D(
                sampledChannelValue(aLine, type, 0, aFrame), sampledChannelValue(aLine, type, 1, aFrame)
            ));
            TimeLineUtil::pushNewImageKey(*mProject, aTarget, frame, static_cast<core::ImageKey*>(seed.release()));
            break;
        }
        default:
            // No component of this type is graphable, so no Channel could have
            // named it: the seed is dropped by its scoped pointer and nothing is
            // pushed.
            return false;
        }
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::beginDrag(const Hit& aHit, const QVector<Entry>& aItems) {
        if (!mProject)
            return;
        if (aHit.item < 0 || aHit.item >= aItems.size() || aHit.segment < 0 || aHit.handle < 0)
            return;

        const Entry& entry = aItems[aHit.item];
        if (aHit.segment >= entry.curve.segmentCount())
            return;

        const graph::Curve::Segment segment = entry.curve.segment(aHit.segment);

        // Playback reads the segment's easing from its `from` key, for the whole
        // span, so both of the segment's handles edit that one key's param.
        mDragging = true;
        mKeyMoveArmed = false; // this is a handle drag: the two kinds never coexist
        mDragItem = aHit.item;
        mDragSegment = aHit.segment;
        mDragHandle = aHit.handle;
        mDragKey = segment.from;
        // No command is pushed yet: the first move is what commits, so a press
        // that never moves leaves the history untouched.
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::beginKeyMove(const Hit& aHit, const QVector<Entry>& aItems, const QPoint& aPressPos) {
        if (!mProject)
            return;
        if (aHit.item < 0 || aHit.item >= aItems.size())
            return;

        const Entry& entry = aItems[aHit.item];

        // The key the press hit, in the timeline's own currency - the layer that
        // owns the map, the node the event will name, the key type and the frame
        // the key is at. Held as those four things rather than as aHit.item, so
        // the move follows the key it grabbed and not the position of an entry
        // list that a re-selection could rebuild differently.
        mDragging = true;   // the panel keeps the cursor while the key is held
        mKeyMoveArmed = true;
        mDragItem = aHit.item;
        mDragKey = nullptr; // no handle: this drag moves the key, not its easing
        mDragSegment = -1;
        mDragHandle = -1;
        mMoveLine = entry.line;
        mMoveNode = entry.node;
        mMoveType = entry.channel.type();
        mMoveFrame = aHit.keyFrame;
        // Where the pointer was: the key follows the pointer's frame DELTA, so a
        // press up to the key radius off the key's centre does not make the key
        // jump by that offset on the first move.
        mMovePointerFrame = mScale.frameAt(aPressPos.x());
        // No command yet: the first ACCEPTED move is what commits, so a press that
        // never crosses a frame boundary leaves the history untouched.
        mMoveCommand = nullptr;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::updateKeyMove(const QPoint& aPos) {
        if (!mProject || !mDragging || !mKeyMoveArmed)
            return false;

        core::TimeLine* line = mMoveLine ? mMoveLine->timeLine() : nullptr;
        if (!line || !mMoveNode) {
            endDrag();
            return false;
        }

        // The anchor check, the handle drag's own honesty: if the armed key is no
        // longer in the document (an undo removed it, or another view did), this
        // move would write a key that is not there. MoveFrameOfKey's own
        // validation would refuse every step, leaving the drag silently dead under
        // the cursor - so close the edit and say why.
        if (!line->hasTimeKey(mMoveType, mMoveFrame.get())) {
            mLogger.pushLog(
                QObject::tr("The animation graph stopped moving: the dragged key changed."), UILogType_Info
            );
            endDrag();
            return false;
        }

        // Only x is read, so a vertical drag moves nothing by construction: a move
        // is a FRAME edit, and a key VALUE edit is a design of its own.
        const core::Frame pointerFrame = mScale.frameAt(aPos.x());
        const int step = pointerFrame.get() - mMovePointerFrame.get();
        if (step == 0)
            return false; // no frame boundary crossed: nothing to rewrite, nothing to open

        // The frame range is playback's business, not the keying system's: keys
        // may live anywhere, so the move is asked with the widest range there is
        // (the timeline's own move asks the same way).
        const util::Range frameRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());

        if (mMoveCommand && mProject->commandStack().isModifiable(mMoveCommand)) {
            // The command this gesture already pushed: REWRITE it, which is what
            // makes the whole drag one undo step. modifyMove refuses a destination
            // another key of the type owns, and a refusal leaves the command
            // exactly as it was (no move, no mMove, no notification).
            core::TimeLineEvent modEvent;
            int clampedStep = step;
            if (!mMoveCommand->modifyMove(modEvent, step, frameRange, &clampedStep) || clampedStep == 0)
                return false;

            mMoveFrame = core::Frame(mMoveFrame.get() + clampedStep);
            mMovePointerFrame = core::Frame(mMovePointerFrame.get() + clampedStep);

            // The rewrite goes straight through the stack, so the notification the
            // first commit pushed has to be sent again here - the same event the
            // timeline emits for keys whose frames moved.
            mProject->onTimeLineModified(modEvent, false);
            return true;
        }

        // The gesture's first accepted move, or its first after an undo dropped
        // the stack's modifiable link.
        //
        // The command is built and VALIDATED before the macro is opened: a
        // not-yet-pushed MoveFrameOfKey only records where it would move to (its
        // own mCurrent == 0 path), so a REFUSED move can be dropped right here -
        // no command on the stack, no notification, no empty history entry.
        core::TimeLineEvent source;
        source.setType(core::TimeLineEvent::Type_MoveKey);
        source.pushTarget(*mMoveNode, mMoveType, mMoveFrame.get());

        std::unique_ptr<TimeLineUtil::MoveFrameOfKey> command(new TimeLineUtil::MoveFrameOfKey(source));
        core::TimeLineEvent modEvent;
        int clampedStep = step;
        if (!command->modifyMove(modEvent, step, frameRange, &clampedStep) || clampedStep == 0)
            return false;

        TimeLineUtil::MoveFrameOfKey* pushed = command.release();
        {
            cmnd::ScopedMacro macro(mProject->commandStack(), CmndName::tr("move a key"));

            // The notifier carries the move's ORIGINAL target, exactly like the
            // timeline's beginMoveKeys: what reaches the application names where
            // the key came from, and the macro's close is what fires it for this
            // first commit (the later rewrites notify by hand, above).
            auto* notifier = new TimeLineUtil::Notifier(*mProject);
            notifier->event() = source;
            macro.grabListener(notifier);

            mProject->commandStack().push(pushed);
        }

        mMoveCommand = pushed;
        mMoveFrame = core::Frame(mMoveFrame.get() + clampedStep);
        mMovePointerFrame = core::Frame(mMovePointerFrame.get() + clampedStep);
        return true;
    }

    void GraphEditor::updateDrag(const QPoint& aPos) {
        if (!mProject || !mDragging || !mDragKey)
            return;

        // The entries are rebuilt from the document on every use, so the drag
        // follows the keys the document holds now instead of a press-time
        // snapshot.
        const QVector<Entry> entries = buildEntries();
        if (mDragItem < 0 || mDragItem >= entries.size() || mDragSegment < 0)
            return;

        const Entry& entry = entries[mDragItem];
        if (mDragSegment >= entry.curve.segmentCount())
            return;

        const graph::Curve::Segment segment = entry.curve.segment(mDragSegment);

        // The anchor check: if the keys moved under the drag (another view, an
        // undo), the segment at this index is no longer the anchor's and the
        // write would land on a different key's easing.
        if (segment.from != mDragKey) {
            // The key this drag grabbed is no longer the segment's start - an
            // undo mid-drag removed or moved it - so this move would write
            // ANOTHER key's easing. Close the edit and say why: silently
            // ignoring the moves would leave the handle visibly stuck under the
            // cursor with nothing to explain it.
            mLogger.pushLog(
                QObject::tr("The animation graph stopped editing: the dragged key changed."), UILogType_Info
            );
            endDrag();
            return;
        }

        core::TimeLine* line = entry.line ? entry.line->timeLine() : nullptr;
        core::TimeKey* key = line ? line->timeKey(entry.channel.type(), mDragKey->frame()) : nullptr;
        util::Easing::Param* easing = key ? graph::Curve::easingOf(*key, entry.channel.type()) : nullptr;
        if (!easing)
            return; // the type carries no easing: nothing this graph could be editing

        const QPointF normalized = handleNormalized(
            mScale, segment, aPos, graph::Curve::handle(segment, mDragHandle).y()
        );

        // Built from the param the document holds NOW, never from a press-time
        // copy, so the fields this drag does not touch - the easing type above
        // all - cannot be reverted by a stale snapshot.
        util::Easing::Param next = *easing;
        graph::Curve::setHandle(next, mDragHandle, normalized);

        if (mGesture.continues()) {
            // Rewrite the command this drag already pushed, so the whole drag
            // stays ONE undo step.
            mGesture.command()->modifyValue(next);

            // The rewrite goes straight through the stack, so the notification
            // the first commit pushed has to be sent again here - the same event
            // the timeline emits for a key whose value changed.
            core::TimeLineEvent event;
            event.setType(core::TimeLineEvent::Type_ChangeKeyValue);
            event.pushTarget(*entry.node, entry.channel.type(), mDragKey->frame());
            mProject->onTimeLineModified(event, false);
        } else {
            // The drag's first commit (or its first after an undo/redo dropped
            // the stack's modifiable link): the notifier and the assign are
            // pushed inside ONE macro, so one notification and one command are
            // one commit.
            mGesture.begin(CmndName::tr("change a key easing"), [&](cmnd::ScopedMacro& aMacro) {
                auto* notifier = new TimeLineUtil::Notifier(*mProject);
                notifier->event().setType(core::TimeLineEvent::Type_ChangeKeyValue);
                notifier->event().pushTarget(*entry.node, entry.channel.type(), mDragKey->frame());
                aMacro.grabListener(notifier);

                return new cmnd::ModifiableAssign<util::Easing::Param>(easing, next);
            });
        }
    }

    void GraphEditor::endDrag() {
        // The command is already on the stack with its last value, so closing the
        // edit is all a release has to do: Gesture::end() makes the next drag open
        // a fresh command instead of coalescing into this one, and a gesture that
        // never committed pushes nothing at all.
        //
        // NOTE: this does NOT pay a pending focus fit. The press branch runs it
        // before it has interpreted the press, and the whole point of the pending
        // flag is that the window must not move under a press or a gesture; the
        // callers that ARE a gesture's end (the release handling, a hide) call
        // applyPendingFocusFit() themselves, after the event has been read.
        mGesture.end();
        mDragging = false;
        mKeyMoveArmed = false;
        mDragKey = nullptr;
        mDragItem = -1;
        mDragSegment = -1;
        mDragHandle = -1;

        // The key move's command stays on the stack (it IS the gesture's one undo
        // step, whatever it was last asked to do); this drops the handle to it, so
        // no later press can rewrite a command that belongs to a finished
        // gesture. The armed key goes with it: nothing may move until a new press
        // arms one.
        mMoveCommand = nullptr;
        mMoveLine = nullptr;
        mMoveNode = nullptr;
        mMoveType = core::TimeKeyType_TERM;
        mMoveFrame = core::Frame();
        mMovePointerFrame = core::Frame();
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::setChannelVisible(core::ObjectNode& aTarget, const graph::Channel& aChannel, bool aVisible) {
        // Only the HIDDEN channels are stored: a channel is visible unless it was
        // hidden, so an untouched document needs no entries at all. Identity is
        // (node, channel) - the node pointer the caller owns, and the channel's
        // type/component - so a hidden channel survives its target leaving and
        // rejoining the selection.
        const QPair<core::ObjectNode*, graph::Channel> key(&aTarget, aChannel);
        const auto itr = std::find(mHiddenChannels.begin(), mHiddenChannels.end(), key);

        if (aVisible) {
            if (itr != mHiddenChannels.end()) {
                mHiddenChannels.erase(itr);
            }
        } else if (itr == mHiddenChannels.end()) {
            mHiddenChannels.append(key);
        }
    }

    bool GraphEditor::isChannelVisible(const core::ObjectNode& aTarget, const graph::Channel& aChannel) const {
        for (const QPair<core::ObjectNode*, graph::Channel>& hidden : mHiddenChannels) {
            // Pointer identity only. A deleted target can leave an entry nothing
            // can reach, which is never dereferenced; setProject clears the set.
            if (hidden.first == &aTarget && hidden.second == aChannel) {
                return false;
            }
        }
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::retrieveSelection(core::TimeLineEvent& aOut) const {
        // The graph's selection IS a TimeLineEvent, so it is handed out in the
        // timeline's currency rather than translated into a second type. The type
        // stays as it was set (Type_TERM when the selection is the graph's own):
        // picking a key is not a document change, and inventing one of the change
        // types here would make a consumer act on a value that did not move.
        for (const core::TimeLineEvent::Target& target : mSelection.targets()) {
            aOut.pushTarget(*target.node, target.pos);
        }
        return !mSelection.targets().isEmpty();
    }

    void GraphEditor::clearSelection() {
        mSelection = core::TimeLineEvent();
    }

    void GraphEditor::selectKey(core::ObjectNode& aNode, const graph::Channel& aChannel, core::Frame aFrame) {
        // One key, one target: the graph selects a key, not a range, and it is
        // addressed by the same (line, type, frame) triple the timeline's own keys
        // are.
        mSelection = core::TimeLineEvent();
        mSelection.pushTarget(aNode, aChannel.type(), aFrame.get());
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::removeSelectedKey() {
        if (!mProject)
            return false;

        // The panel's current key is its selection, which is one target by
        // construction (selectKey). Anything else - no selection, or a hand-made
        // one with several targets - is not a key the panel can name, so nothing
        // happens rather than something approximate.
        if (mSelection.targets().size() != 1)
            return false;
        // The panel's current key, as a mutable target: the remover factory needs
        // the owning TimeLine to build its command against, and TimeKeyPos hands
        // out a non-const line only from a non-const pos.
        core::TimeLineEvent::Target& target = mSelection.targets().first();
        if (!target.node)
            return false;

        core::TimeLine* line = target.pos.line();
        if (!line)
            return false;

        // The key must be there to be removed. This is the same guard the ADD
        // side needs for the opposite reason: TimeLine::createRemover with
        // nothing at the frame builds an EMPTY command, and an empty command on
        // the stack is an undo step that undoes nothing.
        if (!line->hasTimeKey(target.pos.type(), target.pos.index())) {
            // A selection naming a frame that holds no key is stale (an undo, or
            // another view's edit) - drop it rather than keep naming a key that
            // is not in the document.
            clearSelection();
            return false;
        }

        {
            // The timeline's own delete route (TimeLineEditor::deleteCheckedKeys):
            // one notifier carrying the Type_RemoveKey event, one
            // ScopedMacro("Delete keys")-shaped macro, and the remover factory
            // that owns the map removal.
            cmnd::ScopedMacro macro(mProject->commandStack(), CmndName::tr("remove a key"));

            auto* notifier = new TimeLineUtil::Notifier(*mProject);
            notifier->event().setType(core::TimeLineEvent::Type_RemoveKey);
            notifier->event().pushTarget(*target.node, target.pos.type(), target.pos.index());
            macro.grabListener(notifier);

            mProject->commandStack().push(
                line->createRemover(target.pos.type(), target.pos.index(), true)
            );
        }

        // The key is gone, so the panel's current key goes with it: keeping the
        // selection would name a key no longer in the document (and the next
        // remove would refuse it - the guard above - rather than remove a
        // neighbour by accident).
        clearSelection();
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    bool GraphEditor::setSelectedKeysEasing(util::Easing::Type aType) {
        if (!mProject)
            return false;

        // WHAT CAN BE WRITTEN, collected BEFORE the macro is opened. The header
        // says why this has to happen first: cmnd::Stack appends a macro even
        // when it holds no command, and counts it as an edit, so an action that
        // opened the macro and then found nothing to write would leave an empty
        // undo step behind - the defect removeSelectedKey's own guard avoids.
        struct Edit {
            util::Easing::Param* easing;
            core::ObjectNode* node;
            core::TimeKeyType type;
            int frame;
        };
        QVector<Edit> edits;

        for (core::TimeLineEvent::Target& target : mSelection.targets()) {
            // The mutable target: the action writes the keys through the map the
            // TimeLinePos names, and it is TimeKeyPos::line()'s non-const overload
            // that hands out a writable map.
            core::TimeLine* line = target.pos.line();
            if (!line || !target.node)
                continue; // a hand-made target with no owner: nothing to address

            // The key must BE there. A selection can outlive the key it names (an
            // undo, or another view's edit), and a stale one must not push a
            // command against a frame that holds nothing.
            core::TimeKey* key = line->timeKey(target.pos.type(), target.pos.index());
            if (!key)
                continue;

            // The type has to carry an easing at all: bone, pose, mesh and FFD
            // keys are structural and easingOf answers null for them, which is
            // also why the panel has no channel for them.
            util::Easing::Param* easing = graph::Curve::easingOf(*key, target.pos.type());
            if (!easing)
                continue;

            // ...and playback has to CONSULT that easing. The image offset is the
            // one graphable value that is step-held (blendImageKey holds the last
            // key; graph::Channel::isInterpolated is the rule), so its param
            // changes no drawn sample and no played value: refusing it is the
            // timeline's own behaviour with Image, not a special case invented
            // here.
            if (!graph::Channel(target.pos.type(), 0).isInterpolated())
                continue;

            // Already at the requested type: writing it would push a command whose
            // exec and undo are the same value - a history entry for a change that
            // did not happen. So a menu entry that matches the key's current easing
            // leaves the document and the history exactly as they are.
            if (easing->type == aType)
                continue;

            edits.append(Edit{easing, target.node, target.pos.type(), key->frame()});
        }

        if (edits.isEmpty())
            return false;

        {
            // ONE macro and ONE notifier for the whole selection - the timeline's
            // own route (TimeLineUtil::assignKeyEasing: a cmnd::Assign of the key's
            // util::Easing::Param, a TimeLineUtil::Notifier, and a
            // core::TimeLineEvent stating Type_ChangeKeyValue), batched so the
            // selection is ONE undo step. The per-key helper cannot be reused for
            // that: it opens a macro of its own, and cmnd::Stack::beginMacro
            // asserts on a nested one.
            cmnd::ScopedMacro macro(mProject->commandStack(), CmndName::tr("change key easing"));

            auto* notifier = new TimeLineUtil::Notifier(*mProject);
            notifier->event().setType(core::TimeLineEvent::Type_ChangeKeyValue);
            macro.grabListener(notifier);

            util::Easing::Param next;
            for (const Edit& edit : edits) {
                // The key's OWN param with its type replaced: the range, the
                // weight and the Custom bezier's two controls belong to the key,
                // and the action only chooses the type.
                next = *edit.easing;
                next.type = aType;

                // The frame the key is at NOW rather than the one the selection
                // names: they are the same for a live key (the lookup above went
                // through the same frame), and the key's own is the one the
                // document answers with.
                notifier->event().pushTarget(*edit.node, edit.type, edit.frame);
                mProject->commandStack().push(new cmnd::Assign<util::Easing::Param>(edit.easing, next));
            }
        }
        return true;
    }

    //-------------------------------------------------------------------------------------------------
    void GraphEditor::setFocusedChannel(core::ObjectNode& aNode, const graph::Channel& aChannel) {
        // Re-focusing the channel that is already focused is a NO-OP: the value
        // axis is the user's, and grabbing a handle on the focused channel must
        // not snap a window the user zoomed back onto the data. Only a real change
        // of focus refits.
        if (mFocusNode == &aNode && mFocusChannel == aChannel)
            return;

        mFocusNode = &aNode;
        mFocusChannel = aChannel;

        // The focus drives the fit, but the fit is PENDING (see the policy note in
        // the header): this runs from a PRESS, and a press may go on to arm a
        // handle drag or a key move. Refitting here would move the very key or
        // handle the press just grabbed, and the gesture would then run against
        // geometry the fit had already invalidated. applyPendingFocusFit() pays
        // this once the gesture is over. The shown-curve basis is untouched, so a
        // later cursor pass with the same shown curves cannot refit over it.
        mFocusFitPending = true;
    }

    void GraphEditor::clearFocus() {
        if (!mFocusNode)
            return; // already clear: nothing to drop and nothing to refit

        mFocusNode = nullptr;
        mFocusChannel = graph::Channel();

        // No focus means the union of the shown curves again (or, when nothing
        // shown has a key, the window the user set - applyValueFit's own guard).
        // Pending for the same reason setFocusedChannel's fit is: clearFocus can
        // be reached from a press's own path, and the window must not move under
        // that press.
        mFocusFitPending = true;
    }

    //-------------------------------------------------------------------------------------------------
    core::Frame GraphEditor::currentFrame() const {
        // The playhead's frame, for views that draw it. A missing project has no
        // frame; -1 is outside the real range (frames start at 1), so it reads as
        // "nothing" without inventing a second type.
        return mProject ? mProject->currentTimeInfo().frame : core::Frame(-1);
    }

} // namespace ctrl
