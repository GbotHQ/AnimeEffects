#ifndef GUI_GRAPHPANELMODEL_H
#define GUI_GRAPHPANELMODEL_H

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>
#include "ctrl/graph/graph_Channel.h"
#include "util/Range.h"

namespace core {
class ObjectNode;
}
namespace ctrl {
class GraphEditor;
}
namespace theme {
class Graph;
}

namespace gui {

// The animation graph panel as an engine-neutral MODEL: everything the overlay
// has to know to draw the panel, and nothing about how it is drawn.
//
// The panel was once a QPainter renderer (ctrl::graph::Renderer) and then a Dear
// ImGui spike (src/gui/ImGuiSpike.cpp, buildDocumentScene); both are deleted.
// This header is the durable form that replaced them: a pure function from the
// editor, a viewport size and a palette to a flat, POD-ish description of the
// panel. The drawing layer - gui::drawPanel, in the shipping ImGui overlay -
// walks the vectors below and emits primitives; it owns every appearance
// decision the model deliberately does not make (line widths, halos, fonts,
// alpha, emphasis).
//
// WHY IT EXISTS
//   * the panel's content is testable headlessly: no QPainter, no GL context, no
//     widget - tests/graph_panel_model.cpp asserts the geometry below against
//     the editor's own scaler;
//   * the drawing layer and the editor's hit-test read one geometry: the model
//     publishes what gui::drawPanel draws from the same functions the editor
//     picks with (ctrl/graph/graph_Geometry.h), so the drawn item and the
//     clickable item are one;
//   * the document is read, never written. See READ-ONLY below.
//
// WHAT IT IS NOT
//   * no background, no grid lines, no frame/value rules. The two axes are SHORT
//     tick marks only (see the axes below), so the viewport's animation stays
//     readable through the whole panel;
//   * no styled text: labels are RECTS. The halo/outline pass, the font, the
//     colour and the alignment are the drawing layer's;
//   * no interaction state. Hit-testing, the drag, the selection and the sticky
//     focus all live in the editor and in the drawing layer. Each PanelCurve
//     carries its (node, channel) identity so the drawing layer can emphasise
//     exactly one curve - the model never learns which one that is;
//   * no project mutation: the editor, its scaler, its selection and the
//     document come out of buildGraphPanelModel() exactly as they went in.

// The panel's empty-state strings, translatable.
//
// The model is not a QObject, so a plain tr() here would be resolved through a
// context this class does not own: lupdate would file the string under the class
// while Qt asked for it somewhere else, and the message would ship English in
// every language with nothing failing. The repository's precedent for a string
// that lives outside a QObject is an EXPLICIT context
// (src/ctrl/ExportTargetStrings.h): the literal carries
// QT_TRANSLATE_NOOP("GraphPanel", ...) so the extractor sees it, and this asks
// the SAME context at runtime. Both halves of that pair are written in one
// expression at each call site, so they cannot drift - which is what
// scripts/i18n/check_tr_contexts.py exists to police.
QString graphPanelTr(const char* aSourceText);
// The PLURAL half of the same pair: a string carrying %n, which Qt resolves
// through the plural form of the same context ("3 keys", "1 key"). aN is the
// count. Only the strings the panel writes as a COUNT use it - the channel
// button's label and the selection label - and they are marked with
// QT_TRANSLATE_N_NOOP("GraphPanel", ...) at their call sites, the extractor half
// of the same pairing.
QString graphPanelTr(const char* aSourceText, int aN);

// One row of the CHANNEL LIST: a channel of one target, as the panel's one
// control (the "N channels" button, see CHANNELS BUTTON) lists it. A row exists
// for every (target, channel) that HAS KEYS, including a channel hidden through
// GraphEditor::setChannelVisible - a hidden channel keeps its row (visible ==
// false) and contributes no curve, so the user can see what is hidden and turn
// it back on.
//
// NO GEOMETRY IS PUBLISHED FOR A ROW, and that is the design: the list is the
// application's own QMenu (gui::MainDisplayWidget::showGraphChannelMenu), a
// native window this drawing layer neither draws nor measures - the same rule
// the key menu already follows. What a consumer gets here is the row's DATA: the
// layer it is grouped under, its name, its hue and its two states. A row is
// NEVER dropped for want of room: the menu scrolls, so every keyed channel is
// listed whatever the panel's size.
struct PanelChannelRow {
    // The target the row acts on. NON-CONST on purpose: the two actions a row
    // performs go through the editor, and both of those take a mutable node
    // (GraphEditor::setChannelVisible and setFocusedChannel). The targets the builder
    // walks carry non-const pointers (GraphEditor::targets() is a const method whose
    // ELEMENTS are ObjectNode*), so filling this needs no cast - only the sticky focus
    // it is compared against is const (focusedNode()), and that is a comparison.
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    QString layer;      // the target's own name: the heading the row is grouped under
    QString name;       // the channel's label for a list row or a chip ("Move X"), or
                        // the bare type name for a one-component type ("Opa") - see CHIPS
    QColor color;       // theme::Graph::keyColor(channel.type()), theme::Graph::curve() when unnamed
    bool visible = true;  // == GraphEditor::isChannelVisible
    bool focused = false; // == the editor's sticky focus (focusedNode()/focusedChannel())
};

// One tangent handle of one segment, ready to stroke and to dot. A pair is
// published per segment that HAS handles: index 0 leaves the segment's `from`
// key, 1 arrives at its `to` key. Two kinds of pair exist and they are NOT
// interchangeable (see PanelHandle::ghost):
//   * a Custom segment's STORED pair (Curve::handle), the one a drag writes; and
//   * an Auto segment's DERIVED pair (Curve::ghostHandles), which the document
//     does not hold - it is recomputed from the key's neighbours, which is why
//     it cannot be stored - so the drawing layer can show that the segment's
//     tangent is implicit and a drag would convert it.
// Either way the drawing layer never has to know the easing model: only where to
// put a stem and a dot.
struct PanelHandle {
    int segment = -1; // index into Curve::segmentCount()
    int index = 0;    // 0 = leaving `from`, 1 = arriving at `to`
    QPointF base;     // where the stem starts (the key the handle belongs to)
    QPointF dot;      // where the user grabs (mapped through the scaler)
    int radius = 0;   // the dot's drawn radius
    // TRUE for an Auto segment's DERIVED pair and false for a Custom one's
    // STORED pair. It is the whole of what distinguishes them: a ghost handle is
    // not in the document (dragging one converts the key to Custom), so the
    // drawing layer draws it hollow - a stored handle is filled - and a consumer
    // that grabbed a handle knows which kind of write it would start.
    bool ghost = false;
};

// One channel's curve, ready to stroke and to dot with keys.
struct PanelCurve {
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    // The project's rule for the frames outside the keys (ctrl::GraphEditor::isLooping/frameRange):
    // the model rebuilds its own Curve from (node, channel), so the rule has to travel with them or
    // the drawn polyline would hold where playback wraps.
    bool loop = false;
    util::Range loopRange;
    QVector<QPointF> points; // (frame, value) mapped to panel pixels, in frame order
    QVector<QRect> keys;     // the key HIT squares, each clipped to the plot
    // The VISUAL of each key in `keys`: the centre of the marker its drawing
    // layer paints - a circle of radius ctrl::graph::kKeyDotRadius (see the
    // VISUAL/HIT split in ctrl/graph/graph_Geometry.h). Same order and same
    // length as `keys` and `keyFrames`, and published rather than recomputed
    // from the rect because the rect is CUT at the plot's edge: its centre is
    // not the key's centre for a key on the border, so a marker reconstructed
    // from the rect would sit off its key. A key that publishes no rect (one
    // entirely outside the plot) publishes no dot either.
    QVector<QPointF> keyDots;
    // The FRAME of each rect in `keys`, same order and same length: a rect is
    // where a key is HIT, which is all a hit-test needs, but an action needs to
    // NAME the key - and the triple (node, channel, frame) is the timeline's own
    // name for one. Parallel to `keys` (a) rather than a member of it because the
    // rects are the editor's shared geometry (ctrl::graph::keyRect) and a pair
    // would be a second copy of the same rule, and (b) because a key whose rect
    // comes out empty is DROPPED from `keys`: an index into `keys` is therefore
    // not an index into the curve's own key list, which is exactly the desync a
    // consumer must not be trusted to reconstruct for itself.
    QVector<core::Frame> keyFrames;
    QVector<PanelHandle> handles; // every segment's tangent handles, in segment order
    QColor color;            // == channels[channelRow].color; the drawing layer paints
                             // this curve's stroke, its key markers' FILL, its chip's
                             // text and its handle stems in it, so a channel is one
                             // colour everywhere
    int channelRow = -1;     // the row of `channels` this curve belongs to
    // The CHIP that names this curve IN PLACE: the box the name sits in, at the
    // plot's right edge, on the value the curve leaves the window with (see
    // CHIPS), and the text that goes in it. EMPTY when the chip would not fit
    // inside the plot - the drawing layer draws nothing for it and hit-tests
    // nothing derived from it.
    QRect chip;
    QString chipText;
};

// One piece of text and the box it goes in. The box is what the model decides;
// the text itself may be drawn centred in it (frame numbers) or right-aligned
// (value numbers) by the drawing layer.
struct PanelLabel {
    QRect rect;
    QString text;
};

// The panel's complete description for one frame.
//
// GEOMETRY
//   panel   == ctrl::GraphEditor::panelRect(aViewport). The panel covers the
//              whole viewport (it is the fullscreen graph view over the
//              animation), and this model never hardcodes a size: whatever that
//              function returns is the panel.
//   plot    == the editor's own scaler, given `panel`, i.e.
//              ctrl::graph::Scaler::plotRect() = panel minus kMargin on all
//              sides and minus the MEASURED label column on the left
//              (Scaler::axisLabelWidth(); its floor is kAxisLabelWidth). Empty
//              on a viewport too small for that inset; see emptyState.
//   valueColumn == the left column the value numbers sit in, the deleted painter
//              renderer's layout, kept as the panel's convention:
//              QRect(panel.left() + kMargin, plot.top(), scale.axisLabelWidth(),
//              plot.height()). Its width is the scaler's STABLE reserve for the
//              value format - the widest label that format can write, measured
//              once per font, NOT the widest label this window shows (see THE
//              LABEL COLUMN in ctrl/graph/graph_Scaler.h: the column insets the
//              plot, so a window-dependent width would move the plot's left edge
//              - and every frame coordinate - whenever a gesture changed the
//              numbers). It is the same width the plot was inset by, so the
//              column and the plot are one geometry. It never overlaps the plot.
//
// THE OCCLUDED TOP (rulerBandTop)
//   A widget may float ABOVE the viewport and cover the panel's first rows - the
//   project tab bar does, in the application's own layout - and the panel draws no
//   background, so anything under it is simply lost. buildGraphPanelModel therefore
//   takes the occluded height (aTopInset, logical pixels, 0 when nothing covers the
//   panel) and publishes rulerBandTop: the y where the panel's TOP CONTENT may
//   begin, i.e. plot.top() normally and the occluded line when the inset reaches
//   past it (clamped to the plot's bottom, so a viewport whose whole top is covered
//   simply has no room for a ruler band).
//   WHAT MOVES AND WHAT DOES NOT. The RULER BAND moves: the tick marks are drawn
//   down from rulerBandTop (not from plot.top()) and every frame label's box starts
//   below it, and a value label whose own row would fall under the occluded band is
//   nudged down to it - all of that is non-interactive ink, and it is the ink the
//   user cannot see otherwise. The PLOT does not move: it is ctrl::GraphEditor's own
//   rect (the editor's hit-test geometry, and the rect the editor's scaler is pinned
//   to), so `plot`, the curves, the keys, the handles, the playhead and the marquee
//   stay exactly where the editor acts on them. Moving them here would put the drawn
//   key a tab bar's height away from the key a press grabs.
//
// THE AXES (no grid, no rules)
//   frameTickX: the x of every frame the ladder classifies (time::Scaler's
//               TickLevel != None) inside the plot, ascending. The drawing layer
//               draws a SHORT mark down from rulerBandTop at each - 4px is the
//               convention (kFrameTickLength) - and nothing else.
//   frameTickLevel: the tier of each of those ticks, one entry per frameTickX
//               entry in the same order, so index i is tick i's tier. It is the
//               ladder's own classification (ctrl::time::Scaler::TickLevel, carried
//               as its integer value; None never appears, because such a frame is
//               not a tick and frameTickX has no entry for it) and NOT a
//               measurement: the tier is what the ladder that decided the tick
//               said about it, and recovering it in the drawing layer from tick
//               spacing or label widths is the pixel measurement the timeline's own
//               ruler comment rejects. It is a CLASSIFICATION and not appearance -
//               the model names no tier's alpha or length, exactly as it publishes
//               a position and not a span, and the drawing layer decides how a tier
//               looks (the timeline's own table, 0.85 major / 0.60 intermediate /
//               0.20 minor, one length for every level). The two vectors are
//               appended in one step, so they are the same length by construction.
//   valueTickY: the y of the value axis' own ticks, one per window value (max,
//               midpoint, min), in that order. A SHORT mark rightwards from
//               plot.left() at each. A value whose NUMBER the column could not
//               fit still gets its tick - the axis has a tick there whatever can
//               be written beside it, the frame axis' own rule - so a tick can
//               outnumber the valueLabels below (only past the column's ceiling,
//               unreachable at the UI font; see graph_Scaler.h).
//   The model publishes POSITIONS (ints) and CLASSIFICATIONS (ints), never
//   spans and never ink: a full-height or full-width line cannot be expressed in
//   this structure at all, which is the mechanical form of "no grid", and a
//   tick's tier is an index into the drawing layer's own appearance table rather
//   than a colour or a length.
//
// LABELS (rects only)
//   frameLabels: the frame NUMBER of a Major tick, in a box centred on that tick
//               and always inside the plot (a label near an edge is nudged in).
//               A label is dropped when it would come closer than kFrameLabelGap
//               (8px) to the previous label's right edge - a rule stated on the
//               boxes, not on their centres, so it holds for any font.
//               THE BOX'S TOP EDGE IS THE ROOM A NUMBER HAS: it sits at
//               plot.top() + kFrameTickLength + 1, i.e. under the tick band, and a
//               drawing layer must never put a glyph ABOVE its box's top edge - a
//               font taller than the box is clamped into it (drawLabel in
//               src/gui/ImGuiOverlay.cpp). That pair is what keeps the top row of
//               numbers off the plot's top edge and out of the plot clip; the model
//               measures the box with the default UI font and cannot know the
//               drawing layer's, so the obligation is split rather than assumed.
//   valueLabels: the window's max / midpoint / min, formatted 'g' with four
//               significant digits (the deleted painter renderer's valueText(),
//               now ctrl::graph::Scaler::valueAxisText - the SAME function the
//               column's width is measured from; two equal strings are
//               deduplicated). The box is the value column,
//               ctrl::graph::Scaler::kAxisLabelPad (4px) narrower on the right,
//               vertically centred on the value's own pixel and clamped into the
//               column - the drawing layer right-aligns the text in it.
//               THE COLUMN IS SIZED FOR THE WHOLE FORMAT, not for this window's
//               three strings: its width is the scaler's stable reserve (the
//               widest label 'g'/4 can write, plus the box's inset and a small
//               gap - Scaler::axisLabelWidth()), so every published box is at
//               least as wide as the text it carries, no number can spill past
//               the panel's own left margin, and NO gesture can move the column
//               (or the plot's left edge) by changing which label is widest.
//               That last part is why the reserve is format-derived: a
//               window-measured column moved the plot by a pixel on a 40px pan
//               (measured). A number that still would not fit (only past the
//               column's ceiling, which the format's widest label cannot reach
//               at the UI font) is DROPPED, not published overflowing, exactly as
//               the frame axis drops a number wider than the plot; its
//               valueTickY entry is still published.
//   The boxes are measured with the default UI font (QFontMetrics(QFont())):
//   the model has no painter, and the rects are advisory geometry, not a text
//   layout. A drawing layer with its own font puts the glyphs in the box it was
//   handed. The column is measured with that same font (it is the scaler that
//   measures it), so the split obligation is still "put the glyphs in the box",
//   not "make them fit".
//
// CURVES
//   One PanelCurve per VISIBLE channel of every target that has keys - the same
//   set GraphEditor::buildEntries() draws, rebuilt here from the public editor
//   surface (targets(), isChannelVisible(), the node's timeline) because a Curve
//   only names its timeline, not its node, and the drawing layer needs the
//   (node, channel) identity.
//   points: Curve::buildPolyline over the plot's own frame window, sampled at
//   qBound(2, plot.width() / (2 * segmentCount), 256) samples per segment - one
//   point per ~2px of plot width, clamped - then mapped through
//   Scaler::pixelXOfFrame / Scaler::pixelY. A curve whose keys all sit outside
//   the window has its identity and colour but no geometry.
//   THE POLYLINE IS NOT CLIPPED TO THE PLOT, and the model does not clip it:
//   Curve::buildPolyline emits every touched segment's two endpoints even when
//   they lie outside the sampled window - deliberately, so a polyline cut to the
//   window's interior would not fall a step short of the plot's edge - and the
//   model maps those samples through the scaler unchanged. A published point may
//   therefore lie outside `plot`: in x when a segment's endpoint frame is outside
//   the window, and in y whenever the value window is narrower than the curve's
//   own values (the user's zoom). CLIPPING IS THE CONSUMER'S OBLIGATION: a
//   drawing layer MUST clip the polyline to `plot` before stroking it (the
//   shipping overlay pushes a clip rect to the plot). A consumer that cannot
//   clip MUST NOT consume `points`. The key RECTS, by contrast, ARE clipped:
//   ctrl::graph::keyRect(scale, frame, valueOf(key)) clipped to the plot,
//   empties dropped - one geometry with the editor's hit-test. Nothing in
//   `points` is dropped or cut, so a curve's own samples can be reproduced from
//   them exactly.
//   keys: the key HIT squares, from ctrl::graph::keyRect - the editor's own
//   hit-test rectangle (and the panel's right-click menu's), clipped to the
//   plot, empties dropped. They are HIT geometry: the drawn marker is the
//   circle `keyDots` names (the VISUAL/HIT split in graph_Geometry.h), so a
//   consumer must not size its ink from these rects.
//   keyDots: the marker centre of each of those keys, from
//   ctrl::graph::keyDot - a circle of radius ctrl::graph::kKeyDotRadius (3.75px,
//   the timeline's own key-body radius ctrl::keyRadius) is drawn about it. Same
//   order and length as `keys` and `keyFrames`. Published because the hit rect
//   is cut at the plot's edge while the marker stays on the key: the rect's own
//   centre is the wrong centre for a key on the border.
//   keyFrames: the frame of each of those keys, same order, same length - the
//   half of a key's identity a point or a rect cannot carry, so a consumer that
//   HITS a key can name the key it hit (node, channel and frame are the
//   timeline's own triple). Published beside `keys` rather than reconstructed by
//   the consumer: an empty rect is dropped, so an index into `keys` is not an
//   index into the curve's key list.
//   handles: one PanelHandle pair per segment of the curve HAVE handles, in
//   segment order (segment 0's two handles, then segment 1's, ...). base is the
//   key the handle belongs to, mapped through the scaler (handle 0's base is the
//   segment's `from` key, handle 1's the `to` key - each control's stem leaves
//   the key it moves).
//   A CUSTOM segment publishes its STORED pair (ghost == false): dot is
//   ctrl::graph::handlePoint(scale, segment, index) - the SAME mapping the
//   editor's hit-test grabs through, so a grab at the dot is the drag the editor
//   performs.
//   An AUTO segment publishes its DERIVED pair (ghost == true): dot is
//   ctrl::graph::handlePointAt over Curve::ghostHandles(segment.from), i.e. the
//   SAME mapping through the same function (x is always the third and two thirds
//   of the segment's span; the two y values come from the key's neighbours), so
//   a stored pair and a derived one cannot drift apart. The document holds no
//   such pair - it is recomputed - which is exactly what `ghost` tells a
//   consumer: drawing it filled would claim the key stores a tangent it does
//   not.
//   Every other easing type (the analytic curves) publishes nothing for that
//   segment, so a curve with no Custom and no Auto segment has an empty
//   `handles`.
//   radius is ctrl::graph::kHandleRadius (the dot's drawn radius); the grab
//   radius is the larger ctrl::graph::kHandleHitRadius, and the two are
//   deliberately different (the same visual/hit split as a key's).
//   A pair is published whether or not it lies inside the plot: the drawing
//   layer clips (the overlay's plot clip covers the handles like everything
//   else), and a handle the user can grab off the plot's edge must still be
//   published for the highlight to name it.
//   color: theme::Graph::keyColor(channel.type()), which falls back to
//   theme::Graph::curve() for a type the palette does not name.
//   chip/chipText: the curve's own NAME, IN PLACE.
//   chip is the box the name sits in: it is anchored at the plot's RIGHT edge
//   (kChipRightGap = 8px inside it) and vertically centred on the value the
//   curve's polyline leaves the window with - the last of `points`, so the chip
//   is derived from the curve it names rather than from a second evaluation of
//   the curve at the edge. The box is the text's own advance plus
//   kChipPadX (6px) on each side, the reference's own backing padding. It is a
//   chip at the CURVE, not a legend in a corner,
//   so the name of a channel cannot be read off the wrong row; that is the whole
//   difference from the deleted legend.
//   chipText is "Move X", or the bare type name for a one-component type
//   ("Opa", not "Opa Opacity"): a channel whose type has one component has no
//   sibling to be told apart from, so the component word is noise.
//   chip is EMPTY when the box would not fit inside the plot - a curve leaving
//   the window too near the top or the bottom edge, or a plot too narrow for the
//   name - and the drawing layer MUST draw nothing for it and MUST NOT
//   hit-test any geometry derived from it (QRect().right() is -1 and its centre
//   is (0,0), so a box derived from an empty rect lands in the panel's corner).
//   WHICH chips are drawn is the drawing layer's: the model publishes one chip
//   per curve and the layer draws the ones IN PLAY - the channels that own a
//   selected key and whatever the pointer is over - and pushes the drawn ones
//   apart so two names cannot overlap. That subset is EMPHASIS, which is the
//   drawing layer's decision; the model publishes the candidate geometry only,
//   exactly as it publishes every curve and lets the layer dim the ones that own
//   no selected key.
//
// CHANNEL LIST
//   channels: one row per (target, channel) with keys, in target then channel
//   order - the same order as the curves, and the same set the editor builds
//   entries for. A hidden channel keeps its row with visible == false: that is
//   how the user sees what is hidden. The list is the application's own QMenu,
//   so NO row geometry is published (see PanelChannelRow): the menu is a native
//   window the drawing layer neither draws nor measures, exactly like the key
//   menu, and it can scroll, so no row is ever dropped for want of room.
//
// CHANNELS BUTTON
//   channelsButton/channelsButtonText: the panel's ONE control - the button that
//   opens the channel list - and the words on it. It sits in the panel's
//   top-right corner, kChannelsButtonGap (8px) inside the PANEL's right edge and
//   kChannelsButtonTopGap (8px) below rulerBandTop, so it stays clear of the
//   ruler band and cannot be read as part of the scale (the reference's own
//   placement: `right: 8px; top: 30px` of the panel root, 30 being the band's 22
//   plus this 8 while nothing occludes the band). The box is the text's advance plus the button's own padding
//   (kChannelsButtonPadX/Y, the reference's 6px/2px), and it is EMPTY when it
//   would not fit inside the plot - a control that cannot be drawn must not be
//   hit-tested either. It is EMPTY too while the panel has no channel at all
//   (channelsButtonText is empty with it): there is no list to open, and an empty
//   panel's own reason is what says why it is empty.
//   The TEXT is "6 channels" while every channel of the panel is shown, and
//   "4 of 6" once one is hidden: the count is the state the button changes, so
//   the button states it. It is translated here (the model is where the panel's
//   words live), and the list itself is built by the application from the rows.
//
// SELECTION HULL
//   selectionHull: the convex outline of the selected keys, in the SAME
//   viewport pixels as `plot` - ctrl::GraphEditor::selectionHull() is (frame,
//   value) and this maps every point through the same Scaler the curves use, so
//   the outline cannot desync from the markers it encloses. Counter-clockwise,
//   collinear points dropped. EMPTY when nothing is selected, and then nothing
//   is drawn for it: an empty hull is the signal, exactly as an empty marquee is.
//   selectionCount is ctrl::GraphEditor::selectionCount(), the number of keys
//   the hull encloses: the editor's own FACT, published beside the label the way
//   keyFrames is published beside keyDots, so a consumer that shows the count
//   somewhere other than this box (a header, a status line) reads the number
//   rather than the box.
//   selectionLabel: the one place this model writes it - "3 keys" - in the
//   panel's own label currency, in a box centred on the hull's bounding box just
//   above its top edge (and just inside it when the hull touches the plot's top),
//   clamped into the plot. EMPTY while nothing is selected (the count is 0).
//   The hull is drawn with the marquee's ink: it is the same selection, read as
//   an outline around the markers instead of around a drag rectangle.
//
// PLAYHEAD
//   playheadX == Scaler::pixelX(aEditor.currentFrame()), or -1 when the editor
//   reports no frame (currentFrame() < 0, i.e. no project is set). It is read
//   even when the plot is empty, because a playhead is a property of the
//   document and not of the geometry.
//
// MARQUEE
//   marquee == the editor's own box-select rectangle (GraphEditor::marqueeRect()),
//   or an EMPTY rect while no marquee drag is in flight - which is the model's
//   whole signal, since the editor publishes an empty rect whenever no drag is
//   live (a press and release with no movement included). It is the gesture's own
//   geometry, in the same viewport pixels as `plot`, and it is NOT clipped here:
//   the editor already clips it to the plot, and the drawing layer's plot clip
//   covers it whichever way the rectangle runs. The model draws nothing for it (it
//   publishes no ink of its own anywhere); what it adds is the identity of the
//   rectangle, so the gesture the editor is performing and the rectangle on screen
//   are one geometry.
//
// zeroY
//   The y of value 0 while the value window contains it, else -1. Published so
//   the drawing layer can place a zero rule without re-deriving the axis; the
//   model draws nothing for it (the panel has no grid).
//
// EMPTY STATE
//   emptyState is non-empty exactly when there is nothing to draw, and then NO
//   curve is produced (isEmpty() is the pair of those facts):
//     * "Select a layer to show its curves"  - no target selected;
//     * "No keys on the selected layers"     - targets selected, no channel has
//                                              a key;
//     * "All channels of the selected layers are hidden";
//     * "No room for the graph in this viewport" - the plot came out empty.
//   The strings are translatable: they are marked with
//   QT_TRANSLATE_NOOP("GraphPanel", ...) and resolved by graphPanelTr() through
//   that same context (see it above).
//   With targets but every shown channel hidden, the channel list still carries
//   the hidden rows and emptyState names the state: an empty panel must say why
//   it is empty.
//
// READ-ONLY
//   buildGraphPanelModel() takes the editor by non-const reference only because
//   GraphEditor's own accessors are split that way (curves(), targets() and
//   scale() are const-callable, isChannelVisible() is not). It mutates nothing:
//   it never writes the document, the editor, its scaler, its selection or its
//   hidden-channel set, and the scaler it maps through is a local copy.
struct GraphPanelModel {
    QRect panel;
    QRect plot;
    QRect valueColumn;
    // Where the panel's top content may begin: plot.top() unless a widget floating
    // above the viewport covers part of it (see THE OCCLUDED TOP above). Always
    // >= plot.top() and never past the plot's bottom edge.
    int rulerBandTop = -1;
    QVector<int> frameTickX;
    // The tier of each tick in frameTickX: same length, same order, so index i is
    // tick i's tier (the ladder's own ctrl::time::Scaler::TickLevel as an int; None
    // never appears). A CLASSIFICATION, not an appearance - the drawing layer maps a
    // tier to its own alpha (see THE AXES above).
    QVector<int> frameTickLevel;
    QVector<int> valueTickY;
    QVector<PanelLabel> frameLabels;
    QVector<PanelLabel> valueLabels;
    int zeroY = -1;
    int playheadX = -1;
    // The live box-select rectangle, or an empty rect while no marquee drag is in
    // flight (see the MARQUEE note above). The drawing layer paints it inside the
    // plot clip; nothing is drawn for an empty rect.
    QRect marquee;
    // The live selection's hull in panel pixels, mapped from
    // ctrl::GraphEditor::selectionHull() through the same Scaler the curves use
    // (see SELECTION HULL); EMPTY when nothing is selected, and then nothing is
    // drawn for it and no label is published either.
    QVector<QPointF> selectionHull;
    // The number of keys that hull encloses (ctrl::GraphEditor::selectionCount()),
    // 0 with nothing selected.
    int selectionCount = 0;
    // The one place this model writes that number as words - "3 keys" - and the box
    // it goes in, above the hull. EMPTY while nothing is selected.
    PanelLabel selectionLabel;
    QVector<PanelCurve> curves;
    // The project's own loop rule, as the editor reported it, for every curve in this model.
    bool projectLoop = false;
    util::Range projectLoopRange;
    QVector<PanelChannelRow> channels;
    // The panel's ONE control: the button that opens the channel list, and the
    // words on it. Both empty/empty-rect when the button would not fit inside the
    // plot (see CHANNELS BUTTON), and then there is no control to draw or to hit.
    QRect channelsButton;
    QString channelsButtonText;
    QString emptyState;
    bool isEmpty() const { return curves.isEmpty() && !emptyState.isEmpty(); }
};

// Builds the model. aViewport is the whole viewport the panel is pinned over;
// aTheme is the palette the curves and swatches take their colours from; aTopInset is
// how many LOGICAL pixels of that viewport's top a widget floating above it covers (0
// when nothing does), which moves the ruler band and nothing else - see THE OCCLUDED
// TOP above.
GraphPanelModel buildGraphPanelModel(
    ctrl::GraphEditor& aEditor, const QSize& aViewport, const theme::Graph& aTheme, int aTopInset = 0
);

} // namespace gui

#endif // GUI_GRAPHPANELMODEL_H
