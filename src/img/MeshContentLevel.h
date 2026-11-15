#ifndef IMG_MESHCONTENTLEVEL_H
#define IMG_MESHCONTENTLEVEL_H

#include <QSize>

namespace img {

// The alpha level a layer's mesh content test uses: a pixel counts as CONTENT when its alpha is
// above this level, so a mesh cell exists only where the picture has paint above it.
//
// It used to be the fixed constant 10 (GridMeshCreator::kContentAlphaThreshold). A fixed value is
// wrong in both directions: too high and it cuts the skirt of deliberate soft paint, too low and
// erase residue becomes content that bends the mesh silhouette. So the level is DERIVED from the
// layer's own alpha statistics:
//
//     T = 10 when no pixel is above alpha 2; otherwise
//     T = clamp(round(0.05 * mean(alpha : alpha > 2)), 1, 10)
//
// The reasoning: a layer that paints at full strength everywhere (mean near 255) gets the strict
// level 10, so a few faint stray pixels stay out of its mesh; a layer whose pixels are mostly faint
// - a soft glow, an airbrushed shadow - is telling us that its faint end IS the picture, and its
// level comes down towards 1, so the mesh follows what was painted. Pixels at or below alpha 2 do
// not enter the statistic: the corpus comparison found no usable separation between deliberate
// soft paint and erase residue in that band, and including it would only let a sprinkle of residue
// pull an opaque layer's level down.
//
// This was chosen by the artist from a 30-file corpus comparison (tools/psd_composite_probe, the
// "mean" variant, page and README): of the three candidates - the constant 10, this mean rule, and
// a rule derived from the ratio of 2..10 pixels to pixels above 10 - the mean rule was the one
// chosen. The 0.05 factor and the 1..10 clamp are the chosen mapping; T is an integer alpha level
// because the comparison is against an 8-bit alpha byte, and the clamp keeps every layer in the
// range the corpus's mean variant produced.
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

    // The mapping itself, exposed for tests and for the comparison tooling. A non-positive mean is
    // the "nothing to measure" case and returns the strict level, not the clamp's lower bound.
    static int fromMean(double aMeanAbove2);
};

} // namespace img

#endif // IMG_MESHCONTENTLEVEL_H
