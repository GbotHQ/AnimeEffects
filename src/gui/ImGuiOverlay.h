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

struct ImDrawList;
struct ImFont;
struct ImGuiContext;

namespace core {
class Project;
}

namespace theme {
class Graph;
}

namespace gui {

// Which channel the drawing layer emphasises: the editor's STICKY FOCUS, passed
// in rather than read here, so the drawing stays a pure function of its inputs
// (and a headless pixel gate can hand over a synthetic one).
struct PanelFocus {
    core::ObjectNode* node = nullptr;
    ctrl::graph::Channel channel;
    bool valid = false;

    // One key the editor's selection carries, in the currency the drawing needs:
    // the key TYPE names the curve it belongs to and the pixel X is where that
    // key's square is centred. PanelCurve::keys publishes key RECTS without their
    // frames, and ctrl::graph::keyRect centres a square exactly on the mapped
    // point, so this is the whole identity a key square can be matched by. The
    // caller maps the selection through the editor's own scaler, with the model's
    // panel.
    struct SelectedKey {
        core::TimeKeyType type = core::TimeKeyType_TERM;
        int x = 0;
    };
    QVector<SelectedKey> selectedKeys;
};

// Draws the panel into aDrawList: the short ticks, the labels with their halo,
// the zero rule, the curves (clipped to the plot), the key squares, the Custom
// tangent handles (the grabbed one highlighted from io.MousePos), the playhead,
// the legend swatches and labels, and the empty state. It keeps no state - which
// is what lets the overlay call it per frame and a headless pixel gate call it
// once. It reads no font off the current context (the draw list carries its own),
// but the handle highlight DOES read ImGui::GetIO().MousePos, so a caller must
// own a context: the overlay's frame and the headless pixel gate both do.
//
// It draws the MODEL and nothing else: every rect it uses comes out of aModel
// (or is derived from the draw list's own font, for the legend labels). No
// background and no grid are drawn - the model publishes none, and the panel
// must leave the artwork showing through (a plot area with nothing in it is
// therefore untouched by this function).
void drawPanel(
    ImDrawList& aDrawList, const GraphPanelModel& aModel, const theme::Graph& aTheme, const PanelFocus& aFocus
);

// The box a legend row's LABEL occupies, beside its swatch: the text's own advance
// wide, vertically centred on the swatch, measured with aFont at aFontSize.
//
// It is EMPTY when the row's swatch is empty (a row with no room: see the LEGEND
// note in GraphPanelModel.h). That guard is load-bearing rather than cosmetic -
// QRect().right() is -1 and a null rect's centre is (0,0), so a box derived from an
// empty swatch would land near the panel's top-left corner and become a control
// that is nowhere on screen.
//
// This is THE one geometry of a legend label: drawPanel draws the glyphs in it,
// ImGuiOverlay::press() hit-tests it, and the accessibility item carries it. It is
// exposed so a pixel gate can assert the no-room rule on the geometry itself.
QRect legendLabelRect(const PanelLegendRow& aRow, ImFont* aFont, float aFontSize);

// Derives the ImGui style the panel's one POPUP is drawn with - the key context
// menu (drawKeyContextMenu) - from the panel's theme tokens, and applies it to the
// current ImGui context's style. This is the ONE place where the application's
// palette meets ImGuiStyle:
//
//   * the surface, its 1px edge and its radius are theme::Graph::menuBody/Edge/
//     Rounding, i.e. the same rounded floater gui::AppStyle draws a QMenu as;
//   * the text, the disabled text, the row's hover pill, the text on that pill and
//     the separator line are the app's own menu tokens;
//   * the font is NOT set here: the popup draws with the context's current font,
//     which is the atlas built from the WIDGET's font through gui::imgui_qt
//     (ImGuiOverlay::init), i.e. the face and size the application's own menus use.
//
// It is a pure function of the theme, so the caller applies it again whenever the
// palette could have changed: ImGuiOverlay::paint calls it once per frame from
// theme::Graph::from(theme::Colors::current()), which is what makes a runtime accent
// or light/dark change reach the popup with no signal and no cached copy (the frame
// already re-derives the panel's colours the same way). tests/graph_panel_pixels.cpp
// pins the mapping itself.
//
// WHAT IT CANNOT EXPRESS, and what is approximated: ImGui's Selectable highlight is
// a square rect (no rounding knob), so the item pill is drawn by the menu itself and
// the style's three Header colours are switched off - except the nav cursor, which
// ImGui draws around the keyboard-focused entry and which is therefore the one square
// highlight left; and Qt's font-relative menu paddings (0.3em panel, 0.23em/1.3em
// item) have no ImGui equivalent, so the popup uses its own fixed paddings
// (theme::Graph::menuPadX/menuPadY/menuItemPadX).
void applyPanelStyle(const theme::Graph& aTheme);

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

// WCAG's minimum contrast ratio for large text and UI components - 3:1. The ratio
// is only that number in LINEAR light, so luma() linearises the sRGB channels
// first (see the fragment shader). This is a starting point, not a law: it is the
// one number that decides how eager the ring is.
constexpr float kMinContrast = 3.0f;

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
// FRAME TIMING, and the one thing a frame owes the NEXT one. ImGui does not
// necessarily apply everything it was handed in the frame that was drawn: a press
// and the release that follow one another are applied on TWO frames
// (io.ConfigInputTrickleEventQueue, ImGui's own default - the split is what keeps
// the pair from flattening into "no click"), and an ImGui Selectable fires on the
// RELEASE, i.e. on the second of those frames. So a frame that still holds input
// for ImGui ends by asking for its successor (imguiHasQueuedInput), which is a
// request no EVENT handler can make: a requestRepaint() from one is a
// QWidget::update() like the widget's own update() for a press this class
// consumed, and Qt coalesces everything requested while a paint is already pending
// into that one pending paint. Without it a single click on a menu entry stays in
// ImGui's queue until some unrelated repaint happens to run - the popup's own entry
// not responding to one click.
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
//   * curve stroke. kCurveWidth (2.0) with ImGui's antialiasing on - the width is a
//     whole number ON PURPOSE, because that is what puts the stroke on ImGui's
//     texture-AA path (one quad strip, 2 vertices per point) instead of its
//     per-segment geometry path (4 per point), and a stroke's opaque core is only
//     width-1 px wide on that path: 1.5px left a 0.5px core under a 1px ramp, the
//     least opaque stroke ImGui can draw. See the constant's own comment.
//   * key markers. A key square is FILLED WITH ITS CHANNEL'S COLOUR
//     (PanelCurve::color == theme::Graph::keyColor(channel.type()), the colour its
//     curve is stroked in and its legend swatch filled with) inside
//     theme::Graph::keyEdge() - a floor-toned ring, which is what keeps a key
//     visible on top of the curve that carries it. The editor's selection turns both
//     into theme::Graph::selected().
//   * the box-select marquee. PanelCurve... see GraphPanelModel::marquee: a rounded
//     (6px) body in theme::Graph::marqueeFill() under a 1px theme::Graph::marqueeEdge()
//     edge - the timeline's own box-select colours (ctrl/time/time_Renderer.cpp), so
//     the gesture reads the same in both views. ImGui's draw list has no dashed
//     stroke, so the edge is solid where the timeline's is dashed.
//   * emphasis. While a focus exists, the focused channel's curve and its key
//     squares are drawn at full colour and every other curve at 45% alpha
//     (kUnfocusedAlpha). With no focus every curve is full.
//   * tangent handles. Each PanelHandle is a stem (theme::Graph::handleLine())
//     from its base to its dot, then the dot itself (theme::Graph::handleFill()
//     inside theme::Graph::handleEdge()). The dot NEAREST the current pointer,
//     within ctrl::graph::kHandleHitRadius, is drawn in theme::Graph::selected()
//     instead - the same distance the editor's hit-test uses, so the highlight
//     names exactly the handle a grab would take. The handles are clipped to the
//     plot like the rest of the data, so one that leaves the plot shows its stem
//     cut at the edge rather than painting over the panel's chrome.
//   * legend text. A row's label is drawn beside its swatch - kLegendLabelGap
//     (6px) right of it, vertically centred on the swatch - inside a box whose
//     width is the text's own advance, and that same box is what the hit-test and
//     the accessibility rect use, so the clickable label is the drawn label.
//   * a row with NO ROOM (PanelLegendRow::swatch is empty, see the LEGEND note in
//     GraphPanelModel.h): NOTHING is drawn for it, nothing is hit-tested for it and
//     no accessibility item is published for it. That is not a detail: QRect().right()
//     is -1 and its centre is (0,0), so a label box derived from an empty swatch
//     would land at the panel's top-left corner - a label sliver nobody drew and a
//     hit box at a place no control is. The count of such rows is named in the
//     accessible summary instead, because a screen-reader user has no other way to
//     learn that a channel is controllable but unlisted.
//
// Strings: every user-visible string this class creates goes through
// QCoreApplication::translate with the "gui::ImGuiOverlay" context (the class is
// not a QObject, so there is no tr() member), and the panel's own reasons for
// being empty come from the model, which states them as plain English literals.
// Two deliberate exceptions, neither of them a second convention:
//   * the menu's EASING entries take their labels from util::Easing::getTypeName,
//     which resolves through the "EasingName" context - the same call, and
//     therefore the same strings and catalog entries, the timeline's own easing
//     menu uses (TimeLineEditorWidget builds its entries from that function too);
//   * the menu's "Delete key" entry is asked for in the TIMELINE's context
//     ("TimeLineEditorWidget"), because the string is the timeline's own word for
//     that action: one English literal, one catalog entry, one translation. A
//     second copy under this file's context would let the two menus drift apart
//     in a translation while looking identical in English.
// Inventing a second set of easing names or a second delete wording here is what
// those two avoid.
//
// THE KEY CONTEXT MENU. A right-click ON A KEY opens a popup at the pointer
// offering the timeline's easing choices and "Delete key"; nothing else on the
// panel is a right-click target, so a right-click that hits no key opens nothing
// and is consumed by nothing (the graph itself takes no right-button action - it
// is ctrl::Driver's cursor test that decides whether the press is the panel's at
// all).
//
//   * the key under the pointer is found the way the panel is HIT-TESTED and
//     DRAWN: the model's own key rectangles, which are ctrl::graph::keyRect
//     clipped to the plot. No rect is computed here, so a menu can never open on
//     a key the editor's hit-test (which reads the same function through the same
//     scaler) would not have found. The rects carry no frame, which is why the
//     model publishes PanelCurve::keyFrames beside them - the frame is what names
//     the key the menu acts on.
//   * the right-click also makes that key the panel's SELECTION, which is the
//     timeline's own behaviour: its key menu calls selectKeysAt() with the
//     right-click point and then acts on the result. So the menu's wording
//     ("key(s)") and its effect agree: the menu acts on the selection, and the
//     key it was opened on is in it. The selection is not restored on dismissal,
//     exactly as the timeline leaves the key it lent its selection to.
//   * the chosen easing goes through the editor's own action
//     (ctrl::GraphEditor::setSelectedKeysEasing): ONE undo step, on the
//     timeline's notifier and event currency, so the timeline's menu and this one
//     cannot disagree about the document.
//
// ImGui draws the popup, so - unlike the legend rows and the state line, which
// this class publishes itself - THE MENU IS NOT IN THE ACCESSIBLE TREE, and a
// screen reader cannot address an entry by name. That is consistent with the rest
// of the application (the repository's own QMenus, the object tree's row menu
// included, are not exposed either: measured in the application-level probe), so
// it is recorded here rather than worked around. What the accessibility layer
// says INSTEAD is the state the menu changes: the summary line names the selected
// key and the easing it carries (see publishAccessibility), so the one thing a
// screen-reader user cannot reach is still reported.
//
// THE MENU'S KEYBOARD, and why it is gated rather than simply enabled.
// io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard is what ImGui gates its own
// popup keyboard behaviour on: ImGui::NavUpdateCancelRequest closes the front popup
// on Escape, and the arrows move the popup's own entry highlight (its own rectangle,
// drawn by ImGui in theme colours) which Enter then activates - the same three things
// a QMenu gives the timeline's key menu. The flag is NOT set for the whole session,
// because this overlay covers the whole viewport: with it set, ImGui claims the
// keyboard for the panel (io.NavActive -> io.WantCaptureKeyboard), and the
// application's own key commands (gui::KeyCommandMap: ctrl+Z, Delete, ctrl+Right, ...)
// would stop reaching MainWindow for as long as the panel is on screen.
//
// So it is set EXACTLY while the key context menu is open, in ImGuiOverlay::paint
// immediately before ImGui::NewFrame, from the popup's last known open state - the
// one place a flag ImGui reads during NewFrame can be changed in time. That is a
// per-menu toggle rather than a per-frame one in effect: the flag is already past
// the point where it could affect the frame that opens the popup, and it is cleared
// on the first frame after the popup closes. While the menu is open the class also
// asks for the keyboard explicitly (ImGui::SetNextFrameWantCaptureKeyboard, every
// frame it is up), so the widget consumes every key the menu could act on rather
// than letting it reach the application's key map, and ImGuiOverlay::key asks for
// the repaint the popup's own keyboard answer needs - the same thing mouseMove and
// mouseButton already do for the pointer's.
//
// AND WHILE IT IS OPEN THE WIDGET TAKES THE QT FOCUS, without which none of that
// arrives: keys reach this class through MainDisplayWidget::keyPressEvent, and the
// widget carries QWidget's default Qt::NoFocus policy (it never sets one), so a
// press in the viewport does not make it the focus object and the keys go to
// whichever widget last had the focus. MEASURED with this class instrumented and
// its own application probe driving the menu: zero key events reached it in a whole
// run and the popup stayed up through Escape. The focus it takes is handed back
// when the menu closes (and only if this class still holds it), which is the
// hand-back the ImGui TEXT FIELD path below does not need because a field's focus
// is the user's own doing; taking it here is the same move that path makes.
//
// Net effect, and it is the contract the application-level probe checks: with the
// menu closed the overlay claims nothing and every application key command works
// exactly as it did before this menu existed; with the menu open Escape closes it
// without touching the document, the arrows move the highlight, Enter activates the
// highlighted entry, and no key of that sequence reaches the application.
//
// THE POPUP'S APPEARANCE IS THE APPLICATION'S OWN CONTEXT MENU'S, and it is derived
// rather than written: gui::applyPanelStyle copies the menu tokens
// (theme::Graph::menuBody/menuEdge/menuText/menuTextDisabled/menuHover/menuHoverText/
// menuSeparator and the radii and paddings beside them) into ImGui's style, and paint()
// re-applies it every frame from theme::Colors::current(), so a runtime accent or
// light/dark change reaches the popup with no signal. Qt's font-relative menu paddings
// (0.3em panel, 0.23em/1.3em item) have no ImGui equivalent and the popup's own
// paddings are used instead; ImGui's item highlight cannot be rounded, so the menu
// draws the app's 4px pill itself and only its KEYBOARD highlight (the nav cursor) is
// left as ImGui's square rect. Both are stated at applyPanelStyle.
//
// The popup's geometry is fixed rather than ImGui's default, and that is a
// contract this class keeps with its callers: zero ItemSpacing, a compact row
// height, and one row per entry, so the row under a given pixel is a function of
// the popup's top edge and its height alone. It exists because the popup is
// confined to the VIEWPORT - an ImGui window, not a separate native menu window
// like Qt's - so it has to fit in a small one; the entries are laid out in two
// columns for the same reason, and the popup is placed to fit rather than at the
// pointer when the pointer's row of the viewport cannot hold it.
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
    // gui::imgui_qt::installQtFontLoader at the widget font's pixel size; when
    // that loader cannot be installed the overlay still runs, on ImGui's own
    // default font, because a panel drawn in the wrong face beats no panel.
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
    // A press on a legend swatch toggles that channel's visibility, a press on a
    // legend label focuses that channel, and - while the KEY CONTEXT MENU is open
    // (see the class comment) - a press belongs to the menu, so this consumes it
    // and returns true without touching the graph. aPos is in widget-logical
    // pixels, the space the model's rects are in. Returns true when it acted (the
    // widget then must not route the press to the viewport tools, and repaints).
    bool press(ctrl::GraphEditor& aEditor, const QPoint& aPos);

    // A double-click on the plot: fits both axes. Returns true when it acted.
    bool doubleClick(ctrl::GraphEditor& aEditor, const QPoint& aPos);

private:
    // Tears the backend and the context down (with the widget's context made
    // current, when it is still usable). Called from the destructor and from
    // init() when the widget offers a NEW GL context - a second initializeGL - in
    // which case every object the backend created belongs to the old one.
    void shutdown();

    // Publishes the frame just drawn to Qt's accessibility layer: one item per
    // legend swatch, one line of state, one fit action.
    void publishAccessibility();
    // The key context menu, called from the frame with the model of THIS frame -
    // the geometry a right-click is tested against is therefore the geometry that
    // was just drawn, which is the panel's rule for every other control too (see
    // the class comment for what it opens on, what it offers and why its layout
    // is fixed).
    void drawKeyContextMenu(
        ctrl::GraphEditor& aEditor, const GraphPanelModel& aModel, const theme::Graph& aTheme
    );
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
