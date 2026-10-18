#ifndef GUI_IMGUIOVERLAY_H
#define GUI_IMGUIOVERLAY_H

#include <QColor>
#include <QPoint>
#include <QRect>
#include <QScopedPointer>
#include <QSize>
#include <QVariant>
#include <QVector>
#include <QtCore/qnamespace.h>
#include <QtOpenGL/QtOpenGL>

#include "core/TimeKeyType.h"
#include "gui/GraphPanelModel.h"

class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QOpenGLWidget;
class QTabletEvent;
class QTouchEvent;
class QWheelEvent;

struct ImDrawCmd;
struct ImDrawList;
struct ImFont;
struct ImGuiContext;

namespace core {
class Project;
class TimeLineEvent;
}

namespace theme {
class Graph;
}

namespace gui {

// What the drawing layer reads off the editor: the STICKY FOCUS and the SELECTION,
// passed in rather than read here, so the drawing stays a pure function of its inputs
// (and a headless pixel gate can hand over a synthetic one).
//
// The two answer different questions and are not interchangeable:
//   * EMPHASIS is the SELECTION's (selectedKeys): a curve whose channel owns a
//     selected key is drawn at full strength and every other curve at 45%, with an
//     empty selection dimming every curve - the design's decision table, "Curve
//     emphasis". The sticky focus used to dim, and it no longer does.
//   * EACH CURVE'S TANGENT HANDLES are still the FOCUS's (node/channel/valid): the
//     panel draws them for one channel at a time because they cost ~52 of the ~68
//     vertices a key spends (the measured rule in ImGuiOverlay.cpp), and the editor's
//     hit-test focuses a channel on a handle press.
struct PanelFocus {
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    bool valid = false;

    // One key the editor's selection carries, in the currency the drawing needs:
    // the key TYPE names the curve it belongs to and the pixel X is where that
    // key's MARKER is centred. PanelCurve::keyDots publishes those centres (and
    // PanelCurve::keyFrames their frames), and ctrl::graph::keyDot is the same
    // mapping the editor's hit-test measures from, so the pixel X is the whole
    // identity a key marker can be matched by - both to ring it (drawPanel) and to
    // decide whose curve is at full strength (curveStrength). The caller maps the
    // selection through the editor's own scaler, with the model's panel.
    struct SelectedKey {
        core::TimeKeyType type = core::TimeKeyType_TERM;
        int x = 0;
    };
    QVector<SelectedKey> selectedKeys;
};

// Draws the panel into aDrawList: the short ticks (in the numbers' own token, each
// frame mark at the tier's alpha the model publishes for it - one ink for the ruler,
// the timeline's rule), the labels with their
// halo, the zero rule, the curves (clipped to the plot; the channels that own a
// selected key at full colour and every other curve dimmed - see PanelFocus), the key
// markers (a filled
// dot of radius ctrl::graph::kKeyDotRadius about each key's centre, ringed in
// theme::Graph::keyEdge()), every segment's tangent handles (a stored pair filled, an
// Auto segment's derived pair hollow; the grabbed one highlighted from io.MousePos),
// the playhead, the box-select marquee, the selection HULL and its count label, the
// name CHIP of every curve in play, the panel's one control (the channel button), and
// the empty state. It keeps no state - which
// is what lets the overlay call it per frame and a headless pixel gate call it
// once. It reads no font off the current context (the draw list carries its own),
// but the handle highlight and the hovered-curve chip DO read
// ImGui::GetIO().MousePos, so a caller must own a context: the overlay's frame and
// the headless pixel gate both do.
//
// It draws the MODEL and nothing else: every rect it uses comes out of aModel
// (or is derived from the draw list's own font, for the chips' text boxes and the
// hull's count label, whose models are rects the drawing layer puts glyphs in). No
// background and no grid are drawn - the model publishes none, and the panel
// must leave the artwork showing through (a plot area with nothing in it is
// therefore untouched by this function).
void drawPanel(
    ImDrawList& aDrawList, const GraphPanelModel& aModel, const theme::Graph& aTheme, const PanelFocus& aFocus
);

// Whether drawPanel draws the LABEL HALO: the same glyphs 16 more times, at the
// eight compass offsets of radius 1 and radius 2, in theme::Graph::halo().
// SHIPPED OFF. The halo is unconditional - it prints an opaque panel-toned smear
// around every label, including the labels sitting on artwork that never needed
// one - and it costs 17 glyph passes for a single line of text. The contrast
// outline pass below rings the same ink, glyphs included, but only where that ink
// would otherwise have been lost against what is behind it, and in the outline
// colour rather than a panel tone. The halo's code is kept and this flag is the
// only thing that switches it off, so the two can still be compared.
constexpr bool kDrawTextHalo = false;

// The gutter a VALUE label keeps from the right edge of its box, when the box has
// that much room to spare (see drawLabel's right-alignment): the top value label and
// the plot's first frame number otherwise land 7px apart on the same rows and read as
// the single string "10" - measured in the design-review capture (the value label '1'
// at x 208..215 and the frame label '0' at x 223..231 of a 1440x900 shot).
//
// WHY 8. The timeline answers this exact defect with its own margin:
// src/gui/TimeRuler.cpp's kNumberMargin = 8 ("Number labels never come closer than
// this gap: without a margin, adjacent numbers ... read as one"). The panel takes the
// application's own number for the same problem, and 8px on top of the model's own
// ctrl::graph::Scaler::kAxisLabelPad (4px) and the frame number's inset leaves the two
// numbers ~15px apart - more than two 7px digits of clear gutter.
//
// IT IS A MAXIMUM, NOT A SHIFT: the pull is limited to the box's spare width, so a
// label that fills its box keeps the position the model's measured column gave it and
// the gutter can never push a number out of the column.
//
// Declared here rather than in the .cpp because tests/graph_panel_pixels.cpp asserts
// the drawn gutter against THIS number, so the bar and the drawing cannot drift (the
// same reason kDrawTextHalo lives here).
constexpr int kValueLabelGap = 8;

// The contrast outline and composite: ONE full-screen pass that rings panel ink
// whose own colour is too close to the artwork underneath it, and then composites
// the panel over that artwork.
//
// WHY IT IS A FREE UNIT AND NOT AN ImGuiOverlay MEMBER: the same reason drawPanel
// is one - a headless pixel gate has to be able to drive it with synthetic
// textures and no overlay instance, a real context and the real framebuffer.
//
// WHAT IT READS. Two textures of the same pixel size:
//   * the overlay: the panel rendered into its OWN framebuffer, where rgb is the
//     panel's ink and alpha is its coverage. That render is the ImGui backend's
//     own source-over blend into a framebuffer cleared to (0,0,0,0), so the rgb
//     the pass reads is PREMULTIPLIED by the coverage (the backend uses
//     glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, ...),
//     which leaves dst.rgb = src.rgb * src.a). The pass divides the coverage back
//     out before it does anything else, so its own formulas are written in the
//     panel's colours. Without that step a partially covered pixel would composite
//     at alpha SQUARED - the artefact the "do not gate at alpha >= 1" rule exists
//     to avoid would be replaced by a different one on every AA fringe.
//   * the backdrop: the pixels the panel would sit on if it were not there. It is
//     a GPU copy of the widget's framebuffer (copyBackdrop, a glBlitFramebuffer
//     resolve, never a readback), so the pass can ask what the artwork under a
//     label actually is.
//
// THE RING IS BUILT FROM TAPS, NOT FROM THE PIXEL'S OWN COVERAGE. The four taps
// one pixel away from the ink carry the ring: a 40%-covered stroke gets a 40%
// ring, two adjacent strokes fill in without double-darkening, and nothing is
// drawn where no ink is nearby. The contrast test compares each TAP's own colour
// against the backdrop at THAT pixel, so a glyph straddling a light/dark boundary
// is ringed only on the side that needs it.
namespace outline {

// The ratio below which the ring is drawn. The shader rings ink whose contrast
// against the backdrop is BELOW this number, so a HIGHER constant rings MORE: it
// is the "the ink would otherwise have been lost" line, not a target for the ink.
//
// WHY 2.0 AND NOT 3.0 (the value this shipped with). 3.0 is WCAG's minimum for
// large text and UI components, and read as a ring TRIGGER it is far too eager:
// the panel's ink is theme::Colors::text, and in linear light white ink reaches
// 3:1 against any backdrop darker than grey 149/255 - i.e. the ring was drawn over
// almost every mid-tone in the artwork, which is the "the ring shows up
// everywhere" report. 2.0 draws it only where the ink is genuinely near-invisible:
// white ink rings over a backdrop brighter than grey 183/255, and every darker
// backdrop is left alone. The relation is measured in the gate
// (tests/graph_panel_pixels.cpp, the outline block): a grey-130 stroke over the
// bright 200 patch is 2.30:1, untouched at 2.0 and ringed at 3.0 - the pair that
// pins this number rather than merely accepting it.
//
// The ratio is only that number in LINEAR light, so luma() linearises the sRGB
// channels first (see the fragment shader).
constexpr float kMinContrast = 2.0f;

// The shipped ring: the four taps one pixel away. The alternative measured here is
// 2, which adds a second ring at +/-2px and takes maxA over all eight taps - the
// extension for a ring that reads too thin next to 13px labels. It is a parameter
// rather than a compile-time constant so the two can be compared without a second
// build; the pass pays for the extra four taps only when it is set.
constexpr float kRingRadiusPx = 1.0f;

struct Params {
    float minContrast = kMinContrast;
    float ringRadiusPx = kRingRadiusPx;
    QColor colour = QColor(0, 0, 0); // theme::Graph::outline() in the shipping path
};

// Builds the pass's program and its full-screen quad on the CURRENT GL context.
// Idempotent (safe to call per frame) and false when the context cannot compile
// it - the caller must then draw without it rather than draw nothing.
bool init();
// Releases the program and the quad, with the context current.
void shutdown();

// Copies the framebuffer aSourceFbo (aPixelSize pixels) into aDestinationFbo with
// glBlitFramebuffer: a GPU copy, never a readback. The widget's own framebuffer is
// 4x multisampled (MainWindow's QSurfaceFormat), and a blit from it RESOLVES it -
// which is why the filter is GL_NEAREST, the only filter a multisample resolve
// accepts. Both framebuffers must have the same internal format (GL_RGBA8 here)
// and the same size.
void copyBackdrop(GLuint aSourceFbo, GLuint aDestinationFbo, const QSize& aPixelSize);

// Runs the pass into the CURRENTLY BOUND framebuffer, which holds aPixelSize
// pixels. The overlay (unit 0) and the backdrop (unit 1) are sampled, and the pass
// sets every piece of GL state it depends on: this is the last thing in the frame,
// and the ImGui backend leaves blending, the scissor box and the viewport as its
// own last draw left them.
void composite(GLuint aOverlay, GLuint aBackdrop, const QSize& aPixelSize, const Params& aParams);

} // namespace outline

// The CURVE INK: the channel polylines, stroked by a coverage pass of our own
// instead of by the draw list's own ImDrawList::AddPolyline - and the tangent HANDLE STEMS,
// which are 1px lines with fractional ends and the one 1px family in the panel whose ink can
// sit off the line it means.
//
// WHY NOT AddPolyline. ImGui antialiases a polyline by building the fringe into the
// GEOMETRY: each segment is a quad whose edge vertices carry a partial alpha, so how
// thick the stroke looks is a property of its vertices - and a horizontal run and a
// steep diagonal of the same nominal width then disagree. This pass measures the
// coverage PER FRAGMENT, from the distance to the segment's own centreline, so the
// width is a property of the SHAPE. It is the technique of Chan & Durand, "Rendering
// Antialiased Lines on the GPU" (2005) and the treatment in Rougier, "Shader-Based
// Antialiased, Dashed, Stroked Polylines", JCGT 2(2) 2013 (whose body distance is the
// absolute perpendicular coordinate and whose filter radius is a parameter) - the same
// family Vello/piet-gpu draw strokes with.
//
// WHY THE STEMS ARE HERE TOO. A handle stem is one pixel wide and its ends are fractional
// (PanelHandle::base/dot are QPointF), and ImGui's AddLine shifts its geometry by (+0.5,+0.5)
// (the 1px rasterization convention in imgui_draw.cpp) - which is a pure translation, but the
// coverage that matters is measured PERPENDICULAR to the line, so on a stem at a diagonal the
// ink lands up to 0.71px off the line the model asked for (the (0.5,0.5) shift's component
// along the stem's own normal is 0.699 for the gate's fixture). MEASURED on that fixture, at
// three stations along it, through one method for both strokes: +0.68, +0.75 and +0.76px for
// AddLine, against -0.01, -0.00 and +0.01px for this pass. The pass measures the
// distance to the segment that was asked for, so the offset goes away; the stem is already
// antialiased today (drawPanel does NOT turn the draw list's line AA off for it), so its
// crispness is unchanged - only its position.
//
// The integer-positioned chrome (the frame and value ticks, the zero rule and the playhead) does
// NOT come through here, and that is a measured decision rather than an omission: those three
// are drawn with the line AA explicitly OFF (the timeline's hairline rule), every one of them is
// positioned by an INTEGER column, and at an integer column the hard-edged line is exact - one
// column at full coverage, on the column the model named. A 1px stroke through this pass on a
// pixel boundary puts half its ink in each of two columns, i.e. it would trade one crisp column
// for two grey ones. tests/graph_panel_pixels.cpp pins both halves of that (its assertion 27),
// so the decision fails loudly if someone makes it by accident.
//
// HOW IT DRAWS. One quad per segment, expanded on every side to
// halfWidth + filter, so a segment's capsule already has its round join and cap and
// no join geometry is needed; each vertex carries the segment's own two endpoints.
// The fragment computes the distance to the SEGMENT (never to its infinite line) and
// turns it into a coverage with a one-device-pixel box filter, so the 50% crossing
// sits exactly on the nominal edge. Below one device pixel the stroke keeps the 1px
// footprint with its alpha scaled down rather than vanishing.
//
// NO DOUBLE-DARKENING AT THE JOINS. Neighbouring segments' quads overlap by the
// filter's own width, and compositing them one after another would blend the overlap
// twice - which is worse than it sounds, because a dimmed curve is drawn at 45%
// alpha and even a FULLY covered overlap composites darker than the stroke's own body.
// So the coverage is accumulated with glBlendEquation(GL_MAX) into a scratch texture
// (the union of the capsules, which IS the round-joined stroke) and composited once
// into the frame with the ImGui backend's own source-over function, in the
// premultiplied convention the contrast outline pass documents.
//
// WHERE IT SITS IN THE FRAME. The curves belong between the ticks and the key
// markers, and the ImGui backend renders a whole draw list in one call, so the pass is
// inserted INTO the draw list: drawPanel puts one ImDrawList::AddCallback per curve,
// carrying that curve's polyline, colour and strength in the callback's own COPIED
// data, and the callback draws into whichever framebuffer is bound (the overlay's
// coverage framebuffer in the application, the harness's own in the pixel gate) using
// the backend's own ortho matrix - so a curve lands on exactly the pixels AddPolyline
// would have inked.
//
// IT IS A GUEST IN THE FRAME and puts back every binding it changes: the ImGui backend
// keeps its program, its vertex array and its array buffer bound across commands and
// uploads the next draw list's vertices with them.
//
// WITHOUT init() - a context that refuses the programs, or a caller that never built
// the pass - drawPanel keeps the old AddPolyline path: a panel drawn with the fringe
// stroke beats a panel with no curves at all. The pixel gate drives both, which is
// what makes the two stroke geometries comparable in one run.
namespace stroke {

// The stroke's nominal width in LOGICAL pixels - gui::kCurveWidth, the number both
// paths stroke with. Declared here because tests/graph_panel_pixels.cpp asserts the
// measured width against it, the same reason gui::kValueLabelGap lives in the header.
constexpr float kWidthPx = 2.0f;

// The coverage filter's width in DEVICE pixels: the box filter the fragment turns the
// distance to the centreline into a coverage with. Rougier's r, and 1 is both the
// paper's usual value and this shipment, so the ramp spans the pixel either side of the
// nominal edge. Read only by the pass; named here so the gate can say what it measured
// against.
constexpr float kFilterPx = 1.0f;

// Builds the pass's programs, its vertex array and its coverage scratch on the CURRENT
// GL context. Idempotent (safe to call per frame) and false when the context cannot
// compile the programs - drawPanel then strokes with ImDrawList::AddPolyline.
bool init();
// Releases the programs, the vertex array and the scratch, with a context current.
void shutdown();
// Whether init() has built the pass, i.e. whether drawPanel will insert it. The gate
// uses this to render the SAME model through both stroke geometries in one run.
bool isReady();

// The draw-list callback drawPanel inserts where a curve's ink belongs (an
// ImDrawCallback, one per curve). Public for the same reason drawPanel and
// outline::composite are: the pixel gate has to be able to drive it directly, which is
// how it is checked for leaving the frame's GL state exactly as it found it. It draws
// only what the ImDrawCmd it is handed carries - the curve's polyline, colour and
// strength, in the callback data ImGui copied for it - and it reads the projection
// from the frame's own ImDrawData, so it must be called while that frame is current.
// It returns without drawing when there is no frame to project against.
void drawListCallback(const ImDrawList* aDrawList, const ImDrawCmd* aCommand);

} // namespace stroke

// The shipping viewport overlay: the animation graph panel drawn with Dear ImGui
// in the widget's own GL pass.
//
// WHAT IT OWNS. One ImGui context, the GL3 renderer backend on the widget's own
// context, the font atlas built through the Qt/QRawFont loader (so CJK, `.ttc`
// and variable faces rasterize with the face the rest of the application draws
// with), one ImGuiInputBridge (Qt events -> ImGuiIO), the accessibility publish
// (gui/ImGuiA11y.h) and the interaction state (the model of the last drawn
// frame, so a press can be hit-tested against the geometry that was painted).
//
// ONE FRAME. paint() runs at the very end of MainDisplayWidget::paintEvent, after
// ctrl::PainterHandle::end(), with the widget's GL context current and its
// framebuffer bound - the order is load-bearing: the painter pass flushes the
// queued QPainter command batch, so drawing before it would put the panel UNDER
// the tools' overlay. It sets io.DisplaySize / io.DisplayFramebufferScale /
// io.DeltaTime, then calls the backend's NewFrame() and only then
// ImGui::NewFrame() (in 1.92 the Renderer_NewFrame call is the caller's duty - it
// is what creates the backend's device objects on the first frame, and skipping
// it makes the first RenderDrawData() draw with every handle 0). After the draw
// it reads back what the backend did NOT restore (the viewport and the scissor
// box are left as the backend set them, and a temporary VAO is created per draw);
// the application re-sets its own viewport and render state at the top of
// paintGL, so nothing here has to restore them.
//
// While the panel is drawn, the ImGui frame does NOT go straight into the
// widget's framebuffer: it goes into the pass's own coverage framebuffer, and the
// last thing the frame does is the contrast outline and composite pass (see
// gui::outline below) - one backdrop copy and one full-screen pass per frame.
// With the panel hidden, or with the driver refusing the pass or its targets, the
// draw goes into the widget's framebuffer exactly as it did before the pass
// existed.
//
// It runs whether or not the graph is visible - a hidden panel emits no draw
// command at all, but the frame still drains ImGuiIO's input queue, which would
// otherwise grow for as long as the widget keeps forwarding events.
//
// INTERACTION. The widget asks this class BEFORE it routes a press to the
// viewport tools (see press()) and for a double-click on the plot (see
// doubleClick()), and keeps forwarding the cursor itself: the plot's drag, the
// key move/insert and the wheel still go through ctrl::Driver to
// ctrl::GraphEditor, exactly as they did when the panel was painted with
// QPainter.
//
// APPEARANCE (the decisions GraphPanelModel deliberately does not make):
//   * contrast outline. The pass below rings panel ink whose own colour is too
//     close to the artwork behind it, in theme::Graph::outline(). It is why the
//     halo is off: it protects the same ink (glyphs included) where the ink would
//     otherwise be lost, rather than everywhere.
//   * label halo. TURNED OFF at the moment (kDrawTextHalo, below); while it is on,
//     every label is drawn once more in theme::Graph::halo(), at the eight compass
//     offsets of radius 1 and radius 2 (16 extra glyph passes, i.e. 17 glyph
//     passes per line), so the ring reaches 2px past the glyph ink in every
//     direction and the glyphs are then drawn on top in their own colour. The
//     panel has no background, so the halo was the only thing that kept a label
//     readable over arbitrary artwork, at the cost of an opaque panel-toned ring
//     around every label - including over artwork that needed none - which is what
//     the outline pass replaced. The code is kept so the two can be compared.
//   * curve stroke. kCurveWidth (2.0) - the panel's own width - drawn by gui::stroke
//     below: one quad per segment, the coverage measured per fragment from the distance
//     to the segment's own centreline, the segments' capsules unified with GL_MAX so a
//     join cannot be blended twice. With that pass unavailable (a context that refuses
//     its programs) the stroke falls back to ImDrawList::AddPolyline, where the whole
//     number still matters for a different reason: it is what puts ImGui on its
//     texture-AA path (one quad strip, 2 vertices per point) instead of its per-segment
//     geometry path (4 per point). See kCurveWidth's own comment and gui::stroke.
//   * key markers. A key is a DOT - a circle of radius ctrl::graph::kKeyDotRadius
//     (3.75px, the timeline's own key-body radius) at PanelCurve::keyDots, FILLED
//     WITH ITS CHANNEL'S COLOUR (PanelCurve::color ==
//     theme::Graph::keyColor(channel.type()), the colour its curve is stroked in
//     and its chip's text drawn in) inside theme::Graph::keyEdge() - a
//     floor-toned ring, which is what keeps a key visible on top of the curve that
//     carries it. The editor's selection turns both into theme::Graph::selected().
//     The marker is deliberately NOT the size of the pick area: the editor grabs a
//     key within ctrl::graph::kKeyHitRadius (10px) of its centre and the model's
//     `keys` rect is that rule's square form, so the ink is small and the target is
//     generous - the timeline's own rule for a small element.
//   * the box-select marquee. PanelCurve... see GraphPanelModel::marquee: a rounded
//     (6px) body in theme::Graph::marqueeFill() under a 1px theme::Graph::marqueeEdge()
//     edge - the timeline's own box-select colours (ctrl/time/time_Renderer.cpp), so
//     the gesture reads the same in both views. ImGui's draw list has no dashed
//     stroke, so the edge is solid where the timeline's is dashed.
//   * emphasis. A curve whose channel owns at least one SELECTED key (and its key
//     markers with it) is drawn at full colour; every other curve at 45% alpha, and
//     with an EMPTY selection every curve is dimmed - the selection decides, not the
//     sticky focus (curveStrength in ImGuiOverlay.cpp, and the design's decision
//     table, "Curve emphasis").
//   * tangent handles. Each PanelHandle is a stem from its base to its dot, then the
//     dot itself. WHICH ink depends on PanelHandle::ghost, and that is the document's
//     distinction, not a style: a Custom segment's pair is STORED (Curve::handle) and
//     is drawn FILLED (theme::Graph::handleFill() inside theme::Graph::handleEdge(),
//     the stem in theme::Graph::handleLine(), as before); an Auto segment's pair is
//     DERIVED (Curve::ghostHandles) and is drawn HOLLOW - no fill, a smaller dot
//     (kGhostHandleShrink less than the stored radius), stem and ring in the channel's
//     own hue (PanelCurve::color) at the reference's reduced alphas - because a drag
//     on it would CONVERT the key to Custom, and a filled dot would claim the document
//     stores a tangent it does not. The dot NEAREST the current pointer, within
//     ctrl::graph::kHandleHitRadius, is drawn in theme::Graph::selected() instead -
//     ghost or not, and the same distance the editor's hit-test uses, so the highlight
//     names exactly the handle a grab would take. The handles are clipped to the
//     plot like the rest of the data, so one that leaves the plot shows its stem
//     cut at the edge rather than painting over the panel's chrome.
//   * the selection HULL. GraphPanelModel::selectionHull is a polygon in panel
//     pixels; a 1px closed outline in theme::Graph::marqueeEdge() - the panel's
//     selection accent, the same ink the marquee's edge uses - inside the plot clip,
//     and NOTHING is drawn while the hull is empty. Its count is the model's
//     selectionLabel ("3 keys"), drawn in theme::Graph::axisText() in the box the
//     model measured.
//   * the name CHIP of a curve in play. A chip is drawn for a curve whose channel
//     owns a selected key (curveStrength == 1) and for whatever curve the pointer is
//     over, and for NOTHING else: that subset is what is being edited, and it is the
//     drawing layer's decision (emphasis), exactly as the curve dimming is. The chip
//     is the model's `chip` box (empty when it would not fit the plot: then nothing
//     is drawn) with its `chipText` right-aligned in it, the text in the channel's
//     hue (curve.color, at full strength for a selected channel and 0.75 for one the
//     pointer is merely over) over its own backing - theme::Graph::halo() at
//     kChipBackdropAlpha, an opaque panel tone at 0.82, so the name stays readable
//     wherever the curve leaves the window. Chips that would overlap are pushed
//     apart vertically (the reference's own de-collide) and clamped into the plot.
//   * the panel's ONE control. GraphPanelModel::channelsButton is the box the
//     "N channels" button is drawn in, with channelsButtonText as its label, filled
//     with theme::Graph::halo() at kChannelsButtonAlpha (the reference floats the
//     same translucent panel tone under it) and labelled in
//     theme::Graph::axisText(). An EMPTY box means the button did not fit the plot:
//     nothing is drawn for it and nothing is hit-tested (see press()).
//   * a chip box that is EMPTY (GraphPanelModel's CHIPS) is skipped whole: nothing is
//     drawn for it and no geometry is derived from it. That is not a detail:
//     QRect().right() is -1 and its centre is (0,0), so a box derived from an empty
//     rect would land at the panel's top-left corner. The same rule holds for the
//     channel button's and the selection label's empty rects.
//
// Strings: every user-visible string this class creates goes through
// QCoreApplication::translate with the "gui::ImGuiOverlay" context (the class is
// not a QObject, so there is no tr() member), and the panel's own reasons for
// being empty come from the model, which states them as plain English literals.
// THE KEY ROUTE'S STRINGS ARE NOT THIS CLASS'S: the control that route opens is the
// TIMELINE's own easing picker, and the easing NAME this class prints for a
// selected key comes from the timeline's table (util::Easing::getTypeName through
// the "EasingName" context), so a translator has one set of easing names for the
// whole application and this file cannot drift from it. The one string this class
// adds for the route is the accessible ACTION that opens the picker (see
// publishAccessibility).
//
// THE KEY ROUTE OPENS THE APPLICATION'S OWN EASING PICKER. A right-click within
// ctrl::graph::kKeyHitRadius of a key's CENTRE selects that key exactly as a press
// on it would and then opens the TIMELINE's one picker through
// gui::MainDisplayWidget::showGraphKeyMenu (gui::TimeLineWidget::showKeyEasingPicker)
// - the same popover the timeline header's easing chip opens, and the single
// instance TimeLineEditorWidget owns - so there is no second list of easings, no
// second wording and no second edit path to keep in step. Nothing else on the panel
// is a right-click target: a right-click that hits no key opens nothing and consumes
// nothing (the graph itself takes no right-button action - it is ctrl::Driver's
// cursor test that decides whether a press is the panel's at all).
//
//   * THE HIT IS THE EDITOR'S OWN RULE, not the drawn shape and not the model's
//     key RECT. ctrl::GraphEditor::hitTest grabs a key within
//     ctrl::graph::kKeyHitRadius (10px) of its centre, and this asks the same
//     question of the same centre: PanelCurve::keyDots, which is
//     ctrl::graph::keyDot through the model's own scaler. The rect form
//     (PanelCurve::keys, a 10x10 square) is HIT geometry for a marquee, and the
//     marker is drawn at kKeyDotRadius (3.75px), so neither is the rule here - the
//     generous circle around the centre is, and it is the same one a left press
//     grabs through.
//   * the frame the hit names comes from PanelCurve::keyFrames, published beside
//     the dots for exactly this: (node, channel type, frame) is the timeline's own
//     name for a key.
//   * the right-click also makes that key the panel's SELECTION, which is the
//     timeline's own behaviour: its key gesture calls selectKeysAt() with the
//     right-click point and then acts on the result. So the picker's effect and the
//     selection agree: the open acts on the selection, and the key it was opened on
//     is in it. The selection is not restored on dismissal, exactly as the timeline
//     leaves the key it lent its selection to.
//   * the edit the picker commits is written by the TIMELINE itself, on the
//     timeline's notifier and event currency (TimeLineEditorWidget::assignEasingToTargets),
//     so the panel's route and the timeline's chip cannot disagree about the
//     document. The picker is a Qt popup (a native window) that holds the keyboard
//     while it is up, Escape closing it, exactly as the QMenu it replaced did:
//     nothing here gates ImGui's navigation, takes the Qt focus, or claims a key.
//
// WHERE IT IS OPENED FROM. The widget forwards every button to mouseButton(), which
// is the only place this class sees a right press; press() is the LEFT press path
// and the widget only offers it the left button. The picker is a native window the
// TIMELINE owns (shown, never exec()ed: the caller is inside a press handler and a
// nested event loop there could re-enter the code asking for it), so the overlay
// does not draw it and cannot measure it.
//
// AND THE ACCESSIBILITY LAYER KEEPS ITS ROUTE TO IT. The picker is the timeline's
// own native window, so this class neither draws nor describes it, and what the tree
// says instead is the state it changes: the summary line names the selected key and
// the easing it carries (see publishAccessibility). On top of that, when the
// selection names exactly one key the class publishes ONE more action - a button on
// the key's own marker - and activating it calls the SAME route the pointer does
// (showGraphKeyMenu with the key's position in this widget), so the picker is
// reachable without a pointer. Its rect is the region the pointer's
// gesture works in (the key's centre grown by ctrl::graph::kKeyHitRadius), which is
// the same "a click where this item says it is does what this item does" rule the
// fit action follows.
//
// THE PANEL'S ONE CONTROL IS THE CHANNEL BUTTON, AND ITS LIST IS A QMENU. The
// panel has exactly one control - the "N channels" button the model lays out in the
// plot's top-right corner (GraphPanelModel's CHANNELS BUTTON) - and pressing it
// opens the application's channel list through the widget
// (gui::MainDisplayWidget::showGraphChannelMenu), which builds and pops a real QMenu
// from the rows the model published: one row per channel, grouped under its layer,
// the row's swatch its colour key (filled while the channel is shown, hollow while
// it is hidden), the row's activation its visibility switch and its highlight the
// channel's focus. The key route delegates the same way (showGraphKeyMenu, to the
// timeline's picker) for the same reasons: one implementation for the application,
// nothing drawn by this class, and no second list of channels to keep in step. A
// list of controls is what replaced the legend the panel used to paint in its
// bottom-left corner: the legend named every
// channel at equal weight and covered the value labels; now a name appears AT the
// curve it belongs to, and only for the channels in play (see drawPanel's chips),
// while the complete list - which is a control surface, not an annotation - lives
// behind the one button.
class ImGuiOverlay {
public:
    ImGuiOverlay();
    ~ImGuiOverlay();

    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;

    // Creates the context, configures ImGuiIO, initializes the GL3 backend and
    // builds the font atlas, on the widget's current context (call it from
    // initializeGL, with that context current). Returns false and leaves nothing
    // behind - no context, no backend, no atlas, no accessibility factory - when
    // the backend refuses. The atlas is built through
    // gui::imgui_qt::installQtFontLoader at the pixel size
    // gui::imgui_qt::panelFontPixelSize derives from the widget's font - the size
    // Qt RASTERIZES that face at, which is what the timeline's own numbers use (the
    // font's LINE height is 1.298x that and drew every panel numeral ~30% too
    // large); when the font states no size at all the panel falls back to
    // kFallbackFontPixelSize, and when the loader cannot be installed the overlay
    // still runs, on ImGui's own default font, because a panel drawn in the wrong
    // face beats no panel.
    bool init(QOpenGLWidget& aWidget);
    bool isReady() const { return mReady; }

    // One frame. aVisible is whether the panel is shown at all: with false it
    // draws nothing (and publishes an empty model, so the interaction and the
    // accessibility layer stay in step with what is on screen). aProject is part
    // of the caller's frame signature; the panel itself is built from the editor.
    void paint(QOpenGLWidget& aWidget, ctrl::GraphEditor* aEditor, core::Project* aProject, bool aVisible);

    // ---- Qt -> ImGuiIO, forwarded by the widget. ----
    // Each returns whether the overlay consumed the event (the bridge's answer);
    // the widget forwards EVERY event regardless, because ImGui derives its own
    // latched state from that stream.
    bool mouseMove(const QMouseEvent& aEvent);
    bool mouseButton(const QMouseEvent& aEvent);
    bool wheel(const QWheelEvent& aEvent);
    bool key(const QKeyEvent& aEvent);
    bool inputMethod(const QInputMethodEvent& aEvent);
    bool tablet(const QTabletEvent& aEvent);
    bool touch(const QTouchEvent& aEvent);
    void focus(bool aFocused);
    void dprChanged(qreal aDpr);

    // HOW MUCH OF THE VIEWPORT'S TOP ANOTHER WIDGET COVERS, in LOGICAL pixels: 0 when
    // nothing floats over the panel (the caller's own state, so it is set every frame
    // it can change - the project tab bar's visibility and height change without a
    // resize). The panel's own ink is what the inset moves: the ruler band (the tick
    // marks and the frame numbers, and a value label whose row would fall under it)
    // starts below it, so nothing of the panel is lost under the widget. What it does
    // NOT move is the panel's INTERACTIVE geometry - the plot, the curves, the keys,
    // the handles, the playhead and the marquee stay exactly where ctrl::GraphEditor
    // hit-tests them - because the drawn control has to be the clickable control; see
    // THE OCCLUDED TOP in GraphPanelModel.h, which is where the split is stated.
    void setTopInset(int aLogicalPixels);

    // The widget's inputMethodQuery() override forwards here; see
    // ImGuiInputBridge::inputMethodQuery for what Qt asks and why.
    QVariant inputMethodQuery(Qt::InputMethodQuery aQuery) const;
    // Whether a Dear ImGui text field currently wants text: the widget keeps
    // Qt::WA_InputMethodEnabled set exactly while this is true.
    bool imeWantsTextInput() const;

    // ---- the overlay's own interaction ----
    // A press on the panel's ONE control - the "N channels" button
    // (GraphPanelModel::channelsButton) - opens the application's channel list
    // through the widget (see showGraphChannelMenu). aPos is in widget-logical
    // pixels, the space the model's rects are in. Returns true when it acted (the
    // widget then must not route the press to the viewport tools, and repaints). An
    // empty button rect (no room for it) is not a control: the press falls through
    // to the viewport.
    bool press(ctrl::GraphEditor& aEditor, const QPoint& aPos);

    // A double-click on the plot: fits both axes. Returns true when it acted.
    bool doubleClick(ctrl::GraphEditor& aEditor, const QPoint& aPos);

private:
    // Tears the backend and the context down (with the widget's context made
    // current, when it is still usable). Called from the destructor and from
    // init() when the widget offers a NEW GL context - a second initializeGL - in
    // which case every object the backend created belongs to the old one.
    void shutdown();

    // Publishes the frame just drawn to Qt's accessibility layer: the panel's one
    // control (the channel button), one line of state, one fit action and - while
    // the selection names one key - the key route's own action (the one that opens
    // the timeline's easing picker).
    void publishAccessibility();
    // The route to the application's own easing picker, at aPos in widget-logical
    // pixels: the key under the pointer by the EDITOR's distance rule
    // (ctrl::graph::kKeyHitRadius about the key's centre, PanelCurve::keyDots),
    // selected exactly as a press on it would select it, then
    // gui::MainDisplayWidget::showGraphKeyMenu with the selection. Does nothing when
    // no key is that close or when the panel is hidden. Returns true when a key was
    // hit, i.e. when the right-click was this class's (the picker itself belongs to
    // the widget, which may have no timeline to open one from - see
    // showGraphKeyMenu).
    bool openKeyMenuAt(const QPoint& aPos);
    // The panel's ONE control: opens the application's channel list through the
    // widget (gui::MainDisplayWidget::showGraphChannelMenu) at aPos in
    // widget-logical pixels, from the rows the last frame's model published. Does
    // nothing when the model gave the button no box (no room for it in the plot) or
    // when the panel is hidden - an empty rect is not a control. Returns true when
    // the press landed on the button, i.e. when it is this class's rather than the
    // viewport's.
    bool openChannelMenuAt(const QPoint& aPos);
    // The drawn marker of the one key aTargets names, in widget-logical pixels -
    // the same (node, channel type, frame) triple looked up in the model's own
    // geometry that openKeyMenuAt() finds by distance. False when the selection
    // names no single key the model carries (a key outside the plot's window, or
    // on a hidden channel).
    bool selectedKeyDot(const core::TimeLineEvent& aTargets, QPoint& aDot) const;
    // Runs the action an accessible item advertises, i.e. the one the pointer
    // performs on the same control.
    void onAccessibilityAction(int aId);
    // Asks the widget for a repaint. The widget asks itself after its own
    // interaction; this is for the paths that fire outside an event handler.
    void requestRepaint();
    // The inset setTopInset() recorded, in the currency the model takes (logical
    // pixels). Read per frame, so a setter call between frames is picked up by the
    // next one.
    int topInset() const;

    struct State;
    QScopedPointer<State> mState;
    bool mReady;
};

} // namespace gui

#endif // GUI_IMGUIOVERLAY_H
