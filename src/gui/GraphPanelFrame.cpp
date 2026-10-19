#include "gui/GraphPanelFrame.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFont>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPointer>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QVarLengthArray>

#include "imgui.h"
// The draw list's own font lives in ImDrawListSharedData, which imgui.h only
// forward-declares. Reaching it here is deliberate: it is what lets drawPanel
// measure and draw text from the DRAW LIST alone, with no ImGui context and no
// frame - the property a headless pixel gate needs. (The vendored ImGui is
// version-pinned, and the Qt font loader already depends on this header.)
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"

#include "core/Frame.h"
#include "core/ObjectNode.h"
#include "core/TimeLineEvent.h"
#include "ctrl/GraphEditor.h"
#include "ctrl/graph/graph_Geometry.h"
#include "ctrl/graph/graph_Scaler.h"
#include "gl/EasyShaderProgram.h"
#include "gl/Framebuffer.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/Vector2.h"
#include "gl/VertexArrayObject.h"
#include "gui/ImGuiA11y.h"
#include "gui/ImGuiQtFontLoader.h"
#include "gui/theme/Colors.h"
#include "gui/theme/Graph.h"

namespace gui {
namespace {

// The GLSL version string handed to the GL3 backend. The application's context is
// a 4.0 CORE profile (gl::Global::kVersion), and the backend's own table maps GL
// 4.0 to "#version 400 core"; a core profile rejects the pre-150 strings the
// backend would otherwise default to on desktop.
const char* kGlslVersion = "#version 400 core";

// The curve stroke width, in LOGICAL pixels - the one number the shipping coverage
// pass (gui::stroke) and the AddPolyline fallback below both stroke with.
//
// WHY 2.0. The curve is read at its full 2px on the plot, and the 1.5px it had before
// that cost 1.71px of ink per column on the pixel gate's own slope-0.5 segment against
// 2.21 (AE_PANEL_CROPS, measured across 41 columns, both constant to 3 decimals) - so
// the weight class is 2.0 on a real measurement, not on taste.
//
// WHAT IT IS FOR THE FALLBACK, where a whole number still matters: ImGui antialiases a
// polyline by one of two paths and the WIDTH decides which (imgui_draw.cpp,
// ImDrawList::AddPolyline). A width with a zero fractional part uses the baked LINE
// TEXTURE - one continuous quad strip whose edges the GPU's bilinear filter turns into
// a ramp, 2 vertices and 6 indices per point - while any fractional width falls back to
// the GEOMETRY path, 4 vertices and 18 indices per point whose joins overlap (each
// segment's fringe is blended over its neighbour's). 2.0 keeps the fallback on the
// cheap path; the SHIPPING path is gui::stroke, which builds its own geometry and
// measures the coverage per fragment, and it is what makes the stroke's width stop
// depending on the angle (see tests/graph_panel_pixels.cpp's angle assertion).
constexpr float kCurveWidth = stroke::kWidthPx;

// Length of the short axis marks, and where they hang from. The frame ruler is the
// TIMELINE'S RULER (see GraphPanelModel.h): its ticks are gui::frameRuler::kTickLength
// (5px, TimeRuler.cpp's kTickLength) and they are drawn UP from the band's last row,
// exactly as TimeRuler draws them; the value axis keeps its own short marks. ONE
// length for every frame-tick level: the tier is read from the ALPHA, never from a
// pixel length (the timeline's own rule, TimeRuler.cpp:19-30).
constexpr int kAxisTickLength = gui::frameRuler::kTickLength;

// The ruler ink's ALPHA for a tick mark, keyed by the tick's own tier, on top of
// the one token the ticks and the numbers share (theme::Graph::gridMajor() ==
// axisText() == Colors::text; see assignFrom). What these numbers are is the
// timeline's own ruler table (TimeRuler.cpp:22-30), mirrored here because the two
// views' rulers have to read as one ruler: the frame numbers stay at full strength
// and the marks under them take their tier's alpha, so a major, an intermediate and
// a minor tick are one ink at three emphases rather than three colours. THE TIER IS
// THE ALPHA, NEVER THE LENGTH: every mark is kAxisTickLength long, the timeline's own
// rule of one length for every level.
//
// THE TIER IS THE MODEL'S, not something measured here. GraphPanelModel classifies
// every tick against the ladder it already owns and publishes the classification
// beside the position (`frameTickLevel`, one entry per `frameTickX` entry, the
// ctrl::time::Scaler::TickLevel as its integer value - see THE AXES in
// GraphPanelModel.h). A tier recovered in this layer from tick spacing or label
// widths would be the pixel measurement the timeline's own comment rejects; the
// model has the ladder, so it states the tier and this table only says what a tier
// looks like.
constexpr float kMajorTickAlpha = 0.85f;

float tickAlpha(int aLevel) {
    switch (static_cast<ctrl::time::Scaler::TickLevel>(aLevel)) {
        case ctrl::time::Scaler::TickLevel::Major:
            return kMajorTickAlpha;
        case ctrl::time::Scaler::TickLevel::Intermediate:
            return 0.60;
        case ctrl::time::Scaler::TickLevel::Minor:
            return 0.20;
        case ctrl::time::Scaler::TickLevel::None:
            return 0.0; // not a tick: the model publishes no entry for it
    }
    return 0.0;
}

// The alpha of the model's tick `aIndex`, from its published tier. A model that
// published no tier for that tick (a hand-built one, e.g. a pixel gate's model built
// by hand) keeps the single major-tier alpha this layer drew before the tiers were
// published, so the marks are never dropped for want of a classification.
float tickAlphaOf(const GraphPanelModel& aModel, int aIndex) {
    if (0 <= aIndex && aIndex < aModel.frameTickLevel.size())
        return tickAlpha(aModel.frameTickLevel.at(aIndex));
    return kMajorTickAlpha;
}

// The box-select marquee's corner radius, in pixels: the timeline's own box
// (ctrl/time/Renderer.cpp draws a 6px rounded rect) so the two views' selection
// gestures are the same shape.
constexpr float kMarqueeRounding = 6.0f;

// The label halo: the same glyphs drawn at the eight compass offsets of radius 1
// and radius 2, so the ring reaches 2px past the glyph ink in every direction.
// Read only while gui::kDrawTextHalo is on (shipped off: see addHaloedText).
constexpr int kHaloRadius = 2;
constexpr float kHaloAlpha = 0.85f;

// Emphasis by SELECTION: the alpha a curve and its key markers are drawn at when its
// channel owns no selected key (gui::drawPanel's curveStrength). 45% keeps the shape
// readable while leaving no doubt about which curves are the ones being edited - and
// with an EMPTY selection every curve is drawn at this strength, because no channel
// owns a selected key (the design's decision table, "Curve emphasis": full strength
// for the channels that own a selected key, 0.45 for the rest, all dimmed when
// nothing is selected). This replaced the sticky focused channel's dim; the focus
// still governs which channel's tangent handles are drawn.
constexpr float kUnselectedAlpha = 0.45f;

// A curve's name CHIP (GraphPanelModel's CHIPS), drawn in the plot's right-hand
// edge for the curves IN PLAY - the channels that own a selected key and whatever
// the pointer is over. The chip carries its own backing, so it stays readable
// wherever the curve leaves the window: the model's chip box filled with the panel
// tone at this alpha, and the name in the channel's own hue. The backdrop's alpha is
// the reference's (mock-graph.js draws 'rgba(16,16,16,0.82)' - the panel tone at
// 82%), through theme::Graph::halo(), which IS this palette's panel tone.
constexpr float kChipBackdropAlpha = 0.82f;
// The chip's own corner radius: the reference rounds its backing by 3px
// (mock-graph.js's roundRect(..., 3)).
constexpr float kChipRounding = 3.0f;
// The alpha of a chip's text for a curve the pointer is merely OVER, against full
// strength for one whose channel owns a selected key (the reference's own two
// strengths, mock-graph.js's _curveLabels).
constexpr float kChipHoverAlpha = 0.75f;
// How close the pointer's row must be to a curve's value at the pointer's own x for
// that curve to count as hovered. The reference's own reach (mock-graph.js's `best =
// 6`, in pixels).
constexpr double kChipHoverDistance = 6.0;

// TANGENT HANDLES (PanelHandle), in the reference's own ink (mock-graph.js:886-905):
//
//   * a STORED (Custom) pair is filled with the raised surface colour and STROKED in
//     the CHANNEL's own hue at 0.9, with its stem in that hue at 0.85 - the handle
//     belongs to a curve, so it reads in that curve's colour;
//   * a GHOST (Auto) pair is hollow, in the channel's hue at 0.7, its dot 1.5px
//     smaller and its stem at 0.5: it is derived, not stored, so a drag on it would
//     CONVERT the key, and it must never be taken for a tangent the document holds.
constexpr float kStoredHandleStemAlpha = 0.85f;
constexpr float kStoredHandleEdgeAlpha = 0.9f;
constexpr float kGhostHandleStemAlpha = 0.5f;
constexpr float kGhostHandleEdgeAlpha = 0.7f;
constexpr float kGhostHandleShrink = 1.5f;

// The text size used when the widget's font states no size at all (neither a pixel
// nor a usable point size: gui::imgui_qt::panelFontPixelSize returns 0). 13 is
// ImGui's own default font size, which keeps the panel legible instead of empty.
constexpr float kFallbackFontPixelSize = 13.0f;

// The accessibility ids are the header's (kPanelSummaryItemId, kPanelFitItemId,
// kPanelKeyMenuItemId): both hosts of the panel dispatch on them through
// publishPanelAccessibility's callback, so they are not private to this translation
// unit any more.

// lupdate's context for everything this file says to the user: the class is not a
// QObject, so there is no tr() member for it to find.
//
// THE NAME IS FROZEN AT "gui::ImGuiOverlay" even though the class that named it is gone
// (the panel's one host is gui::GraphPanelWidget now). It is a CATALOG key, not a
// description: assets/locale/translation_*.ts already carry every panel string under it,
// so renaming it here would silently ship English in five languages until the catalogs
// are re-extracted and re-translated - a translation change, which is not this unit's.
const char* kTranslationContext = "gui::ImGuiOverlay";

// The runtime half of the translation pair. The class is not a QObject, so it asks
// with an EXPLICIT context.
//
// THE EXTRACTOR HALF IS AT THE CALL SITE, and it has to be: lupdate cannot see a
// string that only travels as an argument to a wrapper - a literal here would
// compile, run, and reach no catalog at all, shipping English in every language
// with nothing failing. So every user-visible literal in this file is written as
// QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "...") (or its _N_ plural form for the two
// that carry %n) INSIDE the call, which expands to the plain literal at compile
// time and puts the message and its context in front of the extractor, in the one
// expression where the two halves cannot drift. The repository's own precedent is
// gui::graphPanelTr + QT_TRANSLATE_NOOP("GraphPanel", ...) in
// src/gui/GraphPanelModel.cpp, and scripts/i18n/check_tr_contexts.py polices the
// pairing.
QString trText(const char* aSource, int aPlural = -1) {
    return QCoreApplication::translate(kTranslationContext, aSource, nullptr, aPlural);
}

ImU32 toImU32(const QColor& aColor) {
    return IM_COL32(aColor.red(), aColor.green(), aColor.blue(), aColor.alpha());
}

// The colour as it should be painted under a strength factor. Emphasis and
// "hidden" both scale ALPHA and leave the hue alone, so a dimmed curve still
// reads as its own channel.
ImU32 toImU32Strength(const QColor& aColor, float aStrength) {
    QColor color = aColor;
    color.setAlphaF(qBound(0.0f, color.alphaF() * aStrength, 1.0f));
    return toImU32(color);
}

// The window rect as ImGui's half-open rect: right()/bottom() are the last
// INCLUDED pixel columns.
ImVec2 rectMin(const QRect& aRect) { return ImVec2(float(aRect.left()), float(aRect.top())); }
ImVec2 rectMax(const QRect& aRect) { return ImVec2(float(aRect.right() + 1), float(aRect.bottom() + 1)); }

// The draw list's own font and size. drawPanel must NOT go through
// ImGui::CalcTextSize(): that one reads the current context (g.Font), and the
// drawing has to be callable from a headless pixel gate that owns nothing but a
// draw list. ImDrawList keeps the frame's font in its shared data, so this is
// the same font the glyphs are drawn with.
ImFont* drawListFont(ImDrawList& aDrawList) { return aDrawList._Data ? aDrawList._Data->Font : nullptr; }
float drawListFontSize(ImDrawList& aDrawList) { return aDrawList._Data ? aDrawList._Data->FontSize : 0.0f; }

ImVec2 measureText(ImFont* aFont, float aFontSize, const char* aText) {
    if (!aFont || !aText || !*aText)
        return ImVec2(0.0f, 0.0f);
    return aFont->CalcTextSizeA(aFontSize, FLT_MAX, 0.0f, aText);
}

// One glyph run, haloed: the 16 halo passes, then the text itself. The halo is
// what kept a label readable over arbitrary artwork - the panel has no background
// - and the 16 offsets are what make the ring solid rather than four smudges (see
// the constant above for the geometry).
//
// SHIPPED OFF (gui::kDrawTextHalo, in the header): the 17 glyph passes per label
// are now the contrast outline pass's job, which rings the same glyphs - and every
// other piece of panel ink - but only where the ink would otherwise have been
// lost, and in the outline colour rather than a panel tone. The body below is kept
// whole so the flag is the only difference between the two.
void addHaloedText(
    ImDrawList& aDrawList, ImFont* aFont, float aFontSize, const ImVec2& aPos, ImU32 aColor, ImU32 aHalo,
    const char* aText
) {
    if (!aFont || !aText || !*aText)
        return;

    if (kDrawTextHalo) {
        static const int kOffsets[8][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
        };
        for (int radius = kHaloRadius; radius >= 1; --radius) {
            for (const int* offset : kOffsets) {
                aDrawList.AddText(
                    aFont, aFontSize,
                    ImVec2(aPos.x + float(offset[0] * radius), aPos.y + float(offset[1] * radius)), aHalo, aText
                );
            }
        }
    }
    aDrawList.AddText(aFont, aFontSize, aPos, aColor, aText);
}

enum class LabelAlign { Left, Centre, Right };

// One label inside the box the model decided, at the alignment the model
// documented for it, with the halo. The box is the model's; only the placement
// inside it is the drawing layer's.
//
// THE GLYPHS ARE CLAMPED INTO THE BOX. The model measures its boxes with the
// default UI font (it has no painter) and this layer draws with the widget's, so
// the two line heights are not the same number; a layer font taller than the box
// would put half its overflow ABOVE the box's top edge - and for a frame number
// that is above the plot's top edge, i.e. inside the plot clip, which is how the
// top row of ruler numbers came out with their caps cut off. Centring therefore
// takes the larger of "centred" and "flush with the box's top": an over-tall font
// hangs into the box's own interior (where there is room) instead of out of it.
void drawLabel(
    ImDrawList& aDrawList, ImFont* aFont, float aFontSize, const QRect& aBox, const QString& aText, LabelAlign aAlign,
    ImU32 aColor, ImU32 aHalo, int aRightGapPx = 0
) {
    if (!aFont || aText.isEmpty() || aBox.isEmpty())
        return;

    const QByteArray utf8 = aText.toUtf8();
    const ImVec2 size = measureText(aFont, aFontSize, utf8.constData());
    float x = float(aBox.left());
    if (aAlign == LabelAlign::Centre) {
        x += (float(aBox.width()) - size.x) * 0.5f;
    } else if (aAlign == LabelAlign::Right) {
        // THE RIGHT GAP IS TAKEN FROM THE BOX'S OWN SPARE WIDTH (see
        // kValueLabelGap): a label that fills its box is left exactly where
        // right-alignment puts it, so the gap can never push a number out of the
        // column the model measured for it. What is left over - a label wider than
        // its box, which the measured column cannot produce but a wider drawing font
        // can - is the caller's clip to hold back.
        const float spare = ImMax(0.0f, float(aBox.width()) - size.x);
        x = float(aBox.left() + aBox.width()) - size.x - ImMin(float(aRightGapPx), spare);
    }
    const float y = float(aBox.top()) + ImMax((float(aBox.height()) - size.y) * 0.5f, 0.0f);
    addHaloedText(aDrawList, aFont, aFontSize, ImVec2(x, y), aColor, aHalo, utf8.constData());
}

// THE MARKER RULE's ink tokens, and where each comes from.
//
// The MODEL classifies (PanelCurve::keySelected / keySiblingSelected: which of the
// three states a marker is in) and this layer decides what each state LOOKS like -
// the split every token below lives on the ink side of. The reference is the mock's
// key pass (assets/mock-graph.js's _keys, its `on`/`hov`/`sibling` lines), which the
// design's section 4 quotes:
//
//   * the SELECTED marker brightens its fill toward the keyframe highlight and takes
//     a 1px ring clear of the marker (index.html:279-280, "the fill brightens toward
//     the keyframe highlight and a 1px ring sits 1px clear of the marker; the size
//     never changes"), and the mock mixes the channel hue toward its highlight ink at
//     0.6 for it (lighten(hue, 0.6));
//   * a hovered marker brightens the same way at the mock's lighter step
//     (lighten(hue, 0.4)) and takes NO ring ("hover and selection brighten the fill
//     toward the keyframe highlight (no size change, no accent ring)",
//     index.html:302-303 - and the design says outright that the app has no hover
//     state today and this is what it should be);
//   * a marker whose key has a selected SIBLING keeps a FAINT ring - the mock's own
//     precedent, `hexA(hue, 0.35)` at the same radius as the selection ring, 1px,
//     with no brightened fill ("not selected, but its key is: a faint ring, no
//     brighter fill", mock-graph.js:859-860) - while its CURVE stays at full
//     strength (its channel is in play, _live()).
//
// THE HIGHLIGHT INK IS theme::Graph::axisText(), and that needs saying because the
// mock's token is literally `#ffffff` (assets/proposal.css:63's --key-highlight, the
// target its lighten() mixes toward). A hardcoded white is a palette colour this
// layer does not own, and it is wrong on the light palette, where the floor is near
// white: the mock is dark-only. axisText() is the palette's own "brightest panel ink"
// cell - Colors::text, the very token the design's section 6 asks the panel's ruler
// and its numbers to share - so the mix is "toward this palette's strongest ink" on
// both polarities, which is the design's sentence read against a palette instead of
// against the mock's one.
//
// THE SELECTED MARKER'S INK, one rule for BOTH views (the graph and the timeline):
//
//   * fill  = the channel's own hue mixed 0.45 toward the text colour - a lift toward
//             the brightest panel ink without leaving the channel's identity;
//   * ring  = the channel's OWN HUE at 0.85 alpha, 1px, at
//             kKeyDotRadius + kKeyRingOffsetPx (2.6px), drawn OUTSIDE the marker;
//   * the marker's own 1px edge stroke is drawn OUTSIDE its radius too (see the
//     AddCircle below), so the coloured core is never eaten by its edge.
//
// NO DARK SLEEVE under this ring, and the reason is the ink itself: the mock's sleeve
// exists to make a WHITE highlight ring read on arbitrary artwork
// (mock-graph.js:865-876). This ring is the channel's own saturated hue - the same
// ink family as its curve and its fill - and the marker already sits inside its
// keyEdge() floor ring. A 2.6px black band beneath it would bury the 1px hue stroke
// on the dark palette and read as a second, grey selection accent, which is exactly
// the grey the user rejected for handles.
constexpr float kKeySelectedFillMix = 0.45f;   // 0.45 of the way to the text colour
constexpr float kKeyHoverFillMix = 0.4f;       // mock-graph.js lighten(hue, 0.4)
constexpr float kKeySiblingRingAlpha = 0.35f;  // mock-graph.js hexA(hue, 0.35)
constexpr float kKeyRingWidth = 1.0f;          // the 1px ring of the design's sentence
constexpr float kKeySelectedRingAlpha = 0.85f; // the selection ring, in the channel's hue

// A channel colour moved toward the highlight ink, channel-wise in sRGB - the mock's
// own lighten(): `c + (255 - c) * t` per channel, opaque. A mix on the encoded values
// rather than in linear light is the reference's arithmetic, and it is what keeps this
// layer's "brighter" step identical to the mock's rather than merely similar.
QColor mixedToward(const QColor& aColour, const QColor& aToward, float aMix) {
    const float t = qBound(0.0f, aMix, 1.0f);
    const auto step = [t](int aFrom, int aTo) {
        return int(std::lround(double(aFrom) + (double(aTo) - double(aFrom)) * double(t)));
    };
    return QColor(step(aColour.red(), aToward.red()), step(aColour.green(), aToward.green()),
                  step(aColour.blue(), aToward.blue()));
}

// The curve's INK STRENGTH: the SELECTION's, not the sticky focus's (the design's
// decision table, "Curve emphasis": full strength for the channels that own a
// selected key, 0.45 for the rest, and all of them dimmed when nothing is selected).
// The model publishes the fact this folds over - a channel is IN PLAY when it owns a
// SELECTED marker, or the same KEY is selected on a sibling channel (one Move key
// holds both components, so selecting Move X alone keeps Move Y at full strength:
// mock-graph.js's _live(), and the design's "the marker of the same key that is not
// selected keeps a faint ring and its curve stays at full strength", index.html:244).
// Both flags already mean "in play" for a dot: a sibling flag is set exactly when
// another marker of that key is selected. What this layer decides is the one thing
// the model does not publish - that "in play" is 1.0 and everything else 0.45.
float curveStrength(const PanelCurve& aCurve) {
    const int count = std::min(aCurve.keySelected.size(), aCurve.keySiblingSelected.size());
    for (int i = 0; i < count; ++i) {
        if (aCurve.keySelected.at(i) || aCurve.keySiblingSelected.at(i))
            return 1.0f;
    }
    return kUnselectedAlpha;
}

} // namespace

//-------------------------------------------------------------------------------------------------
// THE CONTRAST OUTLINE AND COMPOSITE PASS (declared and described in the header).
//
// WHY THE GLSL IS A STRING HERE AND NOT A FILE IN assets/shaders. Every other
// shader in this repository is a resource (`:/shaders/*.glsl`, from
// assets/assets.qrc). This pass cannot be: its own pixel gate
// (tests/graph_panel_pixels.cpp) is the reason the pass is a free unit at all, and
// that target compiles THIS translation unit from a source list in
// tests/CMakeLists.txt that has never carried assets.qrc - a fragment shader that
// is only a resource is a shader the gate cannot load. gl::EasyTextureDrawer, the
// other full-screen textured-quad unit here, embeds its shaders for the same kind
// of reason. One copy, reachable by both.
namespace {

// A full-screen triangle generated from gl_VertexID: the three corners (0,0) (2,0)
// (0,2) in UV space land on (-1,-1) (3,-1) (-1,3) in clip space, so the pass needs
// no vertex buffer and no vertex attributes - only a bound VAO, which a core
// profile requires for any draw call.
const char* kOutlineVertexSource =
    "#version 400 core\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUv = corner;\n"
    "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kOutlineFragmentSource =
    "#version 400 core\n"
    "in vec2 vUv;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D uOverlay;\n"
    "uniform sampler2D uBackdrop;\n"
    "uniform vec2 uTexel;\n"
    "uniform float uMinContrast;\n"
    "uniform vec3 uOutlineColour;\n"
    "uniform float uRingRadiusPx;\n"
    "\n"
    "// WCAG relative luminance: the sRGB channels are linearised first, because\n"
    "// uMinContrast is WCAG's 3:1 and that ratio only means 3:1 in linear light.\n"
    "float linearise(float aChannel) {\n"
    "    return aChannel <= 0.04045 ? aChannel / 12.92 : pow((aChannel + 0.055) / 1.055, 2.4);\n"
    "}\n"
    "float luma(vec3 aRgb) {\n"
    "    return dot(vec3(0.2126, 0.7152, 0.0722),\n"
    "               vec3(linearise(aRgb.r), linearise(aRgb.g), linearise(aRgb.b)));\n"
    "}\n"
    "float contrast(float aLuma, float aOther) {\n"
    "    float hi = max(aLuma, aOther);\n"
    "    float lo = min(aLuma, aOther);\n"
    "    return (hi + 0.05) / (lo + 0.05);\n"
    "}\n"
    "\n"
    "// One tap of the overlay, in the panel's OWN colour. The overlay target holds\n"
    "// the ImGui backend's own source-over blend, so its rgb arrives PREMULTIPLIED\n"
    "// by the coverage in alpha; dividing that back out is what lets the contrast\n"
    "// test and the composite below be written in the panel's colours. Without it a\n"
    "// partially covered pixel would composite at alpha squared.\n"
    "vec4 tap(vec2 aUv) {\n"
    "    vec4 t = texture(uOverlay, aUv);\n"
    "    return vec4(t.a > 0.0 ? t.rgb / t.a : vec3(0.0), t.a);\n"
    "}\n"
    "\n"
    "// The ink test is alpha > 0.0 and not an epsilon: a hairline at partial\n"
    "// coverage still counts as ink. The tail of the anti-aliasing can therefore\n"
    "// make the ring about a pixel wider than the visible ink; if it ever looks fat,\n"
    "// this comparison is the knob (an epsilon on aTap.a).\n"
    "bool lowContrastInk(vec4 aTap, float aBackdropLuma) {\n"
    "    return aTap.a > 0.0 && contrast(luma(aTap.rgb), aBackdropLuma) < uMinContrast;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 ov = tap(vUv);\n"
    "    vec4 tW = tap(vUv + vec2(-uTexel.x, 0.0));\n"
    "    vec4 tE = tap(vUv + vec2( uTexel.x, 0.0));\n"
    "    vec4 tN = tap(vUv + vec2(0.0, -uTexel.y));\n"
    "    vec4 tS = tap(vUv + vec2(0.0,  uTexel.y));\n"
    "\n"
    "    // The optional second ring, two pixels out. A uniform branch, so the\n"
    "    // shipped 1px ring pays for four taps and not eight.\n"
    "    vec4 tW2 = vec4(0.0);\n"
    "    vec4 tE2 = vec4(0.0);\n"
    "    vec4 tN2 = vec4(0.0);\n"
    "    vec4 tS2 = vec4(0.0);\n"
    "    if (uRingRadiusPx > 1.5) {\n"
    "        tW2 = tap(vUv + vec2(-2.0 * uTexel.x, 0.0));\n"
    "        tE2 = tap(vUv + vec2( 2.0 * uTexel.x, 0.0));\n"
    "        tN2 = tap(vUv + vec2(0.0, -2.0 * uTexel.y));\n"
    "        tS2 = tap(vUv + vec2(0.0,  2.0 * uTexel.y));\n"
    "    }\n"
    "\n"
    "    // The outline's strength is the STRONGEST TAP, never this pixel's own\n"
    "    // coverage: that is what makes the ring track the shape - a 40% stroke gets\n"
    "    // a 40% ring, adjacent strokes fill in without double-darkening, and no ink\n"
    "    // nearby draws nothing at all.\n"
    "    float maxA = max(max(max(tW.a, tE.a), max(tN.a, tS.a)),\n"
    "                     max(max(tW2.a, tE2.a), max(tN2.a, tS2.a)));\n"
    "\n"
    "    vec4 backdrop = texture(uBackdrop, vUv);\n"
    "    float backdropLuma = luma(backdrop.rgb);\n"
    "    // Per pixel, against THIS pixel's backdrop, so a glyph straddling a light\n"
    "    // and a dark region is ringed only on the side that needs it.\n"
    "    bool low = lowContrastInk(tW, backdropLuma) || lowContrastInk(tE, backdropLuma)\n"
    "            || lowContrastInk(tN, backdropLuma) || lowContrastInk(tS, backdropLuma)\n"
    "            || lowContrastInk(tW2, backdropLuma) || lowContrastInk(tE2, backdropLuma)\n"
    "            || lowContrastInk(tN2, backdropLuma) || lowContrastInk(tS2, backdropLuma);\n"
    "\n"
    "    vec3 col = backdrop.rgb;\n"
    "    if (low && maxA > 0.0)\n"
    "        col = mix(col, uOutlineColour, maxA);\n"
    "    // The panel composites on top at its own coverage, NEVER gated at\n"
    "    // alpha >= 1.0: every edge and every hairline is antialiased, so clipping\n"
    "    // to full coverage would jag the edges and erase the thin strokes.\n"
    "    col = mix(col, ov.rgb, ov.a);\n"
    "    // The backdrop's own alpha survives where nothing is drawn, so the pass is\n"
    "    // an exact no-op on every pixel the panel does not touch.\n"
    "    FragColor = vec4(col, mix(backdrop.a, 1.0, ov.a));\n"
    "}\n";

// The pass's GL objects, held as a RAW OWNED POINTER rather than a smart one, and
// deliberately: every destructor here calls GL, and a namespace-scope object is
// destroyed by exit() - by which time the context this pass was built on is long
// gone, which is a segfault inside glDeleteVertexArrays (measured). The objects are
// therefore destroyed exactly once, by outline::shutdown(), while a context is
// current; if that call is never made, the process is going away anyway.
struct OutlinePass {
    gl::EasyShaderProgram shader;
    std::unique_ptr<gl::VertexArrayObject> quad;
    int overlayLocation = -1;
    int backdropLocation = -1;
    int texelLocation = -1;
    int minContrastLocation = -1;
    int colourLocation = -1;
    int ringLocation = -1;
};

OutlinePass* gOutline = nullptr;

} // namespace

namespace outline {

// Builds the pass's program and its array object, on the CURRENT GL context and FOR that
// context: the program, its uniform locations and the VAO are all context objects, and a
// VAO name in particular is not shared between contexts even when they share buffers and
// textures (see the frame driver's own note on the panel's GL objects). Idempotent (safe
// to call per frame) and false when the context cannot compile the program - the caller
// must then draw without the ring rather than draw nothing.
bool init() {
    if (gOutline)
        return true;

    std::unique_ptr<OutlinePass> pass(new OutlinePass());
    // ONE program: the vertex stage derives the full-screen triangle from gl_VertexID
    // (no attribute, no vertex buffer), so only the fragment stage's six uniforms have
    // to be resolved here.
    const bool linked = pass->shader.setVertexSource(QString::fromLatin1(kOutlineVertexSource))
        && pass->shader.setFragmentSource(QString::fromLatin1(kOutlineFragmentSource)) && pass->shader.link();
    if (!linked) {
        XC_DEBUG_REPORT("the contrast outline pass did not build: %s", qPrintable(pass->shader.log()));
        return false;
    }

    pass->overlayLocation = pass->shader.uniformLocation("uOverlay");
    pass->backdropLocation = pass->shader.uniformLocation("uBackdrop");
    pass->texelLocation = pass->shader.uniformLocation("uTexel");
    pass->minContrastLocation = pass->shader.uniformLocation("uMinContrast");
    pass->colourLocation = pass->shader.uniformLocation("uOutlineColour");
    pass->ringLocation = pass->shader.uniformLocation("uRingRadiusPx");
    // The array object the pass binds around its one draw. It carries no attribute - the
    // positions come from gl_VertexID - but a core profile refuses to draw with NO array
    // object bound, and the binding it makes is saved and put back by composite() (which
    // documents why: the application's painter pass needs its own default VAO left bound).
    pass->quad.reset(new gl::VertexArrayObject());

    // Released by shutdown(), never by exit(): see gOutline above.
    gOutline = pass.release();
    return true;
}

void shutdown() {
    delete gOutline;
    gOutline = nullptr;
}

void copyBackdrop(GLuint aSourceFbo, GLuint aDestinationFbo, const QSize& aPixelSize) {
    if (aPixelSize.isEmpty())
        return;

    gl::Global::Functions& ggl = gl::Global::functions();
    GLint previousRead = 0;
    GLint previousDraw = 0;
    ggl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    ggl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);

    // A blit is scissored exactly like a draw, and the ImGui backend leaves
    // GL_SCISSOR_TEST enabled at the box its own last draw command set.
    ggl.glDisable(GL_SCISSOR_TEST);
    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, aSourceFbo);
    ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, aDestinationFbo);
    ggl.glBlitFramebuffer(
        0, 0, aPixelSize.width(), aPixelSize.height(), 0, 0, aPixelSize.width(), aPixelSize.height(),
        GL_COLOR_BUFFER_BIT, GL_NEAREST
    );

    // The framebuffer bindings go back as they were found: this is a copy, not a
    // change of target.
    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(previousRead));
    ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(previousDraw));
    GL_CHECK_ERROR();
}

void composite(GLuint aOverlay, GLuint aBackdrop, const QSize& aPixelSize, const Params& aParams) {
    if (!gOutline || aPixelSize.isEmpty())
        return;

    gl::Global::Functions& ggl = gl::Global::functions();

    // THE PASS IS A GUEST IN THE FRAME, AND IT PUTS BACK WHAT IT CHANGES. The
    // bindings it saves here are not courtesies: the application's paint path keeps
    // ONE default vertex array bound for the whole session
    // (MainDisplayWidget::initializeGL creates it and comments "keep binding"), and
    // the painter engine that runs at the TOP of the next paint relies on that -
    // every glVertexAttribPointer it makes needs an array object bound in a core
    // profile. MEASURED: a version of this pass that ended with glBindVertexArray(0)
    // made the next frame's painter pass fail with GL_INVALID_OPERATION in
    // glVertexAttribPointer and glDrawArrays, and the error surfaced one call later
    // as an assert in gl::EasyShaderProgram::release(). The ImGui backend restores
    // the same binding for the same reason.
    GLint savedVao = 0;
    GLint savedActiveTexture = 0;
    GLint savedTexture[2] = {0, 0};
    ggl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao);
    ggl.glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture[0]);
    ggl.glActiveTexture(GL_TEXTURE1);
    ggl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture[1]);

    // Every piece of state this pass depends on is SET rather than assumed: it runs
    // at the very end of the frame, where the ImGui backend has left blending on
    // with its own function, the scissor box enabled, and the viewport at whatever
    // its last draw command asked for.
    ggl.glDisable(GL_BLEND);
    ggl.glDisable(GL_DEPTH_TEST);
    ggl.glDisable(GL_CULL_FACE);
    ggl.glDisable(GL_SCISSOR_TEST);
    // Dithering is on by default in GL and would perturb the 8-bit write by a bit,
    // which would stop the pass from being an exact no-op on the pixels the panel
    // does not touch (and off it is again on the next frame: paintGL resets the
    // render state at the top of every paint).
    ggl.glDisable(GL_DITHER);
    ggl.glViewport(0, 0, aPixelSize.width(), aPixelSize.height());

    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, aOverlay);
    ggl.glActiveTexture(GL_TEXTURE1);
    ggl.glBindTexture(GL_TEXTURE_2D, aBackdrop);

    OutlinePass& pass = *gOutline;
    pass.quad->bind();
    pass.shader.bind();
    pass.shader.setUniformValue(pass.overlayLocation, 0);
    pass.shader.setUniformValue(pass.backdropLocation, 1);
    pass.shader.setUniformValue(
        pass.texelLocation,
        QVector2D(GLfloat(1.0) / GLfloat(aPixelSize.width()), GLfloat(1.0) / GLfloat(aPixelSize.height()))
    );
    pass.shader.setUniformValue(pass.minContrastLocation, aParams.minContrast);
    pass.shader.setUniformValue(
        pass.colourLocation,
        QVector3D(GLfloat(aParams.colour.redF()), GLfloat(aParams.colour.greenF()), GLfloat(aParams.colour.blueF()))
    );
    pass.shader.setUniformValue(pass.ringLocation, aParams.ringRadiusPx);
    ggl.glDrawArrays(GL_TRIANGLES, 0, 3);
    // The PROGRAM is deliberately left unbound, exactly as the application's own
    // gl::EasyTextureDrawer leaves it at the end of every paint: 0 is a state the
    // painter's next bind() establishes for itself.
    pass.shader.release();

    ggl.glBindVertexArray(GLuint(savedVao));
    ggl.glBindTexture(GL_TEXTURE_2D, GLuint(savedTexture[1]));
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, GLuint(savedTexture[0]));
    ggl.glActiveTexture(GLenum(savedActiveTexture));
    GL_CHECK_ERROR();
}

} // namespace outline

//-------------------------------------------------------------------------------------------------
// The coverage stroke: the panel's curve ink. gui::stroke in the header says why it is a
// pass of its own rather than ImDrawList::AddPolyline, what the coverage function is, and
// where in the frame its callback sits. What follows is that pass's machinery.
namespace {

// The per-segment program. Every vertex carries the quad's own corner AND the segment's
// two endpoints, so the fragment can measure the distance to the SEGMENT rather than to
// the quad.
//
// THE PROJECTION IS THE BACKEND'S OWN ORTHO, term for term
// (ImGui_ImplOpenGL3_SetupRenderState): it is diagonal plus a translation, so its four
// numbers are all the vertex shader needs, and using the same numbers rather than
// rebuilding a matrix here is what makes a curve land on exactly the pixel ImGui's own
// geometry would have inked.
const char* kStrokeVertexSource =
    "#version 400 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aP0;\n"
    "layout(location = 2) in vec2 aP1;\n"
    "uniform vec4 uOrtho;\n"
    "out vec2 vPos;\n"
    "out vec2 vP0;\n"
    "out vec2 vP1;\n"
    "void main() {\n"
    "    vPos = aPos;\n"
    "    vP0 = aP0;\n"
    "    vP1 = aP1;\n"
    "    gl_Position = vec4(aPos.x * uOrtho.x + uOrtho.z, aPos.y * uOrtho.y + uOrtho.w, 0.0, 1.0);\n"
    "}\n";

const char* kStrokeFragmentSource =
    "#version 400 core\n"
    "in vec2 vPos;\n"
    "in vec2 vP0;\n"
    "in vec2 vP1;\n"
    "uniform float uPxScale;\n"
    "uniform float uHalfWidth;\n"
    "uniform float uFilter;\n"
    "uniform vec3 uInk;\n"
    "uniform float uAlpha;\n"
    "uniform int uMode;\n"
    "out vec4 FragColor;\n"
    "\n"
    "void main() {\n"
    "    // The distance to the SEGMENT, never to its infinite line: the clamp on h is what\n"
    "    // gives each segment its own round join and cap with no extra geometry at all.\n"
    "    vec2 ba = vP1 - vP0;\n"
    "    vec2 pa = vPos - vP0;\n"
    "    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);\n"
    "    // In DEVICE pixels, which is the currency the filter and the stroke's weight are\n"
    "    // in (the nominal width is a logical one, so a 2x screen gets a 2x stroke).\n"
    "    float d = length(pa - ba * h) * uPxScale;\n"
    "\n"
    "    // The box filter: a pixel's coverage of a half-plane edge is the length of the\n"
    "    // filter's span that lies inside the stroke over the span's own width, so the 50%\n"
    "    // crossing lands exactly on the nominal edge at any sub-pixel phase.\n"
    "    float r = 0.5 * uFilter;\n"
    "    // A stroke narrower than the filter keeps the filter's footprint (1 device px) and\n"
    "    // scales its ALPHA instead of thinning away - Rougier's sub-pixel rule.\n"
    "    float body = max(uHalfWidth, r);\n"
    "    float cov = clamp((body + r - d) / uFilter, 0.0, 1.0) * min(1.0, uHalfWidth / r);\n"
    "\n"
    "    // 0: the coverage alone, accumulated with GL_MAX, so overlapping segments take the\n"
    "    // UNION of their capsules instead of blending twice. 1: the ink straight into the\n"
    "    // frame - the path taken when the scratch target could not be created, where a\n"
    "    // join that overlaps itself beats a missing curve.\n"
    "    FragColor = (uMode == 0) ? vec4(cov) : vec4(uInk, cov * uAlpha);\n"
    "}\n";

// The composite: one full-screen triangle that turns the accumulated coverage back into
// the panel's own premultiplied ink. No attributes at all (gl_VertexID carries the
// corner), which is why it needs no vertex buffer.
const char* kStrokeCompositeVertexSource =
    "#version 400 core\n"
    "void main() {\n"
    "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kStrokeCompositeFragmentSource =
    "#version 400 core\n"
    "uniform sampler2D uCoverage;\n"
    "uniform vec2 uSize;\n"
    "uniform vec3 uInk;\n"
    "uniform float uAlpha;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    // gl_FragCoord's centre is the texel centre of the same pixel of an equally sized\n"
    "    // attachment, so this reads the coverage 1:1 with no filtering of its own.\n"
    "    float cov = texture(uCoverage, gl_FragCoord.xy / uSize).a;\n"
    "    // The backend's own source-over (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,\n"
    "    // GL_ONE_MINUS_SRC_ALPHA) makes this the PREMULTIPLIED ink the contrast outline\n"
    "    // pass reads back - the convention the overlay's framebuffer holds.\n"
    "    FragColor = vec4(uInk, cov * uAlpha);\n"
    "}\n";

// The callback data ImGui copies for ONE curve (ImDrawList::AddCallback's userdata_size
// path), so the geometry lives inside the draw list and nothing outlives the call. 24
// bytes of header keeps every blob a multiple of 8, which is what keeps the float pairs
// that follow it aligned inside ImGui's own byte buffer.
struct CurveInkBlob {
    int pointCount;   // >= 2
    float red;
    float green;
    float blue;
    float alpha;      // the ink's strength: 1.0, or kUnselectedAlpha
    float halfWidth;  // the stroke's half width, in LOGICAL px: the curve's kCurveWidth/2,
                      // a handle stem's 0.5. Both are the same stroke otherwise, which is
                      // why there is one record and not two.
};
static_assert(sizeof(CurveInkBlob) == 24, "the blob's alignment rule");

// The pass's GL objects, held as a RAW OWNED POINTER for the same reason outline's are:
// every destructor here calls GL, and a namespace-scope object is destroyed by exit(),
// by which time this context is long gone (a measured segfault inside glDelete*). They
// are destroyed exactly once, by stroke::shutdown(), while a context is current.
struct StrokePass {
    gl::EasyShaderProgram segments;
    gl::EasyShaderProgram composite;
    std::unique_ptr<gl::VertexArrayObject> vao;

    int orthoLocation = -1;
    int pxScaleLocation = -1;
    int halfWidthLocation = -1;
    int filterLocation = -1;
    int inkLocation = -1;
    int alphaLocation = -1;
    int modeLocation = -1;

    int coverageLocation = -1;
    int sizeLocation = -1;
    int compositeInkLocation = -1;
    int compositeAlphaLocation = -1;

    // The coverage scratch: the union of one curve's capsules, at the framebuffer's own
    // pixel size. Rebuilt only when that size changes.
    QSize pixels;
    std::unique_ptr<gl::Texture> coverageTexture;
    std::unique_ptr<gl::Framebuffer> coverageFbo;

    // The vertices of the curve being drawn, kept so a frame that draws the same curves
    // does not reallocate three vectors per curve.
    std::vector<gl::Vector2> positions;
    std::vector<gl::Vector2> starts;
    std::vector<gl::Vector2> ends;

    // False when the driver refuses either object; the caller then draws the ink
    // straight into the frame rather than losing it.
    bool ensureCoverage(const QSize& aPixels) {
        if (pixels == aPixels && coverageFbo)
            return true;
        pixels = QSize();
        coverageFbo.reset();
        coverageTexture.reset();
        coverageTexture.reset(new gl::Texture());
        coverageTexture->create(aPixels);
        coverageTexture->setFilter(GL_NEAREST);
        coverageTexture->setWrap(GL_CLAMP_TO_EDGE);
        coverageFbo.reset(new gl::Framebuffer());
        coverageFbo->setColorAttachment(0, coverageTexture->id());
        if (!coverageFbo->isComplete()) {
            coverageFbo.reset();
            coverageTexture.reset();
            return false;
        }
        pixels = aPixels;
        return true;
    }
};

StrokePass* gStroke = nullptr;

// Every piece of GL state the callback changes, so it can be put back exactly as it was
// found. This is not a courtesy: the ImGui backend keeps its shader program, its vertex
// array and its ARRAY BUFFER bound across a draw list's commands, and uploads the NEXT
// draw list's vertices with whatever array buffer is bound when it gets there.
struct GuestState {
    GLint program = 0;
    GLint vao = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = 0;
    GLint texture0 = 0;
    GLint drawFbo = 0;
    GLint readFbo = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissorBox[4] = {0, 0, 0, 0};
    GLint blendEquationRgb = 0;
    GLint blendEquationAlpha = 0;
    GLint blendSrcRgb = 0;
    GLint blendDstRgb = 0;
    GLint blendSrcAlpha = 0;
    GLint blendDstAlpha = 0;
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean dither = GL_FALSE;

    void save(gl::Global::Functions& aGgl) {
        aGgl.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        aGgl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        aGgl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        aGgl.glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        aGgl.glActiveTexture(GL_TEXTURE0);
        aGgl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        aGgl.glActiveTexture(GLenum(activeTexture));
        aGgl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        aGgl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        aGgl.glGetIntegerv(GL_VIEWPORT, viewport);
        aGgl.glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        aGgl.glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
        aGgl.glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
        aGgl.glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        aGgl.glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        aGgl.glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        aGgl.glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        blend = aGgl.glIsEnabled(GL_BLEND);
        scissor = aGgl.glIsEnabled(GL_SCISSOR_TEST);
        dither = aGgl.glIsEnabled(GL_DITHER);
    }

    void restore(gl::Global::Functions& aGgl) const {
        aGgl.glBlendEquationSeparate(GLenum(blendEquationRgb), GLenum(blendEquationAlpha));
        aGgl.glBlendFuncSeparate(
            GLenum(blendSrcRgb), GLenum(blendDstRgb), GLenum(blendSrcAlpha), GLenum(blendDstAlpha)
        );
        if (blend)
            aGgl.glEnable(GL_BLEND);
        else
            aGgl.glDisable(GL_BLEND);
        if (dither)
            aGgl.glEnable(GL_DITHER);
        else
            aGgl.glDisable(GL_DITHER);
        if (scissor)
            aGgl.glEnable(GL_SCISSOR_TEST);
        else
            aGgl.glDisable(GL_SCISSOR_TEST);
        aGgl.glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        aGgl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        aGgl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
        aGgl.glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFbo));
        aGgl.glBindVertexArray(GLuint(vao));
        aGgl.glBindBuffer(GL_ARRAY_BUFFER, GLuint(arrayBuffer));
        aGgl.glActiveTexture(GL_TEXTURE0);
        aGgl.glBindTexture(GL_TEXTURE_2D, GLuint(texture0));
        aGgl.glActiveTexture(GLenum(activeTexture));
        aGgl.glUseProgram(GLuint(program));
    }
};

// The device-pixel rect a clip rect covers, in the coordinates glScissor takes (y counted
// from the BOTTOM), computed with the backend's own arithmetic and its own truncations -
// so the pass clips on exactly the pixels ImGui clips its own commands on.
QRect scissorRect(const ImVec4& aClipRect, const ImVec2& aDisplayPos, const ImVec2& aScale, int aFramebufferHeight) {
    const ImVec2 clipMin((aClipRect.x - aDisplayPos.x) * aScale.x, (aClipRect.y - aDisplayPos.y) * aScale.y);
    const ImVec2 clipMax((aClipRect.z - aDisplayPos.x) * aScale.x, (aClipRect.w - aDisplayPos.y) * aScale.y);
    if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
        return QRect();
    return QRect(
        int(clipMin.x), int(float(aFramebufferHeight) - clipMax.y), int(clipMax.x - clipMin.x),
        int(clipMax.y - clipMin.y)
    );
}

// The device-pixel rect one curve's ink can reach (its own bounding box, grown by the
// stroke's whole footprint), in the same coordinates.
QRect curveInkRect(const CurveInkBlob* aInk, const ImDrawData* aDrawData, int aFramebufferHeight) {
    const float scale = aDrawData->FramebufferScale.x > 0.0f ? float(aDrawData->FramebufferScale.x) : 1.0f;
    // Half the stroke (its own, not the curve's: this rect is asked for both the curves and
    // the handle stems), the filter's own half width (in logical units), and a pixel of
    // slack - the same reach the quads themselves are built with.
    const float reach = aInk->halfWidth + (0.5f * stroke::kFilterPx + 1.0f) / scale;

    const float* coords = reinterpret_cast<const float*>(reinterpret_cast<const uchar*>(aInk) + sizeof(*aInk));
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    float maxX = -FLT_MAX;
    float maxY = -FLT_MAX;
    for (int i = 0; i < aInk->pointCount; ++i) {
        minX = std::min(minX, coords[2 * i]);
        maxX = std::max(maxX, coords[2 * i]);
        minY = std::min(minY, coords[2 * i + 1]);
        maxY = std::max(maxY, coords[2 * i + 1]);
    }
    if (minX > maxX)
        return QRect();

    const ImVec2 offset = aDrawData->DisplayPos;
    const ImVec2 sc = aDrawData->FramebufferScale;
    const int left = int(std::floor((minX - reach - offset.x) * sc.x));
    const int right = int(std::ceil((maxX + reach - offset.x) * sc.x));
    const int top = int(std::floor((minY - reach - offset.y) * sc.y));
    const int bottom = int(std::ceil((maxY + reach - offset.y) * sc.y));
    return QRect(left, aFramebufferHeight - bottom, right - left, bottom - top);
}

// One polyline's place in the draw list: the geometry and the ink travel in the callback's
// own copied data, so this keeps no state of its own and the panel stays a pure function of
// its model. One callback PER POLYLINE is what keeps the pass in the draw list's own order -
// over the ticks and the labels, under the key markers (a curve), and between the markers
// and their own dots (a handle stem).
void addInkCallback(
    ImDrawList& aDrawList, const QVector<QPointF>& aPoints, const QColor& aColour, float aStrength,
    float aHalfWidth
) {
    if (aPoints.size() < 2)
        return;

    QVarLengthArray<uchar, 2048> blob(int(sizeof(CurveInkBlob) + aPoints.size() * 2 * sizeof(GLfloat)));
    CurveInkBlob header;
    header.pointCount = int(aPoints.size());
    header.red = GLfloat(aColour.redF());
    header.green = GLfloat(aColour.greenF());
    header.blue = GLfloat(aColour.blueF());
    header.alpha = aStrength;
    header.halfWidth = aHalfWidth;
    std::memcpy(blob.data(), &header, sizeof(header));
    GLfloat* coords = reinterpret_cast<GLfloat*>(blob.data() + sizeof(CurveInkBlob));
    for (int i = 0; i < aPoints.size(); ++i) {
        coords[2 * i] = GLfloat(aPoints.at(i).x());
        coords[2 * i + 1] = GLfloat(aPoints.at(i).y());
    }
    // ImGui COPIES this into the draw list, so nothing here has to outlive the call.
    aDrawList.AddCallback(stroke::drawListCallback, blob.data(), size_t(blob.size()));
}

} // namespace

namespace stroke {

bool init() {
    if (gStroke)
        return true;

    std::unique_ptr<StrokePass> pass(new StrokePass());
    const bool segmentsLinked = pass->segments.setVertexSource(QString::fromLatin1(kStrokeVertexSource))
        && pass->segments.setFragmentSource(QString::fromLatin1(kStrokeFragmentSource)) && pass->segments.link();
    const bool compositeLinked = pass->composite.setVertexSource(QString::fromLatin1(kStrokeCompositeVertexSource))
        && pass->composite.setFragmentSource(QString::fromLatin1(kStrokeCompositeFragmentSource))
        && pass->composite.link();
    if (!segmentsLinked || !compositeLinked) {
        // Not fatal: drawPanel strokes with ImDrawList::AddPolyline instead, i.e. exactly
        // the ink this pass was added to replace.
        XC_DEBUG_REPORT(
            "the coverage stroke pass did not build: %s", qPrintable(pass->segments.log() + pass->composite.log())
        );
        return false;
    }

    pass->vao.reset(new gl::VertexArrayObject());
    pass->orthoLocation = pass->segments.uniformLocation("uOrtho");
    pass->pxScaleLocation = pass->segments.uniformLocation("uPxScale");
    pass->halfWidthLocation = pass->segments.uniformLocation("uHalfWidth");
    pass->filterLocation = pass->segments.uniformLocation("uFilter");
    pass->inkLocation = pass->segments.uniformLocation("uInk");
    pass->alphaLocation = pass->segments.uniformLocation("uAlpha");
    pass->modeLocation = pass->segments.uniformLocation("uMode");
    pass->coverageLocation = pass->composite.uniformLocation("uCoverage");
    pass->sizeLocation = pass->composite.uniformLocation("uSize");
    pass->compositeInkLocation = pass->composite.uniformLocation("uInk");
    pass->compositeAlphaLocation = pass->composite.uniformLocation("uAlpha");

    // Released by shutdown(), never by exit(): see gStroke above.
    gStroke = pass.release();
    return true;
}

void shutdown() {
    delete gStroke;
    gStroke = nullptr;
}

bool isReady() { return gStroke != nullptr; }

void drawListCallback(const ImDrawList*, const ImDrawCmd* aCommand) {
    if (!gStroke || !aCommand || !aCommand->UserCallbackData)
        return;
    const CurveInkBlob* ink = static_cast<const CurveInkBlob*>(aCommand->UserCallbackData);
    // A callback is per curve and the data is the blob addCurveInkCallback built; the
    // size ImGui recorded beside it is what makes a corrupt or truncated blob (never one
    // of ours) a no-op rather than a wild read.
    if (ink->pointCount < 2 || aCommand->UserCallbackDataSize < int(sizeof(*ink)))
        return;

    // The frame's OWN projection, clip and scale: the draw data is what the backend
    // renders from, so a curve lands where the backend's own commands land.
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData)
        return; // driven outside a frame: there is nothing to project against
    const int fbWidth = int(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int fbHeight = int(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0)
        return;

    // Where this curve may draw: the panel's own clip (the plot), its own ink's bounds,
    // and only where the two meet.
    const QRect clip = scissorRect(aCommand->ClipRect, drawData->DisplayPos, drawData->FramebufferScale, fbHeight);
    const QRect area = clip.intersected(curveInkRect(ink, drawData, fbHeight));
    if (area.isEmpty())
        return;

    gl::Global::Functions& ggl = gl::Global::functions();
    StrokePass& pass = *gStroke;

    // The stroke's own numbers, both in DEVICE pixels: the filter is one of those by
    // definition, and the nominal width is a logical one that a scaled framebuffer
    // multiplies (so a 2x screen gets a 2x stroke and a one-device-pixel ramp).
    const float pxScale = drawData->FramebufferScale.x > 0.0f ? float(drawData->FramebufferScale.x) : 1.0f;
    const float halfWidth = ink->halfWidth * pxScale;
    const float reach = ink->halfWidth + (0.5f * kFilterPx + 1.0f) / pxScale;

    // The quads: one per segment, expanded on EVERY side to halfWidth + filter, so the
    // capsule's round cap and join are inside the geometry already. All four corners
    // carry the segment's own endpoints - that is what the fragment measures against.
    const float* coords = reinterpret_cast<const float*>(reinterpret_cast<const uchar*>(ink) + sizeof(*ink));
    std::vector<gl::Vector2>& positions = pass.positions;
    std::vector<gl::Vector2>& starts = pass.starts;
    std::vector<gl::Vector2>& ends = pass.ends;
    positions.clear();
    starts.clear();
    ends.clear();
    for (int i = 0; i + 1 < ink->pointCount; ++i) {
        const float ax = coords[2 * i];
        const float ay = coords[2 * i + 1];
        const float bx = coords[2 * i + 2];
        const float by = coords[2 * i + 3];
        const float dx = bx - ax;
        const float dy = by - ay;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0f))
            continue; // a repeated sample: no direction, nothing to stroke
        const float ex = dx / length * reach;
        const float ey = dy / length * reach;
        const float fx = -dy / length * reach;
        const float fy = dx / length * reach;
        const gl::Vector2 corners[4] = {
            gl::Vector2::make(ax - ex + fx, ay - ey + fy),
            gl::Vector2::make(ax - ex - fx, ay - ey - fy),
            gl::Vector2::make(bx + ex - fx, by + ey - fy),
            gl::Vector2::make(bx + ex + fx, by + ey + fy),
        };
        // The corners alternate sides of the segment (c0/c3 on one, c1/c2 on the other), so
        // the two triangles have to share ONE diagonal: the pairing that shares the other
        // one (c2,c1,c3) crosses it instead, and the quad's middle is painted twice while a
        // wedge of it is painted not at all - measured as a hole in the ink, which is why
        // this is the order it is.
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int v = 0; v < 6; ++v) {
            positions.push_back(corners[order[v]]);
            starts.push_back(gl::Vector2::make(ax, ay));
            ends.push_back(gl::Vector2::make(bx, by));
        }
    }
    if (positions.empty())
        return;

    GuestState guest;
    guest.save(ggl);

    // The IMGUI ORTHO, term for term (SetupRenderState): clip.x = x * sx + tx and
    // clip.y = y * sy + ty, with the matrix's own diagonal and translation.
    const float left = drawData->DisplayPos.x;
    const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
    const float top = drawData->DisplayPos.y;
    const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
    const QVector4D ortho(
        GLfloat(2.0f / (right - left)), GLfloat(2.0f / (top - bottom)), GLfloat((right + left) / (left - right)),
        GLfloat((top + bottom) / (bottom - top))
    );

    // The scratch has to exist before anything is drawn into it; when the driver refuses
    // it (never measured, but cheap to survive) the ink goes straight into the frame.
    //
    // A SINGLE SEGMENT needs no scratch at all, and that is not a shortcut: with one segment
    // there is no second capsule to overlap, so the union IS that segment's capsule and the
    // direct path's source-over is bit-for-bit the same composite the accumulate path would
    // produce. It is worth saying because a two-point polyline is the common case: every
    // tangent handle stem, and every channel whose plot window holds one key pair.
    const bool accumulate = ink->pointCount > 2 && pass.ensureCoverage(QSize(fbWidth, fbHeight));

    // Dithering perturbs an 8-bit write by a bit, which is exactly the size of a ramp
    // step: the coverage pass writes real coverage, so it is turned off for the pass and
    // put back afterwards (the contrast outline pass does the same, for its own reason).
    ggl.glDisable(GL_DITHER);
    ggl.glDisable(GL_CULL_FACE);
    ggl.glDisable(GL_DEPTH_TEST);
    ggl.glEnable(GL_SCISSOR_TEST);
    ggl.glScissor(area.left(), area.top(), area.width(), area.height());
    ggl.glEnable(GL_BLEND);

    if (accumulate) {
        // 1. THE UNION OF THE CAPSULES, in the scratch. GL_MAX is the whole point: two
        // overlapping segments must contribute the LARGER coverage, not the sum of two
        // source-over blends (which would darken every join, and darken a dimmed curve's
        // join even where both segments are fully covered).
        pass.coverageFbo->bind();
        ggl.glViewport(0, 0, pass.pixels.width(), pass.pixels.height());
        ggl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        ggl.glClear(GL_COLOR_BUFFER_BIT);
        ggl.glBlendEquation(GL_MAX);
        ggl.glBlendFunc(GL_ONE, GL_ONE);

        pass.vao->bind();
        pass.segments.bind();
        pass.segments.setUniformValue(pass.orthoLocation, ortho);
        pass.segments.setUniformValue(pass.pxScaleLocation, pxScale);
        pass.segments.setUniformValue(pass.halfWidthLocation, GLfloat(halfWidth));
        pass.segments.setUniformValue(pass.filterLocation, GLfloat(kFilterPx));
        pass.segments.setUniformValue(pass.modeLocation, GLint(0));
        pass.segments.setAttributeArray(0, positions.data(), int(positions.size()));
        pass.segments.setAttributeArray(1, starts.data(), int(starts.size()));
        pass.segments.setAttributeArray(2, ends.data(), int(ends.size()));
        ggl.glDrawArrays(GL_TRIANGLES, 0, GLsizei(positions.size()));
        pass.segments.release();

        // 2. ONE composite of that union into the frame, with the ImGui backend's own
        // source-over, so the ink lands in the premultiplied convention the overlay's
        // framebuffer holds and the contrast outline pass reads back.
        ggl.glBindFramebuffer(GL_FRAMEBUFFER, GLuint(guest.drawFbo));
        ggl.glViewport(0, 0, fbWidth, fbHeight);
        ggl.glBlendEquation(GL_FUNC_ADD);
        ggl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        ggl.glActiveTexture(GL_TEXTURE0);
        ggl.glBindTexture(GL_TEXTURE_2D, pass.coverageTexture->id());

        pass.vao->bind();
        pass.composite.bind();
        pass.composite.setUniformValue(pass.coverageLocation, GLint(0));
        pass.composite.setUniformValue(pass.sizeLocation, QVector2D(GLfloat(fbWidth), GLfloat(fbHeight)));
        pass.composite.setUniformValue(
            pass.compositeInkLocation, QVector3D(ink->red, ink->green, ink->blue)
        );
        pass.composite.setUniformValue(pass.compositeAlphaLocation, ink->alpha);
        ggl.glDrawArrays(GL_TRIANGLES, 0, 3);
        pass.composite.release();
    } else {
        // No scratch: the ink goes straight into the frame at the coverage the fragment
        // measured. A join overlaps itself on this path - the one thing this pass exists
        // to avoid - but the curve is on screen, which is what matters here.
        ggl.glViewport(0, 0, fbWidth, fbHeight);
        ggl.glBlendEquation(GL_FUNC_ADD);
        ggl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        pass.vao->bind();
        pass.segments.bind();
        pass.segments.setUniformValue(pass.orthoLocation, ortho);
        pass.segments.setUniformValue(pass.pxScaleLocation, pxScale);
        pass.segments.setUniformValue(pass.halfWidthLocation, GLfloat(halfWidth));
        pass.segments.setUniformValue(pass.filterLocation, GLfloat(kFilterPx));
        pass.segments.setUniformValue(pass.modeLocation, GLint(1));
        pass.segments.setUniformValue(pass.inkLocation, QVector3D(ink->red, ink->green, ink->blue));
        pass.segments.setUniformValue(pass.alphaLocation, ink->alpha);
        pass.segments.setAttributeArray(0, positions.data(), int(positions.size()));
        pass.segments.setAttributeArray(1, starts.data(), int(starts.size()));
        pass.segments.setAttributeArray(2, ends.data(), int(ends.size()));
        ggl.glDrawArrays(GL_TRIANGLES, 0, GLsizei(positions.size()));
        pass.segments.release();
    }

    guest.restore(ggl);
    GL_CHECK_ERROR();
}

} // namespace stroke

//-------------------------------------------------------------------------------------------------
void drawPanel(
    ImDrawList& aDrawList, const GraphPanelModel& aModel, const theme::Graph& aTheme, const PanelFocus& aFocus
) {
    ImFont* font = drawListFont(aDrawList);
    const float fontSize = drawListFontSize(aDrawList);
    const ImU32 halo = toImU32Strength(aTheme.halo(), kHaloAlpha);
    // ONE ink for the ruler: the ticks and the numbers below them are the same token
    // (theme::Graph::axisText()), the numbers at full strength and every FRAME mark at
    // its own tier's alpha (tickAlphaOf, over the timeline's table) - the timeline's
    // own rule, instead of the separator tone's ticks under text numbers. The value
    // axis' three marks have no ladder to tier them (one per window value), so they
    // take the major tier's alpha; the zero rule keeps its own line tone (axisLine),
    // because it is a LINE and not a ruler mark.
    const ImU32 axisText = toImU32(aTheme.axisText());
    const ImU32 valueTick = toImU32Strength(aTheme.gridMajor(), kMajorTickAlpha);

    const QRect& plot = aModel.plot;

    if (plot.isEmpty()) {
        // No room for the plot. The model still states why, and the panel is the
        // whole viewport, so the reason goes in the middle of it.
        drawLabel(aDrawList, font, fontSize, aModel.panel, aModel.emptyState, LabelAlign::Centre, axisText, halo);
        return;
    }

    // Everything that belongs to the plot is clipped to it. This is the capability
    // the painter engine lacked (ctrl::GLCorePaintEngine has no clip), and it is
    // what keeps a curve that leaves the value window from stroking over the
    // value column or the panel's chrome.
    aDrawList.PushClipRect(rectMin(plot), rectMax(plot), true);

    // THE RULER'S BAND: the timeline's own strip, token for token (surface
    // rulerBand(), hairline rulerLine()), and the row the ticks rise from is its last
    // one. Only the model's published band is painted - a hand-built model that names
    // no band still gets the tick geometry below, but no surface it did not ask for.
    // The hairline sits one row UNDER the band, as the timeline draws it on the first
    // track row below its header.
    const QRect rulerBand = aModel.rulerBand.isEmpty()
        ? QRect(plot.left(), aModel.rulerBandTop, plot.width(), gui::frameRuler::kBandHeight)
        : aModel.rulerBand;
    if (!aModel.rulerBand.isEmpty()) {
        aDrawList.AddRectFilled(rectMin(aModel.rulerBand), rectMax(aModel.rulerBand), toImU32(aTheme.rulerBand()));
    }

    // The axes: SHORT marks only, 1px with AA off (an AA'd 1px line straddles two
    // columns and reads as 2px of grey, and every mark here is asked for an INTEGER
    // column by the model). The model publishes positions, never spans, so there is
    // no grid to draw. The flags are put back exactly as they were found, so a
    // caller's own setting is not clobbered.
    const ImDrawListFlags savedLineFlags = aDrawList.Flags;
    aDrawList.Flags = savedLineFlags & ~ImDrawListFlags_AntiAliasedLines;
    // The hairline UNDER the band, in the same 1px integer-column style: the timeline
    // draws its ruler line on the first track row below the header
    // (TimeLineEditorWidget.cpp's paintEvent), so this is that row.
    if (!aModel.rulerBand.isEmpty() && aModel.rulerBand.bottom() + 1 <= plot.bottom()) {
        const float hairlineY = float(aModel.rulerBand.bottom() + 1);
        aDrawList.AddLine(
            ImVec2(float(plot.left()), hairlineY), ImVec2(float(plot.right() + 1), hairlineY),
            toImU32(aTheme.rulerLine()), 1.0f
        );
    }
    // ...UP from the ruler band's LAST ROW, exactly as the timeline's ruler draws them
    // (TimeRuler.cpp:110-112, :281: the baseline is the band's bottom row and a tick
    // rises kTickLength px from it). The band moved down under an occluding widget
    // moves the ticks with it, and the ticks are its bottom edge (see THE OCCLUDED TOP
    // in GraphPanelModel.h). The band is inside the plot by construction.
    const float bandBaseline = float(rulerBand.bottom());
    // One mark per tick, each at its own tier's alpha from the model's classification
    // (walked by index, so the two vectors are read as one geometry).
    for (int i = 0; i < aModel.frameTickX.size(); ++i) {
        const float fx = float(aModel.frameTickX.at(i));
        aDrawList.AddLine(
            ImVec2(fx, bandBaseline), ImVec2(fx, bandBaseline - float(kAxisTickLength)),
            toImU32Strength(aTheme.gridMajor(), tickAlphaOf(aModel, i)), 1.0f
        );
    }
    for (int y : aModel.valueTickY) {
        const float fy = float(y);
        aDrawList.AddLine(
            ImVec2(float(plot.left()), fy), ImVec2(float(plot.left() + kAxisTickLength), fy), valueTick, 1.0f
        );
    }

    // The zero rule, only while 0 is inside the value window (the model publishes
    // zeroY exactly then).
    if (aModel.zeroY >= 0) {
        aDrawList.AddLine(
            ImVec2(float(plot.left()), float(aModel.zeroY)), ImVec2(float(plot.right() + 1), float(aModel.zeroY)),
            toImU32(aTheme.axisLine()), 1.0f
        );
    }
    aDrawList.Flags = savedLineFlags;

    // The frame numbers, centred in the boxes the model nudged inside the plot.
    for (const PanelLabel& label : aModel.frameLabels)
        drawLabel(aDrawList, font, fontSize, label.rect, label.text, LabelAlign::Centre, axisText, halo);

    // The curves. Clipped to the plot (above); the channels that own a SELECTED
    // marker - or the same key on a sibling channel - at full colour, every other
    // curve dimmed, and with an empty selection every curve is dimmed
    // (curveStrength).
    //
    // THE SHIPPING STROKE IS gui::stroke's (see the header): one draw-list callback per
    // curve, carrying the polyline, the colour and the strength, so the ink is measured
    // PER FRAGMENT and its width stops depending on the angle - and so the pass sits in
    // the draw list's own order, over the ticks and under the key markers below. With the
    // pass unavailable (init() refused, or a caller that never built it) this is the
    // ImDrawList::AddPolyline path, which is exactly what the panel shipped before: a
    // panel drawn with the fringe stroke beats a panel with no curves.
    for (const PanelCurve& curve : aModel.curves) {
        if (curve.points.size() < 2)
            continue; // identity and colour but no geometry: the keys are outside the window

        const float strength = curveStrength(curve);

        if (stroke::isReady()) {
            addInkCallback(aDrawList, curve.points, curve.color, strength, kCurveWidth * 0.5f);
            continue;
        }

        // A stack buffer, so the common curve converts without touching the heap.
        QVarLengthArray<ImVec2, 256> points(int(curve.points.size()));
        for (int i = 0; i < curve.points.size(); ++i) {
            const QPointF& point = curve.points.at(i);
            points[i] = ImVec2(float(point.x()), float(point.y()));
        }
        aDrawList.AddPolyline(
            points.constData(), int(points.size()), toImU32Strength(curve.color, strength), ImDrawFlags_None,
            kCurveWidth
        );
    }

    // The key MARKERS, from the model's own centres (PanelCurve::keyDots): a DOT
    // of radius ctrl::graph::kKeyDotRadius - the timeline's own key-body radius
    // (ctrl::keyRadius, 3.75px) - filled with ITS CHANNEL'S OWN COLOUR
    // (PanelCurve::color, i.e. theme::Graph::keyColor(channel.type()) - the same
    // colour its curve is stroked in and its chip's text drawn in), inside the
    // palette's floor-coloured ring, which is what keeps a key visible on top of the
    // curve that carries it (a same-hue key on a same-hue stroke would otherwise
    // disappear into it).
    //
    // THE THREE STATES, in the model's own classification (PanelCurve::keySelected /
    // keySiblingSelected) and with the ink this layer gives each (see the marker
    // rule's tokens above for where every one of them comes from):
    //   * SELECTED - the fill is the channel's hue mixed 0.45 toward the text colour,
    //     and a 1px ring in the CHANNEL'S OWN hue at 0.85 sits 2.6px clear of the
    //     marker, drawn outside its radius. The marker's own SIZE does not change: the
    //     fill keeps the radius and the keyEdge() stroke every marker already has
    //     (that stroke is drawn outside the radius for every marker, selected or not).
    //   * SIBLING (not selected, its key selected on another component) - the faint
    //     ring alone, at the SAME radius as the selection ring, in the channel's own
    //     hue at 0.35, and the fill is left exactly as an unselected marker's is
    //     (that channel is in play, so it is at full strength). One key, two
    //     channels, both in play - but the selection ring is never on the marker that
    //     is not selected. The two rings share a hue by design; their ALPHA is what
    //     tells the in-play sibling from the selected marker.
    //   * HOVERED (the marker gui::panelKeyAt names under the pointer, and not
    //     selected) - the fill brightens at the mock's lighter step and NO ring is
    //     added. Hover COMPOSES with the sibling state (the mock draws both: the
    //     faint ring stays, the fill brightens), and it loses to selection, which is
    //     the mock's own order.
    // The marker's floor ring (keyEdge) is drawn for every marker whether or not the
    // pointer is on it - the pointer never changes a marker's SIZE or its edges.
    //
    // THE MARKER IS NOT THE PICK AREA, and that is the timeline's own rule for a
    // small element ("hit testing and visuals are two separate things"): the editor
    // grabs a key within ctrl::graph::kKeyHitRadius (10px) of this centre, and the
    // model's `keys` rect is that rule's square form, so a 10px square used to be
    // drawn where a 7.5px dot belongs - 2.3x the timeline's key area. The ink here
    // is the marker; the hit stays where the editor's hit-test puts it.
    const float dotRadius = float(ctrl::graph::kKeyDotRadius);
    const float ringRadius = dotRadius + kKeyRingOffsetPx;
    const QColor highlightInk = aTheme.axisText();
    // THE HOVER, by the ONE shared hit test: gui::panelKeyAt answers with the
    // (node, channel, frame) triple the panel names a key by - the same rule (and the
    // same kKeyHitRadius) both hosts open the key menu with - and it reads the model's
    // own keyDots/keyFrames, so the marker the pointer finds and the marker this pass
    // inks are the same key by construction. Under the pointer AND not selected, the
    // three conditions the mock's `hov` line states.
    core::ObjectNode* hoverNode = nullptr;
    ctrl::graph::Channel hoverChannel;
    core::Frame hoverFrame;
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const bool hovered = panelKeyAt(
        aModel, QPoint(int(pointer.x), int(pointer.y)), hoverNode, hoverChannel, hoverFrame
    );
    for (const PanelCurve& curve : aModel.curves) {
        // The markers follow their own curve's emphasis (curveStrength): a channel
        // that owns a selected marker, or a selected sibling of one, draws its dots at
        // full strength - every other channel's are dimmed.
        const float strength = curveStrength(curve);
        const ImU32 fill = toImU32Strength(curve.color, strength);
        const ImU32 edge = toImU32Strength(aTheme.keyEdge(), strength);
        const ImU32 siblingRing = toImU32Strength(curve.color, kKeySiblingRingAlpha);
        const ImU32 selectedRing = toImU32Strength(curve.color, kKeySelectedRingAlpha);
        const int keys = int(curve.keyDots.size());
        for (int k = 0; k < keys; ++k) {
            const QPointF& dot = curve.keyDots.at(k);
            // The flags are published beside the geometry and are the same length (see
            // the header), but a hand-built model - a pixel gate's, or a caller that
            // only carries geometry - may publish the dots alone: a missing flag is
            // "neither state", which is what a model that says nothing about the
            // selection means.
            const bool selected = k < curve.keySelected.size() && curve.keySelected.at(k);
            const bool sibling = k < curve.keySiblingSelected.size() && curve.keySiblingSelected.at(k);
            const bool hoveredHere = hovered && !selected && k < curve.keyFrames.size()
                && curve.node == hoverNode && curve.channel == hoverChannel
                && curve.keyFrames.at(k) == hoverFrame;
            const ImVec2 centre(float(dot.x()), float(dot.y()));
            aDrawList.AddCircleFilled(
                centre, dotRadius,
                selected ? toImU32(mixedToward(curve.color, highlightInk, kKeySelectedFillMix))
                         : hoveredHere ? toImU32(mixedToward(curve.color, highlightInk, kKeyHoverFillMix))
                                       : fill
            );
            // THE EDGE STROKE IS CENTRED ON THE REFERENCE'S RADIUS. ImGui's AddCircle
            // strokes INWARD from the radius it is given (a 1px circle at r covers
            // [r-1, r]), so r on its own would draw the whole band inside the fill. At
            // r + 0.5 the band is [r-0.5, r+0.5] - the same place a canvas stroke at r
            // puts it, which is how the reference draws a key's separating edge. This
            // edge is identical on every marker, selected or not; the ring that must be
            // OUTSIDE is the selection ring below (that inner placement is what made a
            // selected key read smaller).
            aDrawList.AddCircle(centre, dotRadius + 0.5f, edge, 0, 1.0f);
            if (sibling && !selected) {
                // The faint ring, at the selection ring's own radius: the same
                // geometry, so the two can only be told apart by their alpha.
                aDrawList.AddCircle(centre, ringRadius, siblingRing, 0, kKeyRingWidth);
            }
            if (selected) {
                // The rule's ring: the CHANNEL's own hue, 1px, 2.6px clear of the
                // marker's radius - drawn outside it, so it never eats the fill.
                aDrawList.AddCircle(centre, ringRadius, selectedRing, 0, kKeyRingWidth);
            }
        }
    }

    // The Custom tangent handles, from the model's geometry: a stem from each
    // handle's base to its dot, then the dot itself. The dot NEAREST the pointer,
    // within ctrl::graph::kHandleHitRadius, is drawn in the selected token instead
    // - the same distance and the same `<=` the editor's hit-test grabs through, so
    // the highlight names exactly the handle a grab would take. The handles are
    // inside the plot clip (pushed above), so a stem that leaves the plot is cut at
    // the edge rather than painting over the panel's chrome.
    //
    // THEY ARE DRAWN FOR THE FOCUSED CHANNEL, and that is a COST decision with a
    // measured number behind it (the application-level performance diagnosis, on this
    // build and this machine): a key costs ~68 vertices with its tangent handles drawn
    // and ~16 without - the handles are the delta, 2 stems, 2 filled dots and 2
    // circles - at ~50ns of Debug-build CPU per vertex, which is 2.6ms/frame at 600
    // keys, 32ms at 8000 and 165ms at 40000, paid on every viewport repaint while the
    // panel is up. Drawing them for the focused channel alone measured 4.2x fewer
    // vertices and 4.4x less frame time in that diagnosis.
    //
    // WHAT THAT COSTS, stated rather than implied: a handle on a curve the user has
    // NOT focused is not on screen, so a pointer hovering it draws no highlight -
    // and a press there still GRABS it, because the editor's hit-test is its own
    // geometry and a handle press focuses that channel first (GraphEditor::
    // updateCursor), so the handle is drawn - and highlighted - from that frame on.
    // With NO focus at all every curve draws its handles, so nothing is hidden
    // before the user has said which channel they are working on (and the pixel
    // gate's own passes, which hand drawPanel an invalid focus, keep measuring the
    // full geometry).
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const auto drawsHandles = [&aFocus](const PanelCurve& aCurve) {
            return !aFocus.valid || (aFocus.node == aCurve.node && aFocus.channel == aCurve.channel);
        };
        int nearestCurve = -1;
        int nearestHandle = -1;
        double nearestDistance = double(ctrl::graph::kHandleHitRadius);
        for (int c = 0; c < aModel.curves.size(); ++c) {
            const PanelCurve& curve = aModel.curves.at(c);
            if (!drawsHandles(curve))
                continue;
            for (int h = 0; h < curve.handles.size(); ++h) {
                const QPointF& dot = curve.handles.at(h).dot;
                const double distance = std::hypot(double(mouse.x) - dot.x(), double(mouse.y) - dot.y());
                if (distance <= nearestDistance) { // `<=`, like the editor's own pick
                    nearestDistance = distance;
                    nearestCurve = c;
                    nearestHandle = h;
                }
            }
        }

        const ImU32 handleFill = toImU32(aTheme.handleFill());
        const ImU32 handleSelected = toImU32(aTheme.selected());
        for (int c = 0; c < aModel.curves.size(); ++c) {
            const PanelCurve& curve = aModel.curves.at(c);
            if (!drawsHandles(curve))
                continue;
            for (int h = 0; h < curve.handles.size(); ++h) {
                const PanelHandle& handle = curve.handles.at(h);
                const bool highlighted = c == nearestCurve && h == nearestHandle;
                const ImVec2 base(float(handle.base.x()), float(handle.base.y()));
                const ImVec2 dot(float(handle.dot.x()), float(handle.dot.y()));
                // BOTH KINDS READ THE CURVE'S CHANNEL COLOUR (mock-graph.js:886-905):
                // a stored pair's stem and stroke are that hue at 0.85 / 0.9, a ghost
                // pair's at 0.5 / 0.7 - so a handle, the curve it belongs to and the
                // key it leaves are one hue, and only the ALPHAS and the fill say
                // whether the tangent is in the document. A ghost is drawn HOLLOW with
                // no fill at all; only a stored pair is filled. (The pointer's
                // highlight still wins: the dot and stroke under it take the accent.)
                // THE STEM'S INK IS A QColor, NOT ITS PACKED FORM. addInkCallback takes
                // the colour itself; handing it the ImU32 would reach QColor(QRgb) -
                // ImU32 and QRgb are both `unsigned int`, so it COMPILES - and ImGui
                // packs IM_COL32 as A<<24|B<<16|G<<8|R while QColor(QRgb) reads
                // A<<24|R<<16|G<<8|B, i.e. the red and blue channels swap. The ghost
                // stem is already the channel's own hue, and the stored one is now too,
                // so both branches pass the colour and the strength carries the alpha.
                const float stemStrength = handle.ghost ? kGhostHandleStemAlpha : kStoredHandleStemAlpha;
                const ImU32 stem = toImU32Strength(curve.color, stemStrength);
                const float radius =
                    handle.ghost ? ImMax(0.0f, float(handle.radius) - kGhostHandleShrink) : float(handle.radius);
                const ImU32 edge = highlighted
                    ? handleSelected
                    : toImU32Strength(curve.color, handle.ghost ? kGhostHandleEdgeAlpha : kStoredHandleEdgeAlpha);
                // THE STEM IS A 1px LINE WITH FRACTIONAL ENDS, and it goes through the same
                // coverage pass the curves do - measured first, because it is the one 1px
                // line in the panel whose position can be wrong: ImGui's own AddLine shifts
                // its geometry by (+0.5,+0.5) (imgui_draw.cpp, the 1px rasterization
                // convention), which for a stem at this slope moves the ink up to 0.71px
                // PERPENDICULAR to the line it is supposed to draw - measured on the gate's
                // own fractional stem at +0.68/+0.75/+0.76px along its length, against
                // -0.01/-0.00/+0.01px for this pass. The pass has
                // no such offset: its coverage is the distance to the segment the model
                // asked for. The stem is already antialiased today (the draw list's
                // AntiAliasedLines flag is not turned off here), so nothing about its
                // crispness changes; only where its ink sits.
                if (stroke::isReady()) {
                    addInkCallback(
                        aDrawList, QVector<QPointF>{handle.base, handle.dot}, curve.color, stemStrength, 0.5f
                    );
                } else {
                    aDrawList.AddLine(base, dot, stem, 1.0f);
                }
                if (!handle.ghost)
                    aDrawList.AddCircleFilled(dot, radius, highlighted ? handleSelected : handleFill);
                aDrawList.AddCircle(dot, radius, edge, 0, 1.0f);
            }
        }
    }

    // The live box-select marquee, when a drag is in flight. It is the timeline's
    // own gesture and is drawn with the timeline's own colours
    // (theme::Graph::marqueeEdge()/marqueeFill(), ctrl/time/time_Renderer.cpp's
    // renderSelectionRange): a rounded (6px) accent body at low alpha under a 1px
    // accent edge. ImGui's draw list has no dashed stroke, so the edge is solid -
    // the one thing about the timeline's box that cannot be reproduced here.
    // It is drawn INSIDE the plot clip (pushed above), and the playhead goes on top
    // of it, so a drag never paints over the axes or the value column.
    if (!aModel.marquee.isEmpty()) {
        const ImVec2 min = rectMin(aModel.marquee);
        const ImVec2 max = rectMax(aModel.marquee);
        aDrawList.AddRectFilled(min, max, toImU32(aTheme.marqueeFill()), kMarqueeRounding);
        aDrawList.AddRect(min, max, toImU32(aTheme.marqueeEdge()), kMarqueeRounding, ImDrawFlags_None, 1.0f);
    }

    // The playhead: the document's own frame, marked the way the TIMELINE's ruler
    // marks it. The 1px line is theme::Graph::selected() (accentBright, the same
    // colour TimeCursor uses), and the frame NUMBER is the model's playheadText in a
    // rounded accent pill at the band's number row - `playheadPill`, whose width the
    // model measured from that same string. The line starts at `playheadLineTop`,
    // under the pill (TimeCursor's line starts at pillTop + kNumberHeight), so the
    // pill is the top of the playhead rather than a badge painted over the line.
    //
    // It covers the plot's rows EXACTLY. ImGui's non-AA 1px path is half-open: it
    // shifts the endpoints by half a pixel and loses the first row, so a line passed
    // as (top, bottom+1) inks rows top+1..bottom and leaves the first row empty. The
    // half-pixel inset below is what makes the drawn column cover the rows the model
    // names - measured by the gate's playhead assertion
    // (tests/graph_panel_pixels.cpp), which walks the span.
    if (aModel.playheadX >= 0) {
        const float px = float(aModel.playheadX);
        const float lineTop = float(aModel.playheadLineTop >= 0 ? aModel.playheadLineTop : plot.top());
        const ImDrawListFlags savedPlayheadFlags = aDrawList.Flags;
        aDrawList.Flags = savedPlayheadFlags & ~ImDrawListFlags_AntiAliasedLines;
        aDrawList.AddLine(
            ImVec2(px, lineTop - 0.5f), ImVec2(px, float(plot.bottom()) + 0.5f), toImU32(aTheme.selected()),
            1.0f
        );
        aDrawList.Flags = savedPlayheadFlags;

        if (!aModel.playheadPill.isEmpty() && !aModel.playheadText.isEmpty()) {
            const QByteArray utf8 = aModel.playheadText.toUtf8();
            const ImVec2 textSize = measureText(font, fontSize, utf8.constData());
            // TimeCursor draws the number on the ruler's own number row, centred on the
            // playhead's column; drawLabel's centre alignment is the same placement.
            const QRect numberBox(
                int(std::lround(px - textSize.x * 0.5f)), aModel.playheadPill.top(), int(std::ceil(textSize.x)),
                aModel.playheadPill.height()
            );
            aDrawList.AddRectFilled(
                rectMin(aModel.playheadPill), rectMax(aModel.playheadPill), toImU32(aTheme.selected()),
                float(gui::frameRuler::kPillRadius)
            );
            drawLabel(
                aDrawList, font, fontSize, numberBox, aModel.playheadText, LabelAlign::Centre,
                toImU32(aTheme.accentBrightText()), halo
            );
        }
    }

    // The name CHIPS: one for every curve IN PLAY - the channels that own a selected
    // key and whatever curve the pointer is over - and for nothing else. Six
    // permanent chips would be the same wall of tokens the deleted corner legend was;
    // this is the subset being edited, named AT the curve.
    //
    // WHICH curves are in play is EMPHASIS, and emphasis is this layer's decision
    // (the model publishes a chip for every curve - see its CHIPS note): a curve is
    // in play when curveStrength() gives it full strength, i.e. when its channel owns
    // a selected marker OR the same key is selected on a sibling channel, which is the
    // reference's own _live() rule - plus the hovered
    // one, which is not a selection fact at all. With an EMPTY selection no curve is
    // in play and no chip is drawn, exactly as every curve is dimmed then.
    //
    // The pointer's curve is the one whose VALUE AT THE POINTER's own column is
    // within kChipHoverDistance of the pointer - the reference's rule, and the reason
    // a chip can appear while the pointer is merely near the curve (mock-graph.js's
    // _move: |valueAt(x) - y| <= 6). The polyline is one sample per ~2px, so the
    // sample nearest the pointer's x IS the value at that column.
    if (!aModel.curves.isEmpty()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hovered = -1;
        if (plot.contains(QPoint(int(mouse.x), int(mouse.y)))) {
            double best = kChipHoverDistance;
            for (int c = 0; c < aModel.curves.size(); ++c) {
                const PanelCurve& curve = aModel.curves.at(c);
                if (curve.points.isEmpty())
                    continue;
                int nearest = 0;
                double nearestDx = std::abs(curve.points.first().x() - double(mouse.x));
                for (int i = 1; i < curve.points.size(); ++i) {
                    const double dx = std::abs(curve.points.at(i).x() - double(mouse.x));
                    if (dx <= nearestDx) {
                        nearestDx = dx;
                        nearest = i;
                    }
                }
                const double dy = std::abs(curve.points.at(nearest).y() - double(mouse.y));
                if (dy <= best) { // `<=`, so the LAST curve at the same distance wins, like the reference
                    best = dy;
                    hovered = c;
                }
            }
        }

        // The chips to draw, each at the box the model measured (an empty box is not
        // a chip: no room for it inside the plot). Their order is the curves', then
        // pushed apart top-to-bottom so two names cannot overlap - the reference's own
        // de-collide, which keeps the order and moves the LOWER chip down by a line.
        // The push is by the chip's own height, so the same rule holds for any font.
        QVector<int> chips;
        QVector<QRect> boxes(aModel.curves.size()); // indexed BY CURVE, so the sort below can look one up
        for (int c = 0; c < aModel.curves.size(); ++c) {
            const PanelCurve& curve = aModel.curves.at(c);
            if (curve.chip.isEmpty() || (c != hovered && curveStrength(curve) < 1.0f))
                continue;
            chips.append(c);
            boxes[c] = curve.chip;
        }
        std::sort(chips.begin(), chips.end(), [&boxes](int aLhs, int aRhs) {
            return boxes.at(aLhs).top() < boxes.at(aRhs).top();
        });
        for (int i = 1; i < chips.size(); ++i) {
            const QRect& previous = boxes.at(chips.at(i - 1));
            QRect& box = boxes[chips[i]];
            if (box.top() < previous.top() + previous.height())
                box.moveTop(previous.top() + previous.height());
        }

        // AND THE PUSH HAS TO END INSIDE THE PLOT. Every box the model publishes is
        // inside the plot (GraphPanelModel.cpp's buildCurves tests exactly that before
        // it publishes one), so the de-collide above is the only thing in this frame
        // that can move a chip out - and it moves each chip DOWN, so a crowded
        // selection leaves the last names past plot.bottom(), where the plot clip cuts
        // them in half. A chip that no longer fits is therefore DROPPED for this
        // frame rather than drawn clipped, which is the reference's own answer to the
        // edge: mock-graph.js's _curveLabels draws no item whose y is within 6px of
        // the plot's top or bottom edge, and it never moves one back in - the mock
        // SKIPS a chip that does not fit; it does not clamp it. The test is the
        // model's own plot.contains(box), so "fits" means one thing in both files.
        // (The dropped chip leaves the line it had taken empty: the survivors keep the
        // positions the full list gave them, so the column cannot shift from frame to
        // frame as names appear and disappear.)
        chips.erase(
            std::remove_if(
                chips.begin(), chips.end(),
                [&boxes, &plot](int aCurve) { return !plot.contains(boxes.at(aCurve)); }
            ),
            chips.end()
        );

        for (int i = 0; i < chips.size(); ++i) {
            const PanelCurve& curve = aModel.curves.at(chips.at(i));
            const QRect& box = boxes.at(chips.at(i));
            // The chip carries its OWN backing: the name has to be readable wherever
            // the curve leaves the window, and the panel has no background otherwise.
            aDrawList.AddRectFilled(
                rectMin(box), rectMax(box), toImU32Strength(aTheme.halo(), kChipBackdropAlpha), kChipRounding
            );
            // A channel that owns a selected key reads at full strength even while the
            // pointer is on it; only a chip that is there because of the POINTER is
            // dimmed (the reference's own order: live wins over hover).
            const float strength = curveStrength(curve) >= 1.0f ? 1.0f : kChipHoverAlpha;
            drawLabel(
                aDrawList, font, fontSize, box, curve.chipText, LabelAlign::Centre,
                toImU32Strength(curve.color, strength), halo
            );
        }
    }

    // Why the panel is empty, in the middle of the plot: an empty panel has to say
    // so, and this is the only place it can.
    if (!aModel.emptyState.isEmpty())
        drawLabel(aDrawList, font, fontSize, plot, aModel.emptyState, LabelAlign::Centre, axisText, halo);

    aDrawList.PopClipRect();

    // The value numbers live in the column LEFT of the plot, so they are drawn after
    // the clip: the model already clamped each box into the column, and a label must
    // not be cut off by an edge it was deliberately kept clear of.
    //
    // TWO THINGS HAPPEN HERE, and they are different obligations:
    //   * the GUTTER (kValueLabelGap). A right-aligned number keeps that much space
    //     from the box's right edge whenever the box can spare it, so the top value
    //     label and the plot's first frame number do not read as one string - the
    //     measured defect, and the timeline's own kNumberMargin for it.
    //   * the GUARD. The whole pass is clipped to the PANEL, so a label wider than
    //     its box (the model's column is measured so that it cannot be, but a
    //     drawing font wider than the model's can make one) is cut at the panel's
    //     edge instead of painting outside the panel - the one place in this
    //     function whose ink is outside every other clip.
    aDrawList.PushClipRect(rectMin(aModel.panel), rectMax(aModel.panel), true);
    for (const PanelLabel& label : aModel.valueLabels)
        drawLabel(
            aDrawList, font, fontSize, label.rect, label.text, LabelAlign::Right, axisText, halo, kValueLabelGap
        );
    aDrawList.PopClipRect();
}


namespace {

// The frame delta ImGui animates with, clamped so a stall (a load, an attached
// debugger) cannot make an animation jump and a burst of repaints cannot make it
// crawl. The first frame has no interval to measure, so it gets 60Hz.
float nextDeltaTime(QElapsedTimer& aTimer, bool& aStarted) {
    if (!aStarted) {
        aTimer.start();
        aStarted = true;
        return 1.0f / 60.0f;
    }
    const qint64 nanoseconds = aTimer.nsecsElapsed();
    aTimer.restart();
    return float(qBound(qint64(1000000), nanoseconds, qint64(100000000))) / 1.0e9f;
}

// The editor's sticky FOCUS, in the currency drawPanel uses - and nothing else.
//
// It used to carry the selection too, as a list of (key type, pixel x) pairs matched
// back to a marker. That list is gone: the MODEL now publishes which marker is
// selected and which marker's key has a selected sibling (PanelCurve::keySelected /
// keySiblingSelected, read from this same editor in buildGraphPanelModel), and a
// second copy of the same fact here - mapped by hand through the scaler, one entry
// per selected target, with the model's panel re-applied to the editor's scaler -
// was a copy that could disagree with the flags the model had just published. The
// focus stays because the handles are still the focus's rule (see PanelFocus).
PanelFocus buildFocus(ctrl::GraphEditor& aEditor) {
    PanelFocus focus;

    if (aEditor.hasFocus() && aEditor.focusedNode()) {
        focus.node = const_cast<core::ObjectNode*>(aEditor.focusedNode());
        focus.channel = aEditor.focusedChannel();
        focus.valid = true;
    }

    return focus;
}

} // namespace

//-------------------------------------------------------------------------------------------------
// THE SHARED FRAME (see the frame driver in the header): the engine, the accessibility
// publication and the registry that tells the two hosts apart.
//-------------------------------------------------------------------------------------------------
bool panelSelectedKeyDot(const GraphPanelModel& aModel, const core::TimeLineEvent& aTargets, QPoint& aDot) {
    if (aTargets.targets().size() != 1)
        return false;

    // The triple the target carries IS a key's identity - (node, key type, frame) -
    // and PanelCurve publishes the other half of it (keyFrames, keyDots) beside each
    // other, so the marker is found rather than re-derived: a consumer that recomputed
    // the pixel would be a second copy of the scaler's arithmetic.
    const core::TimeLineEvent::Target& target = aTargets.targets().first();
    if (!target.node || target.pos.isNull())
        return false;
    for (const PanelCurve& curve : aModel.curves) {
        if (curve.node != target.node || curve.channel.type() != target.pos.type())
            continue;
        for (int i = 0; i < curve.keyFrames.size() && i < curve.keyDots.size(); ++i) {
            if (curve.keyFrames.at(i).get() != target.pos.index())
                continue;
            aDot = QPoint(int(curve.keyDots.at(i).x()), int(curve.keyDots.at(i).y()));
            return true;
        }
    }
    return false;
}

bool panelKeyAt(
    const GraphPanelModel& aModel, const QPoint& aPos, core::ObjectNode*& aNode,
    ctrl::graph::Channel& aChannel, core::Frame& aFrame
) {
    aNode = nullptr;
    double nearest = double(ctrl::graph::kKeyHitRadius);
    for (const PanelCurve& curve : aModel.curves) {
        // keyFrames is published beside keyDots and has the same length (see
        // GraphPanelModel.h); a model that carries one without the other is not one
        // buildGraphPanelModel makes, and it is not one a key can be named from either,
        // so it is skipped rather than guessed at.
        if (curve.keyFrames.size() != curve.keyDots.size())
            continue;
        for (int i = 0; i < curve.keyDots.size(); ++i) {
            const QPointF& dot = curve.keyDots.at(i);
            const double distance = std::hypot(double(aPos.x()) - dot.x(), double(aPos.y()) - dot.y());
            if (distance <= nearest) { // `<=`, so the last key at the same distance wins
                nearest = distance;
                aNode = curve.node;
                aChannel = curve.channel;
                aFrame = curve.keyFrames.at(i);
            }
        }
    }
    return aNode != nullptr;
}

namespace {

// ---- the accessibility publication's own state (see publishPanelAccessibility) ----
// The widget the panel's ONE a11y installation answers for, the callback installed for
// it, and the docked host once it has drawn a frame (from which point the dock owns the
// publication). QPointers: a host going away needs no unwinding here.
QPointer<QWidget> gPanelA11yHost;
void (*gPanelA11yAction)(void*, int) = nullptr;
void* gPanelA11yUser = nullptr;
QPointer<QWidget> gPanelDockPublisher;

// The model the panel publishes to the accessibility tree, from the frame's own model
// and the editor it was built from (null while the panel is not on screen).
//
// THE PANEL PUBLISHES NO CONTROL OF ITS OWN ANY MORE: the button that opens the channel
// list is the DOCK HEADER's (TimeLineHeader's own Qt widget, outside this tree's panel),
// and the design removed the drawn selection count and drag readout as well. What the
// panel still says is STATE: the summary line below names how many channels are shown,
// how many keys the live selection names and which channel is focused - plus the two
// actions it owns (fit, and the key menu for the selected key).
//
// AND THE LIST ITSELF IS NOT IN THE TREE, exactly like the key menu: it is the
// application's own QMenu (a native window). What the tree says instead is the state
// the list changes, and the summary line below names how many channels are shown and
// which one is focused; the channels the panel cannot list are a count in that same
// line rather than a rack of invisible check boxes.
a11y::Model buildAccessibilityModel(const GraphPanelModel& aModel, ctrl::GraphEditor* aEditor) {
    a11y::Model published;
    published.panel = aModel.panel;

    if (!aEditor) {
        // The panel is not on screen. The host stays a named pane with no content,
        // which is what it is.
        return published;
    }

    // The panel in one line: its own reason while it is empty (the model states it
    // in words), otherwise how much is shown - plus how many keys the live selection
    // names (when it names any) and the focused channel, which is the one piece of
    // the panel's state with no control of its own.
    QString summary;
    if (!aModel.emptyState.isEmpty())
        summary = aModel.emptyState;
    else
        summary = trText(QT_TRANSLATE_N_NOOP("gui::ImGuiOverlay", "%n channel(s) shown"), int(aModel.curves.size()));

    // THE SELECTION'S COUNT, which is the one thing the panel used to DRAW and now
    // only says: the count label that sat above the selection's hull is gone (design
    // decision - the panel prints no gesture numbers), so the number reaches assistive
    // technology from here instead. This is the whole of what a screen reader gets for
    // it: the sentence rides the summary, which is BOTH the root Pane's Name and the
    // StaticText item below (see THE PANEL'S NAME), and it is added only while
    // something IS selected - "0 keys selected" would be a sentence about nothing.
    if (aModel.selectionCount > 0) {
        summary = trText(QT_TRANSLATE_N_NOOP("gui::ImGuiOverlay", "%1, %n key(s) selected"), aModel.selectionCount)
                      .arg(summary);
    }

    if (aEditor->hasFocus() && aEditor->focusedNode()) {
        const QString label =
            aEditor->focusedNode()->name() + QStringLiteral(" : ") + aEditor->focusedChannel().name();
        summary = QCoreApplication::translate(
                      kTranslationContext, QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "%1, focused: %2")
        )
                      .arg(summary, label);
    }

    // THE SELECTED KEY, when the graph's selection names one: which key it is and
    // which easing it carries. The key menu acts on that selection, and the menu is
    // a QMenu - it is not in this tree, like the repository's own QMenus (see the
    // class comment) - so this line is how the state the menu changes stays
    // readable: choose an easing from it and the panel says so. The easing name is
    // the timeline's own (util::Easing::getTypeName, the "EasingName" context), so
    // the two agree in every translation.
    //
    // AND THE MENU'S OWN ACTION IS PUBLISHED from the same selection, one item
    // below: a menu a screen reader cannot address still needs a route to OPEN it.
    core::TimeLineEvent selection;
    QPoint selectedDot;
    const bool oneSelectedKey =
        aEditor->retrieveSelection(selection) && panelSelectedKeyDot(aModel, selection, selectedDot);
    if (oneSelectedKey) {
        const core::TimeLineEvent::Target& target = selection.targets().first();
        const core::TimeLine* line = target.pos.line();
        const core::TimeKey* key = line ? line->timeKey(target.pos.type(), target.pos.index()) : nullptr;
        const util::Easing::Param* easing =
            key ? ctrl::graph::Curve::easingOf(*key, target.pos.type()) : nullptr;
        if (easing) {
            const QString keyLabel = core::TimeLine::getTimeKeyName(target.pos.type()) + QStringLiteral(" ")
                + util::Easing::getTypeName(easing->type);
            summary = QCoreApplication::translate(
                          kTranslationContext, QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "%1, selected key: %2")
            )
                          .arg(summary, keyLabel);
        }
    }

    // THE PANEL'S NAME, which is the same line: gui::a11y's Model carries the summary
    // as the ROOT Pane's Name (ImGuiA11y.h: "the Model itself -> the viewport widget's
    // interface: QAccessible::Pane ..., its Name is Model::summary"), and that is what a
    // client reads as the region's label before it walks the children. The previous
    // caller never assigned it, so the panel was published as an UNNAMED pane and the
    // summary was only reachable as the StaticText item below; both places say the same
    // thing on purpose, exactly as tests/imgui_a11y.cpp asserts (a named root whose
    // childCount is the item count).
    published.summary = summary;

    a11y::Item summaryItem;
    summaryItem.role = a11y::Item::StaticText;
    summaryItem.id = kPanelSummaryItemId;
    summaryItem.name = summary;
    summaryItem.rect = aModel.plot.isEmpty() ? aModel.panel : aModel.plot;
    published.items.append(summaryItem);

    // The fit action. Its rectangle is the plot because the gesture that performs it
    // is the double-click on the plot: an assistive tool activates the action
    // directly (the host's own handler runs) rather than clicking, and a click where
    // this item says it is lands on the plot, i.e. on the same region the gesture is
    // bound to.
    a11y::Item fitItem;
    fitItem.role = a11y::Item::PushButton;
    fitItem.id = kPanelFitItemId;
    fitItem.name = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "Fit graph"));
    fitItem.value = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "double-click the plot"));
    fitItem.rect = aModel.plot.isEmpty() ? aModel.panel : aModel.plot;
    published.items.append(fitItem);

    // The key menu's action, while the selection names one key the panel draws. Its
    // rectangle is the region the POINTER's gesture works in - the key's centre
    // grown by ctrl::graph::kKeyHitRadius, the distance the right-click is tested
    // against - so a click where this item says it is does what this item does, the
    // same rule the fit action follows. Nothing is published when no key is
    // selected, or when the selected key is not in the drawn model (outside the
    // plot's window, or on a hidden channel): an item whose action had no key to
    // open a menu on would be a control that does nothing.
    if (oneSelectedKey) {
        a11y::Item menuItem;
        menuItem.role = a11y::Item::PushButton;
        menuItem.id = kPanelKeyMenuItemId;
        menuItem.name = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "Key menu"));
        menuItem.value = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "right-click the key"));
        menuItem.rect = QRect(selectedDot, selectedDot).adjusted(
            -ctrl::graph::kKeyHitRadius, -ctrl::graph::kKeyHitRadius, ctrl::graph::kKeyHitRadius,
            ctrl::graph::kKeyHitRadius
        );
        published.items.append(menuItem);
    }

    return published;
}

// ---- the panel's GL objects and the context they belong to (see acquirePanelContext) ----
// How many hosts hold the panel's context: the dock's Graph tab is the only one today,
// and this counter is what lets a second host attach without a second ImGui context if
// one ever returns (see the header).
int gPanelContextHolders = 0;
// THE CONTEXT THE PANEL'S GL OBJECTS WERE CREATED AGAINST, and the guard the panel has
// instead of a second backend. Sharing is NOT enough for these objects: Qt's sharable
// context group gives buffers, textures, shaders and programs, but NOT vertex-array
// objects - a VAO name created in one context is GL_INVALID_OPERATION in another on a
// core profile (measured: a second host of the panel bound the stroke pass's VAO in its
// own context and the very next GL_CHECK_ERROR() aborted on 1282 in a Debug build).
// So the ImGui context, its backend's device objects and both GL passes are re-created
// whenever a frame arrives with a context other than this one - see
// rebindPanelObjects() below, which runPanelFrame calls before it touches anything GL.
QPointer<QOpenGLContext> gPanelOwnerContext;

// Creates the ImGui context, its GL3 backend and the font atlas on aHost's CURRENT
// context, and records that context as the one the objects belong to.
bool createPanelContextOn(QOpenGLWidget& aHost) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // No runtime files, ever: no imgui.ini beside the application and no ImGui log.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // The panel never changes the cursor itself; the viewport's cursor policy
    // (MainDisplayWidget) stays the only thing that does.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // ImGui's style is left at the context's own default (ImGui::CreateContext
    // calls StyleColorsDark itself): the panel draws every colour it uses from the
    // palette into its own draw list, and the menus are the application's QMenus,
    // so nothing the frame draws reads ImGuiStyle at all.

    // The atlas through the Qt/QRawFont loader: the application font at the size Qt
    // RASTERIZES it at, plus the writing-system fallbacks Qt resolves for it, so CJK,
    // `.ttc` and variable faces rasterize with the face the rest of the application
    // draws with.
    //
    // THE SIZE IS panelFontPixelSize, NOT THE FONT'S LINE HEIGHT. The panel drew
    // every numeral ~30% larger than the timeline draws the same font, because
    // QFontMetricsF::height() is the LINE height (ascent + descent + leading:
    // 15.578125px for this application's Geist 9pt at 96dpi) while Qt rasterizes the
    // face at 12px - the size panelFontPixelSize derives, and the size the
    // timeline's own ruler numbers use. See gui/ImGuiQtFontLoader.h for the
    // measurement; 0 means the font stated no size, and the panel keeps its own
    // fallback rather than an atlas with no size at all.
    //
    // The loader's last argument is NOT the widget's device pixel ratio. It is
    // ImFontConfig::RasterizerDensity, and on the dynamic-atlas path (which the GL3
    // backend turns on through ImGuiBackendFlags_RendererHasTextures) ImGui
    // multiplies it by the bake's own density, taken from
    // io.DisplayFramebufferScale - i.e. from the widget's ratio again (imgui.cpp:4605
    // sets g.FontRasterizerDensity from the viewport's FramebufferScale, and
    // imgui_draw.cpp rasterizes at Size * baked->RasterizerDensity *
    // src->RasterizerDensity). Passing the ratio here would square it: 4x the linear
    // raster, and 16x the atlas area per glyph, for a 2x screen.
    //
    // The face is aHost's own font, which is the application's face (the dock's page
    // sets none of its own), so the panel draws the text the timeline draws.
    const float statedSize = imgui_qt::panelFontPixelSize(aHost.font(), aHost.logicalDpiY());
    const float pixelSize = (statedSize > 0.0f) ? statedSize : kFallbackFontPixelSize;
    if (imgui_qt::installQtFontLoader(*io.Fonts, aHost.font(), pixelSize, 1) == 0) {
        // No Qt-rasterized source: the bundled face could not be read, or Qt has no
        // face for any writing system. ImGui installs its own default font when the
        // first frame builds an empty atlas (imgui_draw.cpp, ImFontAtlasBuildMain),
        // so the panel still draws - in the wrong face for anything but Latin. A
        // panel drawn at all beats no panel, so this is not a failure.
    }

    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        // Nothing to unwind beyond the context: the atlas and the loader state live
        // inside it.
        ImGui::DestroyContext();
        return false;
    }

    gPanelOwnerContext = aHost.context();
    return true;
}

// Drops every GL object the panel owns - the two passes and the ImGui context with its
// backend and atlas - on the context that is CURRENT. The caller guarantees one: this is
// only ever called from a paint/initialize path, or from a teardown that made the host's
// context current first.
void destroyPanelObjects() {
    stroke::shutdown();
    outline::shutdown();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
    gPanelOwnerContext.clear();
}

// THE PER-CONTEXT GUARD: if the panel's objects belong to another context than the one
// the frame is about to be drawn on, drop them and build them again HERE (see
// gPanelOwnerContext for the measured failure this replaces). Idempotent, and a no-op on
// every frame of a single host, because a host's context does not change under it.
bool rebindPanelObjects(QOpenGLWidget& aHost) {
    if (gPanelContextHolders <= 0 || gPanelOwnerContext == aHost.context())
        return true;

    if (gPanelOwnerContext) {
        // A real change of context (or a second host on its own context). Said out loud
        // because it means the objects are being built twice in one session - correct,
        // but worth a line in a log when a host is added. A failed creation below leaves
        // gPanelOwnerContext null, so the retry on the next frame does not repeat this.
        qWarning(
            "gui: the panel is being drawn with a different OpenGL context than the one its ImGui "
            "context, backend and GL passes were created on; re-creating them on this context "
            "(vertex-array objects are not shared between contexts, even in one window)."
        );
        destroyPanelObjects();
    }
    return createPanelContextOn(aHost);
}

} // namespace

bool acquirePanelContext(QOpenGLWidget& aHost) {
    if (gPanelContextHolders > 0) {
        // Already up. The objects belong to a context; a host that acquires on a different
        // one gets them re-created on ITS context (rebindPanelObjects), and the frame path
        // makes the same check per frame - so this is belt and braces rather than the only
        // guard.
        if (!rebindPanelObjects(aHost))
            return false;
        ++gPanelContextHolders;
        return true;
    }

    if (!createPanelContextOn(aHost))
        return false;
    ++gPanelContextHolders;
    return true;
}

void releasePanelContext(QOpenGLWidget& aHost) {
    if (gPanelContextHolders <= 0)
        return;
    --gPanelContextHolders;
    if (gPanelContextHolders > 0)
        return; // another host is still drawing with it: nothing here goes yet

    // The LAST holder: the context, its backend and the two GL passes are context
    // objects, so the releasing host's context is made current while they go. A host torn
    // down after its context has gone leaves those few objects to the driver rather than
    // calling into a dead context - the rule this unit always had.
    QOpenGLWidget* widget = &aHost;
    const bool madeCurrent = widget->context() && widget->context()->isValid();
    if (madeCurrent)
        widget->makeCurrent();
    destroyPanelObjects();
    if (madeCurrent)
        widget->doneCurrent();
}

bool panelContextReady() { return gPanelContextHolders > 0 && ImGui::GetCurrentContext() != nullptr; }

ImDrawData* runPanelFrame(
    QOpenGLWidget& aHost, GraphPanelHost aHostStyle, ctrl::GraphEditor* aEditor, bool aVisible,
    PanelFrameState& aState, const QPoint& aPointer
) {
    if (!panelContextReady())
        return nullptr;

    // THE OBJECTS MUST BELONG TO THIS CONTEXT (see gPanelOwnerContext): a frame that
    // arrived with another one would bind VAOs that mean nothing here, which is a GL error
    // and not a sharing problem. Every host goes through this function, so every host is
    // covered.
    if (!rebindPanelObjects(aHost) || !panelContextReady())
        return nullptr;

    const QSize viewport = aHost.size();
    const double dpr = aHost.devicePixelRatioF();
    ImGuiIO& io = ImGui::GetIO();
    // Logical (device independent) pixels, which is the space QMouseEvent::position()
    // and the model's rects are in; ImGui scales the projection by
    // ImDrawData::FramebufferScale itself. THIS HOST'S OWN numbers: the frame lands in
    // this widget's viewport at this widget's ratio.
    io.DisplaySize = ImVec2(float(viewport.width()), float(viewport.height()));
    io.DisplayFramebufferScale = ImVec2(float(dpr > 0.0 ? dpr : 1.0), float(dpr > 0.0 ? dpr : 1.0));
    io.DeltaTime = nextDeltaTime(aState.frameTimer, aState.frameTimerStarted);

    // The backend's own NewFrame is the CALLER's duty in 1.92: nothing in
    // ImGui::NewFrame() calls ImGuiPlatformIO::Renderer_NewFrame, and it is what
    // creates the backend's device objects (shader, VBO, EBO) on the first frame.
    // Skipping it leaves every handle 0 and the first RenderDrawData() draws with no
    // index buffer bound.
    ImGui_ImplOpenGL3_NewFrame();

    // THE PALETTE of THIS frame, which is what the panel is drawn from. It is
    // re-derived every frame rather than cached because the app changes palette at
    // runtime (a settings change, a different accent): theme::Colors::current() is
    // the live answer and the panel's colours are rebuilt from it below, so there is
    // nothing to invalidate and no signal to miss.
    const theme::Graph palette = theme::Graph::from(theme::Colors::current());

    ImGui::NewFrame();

    // THE POINTER IS THE HOST'S, NOT ImGui's QUEUED INPUT (see the header). This host
    // paints when it is asked to, not once per input event, and Dear ImGui trickles its
    // event queue across NewFrame() calls - so the position the queue hands this frame can
    // be behind the pointer the host itself has tracked. Measured in the docked host while
    // the panel still drew a key-move readout: it was drawn a whole drag behind the
    // pointer, at the press position (its box at the press + 12px while the pointer had
    // moved 60px), in the frame whose model carried the gesture's final deltas - and what
    // reads the pointer now (the hovered chip, the marker it lights) can lag the same way.
    // ImGui's own NewFrame() has already run, so this write is what the drawing layer
    // (and nothing else: the panel owns no ImGui window) reads as io.MousePos for the
    // frame.
    io.MousePos = ImVec2(float(aPointer.x()), float(aPointer.y()));

    // The curve ink's own pass, built on the current context the first time this runs and
    // kept for the life of that context (idempotent, so the call per frame costs a null
    // check). It is called HERE because drawPanel is what decides which stroke to use, and
    // it refuses quietly: a context that cannot compile the programs leaves the panel on
    // its AddPolyline path rather than with no curves at all (see gui::stroke).
    stroke::init();

    // The model of this frame, and nothing else: the panel's geometry never comes
    // from a rect computed here. aHostStyle is republished as the floor the model was
    // built for; it places nothing (the panel's one control lives in the dock header).
    GraphPanelModel model;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const bool panelDrawn = aVisible && aEditor && drawList;
    if (panelDrawn) {
        model = buildGraphPanelModel(*aEditor, viewport, palette, aState.topInset, aHostStyle);
        drawPanel(*drawList, model, palette, buildFocus(*aEditor));
    }

    // Published whether or not anything was drawn: the host's own hits and the
    // accessibility layer both read THIS, so they can never disagree with the pixels.
    aState.model = model;
    aState.editor = (aVisible && aEditor) ? aEditor : nullptr;
    aState.palette = palette;

    ImGui::Render();
    return ImGui::GetDrawData();
}

//-------------------------------------------------------------------------------------------------
void publishPanelAccessibility(
    QOpenGLWidget& aHost, PanelFrameState& aState, void (*aOnAction)(void*, int aId), void* aUser
) {
    const bool onScreen = aState.editor != nullptr;

    if (!onScreen && gPanelA11yHost != &aHost) {
        // The panel is not on screen and this host has never published: there is nothing
        // to publish and no installation to take. (A host that HAS published - i.e. that
        // drew a visible frame - publishes the EMPTY model below instead, so the tree is
        // told the panel is gone rather than being left with the last frame of a hidden
        // tab.)
        return;
    }

    // ONE installation (gui::a11y): the host's constructor installed the factory for this
    // widget (installFactoryOnly, see GraphPanelWidget), so this call is the SECOND half
    // of that pairing - a repeat install for the SAME widget, which keeps the interface
    // and its id and only takes over the action callback. The registry below is what keeps
    // it to one call per callback: a per-frame install() would re-take the callback (and
    // re-ask Qt for the interface) on every frame.
    if (gPanelA11yHost != &aHost || gPanelA11yAction != aOnAction || gPanelA11yUser != aUser) {
        a11y::install(aHost, [aOnAction, aUser](int aId) { aOnAction(aUser, aId); });
        gPanelA11yHost = &aHost;
        gPanelA11yAction = aOnAction;
        gPanelA11yUser = aUser;
    }

    a11y::sync(buildAccessibilityModel(aState.model, aState.editor));
}

} // namespace gui
