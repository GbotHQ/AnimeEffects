// The animation graph panel's PIXEL CONTRACT.
//
// This is the pixel gate of the ImGui panel (gui::drawPanel, src/gui/ImGuiOverlay.cpp)
// and the successor of tests/graph_render_pixels.cpp, which pinned the RASTER
// painter renderer (ctrl::graph::Renderer into a QImage). That renderer is deleted
// in the cutover, and the panel must not lose its pixel gate with it: this file
// drives the SHIPPING drawing path - a real ImGui frame, the real GL3 backend,
// the real framebuffer - and reads the pixels back with glReadPixels.
//
// WHAT IS DRIVEN. Not the application: no ctrl::GraphEditor, no document, no
// widget paint event. A SYNTHETIC gui::GraphPanelModel is built by hand from the
// model's own coordinates, and gui::drawPanel is handed it inside one ImGui
// frame. drawPanel is a pure function of (model, palette, focus), so the pixels
// it leaves in the framebuffer are the panel's pixels.
//
// WHAT IT PINS, and why each assertion is a regression gate:
//   1 no grid, no background. A rect inside the plot that holds none of the
//     model's geometry (verified against EVERY tick, label, curve sample, key,
//     playhead, zero rule and legend item the model publishes, each grown to its
//     own ink bound) must come back byte-identical to the clear colour. If a
//     background or a grid line is ever added, this is the assertion that fails.
//   2 the curve stroke lands where the model says: sampled points along the
//     synthetic polylines - including one between two key squares - carry ink.
//   3/5 the key squares and the legend swatches are painted, and their colours
//     are the palette's own tokens (extraction stated at each site).
//   4 the label halo, behind gui::kDrawTextHalo (SHIPPED OFF): with the flag on, a
//     pixel 1-2px outside the advance box is inked and is closer to
//     theme::Graph::halo() than to theme::Graph::axisText(), so it is the halo and
//     not a glyph; with it off - which is what ships - nothing is painted outside
//     the glyph box at all, which is the same statement from the other side. The
//     flag is read from src/gui/ImGuiOverlay.h, so this file cannot drift from the
//     panel's own configuration.
//   6 the empty state: the reason is drawn, and the rest of the plot stays clean.
//   7 the plot clip: a curve that leaves the value window in y paints NOTHING
//     outside the plot - not into the value column, not below the panel.
//   8 the zero rule: present at zeroY, absent at zeroY == -1.
//   9 a legend row with NO ROOM (an empty swatch) has no label box, paints nothing
//     (the region its derived box would land in stays clean) and contributes no
//     geometry at all - the row is still published by the model, so the guard is
//     the only thing that keeps its empty rect from becoming a control at the
//     panel's corner.
//  10 the Custom tangent handles: each handle's stem carries ink at its own
//     midpoint and each dot is theme::Graph::handleFill() at its own coordinates;
//     with io.MousePos on one dot (a second pass that differs ONLY in the pointer)
//     that dot turns theme::Graph::selected() while every other dot is unchanged -
//     the highlight at the same distance and in the same token the editor's own
//     hit-test grabs through.
//  11-15 THE CONTRAST OUTLINE AND COMPOSITE PASS (gui::outline), driven directly
//     with hand-authored textures: a low-contrast stroke over a bright patch is
//     ringed in black and the ring's strength follows the stroke's coverage; the
//     same stroke over a contrasting patch is left alone BYTE FOR BYTE; an empty
//     overlay leaves a patterned backdrop byte-identical through the pass's own
//     blit (the inverse "always draws" assertion); a 50%-covered pixel blends with
//     the backdrop instead of replacing it; and the optional +/-2px second ring
//     reaches a pixel the shipped 1px ring leaves alone. See the block above those
//     checks for what each one is a regression gate on.
//
// Every number is printed as `MEASURED <what> ...` so a failure states the
// measurement, not only the verdict, and every precondition is asserted so a run
// that could not observe the behaviour fails loudly instead of passing quietly.
//
// HOW IT OWNS ITS PIXELS. The same bootstrap as tests/overlay_engine_primitives.cpp:
// a real core-profile QOpenGLWidget context (QSurfaceFormat + gl::Global::kVersion),
// gl::Global::setContext/setFunctions over QOpenGLFunctions_4_0_Core, and this
// file's OWN framebuffer, read with glReadPixels. Never QOpenGLWidget::grabFramebuffer(),
// which re-enters Qt's render and invalidates the colour buffer. The widget's
// paint event is never involved; run it under xvfb-run.
//
// The viewport is 400x320 while the panel is 400x300: the 20 rows below the panel
// are the visible "below the panel" probe of assertion 7.
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QOpenGLFunctions_4_0_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLWidget>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QSurfaceFormat>
#include <QVector>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

// ImDrawListSharedData, i.e. the frame's font and font size that gui::drawPanel
// reads off the draw list itself (src/gui/ImGuiOverlay.cpp does the same). The
// label geometry below has to be measured with THE SAME font the glyphs are
// rasterized with, or "outside the glyph box" would be a statement about a box
// nobody drew.
#include "imgui_internal.h"

#include "XCAssert.h"
#include "gl/DeviceInfo.h"
#include "gl/Framebuffer.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/VertexArrayObject.h"

#include "gui/GraphPanelModel.h"
#include "gui/ImGuiOverlay.h"
#include "gui/theme/Colors.h"
#include "gui/theme/Graph.h"
#include "ctrl/graph/graph_Geometry.h"

// The ctrl/core archives this target links reference the GUI
// AudioPlaybackWidget::deserialize (core::Project.cpp uses it for a .aemus
// sidecar). Nothing here ever loads a project, so stub it with the same one-liner
// tests/overlay_engine_primitives.cpp uses and the target needs no gui library.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

class HarnessAssertHandler: public XCAssertHandler {
public:
    // An engine invariant firing means this harness can no longer describe what
    // it measured (a pending GL error, an unexpected state). Die loudly.
    void failure() const override { std::abort(); }
};

class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString& aText, const QString& aInfo, const QString& aDetail) const override {
        std::fprintf(stderr, "XC_FATAL %s %s %s\n", qPrintable(aText), qPrintable(aInfo), qPrintable(aDetail));
        std::abort();
    }
};

HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;

#define REQUIRE(cond, ...)                                                                              \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::printf("FAIL %d: ", __LINE__);                                                         \
            std::printf(__VA_ARGS__);                                                                   \
            std::printf("\n");                                                                          \
            return false;                                                                               \
        }                                                                                               \
    } while (false)

std::string hex(const QColor& aColor) { return aColor.name(QColor::HexRgb).toStdString(); }

// The framebuffer. WIDER AND TALLER THAN THE PANEL on purpose: the panel covers
// a 400x300 viewport, so rows 300..319 of the 400x320 attachment are "below the
// panel" and can be sampled (assertion 7).
constexpr int kViewW = 400;
constexpr int kViewH = 320;

const QRect kPanel(0, 0, 400, 300);
const QRect kPlot(90, 30, 290, 240);        // x 90..379, y 30..269
const QRect kValueColumn(6, 30, 78, 240);   // x 6..83, clear of the plot

const core::TimeKeyType kTypeA = core::TimeKeyType_Move;
const core::TimeKeyType kTypeB = core::TimeKeyType_Scale;

// The colour every pass clears to. Magenta: the panel draws no background, so
// everything the panel DOES draw has to move away from it. The precondition in
// main() proves that claim for every colour this model can paint, instead of
// assuming it.
const QColor kClear(255, 0, 255);

// "this pixel carries ink": the largest per-channel distance from the clear
// colour. 24 is far above the readback's noise (there is none: every pass is a
// fresh clear plus one blur-free draw) and far below any real stroke.
constexpr int kInkMin = 24;

// An OPAQUE fill (a key square's body, a legend swatch, the key ring) is the
// palette token itself, so it may be compared tightly.
constexpr int kFillTol = 10;

// An antialiased 1.5px stroke lands with partial coverage on its own fringes, so
// the curve colour is searched for in a small ring with a looser tolerance.
constexpr int kCurveTol = 48;

enum class Align { Left, Centre, Right };

gl::Framebuffer* gFbo = nullptr;

//-------------------------------------------------------------------------------------------------
// Read the whole attachment back as a TOP-DOWN RGBA image, with the FBO bound.
// GL's origin is bottom-left, so glReadPixels hands back the bottom row first:
// without the flip every sampled pixel below would come from the mirrored row.
QImage readFboTopDown() {
    QImage image(kViewW, kViewH, QImage::Format_RGBA8888);
    gl::Global::functions().glReadPixels(0, 0, kViewW, kViewH, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    return image.flipped(Qt::Vertical);
}

QColor pixelAt(const QImage& aImage, int aX, int aY) {
    const uchar* line = aImage.constScanLine(aY);
    const uchar* pixel = line + 4 * aX;
    return QColor(int(pixel[0]), int(pixel[1]), int(pixel[2]), int(pixel[3]));
}

int colourDistance(const QColor& a, const QColor& b) {
    return std::max(
        {std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue()),
         std::abs(a.alpha() - b.alpha())}
    );
}

int clearDistance(const QColor& aColor) { return colourDistance(aColor, kClear); }

int inkCount(const QImage& aImage, const QRect& aRect) {
    const QRect r = aRect.intersected(aImage.rect());
    int count = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            if (clearDistance(pixelAt(aImage, x, y)) > kInkMin) {
                ++count;
            }
        }
    }
    return count;
}

QRect grow(const QRect& aRect, int aPixels) { return aRect.adjusted(-aPixels, -aPixels, aPixels, aPixels); }

// How many pixel columns/rows separate two rects; 0 when they touch or overlap.
int rectGap(const QRect& a, const QRect& b) {
    const int dx = std::max(0, std::max(a.left() - b.right() - 1, b.left() - a.right() - 1));
    const int dy = std::max(0, std::max(a.top() - b.bottom() - 1, b.top() - a.bottom() - 1));
    return std::max(dx, dy);
}

// The glyph box drawLabel's own placement produces: the same font, the same
// alignment and the same vertical centring as src/gui/ImGuiOverlay.cpp's
// drawLabel(). Rebuilding it here is what lets the halo assertion say "this pixel
// is OUTSIDE the glyph box" rather than "somewhere near the text".
QRect glyphBox(const QRect& aBox, const QString& aText, ImFont* aFont, float aFontSize, Align aAlign) {
    if (!aFont || aText.isEmpty() || aBox.isEmpty()) {
        return QRect();
    }
    const QByteArray utf8 = aText.toUtf8();
    const ImVec2 size = aFont->CalcTextSizeA(aFontSize, FLT_MAX, 0.0f, utf8.constData());
    float x = float(aBox.left());
    if (aAlign == Align::Centre) {
        x += (float(aBox.width()) - size.x) * 0.5f;
    } else if (aAlign == Align::Right) {
        x = float(aBox.left() + aBox.width()) - size.x;
    }
    const float y = float(aBox.top()) + (float(aBox.height()) - size.y) * 0.5f;
    return QRect(int(std::floor(x)), int(std::floor(y)), int(std::ceil(size.x)), int(std::ceil(size.y)));
}

// The glyph box of a legend row's label: legendLabelRect()'s own formula (the
// swatch's right edge + kLegendLabelGap, vertically centred on the swatch) and
// then the left-aligned glyph box inside it.
QRect legendLabelGlyphBox(const gui::PanelLegendRow& aRow, ImFont* aFont, float aFontSize) {
    if (!aFont || aRow.label.isEmpty()) {
        return QRect();
    }
    const QByteArray utf8 = aRow.label.toUtf8();
    const ImVec2 size = aFont->CalcTextSizeA(aFontSize, FLT_MAX, 0.0f, utf8.constData());
    const int width = int(size.x + 0.5f);
    const int height = int(size.y + 0.5f);
    if (width <= 0 || height <= 0) {
        return QRect();
    }
    const QRect box(aRow.swatch.right() + 1 + 6, aRow.swatch.center().y() - height / 2, width, height);
    return glyphBox(box, aRow.label, aFont, aFontSize, Align::Left);
}

//-------------------------------------------------------------------------------------------------
// The synthetic model. Every coordinate below is chosen so that the assertions
// can point at a region and say exactly which geometry is and is not in it.
QRect keySquare(int aCentreX, int aCentreY) { return QRect(aCentreX - 3, aCentreY - 3, 7, 7); }

gui::GraphPanelModel makeFullModel(const theme::Graph& aTheme) {
    gui::GraphPanelModel model;
    model.panel = kPanel;
    model.plot = kPlot;
    model.valueColumn = kValueColumn;

    model.frameTickX = {140, 320};
    model.valueTickY = {70, 210};
    model.frameLabels = {
        {QRect(130, 40, 20, 14), QStringLiteral("10")},
        {QRect(310, 40, 20, 14), QStringLiteral("20")},
    };
    model.valueLabels = {
        {QRect(6, 60, 78, 14), QStringLiteral("1")},
        {QRect(6, 240, 78, 14), QStringLiteral("0")},
    };
    model.zeroY = 180;
    model.playheadX = 200;

    gui::PanelCurve a;
    a.node = nullptr;
    a.channel = ctrl::graph::Channel(kTypeA, 0);
    a.points = {QPointF(110, 240), QPointF(150, 220), QPointF(200, 230), QPointF(250, 200), QPointF(300, 210)};
    a.keys = {keySquare(150, 220), keySquare(250, 200)};
    // Two tangent handles, as a Custom segment's pair. Their bases sit ON the
    // curve (a real handle's base is the key it belongs to, but a base on a key
    // centre would put the stem's own 1px over that key's sampled centre and
    // confound the key-square assertion), and their dots are clear of every
    // other element. All four points are integers, so the pointer probe below
    // can name the dot exactly.
    {
        gui::PanelHandle first;
        first.segment = 0;
        first.index = 0;
        first.base = QPointF(200, 230);
        first.dot = QPointF(185, 255);
        first.radius = ctrl::graph::kHandleRadius;
        gui::PanelHandle second;
        second.segment = 1;
        second.index = 1;
        second.base = QPointF(275, 205);
        second.dot = QPointF(300, 240);
        second.radius = ctrl::graph::kHandleRadius;
        a.handles = {first, second};
    }
    a.color = aTheme.keyColor(kTypeA);
    a.legendIndex = 0;

    gui::PanelCurve b;
    b.node = nullptr;
    b.channel = ctrl::graph::Channel(kTypeB, 0);
    b.points = {QPointF(110, 120), QPointF(170, 140), QPointF(230, 100), QPointF(290, 130)};
    b.keys = {keySquare(170, 140), keySquare(290, 130)};
    b.color = aTheme.keyColor(kTypeB);
    b.legendIndex = 1;

    model.curves = {a, b};

    gui::PanelLegendRow rowA;
    rowA.node = nullptr;
    rowA.channel = a.channel;
    rowA.label = QStringLiteral("Move X");
    rowA.color = a.color;
    rowA.swatch = QRect(96, 244, 10, 10);
    rowA.visible = true;

    gui::PanelLegendRow rowB;
    rowB.node = nullptr;
    rowB.channel = b.channel;
    rowB.label = QStringLiteral("Scale X");
    rowB.color = b.color;
    rowB.swatch = QRect(96, 256, 10, 10);
    rowB.visible = true;

    model.legend = {rowA, rowB};
    return model;
}

// The clip model: ONE curve and nothing else, because every other element of the
// panel is legitimately drawn outside the plot (the value labels live in the
// value column). With nothing else in the model, "no ink outside the plot" is a
// statement about the CLIP alone.
gui::GraphPanelModel makeClipModel(const theme::Graph& aTheme) {
    gui::GraphPanelModel model;
    model.panel = kPanel;
    model.plot = kPlot;
    model.valueColumn = kValueColumn;

    gui::PanelCurve curve;
    curve.node = nullptr;
    curve.channel = ctrl::graph::Channel(kTypeA, 0);
    // 650px above and 230px below a 240px-tall plot: the two steep segments leave
    // the plot's y range immediately, and the third runs across its middle.
    curve.points = {QPointF(110, -500), QPointF(150, 150), QPointF(250, 150), QPointF(290, 500)};
    curve.color = aTheme.keyColor(kTypeA);
    curve.legendIndex = -1;
    model.curves = {curve};
    return model;
}

gui::GraphPanelModel makeEmptyModel() {
    gui::GraphPanelModel model;
    model.panel = kPanel;
    model.plot = kPlot;
    model.valueColumn = kValueColumn;
    model.frameTickX = {140};
    model.valueTickY = {70};
    model.frameLabels = {{QRect(130, 40, 20, 14), QStringLiteral("10")}};
    model.valueLabels = {{QRect(6, 60, 78, 14), QStringLiteral("1")}};
    model.emptyState = QStringLiteral("No keys on the selected layers");
    return model;
}

//-------------------------------------------------------------------------------------------------
struct Rendered {
    QImage image;
    int vtx = 0;
    int idx = 0;
    int cmdLists = 0;
    ImFont* font = nullptr;
    float fontSize = 0.0f;
};

// One whole frame through the real backend, into this file's own framebuffer.
// The order is the one gui::ImGuiOverlay::paint() uses and it is load-bearing in
// 1.92: ImGui_ImplOpenGL3_NewFrame() (the caller's duty, it creates the backend's
// device objects) BEFORE ImGui::NewFrame().
Rendered renderPass(
    const gui::GraphPanelModel& aModel, const theme::Graph& aTheme, const gui::PanelFocus& aFocus,
    const char* aName, const ImVec2& aMouse = ImVec2(-FLT_MAX, -FLT_MAX)
) {
    auto& funcs = gl::Global::functions();

    gFbo->bind();
    // What ImGui_ImplOpenGL3_RenderDrawData does NOT restore (its scissor box and
    // viewport are left as the last draw command set them). A glClear() is itself
    // scissored, so both are re-established before the clear.
    funcs.glDisable(GL_SCISSOR_TEST);
    funcs.glScissor(0, 0, kViewW, kViewH);
    funcs.glViewport(0, 0, kViewW, kViewH);
    funcs.glClearColor(kClear.redF(), kClear.greenF(), kClear.blueF(), 1.0f);
    funcs.glClear(GL_COLOR_BUFFER_BIT);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(kViewW), float(kViewH));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // drawPanel highlights the tangent handle nearest io.MousePos, so the pointer
    // is part of the frame this harness drives. Off-screen by default (no
    // highlight); the handle assertion passes a dot's own coordinates.
    io.MousePos = aMouse;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    gui::drawPanel(*drawList, aModel, aTheme, aFocus);

    Rendered out;
    out.font = drawList->_Data ? drawList->_Data->Font : nullptr;
    out.fontSize = drawList->_Data ? drawList->_Data->FontSize : 0.0f;

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    out.vtx = drawData->TotalVtxCount;
    out.idx = drawData->TotalIdxCount;
    out.cmdLists = drawData->CmdListsCount;
    ImGui_ImplOpenGL3_RenderDrawData(drawData);

    out.image = readFboTopDown();
    gFbo->release();

    std::printf(
        "MEASURED pass=%s vtx=%d idx=%d cmd_lists=%d font_px=%d\n", aName, out.vtx, out.idx, out.cmdLists,
        int(std::lround(out.fontSize))
    );
    return out;
}

//-------------------------------------------------------------------------------------------------
// The model's own geometry, as ink bounds. Used to PROVE that the no-geometry
// probe rect of assertion 1 holds nothing: a curve is represented by its whole
// polyline sampled at 1px steps (a stroke is a line, not just its vertices), and
// every rect is grown by the ink its own draw call can add beyond its box.
struct Geom {
    std::string name;
    QRect rect;
};

void addCurveSamples(std::vector<Geom>& aOut, const QVector<QPointF>& aPoints) {
    for (int i = 0; i + 1 < aPoints.size(); ++i) {
        const QPointF a = aPoints.at(i);
        const QPointF b = aPoints.at(i + 1);
        const int steps = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y())) + 1);
        for (int s = 0; s <= steps; ++s) {
            const double t = double(s) / double(steps);
            const double x = a.x() + t * (b.x() - a.x());
            const double y = a.y() + t * (b.y() - a.y());
            // A 1.5px stroke's centre + its AA fringe, grown by 2 on top.
            aOut.push_back({"curve", grow(QRect(int(std::floor(x)) - 1, int(std::floor(y)) - 1, 3, 3), 2)});
        }
    }
}

std::vector<Geom> geometryOf(
    const gui::GraphPanelModel& aModel, ImFont* aFont, float aFontSize
) {
    std::vector<Geom> out;
    for (int x : aModel.frameTickX) {
        out.push_back({"frame_tick", grow(QRect(x, aModel.plot.top() - 1, 1, 6), 2)});
    }
    for (int y : aModel.valueTickY) {
        out.push_back({"value_tick", grow(QRect(aModel.plot.left() - 1, y, 6, 1), 2)});
    }
    for (const gui::PanelLabel& label : aModel.frameLabels) {
        out.push_back({"frame_label", grow(glyphBox(label.rect, label.text, aFont, aFontSize, Align::Centre), 2)});
    }
    for (const gui::PanelLabel& label : aModel.valueLabels) {
        out.push_back({"value_label", grow(glyphBox(label.rect, label.text, aFont, aFontSize, Align::Right), 2)});
    }
    if (aModel.zeroY >= 0) {
        out.push_back(
            {"zero_rule", grow(QRect(aModel.plot.left(), aModel.zeroY, aModel.plot.width() + 1, 1), 2)}
        );
    }
    if (aModel.playheadX >= 0) {
        out.push_back({"playhead", grow(QRect(aModel.playheadX, aModel.plot.top(), 1, aModel.plot.height() + 1), 2)});
    }
    for (const gui::PanelCurve& curve : aModel.curves) {
        addCurveSamples(out, curve.points);
        for (const QRect& key : curve.keys) {
            out.push_back({"key", grow(key, 2)});
        }
        for (const gui::PanelHandle& handle : curve.handles) {
            addCurveSamples(out, QVector<QPointF>{handle.base, handle.dot}); // the stem
            const QRect dot(
                int(handle.dot.x()) - handle.radius, int(handle.dot.y()) - handle.radius,
                2 * handle.radius + 1, 2 * handle.radius + 1
            );
            out.push_back({"handle_dot", grow(dot, 2)});
        }
    }
    for (const gui::PanelLegendRow& row : aModel.legend) {
        out.push_back({"legend_swatch", grow(row.swatch, 2)});
        out.push_back({"legend_label", grow(legendLabelGlyphBox(row, aFont, aFontSize), 2)});
    }
    if (!aModel.emptyState.isEmpty()) {
        out.push_back(
            {"empty_state",
             grow(glyphBox(aModel.plot, aModel.emptyState, aFont, aFontSize, Align::Centre), 2)}
        );
    }
    return out;
}

//-------------------------------------------------------------------------------------------------
// 0: the machinery's own control. drawPanel returns immediately for a model with
// no panel rect, so a pass that draws nothing must count nothing - otherwise the
// readback or the counter invents pixels and every number below is worthless.
bool checkControl(const Rendered& aRendered) {
    const int ink = inkCount(aRendered.image, QRect(0, 0, kViewW, kViewH));
    std::printf("MEASURED control ink_pixels=%d of %d\n", ink, kViewW * kViewH);
    REQUIRE(
        aRendered.vtx == 0, "the control pass emitted %d vertices for a model with no panel", aRendered.vtx
    );
    REQUIRE(ink == 0, "the control pass painted %d pixels while nothing was drawn", ink);
    return true;
}

// 1: the inverse no-grid assertion.
bool checkNoGrid(const Rendered& aRendered, const gui::GraphPanelModel& aModel) {
    REQUIRE(aRendered.font != nullptr, "the draw list carried no font: the label geometry cannot be described");
    REQUIRE(!aModel.panel.isEmpty() && !aModel.plot.isEmpty(), "precondition: the model must have a plot");

    // A rect inside the plot, in the top-right quadrant, chosen against the model
    // below: the nearest thing to it is the second frame label's halo.
    const QRect probe(340, 200, 30, 30);
    REQUIRE(kPlot.contains(probe), "precondition: the probe rect must be inside the plot");
    REQUIRE(kPanel.contains(probe), "precondition: the probe rect must be inside the panel");
    REQUIRE(!probe.intersects(aModel.valueColumn), "precondition: the probe rect must be outside the value column");

    const std::vector<Geom> geoms = geometryOf(aModel, aRendered.font, aRendered.fontSize);
    std::string nearest = "<none>";
    int minGap = -1;
    for (const Geom& geom : geoms) {
        REQUIRE(
            !probe.intersects(geom.rect),
            "precondition: the no-geometry probe rect (%d,%d %dx%d) intersects %s (%d,%d %dx%d)", probe.left(),
            probe.top(), probe.width(), probe.height(), geom.name.c_str(), geom.rect.left(), geom.rect.top(),
            geom.rect.width(), geom.rect.height()
        );
        const int gap = rectGap(probe, geom.rect);
        if (minGap < 0 || gap < minGap) {
            minGap = gap;
            nearest = geom.name;
        }
    }

    const int ink = inkCount(aRendered.image, probe);
    // The control that guards against a false negative: the SAME rect, moved onto
    // the zero rule, must come back inked. If it did not, "0 ink" above would be a
    // property of the counter rather than of the panel.
    const QRect onRule(340, aModel.zeroY - 15, 30, 30);
    const int ruleInk = inkCount(aRendered.image, onRule);
    std::printf(
        "MEASURED no_grid sensitivity_control rect=(%d,%d %dx%d) contains_zero_rule=1 ink_pixels=%d of %d\n",
        onRule.left(), onRule.top(), onRule.width(), onRule.height(), ruleInk, onRule.width() * onRule.height()
    );
    REQUIRE(
        ruleInk > 0,
        "the no-grid probe counted 0 pixels over a rect that contains the zero rule: the probe cannot see ink at all"
    );
    std::printf(
        "MEASURED no_grid rect=(%d,%d %dx%d) geometry_items=%d nearest=%s gap_px=%d ink_pixels=%d of %d\n",
        probe.left(), probe.top(), probe.width(), probe.height(), int(geoms.size()), nearest.c_str(), minGap, ink,
        probe.width() * probe.height()
    );
    std::printf(
        "MEASURED no_grid margins: left_to_value_column=%dpx right_to_plot_edge=%dpx top_to_zero_rule=%dpx "
        "bottom_to_plot_edge=%dpx\n",
        probe.left() - aModel.valueColumn.right() - 1, kPlot.right() - probe.right(), probe.top() - aModel.zeroY,
        kPlot.bottom() - probe.bottom()
    );
    REQUIRE(
        ink == 0,
        "the panel painted %d pixels in a rect the model says holds no geometry: the panel must have no"
        " background and no grid line (nearest geometry: %s at %dpx)",
        ink, nearest.c_str(), minGap
    );
    return true;
}

// 2: the curve stroke lands where the model says.
//
// Each model point is sampled EXACTLY (tolerance 0) where the stroke runs through
// the pixel, and within 1px where the point is a polyline VERTEX: ImGui's
// antialiased stroke quad does not fully cover its own terminal vertex pixel
// (measured: (300,210) at the end of curve A comes back clear while every
// interior sample, including the ones between two key squares, is 79..120 away
// from the clear colour). The tolerance is stated per sample and printed with the
// offset that was actually found, so "within 1px of the model's point" is the
// claim being made - not "somewhere on the panel".
bool checkCurveInk(const Rendered& aRendered, const gui::GraphPanelModel& aModel) {
    struct Sample {
        QPointF point;
        const char* what;
        int tolerance;
    };
    // The samples at x=175/275 (curve A) and x=200 (curve B) sit BETWEEN two key
    // squares, where only the stroke can put ink.
    const std::vector<Sample> samples = {
        {{110, 240}, "curve_a endpoint", 1},
        {{130, 230}, "curve_a mid-segment", 0},
        {{175, 225}, "curve_a between two keys", 0},
        {{225, 215}, "curve_a mid-segment", 0},
        {{275, 205}, "curve_a between two keys", 0},
        {{300, 210}, "curve_a endpoint", 1},
        {{110, 120}, "curve_b endpoint", 1},
        {{140, 130}, "curve_b mid-segment", 0},
        {{200, 120}, "curve_b between two keys", 0},
        {{260, 115}, "curve_b mid-segment", 0},
        {{275, 122}, "curve_b mid-segment", 0},
    };

    std::vector<QRect> keySquares;
    for (const gui::PanelCurve& curve : aModel.curves) {
        for (const QRect& key : curve.keys) {
            keySquares.push_back(key);
        }
    }

    int checked = 0;
    for (const Sample& sample : samples) {
        const int x = int(std::lround(sample.point.x()));
        const int y = int(std::lround(sample.point.y()));
        REQUIRE(kPlot.contains(x, y), "precondition: sample %s (%d,%d) is outside the plot", sample.what, x, y);
        for (const QRect& key : keySquares) {
            REQUIRE(
                !grow(key, 1 + sample.tolerance).contains(x, y),
                "precondition: the neighbourhood of sample %s (%d,%d) reaches a key square", sample.what, x, y
            );
        }

        int best = clearDistance(pixelAt(aRendered.image, x, y));
        int bestX = x;
        int bestY = y;
        for (int dy = -sample.tolerance; dy <= sample.tolerance; ++dy) {
            for (int dx = -sample.tolerance; dx <= sample.tolerance; ++dx) {
                const QColor c = pixelAt(aRendered.image, x + dx, y + dy);
                if (clearDistance(c) > best) {
                    best = clearDistance(c);
                    bestX = x + dx;
                    bestY = y + dy;
                }
            }
        }
        std::printf(
            "MEASURED curve_ink sample=%s model=(%d,%d) tolerance=%dpx best=(%d,%d) offset=(%+d,%+d) rgb=%s"
            " clear_distance=%d\n",
            sample.what, x, y, sample.tolerance, bestX, bestY, bestX - x, bestY - y,
            hex(pixelAt(aRendered.image, bestX, bestY)).c_str(), best
        );
        REQUIRE(
            best > kInkMin,
            "the curve stroke has no ink within %dpx of %s (%d,%d): the nearest sample is %d from the clear colour",
            sample.tolerance, sample.what, x, y, best
        );
        ++checked;
    }
    std::printf("MEASURED curve_ink samples_with_ink=%d of %d\n", checked, int(samples.size()));
    return true;
}

// 3: the key squares. EXTRACTION: drawPanel paints a key with
// AddRectFilled(keyFill) + AddRect(keyEdge), so the square's centre is compared
// to theme::Graph::keyFill() and its ring to theme::Graph::keyEdge(); the key's
// OWN palette colour - theme::Graph::keyColor(channel.type()), the colour the
// curve and the legend row carry - is then required to be present within 4px of
// the square, because the curve passes through it.
bool checkKeys(const Rendered& aRendered, const gui::GraphPanelModel& aModel, const theme::Graph& aTheme) {
    int keys = 0;
    for (const gui::PanelCurve& curve : aModel.curves) {
        const QColor own = aTheme.keyColor(curve.channel.type());
        for (const QRect& key : curve.keys) {
            REQUIRE(kPlot.contains(key), "precondition: a key square lies outside the plot");
            const QPoint centre = key.center();
            const QColor inner = pixelAt(aRendered.image, centre.x(), centre.y());
            const QColor ring = pixelAt(aRendered.image, key.left(), centre.y());

            std::printf(
                "MEASURED key channel_type=%d rect=(%d,%d %dx%d) centre_rgb=%s keyFill=%s d=%d ring_rgb=%s"
                " keyEdge=%s d=%d\n",
                int(curve.channel.type()), key.left(), key.top(), key.width(), key.height(), hex(inner).c_str(),
                hex(aTheme.keyFill()).c_str(), colourDistance(inner, aTheme.keyFill()), hex(ring).c_str(),
                hex(aTheme.keyEdge()).c_str(), colourDistance(ring, aTheme.keyEdge())
            );

            REQUIRE(
                clearDistance(inner) > kInkMin, "the key square at (%d,%d) has no ink at its centre: rgb=%s",
                centre.x(), centre.y(), hex(inner).c_str()
            );
            REQUIRE(
                colourDistance(inner, aTheme.keyFill()) <= kFillTol,
                "the key centre rgb=%s is not theme::Graph::keyFill() rgb=%s (distance %d > %d)", hex(inner).c_str(),
                hex(aTheme.keyFill()).c_str(), colourDistance(inner, aTheme.keyFill()), kFillTol
            );
            REQUIRE(
                colourDistance(ring, aTheme.keyEdge()) <= kFillTol,
                "the key ring rgb=%s is not theme::Graph::keyEdge() rgb=%s (distance %d > %d)", hex(ring).c_str(),
                hex(aTheme.keyEdge()).c_str(), colourDistance(ring, aTheme.keyEdge()), kFillTol
            );

            // The key's own palette colour, searched in the ring of pixels 2..4px
            // outside the square (the square's own 1px growth is excluded, so the
            // colour cannot come from the square's edge).
            int best = 1 << 20;
            for (int y = key.top() - 4; y <= key.bottom() + 4; ++y) {
                for (int x = key.left() - 4; x <= key.right() + 4; ++x) {
                    if (grow(key, 1).contains(x, y) || !kPlot.contains(x, y)) {
                        continue;
                    }
                    const QColor c = pixelAt(aRendered.image, x, y);
                    if (clearDistance(c) <= kInkMin) {
                        continue;
                    }
                    best = std::min(best, colourDistance(c, own));
                }
            }
            std::printf(
                "MEASURED key_own_colour channel_type=%d keyColor=%s nearest_ring_distance=%d tolerance=%d\n",
                int(curve.channel.type()), hex(own).c_str(), best, kCurveTol
            );
            REQUIRE(
                best <= kCurveTol,
                "the key's own palette colour rgb=%s (keyColor of channel type %d) is not painted within 4px of"
                " its square (nearest ring pixel is %d away, tolerance %d)",
                hex(own).c_str(), int(curve.channel.type()), best, kCurveTol
            );
            ++keys;
        }
    }
    REQUIRE(keys == 4, "expected 4 key squares in the full model, walked %d", keys);
    return true;
}

// 4: the label, and the halo behind gui::kDrawTextHalo.
//
// The halo is SHIPPED OFF, so what this asserts in the shipping configuration is
// the INVERSE: nothing is painted outside the glyph box. The haloed assertions
// below it are kept whole for the flag-on run - and which of the two runs is the
// shipped one is a property of the flag itself (gui::kDrawTextHalo, declared in
// src/gui/ImGuiOverlay.h), so this file cannot drift from the panel.
bool checkLabelHalo(const Rendered& aRendered, const gui::GraphPanelModel& aModel, const theme::Graph& aTheme) {
    REQUIRE(!aModel.frameLabels.isEmpty(), "precondition: the model must carry a frame label");
    const gui::PanelLabel& label = aModel.frameLabels.at(0);
    REQUIRE(kPlot.contains(label.rect), "precondition: the label box must be inside the plot");

    const QRect glyph = glyphBox(label.rect, label.text, aRendered.font, aRendered.fontSize, Align::Centre);
    REQUIRE(!glyph.isEmpty(), "precondition: the frame font lays out '%s' with empty metrics", qPrintable(label.text));
    REQUIRE(kPlot.contains(glyph), "precondition: the glyph box must be inside the plot");

    // (a) the glyph itself is inked.
    std::vector<QColor> glyphInk;
    for (int y = glyph.top(); y <= glyph.bottom(); ++y) {
        for (int x = glyph.left(); x <= glyph.right(); ++x) {
            const QColor c = pixelAt(aRendered.image, x, y);
            if (clearDistance(c) > kInkMin) {
                glyphInk.push_back(c);
            }
        }
    }
    std::printf(
        "MEASURED label box=(%d,%d %dx%d) glyph=(%d,%d %dx%d) text=%s ink_pixels_in_glyph_box=%d of %d"
        " halo_flag=%d\n",
        label.rect.left(), label.rect.top(), label.rect.width(), label.rect.height(), glyph.left(), glyph.top(),
        glyph.width(), glyph.height(), qPrintable(label.text), int(glyphInk.size()), glyph.width() * glyph.height(),
        int(gui::kDrawTextHalo)
    );
    REQUIRE(!glyphInk.empty(), "the label '%s' painted nothing inside its own glyph box", qPrintable(label.text));

    // (b) the ring 1-2px outside the glyph box, measured against BOTH tokens. The
    // pixel is outside the advance box drawLabel measured, so no glyph pass puts ink
    // there: while the halo is on, the only passes that do are its 16 offsets and
    // their colour is theme::Graph::halo().
    const QColor axisText = aTheme.axisText();
    const QColor halo = aTheme.halo();
    int haloPixels = 0;
    int bestHaloDistance = 1 << 20;
    int worstHaloDistance = -1;
    int bestHaloX = -1;
    int bestHaloY = -1;
    for (int y = glyph.top() - 2; y <= glyph.bottom() + 2; ++y) {
        for (int x = glyph.left() - 2; x <= glyph.right() + 2; ++x) {
            if (glyph.contains(x, y) || !kPlot.contains(x, y)) {
                continue;
            }
            const QColor c = pixelAt(aRendered.image, x, y);
            if (clearDistance(c) <= kInkMin) {
                continue;
            }
            ++haloPixels;
            const int d = colourDistance(c, halo);
            if (d < bestHaloDistance) {
                bestHaloDistance = d;
                bestHaloX = x;
                bestHaloY = y;
            }
            worstHaloDistance = std::max(worstHaloDistance, d);
        }
    }

    std::printf(
        "MEASURED halo ring=1..2px_outside_glyph inked_pixels=%d best=(%d,%d) distance_to_halo=%d"
        " distance_to_axisText=%d worst_distance_to_halo=%d halo=%s axisText=%s\n",
        haloPixels, bestHaloX, bestHaloY, bestHaloDistance,
        bestHaloX >= 0 ? colourDistance(pixelAt(aRendered.image, bestHaloX, bestHaloY), axisText) : -1,
        worstHaloDistance, hex(halo).c_str(), hex(axisText).c_str()
    );

    if (!gui::kDrawTextHalo) {
        // SHIPPED: the halo is off, so the panel paints NOTHING outside a glyph box.
        // This is the assertion that catches the halo being re-enabled by accident,
        // and the one that pins "the ink protection is the outline pass's job now":
        // that protection is a separate pass with its own gate (assertions 11-15).
        REQUIRE(
            haloPixels == 0,
            "the halo flag is off but %d pixels 1-2px outside the glyph box carry ink (nearest halo distance %d):"
            " the label is still haloed",
            haloPixels, bestHaloDistance
        );
        REQUIRE(
            colourDistance(halo, axisText) > kInkMin,
            "precondition: this palette's halo and text colours are indistinguishable, so the halo cannot be told from"
            " a glyph"
        );
        return true;
    }

    REQUIRE(
        haloPixels > 0, "the halo flag is on but no pixel within 1-2px of the glyph box carries the halo"
    );
    REQUIRE(
        bestHaloDistance < colourDistance(pixelAt(aRendered.image, bestHaloX, bestHaloY), axisText),
        "the inked pixel outside the glyph box at (%d,%d) is nearer the text colour than the halo colour: it is not"
        " the halo",
        bestHaloX, bestHaloY
    );
    REQUIRE(
        colourDistance(halo, axisText) > kInkMin,
        "precondition: this palette's halo and text colours are indistinguishable, so the halo cannot be told from"
        " a glyph"
    );
    return true;
}

// 5: the legend swatches. EXTRACTION: the swatch is AddRectFilled(row.color), so
// its centre is compared to row.color AND to theme::Graph::keyColor(row.channel.type())
// - the model documents them as the same colour, and this is where that is checked.
bool checkLegend(const Rendered& aRendered, const gui::GraphPanelModel& aModel, const theme::Graph& aTheme) {
    REQUIRE(aModel.legend.size() == 2, "precondition: the full model must carry two legend rows");
    for (const gui::PanelLegendRow& row : aModel.legend) {
        REQUIRE(kPlot.contains(row.swatch), "precondition: a legend swatch lies outside the plot");
        REQUIRE(!row.swatch.isEmpty(), "precondition: a legend swatch is empty");
        const QPoint centre = row.swatch.center();
        const QColor c = pixelAt(aRendered.image, centre.x(), centre.y());
        const QColor keyColour = aTheme.keyColor(row.channel.type());

        std::printf(
            "MEASURED legend_swatch label=%s rect=(%d,%d %dx%d) centre_rgb=%s row_color=%s d=%d keyColor=%s d=%d\n",
            qPrintable(row.label), row.swatch.left(), row.swatch.top(), row.swatch.width(), row.swatch.height(),
            hex(c).c_str(), hex(row.color).c_str(), colourDistance(c, row.color), hex(keyColour).c_str(),
            colourDistance(c, keyColour)
        );
        REQUIRE(
            clearDistance(c) > kInkMin, "the legend swatch '%s' has no ink at its centre", qPrintable(row.label)
        );
        REQUIRE(
            colourDistance(c, row.color) <= kFillTol,
            "the legend swatch '%s' centre rgb=%s is not the row colour rgb=%s (distance %d > %d)",
            qPrintable(row.label), hex(c).c_str(), hex(row.color).c_str(), colourDistance(c, row.color), kFillTol
        );
        REQUIRE(
            colourDistance(c, keyColour) <= kFillTol,
            "the legend swatch '%s' centre rgb=%s is not theme::Graph::keyColor() rgb=%s (distance %d > %d)",
            qPrintable(row.label), hex(c).c_str(), hex(keyColour).c_str(), colourDistance(c, keyColour), kFillTol
        );

        // The whole swatch, not just its centre: every row's swatch must be filled.
        const int ink = inkCount(aRendered.image, row.swatch);
        std::printf(
            "MEASURED legend_swatch_fill label=%s ink_pixels=%d of %d\n", qPrintable(row.label), ink,
            row.swatch.width() * row.swatch.height()
        );
        REQUIRE(
            ink == row.swatch.width() * row.swatch.height(),
            "the legend swatch '%s' is not filled: %d of %d pixels carry ink", qPrintable(row.label), ink,
            row.swatch.width() * row.swatch.height()
        );
    }
    return true;
}

// 6: the empty state.
bool checkEmptyState(const Rendered& aRendered, const gui::GraphPanelModel& aModel) {
    REQUIRE(aModel.curves.isEmpty() && !aModel.emptyState.isEmpty(), "precondition: the model must be the empty one");
    REQUIRE(aRendered.font != nullptr, "the draw list carried no font: the empty state cannot be located");

    // The state text is drawn centred in the PLOT.
    const QRect text = glyphBox(aModel.plot, aModel.emptyState, aRendered.font, aRendered.fontSize, Align::Centre);
    REQUIRE(!text.isEmpty(), "precondition: the state string '%s' has empty metrics", qPrintable(aModel.emptyState));
    REQUIRE(kPlot.contains(text), "precondition: the centred state text must be inside the plot");

    const int textInk = inkCount(aRendered.image, grow(text, 2));
    std::printf(
        "MEASURED empty_state text=%s glyph=(%d,%d %dx%d) ink_pixels=%d\n", qPrintable(aModel.emptyState),
        text.left(), text.top(), text.width(), text.height(), textInk
    );
    REQUIRE(textInk > 0, "the empty panel painted nothing for its own reason: '%s'", qPrintable(aModel.emptyState));

    // And the rest of the plot is untouched. The probe rect is verified against
    // every other geometry item of THIS model (which has no curves at all).
    const QRect probe(350, 200, 25, 25);
    REQUIRE(kPlot.contains(probe), "precondition: the empty-state probe rect must be inside the plot");
    const std::vector<Geom> geoms = geometryOf(aModel, aRendered.font, aRendered.fontSize);
    for (const Geom& geom : geoms) {
        REQUIRE(
            !probe.intersects(geom.rect),
            "precondition: the empty-state probe rect intersects %s (%d,%d %dx%d)", geom.name.c_str(),
            geom.rect.left(), geom.rect.top(), geom.rect.width(), geom.rect.height()
        );
    }
    REQUIRE(!probe.intersects(grow(text, 2)), "precondition: the empty-state probe rect overlaps the state text");

    const int ink = inkCount(aRendered.image, probe);
    std::printf(
        "MEASURED empty_state_clean rect=(%d,%d %dx%d) margins: right_of_text+halo=%dpx above_plot_bottom=%dpx"
        " ink_pixels=%d of %d\n",
        probe.left(), probe.top(), probe.width(), probe.height(), probe.left() - (text.right() + 2),
        kPlot.bottom() - probe.bottom(), ink, probe.width() * probe.height()
    );
    REQUIRE(
        ink == 0,
        "the empty panel painted %d pixels in an otherwise empty part of the plot: the panel must have no background"
        " and no grid",
        ink
    );
    return true;
}

// 7: the plot clip.
bool checkClip(const Rendered& aRendered, const gui::GraphPanelModel& aModel) {
    REQUIRE(aModel.curves.size() == 1, "precondition: the clip model must carry exactly one curve");
    REQUIRE(!aModel.isEmpty(), "precondition: the clip model must not be an empty state");
    REQUIRE(
        aModel.valueLabels.isEmpty() && aModel.frameLabels.isEmpty() && aModel.frameTickX.isEmpty()
            && aModel.valueTickY.isEmpty() && aModel.legend.isEmpty() && aModel.playheadX < 0 && aModel.zeroY < 0,
        "precondition: the clip model must hold nothing but the curve, or 'no ink outside the plot' would have to"
        " allow for the value column's own labels"
    );

    // The one in-plot part of the polyline: the horizontal segment at y=150.
    const QRect inPlot(180, 146, 40, 9);
    REQUIRE(kPlot.contains(inPlot), "precondition: the in-plot sample rect must be inside the plot");
    const int inPlotInk = inkCount(aRendered.image, inPlot);
    std::printf(
        "MEASURED clip in_plot_rect=(%d,%d %dx%d) ink_pixels=%d of %d\n", inPlot.left(), inPlot.top(),
        inPlot.width(), inPlot.height(), inPlotInk, inPlot.width() * inPlot.height()
    );
    REQUIRE(inPlotInk > 0, "the in-plot part of the clipped curve painted nothing: the clip cannot be measured");

    // Everything the clip must hold back. The bands cover the whole attachment
    // outside the plot, including the value column and the 20 rows below the panel.
    const QRect bands[] = {
        QRect(0, 0, kPlot.left(), kViewH),                                    // left of the plot (the value column)
        QRect(kPlot.right() + 1, 0, kViewW - kPlot.right() - 1, kViewH),      // right of the plot
        QRect(kPlot.left(), 0, kPlot.width(), kPlot.top()),                   // above the plot
        QRect(kPlot.left(), kPlot.bottom() + 1, kPlot.width(), kViewH - kPlot.bottom() - 1), // below the plot
        QRect(0, kPanel.height(), kViewW, kViewH - kPanel.height()),          // below the panel
    };
    const char* bandNames[] = {"left_of_plot", "right_of_plot", "above_plot", "below_plot", "below_panel"};

    const int valueColumnInk = inkCount(aRendered.image, aModel.valueColumn);
    std::printf(
        "MEASURED clip_value_column rect=(%d,%d %dx%d) ink_pixels=%d of %d\n", aModel.valueColumn.left(),
        aModel.valueColumn.top(), aModel.valueColumn.width(), aModel.valueColumn.height(), valueColumnInk,
        aModel.valueColumn.width() * aModel.valueColumn.height()
    );
    REQUIRE(
        valueColumnInk == 0,
        "the clipped curve painted %d pixels in the value column: a curve outside the value window must not reach it",
        valueColumnInk
    );

    int total = 0;
    for (int i = 0; i < 5; ++i) {
        const int ink = inkCount(aRendered.image, bands[i]);
        total += ink;
        std::printf(
            "MEASURED clip_band band=%s rect=(%d,%d %dx%d) ink_pixels=%d\n", bandNames[i], bands[i].left(),
            bands[i].top(), bands[i].width(), bands[i].height(), ink
        );
        REQUIRE(
            ink == 0, "the clipped curve painted %d pixels in %s (%d,%d %dx%d): the plot clip is not holding",
            ink, bandNames[i], bands[i].left(), bands[i].top(), bands[i].width(), bands[i].height()
        );
    }
    std::printf("MEASURED clip ink_pixels_outside_plot=%d of %d\n", total, kViewW * kViewH - kPlot.width() * kPlot.height());
    return true;
}

// 8: the zero rule, as a pair: the same model with zeroY set and with zeroY == -1.
bool checkZeroRule(const Rendered& aWithRule, const Rendered& aWithoutRule, const gui::GraphPanelModel& aModel) {
    REQUIRE(aModel.zeroY >= 0, "precondition: the 'with rule' model must publish zeroY");
    REQUIRE(kPlot.contains(kPlot.left(), aModel.zeroY) && kPlot.contains(kPlot.right(), aModel.zeroY),
            "precondition: zeroY must be inside the plot's y range");

    // x positions clear of every other mark (the playhead is at x=200).
    const int columns[] = {100, 160, 300, 370};

    // The rule is a 1px line with AA off: where it lands within the ±2 band is the
    // rasterizer's business, so the band is sampled, not one row.
    int withInk = 0;
    for (int x : columns) {
        REQUIRE(kPlot.contains(x, aModel.zeroY), "precondition: the sampled column x=%d must be inside the plot", x);
        int inkedRow = -1;
        for (int y = aModel.zeroY - 2; y <= aModel.zeroY + 2 && inkedRow < 0; ++y) {
            if (clearDistance(pixelAt(aWithRule.image, x, y)) > kInkMin) {
                inkedRow = y;
            }
        }
        bool cleanWithout = true;
        for (int y = aModel.zeroY - 2; y <= aModel.zeroY + 2; ++y) {
            if (clearDistance(pixelAt(aWithoutRule.image, x, y)) > kInkMin) {
                cleanWithout = false;
            }
        }
        std::printf(
            "MEASURED zero_rule x=%d zeroY=%d inked_row_with_rule=%d clean_band_without_rule=%d band=%d..%d\n", x,
            aModel.zeroY, inkedRow, int(cleanWithout), aModel.zeroY - 2, aModel.zeroY + 2
        );
        REQUIRE(
            inkedRow >= 0, "the zero rule left no ink at x=%d in the rows %d..%d around zeroY", x, aModel.zeroY - 2,
            aModel.zeroY + 2
        );
        REQUIRE(
            cleanWithout,
            "the row band around zeroY carries ink at x=%d with zeroY == -1: something other than the zero rule"
            " paints there, and the pair cannot tell the rule apart",
            x
        );
        ++withInk;
    }
    REQUIRE(withInk == 4, "expected the zero rule at 4 columns, measured %d", withInk);

    // And the rule stops at the plot's own edges: it must not reach into the value
    // column, nor past the plot's right edge.
    const int outsideLeft = inkCount(aWithoutRule.image, QRect(kValueColumn.left(), aModel.zeroY - 2, kValueColumn.width(), 5));
    const int outsideLeftWith = inkCount(aWithRule.image, QRect(kValueColumn.left(), aModel.zeroY - 2, kValueColumn.width(), 5));
    const int outsideRightWith = inkCount(aWithRule.image, QRect(kPlot.right() + 1, aModel.zeroY - 2, kViewW - kPlot.right() - 1, 5));
    std::printf(
        "MEASURED zero_rule_edges value_column_ink=%d (without rule %d) right_of_plot_ink=%d\n", outsideLeftWith,
        outsideLeft, outsideRightWith
    );
    REQUIRE(
        outsideLeftWith == 0 && outsideLeft == 0 && outsideRightWith == 0,
        "the zero rule reached outside the plot: value column %d px, right of the plot %d px", outsideLeftWith,
        outsideRightWith
    );
    return true;
}

// 9: a legend row with NO ROOM (an empty swatch, GraphPanelModel's LEGEND note).
//
// The trap this pins is geometric, not cosmetic: QRect().right() is -1 and a null
// rect's centre is (0,0), so a label box derived from an empty swatch would land
// near (6, -height/2) - the panel's top-left corner - and be both a label sliver
// nobody drew and a hit box at a place no control is. The three statements below
// are the whole rule: the row has no label geometry (gui::legendLabelRect), nothing
// is painted for it (its pass draws strictly less, and the would-be sliver's region
// stays clean), and the row is not silently lost (the accessible summary names it -
// checked in the report, since this gate has no accessibility layer).
bool checkNoRoomRow(
    const Rendered& aLaidOut, const Rendered& aNoRoom, const gui::GraphPanelModel& aModel, ImFont* aFont,
    float aFontSize
) {
    REQUIRE(aModel.legend.size() == 2, "precondition: the no-room model must carry two legend rows");
    const gui::PanelLegendRow& laidOut = aModel.legend.at(0);
    const gui::PanelLegendRow& noRoom = aModel.legend.at(1);
    REQUIRE(!laidOut.swatch.isEmpty(), "precondition: the first row must have a swatch");
    REQUIRE(noRoom.swatch.isEmpty(), "precondition: the second row must have no swatch at all");

    // The control: a row WITH a swatch has a label box...
    const QRect laidOutBox = gui::legendLabelRect(laidOut, aFont, aFontSize);
    REQUIRE(!laidOutBox.isEmpty(), "control: a row with a swatch has no label box");

    // ...and a row WITHOUT one has none, which is what takes the phantom hit box
    // (press()/doubleClick() test exactly this rect) out of existence.
    const QRect noRoomBox = gui::legendLabelRect(noRoom, aFont, aFontSize);
    REQUIRE(noRoomBox.isEmpty(), "a row with no swatch still has a label box: %d,%d %dx%d",
            noRoomBox.left(), noRoomBox.top(), noRoomBox.width(), noRoomBox.height());

    // The region the naive derivation would have painted into: the block of the
    // panel left of the plot and above the first value label. Nothing the model
    // publishes is legitimately there - the value column's labels sit at y=60 and
    // y=240, the ticks start at the plot's edges, and every curve is clipped to the
    // plot - so ink here would be the sliver and nothing else.
    const QRect sliverBand(0, 0, kPlot.left(), 24);
    const int sliverInkWithRoom = inkCount(aLaidOut.image, sliverBand);
    const int sliverInkNoRoom = inkCount(aNoRoom.image, sliverBand);
    REQUIRE(
        sliverInkWithRoom == 0 && sliverInkNoRoom == 0,
        "the no-room row's derived label box painted at the panel's corner: %d px with the row laid out, %d without",
        sliverInkWithRoom, sliverInkNoRoom
    );

    // And the whole row contributes nothing: the two passes differ only in that
    // row's swatch, so the row with no room must draw strictly less (its swatch's
    // two rects and its label's glyph passes are gone).
    REQUIRE(
        aNoRoom.vtx < aLaidOut.vtx && aNoRoom.idx < aLaidOut.idx,
        "a row with no swatch still contributed geometry: %d/%d vertices/indices vs %d/%d",
        aNoRoom.vtx, aNoRoom.idx, aLaidOut.vtx, aLaidOut.idx
    );

    std::printf(
        "MEASURED no_room row=%s swatch_empty=1 label_box=%d,%d %dx%d control_box=%d,%d %dx%d"
        " sliver_band=(%d,0 %dx24) ink_with_room=%d ink_no_room=%d vtx_with_room=%d vtx_no_room=%d"
        " delta_vtx=%d\n",
        qPrintable(noRoom.label), noRoomBox.left(), noRoomBox.top(), noRoomBox.width(), noRoomBox.height(),
        laidOutBox.left(), laidOutBox.top(), laidOutBox.width(), laidOutBox.height(), sliverBand.left(),
        sliverBand.width(), sliverInkWithRoom, sliverInkNoRoom, aLaidOut.vtx, aNoRoom.vtx,
        aLaidOut.vtx - aNoRoom.vtx
    );
    return true;
}

// 10: the Custom easing's tangent handles. The panel publishes each handle as a
// stem (base -> dot) and a dot, and the drawing layer must put ink at exactly
// those coordinates - and highlight the dot the pointer is on, in the same token
// and at the same distance the editor's hit-test grabs through.
//
// The pair is the whole rule: with the pointer OFF (the default pass, io.MousePos
// off-screen) every dot is drawn in theme::Graph::handleFill() and the stem in
// theme::Graph::handleLine(); with the pointer ON one dot's centre (a second pass
// that differs ONLY in io.MousePos) that dot is theme::Graph::selected() while
// every other dot keeps its fill. Nothing else in the model moved between the two
// passes, so a change at a dot's centre is the highlight and nothing else.
bool checkHandles(
    const Rendered& aOff, const Rendered& aOn, const gui::GraphPanelModel& aModel, const theme::Graph& aTheme,
    const QPoint& aPointerDot
) {
    int handles = 0;
    int highlighted = 0;
    for (const gui::PanelCurve& curve : aModel.curves) {
        for (const gui::PanelHandle& handle : curve.handles) {
            ++handles;
            const QPoint dot(int(handle.dot.x()), int(handle.dot.y()));
            REQUIRE(kPlot.contains(dot), "precondition: handle dot (%d,%d) is outside the plot", dot.x(), dot.y());
            REQUIRE(handle.radius == ctrl::graph::kHandleRadius, "a handle dot carries radius %d, not kHandleRadius",
                    handle.radius);

            // The stem: a point midway between base and dot carries the stem's ink.
            // The midpoint is clear of both ends, so neither the key square nor the
            // dot itself can supply it.
            const QPoint mid(
                int(std::lround((handle.base.x() + handle.dot.x()) * 0.5)),
                int(std::lround((handle.base.y() + handle.dot.y()) * 0.5))
            );
            const QColor stem = pixelAt(aOff.image, mid.x(), mid.y());
            const QColor dotOff = pixelAt(aOff.image, dot.x(), dot.y());
            const QColor dotOn = pixelAt(aOn.image, dot.x(), dot.y());

            const int dotDistance = colourDistance(dotOff, aTheme.handleFill());
            const int highlightDistance = colourDistance(dotOn, aTheme.selected());
            const int moved = colourDistance(dotOff, dotOn);
            const bool pointed = dot == aPointerDot;

            std::printf(
                "MEASURED handle base=(%d,%d) dot=(%d,%d) radius=%d stem_mid=(%d,%d) stem_rgb=%s stem_clear=%d"
                " dot_rgb=%s handleFill=%s d=%d dot_on_rgb=%s selected=%s d=%d delta=%d pointed=%d\n",
                int(handle.base.x()), int(handle.base.y()), dot.x(), dot.y(), handle.radius, mid.x(), mid.y(),
                hex(stem).c_str(), clearDistance(stem), hex(dotOff).c_str(), hex(aTheme.handleFill()).c_str(),
                dotDistance, hex(dotOn).c_str(), hex(aTheme.selected()).c_str(), highlightDistance, moved,
                int(pointed)
            );

            REQUIRE(clearDistance(stem) > kInkMin, "the handle stem has no ink at its midpoint (%d,%d): rgb=%s",
                    mid.x(), mid.y(), hex(stem).c_str());
            REQUIRE(dotDistance <= kFillTol,
                    "the handle dot at (%d,%d) rgb=%s is not theme::Graph::handleFill() rgb=%s (distance %d > %d)",
                    dot.x(), dot.y(), hex(dotOff).c_str(), hex(aTheme.handleFill()).c_str(), dotDistance, kFillTol);

            if (pointed) {
                // The dot under the pointer is the highlighted one: it moved between
                // the passes and it is now theme::Graph::selected().
                REQUIRE(moved > 0, "the dot under the pointer (%d,%d) did not change between the two passes",
                        dot.x(), dot.y());
                REQUIRE(highlightDistance <= kFillTol,
                        "the dot under the pointer (%d,%d) rgb=%s is not theme::Graph::selected() rgb=%s"
                        " (distance %d > %d)",
                        dot.x(), dot.y(), hex(dotOn).c_str(), hex(aTheme.selected()).c_str(), highlightDistance,
                        kFillTol);
                ++highlighted;
            } else {
                // Every other dot is untouched: the two passes differ only in
                // io.MousePos, so a change here would be the highlight leaking.
                REQUIRE(moved == 0, "the dot at (%d,%d) is not under the pointer but changed (delta %d)",
                        dot.x(), dot.y(), moved);
            }
        }
    }
    REQUIRE(handles > 0, "the model published no tangent handles: the handle contract cannot be observed");
    REQUIRE(highlighted == 1, "expected exactly one highlighted handle dot, measured %d of %d", highlighted, handles);
    return true;
}

//-------------------------------------------------------------------------------------------------
// THE CONTRAST OUTLINE AND COMPOSITE PASS (gui::outline, src/gui/ImGuiOverlay.cpp).
//
// WHAT IS DRIVEN. The pass itself, with two textures this file authors by hand: the
// OVERLAY (the panel as the ImGui backend leaves it - rgb PREMULTIPLIED by the
// coverage in alpha, because the backend blends source-over into a framebuffer
// cleared to zero) and the BACKDROP (the pixels the panel would sit on). The
// backdrop goes through the pass's own copy - copyBackdrop, a glBlitFramebuffer -
// from a source framebuffer, so the copy is covered as well as the composite.
//
// Every assertion here is a regression gate on a MEASURED decision:
//   11 a low-contrast stroke over a BRIGHT patch gets a black ring, and the ring's
//      strength follows the stroke's COVERAGE: the same ink at 40% gets a 40% ring.
//      This is the whole point of the pass - and the coverage is the ring's alpha,
//      which is a property of the TAPS and not of the pixel.
//   12 the same stroke over a CONTRASTING patch gets NO ring: the pixels around it
//      are byte-identical to the backdrop, and the stroke's own interior is the
//      ink - i.e. the pass is invisible where it is not needed.
//   13 the inverse "always draws" assertion: with NO overlay coverage anywhere, the
//      WHOLE framebuffer comes back byte-identical to a patterned backdrop, through
//      the blit. A pass that painted anything unconditionally cannot pass this.
//   14 the composite is not gated at alpha >= 1.0: a 50%-covered overlay pixel
//      blends with the backdrop and lands between the two (the >= 1.0 failure mode
//      would make it either replace the backdrop or vanish).
//   15 the +/-2px second ring (outline::Params::ringRadiusPx), which is the
//      extension for a ring that reads too thin: with it on, a pixel two away from
//      the stroke is ringed where the shipped 1px ring leaves the backdrop alone.

// The authored geometry, in the pass's own texture space. The readback is aligned
// with it: uploadTexture flips for GL's bottom-up rows, and readFboTopDown
// un-flips the attachment again.
const QRect kBrightPatch(20, 30, 61, 131);     // x 20..80, y 30..160
const QRect kSolidStroke(40, 40, 10, 10);      // x 40..49, y 40..49
const QRect kPartialStroke(40, 140, 10, 10);   // the same ink at 40% coverage
const QRect kContrastStroke(40, 240, 10, 10);  // a contrasting ink over the dark area
const QRect kHalfPixel(240, 40, 10, 10);       // a 50%-covered overlay pixel

const QColor kBright(200, 200, 200);  // the patch the low-contrast inks sit on
const QColor kDark(60, 60, 60);       // the rest of the backdrop
const QColor kLowInk(180, 180, 180);  // ~1.24:1 against kBright: far below 3:1
const QColor kWhiteInk(255, 255, 255); // ~11:1 against kDark: far above 3:1

// The 40% stroke's alpha byte: 102/255 is exactly 0.4, which keeps every expected
// value below an exact number rather than a rounded one.
constexpr int kPartialAlpha = 102;
constexpr int kHalfAlpha = 128; // 128/255, the 50% pixel

QImage blankImage(const QColor& aFill) {
    QImage image(kViewW, kViewH, QImage::Format_RGBA8888);
    image.fill(aFill);
    return image;
}

// The overlay texel that an ImGui draw of aColour at aCoverage leaves behind: the
// backend's glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, ...)
// makes dst.rgb = src.rgb * src.a into a cleared target, so the rgb is
// PREMULTIPLIED. That is a property of the pass's INPUT, not an assumption of the
// pass: dividing it back out is what the shader has to do.
QColor overlayInk(const QColor& aColour, int aAlpha) {
    const auto premultiply = [aAlpha](int aChannel) {
        return int(std::lround(double(aChannel) * double(aAlpha) / 255.0));
    };
    return QColor(premultiply(aColour.red()), premultiply(aColour.green()), premultiply(aColour.blue()), aAlpha);
}

void fillRect(QImage& aImage, const QRect& aRect, const QColor& aColour) {
    for (int y = aRect.top(); y <= aRect.bottom(); ++y) {
        for (int x = aRect.left(); x <= aRect.right(); ++x)
            aImage.setPixelColor(x, y, aColour);
    }
}

// A backdrop that is DIFFERENT in every pixel, so "byte-identical" is a real
// statement about the whole pass and not about a flat fill.
QImage patternBackdrop() {
    QImage image(kViewW, kViewH, QImage::Format_RGBA8888);
    for (int y = 0; y < kViewH; ++y) {
        for (int x = 0; x < kViewW; ++x) {
            image.setPixelColor(
                x, y, QColor((x * 7 + y * 3) & 0xFF, (x * 5 + y * 11) & 0xFF, (x * 13 + y * 2) & 0xFF, 255)
            );
        }
    }
    return image;
}

// The authored images are TOP-DOWN, GL's texture rows are bottom-up, and the
// readback below is un-flipped by readFboTopDown: uploading the image FLIPPED is
// what makes pixelAt(readback, x, y) the authored (x, y) instead of its mirror.
//
// It re-uploads into the EXISTING texture object rather than calling
// gl::Texture::create() again: create() deletes the texture and generates a new
// one, and a framebuffer that was attached to the old name would then be reading a
// deleted texture (measured: the blit becomes a no-op and the backdrop comes back
// black).
void uploadTexture(gl::Texture& aTexture, const QImage& aTopDown) {
    const QImage bottomUp = aTopDown.flipped(Qt::Vertical);
    auto& funcs = gl::Global::functions();
    funcs.glBindTexture(GL_TEXTURE_2D, aTexture.id());
    funcs.glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, aTopDown.width(), aTopDown.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
        bottomUp.constBits()
    );
    funcs.glBindTexture(GL_TEXTURE_2D, 0);
}

// The harness's own GL-error gate: a blit or an upload that failed silently would
// otherwise be measured as "the pass produced black".
bool noGlError(const char* aWhere) {
    const GLuint error = gl::Global::functions().glGetError();
    if (error != GL_NO_ERROR) {
        std::printf("FAIL: GL error 0x%x %s\n", error, aWhere);
        return false;
    }
    return true;
}

// The pass's inputs and targets, at the size of this file's framebuffer. The
// backdrop is uploaded into a SOURCE framebuffer and then COPYBACKDROP-ed into the
// texture the pass reads, which is the shipping path (the widget's framebuffer
// becomes the backdrop) with a hand-authored pattern in place of the artwork.
struct OutlineHarness {
    gl::Texture overlayTexture;
    gl::Texture backdropSourceTexture;
    gl::Framebuffer backdropSourceFbo;
    gl::Texture backdropTexture;
    gl::Framebuffer backdropFbo;

    bool init() {
        const QSize size(kViewW, kViewH);
        overlayTexture.create(size);
        backdropSourceTexture.create(size);
        backdropTexture.create(size);
        // gl::Texture::create leaves the parameters at GL's defaults, and the
        // default MIN filter is a MIPMAP one: with no mipmaps the texture is
        // INCOMPLETE and samples as (0,0,0,1). The pass samples both 1:1 at the
        // texel centre, and CLAMP_TO_EDGE is what keeps a tap one pixel outside the
        // first column from wrapping to the last one.
        for (gl::Texture* texture : {&overlayTexture, &backdropSourceTexture, &backdropTexture}) {
            texture->setFilter(GL_NEAREST);
            texture->setWrap(GL_CLAMP_TO_EDGE);
        }
        backdropSourceFbo.setColorAttachment(0, backdropSourceTexture.id());
        backdropFbo.setColorAttachment(0, backdropTexture.id());
        return backdropSourceFbo.isComplete() && backdropFbo.isComplete();
    }

    // The uploads and the backdrop BLIT - everything before the pass itself, so a
    // caller that wants to arrange the state around the composite call alone can.
    bool prepare(const QImage& aBackdrop, const QImage& aOverlay) {
        uploadTexture(overlayTexture, aOverlay);
        uploadTexture(backdropSourceTexture, aBackdrop);
        if (!noGlError("uploading the pass's textures"))
            return false;
        gui::outline::copyBackdrop(backdropSourceFbo.id(), backdropFbo.id(), QSize(kViewW, kViewH));
        return noGlError("copyBackdrop");
    }

    // One whole composite: upload, blit, run the pass, read back. The framebuffer
    // is cleared to the clear colour first, so a pass that drew NOTHING leaves
    // magenta behind rather than passing quietly.
    QImage run(const QImage& aBackdrop, const QImage& aOverlay, const gui::outline::Params& aParams) {
        if (!prepare(aBackdrop, aOverlay))
            return QImage();

        const QSize size(kViewW, kViewH);
        auto& funcs = gl::Global::functions();
        gFbo->bind();
        funcs.glDisable(GL_SCISSOR_TEST);
        funcs.glScissor(0, 0, kViewW, kViewH);
        funcs.glViewport(0, 0, kViewW, kViewH);
        funcs.glClearColor(kClear.redF(), kClear.greenF(), kClear.blueF(), 1.0f);
        funcs.glClear(GL_COLOR_BUFFER_BIT);

        gui::outline::composite(overlayTexture.id(), backdropTexture.id(), size, aParams);
        if (!noGlError("the composite pass"))
            return QImage();

        const QImage image = readFboTopDown();
        gFbo->release();
        return image;
    }
};

// Whether every byte of two images agrees. The identity assertions are about the
// pass being a no-op, so they are byte comparisons and not tolerance tests.
bool imagesEqual(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* left = a.constScanLine(y);
        const uchar* right = b.constScanLine(y);
        if (std::memcmp(left, right, size_t(4 * a.width())) != 0)
            return false;
    }
    return true;
}

// The backdrop the ring assertions use: a bright patch in the top-left quadrant,
// dark everywhere else.
QImage ringBackdrop() {
    QImage image = blankImage(kDark);
    for (int y = kBrightPatch.top(); y <= kBrightPatch.bottom(); ++y) {
        for (int x = kBrightPatch.left(); x <= kBrightPatch.right(); ++x)
            image.setPixelColor(x, y, kBright);
    }
    return image;
}

// 11 + 12 + 14 + 15: ONE overlay, one backdrop, four statements about the ring and
// the composite. The strokes are 10x10 blocks with integer edges, so every sampled
// pixel is decided by an exact tap and not by anti-aliasing.
bool checkOutlineRing(OutlineHarness& aHarness) {
    QImage overlay = blankImage(QColor(0, 0, 0, 0));
    fillRect(overlay, kSolidStroke, overlayInk(kLowInk, 255));
    fillRect(overlay, kPartialStroke, overlayInk(kLowInk, kPartialAlpha));
    fillRect(overlay, kContrastStroke, overlayInk(kWhiteInk, 255));
    fillRect(overlay, kHalfPixel, overlayInk(kWhiteInk, kHalfAlpha));

    const QImage backdrop = ringBackdrop();
    gui::outline::Params params;
    params.colour = QColor(0, 0, 0);

    const QImage solid = aHarness.run(backdrop, overlay, params);

    // The pixel one column LEFT of each stroke: for the solid and the 40% stroke it
    // sits on the bright patch, and the only ink its taps can see is the stroke.
    const QColor ringSolid = pixelAt(solid, kSolidStroke.left() - 1, kSolidStroke.center().y());
    const QColor ringPartial = pixelAt(solid, kPartialStroke.left() - 1, kPartialStroke.center().y());
    const QColor fillSolid = pixelAt(solid, kSolidStroke.center().x(), kSolidStroke.center().y());
    const QColor fillPartial = pixelAt(solid, kPartialStroke.center().x(), kPartialStroke.center().y());
    const QColor half = pixelAt(solid, kHalfPixel.center().x(), kHalfPixel.center().y());
    const QColor besideHalf = pixelAt(solid, kHalfPixel.left() - 1, kHalfPixel.center().y());
    const QColor twoAwaySolid = pixelAt(solid, kSolidStroke.left() - 2, kSolidStroke.center().y());

    std::printf(
        "MEASURED outline_ring solid_ring=(%d,%d,%d,%d) partial_ring=(%d,%d,%d,%d) ring_distance=%d"
        " solid_fill=(%d,%d,%d,%d) partial_fill=(%d,%d,%d,%d) half=(%d,%d,%d,%d) beside_half=(%d,%d,%d,%d)"
        " two_away=(%d,%d,%d,%d)\n",
        ringSolid.red(), ringSolid.green(), ringSolid.blue(), ringSolid.alpha(), ringPartial.red(),
        ringPartial.green(), ringPartial.blue(), ringPartial.alpha(),
        colourDistance(ringSolid, ringPartial), fillSolid.red(), fillSolid.green(), fillSolid.blue(),
        fillSolid.alpha(), fillPartial.red(), fillPartial.green(), fillPartial.blue(), fillPartial.alpha(),
        half.red(), half.green(), half.blue(), half.alpha(), besideHalf.red(), besideHalf.green(),
        besideHalf.blue(), besideHalf.alpha(), twoAwaySolid.red(), twoAwaySolid.green(), twoAwaySolid.blue(),
        twoAwaySolid.alpha()
    );

    // 11: the solid stroke's ring is BLACK - the outline colour at full strength.
    REQUIRE(
        colourDistance(ringSolid, QColor(0, 0, 0)) <= 2,
        "the ring beside a solid low-contrast stroke is (%d,%d,%d), not the outline colour",
        ringSolid.red(), ringSolid.green(), ringSolid.blue()
    );
    // ... and the 40% stroke's ring is the SAME ink at 40%, i.e. 60% of the bright
    // patch. The exact value is the pass's arithmetic, so it is asserted exactly
    // (mix(200, 0, 102/255) = 120).
    REQUIRE(
        std::abs(ringPartial.red() - 120) <= 3 && ringPartial.red() == ringPartial.green() &&
            ringPartial.green() == ringPartial.blue(),
        "the ring beside a 40%% stroke is (%d,%d,%d), not 40%% of the outline over the patch (120)",
        ringPartial.red(), ringPartial.green(), ringPartial.blue()
    );
    // The two rings differ: the strength really does follow the coverage.
    REQUIRE(
        colourDistance(ringSolid, ringPartial) > 60,
        "a solid and a 40%% stroke produced rings %d apart: the ring does not follow the coverage",
        colourDistance(ringSolid, ringPartial)
    );
    // The stroke itself is the composite of the ink over the patch, i.e. the INK -
    // never the ring colour, and never the patch.
    REQUIRE(
        colourDistance(fillSolid, kLowInk) <= kFillTol,
        "the solid stroke's own pixel is (%d,%d,%d), not its ink (%d,%d,%d)", fillSolid.red(),
        fillSolid.green(), fillSolid.blue(), kLowInk.red(), kLowInk.green(), kLowInk.blue()
    );
    // The 40%-covered pixel ALSO has low-contrast ink among its taps, so the ring is
    // painted UNDER its own ink and shows through the coverage: 0.6 * ring(120) +
    // 0.4 * ink(180) = 144, not the plain composite (192). That order is the pass's,
    // and it is pinned here because it is the only place a translucency is darkened;
    // over the panel's own opaque ink the ring is simply hidden by it. It is also a
    // second, independent pin on the premultiplied overlay being divided back out:
    // compositing it premultiplied again would give 101.
    REQUIRE(
        std::abs(fillPartial.red() - 144) <= 3 && fillPartial.red() == fillPartial.green() &&
            fillPartial.green() == fillPartial.blue(),
        "a 40%%-covered low-contrast pixel came out (%d,%d,%d); ink at 40%% over its own 40%% ring is 144",
        fillPartial.red(), fillPartial.green(), fillPartial.blue()
    );
    // 14: the 50%-covered pixel blends. bg = 60, ink = 255, so the expected value is
    // 0.5*60 + 0.5*255 = 157.5 -> 157 or 158. Both the "replaced" (255) and the
    // "vanished" (60) failure modes are far outside the tolerance.
    REQUIRE(
        std::abs(half.red() - 158) <= 3,
        "a 50%%-covered overlay pixel came out (%d,%d,%d); the blend of %d and 255 is 158",
        half.red(), half.green(), half.blue(), kDark.red()
    );
    REQUIRE(half.red() == half.green() && half.green() == half.blue(), "the 50%% pixel is not grey");
    // 12 (the composite half): that pixel's surroundings are untouched, because a
    // white pixel over a dark backdrop has all the contrast it needs.
    REQUIRE(
        colourDistance(besideHalf, kDark) == 0,
        "the pixel beside the high-contrast stroke is (%d,%d,%d), not the untouched backdrop",
        besideHalf.red(), besideHalf.green(), besideHalf.blue()
    );
    // The shipped ring is exactly ONE pixel wide: two columns out is still backdrop.
    REQUIRE(
        colourDistance(twoAwaySolid, kBright) == 0,
        "the shipped 1px ring reached two pixels from the stroke: (%d,%d,%d)", twoAwaySolid.red(),
        twoAwaySolid.green(), twoAwaySolid.blue()
    );

    // 15: the +/-2px second ring, the same model and only the radius differs.
    gui::outline::Params wide = params;
    wide.ringRadiusPx = 2.0f;
    const QImage wideRing = aHarness.run(backdrop, overlay, wide);
    const QColor wideTwoAway = pixelAt(wideRing, kSolidStroke.left() - 2, kSolidStroke.center().y());
    const QColor narrowTwoAway = pixelAt(solid, kSolidStroke.left() - 2, kSolidStroke.center().y());
    std::printf(
        "MEASURED outline_ring_radius=2 two_away=(%d,%d,%d) one_away=(%d,%d,%d) narrow_two_away=(%d,%d,%d)\n",
        wideTwoAway.red(), wideTwoAway.green(), wideTwoAway.blue(),
        pixelAt(wideRing, kSolidStroke.left() - 1, kSolidStroke.center().y()).red(),
        pixelAt(wideRing, kSolidStroke.left() - 1, kSolidStroke.center().y()).green(),
        pixelAt(wideRing, kSolidStroke.left() - 1, kSolidStroke.center().y()).blue(), narrowTwoAway.red(),
        narrowTwoAway.green(), narrowTwoAway.blue()
    );
    REQUIRE(
        colourDistance(wideTwoAway, QColor(0, 0, 0)) <= 2,
        "with ringRadiusPx = 2 the pixel two away from the stroke is (%d,%d,%d), not ringed",
        wideTwoAway.red(), wideTwoAway.green(), wideTwoAway.blue()
    );
    REQUIRE(
        colourDistance(narrowTwoAway, kBright) == 0,
        "precondition: the 1px ring must leave the pixel two away alone, it is (%d,%d,%d)", narrowTwoAway.red(),
        narrowTwoAway.green(), narrowTwoAway.blue()
    );
    return true;
}

// 12: the same stroke over a CONTRASTING patch gets NO ring, and the backdrop
// around it survives byte for byte in every direction.
bool checkOutlineNoRing(OutlineHarness& aHarness) {
    QImage overlay = blankImage(QColor(0, 0, 0, 0));
    fillRect(overlay, kContrastStroke, overlayInk(kWhiteInk, 255));

    const QImage backdrop = ringBackdrop();
    gui::outline::Params params;
    params.colour = QColor(0, 0, 0);
    const QImage rendered = aHarness.run(backdrop, overlay, params);

    int checked = 0;
    int changed = 0;
    for (int y = kContrastStroke.top() - 2; y <= kContrastStroke.bottom() + 2; ++y) {
        for (int x = kContrastStroke.left() - 2; x <= kContrastStroke.right() + 2; ++x) {
            if (kContrastStroke.contains(x, y))
                continue;
            ++checked;
            if (colourDistance(pixelAt(rendered, x, y), pixelAt(backdrop, x, y)) != 0)
                ++changed;
        }
    }
    const QColor inside = pixelAt(rendered, kContrastStroke.center().x(), kContrastStroke.center().y());
    std::printf(
        "MEASURED outline_no_ring patch=%d,%d,%d stroke_ink=(%d,%d,%d) ring_pixels_checked=%d changed=%d\n",
        kDark.red(), kDark.green(), kDark.blue(), inside.red(), inside.green(), inside.blue(), checked, changed
    );
    REQUIRE(checked > 0, "the no-ring probe checked no pixels at all");
    REQUIRE(
        changed == 0, "%d of %d pixels around a high-contrast stroke changed: the ring is unconditional", changed,
        checked
    );
    REQUIRE(
        colourDistance(inside, kWhiteInk) <= kFillTol, "the high-contrast stroke's own pixel is (%d,%d,%d), not white",
        inside.red(), inside.green(), inside.blue()
    );
    return true;
}

// 13: the inverse assertion. With NO overlay coverage anywhere, the whole
// framebuffer must come back byte-identical to a patterned backdrop - through the
// blit AND through the pass. This is what catches an "always draws" regression:
// a backdrop that is one bit off, one ringed pixel, one blend applied to the panel
// area, and it fails.
bool checkOutlineBackdropIdentical(OutlineHarness& aHarness) {
    const QImage overlay = blankImage(QColor(0, 0, 0, 0));
    const QImage backdrop = patternBackdrop();
    gui::outline::Params params;
    params.colour = QColor(0, 0, 0);

    const QImage rendered = aHarness.run(backdrop, overlay, params);

    int changed = 0;
    int worst = 0;
    QRect worstAt;
    for (int y = 0; y < kViewH; ++y) {
        for (int x = 0; x < kViewW; ++x) {
            const int distance = colourDistance(pixelAt(rendered, x, y), pixelAt(backdrop, x, y));
            if (distance != 0) {
                ++changed;
                if (distance > worst) {
                    worst = distance;
                    worstAt = QRect(x, y, 1, 1);
                }
            }
        }
    }
    std::printf(
        "MEASURED outline_backdrop_identical pixels=%d changed=%d worst_distance=%d worst_at=(%d,%d)"
        " byte_equal=%d\n",
        kViewW * kViewH, changed, worst, worstAt.left(), worstAt.top(), int(imagesEqual(rendered, backdrop))
    );
    REQUIRE(
        changed == 0,
        "%d of %d pixels changed with an empty overlay (worst %d at (%d,%d)): the pass is not a no-op",
        changed, kViewW * kViewH, worst, worstAt.left(), worstAt.top()
    );
    return true;
}

// 16: the pass is a GUEST in the frame - the bindings it finds are the bindings it
// leaves. THIS IS NOT A COURTESY: the application's paint path keeps ONE default
// vertex array bound for the whole session (MainDisplayWidget::initializeGL creates
// it and comments "keep binding"), and the painter engine that runs at the TOP of
// the next paint relies on it - every glVertexAttribPointer it makes needs an array
// object bound in a core profile. A version of this pass that ended with
// glBindVertexArray(0) made the application's next paint fail with
// GL_INVALID_OPERATION in glVertexAttribPointer and glDrawArrays (measured on a real
// run; the error surfaced one call later as an assert in
// gl::EasyShaderProgram::release()). The unit itself is also asserted to be quiet:
// a pass that leaves a GL error behind fails this file's own noGlError checks.
bool checkOutlineRestoresBindings(OutlineHarness& aHarness) {
    auto& funcs = gl::Global::functions();

    const QImage overlay = blankImage(QColor(0, 0, 0, 0));
    const QImage backdrop = patternBackdrop();
    REQUIRE(aHarness.prepare(backdrop, overlay), "precondition: the harness could not prepare its textures");

    // A marker binding in every slot the pass touches: its own vertex array, its own
    // texture on each unit, and a NON-ZERO active unit (the pass restores the active
    // unit last, so leaving it elsewhere would be invisible otherwise).
    gl::VertexArrayObject markerVao;
    gl::Texture markerTexture0;
    gl::Texture markerTexture1;
    markerTexture0.create(QSize(4, 4));
    markerTexture1.create(QSize(4, 4));
    markerVao.bind();
    funcs.glActiveTexture(GL_TEXTURE0);
    funcs.glBindTexture(GL_TEXTURE_2D, markerTexture0.id());
    funcs.glActiveTexture(GL_TEXTURE1);
    funcs.glBindTexture(GL_TEXTURE_2D, markerTexture1.id());

    gFbo->bind();
    funcs.glDisable(GL_SCISSOR_TEST);
    funcs.glScissor(0, 0, kViewW, kViewH);
    funcs.glViewport(0, 0, kViewW, kViewH);
    funcs.glClearColor(kClear.redF(), kClear.greenF(), kClear.blueF(), 1.0f);
    funcs.glClear(GL_COLOR_BUFFER_BIT);

    gui::outline::Params params;
    params.colour = QColor(0, 0, 0);
    gui::outline::composite(aHarness.overlayTexture.id(), aHarness.backdropTexture.id(), QSize(kViewW, kViewH), params);

    GLint vao = 0;
    GLint activeTexture = 0;
    GLint texture0 = 0;
    GLint texture1 = 0;
    GLint program = -1;
    GLint drawFbo = 0;
    funcs.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    funcs.glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    funcs.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture1);
    funcs.glActiveTexture(GL_TEXTURE0);
    funcs.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    funcs.glActiveTexture(GLenum(activeTexture));
    funcs.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    funcs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);

    std::printf(
        "MEASURED outline_state_restored vao=%d expected_vao=%u active_texture=0x%x expected=0x%x"
        " texture0=%d expected0=%u texture1=%d expected1=%u program=%d draw_fbo=%u expected_fbo=%u\n",
        vao, markerVao.id(), activeTexture, GLuint(GL_TEXTURE1), texture0, markerTexture0.id(), texture1,
        markerTexture1.id(), program, drawFbo, gFbo->id()
    );

    REQUIRE(
        vao == GLint(markerVao.id()),
        "the pass left vertex array %d bound, not the %u it found: the application's default VAO would be gone",
        vao, markerVao.id()
    );
    REQUIRE(
        activeTexture == GLint(GL_TEXTURE1), "the pass left texture unit 0x%x active, not the 0x%x it found",
        activeTexture, GLuint(GL_TEXTURE1)
    );
    REQUIRE(
        texture0 == GLint(markerTexture0.id()) && texture1 == GLint(markerTexture1.id()),
        "the pass left textures %d/%d bound on units 0/1, not the %u/%u it found", texture0, texture1,
        markerTexture0.id(), markerTexture1.id()
    );
    // The framebuffer is the one binding the pass does not put back, and it must not
    // need to: it is a full-screen pass and it writes into whatever is bound.
    REQUIRE(
        GLuint(drawFbo) == gFbo->id(), "the pass ended with FBO %u bound, not the %u it was asked to draw into",
        drawFbo, gFbo->id()
    );
    // The program is DELIBERATELY left unbound, exactly as the application's own
    // gl::EasyTextureDrawer leaves it at the end of every paint.
    REQUIRE(program == 0, "the pass left program %d bound; the application's drawers leave 0", program);
    REQUIRE(noGlError("the pass's own state"), "the pass left a GL error behind");
    return true;
}

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // A live core-profile context, exactly as tests/overlay_engine_primitives.cpp
    // and roundtrip_main.cpp take one. The format has to be the default before the
    // widget creates its context, which happens on the first show()/makeCurrent().
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);

    QOpenGLWidget glWidget;
    glWidget.resize(kViewW, kViewH);
    glWidget.show();
    glWidget.makeCurrent();
    if (!glWidget.context()) {
        std::fprintf(stderr, "FAIL: no GL context could be created\n");
        return 2;
    }
    auto funcs = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(glWidget.context());
    if (!funcs || !funcs->initializeOpenGLFunctions()) {
        std::fprintf(stderr, "FAIL: no GL function table for the widget's context\n");
        return 2;
    }
    gl::Global::setContext(glWidget);
    gl::Global::setFunctions(*funcs);

    gl::DeviceInfo device;
    device.vender = "harness";
    device.renderer = "harness";
    device.version = "4.0";
    device.maxTextureSize = 16384;
    device.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&device);

    // Every rect in this file is in the panel's own coordinates, and ImGui's
    // projection is built from io.DisplaySize: a window manager that resized this
    // widget would move every sampled pixel, so the size is asserted.
    if (glWidget.width() != kViewW || glWidget.height() != kViewH) {
        std::fprintf(
            stderr, "FAIL: the widget is %dx%d, not %dx%d\n", glWidget.width(), glWidget.height(), kViewW, kViewH
        );
        return 2;
    }

    std::printf(
        "INFO GL renderer=%s version=%s\n",
        reinterpret_cast<const char*>(funcs->glGetString(GL_RENDERER)),
        reinterpret_cast<const char*>(funcs->glGetString(GL_VERSION))
    );

    // A pending GL error would be turned into an abort by GL_CHECK_ERROR on the
    // first call below and read as that call's fault. Drop what the widget's setup
    // left behind.
    {
        GLuint glError = 0;
        while ((glError = funcs->glGetError()) != GL_NO_ERROR) {
        }
    }

    // A core-profile draw with no VAO bound is GL_INVALID_OPERATION.
    gl::VertexArrayObject vao;
    vao.bind();

    // This harness's OWN framebuffer, never the widget's (see the file header).
    gl::Texture colorTexture;
    colorTexture.create(QSize(kViewW, kViewH));
    gl::Framebuffer fbo;
    fbo.setColorAttachment(0, colorTexture.id());
    if (!fbo.isComplete()) {
        std::fprintf(stderr, "FAIL: the harness framebuffer is incomplete\n");
        return 2;
    }
    gFbo = &fbo;

    // The ImGui context. NO renderer backend other than the GL3 one compiled
    // through src/gui/ImGuiGLBackend.cpp: that TU carries the loader shim over
    // gl::Global::functions(), which is why the app's function table has to be
    // installed before ImGui_ImplOpenGL3_Init().
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // never write imgui.ini from a test
    io.LogFilename = nullptr;
    if (!io.Fonts->AddFontDefaultBitmap()) {
        std::fprintf(stderr, "FAIL: ImGui's default bitmap font could not be added\n");
        return 2;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 400 core")) {
        std::fprintf(stderr, "FAIL: the GL3 backend refused to initialize\n");
        return 2;
    }

    const theme::Graph palette = theme::Graph::from(theme::Colors::current());
    std::printf(
        "INFO palette halo=%s axisText=%s gridMajor=%s axisLine=%s selected=%s keyFill=%s keyEdge=%s moveKey=%s"
        " scaleKey=%s clear=%s\n",
        hex(palette.halo()).c_str(), hex(palette.axisText()).c_str(), hex(palette.gridMajor()).c_str(),
        hex(palette.axisLine()).c_str(), hex(palette.selected()).c_str(), hex(palette.keyFill()).c_str(),
        hex(palette.keyEdge()).c_str(), hex(palette.keyColor(kTypeA)).c_str(),
        hex(palette.keyColor(kTypeB)).c_str(), hex(kClear).c_str()
    );

    // The clear colour has to be far from everything the model can paint, or "this
    // pixel moved" would not mean "the panel painted here".
    {
        const QColor painted[] = {
            palette.halo(), palette.axisText(), palette.gridMajor(), palette.axisLine(), palette.selected(),
            palette.keyFill(), palette.keyEdge(), palette.keyColor(kTypeA), palette.keyColor(kTypeB),
        };
        for (const QColor& color : painted) {
            if (clearDistance(color) <= 60) {
                std::printf(
                    "FAIL: precondition: the clear colour rgb=%s is only %d from the panel colour rgb=%s\n",
                    hex(kClear).c_str(), clearDistance(color), hex(color).c_str()
                );
                return 1;
            }
        }
    }

    // The contrast outline pass's own gate (assertions 11-15): the pass has to
    // build on this context before any of it can be measured.
    OutlineHarness outlineHarness;
    if (!outlineHarness.init()) {
        std::printf("FAIL: the outline harness's framebuffers are incomplete\n");
        return 2;
    }
    if (!gui::outline::init()) {
        std::printf("FAIL: the contrast outline pass refused to build on this context\n");
        return 2;
    }
    std::printf("INFO outline minContrast=%.1f ringPx=%.1f outlineColour=%s\n", gui::outline::kMinContrast,
                gui::outline::kRingRadiusPx, hex(palette.outline()).c_str());

    const gui::GraphPanelModel full = makeFullModel(palette);
    const gui::GraphPanelModel clip = makeClipModel(palette);
    const gui::GraphPanelModel empty = makeEmptyModel();

    // An INVALID focus: every curve is drawn at full strength, so no assertion here
    // depends on the emphasis path (see the file header: it is deliberately not
    // covered).
    gui::PanelFocus focus;
    focus.valid = false;

    // The same model with the zero rule switched OFF: the ONLY difference is zeroY,
    // which is what makes assertion 8 a pair.
    gui::GraphPanelModel fullNoRule = full;
    fullNoRule.zeroY = -1;

    // The same model with the second legend row given NO ROOM: the ONLY difference
    // is that row's swatch, which is what makes assertion 9 a pair.
    gui::GraphPanelModel legendNoRoom = full;
    legendNoRoom.legend[1].swatch = QRect();

    // 0: the control, then one pass per model the assertions read.
    const Rendered control = renderPass(gui::GraphPanelModel(), palette, focus, "control");
    const Rendered rendered = renderPass(full, palette, focus, "full_model");
    const Rendered emptyRendered = renderPass(empty, palette, focus, "empty_state");
    const Rendered clipRendered = renderPass(clip, palette, focus, "clip");
    const Rendered noRuleRendered = renderPass(fullNoRule, palette, focus, "full_model_zeroY_off");
    const Rendered noRoomRendered = renderPass(legendNoRoom, palette, focus, "legend_no_room");

    // The handle highlight pass: the SAME model with the pointer on the first
    // handle's dot, so the ONLY difference from `rendered` is io.MousePos - which
    // is what makes "the dot changed" a statement about the highlight alone.
    if (full.curves.isEmpty() || full.curves.at(0).handles.isEmpty()) {
        std::printf("FAIL: the full model publishes no tangent handle to point at\n");
        return 1;
    }
    const QPoint pointerDot(
        int(full.curves.at(0).handles.at(0).dot.x()), int(full.curves.at(0).handles.at(0).dot.y())
    );
    const Rendered highlightRendered = renderPass(
        full, palette, focus, "full_model_pointer_on_handle",
        ImVec2(float(pointerDot.x()), float(pointerDot.y()))
    );

    if (rendered.vtx <= 0) {
        std::printf("FAIL: the full model emitted no vertices at all\n");
        return 1;
    }

    if (!checkControl(control)) {
        std::printf("FAILED: control\n");
        return 1;
    }
    if (!checkNoGrid(rendered, full)) {
        std::printf("FAILED: no grid / no background\n");
        return 1;
    }
    if (!checkCurveInk(rendered, full)) {
        std::printf("FAILED: curve stroke\n");
        return 1;
    }
    if (!checkKeys(rendered, full, palette)) {
        std::printf("FAILED: key squares\n");
        return 1;
    }
    if (!checkLabelHalo(rendered, full, palette)) {
        std::printf("FAILED: label halo\n");
        return 1;
    }
    if (!checkLegend(rendered, full, palette)) {
        std::printf("FAILED: legend swatches\n");
        return 1;
    }
    if (!checkEmptyState(emptyRendered, empty)) {
        std::printf("FAILED: empty state\n");
        return 1;
    }
    if (!checkClip(clipRendered, clip)) {
        std::printf("FAILED: plot clip\n");
        return 1;
    }
    if (!checkZeroRule(rendered, noRuleRendered, full)) {
        std::printf("FAILED: zero rule\n");
        return 1;
    }
    if (!checkNoRoomRow(rendered, noRoomRendered, legendNoRoom, rendered.font, rendered.fontSize)) {
        std::printf("FAILED: legend row with no room\n");
        return 1;
    }
    if (!checkHandles(rendered, highlightRendered, full, palette, pointerDot)) {
        std::printf("FAILED: tangent handles\n");
        return 1;
    }
    if (!checkOutlineRing(outlineHarness)) {
        std::printf("FAILED: contrast outline ring\n");
        return 1;
    }
    if (!checkOutlineNoRing(outlineHarness)) {
        std::printf("FAILED: contrast outline draws where it is not needed\n");
        return 1;
    }
    if (!checkOutlineBackdropIdentical(outlineHarness)) {
        std::printf("FAILED: the composite pass is not a no-op without the panel\n");
        return 1;
    }
    if (!checkOutlineRestoresBindings(outlineHarness)) {
        std::printf("FAILED: the pass does not leave the GL bindings it found\n");
        return 1;
    }

    // The pass's GL objects go while this context is current, exactly as the
    // overlay's shutdown() does it.
    gui::outline::shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    std::printf("OK: the graph panel's pixel contract holds (16 assertions, 7 panel passes + 5 outline passes)\n");
    return 0;
}
