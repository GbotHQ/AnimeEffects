// Contract test for the engine-neutral graph panel model: gui::buildGraphPanelModel
// (src/gui/GraphPanelModel.h/.cpp).
//
// The model is what the overlay (Dear ImGui now, the QPainter renderer until
// then) draws the graph panel from, so what is pinned below is the geometry a
// drawing layer would consume: the panel/plot/value-column rectangles, the two
// SHORT tick axes with their label rects, the curves with their keys, the channel
// list with its rows (grouped per layer and key type, each row carrying its
// channel hue from the one table both views read), the panel's one control (the
// channel button), the selection
// hull with its count, the playhead, and the empty state. Every assertion prints
// the value it measured.
//
// The oracle is the editor's own scaler (GraphEditor::scale(), geometrised with
// the same panelRect the model uses) and the editor's own curve set
// (GraphEditor::curves()) - never a re-statement of the model's arithmetic, which
// would agree with any bug that is in both.
//
// Coverage, in the order of the acceptance list:
//   1 consistency with the editor: panel == GraphEditor::panelRect(viewport),
//     plot inside the panel, value column left of and clear of the plot (at
//     1280x720 and at 320x200)
//   2 no grid: the tick axes are POSITION lists (static_assert on their types) and
//     every frame tick's TIER is a CLASSIFICATION of the same length and order
//     (static_assert too: an int per tick, no appearance),
//     no published rect spans the plot, every published frame tick is a frame the
//     ladder classifies and every frame the ladder classifies inside the plot has
//     its tick, the value ticks are exactly the published value labels' own
//     pixels, and every frame label box lies inside the RULER BAND above the plot
//     (the band is a section outside the plot, and no label reaches into the plot)
//   3 the ladder follows the scaler: a label's number is scale.frameAt(tick),
//     neighbouring label boxes keep a minimum gap, and every published tier is one
//     of the three the ruler draws and is the ladder's own level for that tick's
//     frame (index-aligned with the positions)
//   4 curves: exactly one per shown (target, channel) - the same set and the
//     same order as the editor's own curves() and as the channel list's rows -
//     each with >1 point, key rects clipped into the plot and equal to
//     graph::keyRect, the polyline's y at every key frame equal to
//     scale.pixelY(valueOf(key)), and a NAME CHIP whose box is derived from the
//     curve it names (its centre on the value the curve leaves the plot with) and
//     lies inside the plot
//   5 hidden channels: the row survives with visible == false and no curve
//   6 empty state: no targets -> a message and no curves
//   7 playhead: scale.pixelX(editor.currentFrame()) after moving the document's
//     frame, and -1 with no project
//   8 NO LEGEND, and the panel's one control: nothing publishes a swatch stack
//     (the legend is gone, not orphaned), the channel list still carries every
//     row whatever the viewport, a chip that cannot fit the plot publishes NO box
//     (an empty rect, never one in the margin), and a plot too small for the
//     "N channels" button publishes no button rect - a control that cannot be
//     drawn must not be hit
//   9 the polyline is published unclipped and unreduced - point for point the
//     sampled curve mapped through the scaler, out-of-plot samples included, so
//     the drawing layer's own clip is the only clip (the header's CURVES)
//  10 tangent handles: one PanelHandle pair per segment that HAS one - a Custom
//     segment's STORED pair (ghost == false) matching the editor's own grab
//     mapping, ctrl::graph::handlePoint, and an Auto segment's DERIVED pair
//     (ghost == true) whose x IS the third and two thirds of the segment's span
//     and whose dot is the same mapping (ctrl::graph::handlePointAt) - and none on
//     a segment whose easing is neither
//  11 the ruler band as a section ABOVE the plot (its own reserved rows, the
//     numbers inside it, the playhead crossing from its pill column into the
//     plot), the occluded top that can only hide it, and the live marquee
//  12 the label column is a STABLE, FORMAT-DERIVED reserve: its width is the
//     widest label the value format can write (plus the box inset and text gap,
//     clamped into the scaler's floor and ceiling), so it is the SAME width for
//     every value window at several viewports - the plot rect stays
//     byte-identical across six windows, across a value-axis wheel step and a
//     value pan, and a 40px FRAME pan moves a frame's pixel by exactly 40 - while
//     every published label's box is still at least as wide as its own measured
//     text (checked with a wide value, 12345.6 -> "1.235e+04", a small negative
//     and the float extremes)
//  13 the key DOT is the marker's own geometry: circles of radius
//     ctrl::graph::kKeyDotRadius (== the TIMELINE's ctrl::keyRadius) centred on
//     each key, published beside the key rects, with the hit radius a separate,
//     larger constant
//  14 the SELECTION HULL is the editor's own outline in the model's own pixels -
//     EMPTY with nothing selected (and then no count label either) and
//     NON-EMPTY with a key selected, each point the same (frame, value) the editor
//     stated mapped through the same scaler the curves use - with the count the
//     editor reports and the label that writes it
//  15 the marker state: which marker is selected, and whether the key it belongs
//     to has a selected sibling - both raw answers from the editor's own queries
//  16 the ACTIVE-range recession (one inside/outside flag per tick and per frame
//     label, against the project range and against an active play window) and the
//     play window's overlay geometry (band, both stems, both arms, and the one
//     centred merged pill of a window narrower than its labels)
// plus a read-only probe: building a model must not move the editor's scaler,
// its targets, the project's dirty flag or the document's keys.
//
// Headless: QApplication (translations + the font database the label metrics
// need), no widgets, no GL, no .anie fixture.
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include "XCAssert.h"
#include "cmnd/Stack.h"
#include "core/Animator.h"
#include "core/Frame.h"
#include "core/MoveKey.h"
#include "core/ObjectNode.h"
#include "core/OpaKey.h"
#include "core/Project.h"
#include "core/RotateKey.h"
#include "core/TimeKeyType.h"
#include "core/TimeFormat.h"
#include "core/TimeLine.h"
#include "ctrl/GraphEditor.h"
#include "ctrl/UILogger.h"
#include "ctrl/graph/graph_Channel.h"
#include "ctrl/graph/graph_Curve.h"
#include "ctrl/graph/graph_Geometry.h"
#include "ctrl/graph/graph_Scaler.h"
// The timeline's own key marker (ctrl::keyRadius = 3.0 * ctrl::radiiFactor), the
// constant the panel's key dot is derived from - block [13] pins the two
// together instead of trusting a comment. Header-only: the constants are
// constexpr, so nothing from the timeline's renderer is linked.
#include "ctrl/time/time_Renderer.h"
#include "gui/GraphPanelModel.h"
#include "gui/theme/Colors.h"
#include "gui/theme/Graph.h"
#include "util/Range.h"

// core::Project.cpp references the GUI AudioPlaybackWidget::deserialize only to
// load a .aemus sidecar (this test never has one), and the object-tree symbols
// come in with ctrl's widgets. Stub them, exactly as graph_curve / graph_drag /
// creation_select_keys do, so the link succeeds without the whole gui library.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

#include "gui/ObjectTreeWidget.h"
bool gui::obj::Item::isTopNode() const { return false; }
std::pair<core::TimeKey*, core::LayerMesh*> gui::ObjectTreeWidget::getAreaMeshImpl(core::ObjectNode&, int) {
    return { nullptr, nullptr };
}

namespace {

class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;

int gFailures = 0;

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

// The assertions count and continue: a run has to print every measurement it
// took, not stop at the first one that failed.
#define CHECK(cond)                                                                                    \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::printf("  FAIL %d: %s\n", __LINE__, #cond);                                           \
            ++gFailures;                                                                               \
        }                                                                                              \
    } while (false)

namespace core {
// Minimal ObjectNode subclass owning a real TimeLine (ObjectNode::timeLine() is
// virtual and null by default), with a name so the channel list's rows (and the
// chips that name them in place) are distinct.
class TestNode: public ObjectNode {
    TimeLine mTimeLine;
    QString mName;

public:
    ObjectType type() const override { return ObjectType_Layer; }
    void setName(const QString& aName) override { mName = aName; }
    const QString& name() const override { return mName; }
    float initialDepth() const override { return 0; }
    void setVisibility(bool) override {}
    bool isVisible() const override { return true; }
    void setSlimDown(bool) override {}
    bool isSlimmedDown() const override { return false; }
    bool canHoldChild() const override { return false; }
    void setInitialRect(const QRect&) override {}
    QRect initialRect() const override { return QRect(); }
    bool serialize(Serializer&) const override { return true; }
    bool deserialize(Deserializer&) override { return true; }
    TimeLine* timeLine() override { return &mTimeLine; }
    const TimeLine* timeLine() const override { return &mTimeLine; }
};
} // namespace core

namespace {

// The document's clock, with a frame the test moves (core::Animator is an
// interface; the application's implementation is in the gui layer).
class TestAnimator: public core::Animator {
    core::Frame mFrame = core::Frame(1);

public:
    void setFrame(core::Frame aFrame) { mFrame = aFrame; }
    core::Frame currentFrame() const override { return mFrame; }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

class NullLogger: public ctrl::UILogger {
public:
    void pushLog(const QString&, ctrl::UILogType) override {}
};

using Pair = QPair<core::ObjectNode*, ctrl::graph::Channel>;

// Qt 6 containers report qsizetype; printf's %d (and the arithmetic below) want
// an int, and a mismatch there is undefined behaviour, not a formatting quirk.
template <class T>
int isz(const T& aContainer) {
    return int(aContainer.size());
}

bool samePair(const Pair& aLhs, const core::ObjectNode* aNode, const ctrl::graph::Channel& aChannel) {
    return aLhs.first == aNode && aLhs.second == aChannel;
}

// The (node, channel) pairs the editor is expected to show: the same walk
// GraphEditor::buildEntries() makes, expressed with the public Channel table.
QVector<Pair> expectedPairs(const QVector<core::ObjectNode*>& aTargets) {
    QVector<Pair> pairs;
    for (core::ObjectNode* node : aTargets) {
        if (!node)
            continue;
        const core::TimeLine* timeLine = node->timeLine();
        if (!timeLine)
            continue;
        for (core::TimeKeyType type : ctrl::graph::Channel::graphableTypes()) {
            if (timeLine->map(type).isEmpty())
                continue;
            for (const ctrl::graph::Channel& channel : ctrl::graph::Channel::channelsOf(type)) {
                pairs.append(Pair(node, channel));
            }
        }
    }
    return pairs;
}

QString channelText(const ctrl::graph::Channel& aChannel) {
    return QStringLiteral("%1/%2").arg(int(aChannel.type())).arg(aChannel.component());
}

// The value axis' number format, the renderer's own valueText(): 'g' with four
// significant digits. The model reads it from ctrl::graph::Scaler::valueAxisText
// (the function the column's width is measured from), so the test restates it
// here as the documented convention the published label text has to match.
QString valueTextOf(float aValue) {
    return QString::number(double(aValue), 'g', 4);
}

// The label column's reserve, restated from THE LABEL COLUMN in graph_Scaler.h:
// the widest label the value FORMAT can write - one candidate per shape 'g'/4
// can emit, both signs - plus the box's right inset and the text gap, clamped
// into the column's floor and ceiling. It is a function of the format and the
// font ALONE: it deliberately does not take a value window, because a reserve
// that read one would move the plot's left edge whenever a gesture changed which
// label is widest. Stated here so a column that stopped measuring (or that
// measured the window again) fails block [12].
int reserveAxisLabelWidth(const QFontMetrics& aMetrics) {
    const float extremes[] = {
        -std::numeric_limits<float>::max(),        // "-3.403e+38", the exponent form's widest
        -0.0009999f,                               // "-0.0009999", the plain form's widest
        -std::numeric_limits<float>::denorm_min(), // "-1.401e-45"
        std::numeric_limits<float>::max(),         // "3.403e+38"
        std::numeric_limits<float>::denorm_min(),  // "1.401e-45"
        12345.6f,                                  // "1.235e+04"
        100000.0f,                                 // "1e+05"
        -100000.0f,                                // "-1e+05"
        -1234.0f,                                  // "-1234"
    };
    int widest = 0;
    for (const float value : extremes)
        widest = std::max(widest, aMetrics.horizontalAdvance(valueTextOf(value)));
    return std::clamp(
        widest + ctrl::graph::Scaler::kAxisLabelPad + ctrl::graph::Scaler::kAxisLabelTextPad,
        ctrl::graph::Scaler::kAxisLabelWidth, ctrl::graph::Scaler::kMaxAxisLabelWidth
    );
}

// Every rect the model publishes, so "no grid" can be checked mechanically.
QVector<QRect> publishedRects(const gui::GraphPanelModel& aModel) {
    QVector<QRect> rects;
    for (const gui::PanelLabel& label : aModel.frameLabels)
        rects.append(label.rect);
    for (const gui::PanelLabel& label : aModel.valueLabels)
        rects.append(label.rect);
    for (const gui::PanelCurve& curve : aModel.curves) {
        for (const QRect& key : curve.keys)
            rects.append(key);
        // A curve's NAME CHIP: the panel has no swatch stack to publish any more (the
        // legend is gone), and this is the one box the model lays out per channel for
        // the drawing layer to write a name in.
        rects.append(curve.chip);
    }
    rects.append(aModel.marquee);
    return rects;
}

// The y of the polyline at a frame: the sample whose x is the key's own mapped
// x. Curve::buildPolyline emits every touched segment's endpoints, so a key
// inside the window is always a sample, and the nearest-x search is exact.
bool polylineYAt(const gui::PanelCurve& aCurve, double aX, double& aOutY, double& aOutDistance) {
    bool found = false;
    double best = std::numeric_limits<double>::max();
    for (const QPointF& point : aCurve.points) {
        const double distance = std::fabs(point.x() - aX);
        if (distance <= best) {
            best = distance;
            aOutY = point.y();
            found = true;
        }
    }
    aOutDistance = best;
    return found;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    TestAnimator animator;
    core::Project project("graph_panel_model.anie", animator, nullptr);

    // The ladder is a function of the fps and the frame range, and it is seeded
    // when the editor is constructed - so the attribute goes in first.
    project.attribute().setFps(24);
    project.attribute().setFrameRange(util::Range(1, 120));

    // The keys go onto a stack this test owns, so the project's own stack stays
    // empty and the read-only probe below is measurable.
    cmnd::Stack setup;

    core::TestNode nodeA;
    nodeA.setName(QStringLiteral("A"));
    core::TestNode nodeB;
    nodeB.setName(QStringLiteral("B"));

    // A: Rotate (1 channel, 0 -> 90 deg) and Move (2 channels: x 0 -> 100, y 10 -> -30).
    core::RotateKey* rotate0 = new core::RotateKey();
    rotate0->setRotate(0.0f);
    setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Rotate, 0, rotate0));
    core::RotateKey* rotate1 = new core::RotateKey();
    rotate1->setRotate(90.0f);
    setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Rotate, 24, rotate1));

    core::MoveKey* move0 = new core::MoveKey();
    move0->setPos(QVector2D(0.0f, 10.0f));
    setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Move, 0, move0));
    core::MoveKey* move1 = new core::MoveKey();
    move1->setPos(QVector2D(100.0f, -30.0f));
    setup.push(nodeA.timeLine()->createPusher(core::TimeKeyType_Move, 20, move1));

    // B: Opa (1 channel, 0.25 -> 0.75) - a second target, so "every channel of
    // every target" has something to be about.
    core::OpaKey* opa0 = new core::OpaKey();
    opa0->setOpacity(0.25f);
    setup.push(nodeB.timeLine()->createPusher(core::TimeKeyType_Opa, 6, opa0));
    core::OpaKey* opa1 = new core::OpaKey();
    opa1->setOpacity(0.75f);
    setup.push(nodeB.timeLine()->createPusher(core::TimeKeyType_Opa, 30, opa1));

    NullLogger logger;
    ctrl::GraphEditor editor(project, logger);
    // The show the whole file rides on is the surviving path the application uses:
    // the editor has no shown/hidden state any more (the dock's Graph page IS the
    // panel while its tab is current), and the fit that fills the value window is
    // one of the things setTargets() does - the application hands the panel its
    // targets when the document opens and whenever the tree selection changes, so
    // every model built below measures a FITTED editor. Its POST-CONDITION is
    // asserted right here: the window must leave the scaler's seed 0..1, which is
    // what a hand-in that did not take used to make a run fail loudly for.
    editor.setTargets({ &nodeA, &nodeB });
    CHECK(editor.scale().valueMin() != 0.0f || editor.scale().valueMax() != 1.0f);

    const QVector<core::ObjectNode*> targets = { &nodeA, &nodeB };
    const QVector<Pair> pairs = expectedPairs(targets);

    const QSize viewport(1280, 720);
    const theme::Graph palette = theme::Graph::from(theme::Colors::current());

    // THE RULER TOKENS ARE THE TIMELINE'S, token for token: the graph's band, hairline,
    // number ink and playhead-pill ink read the same palette cells the timeline's
    // TimeLineEditorWidget publishes (theme::TimeLine maps its headerBackgroundColor to
    // Colors::base, rulerLineColor to Colors::hairline and headerContentColor to
    // Colors::text - see src/gui/theme/TimeLine.cpp). Measured BEFORE the band's
    // geometry is checked, so a palette drift fails here rather than showing up as a
    // pixel difference between the two tabs. (The timeline theme's own class is not
    // linked into this target; its source cells are the contract.)
    {
        const theme::Colors& colors = theme::Colors::current();
        std::printf(
            "[0] tokens: band=%s line=%s numbers=%s pill=%s  timeline cells base=%s hairline=%s text=%s\n",
            qUtf8Printable(palette.rulerBand().name(QColor::HexRgb)),
            qUtf8Printable(palette.rulerLine().name(QColor::HexRgb)),
            qUtf8Printable(palette.axisText().name(QColor::HexRgb)),
            qUtf8Printable(palette.playheadText().name(QColor::HexRgb)),
            qUtf8Printable(colors.base.name(QColor::HexRgb)),
            qUtf8Printable(colors.hairline.name(QColor::HexRgb)),
            qUtf8Printable(colors.text.name(QColor::HexRgb))
        );
        CHECK(palette.rulerBand() == colors.base);       // TimeLine::headerBackgroundColor
        CHECK(palette.rulerLine() == colors.hairline);   // TimeLine::rulerLineColor
        CHECK(palette.axisText() == colors.text);        // TimeLine::headerContentColor
        CHECK(palette.playheadText() == colors.text);    // the same number ink as the ruler
    }

    // The oracle scaler: the editor's own axes, geometrised with the same panel
    // rect the model derives - built by the test, never read out of the model.
    const QRect panel = ctrl::GraphEditor::panelRect(viewport);
    ctrl::graph::Scaler scale = editor.scale();
    scale.setRect(panel);

    std::printf("viewport %dx%d  panel (%d,%d,%dx%d)  plot (%d,%d,%dx%d)\n", viewport.width(), viewport.height(),
                panel.x(), panel.y(), panel.width(), panel.height(), scale.plotRect().x(), scale.plotRect().y(),
                scale.plotRect().width(), scale.plotRect().height());
    std::printf("expected (target, channel) pairs: %d  [", isz(pairs));
    for (const Pair& pair : pairs)
        std::printf("%s%c", qUtf8Printable(channelText(pair.second)), &pair == &pairs.back() ? ']' : ' ');
    std::printf("\n");

    // -------------------------------------------------------------------------
    // 1. consistency with the editor
    gui::GraphPanelModel model = gui::buildGraphPanelModel(editor, viewport, palette);
    {
        std::printf("[1] panel (%d,%d,%dx%d) == panelRect(viewport): %d  plot (%d,%d,%dx%d)  valueColumn (%d,%d,%dx%d)\n",
                    model.panel.x(), model.panel.y(), model.panel.width(), model.panel.height(),
                    int(model.panel == ctrl::GraphEditor::panelRect(viewport)), model.plot.x(), model.plot.y(),
                    model.plot.width(), model.plot.height(), model.valueColumn.x(), model.valueColumn.y(),
                    model.valueColumn.width(), model.valueColumn.height());

        CHECK(model.panel == ctrl::GraphEditor::panelRect(viewport));
        CHECK(!model.plot.isEmpty());
        CHECK(model.panel.contains(model.plot));
        CHECK(model.valueColumn.right() < model.plot.left());
        CHECK(!model.valueColumn.intersects(model.plot));
        // The plot is inset by the MEASURED label column (graph_Scaler.h's THE
        // LABEL COLUMN), so this is the old statement with the measurement in
        // place of the fixed reserve - and the reserve is now its floor. The
        // oracle scaler is the test's own copy of the editor's axes, so the two
        // independent derivations of the column have to agree.
        CHECK(model.plot.left() == model.panel.left() + ctrl::graph::Scaler::kMargin + model.valueColumn.width());
        // THE ONLY RECT THE BAND MAY MOVE IS ITS OWN: the plot is the editor's own
        // scaler rect (the hit-test geometry), byte for byte, whatever the band's rows.
        CHECK(model.plot == scale.plotRect());
        CHECK(model.valueColumn.width() == scale.axisLabelWidth());
        CHECK(ctrl::graph::Scaler::kAxisLabelWidth <= model.valueColumn.width());
        CHECK(model.valueColumn.width() <= ctrl::graph::Scaler::kMaxAxisLabelWidth);
        // THE RULER IS A SECTION ABOVE THE GRAPH. The plot's top is the panel's
        // margin PLUS the band's 22 reserved rows, and its height loses exactly
        // those rows: the scaler's plot rect is the shared geometry, so every axis,
        // every curve point and the editor's hit-test start below the ruler.
        CHECK(model.plot.top()
              == model.panel.top() + ctrl::graph::Scaler::kMargin
                  + ctrl::graph::Scaler::kRulerBandHeight);
        // ...and its HEIGHT loses the two margins PLUS the one the scroll bars sit in
        // at the bottom edge (the right margin carries the same reserve, see THE
        // SCROLL BARS' RESERVE in graph_Scaler.h: the plot gives up 14px at those two
        // edges - the app's 12px lane plus a 2px gap - so the bars have a strip that
        // is outside the plot and inside the panel).
        CHECK(model.plot.height()
              == model.panel.height() - 2 * ctrl::graph::Scaler::kMargin
                  - ctrl::graph::Scaler::kRulerBandHeight - ctrl::graph::Scaler::kScrollBarReserve);
        CHECK(model.plot.width()
              == model.panel.width() - 2 * ctrl::graph::Scaler::kMargin - model.valueColumn.width()
                  - ctrl::graph::Scaler::kScrollBarReserve);
        CHECK(model.plot.right() == model.panel.right() - ctrl::graph::Scaler::kMargin
                  - ctrl::graph::Scaler::kScrollBarReserve);
        CHECK(model.plot.bottom() == model.panel.bottom() - ctrl::graph::Scaler::kMargin
                  - ctrl::graph::Scaler::kScrollBarReserve);
        // The band is on the TIMELINE'S ROW: the timeline's TimeRuler strip starts at
        // its page's top edge, and both views are pages of the same dock stack, so the
        // band must start at the panel's top row - not at the scaler's top margin
        // (measured before this rule: timeline strip page.top()+9, graph band
        // page.top()+13). The scaler still reserves the band's 22 rows, but the
        // kMargin rows now sit BETWEEN the band's hairline and the plot, so the plot
        // (the editor's hit geometry) does not move.
        CHECK(model.rulerBand.top() == model.panel.top());
        // THE BAND SPANS THE PANEL: the shared frame mapping puts frames on the
        // timeline's own columns, so the ruler must cover all of them - including the
        // value column's top-left corner, which a plot-wide band left bare.
        CHECK(model.rulerBand.left() == model.panel.left());
        CHECK(model.rulerBand.width() == model.panel.width());
        CHECK(model.rulerBand.right() == model.panel.right());
        CHECK(model.rulerBand.bottom() + 1 + ctrl::graph::Scaler::kMargin == model.plot.top());
        CHECK(model.rulerBand.bottom() + 1 < model.plot.top());
        CHECK(!model.plot.intersects(model.rulerBand));

        // The small viewport, same invariants. What happens to the plot at this
        // size is reported rather than assumed: at 320x200 it is still non-empty,
        // and the curves survive (below).
        const QSize small(320, 200);
        const QRect smallPanel = ctrl::GraphEditor::panelRect(small);
        const gui::GraphPanelModel smallModel = gui::buildGraphPanelModel(editor, small, palette);
        std::printf("[1] small viewport %dx%d -> panel (%d,%d,%dx%d)  plot (%d,%d,%dx%d) empty=%d  curves=%d  "
                    "frameTicks=%d  frameLabels=%d\n",
                    small.width(), small.height(), smallModel.panel.x(), smallModel.panel.y(), smallModel.panel.width(),
                    smallModel.panel.height(), smallModel.plot.x(), smallModel.plot.y(), smallModel.plot.width(),
                    smallModel.plot.height(), int(smallModel.plot.isEmpty()), isz(smallModel.curves),
                    isz(smallModel.frameTickX), isz(smallModel.frameLabels));

        CHECK(smallModel.panel == smallPanel);
        CHECK(!smallModel.plot.isEmpty());
        CHECK(smallPanel.contains(smallModel.plot));
        CHECK(smallModel.valueColumn.right() < smallModel.plot.left());
        CHECK(!smallModel.valueColumn.intersects(smallModel.plot));
        CHECK(smallModel.curves.size() == model.curves.size());
        // The tier vector is index-aligned with the positions at every viewport: both
        // are appended in one step (buildFrameAxis), so this is a structural invariant
        // of the model rather than a property of this window.
        CHECK(smallModel.frameTickLevel.size() == smallModel.frameTickX.size());

        // A viewport too small for the label column and the margins has no plot
        // at all: the model says so instead of publishing geometry it cannot back.
        const QSize degenerate(40, 20);
        const gui::GraphPanelModel degenerateModel = gui::buildGraphPanelModel(editor, degenerate, palette);
        std::printf("[1] degenerate viewport %dx%d -> plot (%d,%d,%dx%d) empty=%d  emptyState=\"%s\"\n",
                    degenerate.width(), degenerate.height(), degenerateModel.plot.x(), degenerateModel.plot.y(),
                    degenerateModel.plot.width(), degenerateModel.plot.height(), int(degenerateModel.plot.isEmpty()),
                    qUtf8Printable(degenerateModel.emptyState));
        CHECK(degenerateModel.plot.isEmpty());
        CHECK(!degenerateModel.emptyState.isEmpty());
        CHECK(degenerateModel.curves.isEmpty());
    }

    // -------------------------------------------------------------------------
    // 2. no grid: the axes are position lists, and nothing published spans the plot
    {
        // The structural half: a full-height or full-width line cannot be
        // expressed in these members at all - they are ints, not rects.
        static_assert(std::is_same<decltype(gui::GraphPanelModel::frameTickX), QVector<int> >::value,
                      "frameTickX must be tick POSITIONS (ints): the model publishes no line geometry");
        static_assert(std::is_same<decltype(gui::GraphPanelModel::valueTickY), QVector<int> >::value,
                      "valueTickY must be tick POSITIONS (ints): the model publishes no line geometry");
        // The tier vector is the same KIND of statement about a tick as the position
        // vector is: an int per tick. It CLASSIFIES the tick - the ladder's own
        // ctrl::time::Scaler::TickLevel as its value - and it is not appearance: no
        // colour, no length, nothing a drawing layer would have to obey. The ladder
        // check in block 3 pins that every value is one of the three drawn tiers and
        // that the vector is index-aligned with frameTickX.
        static_assert(std::is_same<decltype(gui::GraphPanelModel::frameTickLevel), QVector<int> >::value,
                      "frameTickLevel must be a per-tick CLASSIFICATION (ints): the model publishes no appearance");

        const QVector<QRect> rects = publishedRects(model);
        int spanning = 0;
        for (const QRect& rect : rects) {
            if (rect.height() >= model.plot.height() || rect.width() >= model.plot.width())
                ++spanning;
        }
        std::printf("[2] frameTickX=%d valueTickY=%d published rects=%d spanning the plot=%d\n", isz(model.frameTickX),
                    isz(model.valueTickY), isz(rects), spanning);

        CHECK(!model.frameTickX.isEmpty());
        CHECK(!model.valueTickY.isEmpty());
        CHECK(!rects.isEmpty());
        CHECK(spanning == 0);

        // THE FRAME NUMBERS BELONG TO THE RULER, NOT THE GRAPH: their boxes are in
        // the band above the plot, and none of them may reach into the plot's rows
        // (the model's own section boundary).
        int outside = 0;
        int inPlotRows = 0;
        for (const gui::PanelLabel& label : model.frameLabels) {
            if (!model.rulerBand.contains(label.rect))
                ++outside;
            if (label.rect.intersects(model.plot))
                ++inPlotRows;
        }
        std::printf("[2] frameLabels=%d  outside the ruler band=%d  reaching into the plot=%d\n",
                    isz(model.frameLabels), outside, inPlotRows);
        CHECK(!model.frameLabels.isEmpty());
        CHECK(outside == 0);
        CHECK(inPlotRows == 0);

        int valuesOutside = 0;
        for (const gui::PanelLabel& label : model.valueLabels) {
            if (!model.valueColumn.contains(label.rect))
                ++valuesOutside;
        }
        std::printf("[2] valueLabels=%d  outside the value column=%d  first=\"%s\" at x=%d\n", isz(model.valueLabels),
                    valuesOutside, model.valueLabels.isEmpty() ? "" : qUtf8Printable(model.valueLabels.first().text),
                    model.valueLabels.isEmpty() ? -1 : model.valueLabels.first().rect.x());
        CHECK(!model.valueLabels.isEmpty());
        CHECK(valuesOutside == 0);
    }

    // -------------------------------------------------------------------------
    // 3. the ladder follows the scaler
    {
        // THE ONE FORMATTER the whole frame axis writes with: the test's own
        // core::TimeFormat, built from the EDITOR's frame range and rate, so every
        // label's text is checked against
        // core::TimeFormat::frameToString(frame, editor.timeFormatType()) for the
        // frame(s) the label publishes - never against the model's own string.
        const core::TimeFormat format(editor.frameRange(), editor.fps());
        const core::TimeFormatType formatType = editor.timeFormatType();
        const int numberReserve = gui::frameRuler::kNumberCharWidth;

        int sampled = 0;
        int wrongText = 0;
        int noFrame = 0;
        int noTick = 0;
        int minGap = std::numeric_limits<int>::max();
        int previousRight = std::numeric_limits<int>::min();
        bool anyPrevious = false;

        for (const gui::PanelLabel& label : model.frameLabels) {
            ++sampled;
            // A merged boundary label names TWO frames ("first-last"); every other
            // frame label names exactly one, and that frame is published beside its
            // text in PanelLabel::frame.
            const bool merged = label.endFrame >= 0;
            const QString expected =
                merged ? format.frameToString(label.frame, formatType) + QString::fromUtf8("\u2013") +
                             format.frameToString(label.endFrame, formatType)
                       : format.frameToString(label.frame, formatType);
            if (!merged && label.frame < 0)
                ++noFrame;
            if (label.text != expected)
                ++wrongText;

            // The frame(s) the text names own a tick: an ordinary number's box contains
            // the tick of its own frame (the box is centred on it and nudged inside the
            // plot, so clamping can move the centre but never off the tick), and the
            // merged label's two boundary frames each have their tick somewhere.
            const auto tickInsideBox = [&model, &scale, &label](int aFrame) {
                for (const int x : model.frameTickX) {
                    if (x >= label.rect.left() && x <= label.rect.right() && scale.frameAt(x).get() == aFrame)
                        return true;
                }
                return false;
            };
            const auto tickExists = [&model, &scale](int aFrame) {
                for (const int x : model.frameTickX) {
                    if (scale.frameAt(x).get() == aFrame)
                        return true;
                }
                return false;
            };
            if (merged ? !(tickExists(label.frame) && tickExists(label.endFrame)) : !tickInsideBox(label.frame))
                ++noTick;

            if (anyPrevious) {
                const int gap = label.rect.left() - previousRight - 1;
                minGap = std::min(minGap, gap);
            }
            previousRight = label.rect.right();
            anyPrevious = true;

            if (sampled <= 4) {
                std::printf(
                    "[3] label \"%s\" frame=%d end=%d expected=\"%s\" box x=%d..%d y=%d..%d reserve=%dpx\n",
                    qUtf8Printable(label.text), label.frame, label.endFrame, qUtf8Printable(expected),
                    label.rect.left(), label.rect.right(), label.rect.top(), label.rect.bottom(),
                    numberReserve * int(label.text.size())
                );
            }
        }

        std::printf("[3] sampled %d frame labels, %d not equal to frameToString(frame), %d without a frame,"
                    " %d without the tick of their own frame, minimum gap between neighbouring boxes=%d px\n",
                    sampled, wrongText, noFrame, noTick, sampled > 1 ? minGap : -1);
        CHECK(sampled > 1);
        CHECK(wrongText == 0);
        CHECK(noFrame == 0);
        CHECK(noTick == 0);
        CHECK(minGap >= 8);

        // One tick, one number: the label rects are never the same box twice.
        int duplicates = 0;
        for (int i = 1; i < model.frameLabels.size(); ++i) {
            if (model.frameLabels[i].rect.intersects(model.frameLabels[i - 1].rect))
                ++duplicates;
        }
        std::printf("[3] overlapping neighbouring label boxes=%d\n", duplicates);
        CHECK(duplicates == 0);

        // The ticks ARE the ladder's own ticks (plus the ruler's forced boundaries
        // below), not a tick per pixel: deleting the `level == None` filter in
        // buildFrameAxis would turn the axis into the solid comb the design removed -
        // one tick per pixel at the 1px/frame floor. The measurements that catch that
        // run after the oracle, which knows which columns the ruler forced.

        // THE LADDER'S TICKS, PLUS THE RULER'S FORCED BOUNDARIES. The oracle is the
        // scaler's own classification, re-derived with the documented rule the model
        // follows: every None-filtered classified frame inside the plot is a tick, and
        // the PROJECT's first/last frames (the editor's own range) replace their tick
        // with a Major one when the ruler names them (their labels are active or the
        // two are merged) - the timeline's boundary rule (TimeRuler.cpp:139-182). The
        // model's own vectors are never used as their own oracle.
        // The axis is enumerated over the RULER BAND - the panel's columns - because
        // that is where the ticks are drawn now (the band spans the panel, and the
        // frame mapping is the timeline's).
        const core::Frame windowFirst = scale.frameAt(model.rulerBand.left());
        const core::Frame windowLast = scale.frameAt(model.rulerBand.right());
        QVector<int> ladderTicks;
        QVector<int> ladderLevels;
        int classified = 0;
        for (int frame = windowFirst.get(); frame <= windowLast.get(); ++frame) {
            const ctrl::time::Scaler::TickLevel level =
                scale.frameScale().attributeAt(frame, scale.framePpf()).level;
            if (level == ctrl::time::Scaler::TickLevel::None)
                continue;
            ++classified;
            const int x = scale.pixelX(core::Frame(frame));
            if (x < model.rulerBand.left() || model.rulerBand.right() < x)
                continue;
            if (ladderTicks.isEmpty() || ladderTicks.last() != x) {
                ladderTicks.append(x);
                ladderLevels.append(int(level));
            }
        }
        // The boundary frames, promoted to the ruler's Major tick when the model is
        // asked to name them - the same condition buildFrameAxis uses.
        const int boundaryFirstFrame = editor.frameRange().min();
        const int boundaryLastFrame = editor.frameRange().max();
        const bool hasBoundaryRange = boundaryFirstFrame < boundaryLastFrame;
        const int boundaryFirstX = hasBoundaryRange ? scale.pixelX(core::Frame(boundaryFirstFrame)) : -1;
        const int boundaryLastX = hasBoundaryRange ? scale.pixelX(core::Frame(boundaryLastFrame)) : -1;
        const auto inBand = [&model](int aX) {
            return model.rulerBand.left() <= aX && aX <= model.rulerBand.right();
        };
        const int majorLevel = int(ctrl::time::Scaler::TickLevel::Major);
        const auto levelOf = [&scale](int aFrame) {
            return int(scale.frameScale().attributeAt(aFrame, scale.framePpf()).level);
        };
        const bool firstInView = hasBoundaryRange && inBand(boundaryFirstX);
        const bool lastInView = hasBoundaryRange && inBand(boundaryLastX);
        const bool firstActive = firstInView && levelOf(boundaryFirstFrame) != majorLevel;
        const bool lastActive = lastInView && levelOf(boundaryLastFrame) != majorLevel;
        const QString firstBoundaryText =
            firstInView ? format.frameToString(boundaryFirstFrame, formatType) : QString();
        const QString lastBoundaryText =
            lastInView ? format.frameToString(boundaryLastFrame, formatType) : QString();
        const bool mergedBoundaries = firstInView && lastInView &&
            (boundaryFirstX + (numberReserve * int(firstBoundaryText.size())) / 2 +
                 gui::frameRuler::kNumberMargin >=
             boundaryLastX - (numberReserve * int(lastBoundaryText.size())) / 2);
        // The forced boundary ticks, by column: a tick the ladder does not classify is
        // allowed exactly here (that is what the ruler's boundary ink is).
        QVector<int> forcedX;
        const auto promote = [&ladderTicks, &ladderLevels, &forcedX](int aX) {
            for (int i = 0; i < ladderTicks.size(); ++i) {
                if (ladderTicks.at(i) == aX) {
                    ladderTicks.removeAt(i);
                    ladderLevels.removeAt(i);
                    break;
                }
            }
            ladderTicks.append(aX);
            ladderLevels.append(int(ctrl::time::Scaler::TickLevel::Major));
            forcedX.append(aX);
        };
        if (firstInView && (mergedBoundaries || firstActive))
            promote(boundaryFirstX);
        if (lastInView && (mergedBoundaries || lastActive))
            promote(boundaryLastX);
        // Ascending, one tick per column (the more prominent tier wins a shared one).
        {
            QVector<int> order;
            for (int i = 0; i < ladderTicks.size(); ++i)
                order.append(i);
            std::stable_sort(order.begin(), order.end(), [&ladderTicks, &ladderLevels](int a, int b) {
                return ladderTicks.at(a) != ladderTicks.at(b) ? ladderTicks.at(a) < ladderTicks.at(b)
                                                              : ladderLevels.at(a) > ladderLevels.at(b);
            });
            QVector<int> sortedTicks;
            QVector<int> sortedLevels;
            for (int i : order) {
                if (!sortedTicks.isEmpty() && sortedTicks.last() == ladderTicks.at(i))
                    continue;
                sortedTicks.append(ladderTicks.at(i));
                sortedLevels.append(ladderLevels.at(i));
            }
            ladderTicks = sortedTicks;
            ladderLevels = sortedLevels;
        }

        int unclassified = 0;
        int notRoundTrip = 0;
        int notAscending = 0;
        for (int i = 0; i < model.frameTickX.size(); ++i) {
            const int x = model.frameTickX.at(i);
            const core::Frame frame = scale.frameAt(x);
            const bool forced = forcedX.contains(x);
            if (!forced && scale.frameScale().attribute(frame.get()).level == ctrl::time::Scaler::TickLevel::None)
                ++unclassified;
            if (scale.pixelX(frame) != x)
                ++notRoundTrip;
            if (0 < i && x <= model.frameTickX.at(i - 1))
                ++notAscending;
        }

        std::printf("[3] frame ticks: published=%d, the ladder's own inside the plot=%d (%d classified frames in "
                    "the window)  unclassified ticks=%d  pixel round-trip mismatches=%d  out of order=%d\n",
                    isz(model.frameTickX), isz(ladderTicks), classified, unclassified, notRoundTrip, notAscending);
        CHECK(!model.frameTickX.isEmpty());
        CHECK(unclassified == 0);
        CHECK(notRoundTrip == 0);
        CHECK(notAscending == 0);
        CHECK(model.frameTickX == ladderTicks);

        // The TIERS: one per tick, index-aligned with the positions, each one of the
        // three tiers the ruler draws, and each the level the oracle derived for that
        // column. TickLevel::None reaches the model only through a FORCED boundary
        // tick (which the ruler promotes to Major), so a None entry elsewhere, a tier
        // for the wrong tick, or a count that does not match the positions, fails here.
        int notATier = 0;
        int tierHistogram[4] = {0, 0, 0, 0};
        for (int i = 0; i < model.frameTickLevel.size(); ++i) {
            const int level = model.frameTickLevel.at(i);
            const bool known = int(ctrl::time::Scaler::TickLevel::Minor) <= level &&
                level <= int(ctrl::time::Scaler::TickLevel::Major);
            if (!known)
                ++notATier;
            else
                ++tierHistogram[level];
        }
        std::printf("[3] frame tick tiers: major=%d intermediate=%d minor=%d  out of the three tiers=%d  "
                    "index-aligned with the positions=%d (ticks=%d levels=%d)  matching the ladder=%d\n",
                    tierHistogram[int(ctrl::time::Scaler::TickLevel::Major)],
                    tierHistogram[int(ctrl::time::Scaler::TickLevel::Intermediate)],
                    tierHistogram[int(ctrl::time::Scaler::TickLevel::Minor)], notATier,
                    int(model.frameTickLevel.size() == model.frameTickX.size()), isz(model.frameTickX),
                    isz(model.frameTickLevel), int(model.frameTickLevel == ladderLevels));
        CHECK(!model.frameTickLevel.isEmpty());
        CHECK(model.frameTickLevel.size() == model.frameTickX.size());
        CHECK(notATier == 0);
        CHECK(model.frameTickLevel == ladderLevels);

        // The value ticks are the value axis' own ticks: one per published value
        // label, in the labels' order, each on the pixel of the value its label
        // names (and the box is centred on that pixel, as far as the column's own
        // edges allow - the clamp moves it at the extremes). Two of the window's
        // three values rounding to the same string are deduplicated, which is the
        // renderer's own rule, so the expected pair of lists is derived from the
        // scaler's window with that rule applied.
        const float windowValues[3] = {
            scale.valueMax(), 0.5f * (scale.valueMin() + scale.valueMax()), scale.valueMin()
        };
        QVector<QString> expectedValueText;
        QVector<int> expectedValueTickY;
        QString shownValue;
        for (const float value : windowValues) {
            const QString text = valueTextOf(value);
            if (text == shownValue)
                continue;
            shownValue = text;
            expectedValueText.append(text);
            expectedValueTickY.append(scale.pixelY(value));
        }

        int valueTextMismatches = 0;
        int valueTicksOffCentre = 0;
        for (int i = 0; i < model.valueLabels.size() && i < expectedValueText.size(); ++i) {
            if (model.valueLabels.at(i).text != expectedValueText.at(i))
                ++valueTextMismatches;
            if (i < model.valueTickY.size()) {
                const int centre = model.valueLabels.at(i).rect.center().y();
                if (std::abs(centre - model.valueTickY.at(i)) > model.valueLabels.at(i).rect.height() / 2 + 1)
                    ++valueTicksOffCentre;
            }
        }

        std::printf("[3] value ticks: published=%d, expected=%d  labels=%d, text mismatches=%d, ticks not on their "
                    "label's own pixel=%d\n",
                    isz(model.valueTickY), isz(expectedValueTickY), isz(model.valueLabels), valueTextMismatches,
                    valueTicksOffCentre);
        CHECK(model.valueTickY == expectedValueTickY);
        CHECK(model.valueLabels.size() == expectedValueText.size());
        CHECK(valueTextMismatches == 0);
        CHECK(valueTicksOffCentre == 0);
    }

    // -------------------------------------------------------------------------
    // 4. curves
    {
        const QVector<ctrl::graph::Curve> editorCurves = editor.curves();
        std::printf("[4] curves=%d  editor.curves()=%d  channel rows=%d\n", isz(model.curves), isz(editorCurves),
                    isz(model.channels));

        // Exactly one curve per expected pair, in the editor's own order - and one
        // CHANNEL LIST row per pair too, in the same order, with the curve naming the
        // row it belongs to (so a consumer of one list can address the other).
        CHECK(model.curves.size() == pairs.size());
        CHECK(editorCurves.size() == pairs.size());
        CHECK(model.channels.size() == pairs.size());
        for (int i = 0; i < pairs.size() && i < model.curves.size(); ++i) {
            CHECK(model.curves[i].node == pairs[i].first);
            CHECK(model.curves[i].channel == pairs[i].second);
            CHECK(model.curves[i].channelRow == i);
            CHECK(0 <= model.curves[i].channelRow && model.curves[i].channelRow < model.channels.size());
            CHECK(model.channels[model.curves[i].channelRow].node == pairs[i].first);
            CHECK(model.channels[model.curves[i].channelRow].channel == pairs[i].second);
            CHECK(model.curves[i].color.isValid());
            CHECK(model.curves[i].color == model.channels[model.curves[i].channelRow].color);
        }
        for (int i = 0; i < editorCurves.size() && i < model.curves.size(); ++i) {
            CHECK(editorCurves[i].channel() == model.curves[i].channel); // same set, same order
        }

        int unique = 0;
        for (int i = 0; i < model.curves.size(); ++i) {
            bool duplicate = false;
            for (int j = 0; j < i; ++j) {
                if (model.curves[j].node == model.curves[i].node && model.curves[j].channel == model.curves[i].channel)
                    duplicate = true;
            }
            if (!duplicate)
                ++unique;
        }
        CHECK(unique == model.curves.size());

        int totalPoints = 0;
        int totalKeys = 0;
        double maxYDeviation = 0.0;
        double maxXDistance = 0.0;
        int badKeyRects = 0;
        int keysOutside = 0;
        int shortCurves = 0;

        for (const gui::PanelCurve& curve : model.curves) {
            totalPoints += curve.points.size();
            totalKeys += curve.keys.size();
            if (curve.points.size() <= 1)
                ++shortCurves;

            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            for (const core::TimeKey* key : source.keys()) {
                const core::Frame frame(key->frame());
                const float value = source.valueOf(*key);

                // The key rect is the hit-test's own rectangle, cut to the plot.
                const QRect expected =
                    ctrl::graph::keyRect(scale, frame, value).intersected(model.plot);
                if (!expected.isEmpty() && !curve.keys.contains(expected))
                    ++badKeyRects;
            }
            for (const QRect& key : curve.keys) {
                if (!model.plot.contains(key))
                    ++keysOutside;
            }

            // The polyline passes through its keys.
            for (const core::TimeKey* key : source.keys()) {
                double y = 0.0;
                double distance = 0.0;
                if (!polylineYAt(curve, scale.pixelXOfFrame(double(key->frame())), y, distance))
                    continue;
                maxXDistance = std::max(maxXDistance, distance);
                maxYDeviation = std::max(maxYDeviation, std::fabs(y - double(scale.pixelY(source.valueOf(*key)))));
            }
        }

        std::printf("[4] points=%d (min per curve=%d) keys=%d  key rects outside the plot=%d  key rects != "
                    "graph::keyRect=%d\n",
                    totalPoints,
                    [&model] {
                        int m = std::numeric_limits<int>::max();
                        for (const gui::PanelCurve& c : model.curves)
                            m = std::min(m, isz(c.points));
                        return m;
                    }(), totalKeys, keysOutside, badKeyRects);
        std::printf("[4] max |polyline.x - keyPixelX| = %.6f px,  max |polyline.y - pixelY(valueOf(key))| = %.6f px\n",
                    maxXDistance, maxYDeviation);

        CHECK(shortCurves == 0);
        CHECK(keysOutside == 0);
        CHECK(badKeyRects == 0);
        CHECK(maxXDistance <= 0.5);
        CHECK(maxYDeviation <= 2.0);

        // The channel list rows, and the NAME CHIP of every curve.
        //
        // A ROW is data only (the list is the application's own QMenu, so the model
        // lays out no row geometry - the header's CHANNEL LIST): it must name a real
        // target and channel, carry its layer, its name and a valid colour, and its
        // `visible` must be the editor's own answer.
        //
        // A CHIP is geometry, and it is derived from the curve it NAMES: the box's
        // right edge is at the plot's right edge, its vertical CENTRE is the value
        // that same curve leaves the plot with (asserted against the curve's own
        // published polyline, not against the model's arithmetic), and it lies
        // inside the plot. A curve that leaves the window too near an edge publishes
        // NO chip - an empty rect - rather than one in the margin.
        int badRows = 0;
        for (const gui::PanelChannelRow& row : model.channels) {
            const bool wellFormed = row.node != nullptr && row.node->timeLine() != nullptr && !row.name.isEmpty() &&
                !row.layer.isEmpty() && row.color.isValid() && row.visible == editor.isChannelVisible(*row.node, row.channel);
            if (!wellFormed)
                ++badRows;
        }
        std::printf("[4] channel rows=%d, malformed rows=%d, first row layer=\"%s\" name=\"%s\" visible=%d\n",
                    isz(model.channels), badRows, qUtf8Printable(model.channels.first().layer),
                    qUtf8Printable(model.channels.first().name), int(model.channels.first().visible));
        CHECK(model.channels.size() == pairs.size());
        CHECK(badRows == 0);

        // THE GROUPED LEGEND AND THE CHANNEL HUE TABLE (design section 7).
        //
        // The list is grouped one group per (layer, key type) - the group naming
        // the property and its components, with one row per component inside it -
        // and each row's colour is the channel's hue from the ONE table both views
        // read (theme::Colors, THE CHANNEL HUE TABLE): axis where the component
        // lies on an axis (X red, Y green), the type's own hue otherwise. This
        // pins the grouping and the per-component hue, so the panel's curve/marker/
        // chip ink cannot drift from the timeline's key and lane ink.
        {
            const theme::Colors& colors = theme::Colors::current();

            // The four keys this document has: A's Move (X, Y) and Rotate, then
            // B's Opa - target order, then the timeline's own type order.
            CHECK(model.channels.size() == 4);
            CHECK(model.channels.at(0).channel == ctrl::graph::Channel(core::TimeKeyType_Move, 0));
            CHECK(model.channels.at(1).channel == ctrl::graph::Channel(core::TimeKeyType_Move, 1));
            CHECK(model.channels.at(2).channel == ctrl::graph::Channel(core::TimeKeyType_Rotate, 0));
            CHECK(model.channels.at(3).channel == ctrl::graph::Channel(core::TimeKeyType_Opa, 0));

            // ONE GROUP PER (layer, key type), naming the property and every
            // component - the design's "line · Move [X][Y]" / "line · Opa [Opacity]".
            CHECK(model.channels.at(0).group == QStringLiteral("Move [X][Y]"));
            CHECK(model.channels.at(1).group == QStringLiteral("Move [X][Y]"));
            CHECK(model.channels.at(2).group == QStringLiteral("Rotate [Angle]"));
            CHECK(model.channels.at(3).group == QStringLiteral("Opa [Opacity]"));
            // A group's rows are contiguous: the same (layer, type) never returns
            // after another group has started, or the menu's boundary walk would
            // open a second heading for one property.
            for (int i = 1; i < model.channels.size(); ++i) {
                const gui::PanelChannelRow& previous = model.channels.at(i - 1);
                const gui::PanelChannelRow& row = model.channels.at(i);
                const bool sameGroup = previous.node == row.node && previous.channel.type() == row.channel.type();
                CHECK(sameGroup ? previous.group == row.group : previous.group != row.group);
            }

            // THE PER-COMPONENT HUE. X and Y of one type differ; a channel with no
            // axis keeps its type's hue.
            CHECK(model.channels.at(0).color == colors.axisX);
            CHECK(model.channels.at(1).color == colors.axisY);
            CHECK(model.channels.at(0).color != model.channels.at(1).color);
            CHECK(model.channels.at(2).color == colors.rotateKey); // angle: no axis
            CHECK(model.channels.at(3).color == colors.opaKey);    // opacity: no axis

            // THE NO-AXIS GUARD: an axis must never be silently assigned to Depth
            // (the design leaves it on its type's hue) or to an angle, and HSV's
            // three values have no axis either. Both the classifier and the colour
            // are pinned, so a future axis added for one of them fails here.
            CHECK(theme::channelAxis(core::TimeKeyType_Depth, 0) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_Rotate, 0) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_Opa, 0) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_HSV, 0) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_HSV, 1) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_HSV, 2) < 0);
            CHECK(theme::channelAxis(core::TimeKeyType_Blur, 2) < 0); // Blur's angle
            // ...while Blur's two RADII are its X/Y axes, the interpretation that
            // makes its three channels distinguishable (the section's own defect
            // list names 'Blur's three' as sharing one hue).
            CHECK(theme::channelAxis(core::TimeKeyType_Blur, 0) == theme::kChannelAxisX);
            CHECK(theme::channelAxis(core::TimeKeyType_Blur, 1) == theme::kChannelAxisY);
            CHECK(theme::channelColor(colors, core::TimeKeyType_Depth, 0) == colors.depthKey);
            CHECK(theme::channelColor(colors, core::TimeKeyType_HSV, 0) == colors.HSVKey);
            CHECK(theme::channelColor(colors, core::TimeKeyType_HSV, 1) == colors.HSVKey);
            CHECK(theme::channelColor(colors, core::TimeKeyType_HSV, 2) == colors.HSVKey);
            CHECK(theme::channelColor(colors, core::TimeKeyType_Blur, 2) == colors.blurKey);
            // The panel and the timeline read the SAME answer: the model row's hue
            // is theme::channelColor, and the timeline's key/lane hue for the type
            // is channelColor(type, 0).
            CHECK(model.channels.at(0).color == theme::channelColor(colors, core::TimeKeyType_Move, 0));
            CHECK(model.channels.at(1).color == theme::channelColor(colors, core::TimeKeyType_Move, 1));
            CHECK(palette.keyColor(core::TimeKeyType_Move) == model.channels.at(0).color);
            CHECK(palette.keyColor(core::TimeKeyType_Opa) == model.channels.at(3).color);
            CHECK(palette.channelColor(core::TimeKeyType_Move, 1) == model.channels.at(1).color);

            // THE UNREACHABLE TYPES, proven rather than assumed: componentCount()
            // is 0 for all four, and graphableTypes() - the filter buildChannels
            // walks - never yields them, so no row and no curve can name them. This
            // is why their entries were deleted from the panel's table; the
            // timeline still draws their keys, so Colors keeps their tokens.
            CHECK(ctrl::graph::Channel::componentCount(core::TimeKeyType_Bone) == 0);
            CHECK(ctrl::graph::Channel::componentCount(core::TimeKeyType_Pose) == 0);
            CHECK(ctrl::graph::Channel::componentCount(core::TimeKeyType_Mesh) == 0);
            CHECK(ctrl::graph::Channel::componentCount(core::TimeKeyType_FFD) == 0);
            CHECK(!ctrl::graph::Channel::graphableTypes().contains(core::TimeKeyType_Bone));
            CHECK(!ctrl::graph::Channel::graphableTypes().contains(core::TimeKeyType_Pose));
            CHECK(!ctrl::graph::Channel::graphableTypes().contains(core::TimeKeyType_Mesh));
            CHECK(!ctrl::graph::Channel::graphableTypes().contains(core::TimeKeyType_FFD));
            for (const gui::PanelChannelRow& row : model.channels)
                CHECK(ctrl::graph::Channel::componentCount(row.channel.type()) > 0);
        }

        int chips = 0;
        int emptyChips = 0;
        int badChipText = 0;
        int emptyChipText = 0;
        int outsideChips = 0;
        int offCurveChips = 0;
        double maxChipCentreError = 0.0;
        for (const gui::PanelCurve& curve : model.curves) {
            if (curve.chipText.isEmpty()) {
                ++emptyChipText;
                continue;
            }
            if (curve.channelRow < 0 || curve.channelRow >= model.channels.size()) {
                ++badChipText;
                continue;
            }
            // The chip names the CURVE: its text is the name of the row this curve
            // belongs to (not some other channel's).
            if (curve.chipText != model.channels.at(curve.channelRow).name)
                ++badChipText;
            if (curve.chip.isEmpty()) {
                ++emptyChips;
                continue;
            }
            ++chips;
            if (!model.plot.contains(curve.chip))
                ++outsideChips;
            // The chip is anchored at the plot's right edge: its box ends inside the
            // plot and starts left of it, so it is a chip AT the curve's own exit
            // point rather than a label parked somewhere else.
            if (curve.chip.right() > model.plot.right() || curve.chip.left() < model.plot.left())
                ++outsideChips;
            // ...and it is centred on the value the curve LEAVES the window with: the
            // curve's own published polyline at the plot's right edge.
            double y = 0.0;
            double distance = 0.0;
            if (polylineYAt(curve, double(model.plot.right()), y, distance)) {
                const double error = std::fabs(double(curve.chip.center().y()) - y);
                maxChipCentreError = std::max(maxChipCentreError, error);
                if (error > 1.0)
                    ++offCurveChips;
            }
        }
        std::printf("[4] chips=%d  curves whose chip is empty=%d  chips outside the plot=%d  chips off their own "
                    "curve=%d (max |centre - polyline y| = %.3f px)  empty chip texts=%d  chips naming another row=%d\n",
                    chips, emptyChips, outsideChips, offCurveChips, maxChipCentreError, emptyChipText, badChipText);
        CHECK(chips > 0);          // the fixture's premise: some curve leaves the window inside the plot's rows
        CHECK(emptyChipText == 0); // every curve names itself
        CHECK(badChipText == 0);
        CHECK(outsideChips == 0);
        CHECK(offCurveChips == 0);
    }

    // -------------------------------------------------------------------------
    // 8. NO LEGEND: the names live at their curves. Two things are pinned, and
    // neither is cosmetic:
    //   1. nothing names a channel in the plot's bottom-left corner any more - the
    //      legend stack is gone and not orphaned, and every chip the model publishes
    //      is anchored at the plot's RIGHT edge (so not one of them can sit where the
    //      stack did, over the value numbers and the zero rule);
    //   2. a cramped viewport changes the LIST not at all - every row is still
    //      published, because the list is the application's own QMenu and a menu
    //      scrolls - while the geometry the panel DRAWS gives up rather than leaving
    //      the plot: a chip with no room is an empty rect;
    //   3. every chip the cramped model does publish is still inside the plot, so a
    //      box that exists is always a box the drawing layer can draw.
    {
        int chipsLeftOfCentre = 0;
        int chipsOutsidePlot = 0;
        for (const gui::PanelCurve& curve : model.curves) {
            if (curve.chip.isEmpty())
                continue;
            if (curve.chip.left() < model.plot.center().x())
                ++chipsLeftOfCentre;
            if (!model.plot.contains(curve.chip))
                ++chipsOutsidePlot;
        }
        std::printf("[8] full viewport: chips=%d  chips left of the plot's centre (where the legend was)=%d  "
                    "chips outside the plot=%d\n",
                    [&model] {
                        int n = 0;
                        for (const gui::PanelCurve& c : model.curves)
                            n += !c.chip.isEmpty();
                        return n;
                    }(), chipsLeftOfCentre, chipsOutsidePlot);
        CHECK(chipsLeftOfCentre == 0);
        CHECK(chipsOutsidePlot == 0);

        const QSize cramped(160, 48);
        const gui::GraphPanelModel crampedModel = gui::buildGraphPanelModel(editor, cramped, palette);

        int identityLost = 0;
        for (int i = 0; i < crampedModel.channels.size(); ++i) {
            const gui::PanelChannelRow& row = crampedModel.channels.at(i);
            const gui::PanelChannelRow& full = model.channels.at(i);
            if (row.node != full.node || row.channel != full.channel || row.name != full.name ||
                row.layer != full.layer || row.group != full.group || row.color != full.color ||
                row.visible != full.visible || row.focused != full.focused)
                ++identityLost; // a row the panel cannot DRAW is still the same row
        }

        int crampedChips = 0;
        int crampedEmptyChips = 0;
        int crampedOutsideChips = 0;
        int dishonestEmptyChips = 0;
        for (const gui::PanelCurve& curve : crampedModel.curves) {
            if (curve.chip.isEmpty()) {
                ++crampedEmptyChips;
                // An empty chip must be TRUTHFUL: the box would not have fitted the
                // plot. (A chip dropped for no reason would be a name the user can
                // never see on a panel that had room for it.)
                if (!curve.points.isEmpty()) {
                    const QFontMetrics metrics{QFont()};
                    const int width = metrics.horizontalAdvance(curve.chipText) + 12; // the model's own padding: 2 x 6
                    const int height = metrics.height();
                    const QRect box(
                        crampedModel.plot.right() + 1 - 8 - width, qRound(curve.points.last().y()) - height / 2,
                        width, height
                    );
                    if (crampedModel.plot.contains(box))
                        ++dishonestEmptyChips;
                }
                continue;
            }
            ++crampedChips;
            if (!crampedModel.plot.contains(curve.chip))
                ++crampedOutsideChips;
        }

        std::printf("[8] cramped viewport %dx%d -> plot (%d,%d,%dx%d)  channel rows=%d (identity mismatches=%d)  "
                    "published chips=%d  empty chips=%d (of those, ones that WOULD have fitted=%d)  chips outside "
                    "the plot=%d\n",
                    cramped.width(), cramped.height(), crampedModel.plot.x(), crampedModel.plot.y(),
                    crampedModel.plot.width(), crampedModel.plot.height(), isz(crampedModel.channels), identityLost,
                    crampedChips, crampedEmptyChips, dishonestEmptyChips, crampedOutsideChips);

        CHECK(!crampedModel.plot.isEmpty());
        CHECK(crampedModel.channels.size() == model.channels.size());
        CHECK(identityLost == 0);
        CHECK(crampedOutsideChips == 0);
        CHECK(dishonestEmptyChips == 0);

        // A plot too small for anything at all: every chip is empty. The viewport has
        // to leave a NON-EMPTY plot first: the label column's reserve is the value
        // format's own width (Scaler::axisLabelWidth(), 68px at the UI font rather
        // than the old 46), so the plot starts 22px further right; and the right
        // edge now reserves the scroll bars' 14px (Scaler::kScrollBarReserve: the
        // app's 12px lane + a 2px gap), so this fixture grew with them
        // (64 -> 80 -> 90 -> 94) to keep the premise - a plot that exists and is
        // narrower than 16px.
        const QSize tooNarrow(94, 200);
        const gui::GraphPanelModel narrowModel = gui::buildGraphPanelModel(editor, tooNarrow, palette);
        int narrowChips = 0;
        for (const gui::PanelCurve& curve : narrowModel.curves) {
            if (!curve.chip.isEmpty())
                ++narrowChips;
        }
        std::printf("[8] narrow viewport %dx%d -> plot width=%d  channel rows=%d  published chips=%d  "
                    "chip text=\"%s\"\n",
                    tooNarrow.width(), tooNarrow.height(), narrowModel.plot.width(), isz(narrowModel.channels),
                    narrowChips,
                    narrowModel.curves.isEmpty() ? "" : qUtf8Printable(narrowModel.curves.first().chipText));
        CHECK(narrowModel.plot.width() < 16); // no room for a 12px-chip-plus-text: see the header's CHIPS
        CHECK(narrowModel.channels.size() == model.channels.size());
        CHECK(narrowChips == 0);
    }

    // -------------------------------------------------------------------------
    // 9. the polyline is published unclipped: the drawing layer owns the clip
    {
        // The model hands the drawing layer the sampled curve mapped through the
        // scaler, point for point and in order, and it does NOT cut the line to the
        // plot - the header's CURVES states the clipping as the CONSUMER's own
        // obligation (the ImGui overlay pushes a clip rect to the plot, and the
        // QPainter renderer cut every segment against it). The value window is
        // narrowed here, through the editor's own scaler - the object a wheel zoom
        // moves - so the fixture is guaranteed to hold samples that leave the plot.
        const float windowMin = editor.scale().valueMin();
        const float windowMax = editor.scale().valueMax();
        editor.scale().setValueWindow(0.0f, 50.0f);

        ctrl::graph::Scaler zoomed = editor.scale();
        zoomed.setRect(panel);
        const gui::GraphPanelModel zoomedModel = gui::buildGraphPanelModel(editor, viewport, palette);

        editor.scale().setValueWindow(windowMin, windowMax); // as the editor was found

        int mismatchedCurves = 0;
        int totalPoints = 0;
        int outsideX = 0;
        int outsideY = 0;
        double worstDelta = 0.0;

        for (const gui::PanelCurve& curve : zoomedModel.curves) {
            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            const core::Frame first = zoomed.frameAt(zoomedModel.plot.left());
            const core::Frame last = zoomed.frameAt(zoomedModel.plot.right());
            const int segments = std::max(1, source.segmentCount());
            // The header's own sampling budget (the renderer's): one point per ~2px
            // of plot width per segment, floored and capped.
            const int samples = std::clamp(zoomedModel.plot.width() / (2 * segments), 2, 256);

            QVector<QPointF> polyline;
            source.buildPolyline(first, last, samples, polyline);

            bool same = (polyline.size() == curve.points.size());
            for (int i = 0; same && i < polyline.size(); ++i) {
                // Both axes through the SCALER's exact mapping: the curve's own coordinates are the
                // mapping's, not a rounded copy of it (a whole-pixel y is what makes a flat curve's
                // edges hard).
                const QPointF expected(
                    zoomed.pixelXOfFrame(polyline.at(i).x()), zoomed.pixelYOfValue(float(polyline.at(i).y()))
                );
                const double dx = std::fabs(expected.x() - curve.points.at(i).x());
                const double dy = std::fabs(expected.y() - curve.points.at(i).y());
                worstDelta = std::max(worstDelta, std::max(dx, dy));
                if (dx > 0.0 || dy > 0.0)
                    same = false; // a cut or dropped sample would show up right here
            }
            if (!same)
                ++mismatchedCurves;

            // A curve's own coordinates are the EXACT mapping's on both axes. The frame axis always
            // was (pixelXOfFrame); the value axis was rounded to whole pixels, which locks a flat
            // curve's edges to the pixel grid - a nearly-horizontal line then has no antialiasing at
            // all while a steep one, whose edge phase runs along x, stays smooth. This window's
            // mapping is fractional, so an integer y here means the rounding is back.
            {
                bool subPixelY = false;
                for (const QPointF& point : curve.points)
                    subPixelY = subPixelY || std::fabs(point.y() - std::floor(point.y())) > 1e-6;
                CHECK(subPixelY);
            }

            // The value HOLDS outside the keys, so every curve reaches the PLOT's edges: the whole X
            // range shows what playback plays. A polyline that stopped at its first and last key
            // would leave the rest of the plot empty (and a one-key channel would draw nothing).
            CHECK(curve.points.size() >= 2);
            CHECK(curve.points.first().x() <= zoomedModel.plot.left() + 1);
            CHECK(curve.points.last().x() >= zoomedModel.plot.right() - 1);

            for (const QPointF& point : curve.points) {
                ++totalPoints;
                if (point.x() < zoomedModel.plot.left() || double(zoomedModel.plot.right()) < point.x())
                    ++outsideX;
                if (point.y() < zoomedModel.plot.top() || double(zoomedModel.plot.bottom()) < point.y())
                    ++outsideY;
            }
        }

        std::printf("[9] window 0..50 (was %g..%g)  curves=%d  points=%d  curves != the sampled curve mapped through "
                    "the scaler=%d  worst coordinate delta=%.9f  samples outside the plot: x=%d y=%d\n",
                    double(windowMin), double(windowMax), isz(zoomedModel.curves), totalPoints, mismatchedCurves,
                    worstDelta, outsideX, outsideY);

        CHECK(!zoomedModel.curves.isEmpty());
        CHECK(totalPoints > 0);
        CHECK(mismatchedCurves == 0);
        // The fixture's premise, and the documented contract: samples the narrowed
        // window pushed out of the plot are published anyway.
        CHECK(outsideY > 0);
    }

    // -------------------------------------------------------------------------
    // 4b. the project's LOOP reaches the drawn polyline
    //
    // The model rebuilds its own Curve from each entry's (node, channel), so the editor's loop rule
    // has to travel with them: with the loop on, the frames outside the keys continue the easing from
    // the last key back to the first, and with it off they hold. The assertion is the DIFFERENCE -
    // a hold cannot make the extension vary.
    {
        project.attribute().setLoop(true);
        const gui::GraphPanelModel loopedModel = gui::buildGraphPanelModel(editor, viewport, palette);
        CHECK(loopedModel.projectLoop);
        CHECK(loopedModel.projectLoopRange == project.attribute().frameRange());

        int varying = 0;
        int held = 0;
        for (const gui::PanelCurve& curve : loopedModel.curves) {
            if (curve.points.size() < 3)
                continue;
            // The last key's own pixel column: everything past it is the extension.
            const double lastKeyX = curve.keys.isEmpty() ? 0.0 : double(curve.keys.last().right());
            double lo = 1e30;
            double hi = -1e30;
            for (const QPointF& point : curve.points) {
                if (point.x() < lastKeyX)
                    continue;
                lo = std::min(lo, point.y());
                hi = std::max(hi, point.y());
            }
            if (hi - lo > 0.5)
                ++varying;
            else if (hi > -1e30)
                ++held;
        }
        std::printf(
            "[4b] loop on: curves=%d extension varying=%d holding=%d range=%d..%d\n",
            isz(loopedModel.curves), varying, held, loopedModel.projectLoopRange.min(),
            loopedModel.projectLoopRange.max()
        );
        CHECK(varying > 0);

        project.attribute().setLoop(false);
        const gui::GraphPanelModel heldModel = gui::buildGraphPanelModel(editor, viewport, palette);
        CHECK(!heldModel.projectLoop);
    }

    // -------------------------------------------------------------------------
    // 5. hidden channels
    {
        const ctrl::graph::Channel hidden(core::TimeKeyType_Move, 1); // Move Y of target A
        editor.setChannelVisible(nodeA, hidden, false);
        const gui::GraphPanelModel hiddenModel = gui::buildGraphPanelModel(editor, viewport, palette);

        int rowIndex = -1;
        for (int i = 0; i < hiddenModel.channels.size(); ++i) {
            if (samePair(Pair(hiddenModel.channels[i].node, hiddenModel.channels[i].channel), &nodeA, hidden))
                rowIndex = i;
        }
        int curvesForHidden = 0;
        for (const gui::PanelCurve& curve : hiddenModel.curves) {
            if (curve.node == &nodeA && curve.channel == hidden)
                ++curvesForHidden;
        }

        std::printf("[5] after hiding Move/Y of A: channel rows=%d (row found=%d, visible=%d) curves=%d (was %d) "
                    "curves for the hidden pair=%d\n",
                    isz(hiddenModel.channels), rowIndex,
                    rowIndex >= 0 ? int(hiddenModel.channels[rowIndex].visible) : -1, isz(hiddenModel.curves),
                    isz(model.curves), curvesForHidden);

        CHECK(rowIndex >= 0);
        CHECK(hiddenModel.channels[rowIndex].visible == false);
        CHECK(curvesForHidden == 0);
        CHECK(hiddenModel.curves.size() == model.curves.size() - 1);
        CHECK(hiddenModel.channels.size() == model.channels.size());
        CHECK(hiddenModel.emptyState.isEmpty());

        editor.setChannelVisible(nodeA, hidden, true);
        const gui::GraphPanelModel restored = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[5] restored: curves=%d channel rows=%d\n", isz(restored.curves), isz(restored.channels));
        CHECK(restored.curves.size() == model.curves.size());
        CHECK(restored.channels.size() == model.channels.size());
    }

    // -------------------------------------------------------------------------
    // 6. empty state
    {
        editor.setTargets({});
        const gui::GraphPanelModel emptyModel = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[6] no targets: emptyState=\"%s\" curves=%d channel rows=%d isEmpty=%d\n",
                    qUtf8Printable(emptyModel.emptyState),
                    isz(emptyModel.curves), isz(emptyModel.channels), int(emptyModel.isEmpty()));

        CHECK(!emptyModel.emptyState.isEmpty());
        CHECK(emptyModel.curves.isEmpty());
        CHECK(emptyModel.channels.isEmpty());
        CHECK(emptyModel.isEmpty());
        CHECK(emptyModel.selectionCount == 0);

        editor.setTargets({ &nodeA, &nodeB });
        const gui::GraphPanelModel again = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[6] targets restored: emptyState=\"%s\" curves=%d\n", qUtf8Printable(again.emptyState),
                    isz(again.curves));
        CHECK(again.emptyState.isEmpty());
        CHECK(again.curves.size() == pairs.size());
    }

    // -------------------------------------------------------------------------
    // 7. playhead
    {
        animator.setFrame(core::Frame(37));
        const gui::GraphPanelModel moved = gui::buildGraphPanelModel(editor, viewport, palette);
        const int expectedX = scale.pixelX(editor.currentFrame());

        std::printf("[7] document frame=%d  editor.currentFrame()=%d  project.currentTimeInfo().frame=%d  "
                    "playheadX=%d  scale.pixelX(currentFrame)=%d\n",
                    animator.currentFrame().get(), editor.currentFrame().get(),
                    project.currentTimeInfo().frame.get(), moved.playheadX, expectedX);

        CHECK(editor.currentFrame().get() == 37);
        CHECK(project.currentTimeInfo().frame.get() == 37);
        CHECK(moved.playheadX == expectedX);
        CHECK(moved.playheadX == scale.pixelX(core::Frame(37)));
        CHECK(model.plot.left() <= moved.playheadX && moved.playheadX <= model.plot.right());

        // THE PILL'S TEXT IS THE RULER'S OWN STRING: the frame written with
        // core::TimeFormat::frameToString and the editor's own format setting - the
        // same call every frame label above is checked against, so the two views
        // cannot name one frame two ways.
        const core::TimeFormat format(editor.frameRange(), editor.fps());
        const QString expectedText = format.frameToString(37, editor.timeFormatType());
        std::printf(
            "[7] playheadText=\"%s\" expected=\"%s\"  playheadPill=(%d,%d %dx%d)  rectangleBand=(%d,%d %dx%d)"
            "  playheadLineTop=%d\n",
            qUtf8Printable(moved.playheadText), qUtf8Printable(expectedText), moved.playheadPill.left(),
            moved.playheadPill.top(), moved.playheadPill.width(), moved.playheadPill.height(),
            moved.rulerBand.left(), moved.rulerBand.top(), moved.rulerBand.width(), moved.rulerBand.height(),
            moved.playheadLineTop
        );
        CHECK(moved.playheadText == expectedText);
        CHECK(!moved.playheadPill.isEmpty());
        CHECK(moved.playheadPill.top() == moved.rulerBand.top() + gui::frameRuler::kPillTop);
        CHECK(moved.playheadPill.height() == gui::frameRuler::kNumberHeight);
        CHECK(moved.playheadPill.bottom() < moved.playheadLineTop);
        CHECK(moved.playheadLineTop == moved.rulerBand.top() + gui::frameRuler::kLineTop);
        // THE PLAYHEAD SPANS BOTH SECTIONS: its line begins in the ruler's pill column
        // (under the pill) and crosses the section boundary into the plot, which
        // starts BELOW the band. The drawing layer inks the band's rows outside the
        // curve clip and the plot's rows inside it; this is the geometry both halves
        // share.
        CHECK(moved.playheadLineTop > moved.rulerBand.top());
        CHECK(moved.playheadLineTop < moved.plot.top());
        // The band's hairline is the last band row + 1; the scaler's margin rows sit
        // between it and the plot (see the band alignment rule in block [1]).
        CHECK(moved.plot.top() == moved.rulerBand.bottom() + 1 + ctrl::graph::Scaler::kMargin);

        // ...AND NO RULER NUMBER IS DRAWN UNDER IT: every label whose centre falls
        // inside the pill's half-width is dropped, so the pill's number is the only
        // one read in its column (the timeline's own rule).
        int underPill = 0;
        for (const gui::PanelLabel& label : moved.frameLabels) {
            if (qAbs(label.rect.center().x() - moved.playheadX) < moved.playheadPill.width() / 2)
                ++underPill;
        }
        std::printf("[7] frame labels under the pill=%d\n", underPill);
        CHECK(underPill == 0);

        // No project: no frame to place, no pill, no text.
        editor.setProject(nullptr);
        const gui::GraphPanelModel orphaned = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[7] no project: editor.currentFrame()=%d  playheadX=%d  playheadText=\"%s\"  emptyState=\"%s\"\n",
                    editor.currentFrame().get(), orphaned.playheadX, qUtf8Printable(orphaned.playheadText),
                    qUtf8Printable(orphaned.emptyState));
        CHECK(editor.currentFrame().get() == -1);
        CHECK(orphaned.playheadX == -1);
        CHECK(orphaned.playheadText.isEmpty());
        CHECK(orphaned.playheadPill.isEmpty());
        CHECK(!orphaned.emptyState.isEmpty());
        CHECK(orphaned.curves.isEmpty());
    }

    // -------------------------------------------------------------------------
    // 16. THE ACTIVE-RANGE RECESSION AND THE TEMPORARY PLAYBACK WINDOW OVERLAY.
    // The timeline's ruler recesses every tick and number outside its ACTIVE range
    // (the project frame range, or the play window while it is active) and overlays
    // the window as a translucent band + bracket stems + label pills. The graph's
    // ruler is the same ruler, so the model must publish (a) inside/outside per tick
    // and per frame label - a CLASSIFICATION, no ink - and (b) the overlay's exact
    // geometry in the ruler band. The oracle is the editor's own scalar and
    // ctrl::TimeLineEditor's playbackRange rule.
    {
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });
        animator.setFrame(core::Frame(37));

        const core::TimeFormat timeFormat(editor.frameRange(), editor.fps());
        const QFontMetrics metrics{QFont()};

        // ---- (a) no window: the project frame range is the active range ----
        editor.setPlayWindow(ctrl::TimeLineEditor::PlayWindow());
        const gui::GraphPanelModel plain = gui::buildGraphPanelModel(editor, viewport, palette);
        const util::Range projectRange = editor.frameRange();
        CHECK(!plain.playWindow.active);
        CHECK(plain.playWindow.band.isEmpty());
        CHECK(plain.playWindow.startStem.isEmpty() && plain.playWindow.endStem.isEmpty());
        CHECK(plain.playWindow.startPill.isEmpty() && plain.playWindow.endPill.isEmpty());
        CHECK(plain.playWindow.mergedPill.isEmpty());

        CHECK(plain.frameTickInActiveRange.size() == plain.frameTickX.size());
        int plainOutsideTicks = 0;
        int plainTickMismatch = 0;
        for (int i = 0; i < plain.frameTickX.size(); ++i) {
            const bool expected = projectRange.contains(scale.frameAt(plain.frameTickX.at(i)).get());
            if (!expected)
                ++plainOutsideTicks;
            if (plain.frameTickInActiveRange.at(i) != expected)
                ++plainTickMismatch;
        }
        int plainLabelMismatch = 0;
        for (const gui::PanelLabel& label : plain.frameLabels) {
            const bool expected = label.endFrame >= 0
                ? projectRange.contains(label.frame) && projectRange.contains(label.endFrame)
                : projectRange.contains(label.frame);
            if (label.inActiveRange != expected)
                ++plainLabelMismatch;
        }
        std::printf("[16] no window: active=%d..%d ticks=%d outside=%d tick_mismatches=%d label_mismatches=%d\n",
                    projectRange.min(), projectRange.max(), isz(plain.frameTickX), plainOutsideTicks,
                    plainTickMismatch, plainLabelMismatch);
        CHECK(plainTickMismatch == 0);
        CHECK(plainLabelMismatch == 0);
        CHECK(plainOutsideTicks > 0); // the window of this viewport reaches outside 1..120

        // ---- (b) an active window: the overlay and a window-based active range ----
        ctrl::TimeLineEditor::PlayWindow window;
        window.set(10, 100);
        editor.setPlayWindow(window);
        const gui::GraphPanelModel withWindow = gui::buildGraphPanelModel(editor, viewport, palette);
        const int x0 = scale.pixelX(core::Frame(window.start));
        const int x1 = scale.pixelX(core::Frame(window.end));
        const int pillTop = withWindow.rulerBand.top() + gui::frameRuler::kPillTop;
        const int stemHeight = gui::frameRuler::kBandHeight - gui::frameRuler::kPillTop;
        std::printf(
            "[16] window %d..%d: x=%d..%d band=(%d,%d %dx%d) stems=(%d,%d) pills=(%d,%d %dx%d)+(%d,%d %dx%d) "
            "texts=\"%s\"/\"%s\"\n",
            window.start, window.end, x0, x1, withWindow.playWindow.band.left(),
            withWindow.playWindow.band.top(), withWindow.playWindow.band.width(),
            withWindow.playWindow.band.height(), withWindow.playWindow.startStem.left(),
            withWindow.playWindow.endStem.left(), withWindow.playWindow.startPill.left(),
            withWindow.playWindow.startPill.top(), withWindow.playWindow.startPill.width(),
            withWindow.playWindow.startPill.height(), withWindow.playWindow.endPill.left(),
            withWindow.playWindow.endPill.top(), withWindow.playWindow.endPill.width(),
            withWindow.playWindow.endPill.height(), qUtf8Printable(withWindow.playWindow.startText),
            qUtf8Printable(withWindow.playWindow.endText)
        );
        CHECK(withWindow.playWindow.active);
        // The band is INSIDE the ruler band and exactly the band's height.
        CHECK(withWindow.playWindow.band.top() == withWindow.rulerBand.top());
        CHECK(withWindow.playWindow.band.height() == gui::frameRuler::kBandHeight);
        CHECK(withWindow.playWindow.band.left() == std::max(x0, withWindow.rulerBand.left()));
        CHECK(withWindow.playWindow.band.right() == std::min(x1, withWindow.rulerBand.right()));
        // The two stems: 2px, from the label row to the band's last row, on the ticks
        // of the window's frames (the end bracket on frame `end`'s own tick).
        CHECK(withWindow.playWindow.startStem.left() == x0);
        CHECK(withWindow.playWindow.startStem.width() == 2);
        CHECK(withWindow.playWindow.startStem.top() == pillTop);
        CHECK(withWindow.playWindow.startStem.height() == stemHeight);
        CHECK(withWindow.playWindow.endStem.left() == x1 - 1);
        CHECK(withWindow.playWindow.endStem.width() == 2);
        CHECK(withWindow.playWindow.endStem.top() == pillTop);
        CHECK(withWindow.playWindow.endStem.height() == stemHeight);
        // The label arms ride the stems' inner faces with the ruler's own text.
        CHECK(withWindow.playWindow.startPill.left() == x0 + 2);
        CHECK(withWindow.playWindow.endPill.right() == x1 - 2);
        CHECK(withWindow.playWindow.startPill.top() == pillTop);
        CHECK(withWindow.playWindow.endPill.top() == pillTop);
        CHECK(withWindow.playWindow.startPill.height() == gui::frameRuler::kNumberHeight);
        CHECK(withWindow.playWindow.endPill.height() == gui::frameRuler::kNumberHeight);
        CHECK(withWindow.playWindow.startText == timeFormat.frameToString(window.start, editor.timeFormatType()));
        CHECK(withWindow.playWindow.endText == timeFormat.frameToString(window.end, editor.timeFormatType()));
        CHECK(withWindow.playWindow.mergedPill.isEmpty());
        CHECK(withWindow.playWindow.mergedText.isEmpty());

        // The active range is now the window: both vectors classify against it.
        CHECK(withWindow.frameTickInActiveRange.size() == withWindow.frameTickX.size());
        int windowOutsideTicks = 0;
        int windowInsideTicks = 0;
        int windowTickMismatch = 0;
        for (int i = 0; i < withWindow.frameTickX.size(); ++i) {
            const int frame = scale.frameAt(withWindow.frameTickX.at(i)).get();
            const bool expected = window.start <= frame && frame <= window.end;
            if (expected)
                ++windowInsideTicks;
            else
                ++windowOutsideTicks;
            if (withWindow.frameTickInActiveRange.at(i) != expected)
                ++windowTickMismatch;
        }
        int windowLabelMismatch = 0;
        for (const gui::PanelLabel& label : withWindow.frameLabels) {
            const bool expected = label.endFrame >= 0
                ? window.start <= label.frame && label.frame <= window.end && window.start <= label.endFrame
                    && label.endFrame <= window.end
                : window.start <= label.frame && label.frame <= window.end;
            if (label.inActiveRange != expected)
                ++windowLabelMismatch;
        }
        std::printf("[16] window active range: inside_ticks=%d outside_ticks=%d tick_mismatches=%d"
                    " label_mismatches=%d\n",
                    windowInsideTicks, windowOutsideTicks, windowTickMismatch, windowLabelMismatch);
        CHECK(windowTickMismatch == 0);
        CHECK(windowLabelMismatch == 0);
        CHECK(windowInsideTicks > 0);
        CHECK(windowOutsideTicks > 0);

        // ---- (c) a narrow window: the two arms collide into ONE centred label ----
        window.set(20, 21);
        editor.setPlayWindow(window);
        const gui::GraphPanelModel merged = gui::buildGraphPanelModel(editor, viewport, palette);
        const int mx0 = scale.pixelX(core::Frame(window.start));
        const int mx1 = scale.pixelX(core::Frame(window.end));
        const QString expectedMerged =
            timeFormat.frameToString(window.start, editor.timeFormatType()) + QString::fromUtf8("\u2013")
            + timeFormat.frameToString(window.end, editor.timeFormatType());
        const int expectedMergedWidth = metrics.horizontalAdvance(expectedMerged) + 2 * gui::frameRuler::kPillPadding;
        std::printf("[16] merged window %d..%d: x=%d..%d merged=(%d,%d %dx%d) text=\"%s\"\n", window.start,
                    window.end, mx0, mx1, merged.playWindow.mergedPill.left(), merged.playWindow.mergedPill.top(),
                    merged.playWindow.mergedPill.width(), merged.playWindow.mergedPill.height(),
                    qUtf8Printable(merged.playWindow.mergedText));
        CHECK(merged.playWindow.active);
        CHECK(!merged.playWindow.mergedPill.isEmpty());
        CHECK(merged.playWindow.mergedText == expectedMerged);
        CHECK(merged.playWindow.mergedPill.width() == expectedMergedWidth);
        CHECK(merged.playWindow.mergedPill.top() == merged.rulerBand.top() + gui::frameRuler::kPillTop);
        CHECK(merged.playWindow.mergedPill.height() == gui::frameRuler::kNumberHeight);
        CHECK(merged.playWindow.mergedPill.left() == (mx0 + mx1) / 2 - expectedMergedWidth / 2);
        CHECK(merged.playWindow.startPill.isEmpty() && merged.playWindow.endPill.isEmpty());
        // The brackets are still there: a merged label covers the arms, not the stems.
        CHECK(!merged.playWindow.startStem.isEmpty() && !merged.playWindow.endStem.isEmpty());

        // The editor goes back to its no-window state for the blocks after this one.
        editor.setPlayWindow(ctrl::TimeLineEditor::PlayWindow());
        CHECK(!editor.playWindow().active());
        CHECK(editor.activeRange() == editor.frameRange());
    }


    // -------------------------------------------------------------------------
    // 10. tangent handles: one PanelHandle pair per Custom segment, in segment
    // order, matching the editor's own grab mapping (Curve::handle through the
    // scaler) - so a grab at a published dot is the drag the editor performs.
    {
        // Block [7] left the editor with no project (the no-frame case); the
        // document and the targets come back here, because a model needs both.
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });

        // A Custom easing on the first Move key: its one segment (frame 0 -> 20)
        // then exposes two tangent handles, on BOTH Move components (x and y).
        util::Easing::Param custom;
        custom.type = util::Easing::Type_Custom;
        custom.cubicBezier.x1 = 0.4f;
        custom.cubicBezier.y1 = 0.1f;
        custom.cubicBezier.x2 = 0.6f;
        custom.cubicBezier.y2 = 0.9f;
        for (int component = 0; component < core::MoveKey::kComponents; ++component) {
            move0->data().easing(component) = custom;
        }

        const gui::GraphPanelModel handled = gui::buildGraphPanelModel(editor, viewport, palette);
        ctrl::graph::Scaler oracle = editor.scale();
        oracle.setRect(panel);

        int totalHandles = 0;
        int customSegments = 0;
        int badHandles = 0;
        int misplacedDots = 0;
        for (const gui::PanelCurve& curve : handled.curves) {
            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            const int segments = source.segmentCount();
            int expected = 0;
            for (int s = 0; s < segments; ++s) {
                if (source.isCustom(source.segment(s)))
                    expected += 2;
            }
            if (expected > 0)
                ++customSegments;
            totalHandles += curve.handles.size();
            if (curve.handles.size() != expected)
                ++badHandles;

            int index = 0;
            for (int s = 0; s < segments; ++s) {
                const ctrl::graph::Curve::Segment segment = source.segment(s);
                if (!source.isCustom(segment))
                    continue;
                const core::TimeKey* owners[2] = { segment.from, segment.to };
                const float ownerValues[2] = { segment.fromValue, segment.toValue };
                for (int h = 0; h < 2 && index < curve.handles.size(); ++h, ++index) {
                    const gui::PanelHandle& handle = curve.handles.at(index);
                    const QPointF expectedDot = ctrl::graph::handlePoint(oracle, segment, h);
                    const QPointF expectedBase(
                        oracle.pixelXOfFrame(double(owners[h]->frame())), double(oracle.pixelY(ownerValues[h]))
                    );
                    if (handle.segment != s || handle.index != h || handle.radius != ctrl::graph::kHandleRadius)
                        ++badHandles;
                    if (handle.dot != expectedDot || handle.base != expectedBase)
                        ++misplacedDots;
                }
            }
        }
        std::printf("[10] curves=%d with_custom_segments=%d handles=%d  handle mismatches=%d  misplaced dots/bases=%d\n",
                    isz(handled.curves), customSegments, totalHandles, badHandles, misplacedDots);
        // Move x and Move y both carry the one Custom segment (the easing lives on
        // the key, and a key shapes every component of its type at once).
        CHECK(customSegments == 2);
        CHECK(totalHandles == 4);
        CHECK(badHandles == 0);
        CHECK(misplacedDots == 0);
        // A STORED pair is not a ghost pair: `ghost` is the whole distinction, and a
        // Custom handle drawn as a derived (smaller, channel-hue) dot would claim the
        // document holds no tangent.
        int storedGhosts = 0;
        for (const gui::PanelCurve& curve : handled.curves) {
            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            for (const gui::PanelHandle& handle : curve.handles) {
                if (0 <= handle.segment && handle.segment < source.segmentCount() &&
                    source.isCustom(source.segment(handle.segment)) && handle.ghost)
                    ++storedGhosts;
            }
        }
        CHECK(storedGhosts == 0);

        // A curve with NO Custom and NO Auto segment publishes no handle at all: the
        // pairs are keyed to the easing, not to "a curve". (The other channels here
        // carry the default Linear easing, which exposes no control point to drag.)
        int nonHandleCurves = 0;
        int handlesOnNonHandleCurves = 0;
        for (const gui::PanelCurve& curve : handled.curves) {
            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            bool anyEditable = false;
            for (int s = 0; s < source.segmentCount(); ++s) {
                const ctrl::graph::Curve::Segment segment = source.segment(s);
                if (source.isCustom(segment) || !source.ghostHandles(*segment.from).isEmpty())
                    anyEditable = true;
            }
            if (!anyEditable) {
                ++nonHandleCurves;
                handlesOnNonHandleCurves += curve.handles.size();
            }
        }
        std::printf("[10] curves with no Custom and no Auto segment=%d, handles they published=%d\n",
                    nonHandleCurves, handlesOnNonHandleCurves);
        CHECK(nonHandleCurves > 0);
        CHECK(handlesOnNonHandleCurves == 0);

        // ---- and now the AUTO segment: the DERIVED pair, which the document does
        // NOT hold. It is published as ghost handles, and its x IS the third and two
        // thirds of the segment's own frame span (the design's automatic handle, the
        // whole reason the pair can be derived at all) while only the two y values
        // come from the key's neighbours. The dot goes through the SAME mapping a
        // stored one does - ctrl::graph::handlePointAt - so the two can never drift
        // apart, and a drag on a ghost dot would convert the key (the editor's own
        // job; the model only has to say WHICH kind of pair this is).
        util::Easing::Param automatic;
        automatic.type = util::Easing::Type_Auto;
        for (int component = 0; component < core::MoveKey::kComponents; ++component) {
            move0->data().easing(component) = automatic;
        }

        const gui::GraphPanelModel ghosted = gui::buildGraphPanelModel(editor, viewport, palette);
        ctrl::graph::Scaler ghostOracle = editor.scale();
        ghostOracle.setRect(panel);

        int ghostPairs = 0;
        int ghostHandles = 0;
        int badGhosts = 0;
        int badGhostX = 0;
        int ghostDotsOffMapping = 0;
        int nonGhostOnAuto = 0;
        for (const gui::PanelCurve& curve : ghosted.curves) {
            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            if (!source.isCustom(source.segment(0)) && source.ghostHandles(*source.segment(0).from).isEmpty())
                continue; // not one of the Auto channels
            if (curve.handles.size() != 2) {
                ++badGhosts;
                continue;
            }
            ++ghostPairs;
            const ctrl::graph::Curve::Segment segment = source.segment(0);
            const QVector<QPointF> derived = source.ghostHandles(*segment.from);
            const core::TimeKey* owners[2] = { segment.from, segment.to };
            for (int h = 0; h < 2 && h < curve.handles.size(); ++h) {
                const gui::PanelHandle& handle = curve.handles.at(h);
                ++ghostHandles;
                if (!handle.ghost || handle.segment != 0 || handle.index != h ||
                    handle.radius != ctrl::graph::kHandleRadius)
                    ++badGhosts;
                if (!handle.ghost)
                    ++nonGhostOnAuto;
                // The base is STILL the key the control belongs to: control 0 leaves
                // the from key, control 1 arrives at the to key.
                const QPointF expectedBase(
                    ghostOracle.pixelXOfFrame(double(owners[h]->frame())),
                    double(ghostOracle.pixelY(h == 0 ? segment.fromValue : segment.toValue))
                );
                if (handle.base != expectedBase)
                    ++badGhosts;
                // The derived x is the THIRD POINT of the span, exactly.
                const double expectedX = h == 0 ? 1.0 / 3.0 : 2.0 / 3.0;
                if (std::fabs(derived.at(h).x() - expectedX) > 1e-9)
                    ++badGhostX;
                if (std::fabs(handle.dot.x() - ghostOracle.pixelXOfFrame(
                        double(segment.from->frame()) + expectedX * double(segment.to->frame() - segment.from->frame())
                    )) > 0.5)
                    ++badGhostX;
                // ...and the dot is that normalized point through the shared mapping.
                if (handle.dot != ctrl::graph::handlePointAt(ghostOracle, segment, derived.at(h)))
                    ++ghostDotsOffMapping;
            }
        }
        std::printf("[10] Auto segments: ghost pairs=%d handles=%d  mismatches=%d  x off the third point=%d  "
                    "dots off handlePointAt=%d  stored-looking ghosts=%d\n",
                    ghostPairs, ghostHandles, badGhosts, badGhostX, ghostDotsOffMapping, nonGhostOnAuto);
        CHECK(ghostPairs == 2); // Move x and Move y again: one key, every component
        CHECK(ghostHandles == 4);
        CHECK(badGhosts == 0);
        CHECK(badGhostX == 0);
        CHECK(ghostDotsOffMapping == 0);
        CHECK(nonGhostOnAuto == 0);

        // Back to the Custom pair, so the blocks after this one measure the geometry
        // they were written against (the trace handle is the one thing that moved).
        for (int component = 0; component < core::MoveKey::kComponents; ++component) {
            move0->data().easing(component) = custom;
        }
        const gui::GraphPanelModel restoredHandles = gui::buildGraphPanelModel(editor, viewport, palette);
        int restoredGhosts = 0;
        for (const gui::PanelCurve& curve : restoredHandles.curves) {
            for (const gui::PanelHandle& handle : curve.handles)
                restoredGhosts += int(handle.ghost);
        }
        std::printf("[10] after restoring Custom: handles=%d  of them ghosts=%d\n",
                    [&restoredHandles] {
                        int n = 0;
                        for (const gui::PanelCurve& c : restoredHandles.curves)
                            n += isz(c.handles);
                        return n;
                    }(), restoredGhosts);
        CHECK(restoredGhosts == 0);
    }

    // -------------------------------------------------------------------------
    // 10b. AND ONLY THE DRAWING LAYER'S OWN ROW CARRIES THEM. The pairs exist to be
    // INKED, and the panel inks a curve's tangent handles for the FOCUSED channel
    // alone (with no focus, for every curve) - so a pair built for any other row is
    // work nobody reads, and half of that work is the expensive kind: an Auto
    // segment's pair is DERIVED by Curve::ghostHandles from the key's neighbours.
    // This pins the publication rule on the model's own rows (PanelChannelRow::
    // focused): with a focus, the focused row keeps its derived pair and every other
    // row publishes NO handle; clearing the focus puts them all back. Nothing else
    // moves - not the curves, not the keys.
    {
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });

        // The Auto easing again: its pair is the DERIVED one, so the rows whose
        // handles are dropped are exactly the rows that would have recomputed a
        // tangent nobody inks.
        util::Easing::Param automatic;
        automatic.type = util::Easing::Type_Auto;
        for (int component = 0; component < core::MoveKey::kComponents; ++component) {
            move0->data().easing(component) = automatic;
        }

        const gui::GraphPanelModel unfocused = gui::buildGraphPanelModel(editor, viewport, palette);
        int unfocusedPairs = 0;
        for (const gui::PanelCurve& curve : unfocused.curves)
            unfocusedPairs += isz(curve.handles);
        CHECK(unfocusedPairs == 4); // no focus: the two Auto rows publish their pairs (as in [10])

        const ctrl::graph::Channel moveX(core::TimeKeyType_Move, 0); // Move X of target A
        editor.setFocusedChannel(nodeA, moveX);
        CHECK(editor.hasFocus());

        const gui::GraphPanelModel focused = gui::buildGraphPanelModel(editor, viewport, palette);
        int focusedRows = 0;
        for (const gui::PanelChannelRow& row : focused.channels)
            focusedRows += int(row.focused);

        int focusedCurveHandles = 0;
        int focusedCurveGhosts = 0;
        int otherCurves = 0;
        int handlesOnOtherCurves = 0;
        for (const gui::PanelCurve& curve : focused.curves) {
            if (curve.node == &nodeA && curve.channel == moveX) {
                focusedCurveHandles = isz(curve.handles);
                for (const gui::PanelHandle& handle : curve.handles)
                    focusedCurveGhosts += int(handle.ghost);
                continue;
            }
            ++otherCurves;
            handlesOnOtherCurves += isz(curve.handles);
        }
        std::printf(
            "[10b] focus on A/Move-x: rows_focused=%d  focused curve handles=%d (ghosts=%d)  other curves=%d"
            "  handles they published=%d  (with no focus: %d handles over %d curves)\n",
            focusedRows, focusedCurveHandles, focusedCurveGhosts, otherCurves, handlesOnOtherCurves,
            unfocusedPairs, isz(focused.curves)
        );
        CHECK(focusedRows == 1);                // the focus is on the panel's own rows
        CHECK(otherCurves > 0);                 // the fixture's premise: there are other rows
        CHECK(focusedCurveHandles == 2);        // the row the layer inks keeps its pair...
        CHECK(focusedCurveGhosts == 2);
        CHECK(handlesOnOtherCurves == 0);       // ...and no other row pays for a pair nobody inks
        CHECK(focused.curves.size() == unfocused.curves.size()); // the curves themselves are untouched

        // Clearing the focus is the no-focus ink: every handle-bearing row publishes
        // again, and the count is exactly the one measured before the focus.
        editor.clearFocus();
        const gui::GraphPanelModel cleared = gui::buildGraphPanelModel(editor, viewport, palette);
        int clearedPairs = 0;
        for (const gui::PanelCurve& curve : cleared.curves)
            clearedPairs += isz(curve.handles);
        CHECK(!editor.hasFocus());
        CHECK(clearedPairs == unfocusedPairs);

        // Leave the document as section 10 left it: the trace handle is the only
        // thing this block moved.
        util::Easing::Param custom;
        custom.type = util::Easing::Type_Custom;
        custom.cubicBezier.x1 = 0.4f;
        custom.cubicBezier.y1 = 0.1f;
        custom.cubicBezier.x2 = 0.6f;
        custom.cubicBezier.y2 = 0.9f;
        for (int component = 0; component < core::MoveKey::kComponents; ++component) {
            move0->data().easing(component) = custom;
        }
    }

    // -------------------------------------------------------------------------
    // 11. the ruler's reserved band, and the live box-select marquee. Both are two
    // halves of one story the drawing layer cannot finish alone: the model RESERVES
    // the space a frame number is allowed to occupy, and it publishes the marquee a
    // drag is drawing. Neither is a rect the drawing layer may invent, so both are
    // pinned here on the editor's own numbers.
    {
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });
        const gui::GraphPanelModel ruled = gui::buildGraphPanelModel(editor, viewport, palette);

        // THE RULER'S BAND is the timeline's: a 22px strip at rulerBandTop whose
        // numbers sit in its top 14 rows and its 5px ticks rise from its last row. The
        // model must carry it, the band must sit ABOVE the plot (the scaler reserved
        // those rows, so the plot's own clip begins below them), and every
        // number's box must be in the band's number row - ABOVE the tick rows, as the
        // timeline draws them (the opposite of the old panel, which hung its numbers
        // below the ticks).
        const int kBandHeight = gui::frameRuler::kBandHeight;
        const int kTickLength = gui::frameRuler::kTickLength;
        int labels = 0;
        int insideBand = 0;
        int inNumberRow = 0;
        for (const gui::PanelLabel& label : ruled.frameLabels) {
            ++labels;
            if (ruled.rulerBand.contains(label.rect))
                ++insideBand;
            if (label.rect.top() == ruled.rulerBand.top() + gui::frameRuler::kNumberTop &&
                label.rect.bottom() < ruled.rulerBand.top() + kBandHeight - kTickLength)
                ++inNumberRow;
        }
        std::printf(
            "[11] plot=(%d,%d %dx%d) rulerBand=(%d,%d %dx%d) bandHeight=%d tickLength=%d frame_labels=%d"
            " inside_band=%d in_number_row=%d\n",
            ruled.plot.left(), ruled.plot.top(), ruled.plot.width(), ruled.plot.height(), ruled.rulerBand.left(),
            ruled.rulerBand.top(), ruled.rulerBand.width(), ruled.rulerBand.height(), kBandHeight, kTickLength,
            labels, insideBand, inNumberRow
        );
        // The precondition of the pixel gate's assertion 16 (which measures the same
        // rule on the drawn glyphs): the model has to carry labels at all, or the
        // reservation is unobservable. THE BAND IS ABOVE THE PLOT and the numbers are
        // inside the BAND (they used to be inside the plot): the plot's clip now
        // starts on the graph's first row, so a number that reached it would be cut.
        CHECK(!ruled.rulerBand.isEmpty());
        CHECK(ruled.rulerBand.top() == ruled.rulerBandTop);
        CHECK(ruled.rulerBand.top() == ruled.panel.top());
        // The band is as wide as the PANEL, not the plot: the frame mapping is the
        // timeline's, so the ruler covers the value column's top corner too.
        CHECK(ruled.rulerBand.left() == ruled.panel.left());
        CHECK(ruled.rulerBand.width() == ruled.panel.width());
        CHECK(ruled.rulerBand.height() == kBandHeight);
        CHECK(ruled.rulerBand.bottom() + 1 + ctrl::graph::Scaler::kMargin == ruled.plot.top());
        CHECK(!ruled.plot.intersects(ruled.rulerBand));
        // EVERY TICK IS THE SCALER'S PIXEL FOR ITS FRAME, and the ticks reach the
        // panel's own columns: the band is the ruler's whole width, so a tick at the
        // value column's top (x < plot.left()) is published, not dropped.
        int ticksLeftOfPlot = 0;
        int tickMismatch = 0;
        for (const int x : ruled.frameTickX) {
            if (scale.pixelX(scale.frameAt(x)) != x)
                ++tickMismatch;
            if (x < ruled.plot.left())
                ++ticksLeftOfPlot;
        }
        std::printf(
            "[11] ticks=%d  left of the plot=%d  pixel mismatches=%d\n", isz(ruled.frameTickX), ticksLeftOfPlot,
            tickMismatch
        );
        CHECK(tickMismatch == 0);
        CHECK(ruled.rulerBand.contains(ruled.valueColumn.left(), ruled.rulerBand.top()));
        CHECK(ruled.rulerBand.contains(ruled.valueColumn.right(), ruled.rulerBand.top()));
        CHECK(labels > 0);
        CHECK(insideBand == labels);
        CHECK(inNumberRow == labels);

        // No drag is live, so the marquee is EMPTY - which is the drawing layer's
        // whole signal to draw nothing.
        std::printf("[11] marquee with no drag live: (%d,%d %dx%d) empty=%d\n", ruled.marquee.left(),
                    ruled.marquee.top(), ruled.marquee.width(), ruled.marquee.height(), int(ruled.marquee.isEmpty()));
        CHECK(ruled.marquee.isEmpty());

        // AND WITH ONE LIVE: a press on empty plot space (no key, no handle - the
        // panel's own plot, far from every published key) that is dragged. The model
        // must publish the EDITOR's rectangle, so the gesture and the rectangle on
        // screen are one geometry rather than two derivations of one.
        core::CameraInfo camera;
        camera.setScreenSize(viewport);
        core::AbstractCursor cursor;
        const QPoint from(ruled.plot.center().x() + 80, ruled.plot.center().y() + 40);
        const QPoint to(from.x() + 60, from.y() + 50);
        CHECK(ruled.plot.contains(from) && ruled.plot.contains(to));
        QMouseEvent press(QEvent::MouseButtonPress, from, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, to, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        // The marquee is the editor's CTRL-press gesture (a plain press in the plot is
        // left to the viewport tools - GraphEditor::ownsCursor's own routing), and the
        // modifier is read from the editor, exactly as the widget feeds it.
        editor.setModifiers(Qt::ControlModifier);
        const bool pressed = cursor.setMousePress(&press, camera) && editor.updateCursor(camera, cursor);
        editor.setModifiers(Qt::NoModifier);
        const bool dragged = cursor.setMouseMove(&move, camera) && editor.updateCursor(camera, cursor);
        const gui::GraphPanelModel draggedModel = gui::buildGraphPanelModel(editor, viewport, palette);
        const QRect published = draggedModel.marquee;
        std::printf(
            "[11] press=%d dragged=%d editor_marquee=(%d,%d %dx%d) model_marquee=(%d,%d %dx%d) equal=%d"
            " inside_plot=%d\n",
            int(pressed), int(dragged), editor.marqueeRect().left(), editor.marqueeRect().top(),
            editor.marqueeRect().width(), editor.marqueeRect().height(), published.left(), published.top(),
            published.width(), published.height(), int(published == editor.marqueeRect()),
            int(ruled.plot.contains(published))
        );
        CHECK(pressed);
        CHECK(dragged);
        CHECK(!published.isEmpty());
        CHECK(published == editor.marqueeRect());
        CHECK(ruled.plot.contains(published));

        // The release ends the gesture, and the model goes back to publishing nothing.
        QMouseEvent release(
            QEvent::MouseButtonRelease, to, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier
        );
        cursor.setMouseRelease(&release, camera);
        editor.updateCursor(camera, cursor);
        const gui::GraphPanelModel afterRelease = gui::buildGraphPanelModel(editor, viewport, palette);
        CHECK(afterRelease.marquee.isEmpty());

        // ---- AND THE OCCLUDED TOP: a widget floating above the viewport ----
        // The project tab bar covers the panel's first rows. The ruler band is a
        // SECTION ABOVE THE GRAPH now, so the only two answers an occluder can get
        // are:
        //   * the same band, when its whole slot is still at or below the occluded
        //     line (the inset cannot push it UP, so the strip does not move); and
        //   * NO band, when the occluder reaches into its slot - the band is never
        //     pushed down into the plot, because the plot's rows are the graph's and
        //     the whole point of the section is that curve ink cannot enter the ruler.
        // In BOTH cases the plot does not move: it is the editor's hit-test geometry,
        // so the drawn key stays the same key a press grabs.
        const int shallowInset = ruled.rulerBand.top() - ruled.panel.top(); // reaches the band's first row at most
        const gui::GraphPanelModel shallow =
            gui::buildGraphPanelModel(editor, viewport, palette, shallowInset);
        std::printf(
            "[11] occluded_shallow inset=%d band=(%d,%d %dx%d) frame_labels=%d plot_moved=%d\n",
            shallowInset, shallow.rulerBand.left(), shallow.rulerBand.top(), shallow.rulerBand.width(),
            shallow.rulerBand.height(), isz(shallow.frameLabels), int(shallow.plot != ruled.plot)
        );
        CHECK(shallow.rulerBand == ruled.rulerBand);
        CHECK(shallow.rulerBandTop == ruled.rulerBandTop);
        CHECK(shallow.frameLabels.size() == ruled.frameLabels.size());
        CHECK(shallow.plot == ruled.plot); // the interactive geometry stays put

        // An inset that reaches INTO the band's slot hides the strip for this frame:
        // no band, no ruler numbers, and - the invariant - no ruler ink inside the
        // plot. The graph itself is untouched.
        const int kInset = 24;
        const gui::GraphPanelModel occluded = gui::buildGraphPanelModel(editor, viewport, palette, kInset);
        std::printf(
            "[11] occluded_top inset=%d rulerBandTop=%d plot.top()=%d band_empty=%d frame_labels=%d"
            " plot_moved=%d value_labels=%d\n",
            kInset, occluded.rulerBandTop, ruled.plot.top(), int(occluded.rulerBand.isEmpty()),
            isz(occluded.frameLabels), int(occluded.plot != ruled.plot), isz(occluded.valueLabels)
        );
        CHECK(occluded.rulerBand.isEmpty());          // the strip is hidden, not moved into the graph
        CHECK(occluded.rulerBandTop == ruled.plot.top());
        CHECK(occluded.plot == ruled.plot);           // the interactive geometry stays put
        CHECK(occluded.panel == ruled.panel);
        CHECK(occluded.curves.size() == ruled.curves.size());
        CHECK(occluded.playheadX == ruled.playheadX);
        CHECK(occluded.frameLabels.isEmpty());
        CHECK(occluded.valueLabels.size() == ruled.valueLabels.size());

        // An inset that swallows the whole panel has the same answer - there is still
        // no ruler ink inside the plot, and still no label under the widget.
        const gui::GraphPanelModel swallowed =
            gui::buildGraphPanelModel(editor, viewport, palette, ruled.plot.bottom() + 40);
        std::printf(
            "[11] occluded_all inset=%d rulerBandTop=%d plot.top()=%d band_empty=%d frame_labels=%d\n",
            ruled.plot.bottom() + 40, swallowed.rulerBandTop, ruled.plot.top(),
            int(swallowed.rulerBand.isEmpty()), isz(swallowed.frameLabels)
        );
        CHECK(swallowed.rulerBand.isEmpty());
        CHECK(swallowed.rulerBandTop == ruled.plot.top());
        CHECK(swallowed.frameLabels.isEmpty());
        CHECK(swallowed.plot == ruled.plot);
    }

    // -------------------------------------------------------------------------
    // 12. THE LABEL COLUMN IS A STABLE, FORMAT-DERIVED RESERVE, AND NO GESTURE
    // MOVES IT. Two things have to hold at once:
    //   (a) the column is a function of the value FORMAT and the UI font, not of
    //       the values currently visible - so it is the SAME width for every
    //       window, and the plot's left edge (and every frame coordinate in the
    //       panel) stays put while the user pans, wheels or adds a key. This is
    //       the defect the first version of this rule had: a reserve measured
    //       from the visible labels moved plot.left by 1px on a 40px pan, so the
    //       frame coordinate moved 41px instead of 40 (measured on this tree).
    //   (b) it still COVERS the labels: it is the widest label 'g'/4 can write,
    //       so no published label exceeds its box, and the format's own sweep
    //       (62px at the UI font) sits inside the ceiling, which is a
    //       panel-protection bound rather than a clipping rule.
    // The reserve rule itself is restated by reserveAxisLabelWidth() above - the
    // test's own format and font metrics, never the model's own numbers.
    {
        const QFontMetrics metrics{QFont()};
        const float savedMin = editor.scale().valueMin();
        const float savedMax = editor.scale().valueMax();
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });

        const int reserve = reserveAxisLabelWidth(metrics);

        struct Window {
            const char* what;
            float lo;
            float hi;
        };
        const Window windows[] = {
            { "the seed window", 0.0f, 1.0f },
            { "a wide value, 12345.6", 0.0f, 12345.6f },
            { "a small negative, -1234", -1234.0f, 0.0f },
            { "both float extremes", -std::numeric_limits<float>::max(), std::numeric_limits<float>::max() },
            { "a mid-size window", 1000.0f, 2000.0f },
            { "a narrow window", 0.4f, 0.6f },
        };
        const QSize viewports[] = { QSize(1280, 720), QSize(800, 480), QSize(320, 200) };

        int wrongReserve = 0;     // a column that is not the format's reserve
        int movedPlot = 0;        // a plot rect that changed with the window
        int movedColumn = 0;      // a column that changed with the window
        int tooNarrowBox = 0;     // a label box narrower than its own measured text
        int tooNarrowColumn = 0;  // a column that cannot hold a label plus its right inset
        int labels = 0;

        for (const QSize& vp : viewports) {
            // One reference geometry per viewport: the plot depends on the
            // viewport, but it must NOT depend on the value window.
            QRect plotRef;
            QRect columnRef;
            bool haveRef = false;

            for (const Window& window : windows) {
                editor.scale().setValueWindow(window.lo, window.hi);
                const gui::GraphPanelModel measuredModel = gui::buildGraphPanelModel(editor, vp, palette);
                if (!haveRef) {
                    plotRef = measuredModel.plot;
                    columnRef = measuredModel.valueColumn;
                    haveRef = true;
                }

                if (measuredModel.valueColumn.width() != reserve)
                    ++wrongReserve;
                if (measuredModel.plot != plotRef)
                    ++movedPlot;
                if (measuredModel.valueColumn != columnRef)
                    ++movedColumn;

                int widestShown = 0;
                QString widestShownText;
                for (const gui::PanelLabel& label : measuredModel.valueLabels) {
                    const int advance = metrics.horizontalAdvance(label.text);
                    ++labels;
                    if (advance > widestShown) {
                        widestShown = advance;
                        widestShownText = label.text;
                    }
                    if (label.rect.width() < advance)
                        ++tooNarrowBox;
                    if (measuredModel.valueColumn.width() < advance + ctrl::graph::Scaler::kAxisLabelPad)
                        ++tooNarrowColumn;
                }
                std::printf(
                    "[12] viewport %4dx%-4d  window %-24s [%g, %g]  column=%3d reserve=%3d  widest label "
                    "\"%s\"=%2dpx box=%2dpx  plot=(%d,%d %dx%d)\n",
                    vp.width(), vp.height(), window.what, double(window.lo), double(window.hi),
                    measuredModel.valueColumn.width(), reserve, qUtf8Printable(widestShownText), widestShown,
                    measuredModel.valueLabels.isEmpty() ? -1 : measuredModel.valueLabels.first().rect.width(),
                    measuredModel.plot.x(), measuredModel.plot.y(), measuredModel.plot.width(),
                    measuredModel.plot.height()
                );
            }
        }
        editor.scale().setValueWindow(savedMin, savedMax); // as the editor was found

        std::printf(
            "[12] labels=%d  boxes narrower than their text=%d  columns too narrow for a label+inset=%d  "
            "columns != the format's reserve (%d)=%d  plots moved by a window change=%d  columns moved=%d\n",
            labels, tooNarrowBox, tooNarrowColumn, reserve, wrongReserve, movedPlot, movedColumn
        );
        CHECK(labels > 0);
        CHECK(wrongReserve == 0);
        CHECK(movedPlot == 0);
        CHECK(movedColumn == 0);
        CHECK(tooNarrowBox == 0);
        CHECK(tooNarrowColumn == 0);

        // ---- THE GESTURES A USER ACTUALLY MAKES, on the editor's own scaler ----
        // A wheel step on the value axis and a value PAN both change the value
        // window (that is what they are for) and must leave the panel's chrome
        // exactly where it was: the plot rect byte-identical to the reference.
        // The column is what makes that possible - a window-measured reserve made
        // the plot's left edge follow the numbers.
        const QSize vp = viewports[0];
        const gui::GraphPanelModel reference = gui::buildGraphPanelModel(editor, vp, palette);
        const QRect plotRef = reference.plot;
        const QRect columnRef = reference.valueColumn;

        const int anchorY = plotRef.center().y();
        const bool zoomed = editor.scale().updateValueZoom(1.25f, anchorY);
        const gui::GraphPanelModel afterZoom = gui::buildGraphPanelModel(editor, vp, palette);
        editor.scale().panValue(40);
        const gui::GraphPanelModel afterPan = gui::buildGraphPanelModel(editor, vp, palette);
        editor.scale().setValueWindow(savedMin, savedMax);

        // ...and a FRAME pan moves a frame's pixel by EXACTLY the pan delta: no
        // pixel of it may be eaten by a column that re-measured on the way.
        const int framePixelBefore = editor.scale().pixelX(core::Frame(0));
        editor.scale().panFrame(40);
        const int framePixelAfter = editor.scale().pixelX(core::Frame(0));
        editor.scale().panFrame(-40);
        const gui::GraphPanelModel afterFramePan = gui::buildGraphPanelModel(editor, vp, palette);

        std::printf(
            "[12] wheel step (anchor y=%d) applied=%d  pan 40px:  plot before=(%d,%d %dx%d) after_zoom=(%d,%d %dx%d) "
            "after_pan=(%d,%d %dx%d)  column before=%d after_zoom=%d after_pan=%d  px(0) frame pan: %d -> %d "
            "(delta %d, expected 40)\n",
            anchorY, int(zoomed), plotRef.x(), plotRef.y(), plotRef.width(), plotRef.height(), afterZoom.plot.x(),
            afterZoom.plot.y(), afterZoom.plot.width(), afterZoom.plot.height(), afterPan.plot.x(), afterPan.plot.y(),
            afterPan.plot.width(), afterPan.plot.height(), columnRef.width(), afterZoom.valueColumn.width(),
            afterPan.valueColumn.width(), framePixelBefore, framePixelAfter, framePixelAfter - framePixelBefore
        );
        CHECK(zoomed); // the fixture's premise: there was a window left to zoom
        CHECK(afterZoom.plot == plotRef);
        CHECK(afterPan.plot == plotRef);
        CHECK(afterFramePan.plot == plotRef);
        CHECK(afterZoom.valueColumn == columnRef);
        CHECK(afterPan.valueColumn == columnRef);
        CHECK(framePixelAfter - framePixelBefore == 40);
        CHECK(editor.scale().pixelX(core::Frame(0)) == framePixelBefore); // panned back

        // The reserve is inside the ceiling and covers the format's widest label:
        // the clamp is panel protection, NOT a rule that clips numbers, and this
        // is the number that keeps it one.
        int widestExtreme = 0;
        QString widestText;
        const float extremes[] = {
            -std::numeric_limits<float>::max(),
            -0.0009999f,
            -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::denorm_min(),
            12345.6f,
            100000.0f,
            -100000.0f,
            -1234.0f,
        };
        for (const float value : extremes) {
            const QString text = valueTextOf(value);
            const int advance = metrics.horizontalAdvance(text);
            if (advance > widestExtreme) {
                widestExtreme = advance;
                widestText = text;
            }
        }
        std::printf(
            "[12] reserve=%d = widest 'g'/4 label \"%s\" %dpx + inset %d + gap %d  ceiling=%d (cleared by %d)  "
            "floor=%d\n",
            reserve, qUtf8Printable(widestText), widestExtreme, ctrl::graph::Scaler::kAxisLabelPad,
            ctrl::graph::Scaler::kAxisLabelTextPad, ctrl::graph::Scaler::kMaxAxisLabelWidth,
            ctrl::graph::Scaler::kMaxAxisLabelWidth - reserve, ctrl::graph::Scaler::kAxisLabelWidth
        );
        CHECK(widestExtreme > 0);
        CHECK(reserve >= widestExtreme + ctrl::graph::Scaler::kAxisLabelPad);
        CHECK(reserve <= ctrl::graph::Scaler::kMaxAxisLabelWidth);
        // The reserve IS the measurement: pinning the column to the old 46px
        // reserve leaves 42px of box for a 62px label - this fails then.
        CHECK(ctrl::graph::Scaler::kAxisLabelWidth < reserve);
    }

    // -------------------------------------------------------------------------
    // 13. THE KEY MARKER IS A CIRCLE, AND THE HIT GEOMETRY IS A SEPARATE, LARGER
    // THING. The drawn marker is a circle of radius ctrl::graph::kKeyDotRadius
    // centred on the key's mapped point - and that radius is the APPLICATION'S
    // OWN key marker, the timeline's (ctrl::keyRadius = 3.0 * ctrl::radiiFactor =
    // 3.75px, src/ctrl/time/time_Renderer.h, the radius drawKeys() paints a key
    // body with). The rects in `keys` stay: they are the editor's hit geometry,
    // a different size on purpose.
    {
        // The tie to the timeline, as a compile-time identity rather than a
        // comment: this fails to build if either side moves.
        static_assert(
            ctrl::graph::kKeyDotRadius == ctrl::keyRadius,
            "the panel's key marker radius must be the timeline's own key-body radius "
            "(ctrl::keyRadius = 3.0 * ctrl::radiiFactor, src/ctrl/time/time_Renderer.h)"
        );
        CHECK(ctrl::graph::kKeyDotRadius < double(ctrl::graph::kKeyHitRadius)); // drawn small, grabbed large
        CHECK(ctrl::graph::kHandleRadius < ctrl::graph::kHandleHitRadius);      // ...for a handle too
        CHECK(ctrl::graph::kKeyDotRadius < double(ctrl::graph::kKeyRadius));    // the marker is not the hit square

        const gui::GraphPanelModel dotted = gui::buildGraphPanelModel(editor, viewport, palette);
        // A fresh copy of the editor's own axes: earlier blocks moved the value
        // window and the targets, and the key mapping under test is the CURRENT
        // one (block [10] refreshes its oracle for the same reason).
        ctrl::graph::Scaler oracle = editor.scale();
        oracle.setRect(panel);
        int keys = 0;
        int dots = 0;
        int badCounts = 0;
        int badCentres = 0;
        int offHitCentre = 0;
        int outsideOwnRect = 0;
        for (const gui::PanelCurve& curve : dotted.curves) {
            if (curve.keyDots.size() != curve.keys.size() || curve.keyDots.size() != curve.keyFrames.size())
                ++badCounts;

            const ctrl::graph::Curve source(*curve.node->timeLine(), curve.channel);
            int published = 0;
            for (const core::TimeKey* key : source.keys()) {
                const core::Frame frame(key->frame());
                const float value = source.valueOf(*key);
                const QRect hit = ctrl::graph::keyRect(oracle, frame, value);
                if (hit.intersected(dotted.plot).isEmpty())
                    continue; // entirely outside the plot: no rect published, so no dot
                if (published >= curve.keyDots.size()) {
                    ++badCounts;
                    break;
                }

                const QPointF dot = curve.keyDots.at(published);
                ++dots;
                // The dot is the key's mapped centre, derived from the scaler
                // rather than from the model's own call to keyDot().
                if (dot != QPointF(oracle.pixelX(frame), oracle.pixelY(value)))
                    ++badCentres;
                // ...and it is concentric with the HIT square: QRectF's centre is
                // the geometric one (left + width/2), so an uncut rect's centre is
                // exactly the key's point.
                if (QRectF(hit).center() != dot)
                    ++offHitCentre;
                if (curve.keys.at(published) == hit) {
                    // The rect is uncut, so the marker it belongs to must sit
                    // inside the geometry the editor picks with - a click on the
                    // dot's own pixel lands on that key.
                    if (!curve.keys.at(published).contains(dot.toPoint()))
                        ++outsideOwnRect;
                }
                ++published;
            }
            if (published != curve.keyDots.size())
                ++badCounts;
            keys += curve.keys.size();
        }

        std::printf(
            "[13] key rects=%d  key dots=%d  radius counts mismatched=%d  dots off the key's own pixel=%d  "
            "dots not concentric with the hit square=%d  uncut dots outside their own square=%d  "
            "kKeyDotRadius=%.2f (timeline ctrl::keyRadius=%.2f)  kKeyHitRadius=%d  kKeyRadius=%d\n",
            keys, dots, badCounts, badCentres, offHitCentre, outsideOwnRect, ctrl::graph::kKeyDotRadius,
            ctrl::keyRadius, ctrl::graph::kKeyHitRadius, ctrl::graph::kKeyRadius
        );
        CHECK(keys > 0);
        CHECK(dots == keys);
        CHECK(badCounts == 0);
        CHECK(badCentres == 0);
        CHECK(offHitCentre == 0);
        CHECK(outsideOwnRect == 0);
    }

    // -------------------------------------------------------------------------
    // 14. THE SELECTION COUNT, which the model still publishes and nothing draws.
    // Its one consumer now is the panel's accessibility summary ("N keys selected",
    // built by gui::GraphPanelFrame from this field), so what is pinned here is that
    // the field is the EDITOR's own number and follows the selection: the model is
    // read against editor.selectionCount(), the file's usual oracle, with nothing
    // selected, with one key selected, and after the selection is cleared again.
    {
        // Nothing before this block selects a key, and that is stated rather than
        // assumed: "the count is 0" is only an assertion if nothing is selected.
        editor.clearSelection();
        const gui::GraphPanelModel none = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[14] no selection: count=%d\n", none.selectionCount);
        CHECK(none.selectionCount == 0);

        // ONE key: the Move X key at frame 0 of target A, selected by the panel's own
        // rule (a key is named by node, type and frame - the timeline's triple).
        const ctrl::graph::Channel moveX(core::TimeKeyType_Move, 0);
        editor.selectKey(nodeA, moveX, core::Frame(0));
        const gui::GraphPanelModel one = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[14] one key selected: count=%d (editor states %d)\n", one.selectionCount,
                    editor.selectionCount());
        CHECK(one.selectionCount == editor.selectionCount());
        CHECK(one.selectionCount == 1);

        editor.clearSelection();
        const gui::GraphPanelModel cleared = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[14] cleared again: count=%d\n", cleared.selectionCount);
        CHECK(cleared.selectionCount == 0);
    }

    // -------------------------------------------------------------------------
    // 15. THE MARKER STATE: which marker is selected, and whether the key it belongs
    // to has a selected SIBLING. The drawing layer's three marker states are drawn
    // from exactly these two flags (see gui::drawPanel's marker pass), and the pair is
    // the design's "one key, two channels, both in play" rule: selecting ONE component
    // of a two-component key leaves the other component's marker unselected - its curve
    // still at full strength, its marker ringed faintly - and never selected.
    //
    // The editor is the oracle and the model is read: every flag below is compared
    // against editor.isMarkerSelected/hasSelectedSiblingMarker asked on the key the
    // model published (node, channel type and component, frame), and the two state
    // vectors are asserted parallel to the geometry they classify.
    {
        // THE VIEW, set to a known window. Blocks [11] and [12] drive the editor's own
        // scaler (a wheel zoom, pans, cursor gestures), and this block needs every key of
        // BOTH components of the Move key inside the plot at once - the two markers of one
        // key are what the sibling flag is about. The editor's own fit is the way back to
        // a window that covers the keys (nothing here sets a window by hand); the panel
        // rect goes into the scaler first, because a fit maps into the scaler's PLOT and
        // this file's own panel rect is the geometry every other block uses.
        editor.setTargets({ &nodeA, &nodeB });
        editor.scale().setRect(panel);
        editor.clearSelection();
        editor.fitAll();

        // One curve of the model, by the identity a channel is named by, or nullptr.
        const auto curveFor = [](const gui::GraphPanelModel& aModel, core::ObjectNode* aNode,
                                 const ctrl::graph::Channel& aChannel) -> const gui::PanelCurve* {
            for (const gui::PanelCurve& curve : aModel.curves) {
                if (curve.node == aNode && curve.channel == aChannel)
                    return &curve;
            }
            return nullptr;
        };
        // One flag of the key at aFrame, with aFound false when the model publishes no
        // marker for that key at all (it is outside the plot's window) - the honest
        // answer, and the one that keeps a malformed index from being read as a state.
        const auto flagOf = [](const QVector<bool>& aFlags, const gui::PanelCurve& aCurve, core::Frame aFrame,
                               bool& aFound) {
            const int i = aCurve.keyFrames.indexOf(aFrame);
            aFound = 0 <= i && i < aFlags.size();
            return aFound && aFlags.at(i);
        };
        // One curve's whole state, key by key: 'S' selected, 's' the SIBLING state
        // (not selected, its key selected on another component), '.' neither - the three
        // states the drawing layer draws, composed exactly as its marker pass composes
        // them from the two published flags.
        const auto statesOf = [](const gui::PanelCurve& aCurve) {
            QString out;
            for (int i = 0; i < aCurve.keySelected.size() && i < aCurve.keySiblingSelected.size(); ++i) {
                out += aCurve.keySelected.at(i) ? QLatin1Char('S')
                                                : aCurve.keySiblingSelected.at(i) ? QLatin1Char('s')
                                                                                  : QLatin1Char('.');
            }
            return out;
        };
        // The model against the editor, cell for cell, over every curve it published.
        struct MarkerTally {
            int keys = 0;           // markers the model classified
            int wider = 0;          // curves whose state vectors are not parallel to the geometry
            int wrongSelected = 0;  // cells where the model and the editor disagree
            int wrongSibling = 0;
        };
        const auto tally = [](const gui::GraphPanelModel& aModel, ctrl::GraphEditor& aEditor) {
            MarkerTally out;
            for (const gui::PanelCurve& curve : aModel.curves) {
                if (curve.keySelected.size() != curve.keyDots.size()
                    || curve.keySiblingSelected.size() != curve.keyDots.size()
                    || curve.keyDots.size() != curve.keyFrames.size()) {
                    ++out.wider;
                }
                for (int i = 0; i < curve.keyFrames.size(); ++i) {
                    const core::Frame frame = curve.keyFrames.at(i);
                    const bool selected = aEditor.isMarkerSelected(*curve.node, curve.channel, frame);
                    const bool sibling = aEditor.hasSelectedSiblingMarker(*curve.node, curve.channel, frame);
                    ++out.keys;
                    if (i >= curve.keySelected.size() || curve.keySelected.at(i) != selected)
                        ++out.wrongSelected;
                    if (i >= curve.keySiblingSelected.size() || curve.keySiblingSelected.at(i) != sibling)
                        ++out.wrongSibling;
                }
            }
            return out;
        };
        // How many cells a model flags at all, either way round: the "nothing is flagged"
        // half of the pair below.
        const auto flaggedCount = [](const gui::GraphPanelModel& aModel) {
            int on = 0;
            for (const gui::PanelCurve& curve : aModel.curves) {
                for (const bool flag : curve.keySelected)
                    on += flag ? 1 : 0;
                for (const bool flag : curve.keySiblingSelected)
                    on += flag ? 1 : 0;
            }
            return on;
        };

        const ctrl::graph::Channel moveX(core::TimeKeyType_Move, 0);
        const ctrl::graph::Channel moveY(core::TimeKeyType_Move, 1);
        const ctrl::graph::Channel rotate(core::TimeKeyType_Rotate, 0);
        const ctrl::graph::Channel opa(core::TimeKeyType_Opa, 0);

        // ---- NOTHING SELECTED: neither flag on any marker of any curve.
        const gui::GraphPanelModel none = gui::buildGraphPanelModel(editor, viewport, palette);
        const MarkerTally noneTally = tally(none, editor);
        std::printf(
            "[15] no selection: markers=%d parallel_mismatches=%d flag_mismatches=%d/%d flagged=%d\n",
            noneTally.keys, noneTally.wider, noneTally.wrongSelected, noneTally.wrongSibling,
            flaggedCount(none)
        );
        CHECK(noneTally.keys > 0); // the fixture's premise: there are markers to classify
        CHECK(noneTally.wider == 0);
        CHECK(noneTally.wrongSelected == 0);
        CHECK(noneTally.wrongSibling == 0);
        CHECK(flaggedCount(none) == 0); // and that is what the assertion is about

        // ---- ONE MARKER of a TWO-component key: Move X at frame 0. The other component
        // of the same key is NOT selected and carries the sibling flag; the key at frame
        // 20 of that same type, and the other target's key, are neither.
        CHECK(editor.selectMarker(nodeA, moveX, core::Frame(0)));
        const gui::GraphPanelModel one = gui::buildGraphPanelModel(editor, viewport, palette);
        const MarkerTally oneTally = tally(one, editor);
        const gui::PanelCurve* oneX = curveFor(one, &nodeA, moveX);
        const gui::PanelCurve* oneY = curveFor(one, &nodeA, moveY);
        const gui::PanelCurve* oneRotate = curveFor(one, &nodeA, rotate);
        const gui::PanelCurve* oneOpa = curveFor(one, &nodeB, opa);
        bool x0 = false;
        bool y0 = false;
        bool x20 = false;
        bool y20 = false;
        const bool oneX0 = oneX && flagOf(oneX->keySelected, *oneX, core::Frame(0), x0);
        const bool oneY0 = oneY && flagOf(oneY->keySelected, *oneY, core::Frame(0), y0);
        const bool oneXSib0 = oneX && flagOf(oneX->keySiblingSelected, *oneX, core::Frame(0), x0);
        const bool oneYSib0 = oneY && flagOf(oneY->keySiblingSelected, *oneY, core::Frame(0), y0);
        const bool oneX20 = oneX && flagOf(oneX->keySelected, *oneX, core::Frame(20), x20);
        const bool oneY20 = oneY && flagOf(oneY->keySelected, *oneY, core::Frame(20), y20);
        const bool oneXSib20 = oneX && flagOf(oneX->keySiblingSelected, *oneX, core::Frame(20), x20);
        const bool oneYSib20 = oneY && flagOf(oneY->keySiblingSelected, *oneY, core::Frame(20), y20);
        std::printf(
            "[15] Move X at frame 0 selected: markers=%d parallel_mismatches=%d flag_mismatches=%d/%d"
            "  Move X=[%s]  Move Y=[%s]  Rotate=[%s]  Opa=[%s]  (frame0 published X/Y=%d/%d,"
            " frame20 X/Y=%d/%d)\n",
            oneTally.keys, oneTally.wider, oneTally.wrongSelected, oneTally.wrongSibling,
            oneX ? qUtf8Printable(statesOf(*oneX)) : "<missing>",
            oneY ? qUtf8Printable(statesOf(*oneY)) : "<missing>",
            oneRotate ? qUtf8Printable(statesOf(*oneRotate)) : "<missing>",
            oneOpa ? qUtf8Printable(statesOf(*oneOpa)) : "<missing>", int(x0), int(y0), int(x20), int(y20)
        );
        CHECK(oneX && oneY && oneRotate && oneOpa); // the model draws every channel of both targets
        CHECK(oneTally.wider == 0);
        CHECK(oneTally.wrongSelected == 0); // the model IS the editor's answer, cell for cell
        CHECK(oneTally.wrongSibling == 0);
        CHECK(x0 && y0 && x20 && y20); // every Move key is inside the fitted window
        // The shape of that answer, stated on the two components of one key:
        CHECK(oneX0);        // the marker the press named is selected
        CHECK(!oneXSib0);    // ... with no sibling of its own
        CHECK(!oneY0);       // the sibling NEVER takes the selection
        CHECK(oneYSib0);     // ... and it is exactly what the sibling flag is for
        // The second key of the same type is nobody's: neither of its components.
        CHECK(!oneX20 && !oneXSib20);
        CHECK(!oneY20 && !oneYSib20);
        // A ONE-component key can have no sibling at all (the engine's own rule: another
        // COMPONENT of the same key, which Rotate has none of), and a Move selection does
        // not reach another target's Opa key either.
        CHECK(oneRotate->keySiblingSelected.count(true) == 0);
        CHECK(oneOpa->keySelected.count(true) == 0 && oneOpa->keySiblingSelected.count(true) == 0);
        CHECK(flaggedCount(one) == 2); // exactly the selected marker and its one sibling

        // ---- THE WHOLE KEY: the timeline's own gesture (and the panel's marquee and
        // ctrl+A) selects every component of a key, so BOTH markers are selected. The
        // engine's sibling query is the RAW one - an entry naming a different marker
        // counts, and a key-level entry names all of them - so both report a sibling too;
        // the drawing layer reaches the faint-ring state by ANDing the two flags
        // (`!selected && sibling`), which is false here. That composition is why the flags
        // are published as the engine states them and not pre-combined.
        CHECK(editor.selectKey(nodeA, moveX, core::Frame(0)));
        const gui::GraphPanelModel whole = gui::buildGraphPanelModel(editor, viewport, palette);
        const gui::PanelCurve* wholeX = curveFor(whole, &nodeA, moveX);
        const gui::PanelCurve* wholeY = curveFor(whole, &nodeA, moveY);
        std::printf(
            "[15] whole key selected: Move X=[%s]  Move Y=[%s]  editor answers X=%d/%d  Y=%d/%d"
            "  (selected/sibling)  faint_ring_cells=%d\n",
            wholeX ? qUtf8Printable(statesOf(*wholeX)) : "<missing>",
            wholeY ? qUtf8Printable(statesOf(*wholeY)) : "<missing>",
            int(editor.isMarkerSelected(nodeA, moveX, core::Frame(0))),
            int(editor.hasSelectedSiblingMarker(nodeA, moveX, core::Frame(0))),
            int(editor.isMarkerSelected(nodeA, moveY, core::Frame(0))),
            int(editor.hasSelectedSiblingMarker(nodeA, moveY, core::Frame(0))),
            [&whole] {
                int faint = 0;
                for (const gui::PanelCurve& curve : whole.curves) {
                    for (int i = 0; i < curve.keySelected.size() && i < curve.keySiblingSelected.size(); ++i) {
                        if (!curve.keySelected.at(i) && curve.keySiblingSelected.at(i))
                            ++faint;
                    }
                }
                return faint;
            }()
        );
        CHECK(wholeX && wholeY);
        bool wx0 = false;
        bool wy0 = false;
        CHECK(wholeX && flagOf(wholeX->keySelected, *wholeX, core::Frame(0), wx0));
        CHECK(wholeY && flagOf(wholeY->keySelected, *wholeY, core::Frame(0), wy0));
        CHECK(wx0 && wy0); // one key lights BOTH its markers
        CHECK(!wholeX->keySiblingSelected.isEmpty() && !wholeY->keySiblingSelected.isEmpty());
        CHECK(tally(whole, editor).wrongSelected == 0);

        // ---- AND CLEARED AGAIN: the flags follow the selection rather than latch it.
        editor.clearSelection();
        const gui::GraphPanelModel cleared = gui::buildGraphPanelModel(editor, viewport, palette);
        const MarkerTally clearedTally = tally(cleared, editor);
        std::printf(
            "[15] cleared again: parallel_mismatches=%d flag_mismatches=%d/%d flagged=%d\n", clearedTally.wider,
            clearedTally.wrongSelected, clearedTally.wrongSibling, flaggedCount(cleared)
        );
        CHECK(clearedTally.wider == 0);
        CHECK(clearedTally.wrongSelected == 0);
        CHECK(clearedTally.wrongSibling == 0);
        CHECK(flaggedCount(cleared) == 0);

        // The editor is left as the blocks before this one left it: no selection, the
        // two targets, the panel's own rect in the scaler.
        editor.clearSelection();
    }

    // -------------------------------------------------------------------------
    // 16: THE SCROLL BARS - the four rects the panel publishes, and what a thumb's
    // LENGTH means.
    //
    // The bars are chrome the user DRAGS: the horizontal one along the plot's bottom
    // edge moves the frame axis' window (the timeline's own window while a timeline
    // owns that axis), the vertical one along the plot's right edge moves the panel's
    // value window. Both live in the margin the scaler reserves for them, so the
    // four rects are outside the plot, inside the panel and disjoint from everything
    // already published - the value column included.
    {
        editor.setTargets({ &nodeA, &nodeB });
        editor.scale().setRect(panel);
        editor.fitAll();

        float valueLo = 0.0f;
        float valueHi = 0.0f;
        CHECK(editor.shownValueExtent(valueLo, valueHi)); // the keys of both layers
        CHECK(valueHi > valueLo);
        const util::Range frames = editor.frameRange(); // the horizontal bar's content
        CHECK(frames.max() > frames.min());

        // THE VALUE AXIS IS ZOOMED INTO THE KEYS on purpose: fitAll fits the value
        // window TO the key extent, i.e. the window would equal its own content, the
        // v-thumb would fill its track and Scaler::scrollBars would publish NO thumb
        // (nothing to scroll - see its note). A quarter-to-three-quarters window is a
        // real thumb, so the assertions below measure a bar that exists. The frame
        // axis keeps fitAll's fit: its CONTENT is the project's frame range, which is
        // wider than the keys' own span either way.
        editor.scale().setValueWindow(
            valueLo + 0.25f * (valueHi - valueLo), valueLo + 0.75f * (valueHi - valueLo)
        );

        const gui::GraphPanelModel barsModel = gui::buildGraphPanelModel(editor, viewport, palette);
        const QRect plot = barsModel.plot;
        CHECK(!plot.isEmpty());

        // THE TRACKS, hand-built from the scaler's own constants and the published
        // plot: outside the plot by kScrollBarGap, kScrollBarThickness thick, each
        // spanning the plot's own edge along the axis it scrolls.
        CHECK(barsModel.hTrack
              == QRect(
                     plot.left(), plot.bottom() + 1 + ctrl::graph::Scaler::kScrollBarGap, plot.width(),
                     ctrl::graph::Scaler::kScrollBarThickness
                 ));
        CHECK(barsModel.vTrack
              == QRect(
                     plot.right() + 1 + ctrl::graph::Scaler::kScrollBarGap, plot.top(),
                     ctrl::graph::Scaler::kScrollBarThickness, plot.height()
                 ));
        CHECK(barsModel.panel.contains(barsModel.hTrack));
        CHECK(barsModel.panel.contains(barsModel.vTrack));
        CHECK(!barsModel.hTrack.intersects(plot));
        CHECK(!barsModel.vTrack.intersects(plot));
        CHECK(!barsModel.hTrack.intersects(barsModel.rulerBand)); // the ruler's rows are above the plot
        CHECK(!barsModel.vTrack.intersects(barsModel.rulerBand));
        CHECK(!barsModel.valueColumn.intersects(barsModel.hTrack));
        CHECK(!barsModel.valueColumn.intersects(barsModel.vTrack));
        // ...the reserve is the APP's lane (12px) plus its 2px gap, and the outer
        // kMargin of the panel survives outside each bar.
        CHECK(ctrl::graph::Scaler::kScrollBarThickness == 12);
        CHECK(ctrl::graph::Scaler::kScrollBarReserve == 14);
        CHECK(barsModel.hTrack.bottom() == barsModel.panel.bottom() - ctrl::graph::Scaler::kMargin);
        CHECK(barsModel.vTrack.right() == barsModel.panel.right() - ctrl::graph::Scaler::kMargin);
        // ...the two bars on DIFFERENT edges, so they cannot meet in a corner.
        CHECK(!barsModel.hTrack.intersects(barsModel.vTrack));
        CHECK(barsModel.hTrack.top() > barsModel.valueColumn.bottom());
        CHECK(barsModel.vTrack.left() > plot.right());

        // THE THUMBS sit inside their tracks and say what the window covers: the
        // length is the window/content fraction of the track (within a pixel of the
        // rounding), never shorter than kScrollBarMinThumb and never longer than the
        // track, and they are inside their own track's rect.
        CHECK(barsModel.hTrack.contains(barsModel.hThumb));
        CHECK(barsModel.vTrack.contains(barsModel.vThumb));
        CHECK(barsModel.hThumb.width() >= std::min(ctrl::graph::Scaler::kScrollBarMinThumb, barsModel.hTrack.width()));
        CHECK(barsModel.vThumb.height() >= std::min(ctrl::graph::Scaler::kScrollBarMinThumb, barsModel.vTrack.height()));
        // The window the thumb describes is the editor's own: the frames the plot
        // covers (the mapping's inverse at its two edges) inside the project's range.
        // The oracle is a copy of the editor's LIVE scaler pinned to this panel - the
        // same state the model's own copy was built from - so the ratio below is
        // measured against the mapping the plot is drawn with.
        ctrl::graph::Scaler live = editor.scale();
        live.setRect(panel);
        const double frameWindow = live.frameAtPixel(double(plot.right() + 1)) - live.frameAtPixel(double(plot.left()));
        const double frameContent = double(frames.max() - frames.min());
        const double expectedHThumb = double(barsModel.hTrack.width()) * frameWindow / frameContent;
        std::printf(
            "[16] bars: plot (%d,%d %dx%d)  hTrack (%d,%d %dx%d) hThumb %d px (expected %.1f)  vTrack (%d,%d "
            "%dx%d) vThumb %d px  value content %.4f..%.4f window %.4f..%.4f  frames %d..%d window %.2f..%.2f\n",
            plot.x(), plot.y(), plot.width(), plot.height(), barsModel.hTrack.x(), barsModel.hTrack.y(),
            barsModel.hTrack.width(), barsModel.hTrack.height(), barsModel.hThumb.width(), expectedHThumb, barsModel.vTrack.x(),
            barsModel.vTrack.y(), barsModel.vTrack.width(), barsModel.vTrack.height(), barsModel.vThumb.height(), valueLo, valueHi,
            editor.scale().valueMin(), editor.scale().valueMax(), frames.min(), frames.max(),
            live.frameAtPixel(double(plot.left())), live.frameAtPixel(double(plot.right() + 1))
        );
        CHECK(std::fabs(double(barsModel.hThumb.width()) - expectedHThumb) <= 1.0);
        CHECK(barsModel.hThumb.left() >= barsModel.hTrack.left());
        CHECK(barsModel.hThumb.right() <= barsModel.hTrack.right());
        CHECK(barsModel.vThumb.top() >= barsModel.vTrack.top());
        CHECK(barsModel.vThumb.bottom() <= barsModel.vTrack.bottom());

        // A FULL-CONTENT WINDOW SHOWS NO BAR: the frame axis asked for a window wider
        // than the project's whole range, and the value axis for one wider than every
        // key's extent - a thumb that fills its track has no travel, so Scaler::
        // scrollBars publishes NO thumb (and therefore nothing is drawn and nothing is
        // claimed; see its own note). The TRACK is still laid out, which is what keeps
        // the plot's rect - and every frame coordinate - independent of the content.
        editor.scale().fitFrameAxis(core::Frame(-500), core::Frame(500));
        editor.scale().setValueWindow(-1000.0f, 1000.0f);
        const gui::GraphPanelModel full = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf("[16] full-content window: hThumb empty %d  vThumb empty %d  tracks laid out %d/%d\n",
                    int(full.hThumb.isEmpty()), int(full.vThumb.isEmpty()), int(!full.hTrack.isEmpty()),
                    int(!full.vTrack.isEmpty()));
        CHECK(full.hThumb.isEmpty());
        CHECK(full.vThumb.isEmpty());
        CHECK(!full.hTrack.isEmpty());
        CHECK(!full.vTrack.isEmpty());
        CHECK(full.plot == barsModel.plot);

        // A NARROW WINDOW YIELDS THE MINIMUM THUMB. The frame axis is given a
        // HALF-FRAME window (setFrameWindow maps a pushed window exactly - no ladder
        // rung rounds it) and the value axis a thousandth of the key extent; both
        // fractions are far under the 16px floor, so both thumbs are exactly that
        // floor and not the fraction.
        editor.setFrameWindow(10.0, 10.5);
        editor.scale().setValueWindow(50.0f, 50.001f);
        const gui::GraphPanelModel narrow = gui::buildGraphPanelModel(editor, viewport, palette);
        std::printf(
            "[16] narrow windows: frame window %.3f..%.3f of %d..%d -> hThumb %d px  value window %.4f..%.4f of "
            "%.4f..%.4f -> vThumb %d px  (floor %d)\n",
            editor.scale().frameAtPixel(double(narrow.plot.left())),
            editor.scale().frameAtPixel(double(narrow.plot.right() + 1)), frames.min(), frames.max(),
            narrow.hThumb.width(), editor.scale().valueMin(), editor.scale().valueMax(), valueLo, valueHi,
            narrow.vThumb.height(), ctrl::graph::Scaler::kScrollBarMinThumb
        );
        CHECK(narrow.hThumb.width() == ctrl::graph::Scaler::kScrollBarMinThumb);
        CHECK(narrow.vThumb.height() == ctrl::graph::Scaler::kScrollBarMinThumb);
        editor.clearFrameAxis(); // the frame axis is the panel's own again for what follows

        // AN EMPTY PLOT PUBLISHES NO BARS AT ALL (there is nowhere to lay one out), and
        // that is also what keeps the panel's margin clean in that state.
        const QSize degenerate(40, 20);
        const gui::GraphPanelModel degenerateModel = gui::buildGraphPanelModel(editor, degenerate, palette);
        CHECK(degenerateModel.plot.isEmpty());
        CHECK(degenerateModel.hTrack.isEmpty());
        CHECK(degenerateModel.hThumb.isEmpty());
        CHECK(degenerateModel.vTrack.isEmpty());
        CHECK(degenerateModel.vThumb.isEmpty());

        // The editor is left as the blocks around this one expect it.
        editor.scale().setRect(panel);
        editor.fitAll();
    }

    // -------------------------------------------------------------------------
    // read-only probe: a build must not move the editor or the document
    {
        editor.setProject(&project);
        editor.setTargets({ &nodeA, &nodeB });
        animator.setFrame(core::Frame(12));

        const QRect rectBefore = editor.scale().rect();
        const float minBefore = editor.scale().valueMin();
        const float maxBefore = editor.scale().valueMax();
        const int targetsBefore = editor.targets().size();
        const int rotateKeysBefore = nodeA.timeLine()->map(core::TimeKeyType_Rotate).size();
        const bool modifiedBefore = project.isModified();
        const int frameBefore = editor.currentFrame().get();

        const gui::GraphPanelModel probe = gui::buildGraphPanelModel(editor, viewport, palette);
        const gui::GraphPanelModel probeAgain = gui::buildGraphPanelModel(editor, viewport, palette);

        std::printf("[R] scale rect %d  window %.6f..%.6f (before %.6f..%.6f)  targets %d  rotate keys %d  "
                    "project.isModified=%d  currentFrame=%d  rebuild identical=%d\n",
                    int(editor.scale().rect() == rectBefore), editor.scale().valueMin(), editor.scale().valueMax(),
                    minBefore, maxBefore, isz(editor.targets()), isz(nodeA.timeLine()->map(core::TimeKeyType_Rotate)),
                    int(project.isModified()), editor.currentFrame().get(),
                    int(probe.curves.size() == probeAgain.curves.size() &&
                        probe.frameTickX == probeAgain.frameTickX && probe.playheadX == probeAgain.playheadX));

        CHECK(editor.scale().rect() == rectBefore);
        CHECK(editor.scale().valueMin() == minBefore);
        CHECK(editor.scale().valueMax() == maxBefore);
        CHECK(editor.targets().size() == targetsBefore);
        CHECK(nodeA.timeLine()->map(core::TimeKeyType_Rotate).size() == rotateKeysBefore);
        CHECK(project.isModified() == modifiedBefore);
        CHECK(editor.currentFrame().get() == frameBefore);
        CHECK(probe.playheadX == probeAgain.playheadX);
        CHECK(probe.frameTickX == probeAgain.frameTickX);
        // The tiers are part of the same pure function: the ladder does not move
        // between two builds either, so the classification is reproducible too.
        CHECK(probe.frameTickLevel == probeAgain.frameTickLevel);
        CHECK(probe.curves.size() == probeAgain.curves.size());
    }

    std::printf("%s: %d failed assertion(s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
    return gFailures == 0 ? 0 : 1;
}
