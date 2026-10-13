#ifndef GUI_THEME_GRAPH_H
#define GUI_THEME_GRAPH_H

#include <QColor>
#include "core/TimeKeyType.h"
#include "gui/theme/Colors.h"

namespace theme {

// Appearance tokens for the animation graph overlay (gui::drawPanel, in
// src/gui/ImGuiOverlay.cpp).
//
// The panel is drawn by the gui layer from gui::GraphPanelModel, so - exactly
// like theme::TimeLine for ctrl::time::Renderer - the palette decisions live here
// and the drawing layer only reads them. Every token is a curated cell of
// theme::Colors, so an accent or light/dark change is one rebuild (Graph::from)
// and never a per-widget color.
//
// Curves are colored by KEY TYPE through keyColor(), i.e. the same hues the
// timeline draws its keys in: a curve on the graph and its keys on the timeline
// read as the same feature.
class Graph {
public:
    Graph();

    // Curated tokens for the active palette.
    static Graph from(const Colors& aColors);

    QColor gridMajor() const { return mGridMajor; }  // the frame ticks (the panel has no grid)
    QColor axisLine() const { return mAxisLine; }    // value-axis base line
    QColor axisText() const { return mAxisText; }    // value and frame labels
    // Label halo: the glyphs drawn once behind every label so the text stays
    // readable over arbitrary artwork. The panel has no background of its own
    // (see GraphPanelModel's header), so the halo is the panel tone the text
    // would sit on if it had one, and the text colour is chosen against it.
    QColor halo() const { return mHalo; }
    // Contrast outline: the ring the composite pass (gui::outline, in
    // src/gui/ImGuiOverlay.cpp) draws around panel ink whose own colour is too
    // close to the artwork underneath it. It is BLACK on both palettes, on
    // purpose: the ring exists only where the panel has lost contrast against
    // arbitrary artwork, and a theme's polarity is a statement about the
    // application's own surfaces, not about what happens to be behind the
    // panel. The palette's black is the neutral row's darkest cell, the same
    // cell in the light and the dark table.
    QColor outline() const { return mOutline; }
    QColor curve() const { return mCurve; }          // fallback curve color
    QColor keyFill() const { return mKeyFill; }      // key dot fill
    QColor keyEdge() const { return mKeyEdge; }      // key dot edge
    QColor selected() const { return mSelected; }    // selected key / handle
    QColor handleFill() const { return mHandleFill; } // tangent handle fill
    QColor handleEdge() const { return mHandleEdge; } // tangent handle edge
    QColor handleLine() const { return mHandleLine; } // tangent handle stem

    // The key-type's own color (the timeline's per-key hues). Falls back to
    // curve() for a type the palette does not name.
    QColor keyColor(core::TimeKeyType aType) const;

private:
    // Fills every token from one palette. A separate step rather than a
    // constructor body, so the default constructor and from() cannot recurse
    // into each other (from() must hand back a filled Graph, and Graph has no
    // other way to obtain storage).
    void assignFrom(const Colors& aColors);

    QColor mGridMajor;
    QColor mAxisLine;
    QColor mAxisText;
    QColor mHalo;
    QColor mOutline;
    QColor mCurve;
    QColor mKeyFill;
    QColor mKeyEdge;
    QColor mSelected;
    QColor mHandleFill;
    QColor mHandleEdge;
    QColor mHandleLine;
    QColor mKeyColors[core::TimeKeyType_TERM];
};

} // namespace theme

#endif // GUI_THEME_GRAPH_H
