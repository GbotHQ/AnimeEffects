#ifndef IMG_MESHCONTENTLEVEL_H
#define IMG_MESHCONTENTLEVEL_H

#include <QSize>

namespace img {

// The alpha level a layer's mesh content test uses: at which alpha a pixel counts as CONTENT, i.e.
// where a mesh cell starts to exist and where a boundary vertex stops trimming.
//
// It used to be the fixed constant 10 (GridMeshCreator::kContentAlphaThreshold). A fixed value is
// wrong in both directions: too high and it cuts the skirt of deliberate soft paint, too low and
// erase residue becomes content that bends the mesh silhouette. So the level is DERIVED from the
// layer's own alpha statistics:
//
//     T = clamp(round(0.05 * mean(alpha : alpha > 2)), 1, 10)
//
// The reasoning: a layer that paints at full strength everywhere (mean near 255) gets the strict
// level 10, so a few faint stray pixels stay out of its mesh; a layer whose pixels are mostly faint
// - a soft glow, an airbrushed shadow - is telling us that its faint end IS the picture, and its
// level comes down towards 1, so the mesh follows what was painted. Pixels at or below 2 alpha are
// ignored by the mean: that is the range where erase tools leave residue nobody chose.
//
// This was chosen by the artist from a 30-file corpus comparison (tools/psd_composite_probe, the
// "mean" variant, page and README): of the three candidates - the constant 10, this mean rule, and
// a rule derived from the ratio of 2..10 pixels to pixels above 10 - the mean rule was the one whose
// renders followed the artwork. The 0.05 factor and the 1..10 clamp are the fitted mapping; T is a
// whole level because the mesh is built per cell, and the clamp keeps every layer within the range
// the corpus was rendered at.
//
// The rule is deliberately a pure function of the layer's pixels: no file, layer name or path enters
// it, so a layer's mesh is reproducible from its own bitmap.
class MeshContentLevel {
public:
    // pass as aContentAlphaThreshold to mean "derive it from this image"
    enum { kDerive = -1 };

    // The level for one RGBA8 image. Returns GridMeshCreator::kContentAlphaThreshold when the image
    // has no pixel above 2 (nothing to measure: the strict level is the safe answer).
    static int derive(const void* aRgba8, const QSize& aSize);

    // The mapping itself, exposed for tests and for the comparison tooling.
    static int fromMean(double aMeanAbove2);
};

} // namespace img

#endif // IMG_MESHCONTENTLEVEL_H
