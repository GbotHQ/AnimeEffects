#include "gui/ImGuiOverlay.h"

#include <cfloat>
#include <cmath>
#include <memory>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPointer>
#include <QTabletEvent>
#include <QVector2D>
#include <QVector3D>
#include <QTouchEvent>
#include <QVarLengthArray>
#include <QWheelEvent>

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
#include "gl/VertexArrayObject.h"
#include "gui/ImGuiA11y.h"
#include "gui/ImGuiInputBridge.h"
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

// The curve stroke width: 1.5px AA'd is the thinnest width that still holds a
// distinct hue on the panel.
constexpr float kCurveWidth = 1.5f;

// Length of the short axis marks. The model publishes positions and leaves the
// length to the drawing layer; 4 is the convention its own KFrameTickLength
// documents (and the room it reserves above a frame number).
constexpr int kAxisTickLength = 4;

// The label halo: the same glyphs drawn at the eight compass offsets of radius 1
// and radius 2, so the ring reaches 2px past the glyph ink in every direction.
// Read only while gui::kDrawTextHalo is on (shipped off: see addHaloedText).
constexpr int kHaloRadius = 2;
constexpr float kHaloAlpha = 0.85f;

// Emphasis: the alpha an unfocused curve and its keys are drawn at WHILE a focus
// exists (with no focus every curve is full). 45% keeps the shape readable and
// leaves no doubt about which curve is the focused one.
constexpr float kUnfocusedAlpha = 0.45f;

// A hidden legend row keeps its swatch and its label - that is how the user sees
// what is hidden and clicks it back - so both read at a third of the strength
// rather than disappearing.
constexpr float kHiddenAlpha = 0.35f;

// Gap between a legend swatch and its label. The model insets the swatch from the
// plot by its own kLegendLeftGap (6px); the label starts the same distance after
// the swatch, so a row reads as one line.
constexpr int kLegendLabelGap = 6;

// The text size used when the widget font cannot be measured (a non-positive
// line height). 13 is ImGui's own default font size, which keeps the panel
// legible instead of empty.
constexpr float kFallbackFontPixelSize = 13.0f;

// The accessibility ids that are not a legend row index. Legend rows are the ids
// 0..n-1, so the two fixed items are negative - one namespace, no collision.
constexpr int kSummaryItemId = -1;
constexpr int kFitItemId = -2;

// lupdate's context for everything this file says to the user: the class is not a
// QObject, so there is no tr() member for it to find.
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
void drawLabel(
    ImDrawList& aDrawList, ImFont* aFont, float aFontSize, const QRect& aBox, const QString& aText, LabelAlign aAlign,
    ImU32 aColor, ImU32 aHalo
) {
    if (!aFont || aText.isEmpty() || aBox.isEmpty())
        return;

    const QByteArray utf8 = aText.toUtf8();
    const ImVec2 size = measureText(aFont, aFontSize, utf8.constData());
    float x = float(aBox.left());
    if (aAlign == LabelAlign::Centre)
        x += (float(aBox.width()) - size.x) * 0.5f;
    else if (aAlign == LabelAlign::Right)
        x = float(aBox.left() + aBox.width()) - size.x;
    const float y = float(aBox.top()) + (float(aBox.height()) - size.y) * 0.5f;
    addHaloedText(aDrawList, aFont, aFontSize, ImVec2(x, y), aColor, aHalo, utf8.constData());
}

// Whether a key square is one the editor's selection carries. The model publishes
// key RECTS without their frames and ctrl::graph::keyRect centres a square exactly
// on the mapped point, so the frame's pixel is the whole identity available here.
bool isKeySelected(const PanelFocus& aFocus, core::TimeKeyType aType, const QRect& aKey) {
    if (aFocus.selectedKeys.isEmpty() || aKey.isEmpty())
        return false;

    const int x = aKey.center().x();
    for (const PanelFocus::SelectedKey& key : aFocus.selectedKeys) {
        if (key.type == aType && key.x == x)
            return true;
    }
    return false;
}

// ---- the key context menu ----
//
// ONE popup, and its id is the whole of its state: ImGui owns whether it is open,
// and this name is how the frame asks (OpenPopup/BeginPopup) and how an event
// handler outside the frame learns it (the class keeps its own copy of the
// answer, because GetID needs a current window and there is none between frames).
const char* kKeyMenuId = "##ae_graph_key_menu";

// The popup's FIXED layout, and the contract the application-level probe's
// coordinate route is built on: the popup is an ImGui window, so unlike a Qt
// QMenu it is confined to the viewport and cannot grow a native window of its
// own; it is laid out with one compact row per entry, no item spacing, and
// enough columns that the whole list fits a small viewport. The row height
// follows the font so text is never clipped, and every row has it, so "which
// entry is at this pixel" is a function of the popup's top edge and its height
// alone - no style number has to be known to the caller that only sees pixels.
constexpr float kMenuRowHeight = 18.0f;
constexpr float kMenuPadX = 6.0f;
constexpr float kMenuPadY = 4.0f;
constexpr float kMenuColumnGap = 4.0f;
constexpr float kMenuInnerPad = 6.0f;
constexpr int kMenuColumns = 2;
// The hairline between the easing entries and the delete entry. It is an ITEM of
// the popup, so the popup's own auto-height counts it; it is not an entry, so the
// row arithmetic above does not.
constexpr float kMenuSeparatorHeight = 1.0f;

// The easing entries the menu offers: the timeline's own list in the shared
// display order (util::Easing::displayCount/typeAtDisplayIndex), with Custom
// skipped - a Custom easing is shaped by dragging the panel's tangent handles or
// by the timeline's shape picker, not by choosing a name from a list, which is
// exactly why the timeline's menu skips it too.
QVector<util::Easing::Type> keyMenuEasingTypes() {
    QVector<util::Easing::Type> types;
    for (int i = 0; i < util::Easing::displayCount(); ++i) {
        const util::Easing::Type type = util::Easing::typeAtDisplayIndex(i);
        if (type == util::Easing::Type_Custom)
            continue;
        types.append(type);
    }
    return types;
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

// The pixel size of a framebuffer's colour attachment. The backdrop copy is a
// BLIT, which requires the source and the destination rectangles to be the same
// size and inside their framebuffers, so the size has to be the widget
// framebuffer's OWN - and that is not a number this file may re-derive: measured
// on this machine (Qt 6.11, 401x321 logical at QT_SCALE_FACTOR=1.5),
// QOpenGLWidget's framebuffer is 602x482, i.e. qRound(w * dpr), while truncating
// gives 601x481 and would leave a one-pixel seam at fractional ratios. The size
// is asked of the attachment instead: QOpenGLWidget's is a multisampled
// RENDERBUFFER (measured: samples=4, GL_RENDERBUFFER), which is also what makes
// the source of the blit a resolve. Zero when there is nothing to ask.
QSize framebufferPixels(GLuint aFramebuffer) {
    gl::Global::Functions& ggl = gl::Global::functions();

    GLint previousRead = 0;
    GLint previousDraw = 0;
    ggl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    ggl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    ggl.glBindFramebuffer(GL_FRAMEBUFFER, aFramebuffer);

    QSize size;
    GLint type = 0;
    GLint name = 0;
    ggl.glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type
    );
    ggl.glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name
    );
    if (type == GL_RENDERBUFFER) {
        GLint width = 0;
        GLint height = 0;
        ggl.glBindRenderbuffer(GL_RENDERBUFFER, GLuint(name));
        ggl.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
        ggl.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
        size = QSize(width, height);
    } else if (type == GL_TEXTURE) {
        // A single-sample texture attachment has its size at mip level 0.
        GLint width = 0;
        GLint height = 0;
        ggl.glBindTexture(GL_TEXTURE_2D, GLuint(name));
        ggl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        ggl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
        size = QSize(width, height);
    }

    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(previousRead));
    ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(previousDraw));
    return size;
}

// The contrast outline pass's two targets, both at the widget framebuffer's own
// pixel size: the BACKDROP (the pixels the panel would sit on - a copy) and the
// panel's own COVERAGE render. Rebuilt only when that size changes (a resize, or
// a device-pixel-ratio change).
struct OutlineTargets {
    QSize pixels;
    std::unique_ptr<gl::Texture> backdropTexture;
    std::unique_ptr<gl::Framebuffer> backdropFbo;
    std::unique_ptr<gl::Texture> overlayTexture;
    std::unique_ptr<gl::Framebuffer> overlayFbo;

    // The GL objects go while a context is current: called before a rebuild and
    // from the overlay's teardown.
    void release() {
        pixels = QSize();
        backdropFbo.reset();
        backdropTexture.reset();
        overlayFbo.reset();
        overlayTexture.reset();
    }

    // False when the driver refuses one of the four objects; the caller then draws
    // the panel without the pass rather than drawing nothing.
    bool ensure(const QSize& aPixels) {
        if (pixels == aPixels && overlayFbo)
            return true;
        release();

        backdropTexture.reset(new gl::Texture());
        backdropTexture->create(aPixels);
        // Both textures are sampled at the texel centre 1:1, and the taps must not
        // WRAP: with the default REPEAT, a tap one pixel left of the first column
        // would read the ink at the right edge of the screen.
        backdropTexture->setFilter(GL_NEAREST);
        backdropTexture->setWrap(GL_CLAMP_TO_EDGE);
        backdropFbo.reset(new gl::Framebuffer());
        backdropFbo->setColorAttachment(0, backdropTexture->id());

        overlayTexture.reset(new gl::Texture());
        overlayTexture->create(aPixels);
        overlayTexture->setFilter(GL_NEAREST);
        overlayTexture->setWrap(GL_CLAMP_TO_EDGE);
        overlayFbo.reset(new gl::Framebuffer());
        overlayFbo->setColorAttachment(0, overlayTexture->id());

        if (!backdropFbo->isComplete() || !overlayFbo->isComplete()) {
            release();
            return false;
        }
        pixels = aPixels;
        return true;
    }
};

} // namespace

namespace outline {

bool init() {
    if (gOutline)
        return true;

    std::unique_ptr<OutlinePass> pass(new OutlinePass());
    if (!pass->shader.setVertexSource(QString::fromLatin1(kOutlineVertexSource))
        || !pass->shader.setFragmentSource(QString::fromLatin1(kOutlineFragmentSource))
        || !pass->shader.link()) {
        // Not fatal: the caller draws the panel into the widget framebuffer
        // directly, i.e. exactly the frame this pass was added to.
        XC_DEBUG_REPORT("the contrast outline pass did not build: %s", qPrintable(pass->shader.log()));
        return false;
    }
    pass->quad.reset(new gl::VertexArrayObject());
    pass->overlayLocation = pass->shader.uniformLocation("uOverlay");
    pass->backdropLocation = pass->shader.uniformLocation("uBackdrop");
    pass->texelLocation = pass->shader.uniformLocation("uTexel");
    pass->minContrastLocation = pass->shader.uniformLocation("uMinContrast");
    pass->colourLocation = pass->shader.uniformLocation("uOutlineColour");
    pass->ringLocation = pass->shader.uniformLocation("uRingRadiusPx");

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
// The one legend label geometry - declared to the gate as well (see the header),
// so a pixel gate can assert the no-room rule on the geometry the hit-test uses.
QRect legendLabelRect(const PanelLegendRow& aRow, ImFont* aFont, float aFontSize) {
    if (!aFont || aRow.label.isEmpty() || aRow.swatch.isEmpty())
        return QRect();

    const QByteArray utf8 = aRow.label.toUtf8();
    const ImVec2 size = measureText(aFont, aFontSize, utf8.constData());
    const int width = int(size.x + 0.5f);
    const int height = int(size.y + 0.5f);
    if (width <= 0 || height <= 0)
        return QRect();
    return QRect(aRow.swatch.right() + 1 + kLegendLabelGap, aRow.swatch.center().y() - height / 2, width, height);
}

//-------------------------------------------------------------------------------------------------
void drawPanel(
    ImDrawList& aDrawList, const GraphPanelModel& aModel, const theme::Graph& aTheme, const PanelFocus& aFocus
) {
    ImFont* font = drawListFont(aDrawList);
    const float fontSize = drawListFontSize(aDrawList);
    const ImU32 halo = toImU32Strength(aTheme.halo(), kHaloAlpha);
    const ImU32 axisText = toImU32(aTheme.axisText());
    const ImU32 tick = toImU32(aTheme.gridMajor());

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
    // value column or the legend.
    aDrawList.PushClipRect(rectMin(plot), rectMax(plot), true);

    // The axes: SHORT marks only, 1px with AA off (the timeline's hairline rule -
    // an AA'd 1px line straddles two columns and reads as 2px of grey). The model
    // publishes positions, never spans, so there is no grid to draw. The flags are
    // put back exactly as they were found, so a caller's own setting is not
    // clobbered.
    const ImDrawListFlags savedLineFlags = aDrawList.Flags;
    aDrawList.Flags = savedLineFlags & ~ImDrawListFlags_AntiAliasedLines;
    for (int x : aModel.frameTickX) {
        const float fx = float(x);
        aDrawList.AddLine(ImVec2(fx, float(plot.top())), ImVec2(fx, float(plot.top() + kAxisTickLength)), tick, 1.0f);
    }
    for (int y : aModel.valueTickY) {
        const float fy = float(y);
        aDrawList.AddLine(
            ImVec2(float(plot.left()), fy), ImVec2(float(plot.left() + kAxisTickLength), fy), tick, 1.0f
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

    // The curves. Clipped to the plot (above); the focused one at full colour,
    // every other one dimmed while a focus exists.
    for (const PanelCurve& curve : aModel.curves) {
        if (curve.points.size() < 2)
            continue; // identity and colour but no geometry: the keys are outside the window

        const bool focused = aFocus.valid && aFocus.node == curve.node && aFocus.channel == curve.channel;
        const float strength = (aFocus.valid && !focused) ? kUnfocusedAlpha : 1.0f;

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

    // The key squares, from the model's rects: the editor's hit-test geometry, so a
    // click lands where the key is drawn. The painter's own idiom - a filled square
    // in the key fill with a contrasting hairline edge - and the editor's selection
    // turns both into the selected token.
    const ImU32 selectedKey = toImU32(aTheme.selected());
    for (const PanelCurve& curve : aModel.curves) {
        const bool focused = aFocus.valid && aFocus.node == curve.node && aFocus.channel == curve.channel;
        const float strength = (aFocus.valid && !focused) ? kUnfocusedAlpha : 1.0f;
        const ImU32 fill = toImU32Strength(aTheme.keyFill(), strength);
        const ImU32 edge = toImU32Strength(aTheme.keyEdge(), strength);
        for (const QRect& key : curve.keys) {
            const bool selected = isKeySelected(aFocus, curve.channel.type(), key);
            const ImVec2 min = rectMin(key);
            const ImVec2 max = rectMax(key);
            aDrawList.AddRectFilled(min, max, selected ? selectedKey : fill);
            aDrawList.AddRect(min, max, selected ? selectedKey : edge, 0.0f, ImDrawFlags_None, 1.0f);
        }
    }

    // The Custom tangent handles, from the model's geometry: a stem from each
    // handle's base to its dot, then the dot itself. The dot NEAREST the pointer,
    // within ctrl::graph::kHandleHitRadius, is drawn in the selected token instead
    // - the same distance and the same `<=` the editor's hit-test grabs through, so
    // the highlight names exactly the handle a grab would take. The handles are
    // inside the plot clip (pushed above), so a stem that leaves the plot is cut at
    // the edge rather than painting over the panel's chrome.
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int nearestCurve = -1;
        int nearestHandle = -1;
        double nearestDistance = double(ctrl::graph::kHandleHitRadius);
        for (int c = 0; c < aModel.curves.size(); ++c) {
            const PanelCurve& curve = aModel.curves.at(c);
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

        const ImU32 handleLine = toImU32(aTheme.handleLine());
        const ImU32 handleFill = toImU32(aTheme.handleFill());
        const ImU32 handleEdge = toImU32(aTheme.handleEdge());
        const ImU32 handleSelected = toImU32(aTheme.selected());
        for (int c = 0; c < aModel.curves.size(); ++c) {
            const PanelCurve& curve = aModel.curves.at(c);
            for (int h = 0; h < curve.handles.size(); ++h) {
                const PanelHandle& handle = curve.handles.at(h);
                const bool highlighted = c == nearestCurve && h == nearestHandle;
                const ImVec2 base(float(handle.base.x()), float(handle.base.y()));
                const ImVec2 dot(float(handle.dot.x()), float(handle.dot.y()));
                aDrawList.AddLine(base, dot, handleLine, 1.0f);
                aDrawList.AddCircleFilled(dot, float(handle.radius), highlighted ? handleSelected : handleFill);
                aDrawList.AddCircle(dot, float(handle.radius), highlighted ? handleSelected : handleEdge, 0, 1.0f);
            }
        }
    }

    // The playhead: the document's own frame, 1px, the same hairline rule as the
    // axes. It is the one mark on the panel that is not part of the curve data.
    if (aModel.playheadX >= 0) {
        const ImDrawListFlags savedPlayheadFlags = aDrawList.Flags;
        aDrawList.Flags = savedPlayheadFlags & ~ImDrawListFlags_AntiAliasedLines;
        const float px = float(aModel.playheadX);
        aDrawList.AddLine(
            ImVec2(px, float(plot.top())), ImVec2(px, float(plot.bottom() + 1)), toImU32(aTheme.selected()), 1.0f
        );
        aDrawList.Flags = savedPlayheadFlags;
    }

    // The legend: one swatch per row, at the swatch rect the model laid out, with
    // the row's own colour and its label beside it. A hidden row keeps both, dimmed.
    // A row with NO ROOM (an empty swatch: GraphPanelModel's LEGEND note) is skipped
    // whole - nothing is drawn for it, and nothing is derived from its empty rect.
    for (const PanelLegendRow& row : aModel.legend) {
        if (row.swatch.isEmpty())
            continue;

        const float strength = row.visible ? 1.0f : kHiddenAlpha;
        const ImVec2 min = rectMin(row.swatch);
        const ImVec2 max = rectMax(row.swatch);
        aDrawList.AddRectFilled(min, max, toImU32Strength(row.color, strength));
        aDrawList.AddRect(min, max, toImU32Strength(aTheme.keyEdge(), strength), 0.0f, ImDrawFlags_None, 1.0f);

        const QRect box = legendLabelRect(row, font, fontSize);
        drawLabel(
            aDrawList, font, fontSize, box, row.label, LabelAlign::Left, toImU32Strength(aTheme.axisText(), strength),
            halo
        );
    }

    // Why the panel is empty, in the middle of the plot: an empty panel has to say
    // so, and this is the only place it can.
    if (!aModel.emptyState.isEmpty())
        drawLabel(aDrawList, font, fontSize, plot, aModel.emptyState, LabelAlign::Centre, axisText, halo);

    aDrawList.PopClipRect();

    // The value numbers live in the column LEFT of the plot, so they are drawn after
    // the clip: the model already clamped each box into the column, and a label must
    // not be cut off by an edge it was deliberately kept clear of.
    for (const PanelLabel& label : aModel.valueLabels)
        drawLabel(aDrawList, font, fontSize, label.rect, label.text, LabelAlign::Right, axisText, halo);
}

//-------------------------------------------------------------------------------------------------
struct ImGuiOverlay::State {
    // The widget the backend was initialized on, so teardown can make its context
    // current before deleting the backend's GL objects.
    QPointer<QOpenGLWidget> widget;
    // The Qt -> ImGuiIO translation. Destroyed before the context it feeds.
    QScopedPointer<ImGuiInputBridge> bridge;

    // The frame the pixels were drawn from, and the editor they were built from
    // (null while the panel is hidden). The widget's own hits - a legend swatch, a
    // legend label, a double-click on the plot - are tested against THIS geometry,
    // so the clickable control is exactly the drawn control.
    GraphPanelModel model;
    ctrl::GraphEditor* editor = nullptr;
    // The frame's font, kept so a press BETWEEN frames measures a legend label
    // exactly as the frame did. It belongs to the context's atlas and lives as
    // long as the context.
    ImFont* font = nullptr;
    float fontSize = 0.0f;

    // Whether the KEY CONTEXT MENU was open at the end of the last frame (see
    // drawKeyContextMenu). ImGui owns the popup itself; this is the copy an event
    // handler can read between frames, where ImGui's own open state cannot be
    // queried (GetID needs a current window). It is also what keeps a press from
    // reaching the graph while the menu is up, and what the frame turns the
    // keyboard over to ImGui for (see paint).
    bool keyMenuOpen = false;
    // The keyboard focus this class took for the menu, and the widget that had it,
    // so the frame can hand it back when the menu closes (see paint). Only ever set
    // while keyMenuOpen is true, and the pointer is cleared with the hand-back.
    QPointer<QWidget> focusBeforeMenu;
    bool tookFocusForMenu = false;
    // Where the right-click that opened it was, in widget-logical pixels. Kept
    // rather than re-read from io.MousePos, which follows the pointer: the popup
    // is placed from this anchor every frame, so a menu that is open does not slide
    // around under the pointer that is aiming at one of its entries.
    QPoint keyMenuAnchor;

    // The contrast outline pass's own targets (see gui::outline): the backdrop copy
    // and the panel's coverage framebuffer. Built lazily on the frame that first
    // needs them, released in shutdown().
    OutlineTargets outline;

    // io.DeltaTime, from the real interval between frames.
    QElapsedTimer frameTimer;
    bool frameTimerStarted = false;
};

namespace {

// The application font's line height in pixels: what a text line laid out by Qt in
// this widget would occupy, and the size the atlas is asked for.
float fontPixelSize(QOpenGLWidget& aWidget) {
    const QFontMetricsF metrics(aWidget.font());
    const float height = float(metrics.height());
    return (height > 0.0f) ? height : kFallbackFontPixelSize;
}

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

// Whether ImGui still holds input it will only APPLY on a later frame, which is the
// only thing that decides whether this frame needs a successor.
//
// ImGui applies a queued press and the release that follows it on TWO frames rather
// than one: io.ConfigInputTrickleEventQueue (ImGui's own default, and what keeps a
// press and its release from flattening into "no click" in a single frame) stops
// UpdateInputEvents() at the first event of the next kind, so a frame that was
// handed both a press and its release applies the press and leaves the release
// queued. Nothing published says so: io.MouseDown is the state ImGui has APPLIED,
// and the waiting list above it is not readable through the public API. The queue
// therefore comes out of imgui_internal.h, the way the draw list's font does (see
// the include at the top of this file) - one fact drawn from ImGui's own frame
// bookkeeping, because it is the fact this question is about.
bool imguiHasQueuedInput() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    return context && context->InputEventsQueue.Size > 0;
}

// The editor's sticky focus plus its selection, in the currency drawPanel uses.
PanelFocus buildFocus(ctrl::GraphEditor& aEditor, const GraphPanelModel& aModel) {
    PanelFocus focus;

    if (aEditor.hasFocus() && aEditor.focusedNode()) {
        focus.node = const_cast<core::ObjectNode*>(aEditor.focusedNode());
        focus.channel = aEditor.focusedChannel();
        focus.valid = true;
    }

    core::TimeLineEvent selection;
    if (!aEditor.retrieveSelection(selection))
        return focus;

    // A selected key has to be found among the model's key RECTS, which carry no
    // frame. ctrl::graph::keyRect centres a square exactly on the mapped point, so
    // the frame's pixel x is what identifies it - and it is the editor's own scaler
    // with the MODEL's panel, i.e. the same mapping the model copied, so the two
    // agree by construction rather than by coincidence.
    ctrl::graph::Scaler scale = aEditor.scale();
    scale.setRect(aModel.panel);
    for (const core::TimeLineEvent::Target& target : selection.targets()) {
        if (target.pos.isNull())
            continue;
        PanelFocus::SelectedKey key;
        key.type = target.pos.type();
        key.x = scale.pixelX(core::Frame(target.pos.index()));
        focus.selectedKeys.append(key);
    }
    return focus;
}

} // namespace

//-------------------------------------------------------------------------------------------------
ImGuiOverlay::ImGuiOverlay(): mState(new State()), mReady(false) {}

ImGuiOverlay::~ImGuiOverlay() {
    // The accessible factory points at this object's callback: it goes first.
    a11y::uninstall();
    shutdown();
}

void ImGuiOverlay::shutdown() {
    if (!mState)
        return;

    // The bridge is ImGui's platform IME target and reads the context: drop it
    // before the context goes, and drop the frame state with it (the editor pointer
    // and the geometry are answers about a panel that no longer exists).
    mState->bridge.reset();
    mState->model = GraphPanelModel();
    mState->editor = nullptr;
    mState->font = nullptr;
    mState->fontSize = 0.0f;

    if (!mReady)
        return;

    // The backend's GL objects (shaders, buffers, textures) only go away on a
    // current context. Make the widget's current for the teardown while it is still
    // usable; a widget torn down after its context has gone leaves those few objects
    // to the driver rather than calling into a dead context.
    QOpenGLWidget* widget = mState->widget;
    const bool madeCurrent = widget && widget->context() && widget->context()->isValid();
    if (madeCurrent)
        widget->makeCurrent();
    // The outline pass's program, quad and two textures are context objects too.
    mState->outline.release();
    outline::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    if (madeCurrent)
        widget->doneCurrent();
    mReady = false;
}

bool ImGuiOverlay::init(QOpenGLWidget& aWidget) {
    if (!mState)
        return false;

    // initializeGL can run more than once, and it does so exactly when the widget
    // has a new GL context (a reparent, a screen change): every object the backend
    // created belongs to the OLD context, so the overlay is rebuilt from scratch
    // rather than kept.
    if (mReady)
        shutdown();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // No runtime files, ever: no imgui.ini beside the application and no ImGui log.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // The overlay never changes the cursor itself; the viewport's cursor policy
    // (MainDisplayWidget) stays the only thing that does.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    // The atlas through the Qt/QRawFont loader: the application font at the widget
    // font's own pixel size, plus the writing-system fallbacks Qt resolves for it,
    // so CJK, `.ttc` and variable faces rasterize with the face the rest of the
    // application draws with.
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
    const float pixelSize = fontPixelSize(aWidget);
    if (imgui_qt::installQtFontLoader(*io.Fonts, aWidget.font(), pixelSize, 1) == 0) {
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

    mState->widget = &aWidget;
    mState->bridge.reset(new ImGuiInputBridge(aWidget));
    mReady = true;

    // ImGui draws triangles into a GL framebuffer, so without this factory the panel
    // does not exist for assistive technology at all. The callback acts on the
    // current frame's editor, i.e. exactly what the pointer would act on.
    a11y::install(aWidget, [this](int aId) { onAccessibilityAction(aId); });
    return true;
}

void ImGuiOverlay::paint(
    QOpenGLWidget& aWidget, ctrl::GraphEditor* aEditor, core::Project*, bool aVisible
) {
    if (!mReady || !mState)
        return;
    State& state = *mState;

    const QSize viewport = aWidget.size();
    const double dpr = aWidget.devicePixelRatioF();
    ImGuiIO& io = ImGui::GetIO();
    // Logical (device independent) pixels, which is the space QMouseEvent::position()
    // and the model's rects are in; ImGui scales the projection by
    // ImDrawData::FramebufferScale itself.
    io.DisplaySize = ImVec2(float(viewport.width()), float(viewport.height()));
    io.DisplayFramebufferScale = ImVec2(float(dpr > 0.0 ? dpr : 1.0), float(dpr > 0.0 ? dpr : 1.0));
    io.DeltaTime = nextDeltaTime(state.frameTimer, state.frameTimerStarted);

    // The backend's own NewFrame is the CALLER's duty in 1.92: nothing in
    // ImGui::NewFrame() calls ImGuiPlatformIO::Renderer_NewFrame, and it is what
    // creates the backend's device objects (shader, VBO, EBO) on the first frame.
    // Skipping it leaves every handle 0 and the first RenderDrawData() draws with no
    // index buffer bound.
    ImGui_ImplOpenGL3_NewFrame();

    // THE KEYBOARD BELONGS TO IMGUI ONLY WHILE THE KEY CONTEXT MENU IS OPEN (see
    // the class comment). io.ConfigFlags is read by ImGui::NewFrame's own
    // navigation update, so the toggle has to be made here, immediately before it,
    // and it is made from the popup's LAST KNOWN open state - which is exactly what
    // "while the menu is open" means for a flag that can only take effect on the
    // next frame (the frame that OPENS the popup is already past that point).
    //
    // WHY IT IS NOT ENABLED FOR THE WHOLE SESSION. This overlay covers the whole
    // viewport, so a globally enabled ImGuiConfigFlags_NavEnableKeyboard makes
    // ImGui claim the keyboard for the panel (io.NavActive -> io.WantCaptureKeyboard,
    // io.ConfigNavCaptureKeyboard defaults to true) and the application's own key
    // commands - ctrl+Z, Delete, ctrl+Right and the rest of gui::KeyCommandMap -
    // would stop reaching MainWindow while the panel is on screen. With the flag off
    // the overlay claims nothing and every key reaches the application exactly as it
    // did before this menu existed, which is the property the application-level probe
    // asserts with the menu closed.
    //
    // WHILE IT IS OPEN the flag is what ImGui gates its own popup keyboard behaviour
    // on: ImGui::NavUpdateCancelRequest closes the front popup on Escape, and the
    // arrows move the popup's own entry highlight (ImGui::NavUpdateCreateMoveRequest),
    // which Enter then activates. None of the three exists without it.
    if (state.keyMenuOpen) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // AND THE KEYS HAVE TO REACH THIS OVERLAY FOR ANY OF THAT TO MEAN
        // ANYTHING. They reach it through MainDisplayWidget::keyPressEvent, and the
        // widget is a plain QOpenGLWidget carrying QWidget's default focus policy
        // (Qt::NoFocus - it never sets one): Qt focuses a widget on a click only
        // when its policy allows it, so a press in the viewport does NOT make the
        // widget the focus object and every key goes to whatever widget last had
        // the focus (which is where the application's own key commands come from).
        // MEASURED, with this class instrumented and the application probe driving
        // the menu: ZERO key events reached ImGuiOverlay::key() in a whole run, and
        // the popup stayed up through Escape. So the menu takes the keyboard while
        // it is open - the same thing this file already does for a Dear ImGui TEXT
        // FIELD (see the IME block at the end of this function: the widget takes
        // focus so Qt asks it), and for the same reason: a part of this overlay
        // needs the keyboard. The widget that had it is remembered and handed back
        // when the menu closes, so a popup never leaves the keyboard somewhere else.
        if (!aWidget.hasFocus()) {
            if (!state.tookFocusForMenu) {
                state.focusBeforeMenu = QApplication::focusWidget();
                state.tookFocusForMenu = true;
            }
            aWidget.setFocus(Qt::OtherFocusReason);
        }
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

        // Hand the keyboard back, but only if this class still holds it: a click on
        // another widget while the menu was up is that widget's focus and must not
        // be overruled here.
        if (state.tookFocusForMenu) {
            state.tookFocusForMenu = false;
            if (state.focusBeforeMenu && aWidget.hasFocus())
                state.focusBeforeMenu->setFocus(Qt::OtherFocusReason);
            state.focusBeforeMenu.clear();
        }
    }

    ImGui::NewFrame();

    // The model of this frame, and nothing else: the panel's geometry never comes
    // from a rect computed here.
    GraphPanelModel model;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const theme::Graph palette = theme::Graph::from(theme::Colors::current());
    const bool panelDrawn = aVisible && aEditor && drawList;
    if (panelDrawn) {
        model = buildGraphPanelModel(*aEditor, viewport, palette);
        drawPanel(*drawList, model, palette, buildFocus(*aEditor, model));
        // The key context menu, on the geometry just drawn (see the class comment).
        // Drawn after the panel, which goes into the BACKGROUND draw list, so the
        // popup - a window - covers it.
        drawKeyContextMenu(*aEditor, model);
        state.font = drawListFont(*drawList);
        state.fontSize = drawListFontSize(*drawList);
    } else {
        // Hidden: no draw command at all, and no geometry left behind for a press
        // to hit. The key menu goes with the panel it belongs to - and it is
        // CLOSED rather than dropped, or it would sit in ImGui's open-popup stack
        // for the rest of the session, where it keeps io.WantCaptureMouse latched
        // and can never be drawn or dismissed again.
        if (state.keyMenuOpen) {
            state.keyMenuOpen = false;
            ImGui::ClosePopupToLevel(0, true);
        }
        state.font = nullptr;
        state.fontSize = 0.0f;
    }

    // Published whether or not anything was drawn: the widget's own hits and the
    // accessibility layer both read THIS, so they can never disagree with the pixels.
    state.model = model;
    state.editor = (aVisible && aEditor) ? aEditor : nullptr;

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    // THE CONTRAST OUTLINE, THEN THE COMPOSITE (see gui::outline in the header). One
    // full-screen pass at the very end of the frame, and the two renders it needs:
    // the widget framebuffer becomes the BACKDROP - what the user would see without
    // the panel - and the panel is drawn into its own framebuffer, where alpha IS
    // the coverage and rgb the ink. The pass then rings the ink that would have been
    // lost against that backdrop and composites the panel over it.
    //
    // It is skipped ENTIRELY while the panel is hidden (an empty draw data has no
    // ink to ring and nothing to composite, which is why the hidden path is exactly
    // the cheap frame it always was) and when the driver refuses the pass or its
    // targets, in which case the panel is drawn straight into the widget
    // framebuffer, i.e. precisely the frame this pass was added to.
    const GLuint widgetFbo = GLuint(aWidget.defaultFramebufferObject());
    const QSize pixels = panelDrawn ? framebufferPixels(widgetFbo) : QSize();
    bool composited = false;
    if (!pixels.isEmpty() && outline::init() && state.outline.ensure(pixels)) {
        OutlineTargets& targets = state.outline;
        gl::Global::Functions& ggl = gl::Global::functions();

        // 1. the backdrop. A BLIT, never a readback: the widget's framebuffer is
        // 4x multisampled (MainWindow's QSurfaceFormat), and a blit resolves it.
        outline::copyBackdrop(widgetFbo, targets.backdropFbo->id(), pixels);

        // 2. the panel, into its own framebuffer.
        targets.overlayFbo->bind();
        // ImGui's own draw commands set a scissor box, so the clear has to be free
        // of one, and the viewport is the framebuffer's - both were last set by
        // something else.
        ggl.glDisable(GL_SCISSOR_TEST);
        ggl.glViewport(0, 0, pixels.width(), pixels.height());
        ggl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        ggl.glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(drawData);

        // 3. ONE pass into the widget's own framebuffer: ring, then composite.
        ggl.glBindFramebuffer(GL_FRAMEBUFFER, widgetFbo);
        outline::Params outlineParams;
        outlineParams.colour = palette.outline();
        outline::composite(targets.overlayTexture->id(), targets.backdropTexture->id(), pixels, outlineParams);
        composited = true;
    }
    if (!composited)
        ImGui_ImplOpenGL3_RenderDrawData(drawData);

    // What the backend does NOT restore (measured: the viewport is left as the
    // backend set it, the scissor box is left set, and a temporary VAO is created
    // and deleted per draw). paintGL re-sets the viewport and the render state at
    // its top, so nothing here has to restore them - and the pass above leaves the
    // widget's own framebuffer bound, which is the state it was called in.

    // The QWidget side of the IME contract (see ImGuiInputBridge's class comment):
    // Qt asks the FOCUS OBJECT for input-method queries, and a plain QOpenGLWidget
    // is not an input item at all - so the attribute is set exactly while a Dear
    // ImGui text field wants text, and the widget takes focus so Qt asks it. With no
    // ImGui field on screen this is a no-op every frame.
    const bool wantsText = state.bridge && state.bridge->imeWantsTextInput();
    if (aWidget.testAttribute(Qt::WA_InputMethodEnabled) != wantsText)
        aWidget.setAttribute(Qt::WA_InputMethodEnabled, wantsText);
    if (wantsText && !aWidget.hasFocus())
        aWidget.setFocus(Qt::OtherFocusReason);

    publishAccessibility();

    // THE FRAME THAT FOLLOWS, when ImGui still holds input for it (see
    // imguiHasQueuedInput). ImGui applies a queued press and its release on two
    // frames, and a popup entry is one of the things it acts on only on the second
    // of them: an ImGui Selectable fires on the RELEASE, in the frame that applies
    // it. THIS frame is the one that carried the press, so it cannot be that second
    // frame, and the request belongs here rather than at the event: an event
    // handler's requestRepaint() is a QWidget::update(), like the widget's own
    // update() for a press the overlay consumed (MainDisplayWidget::mousePressEvent)
    // - the paint already pending absorbs both, and the frame that must apply the
    // trickled event is precisely that one's successor. Without this the release
    // sits in ImGui's queue until some unrelated repaint happens to run, which is a
    // menu entry that does not respond to a single click.
    if (imguiHasQueuedInput())
        requestRepaint();
}

//-------------------------------------------------------------------------------------------------
bool ImGuiOverlay::mouseMove(const QMouseEvent& aEvent) {
    const bool captured = mState && mState->bridge ? mState->bridge->handleMouseMove(aEvent) : false;
    // The key menu is drawn by ImGui, so its hover highlight is a frame's answer:
    // without a repaint per move the entry under the pointer would only be
    // repainted when something else asked for a frame. (The widget's own cursor
    // path does not repaint for a plain move over the panel - the graph has no
    // hover state of its own.)
    if (mState && mState->keyMenuOpen)
        requestRepaint();
    return captured;
}

bool ImGuiOverlay::mouseButton(const QMouseEvent& aEvent) {
    const bool captured = mState && mState->bridge ? mState->bridge->handleMouseButton(aEvent) : false;
    // The menu needs a FRAME, and the widget does not always produce one: a
    // right-click is nobody's cursor action (the graph takes no right-button
    // action, and the Driver's routing returns false for it), so without this the
    // popup would not be drawn until some unrelated repaint. The release is here
    // too because ImGui's queued input is TRICKLED - a press and a release that
    // arrive together are split across two frames (io.ConfigInputTrickleEventQueue)
    // - and while the menu is open the release is the half that fires the entry, so
    // it is worth a frame of its own as much as the press is.
    //
    // This is the frame the EVENT gets. The frame that applies what ImGui had to
    // leave in its queue is the successor THIS frame asks for when it ends (see
    // imguiHasQueuedInput, at the end of paint()): one update() cannot stand in for
    // both, because Qt coalesces everything requested while a paint is already
    // pending into that one pending paint.
    if (aEvent.button() == Qt::RightButton || (mState && mState->keyMenuOpen))
        requestRepaint();
    return captured;
}

bool ImGuiOverlay::wheel(const QWheelEvent& aEvent) {
    return mState && mState->bridge ? mState->bridge->handleWheel(aEvent) : false;
}

bool ImGuiOverlay::key(const QKeyEvent& aEvent) {
    const bool captured = mState && mState->bridge ? mState->bridge->handleKey(aEvent) : false;
    // The popup is drawn by ImGui, so every key it acts on is a frame's answer: the
    // Escape that closes it (ImGui::NavUpdateCancelRequest, inside NewFrame) and the
    // arrows that move its highlight are applied between frames, and without a
    // request here the menu would keep its pixels - and its open state - until some
    // unrelated repaint happened to run. The same reason the move path above asks
    // while the menu is open, and the request is unconditional there for the same
    // reason it is unconditional here: the menu is the only keyboard consumer this
    // class has, so while it is up every key is a candidate for it.
    if (mState && mState->keyMenuOpen)
        requestRepaint();
    return captured;
}

bool ImGuiOverlay::inputMethod(const QInputMethodEvent& aEvent) {
    return mState && mState->bridge ? mState->bridge->handleInputMethod(aEvent) : false;
}

bool ImGuiOverlay::tablet(const QTabletEvent& aEvent) {
    return mState && mState->bridge ? mState->bridge->handleTablet(aEvent) : false;
}

bool ImGuiOverlay::touch(const QTouchEvent& aEvent) {
    return mState && mState->bridge ? mState->bridge->handleTouch(aEvent) : false;
}

void ImGuiOverlay::focus(bool aFocused) {
    if (mState && mState->bridge)
        mState->bridge->handleFocus(aFocused);
}

void ImGuiOverlay::dprChanged(qreal aDpr) {
    if (mState && mState->bridge)
        mState->bridge->handleDprChanged(aDpr);
}

QVariant ImGuiOverlay::inputMethodQuery(Qt::InputMethodQuery aQuery) const {
    return (mState && mState->bridge) ? mState->bridge->inputMethodQuery(aQuery) : QVariant();
}

bool ImGuiOverlay::imeWantsTextInput() const {
    return mState && mState->bridge && mState->bridge->imeWantsTextInput();
}

//-------------------------------------------------------------------------------------------------
bool ImGuiOverlay::press(ctrl::GraphEditor& aEditor, const QPoint& aPos) {
    if (!mReady || !mState || !mState->editor)
        return false; // hidden: nothing is on screen to hit

    // THE KEY MENU FIRST. While it is open the pointer is the menu's: ImGui owns
    // the popup and decides what a press means inside it (an entry, or a dismissal
    // when it lands outside) from the stream it was fed, so the graph must not
    // also act on the same press - a left click on "Delete key" would otherwise
    // delete a legend row, or insert a key under the menu, through the widget's
    // next step.
    if (mState->keyMenuOpen)
        return true;

    // The last frame's own geometry. A swatch takes precedence over a label, and
    // the label's box is the one legendLabelRect measured for the drawn glyphs. A row
    // with no room is skipped in both passes: it has no swatch on screen and - see
    // legendLabelRect - no label box either, so nothing of it is a control.
    for (int i = 0; i < mState->model.legend.size(); ++i) {
        const PanelLegendRow& row = mState->model.legend.at(i);
        if (!row.node || row.swatch.isEmpty() || !row.swatch.contains(aPos))
            continue;
        // The swatch is the channel's switch. Hiding one also drops it from the
        // model's curves on the next frame, and it keeps its (dimmed) row so it can
        // be switched back on.
        aEditor.setChannelVisible(*row.node, row.channel, !row.visible);
        return true;
    }

    for (int i = 0; i < mState->model.legend.size(); ++i) {
        const PanelLegendRow& row = mState->model.legend.at(i);
        if (!row.node || row.swatch.isEmpty())
            continue;
        if (legendLabelRect(row, mState->font, mState->fontSize).contains(aPos)) {
            // Focusing a channel fits the value axis to it (GraphEditor's own rule);
            // it is the one place the panel lets a click choose what the axis follows.
            aEditor.setFocusedChannel(*row.node, row.channel);
            return true;
        }
    }
    return false;
}

bool ImGuiOverlay::doubleClick(ctrl::GraphEditor& aEditor, const QPoint& aPos) {
    if (!mReady || !mState || !mState->editor)
        return false;
    if (!mState->model.plot.contains(aPos))
        return false; // not the plot: the viewport's own double-click semantics apply

    // A double-click ON a legend control is that control's business, not the fit's:
    // the swatch toggles and the label focuses, and a second click must not turn
    // either into a zoom change. A row with no room has neither on screen.
    for (const PanelLegendRow& row : mState->model.legend) {
        if (row.swatch.isEmpty())
            continue;
        if (row.swatch.contains(aPos))
            return false;
        if (legendLabelRect(row, mState->font, mState->fontSize).contains(aPos))
            return false;
    }

    // Both axes to the shown data: the panel's own fit gesture.
    aEditor.fitAll();
    return true;
}

//-------------------------------------------------------------------------------------------------
void ImGuiOverlay::drawKeyContextMenu(ctrl::GraphEditor& aEditor, const GraphPanelModel& aModel) {
    if (!mState)
        return;
    State& state = *mState;
    ImGuiIO& io = ImGui::GetIO();

    // THE RIGHT-CLICK. A key is the only target, and the test is the MODEL's own
    // rectangles - the ones ctrl::graph::keyRect produced for the frame just
    // drawn, through the same geometry the editor's own hit-test reads - so a menu
    // can never open on a key the editor would not have found. A point on no key
    // opens nothing and consumes nothing: this class takes no other right-button
    // action, and neither does the graph, so the click stays what it was before
    // this menu existed.
    if (!state.keyMenuOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsMousePosValid(&io.MousePos)) {
        const QPoint pos(int(io.MousePos.x), int(io.MousePos.y));
        core::ObjectNode* node = nullptr;
        ctrl::graph::Channel channel;
        core::Frame frame;

        for (const PanelCurve& curve : aModel.curves) {
            // keyFrames is published beside keys and has the same length (see
            // GraphPanelModel.h); a model that carries one without the other is
            // not one buildGraphPanelModel makes, and it is not one this class can
            // name a key from either, so it is skipped rather than guessed at.
            if (curve.keyFrames.size() != curve.keys.size())
                continue;
            for (int i = 0; i < curve.keys.size(); ++i) {
                if (!curve.keys.at(i).contains(pos))
                    continue;
                node = curve.node;
                channel = curve.channel;
                frame = curve.keyFrames.at(i);
                break;
            }
            if (node)
                break;
        }

        if (node) {
            // The right-click makes that key the panel's SELECTION, which is the
            // timeline's own behaviour with its key menu (selectKeysAt with the
            // click point, then the menu acts on the result): the menu acts on the
            // selection, so the key it was opened on has to be in it. Selecting is
            // also what a press on a key does, so a right-click here is not a
            // second, quieter way to move the panel's current key.
            aEditor.selectKey(*node, channel, frame);
            state.keyMenuAnchor = pos;
            ImGui::OpenPopup(kKeyMenuId);
            state.keyMenuOpen = true;
            // The menu covers part of the panel and the selection just changed
            // under it, so this frame is already stale: ask for the next one.
            requestRepaint();
        }
    }

    if (state.keyMenuOpen) {
        // THE MENU OWNS THE KEYBOARD WHILE IT IS OPEN, so the keys it acts on
        // (Escape, the arrows, Enter) never reach the application's key commands.
        // The widget consumes a key exactly when ImGuiInputBridge::keyboardCaptured()
        // answers true, i.e. when io.WantCaptureKeyboard was set - and that flag is
        // ImGui's own answer, which this makes deterministic rather than a
        // consequence of which window ImGui happens to have focused: with the flag
        // above, io.NavActive would usually set it too (io.ConfigNavCaptureKeyboard
        // defaults to true), but "usually" is not what a key route should rest on.
        //
        // Called EVERY frame the popup is open, because that is what the API means:
        // it sets the value NewFrame will publish NEXT frame and clears it again at
        // the end of that frame (imgui.cpp, SetNextFrameWantCaptureKeyboard and the
        // WantCaptureKeyboardNextFrame reset in NewFrame). It is the same
        // "force capture keyboard when your widget is being hovered" the header
        // documents, with "hovered" being "this popup is up". The one-frame lag is
        // harmless here: the popup can only open on a mouse event, so no key of this
        // menu's can be lost to it.
        ImGui::SetNextFrameWantCaptureKeyboard(true);

        const QVector<util::Easing::Type> types = keyMenuEasingTypes();
        const int easingRows = (types.size() + kMenuColumns - 1) / kMenuColumns;
        const int rows = easingRows + 1; // the easing grid, then the delete entry
        const float rowHeight = ImMax(kMenuRowHeight, ImGui::GetTextLineHeight());

        // The delete entry is the one user-visible string this menu creates, and
        // it is the timeline's own: the same literal, asked for in the SAME
        // context the timeline asks in, so a translator has one "Delete key" and
        // not two. (The class comment states this exception to the file's own
        // context rule.)
        const QByteArray deleteLabel =
            QCoreApplication::translate(
                "TimeLineEditorWidget", QT_TRANSLATE_NOOP("TimeLineEditorWidget", "Delete key")
            )
                .toUtf8();

        // One width for every entry, from the widest label the popup will draw
        // (the delete entry included, because it spans the whole width), so a row
        // is a rectangle a caller can find from the popup's own edges alone.
        float columnWidth = 0.0f;
        for (const util::Easing::Type type : types) {
            const QByteArray text = util::Easing::getTypeName(type).toUtf8();
            columnWidth = ImMax(columnWidth, ImGui::CalcTextSize(text.constData()).x);
        }
        columnWidth = ImMax(columnWidth + 2.0f * kMenuInnerPad, ImGui::CalcTextSize(deleteLabel.constData()).x + 2.0f * kMenuInnerPad);

        const float contentWidth = kMenuColumns * columnWidth + (kMenuColumns - 1) * kMenuColumnGap;
        const float contentHeight = rows * rowHeight + kMenuSeparatorHeight;

        // WHERE. At the anchor the right-click recorded - ImGui's own
        // context-menu convention, and what "opens a popup at the pointer" means -
        // unless the viewport cannot hold the popup below it, in which case it is
        // placed so the WHOLE menu fits: a popup is an ImGui window and cannot
        // leave the viewport, so a menu hanging off the bottom edge would be
        // entries nobody can reach (a Qt menu, with a native window of its own,
        // has no such constraint; this one does).
        const float border = ImGui::GetStyle().WindowBorderSize;
        const ImVec2 popupSize(
            contentWidth + 2.0f * kMenuPadX + 2.0f * border, contentHeight + 2.0f * kMenuPadY + 2.0f * border
        );
        const float anchorX = float(state.keyMenuAnchor.x());
        const float anchorY = float(state.keyMenuAnchor.y());
        const float x = ImClamp(anchorX, 0.0f, ImMax(0.0f, io.DisplaySize.x - popupSize.x));
        const float y = (anchorY + popupSize.y <= io.DisplaySize.y) ? anchorY
                                                                   : ImMax(0.0f, io.DisplaySize.y - popupSize.y);

        // The fixed layout the header documents: no item spacing (a row is then
        // exactly rowHeight tall) and this popup's own padding. Pushed around the
        // window, so they cover the popup's whole content.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMenuPadX, kMenuPadY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        if (ImGui::BeginPopup(kKeyMenuId)) {
            // The content origin, padding included: every entry is placed
            // relative to it, which is what makes the layout above exact.
            const ImVec2 origin = ImGui::GetCursorPos();

            for (int r = 0; r < easingRows; ++r) {
                for (int c = 0; c < kMenuColumns; ++c) {
                    const int index = r * kMenuColumns + c;
                    if (index >= types.size())
                        break;
                    const util::Easing::Type type = types.at(index);
                    // The label is the timeline's own easing name - util::Easing
                    // ::getTypeName, i.e. the "EasingName" context, the very call
                    // TimeLineEditorWidget's easing menu builds its entries from.
                    const QByteArray label = util::Easing::getTypeName(type).toUtf8();
                    ImGui::SetCursorPos(ImVec2(
                        origin.x + c * (columnWidth + kMenuColumnGap), origin.y + r * rowHeight
                    ));
                    if (ImGui::Selectable(
                            label.constData(), false, ImGuiSelectableFlags_None, ImVec2(columnWidth, rowHeight)
                        )) {
                        // The editor's own action: the graph's whole selection,
                        // ONE undo step, on the timeline's notifier and event.
                        aEditor.setSelectedKeysEasing(type);
                        ImGui::CloseCurrentPopup();
                        requestRepaint(); // the frame was built before the edit
                    }
                }
            }

            // The hairline is an ITEM of the popup (so its auto-height counts it)
            // but not an entry (so the row arithmetic above does not).
            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + easingRows * rowHeight));
            ImGui::Separator();
            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + easingRows * rowHeight + kMenuSeparatorHeight));
            if (ImGui::Selectable(
                    deleteLabel.constData(), false, ImGuiSelectableFlags_None, ImVec2(contentWidth, rowHeight)
                )) {
                // The panel's own remove action, the same call a key press plus
                // the Delete key reaches in the timeline: it removes exactly the
                // key the panel's selection names - which the right-click that
                // opened this menu just set.
                aEditor.removeSelectedKey();
                ImGui::CloseCurrentPopup();
                requestRepaint();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }

    // ImGui owns the answer, and it is read here rather than guessed at: an entry
    // chosen, a click outside and the panel going away all end with the popup
    // closed. The class keeps this copy because a press arrives BETWEEN frames,
    // where ImGui's own open state cannot be asked for (GetID needs a current
    // window).
    state.keyMenuOpen = ImGui::IsPopupOpen(kKeyMenuId, ImGuiPopupFlags_None);
}

//-------------------------------------------------------------------------------------------------
void ImGuiOverlay::publishAccessibility() {
    if (!mState)
        return;

    const GraphPanelModel& model = mState->model;
    ctrl::GraphEditor* editor = mState->editor;

    a11y::Model published;
    published.panel = model.panel;

    if (!editor) {
        // The panel is not on screen. The viewport stays a named pane with no
        // content, which is what it is.
        a11y::sync(published);
        return;
    }

    // One item per legend swatch: the same control the pointer toggles, with the
    // row's own label as its name and its state as the value.
    //
    // A row with NO ROOM is NOT published: its swatch is empty, so an item carrying
    // that rect would be a control at the panel's corner that is nowhere on screen
    // (and a rect of zero size is not something Qt can map to a screen position
    // either). Rows are only skipped for want of room, never for any other reason,
    // and the count of them is named in the summary below - a screen-reader user has
    // no other way to learn that the channel exists and is controllable.
    int roomlessRows = 0;
    for (int i = 0; i < model.legend.size(); ++i) {
        const PanelLegendRow& row = model.legend.at(i);
        if (row.swatch.isEmpty()) {
            ++roomlessRows;
            continue;
        }
        a11y::Item item;
        item.role = a11y::Item::CheckBox;
        item.id = i;
        item.name = row.label;
        item.value = row.visible ? trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "shown"))
                                 : trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "hidden"));
        item.rect = row.swatch;
        item.checked = row.visible;
        item.enabled = row.node != nullptr;
        published.items.append(item);
    }

    // The panel in one line: its own reason while it is empty (the model states it
    // in words), otherwise how much is shown - plus the focused channel, which is
    // the one piece of the panel's state with no control of its own.
    QString summary;
    if (!model.emptyState.isEmpty())
        summary = model.emptyState;
    else
        summary = trText(QT_TRANSLATE_N_NOOP("gui::ImGuiOverlay", "%n channel(s) shown"), int(model.curves.size()));
    if (editor->hasFocus() && editor->focusedNode()) {
        const QString label =
            editor->focusedNode()->name() + QStringLiteral(" : ") + editor->focusedChannel().name();
        summary = QCoreApplication::translate(kTranslationContext, QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "%1, focused: %2")).arg(summary, label);
    }

    // THE SELECTED KEY, when the graph's selection names one: which key it is and
    // which easing it carries. The key context menu acts on that selection, and
    // the menu is an ImGui popup - it is not in this tree, like the repository's
    // own QMenus (see the class comment) - so this line is how the state the menu
    // changes stays readable: choose an easing from it and the panel says so. The
    // easing name is the timeline's own (util::Easing::getTypeName, the
    // "EasingName" context), so the two agree in every translation.
    {
        core::TimeLineEvent selection;
        if (editor->retrieveSelection(selection) && selection.targets().size() == 1) {
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
    }

    if (roomlessRows > 0) {
        // The channels the viewport could not lay out, stated rather than dropped in
        // silence: they keep their curves and they are NOT listed above. %n is the
        // count, so the pair is the plural form.
        summary = QCoreApplication::translate(
                      kTranslationContext,
                      QT_TRANSLATE_N_NOOP(
                          "gui::ImGuiOverlay", "%1 (%n more channel(s) not listed: no room in the panel)"
                      ),
                      nullptr, roomlessRows
        )
                      .arg(summary);
    }

    a11y::Item summaryItem;
    summaryItem.role = a11y::Item::StaticText;
    summaryItem.id = kSummaryItemId;
    summaryItem.name = summary;
    summaryItem.rect = model.plot.isEmpty() ? model.panel : model.plot;
    published.items.append(summaryItem);

    // The fit action. Its rectangle is the plot because the gesture that performs it
    // is the double-click on the plot: an assistive tool activates the action
    // directly (the interfaces call back into onAccessibilityAction) rather than
    // clicking, and a click where this item says it is lands on the plot, i.e. on
    // the same region the gesture is bound to.
    a11y::Item fitItem;
    fitItem.role = a11y::Item::PushButton;
    fitItem.id = kFitItemId;
    fitItem.name = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "Fit graph"));
    fitItem.value = trText(QT_TRANSLATE_NOOP("gui::ImGuiOverlay", "double-click the plot"));
    fitItem.rect = model.plot.isEmpty() ? model.panel : model.plot;
    published.items.append(fitItem);

    a11y::sync(published);
}

void ImGuiOverlay::onAccessibilityAction(int aId) {
    if (!mState || !mState->editor)
        return;

    ctrl::GraphEditor& editor = *mState->editor;
    if (aId == kFitItemId) {
        editor.fitAll();
        requestRepaint();
        return;
    }

    // A legend swatch: the very action a pointer press on it performs.
    if (aId >= 0 && aId < mState->model.legend.size()) {
        const PanelLegendRow& row = mState->model.legend.at(aId);
        if (row.node)
            editor.setChannelVisible(*row.node, row.channel, !row.visible);
        requestRepaint();
    }
}

void ImGuiOverlay::requestRepaint() {
    if (mState && mState->widget)
        mState->widget->update();
}

} // namespace gui
