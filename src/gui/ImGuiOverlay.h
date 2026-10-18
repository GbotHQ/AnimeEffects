#ifndef GUI_IMGUIOVERLAY_H
#define GUI_IMGUIOVERLAY_H

#include <QColor>
#include <QElapsedTimer>
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

// The shared frame API's return type. Forward-declared rather than pulled in through
// imgui.h: the header is included by targets that do not carry ImGui at all (the graph
// tests stub the gui symbols), and a pointer return needs only the name.
struct ImDrawData;

// The palette the frame is drawn from is part of a host's frame state
// (gui::PanelFrameState below), so the token class itself is named here rather than
// forward-declared.
#include "gui/theme/Graph.h"

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

namespace gui {

// What the drawing layer reads off the editor: the STICKY FOCUS, and nothing else.
// Passed in rather than read here, so the drawing stays a pure function of its
// inputs (and a headless pixel gate can hand over a synthetic one).
//
// THE SELECTION IS NOT HERE, and that is deliberate: which marker is selected, and
// which marker's key has a selected sibling, are published per key by the MODEL
// (PanelCurve::keySelected / keySiblingSelected), straight from the editor's own
// two queries. A list of selected keys here would be a SECOND source of the same
// fact - one that had to be matched back to a marker by pixel x, and that could
// silently disagree with the flags the model just published.
//
// The one thing the focus still decides is EACH CURVE'S TANGENT HANDLES
// (node/channel/valid): the panel draws them for one channel at a time because they
// cost ~52 of the ~68 vertices a key spends (the measured rule in ImGuiOverlay.cpp),
// and the editor's hit-test focuses a channel on a handle press. The curve EMPHASIS
// is the selection's, and it comes from the model's marker flags.
struct PanelFocus {
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    bool valid = false;
};

// Draws the panel into aDrawList: the short ticks (in the numbers' own token, each
// frame mark at the tier's alpha the model publishes for it - one ink for the ruler,
// the timeline's rule), the labels with their
// halo, the zero rule, the curves (clipped to the plot; the channels that own a
// selected marker, or the SIBLING of one, at full colour and every other curve dimmed -
// see curveStrength), the key
// markers (a filled dot of radius ctrl::graph::kKeyDotRadius about each key's centre,
// ringed in theme::Graph::keyEdge(), in one of the THREE states the model classifies:
// a selected marker brightens its fill and takes a 1px ring clear of the marker, a
// marker whose key has a selected sibling keeps the faint ring, and the marker under
// the pointer brightens its fill alone - see the marker pass), every segment's tangent
// handles (a stored pair filled, an
// Auto segment's derived pair hollow; the grabbed one highlighted from io.MousePos),
// the playhead, the box-select marquee, the selection HULL and its count label, the
// name CHIP of every curve in play, the panel's one control (the channel button), and
// the empty state. It keeps no state - which
// is what lets the overlay call it per frame and a headless pixel gate call it
// once. It reads no font off the current context (the draw list carries its own),
// but the marker hover (gui::panelKeyAt), the handle highlight and the
// hovered-curve chip DO read
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

// How far past a key marker's own radius its SELECTION ring's centre sits, in pixels:
// the ring the design asks for, "a 1px ring 1px clear of the marker"
// (index.html:279-280 and :302-303, quoted in full in ImGuiOverlay.cpp's marker rule).
//
// WHY 2.6 AND NOT 1.5. A literal reading - the ring's stroke 1px wide, its inner edge
// 1px clear of the marker's own edge - puts the ring's centre at radius + 1.5, and at
// that radius the dark SLEEVE the reference draws under the highlight ink (mock-graph.js
// strokes 2.6px at this same radius, "the ring reads on any artwork") would reach
// 0.2px INSIDE the marker's edge and paint the clear gap black. The reference's own
// radius is `KEY_R + 2.6` (mock-graph.js:865-876) and it is taken as it stands: the
// highlight stroke is 1px as the design says, its centre is 2.6px out, and the whole
// band's inner edge - the sleeve's - then sits 1.3px clear of the marker, which is the
// design's "1px clear" at the scale it means (the marker's own radius, not a hairline).
//
// Declared here rather than in the .cpp because tests/graph_panel_pixels.cpp has to
// find the ring's pixels to read their ink, and a bar that moved would move every probe
// with it (the same reason kDrawTextHalo and kValueLabelGap live here).
constexpr float kKeyRingOffsetPx = 2.6f;

// The air between the pointer and the DRAG READOUT's first glyph (GraphPanelModel's
// PanelKeyDrag): the key move's live numbers are written at the pointer, and the glyphs
// start this far right and below it so the cursor itself never covers them.
//
// Declared here for the same reason as the two above: tests/graph_panel_pixels.cpp has to
// find the readout's glyphs to assert that a live drag paints them and a still panel
// paints nothing, and a gap that moved would move the probe with it.
constexpr int kReadoutGapPx = 12;

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

// ---------------------------------------------------------------------------
// ONE PANEL, TWO HOSTS: the frame driver below is what both of them run.
//
// The panel is drawn by two hosts: the viewport overlay (gui::ImGuiOverlay, over the
// artwork) and the dock's Graph tab (gui::GraphPanelWidget, on the dock's opaque
// floor). Both run the SAME frame - the io setup, the backend's NewFrame, the model
// (gui::buildGraphPanelModel), the ink (gui::drawPanel) and the accessibility
// publication - through this file, and they share ONE Dear ImGui context, ONE GL3
// backend and one font atlas. That sharing is what makes this file the home of the
// frame: there is no second context, no second atlas and no second copy of the
// frame's body to keep in step.
//
// WHY ONE CONTEXT IS ENOUGH, and exactly what the code relies on: Qt gives every
// QOpenGLWidget in one top-level window the same SHARABLE context group
// (qopenglwidget.html, "Context Sharing": "When multiple QOpenGLWidgets are added as
// children to the same top-level widget, their contexts will share with each other"
// and "all QOpenGLWidgets in the same window can access each other's sharable
// resources, like textures"), and the application asks for ONE format for the whole
// process (MainWindow's QSurfaceFormat::setDefaultFormat, the call that same page
// asks for: "prefer using QSurfaceFormat::setDefaultFormat() instead of
// setFormat()" - and the format is what makes contexts sharable at all). So the
// context object, the GL3 backend's program and buffers - created lazily by
// whichever host draws the first frame (ImGui_ImplOpenGL3_NewFrame) - and the atlas
// texture of whichever host builds the first frame are usable from the other host's
// context too. Nothing here creates a second context or a second backend, and
// nothing here ever RE-creates them: the objects are made once and used from both.
// acquirePanelContext() checks the one thing that can break this (a later host whose
// context is NOT in the first host's sharing group) and says so with a qWarning,
// because no fallback exists to take and the alternative is drawing with handles that
// mean nothing in that context.
//
// WHAT EACH HOST STILL OWNS ITSELF: the widget it draws into - each host sets
// io.DisplaySize and io.DisplayFramebufferScale from ITS OWN size and device pixel
// ratio before every frame, which is what puts the frame in its own viewport at its
// own ratio (the backend sets the GL viewport and the scissor boxes from
// draw_data->DisplaySize * FramebufferScale, i.e. in device pixels, per widget) -
// its own input bridge (gui::ImGuiInputBridge, one per widget), its own frame state
// below for its own hit-tests, and the framebuffer it renders the frame into.

// The state of ONE host, between its frames. Each host owns one and hands it to
// runPanelFrame() every frame.
struct PanelFrameState {
    // The frame the pixels were drawn from and the editor it was built from (null
    // while the panel is hidden). A host's OWN interactions - the channel button, a
    // double-click on the plot, the accessible items - are tested against THIS
    // geometry, so the clickable control is exactly the drawn control.
    GraphPanelModel model;
    ctrl::GraphEditor* editor = nullptr;
    // The palette that frame was built and drawn with: theme::Graph::from of the LIVE
    // theme::Colors::current(), derived once per frame here (the app changes palette at
    // runtime, so there is nothing to cache). It travels in the state because a host's
    // work AFTER the frame still needs it - the overlay's contrast composite reads its
    // outline() - and a second derivation would be the same work twice.
    theme::Graph palette;
    // How many LOGICAL pixels of this host's top another widget covers (0 for the dock
    // tab; the viewport feeds the project tab bar's height in). It moves the model's
    // ruler band and nothing else: see ImGuiOverlay::setTopInset.
    int topInset = 0;
    // io.DeltaTime, from the real interval between THIS host's frames.
    QElapsedTimer frameTimer;
    bool frameTimerStarted = false;
};

// The ONE Dear ImGui context, GL3 backend and font atlas, brought up on aHost's own
// GL context. The first host to acquire builds them and every later acquire only
// counts a holder - the contexts of one window share (see above), so there is
// nothing else to do. The LAST release tears them down (with the releasing host's
// context current) and releases the two GL passes that are context objects too
// (gui::stroke's programs and scratch, gui::outline's program and quad).
//
// The atlas is built from aHost's font (the face and the size Qt rasterizes the rest
// of the application with), so the FIRST host to acquire decides the atlas both hosts
// then draw with - which is what keeps the panel's text identical in the two places.
bool acquirePanelContext(QOpenGLWidget& aHost);
void releasePanelContext(QOpenGLWidget& aHost);
// Whether the context is up, i.e. whether a frame can be run at all.
bool panelContextReady();

// ONE FRAME of the panel, for aHost. This is the whole per-frame body: the io setup,
// the backend's NewFrame, ImGui::NewFrame (in that order - the backend's own NewFrame
// is the CALLER's duty in 1.92 and is what creates its device objects), the curve
// ink's pass, the model, drawPanel and ImGui::Render().
//
// aHostStyle is the host's own kind (gui::GraphPanelHost): it is what places the
// model's one control (the docked button sits in the ruler band) and what
// publishPanelAccessibility below reads to decide who owns the panel's single
// accessibility publication. aEditor is the panel's editor (ctrl::Driver::
// graphEditor()) and aVisible whether the panel is up at all: with false the frame
// draws nothing but still drains ImGuiIO's queue, and aState comes back with an empty
// model and a null editor.
//
// The frame's vertices are NOT drawn here. The returned draw data is rendered into
// whichever framebuffer the host owns - the docked host with
// ImGui_ImplOpenGL3_RenderDrawData directly, the overlay behind its contrast-outline
// composite, which draws the same draw data into its own framebuffer first. Null when
// no context is up, i.e. when the host must draw nothing.
ImDrawData* runPanelFrame(
    QOpenGLWidget& aHost, GraphPanelHost aHostStyle, ctrl::GraphEditor* aEditor, bool aVisible,
    PanelFrameState& aState
);

// THE PANEL'S ACCESSIBILITY PUBLICATION, in one place for both hosts.
//
// gui::a11y keeps ONE installation for the process (one factory, one widget, one
// published model), so the panel - one panel, two hosts - can be published exactly
// once, and two hosts calling sync() would fight for it: the second host's model
// would be published with rects in the FIRST host's widget coordinates. The rule
// here is therefore: the DOCKED host is the panel's publisher, and it says so from its
// constructor (declarePanelDockHost below); the viewport overlay never publishes once
// that has happened. It is not a matter of taste: a single installation means whichever
// widget is NOT the installed one gets Qt's ordinary interface cached the first time
// anything asks about it, so a hand-over between the two hosts has to happen ONCE, at a
// moment when neither widget can have been asked yet - and the only such moment is the
// docked host's construction. See declarePanelDockHost for the measured failure this
// rule replaces.
//
// A host that is not on screen publishes the EMPTY model - the panel is not on
// screen, and the tree says so rather than showing the last frame of a hidden tab.
// The installation stays with the docked host either way.
//
// aOnAction(aUser, aId) is the host's own handler for the ids below, called
// synchronously from the accessibility call that activated the item.
void publishPanelAccessibility(
    QOpenGLWidget& aHost, GraphPanelHost aHostStyle, PanelFrameState& aState,
    void (*aOnAction)(void* aUser, int aId), void* aUser
);

// The docked host ANNOUNCES ITSELF, from its CONSTRUCTOR and before anything can ask
// Qt for either widget's accessible interface.
//
// WHY A DECLARATION AND NOT A SIDE EFFECT OF THE FIRST FRAME. gui::a11y keeps ONE
// installation and its factory answers for ONE widget: installFactoryOnly() from a
// constructor is what makes the entry Qt creates for that widget ours, and every OTHER
// widget the factory is asked about falls through to Qt, which then caches that
// widget's ORDINARY interface - permanently (gui/ImGuiA11y.h, "WHERE TO CALL IT"; the
// same reason it says a host "relies on the constructor-time install still being in
// place, not on a re-install after uninstall()"). So an install for the viewport's
// widget at ANY time after the docked host's constructor ran ENDS the docked
// installation, and a query Qt or a client makes about the docked widget in that window
// leaves that widget with its own interface and no way back: the docked panel draws,
// synces, and nothing reaches the tree. Measured exactly that way: the docked page node
// came out as Qt's plain `unknown` node with no children while the panel was on screen.
//
// Declaring here makes the docked host the panel's publisher FOR GOOD, from the moment
// it exists, so the viewport overlay never installs at all (see
// publishPanelAccessibility) and the docked host's own install() on its first frame is
// a REPEAT install for the SAME widget - the constructor-time installation and the
// first frame are one installation, exactly as gui/ImGuiA11y.h describes for a single
// host.
//
// The construction order this rests on (verified): MainWindow builds the viewport's
// central widget first and the dock's TargetWidget second, so the surviving
// constructor-time installation is the docked one.
void declarePanelDockHost(QWidget& aHost);

// The panel's accessible item ids. No item is a row index (the channel list and the
// key menu are the application's own QMenus, not in the tree - see the class comment),
// so every id is a fixed negative, and both hosts dispatch on the same ones.
constexpr int kPanelSummaryItemId = -1;
constexpr int kPanelFitItemId = -2;
constexpr int kPanelKeyMenuItemId = -3;
constexpr int kPanelChannelsItemId = -4;

// The drawn marker of the one key aTargets names, in the model's own pixels: the
// (node, channel type, frame) triple - the timeline's own name for a key - looked up
// in PanelCurve::keyFrames/keyDots, which are published beside each other. False when
// the selection names no single key the model carries (a key outside the plot's
// window, or on a hidden channel). Both hosts need it: it is the rectangle of the key
// menu's accessible action and the position the picker opens at.
bool panelSelectedKeyDot(const GraphPanelModel& aModel, const core::TimeLineEvent& aTargets, QPoint& aDot);

// The key under aPos, by the EDITOR's own rule: the nearest key whose centre is within
// ctrl::graph::kKeyHitRadius of it - the same distance, and the same `<=`,
// ctrl::GraphEditor::hitTest grabs a key with - reported as the triple (node, channel,
// frame) the timeline names a key by. The centre is PanelCurve::keyDots, not the key's
// HIT rect and not the drawn marker: the rect is that rule's square form (too small at
// the diagonals), and the marker is the ink, deliberately smaller than the target.
//
// False when no key is that close, which is a right-click's whole answer: the panel's
// right button has one action (the easing picker on the key under the pointer) and
// opens nothing when there is no key. Both hosts ask it, so the key a right-click names
// is one rule in one place.
bool panelKeyAt(
    const GraphPanelModel& aModel, const QPoint& aPos, core::ObjectNode*& aNode,
    ctrl::graph::Channel& aChannel, core::Frame& aFrame
);

// The shipping viewport overlay: the animation graph panel drawn with Dear ImGui
// in the widget's own GL pass.
//
// WHAT IT OWNS. One ImGuiInputBridge (Qt events -> ImGuiIO), the accessibility
// publication (gui/ImGuiA11y.h) while it is the panel's publisher, the contrast
// outline pass's own targets (the backdrop copy and the panel's coverage
// framebuffer) and the interaction state (the model of the last drawn frame, so a
// press can be hit-tested against the geometry that was painted).
//
// WHAT IT SHARES. The ImGui context, the GL3 renderer backend, the font atlas built
// through the Qt/QRawFont loader (so CJK, `.ttc` and variable faces rasterize with
// the face the rest of the application draws with) and the frame's own body belong to
// the panel, not to this host: they are gui::PanelFrameState + gui::runPanelFrame +
// gui::acquirePanelContext below, and the dock's Graph tab (gui::GraphPanelWidget)
// runs the same ones. This class is the OVERLAY host of that pair - the widget, its
// bridge, its outline pass and its interaction - and nothing else.
//
// ONE FRAME. paint() runs at the very end of MainDisplayWidget::paintEvent, after
// ctrl::PainterHandle::end(), with the widget's GL context current and its
// framebuffer bound - the order is load-bearing: the painter pass flushes the
// queued QPainter command batch, so drawing before it would put the panel UNDER
// the tools' overlay. The frame itself (io.DisplaySize / io.DisplayFramebufferScale
// / io.DeltaTime, the backend's NewFrame() and only then ImGui::NewFrame() - see
// runPanelFrame) is the shared body; what this host adds after it is the composite
// below. After the draw it reads back what the backend did NOT restore (the
// viewport and the scissor box are left as the backend set them, and a temporary VAO
// is created per draw); the application re-sets its own viewport and render state at
// the top of paintGL, so nothing here has to restore them.
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
//     apart vertically (the reference's own de-collide), and a chip the push has
//     taken outside the plot is DROPPED for that frame - not clamped and not
//     drawn clipped: the reference's own rule is that a chip which does not fit
//     near an edge is not drawn at all (mock-graph.js's _curveLabels skips an item
//     whose y is within 6px of the plot's top or bottom edge, and it never moves
//     one back in), and the de-collide is the only place a chip can leave the plot
//     after the model has published it inside one (see CHIPS in
//     GraphPanelModel.h). The test is the model's own: plot.contains(box), the
//     predicate buildCurves publishes a box with.
//   * the panel's ONE control. GraphPanelModel::channelsButton is the box the
//     "N channels" button is drawn in, with channelsButtonText as its label, filled
//     with theme::Graph::halo() at kChannelsButtonAlpha (the reference floats the
//     same translucent panel tone under it) and labelled in
//     theme::Graph::axisText(). The backing is the OVERLAY's: with the model hosted
//     in the dock (GraphPanelHost::Dock) the button is plain text, because the dock's
//     opaque floor is what stands behind it (the reference's .gp-chbtn.hosted rule -
//     no backing). An EMPTY box means the button did not fit the plot:
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
// gui::publishPanelAccessibility).
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
// the easing it carries (see gui::publishPanelAccessibility). On top of that, when the
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

    // Brings the panel's shared context up on aWidget's own context and attaches this
    // host to it: gui::acquirePanelContext (the ONE ImGui context, GL3 backend and
    // font atlas, shared with the dock's Graph tab - see the frame driver at the top
    // of this header) plus this host's own ImGuiInputBridge. Call it from
    // initializeGL, with that context current. Returns false and leaves nothing behind
    // when the backend refuses. The atlas is built through
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
    // Drops this host: its outline targets (its covers framebuffer and its backdrop
    // copy - context objects, so the widget's context is made current while it is
    // still usable), its input bridge, and its hold on the shared context
    // (gui::releasePanelContext, which tears the context, the backend and the atlas
    // down when this was the last host). Called from the destructor and from init()
    // when the widget offers a NEW GL context - a second initializeGL - in which case
    // every object the context created belongs to the old one.
    void shutdown();

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
    // Runs the action an accessible item advertises, i.e. the one the pointer
    // performs on the same control. The thunk publishPanelAccessibility is handed,
    // and what its aId means (kPanelChannelsItemId, kPanelFitItemId,
    // kPanelKeyMenuItemId).
    void onAccessibilityAction(int aId);
    static void accessibilityAction(void* aUser, int aId);
    // Asks the widget for a repaint. The widget asks itself after its own
    // interaction; this is for the paths that fire outside an event handler.
    void requestRepaint();

    struct State;
    QScopedPointer<State> mState;
    bool mReady;
};

} // namespace gui

#endif // GUI_IMGUIOVERLAY_H
