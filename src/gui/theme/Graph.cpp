#include "gui/theme/Graph.h"

#include "gui/theme/Colors.h"

namespace theme {

namespace {
// The neutral row's darkest column: Tailwind's neutral-950, the palette's own
// black. The contrast outline is the one token that must NOT follow the
// palette polarity (see Graph::outline() in the header), so it takes this cell
// from BOTH tables rather than a theme::Colors role - every content token in
// Colors flips to near-white in the dark palette.
constexpr int kNeutralBlackStep = 10;
} // namespace

//-------------------------------------------------------------------------------------------------
Graph::Graph() {
    // Same contract as TimeLine: read the cached palette that GUIResources
    // activated before any editor was built, so constructing a Graph never
    // touches settings or recomputes the tokens.
    assignFrom(Colors::current());
}

Graph Graph::from(const Colors& aColors) {
    Graph graph;
    graph.assignFrom(aColors);
    return graph;
}

void Graph::assignFrom(const Colors& aColors) {
    // The frame ticks read at the separator tone. There is no second, weaker
    // tone: the panel draws ticks and no grid, so nothing is a step behind.
    mGridMajor = aColors.hairline;

    // Value axis: the hover separator is the one line tone that stays legible
    // over the artwork the panel floats on; labels use the muted text.
    mAxisLine = aColors.hairlineHover;
    mAxisText = aColors.textMuted;

    // Label halo: the palette's opaque floater body. The panel paints no
    // background of its own (see GraphPanelModel's header), so the halo is the
    // tone the text would sit on if it had one - and a halo that carried the
    // popup body's translucency would let the artwork show through the glyph
    // ring, which is the one thing the halo exists to prevent.
    mHalo = aColors.floaterBody;

    mOutline = paletteColor(kTailwindRows[kTailwindNeutralRow][kNeutralBlackStep]);

    // Fallback curve color, i.e. the generic line for a type keyColor() does
    // not name: the bright brand edge, the same family the playhead and the
    // selection read as.
    mCurve = aColors.accentBright;

    // Key dots: content-colored fill in a floor-colored ring, so a dot stays
    // visible on top of any curve hue; the selected dot is the brand edge.
    mKeyFill = aColors.text;
    mKeyEdge = aColors.base;
    mSelected = aColors.accentBright;

    // Tangent handles: raised like the widgets they emulate, edged with the
    // popup border tone, stems muted so they never outread the curve itself.
    mHandleFill = aColors.raised;
    mHandleEdge = aColors.floaterEdge;
    mHandleLine = aColors.textMuted;

    // Per-type curve colors are the timeline's key hues, so a curve on the
    // graph and its keys on the timeline read as the same feature. The
    // neutral family shares baseKey (the timeline's default key color); the
    // array covers every type below TimeKeyType_TERM.
    mKeyColors[core::TimeKeyType_Move] = aColors.moveKey;
    mKeyColors[core::TimeKeyType_Rotate] = aColors.rotateKey;
    mKeyColors[core::TimeKeyType_Scale] = aColors.scaleKey;
    mKeyColors[core::TimeKeyType_Depth] = aColors.depthKey;
    mKeyColors[core::TimeKeyType_Opa] = aColors.opaKey;
    mKeyColors[core::TimeKeyType_Bone] = aColors.baseKey;
    mKeyColors[core::TimeKeyType_Pose] = aColors.baseKey;
    mKeyColors[core::TimeKeyType_Mesh] = aColors.baseKey;
    mKeyColors[core::TimeKeyType_FFD] = aColors.baseKey;
    mKeyColors[core::TimeKeyType_Image] = aColors.imageKey;
    mKeyColors[core::TimeKeyType_HSV] = aColors.HSVKey;
    mKeyColors[core::TimeKeyType_Blur] = aColors.blurKey;
}

QColor Graph::keyColor(core::TimeKeyType aType) const {
    // TimeKeyType_TERM is the sentinel, never a real key's type, and a value
    // past it is not a type this palette names: both fall back to the generic
    // curve color.
    const int type = (int)aType;
    if (type < 0 || type >= core::TimeKeyType_TERM)
        return mCurve;
    return mKeyColors[type];
}

} // namespace theme
