#include "gui/GraphPanelModel.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <algorithm>

#include "XCAssert.h"
#include "core/Frame.h"
#include "core/ObjectNode.h"
#include "core/TimeKey.h"
#include "core/TimeLine.h"
#include "ctrl/GraphEditor.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Geometry.h"
#include "ctrl/graph/graph_Scaler.h"
#include "gui/theme/Graph.h"

namespace {

// The panel's layout constants. They were the deleted QPainter renderer's own
// convention (ctrl/graph/graph_Renderer.cpp, removed in the ImGui cutover), and
// they are stated here because the model is now their one home - the renderer
// kept them private to its translation unit. The constants the rest of the code
// shares are read from their own homes instead: Scaler::kMargin, the label
// column's own size rule (Scaler::axisLabelWidth(), measured - see THE LABEL
// COLUMN in graph_Scaler.h) and Scaler::kAxisLabelPad from the scaler,
// ctrl::graph::kKeyDotRadius from graph_Geometry.h.
//
// The short tick marks replaced the renderer's full grid lines. Their length is
// the drawing layer's business (the model publishes positions); 4px is the
// convention this model documents, and the frame numbers are placed clear of it.
constexpr int kFrameTickLength = 4;
// Least gap between the right edge of one frame number's box and the left edge
// of the next. The renderer leaned on the ladder's 48px major spacing instead,
// which is a property of the axis rather than of the label; stating it on the
// boxes keeps two numbers from touching under ANY font metric.
constexpr int kFrameLabelGap = 8;
// Horizontal padding of a text box around its glyphs, the renderer's own
// `horizontalAdvance(text) + 4`.
constexpr int kLabelPad = 2;
// The chip that names a curve IN PLACE (see CHIPS in the header): the box's own
// padding around the text and its inset from the plot's right edge. The numbers
// are the reference's - mock-graph.js right-aligns the text 8px inside the plot
// and pads the chip's backing 6px left and right (its backdrop is the text box
// grown by (6, 8)).
constexpr int kChipRightGap = 8;
constexpr int kChipPadX = 6;

// The panel's ONE control (see CHANNELS BUTTON in the header): its horizontal inset -
// from the PANEL's right edge on the overlay's floor, from the PLOT's on the dock's -
// the air under the ruler band it hangs in, and its padding around its text. The
// reference floats it at the panel's top-right corner, 8px inside the edge and 30px
// from the top - which is kChannelsButtonTopGap below the band's whole block (its
// kFrameTickLength tick rows plus the numbers' own line) while nothing occludes the
// band - padded 6px/2px, "clear of the ruler band so it cannot read as part of the
// scale" (mock.css, .gp-chbtn:not(.hosted)). That clearance holds in BOTH homes,
// the docked one included: the reference's hosted button carries no placement of its
// own (mock.css, .gp-chbtn.hosted) because the host places it, and the panel's band is
// the ruler's own ink either way.
constexpr int kChannelsButtonGap = 8;
constexpr int kChannelsButtonTopGap = 8;
constexpr int kChannelsButtonPadX = 6;
constexpr int kChannelsButtonPadY = 2;

// The gap between the selection label's box and the hull's own bounding box.
constexpr int kSelectionLabelGap = 2;

// The sampling budget of a curve: one point per ~2px of plot width, bounded so a
// dense key set does not explode the point count and a single long segment
// cannot ask for thousands of samples.
constexpr int kMinSamplesPerSegment = 2;
constexpr int kMaxSamplesPerSegment = 256;

// The value axis' number format, and the value labels' texts, are
// ctrl::graph::Scaler::valueAxisText: the label and the column's WIDTH are
// measured off the SAME strings, so the format must not be restated here.

// The frame axis: the ladder's own ticks, and a number on the majors.
//
// The frame window is the plot's edges through the frame mapping: a ladder can
// be far denser than the plot is wide, and only what is visible is published.
// Ticks are positions, never spans - there is no grid. Each tick's TIER travels
// beside its position in frameTickLevel (same order, same length): the ladder that
// decided the tick also said which tier it is, so the classification is read here
// rather than re-derived by a drawing layer from the tick spacing - and what a tier
// looks like (its alpha; the length is one for every level) stays that layer's
// decision, the way the model publishes a position and not a span.
void buildFrameAxis(gui::GraphPanelModel& aModel, const ctrl::graph::Scaler& aScale, const QFontMetrics& aMetrics) {
    const QRect& plot = aModel.plot;
    const core::Frame first = aScale.frameAt(plot.left());
    const core::Frame last = aScale.frameAt(plot.right());
    const int textHeight = aMetrics.height();
    // The box sits below the tick marks, below the band the panel's top may use (see
    // THE OCCLUDED TOP in the header: a widget floating above the viewport moves the
    // band down, and the numbers with it). Without room for a whole text line under
    // the marks there is nothing to label.
    const int labelTop = aModel.rulerBandTop + kFrameTickLength + 1;
    const bool roomForText = (labelTop + textHeight - 1) <= plot.bottom();

    bool anyLabel = false;
    int lastLabelRight = 0;

    // Strided by the finest level the ladder draws (Scaler::tickStride): the window can span
    // viewport / spacing frames at a deep zoom-out, and the drawn ticks are a handful. The start
    // is aligned to the stride, which is what the levels are tested against.
    const double framePpf = aScale.framePpf();
    const int stride = aScale.frameScale().tickStride(framePpf);
    int frame = first.get();
    if (const int rem = frame % stride)
        frame += rem > 0 ? stride - rem : -rem;
    for (; frame <= last.get(); frame += stride) {
        const ctrl::time::Scaler::TickLevel level =
            aScale.frameScale().attributeAt(frame, framePpf).level;
        if (level == ctrl::time::Scaler::TickLevel::None)
            continue; // the ladder says this frame sits too close to its neighbours to mark

        const int x = aScale.pixelX(core::Frame(frame));
        // pixelX rounds, so a frame whose mapping falls a pixel outside the plot
        // would otherwise be ticked in the margin.
        if (x < plot.left() || plot.right() < x)
            continue;
        if (aModel.frameTickX.isEmpty() || aModel.frameTickX.last() != x) {
            aModel.frameTickX.append(x);
            // The tier travels with the position, appended in the same step so the two
            // vectors cannot desync: the model classifies the tick (the ladder's own
            // TickLevel), and how a tier LOOKS - its alpha, its length - stays the
            // drawing layer's decision (see THE AXES in the header).
            aModel.frameTickLevel.append(static_cast<int>(level));
        }

        if (level != ctrl::time::Scaler::TickLevel::Major || !roomForText)
            continue;

        // Only Major frames are numbered. The box is centred on its tick and
        // then nudged inside the plot, so a number at the window's edge stays
        // readable instead of being cut off by the panel's border.
        const QString text = QString::number(frame);
        const int width = aMetrics.horizontalAdvance(text) + 2 * kLabelPad;
        if (width > plot.width())
            continue; // the number cannot fit inside the plot at all

        const int left = std::clamp(x - width / 2, plot.left(), plot.right() - width + 1);
        if (anyLabel && left < lastLabelRight + kFrameLabelGap)
            continue; // this number would touch the previous one

        gui::PanelLabel label;
        label.text = text;
        label.rect = QRect(left, labelTop, width, textHeight);
        aModel.frameLabels.append(label);
        lastLabelRight = left + width;
        anyLabel = true;
    }
}

// The value axis: the window's max / midpoint / min, right-aligned in the left
// column, each on the pixel its value maps to. Two of the three rounding to the
// same string would draw one number on top of itself, so the duplicate is
// dropped (the model's own rule).
//
// The column's width is the scaler's stable reserve for the FORMAT
// (Scaler::axisLabelWidth - the widest label 'g'/4 can write, measured once per
// font), so the numbers fit by construction while the column itself never moves
// when the window does; a number that still does not fit the box - reachable
// only once the column has hit its ceiling, which this format's widest label
// (62px at the UI font) cannot reach - is DROPPED rather than published
// overflowing, the same way the frame axis drops a number wider than the plot.
// Its tick is still published: the axis HAS a tick at that value whatever can be
// written beside it.
void buildValueAxis(gui::GraphPanelModel& aModel, const ctrl::graph::Scaler& aScale, const QFontMetrics& aMetrics) {
    const QRect& column = aModel.valueColumn;
    constexpr int kAxisLabelPad = ctrl::graph::Scaler::kAxisLabelPad;

    // The baseline exists only while 0 is inside the window: a line drawn for an
    // out-of-window zero would sit on (or outside) the plot edge and read as a
    // border rather than as the value origin.
    if (aScale.valueMin() <= 0.f && 0.f <= aScale.valueMax())
        aModel.zeroY = aScale.pixelY(0.f);

    const int textHeight = aMetrics.height();
    // The column the numbers may be placed in: the value column, with its bottom
    // shortened by one text line so a number centred on a value can still fit. The
    // occluded band moves the top of that window down (see THE OCCLUDED TOP), and when
    // the band has reached past the bottom there is no room for a number at all - the
    // same answer as a column shorter than a text line.
    const int columnTop = std::max(column.top(), aModel.rulerBandTop);
    const int columnBottom = column.bottom() - textHeight + 1;
    if (column.height() < textHeight || columnBottom < columnTop)
        return;

    const float values[3] = {aScale.valueMax(), 0.5f * (aScale.valueMin() + aScale.valueMax()), aScale.valueMin()};
    QString shown;
    for (const float value : values) {
        const QString text = ctrl::graph::Scaler::valueAxisText(value);
        if (text == shown)
            continue;
        shown = text;

        // The number is centred on its value and clamped into the column: the
        // extreme values sit on the plot's edges, where half a text height would
        // spill past the panel. The column starts at the plot's top edge, so the
        // topmost number is also clamped below the occluded band (the widget floating
        // above the viewport covers the value column too - see THE OCCLUDED TOP).
        const int y = aScale.pixelY(value);
        const int top = std::clamp(y - textHeight / 2, columnTop, columnBottom);

        aModel.valueTickY.append(y);

        // The box the drawing layer right-aligns the text in. The column was
        // MEASURED from these very strings, so this holds for every label the
        // value axis can write at the UI font; the guard is the honest floor
        // under a wider font (the column's own ceiling, see graph_Scaler.h) and
        // is the frame axis' rule: a number that cannot fit is not drawn
        // half-way off the panel.
        const int boxWidth = column.width() - kAxisLabelPad;
        if (aMetrics.horizontalAdvance(text) > boxWidth)
            continue;

        gui::PanelLabel label;
        label.text = text;
        label.rect = QRect(column.left(), top, boxWidth, textHeight);
        aModel.valueLabels.append(label);
    }
}

// The channel list: one row per (target, channel) that has keys - the same set of
// channels the editor builds entries for (a type whose map is empty has no curve
// and no row), in target then channel order, hidden channels included. The row's
// colour is the curve's colour, so the list's swatch and its curve always read as
// one feature. No row GEOMETRY is laid out: the list is the application's own
// QMenu (see the header's CHANNEL LIST).
//
// The same walk also lays out the panel's ONE control, the button that opens that
// list: its text states the shown/total count and its box is the text plus the
// button's padding, below the ruler band's own block and in the corner its host owns
// (see CHANNELS BUTTON and GraphPanelHost).
void buildChannels(
    gui::GraphPanelModel& aModel, ctrl::GraphEditor& aEditor, const theme::Graph& aTheme, const QFontMetrics& aMetrics,
    gui::GraphPanelHost aHost
) {
    // focusedNode() hands out a CONST pointer (GraphEditor.h), and this model only
    // ever COMPARES it with the targets' own (non-const) pointers - it never mutates
    // through it - so the local keeps the const rather than casting it away.
    const core::ObjectNode* focusedNode = aEditor.hasFocus() ? aEditor.focusedNode() : nullptr;
    const ctrl::graph::Channel focusedChannel = aEditor.focusedChannel();

    for (core::ObjectNode* node : aEditor.targets()) {
        if (!node)
            continue;
        const core::TimeLine* timeLine = node->timeLine();
        if (!timeLine) // a folder, or a node kind that holds no keys at all
            continue;

        for (core::TimeKeyType type : ctrl::graph::Channel::graphableTypes()) {
            if (timeLine->map(type).isEmpty())
                continue; // no keys: nothing to draw and nothing to toggle

            for (const ctrl::graph::Channel& channel : ctrl::graph::Channel::channelsOf(type)) {
                gui::PanelChannelRow row;
                row.node = node;
                row.channel = channel;
                row.layer = node->name();
                // A one-component type has no sibling to be told apart from, so the
                // component word is noise: "Opa", not "Opa Opacity" (the reference
                // draws exactly this chip, mock-graph.js's _curveLabels).
                row.name = ctrl::graph::Channel::componentCount(type) > 1
                               ? channel.name()
                               : core::TimeLine::getTimeKeyName(type);
                const QColor color = aTheme.keyColor(channel.type());
                row.color = color.isValid() ? color : aTheme.curve();
                row.visible = aEditor.isChannelVisible(*node, channel);
                row.focused = node == focusedNode && channel == focusedChannel;
                aModel.channels.append(row);
            }
        }
    }

    // The button's label: the total while every channel is shown ("6 channels"),
    // and the shown count out of the total once one is hidden ("4 of 6") - the
    // count IS the state the button changes. With NO channels there is no list to
    // open, so there is no control either: nothing is published (an empty panel's
    // reason already says why it is empty, and a button that opens nothing would be
    // a hit target with no action behind it).
    int shown = 0;
    for (const gui::PanelChannelRow& row : aModel.channels) {
        if (row.visible)
            ++shown;
    }
    const int total = aModel.channels.size();
    if (total <= 0)
        return;
    aModel.channelsButtonText =
        shown == total
            ? gui::graphPanelTr(QT_TRANSLATE_N_NOOP("GraphPanel", "%n channel(s)"), total)
            : QCoreApplication::translate("GraphPanel", QT_TRANSLATE_NOOP("GraphPanel", "%1 of %2"))
                  .arg(shown)
                  .arg(total);

    // The button's box. It hangs BELOW the band's whole block in BOTH homes: the tick
    // rows and the frame numbers' line are the ruler's own ink, and the panel's one
    // control is a view control, which the design keeps out of it ("never in the ruler
    // band", index.html:328-329) - a 0.55-alpha backing painted over a number is
    // exactly the defect that rule exists to prevent. The offset is stated in the
    // BAND'S own terms rather than as the reference's magic 30, so it cannot drift when
    // the band moves down under a widget floating above the panel or when a font
    // changes the number row's height:
    //
    //   top == rulerBandTop + kFrameTickLength + 1 + textHeight + kChannelsButtonTopGap
    //
    // i.e. the ticks' kFrameTickLength rows, the one line of numbers under them
    // (buildFrameAxis's labelTop == rulerBandTop + kFrameTickLength + 1) and
    // kChannelsButtonTopGap of air. The reference's 30px from the panel's top is
    // exactly this while nothing occludes the band: its band is 22px - a 4px tick row
    // and an 18px line of numbers - plus this 8.
    //
    // WHERE IT SITS HORIZONTALLY is the one thing the two hosts place differently, and
    // it is what GraphPanelHost names - ONE box either way, so a drawing layer draws
    // and hit-tests channelsButton whichever floor it is drawing:
    //
    //   OVERLAY (kChannelsButtonGap 8px inside the PANEL's right edge): the reference's
    //   own floating rule (mock.css, .gp-chbtn:not(.hosted): `right: 8px` of the panel).
    //
    //   DOCK (the same 8px inside the PLOT's right edge): the docked page is drawn edge
    //   to edge inside the dock, so the surface the panel owns is its PLOT, and its one
    //   control sits in that surface's corner - under the band's right end, among the
    //   curves' chips - rather than in the panel's own 4px margin (Scaler::kMargin).
    //
    // A button that does not fit inside the plot publishes NO box - the drawing layer
    // draws nothing and hit-tests nothing, the model's own truthful signal (the same
    // rule every rect here follows).
    const int textHeight = aMetrics.height();
    const int width = aMetrics.horizontalAdvance(aModel.channelsButtonText) + 2 * kChannelsButtonPadX;
    const int height = textHeight + 2 * kChannelsButtonPadY;
    const int right = aHost == gui::GraphPanelHost::Dock ? aModel.plot.right() + 1 : aModel.panel.right() + 1;
    const int top = aModel.rulerBandTop + kFrameTickLength + 1 + textHeight + kChannelsButtonTopGap;
    const QRect button(right - kChannelsButtonGap - width, top, width, height);

    // THE BAND BELONGS TO THE RULER, so the control may not sit in it - in either home.
    // Stated as a precondition on the geometry rather than left to the two expressions
    // above being right: the box is derived from the band's own measures, and this is
    // the guarantee a future placement change has to keep, failing loudly instead of
    // painting over the frame numbers. The band's block is the tick rows (drawn down
    // from rulerBandTop, kFrameTickLength of them) and the numbers' row under them,
    // inside the plot's own width - which is the region the ink covers.
    const QRect band(
        aModel.plot.left(), aModel.rulerBandTop, aModel.plot.width(), kFrameTickLength + 1 + textHeight
    );
    XC_ASSERT(!band.intersects(button));

    if (aModel.plot.contains(button))
        aModel.channelsButton = button;
}

// The selection's HULL and its count, in the same pixels as everything else: the
// editor states the outline in (frame, value) - never in pixels, so it cannot
// desync from the markers it encloses - and this maps every point through the SAME
// Scaler the curves go through.
//
// Nothing is published while nothing is selected (ctrl::GraphEditor::
// selectionHull() is empty then, and so is the hull here): an empty hull IS the
// signal to draw nothing, exactly as an empty marquee is.
void buildSelection(
    gui::GraphPanelModel& aModel, ctrl::GraphEditor& aEditor, const ctrl::graph::Scaler& aScale,
    const QFontMetrics& aMetrics
) {
    aModel.selectionCount = aEditor.selectionCount();

    const QVector<QPointF> hull = aEditor.selectionHull();
    if (hull.isEmpty())
        return;

    aModel.selectionHull.reserve(hull.size());
    int left = 0;
    int top = 0;
    int right = 0;
    for (int i = 0; i < hull.size(); ++i) {
        const QPointF point(
            aScale.pixelXOfFrame(hull.at(i).x()), static_cast<double>(aScale.pixelY(float(hull.at(i).y())))
        );
        aModel.selectionHull.append(point);
        const int x = int(point.x());
        const int y = int(point.y());
        if (i == 0) {
            left = right = x;
            top = y;
            continue;
        }
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
    }

    // The count label, in the panel's own label currency: centred on the hull's own
    // bounding box, just above its top edge - and just INSIDE it when the hull
    // reaches the plot's top edge, so the label is never pushed out of the plot.
    // A box that does not fit is not published (see the header's SELECTION HULL).
    const QString text =
        gui::graphPanelTr(QT_TRANSLATE_N_NOOP("GraphPanel", "%n key(s)"), aModel.selectionCount);
    const int width = aMetrics.horizontalAdvance(text) + 2 * kLabelPad;
    const int height = aMetrics.height();
    if (aModel.plot.width() < width || aModel.plot.height() < height)
        return; // the box could not be clamped into the plot, so there is none
    const int left2 = std::clamp((left + right + 1) / 2 - width / 2, aModel.plot.left(), aModel.plot.right() - width + 1);
    int top2 = top - height - kSelectionLabelGap;
    if (top2 < aModel.plot.top())
        top2 = top + kSelectionLabelGap; // no room above the hull: sit just inside its top edge
    top2 = std::clamp(top2, aModel.plot.top(), aModel.plot.bottom() - height + 1);

    const QRect box(left2, top2, width, height);
    if (!aModel.plot.contains(box))
        return;
    aModel.selectionLabel.rect = box;
    aModel.selectionLabel.text = text;
}

// The curves: one per visible channel row, sampled and mapped through the scaler
// (Curve::buildPolyline over the plot's frame window, then Scaler::pixelXOfFrame
// / Scaler::pixelY), with the keys and every segment's tangent handles as the
// shared hit-test geometry (ctrl/graph/graph_Geometry.h), and the chip that names
// the curve in place.
void buildCurves(
    gui::GraphPanelModel& aModel, const ctrl::graph::Scaler& aScale, const QFontMetrics& aMetrics
) {
    // The drawing layer inks a curve's tangent handles ONLY for the FOCUSED channel
    // (ImGuiOverlay's drawsHandles: with no focus it draws every curve's; with one it
    // draws that curve's alone), so a pair built for any other row is work nobody
    // reads - and half of it is the expensive kind: an Auto segment's pair is DERIVED
    // by Curve::ghostHandles from the key's neighbours. The rows already carry the
    // predicate (PanelChannelRow::focused, filled by buildChannels from the same editor
    // state the drawing layer reads), so it costs one walk of the rows to know whether
    // any of them is the focused one.
    //
    // WHY THE ROWS AND NOT aEditor.hasFocus(): they agree. buildChannels takes its
    // focused node as null when the editor has no focus, so no row can be focused while
    // the editor reports none - anyFocused implies the focus the drawing layer will call
    // valid, and with that focus it draws exactly the curve whose row is focused. The
    // converse is deliberately the SAFE side: an editor focus that matches no row (a
    // hidden channel, or a node that is not among the targets) leaves anyFocused false,
    // so every row publishes its pairs - which is more than the drawing layer uses,
    // never less.
    bool anyFocused = false;
    for (const gui::PanelChannelRow& row : aModel.channels) {
        if (row.focused) {
            anyFocused = true;
            break;
        }
    }

    for (int i = 0; i < aModel.channels.size(); ++i) {
        const gui::PanelChannelRow& row = aModel.channels[i];
        if (!row.visible)
            continue; // a hidden channel keeps its row and contributes no curve

        const core::TimeLine* timeLine = row.node ? row.node->timeLine() : nullptr;
        if (!timeLine)
            continue;

        const ctrl::graph::Curve curve = [&] {
            ctrl::graph::Curve made(*timeLine, row.channel);
            made.setLoop(aModel.projectLoop, aModel.projectLoopRange);
            return made;
        }();

        gui::PanelCurve out;
        out.node = row.node;
        out.channel = row.channel;
        out.loop = aModel.projectLoop;
        out.loopRange = aModel.projectLoopRange;
        out.color = row.color;
        out.channelRow = i;

        const core::Frame first = aScale.frameAt(aModel.plot.left());
        const core::Frame last = aScale.frameAt(aModel.plot.right());
        const int segments = std::max(1, curve.segmentCount());
        // One point per ~2px of plot width per segment (the drawing budget), floored so the
        // analytic easings keep their shape and capped so a long segment cannot ask for thousands
        // of samples. The budget is spent on the part of each segment the PLOT SHOWS (see
        // Curve::buildPolyline), which is what keeps a zoomed-in curve smooth.
        const int samples = std::clamp(aModel.plot.width() / (2 * segments), kMinSamplesPerSegment, kMaxSamplesPerSegment);

        QVector<QPointF> polyline;
        curve.buildPolyline(first, last, samples, polyline);
        out.points.reserve(polyline.size());
        for (const QPointF& sample : polyline) {
            out.points.append(
                QPointF(aScale.pixelXOfFrame(sample.x()), aScale.pixelYOfValue(float(sample.y())))
            );
        }

        // ctrl::graph::keyRect is the editor's HIT-TEST geometry: one rectangle,
        // so a marquee or a right-click lands on the key the editor agrees on. A
        // key straddling the plot's edge is cut rather than skipped - the drawing
        // layer clips a key over the panel's chrome.
        //
        // The DOT is the other half: keyDot() is the key marker's own centre and
        // the drawing layer paints a circle of radius kKeyDotRadius about it
        // (ctrl/graph/graph_Geometry.h's VISUAL/HIT split). It is published
        // rather than derived from the rect on purpose - the rect above is CUT
        // at the plot's edge, so its centre is not the key's centre for a key on
        // the border, and a dot reconstructed from it would sit off the key. Same
        // order and same length as `keys` and `keyFrames`: a key that publishes
        // no rect (one entirely outside the plot) publishes no dot either.
        for (const core::TimeKey* key : curve.keys()) {
            if (!key)
                continue;
            const core::Frame frame(key->frame());
            const float value = curve.valueOf(*key);
            const QRect rect = ctrl::graph::keyRect(aScale, frame, value).intersected(aModel.plot);
            if (!rect.isEmpty()) {
                out.keys.append(rect);
                out.keyFrames.append(frame);
                out.keyDots.append(ctrl::graph::keyDot(aScale, frame, value));
            }
        }

        // Every segment's tangent handles, in segment order: one pair per segment,
        // handle 0 leaving `from` and 1 arriving at `to`. The base is the key the
        // handle belongs to, so the drawn stem starts on the key it moves, and the
        // dot is the mapping the editor's own hit-test grabs through - a Custom
        // segment's STORED pair (handlePoint) or an Auto segment's DERIVED one
        // (handlePointAt over ghostHandles, the same function handlePoint calls), so
        // a stored pair and a derived one cannot drift apart. `ghost` carries which
        // kind it is: the derived pair is not in the document, and the drawing layer
        // draws it hollow (a drag on it converts the key to Custom).
        // Published whether or not the pair lies inside the plot: the drawing layer
        // clips, and a handle off the plot's edge is still one the user can grab.
        //
        // Only for the curve the drawing layer INKS them on (see anyFocused above):
        // the pairs exist for the handle pass and its hover, both of which skip every
        // curve a focus excludes, so a pair built for another row would be a derived
        // tangent nobody reads - an empty `handles` is what the layer sees for those
        // rows. No focus means every row publishes, which is the layer's no-focus ink.
        if (!anyFocused || row.focused) {
            const int handleSegments = curve.segmentCount();
            for (int s = 0; s < handleSegments; ++s) {
                const ctrl::graph::Curve::Segment segment = curve.segment(s);
                if (!segment.from || !segment.to)
                    continue;

                const bool custom = curve.isCustom(segment);
                // The derived pair, when this is an Auto segment and there is something
                // to derive from (a last key in the channel has none, and neither has a
                // type that carries no easing at all): empty otherwise.
                const QVector<QPointF> ghosts = custom ? QVector<QPointF>() : curve.ghostHandles(*segment.from);
                if (!custom && ghosts.size() != 2)
                    continue; // the analytic easings expose no handle to drag at all

                const core::TimeKey* owners[2] = { segment.from, segment.to };
                const float ownerValues[2] = { segment.fromValue, segment.toValue };
                for (int h = 0; h < 2; ++h) {
                    gui::PanelHandle handle;
                    handle.segment = s;
                    handle.index = h;
                    handle.base = QPointF(
                        aScale.pixelXOfFrame(static_cast<double>(owners[h]->frame())),
                        static_cast<double>(aScale.pixelY(ownerValues[h]))
                    );
                    handle.dot = custom ? ctrl::graph::handlePoint(aScale, segment, h)
                                        : ctrl::graph::handlePointAt(aScale, segment, ghosts.at(h));
                    handle.radius = ctrl::graph::kHandleRadius;
                    handle.ghost = !custom;
                    out.handles.append(handle);
                }
            }
        }

        // The chip that names this curve IN PLACE: at the plot's right edge, on the
        // value the curve LEAVES the window with - the last of the polyline
        // `points` above, so the chip is derived from the curve it names rather than
        // from a second evaluation of the curve at the edge. An empty box when the
        // chip would not fit inside the plot (see the header's CHIPS): a curve
        // leaving the window too near an edge, or a plot too narrow for the name,
        // publishes no chip rather than one in the margin or over another row.
        out.chipText = row.name;
        if (!out.points.isEmpty()) {
            const int width = aMetrics.horizontalAdvance(out.chipText) + 2 * kChipPadX;
            const int height = aMetrics.height();
            const double y = out.points.last().y();
            const QRect chip(
                aModel.plot.right() + 1 - kChipRightGap - width, qRound(y) - height / 2, width, height
            );
            if (aModel.plot.contains(chip))
                out.chip = chip;
        }

        aModel.curves.append(out);
    }
}

// Why an empty panel is empty. A panel with nothing in it has to say so: "no
// keys" and "all hidden" are different states with different fixes.
void buildEmptyState(gui::GraphPanelModel& aModel, ctrl::GraphEditor& aEditor) {
    if (!aModel.curves.isEmpty())
        return;

    bool anyTarget = false;
    for (core::ObjectNode* node : aEditor.targets()) {
        if (node)
            anyTarget = true;
    }

    if (aModel.channels.isEmpty()) {
        aModel.emptyState =
            anyTarget ? gui::graphPanelTr(QT_TRANSLATE_NOOP("GraphPanel", "No keys on the selected layers"))
                      : gui::graphPanelTr(QT_TRANSLATE_NOOP("GraphPanel", "Select a layer to show its curves"));
    } else {
        aModel.emptyState =
            gui::graphPanelTr(QT_TRANSLATE_NOOP("GraphPanel", "All channels of the selected layers are hidden"));
    }
}

} // namespace

namespace gui {

    //-------------------------------------------------------------------------------------------------
    QString graphPanelTr(const char* aSourceText) {
        // The context is spelled out on BOTH sides: the QT_TRANSLATE_NOOP at the
        // call site is what the extractor reads, and this is what Qt asks at
        // runtime. They are the same literal "GraphPanel" written once at each
        // call site, so a renamed context fails the extractor's check rather than
        // silently shipping English.
        return QCoreApplication::translate("GraphPanel", aSourceText);
    }

    //-------------------------------------------------------------------------------------------------
    QString graphPanelTr(const char* aSourceText, int aN) {
        // The plural half of the pair above: the same context, resolved through
        // Qt's plural form, and the call site carries the same literal in
        // QT_TRANSLATE_N_NOOP("GraphPanel", ...) for the extractor.
        return QCoreApplication::translate("GraphPanel", aSourceText, nullptr, aN);
    }

    //-------------------------------------------------------------------------------------------------
    GraphPanelModel buildGraphPanelModel(
        ctrl::GraphEditor& aEditor, const QSize& aViewport, const theme::Graph& aTheme, int aTopInset,
        GraphPanelHost aHost
    ) {
        GraphPanelModel model;

        // The floor this model is for: it is republished (see GraphPanelHost) and it
        // places the panel's ONE control, which is the whole of what the two hosts
        // change inside this model.
        model.host = aHost;

        // The panel rect is the editor's own, whatever it is: the fullscreen
        // graph view over the animation. Nothing here hardcodes a size, and
        // nothing here writes back into the editor's scaler.
        model.panel = ctrl::GraphEditor::panelRect(aViewport);

        model.projectLoop = aEditor.isLooping();
        model.projectLoopRange = aEditor.frameRange();

        ctrl::graph::Scaler scale = aEditor.scale();
        scale.setRect(model.panel);

        model.plot = scale.plotRect();
        // The band the panel's top content may use: the plot's own top, moved down by
        // the occluded inset (see THE OCCLUDED TOP in the header). A negative inset is
        // no inset at all, and the result never leaves the plot.
        const int inset = std::max(0, aTopInset);
        model.rulerBandTop = std::min(model.plot.top() + inset, model.plot.bottom() + 1);
        // The column's width is the scaler's STABLE reserve for the value format
        // (see THE LABEL COLUMN in graph_Scaler.h) - the width the plot was
        // already inset by - so the column and the plot are one geometry, a
        // number can never need more room than the column has, and a gesture that
        // changes which label is widest cannot move either one.
        model.valueColumn = QRect(
            model.panel.left() + ctrl::graph::Scaler::kMargin, model.plot.top(), scale.axisLabelWidth(),
            model.plot.height()
        );

        // The playhead belongs to the document, not to the geometry, so it is
        // read before the plot's own guards: a currentFrame() below 0 is "no
        // project", and there is no playhead to place (core frame numbers start
        // at 1, so a negative frame is never a real one).
        const core::Frame playhead = aEditor.currentFrame();
        if (playhead.get() >= 0)
            model.playheadX = scale.pixelX(playhead);

        // The live box-select rectangle, straight from the gesture that owns it: the
        // editor publishes an EMPTY rect while no drag is in flight (and for a press
        // with no movement), which is exactly "draw nothing". Normalised and clipped
        // to the plot by the editor, so this is a copy of one geometry rather than a
        // second derivation of it.
        model.marquee = aEditor.marqueeRect();

        if (model.plot.isEmpty()) {
            // Nothing to lay an axis on. The panel, the band, the playhead, the marquee
            // (the editor's own rect, empty unless a drag is live) and the reason are
            // still published: an empty panel must say why it is empty.
            model.rulerBandTop = model.plot.bottom() + 1;
            model.emptyState =
                graphPanelTr(QT_TRANSLATE_NOOP("GraphPanel", "No room for the graph in this viewport"));
            return model;
        }

        // Text is measured with the default UI font: the model has no painter by
        // design, and a label box is advisory geometry the drawing layer puts its
        // own glyphs in.
        const QFontMetrics metrics{QFont()};

        buildFrameAxis(model, scale, metrics);
        buildValueAxis(model, scale, metrics);
        buildChannels(model, aEditor, aTheme, metrics, aHost);
        buildCurves(model, scale, metrics);
        buildSelection(model, aEditor, scale, metrics);
        buildEmptyState(model, aEditor);

        return model;
    }

} // namespace gui
