#ifndef CORE_DISTANCEFIELD_H
#define CORE_DISTANCEFIELD_H

#include <cstdint>
#include <vector>
#include <QSize>
#include "XC.h"

namespace img {
class Buffer;
class ResourceData;
} // namespace img

namespace core {

// Exact Euclidean distance to a layer's outline, baked on the CPU.
//
// Photoshop's Stroke and Bevel/Emboss are both functions of the distance from the layer's
// edge: the stroke paints the band of pixels within its size of the outline, and the bevel
// builds a height profile over the same distance and lights its slope. This computes that
// distance field once, when the layer's pixels are available (import), and stores it as an
// 8-bit plane in the layer's own image rectangle, exactly like a layer mask - so the shader
// samples it with the image's texture coordinates.
//
// Semantics (all in the image's pixel grid, at pixel centres):
//   - a pixel counts as INSIDE when its alpha is >= the given threshold (the caller passes
//     the alpha plane; Photoshop's outline follows the shape, so a 1-level threshold is the
//     right notion of "edge" for an antialiased sprite),
//   - the field value is the distance to the nearest OUTSIDE pixel, clamped to 255 and
//     rounded to the nearest integer; outside pixels are 0,
//   - so an edge pixel (inside, next to an outside pixel) has the value 1, and a pixel at
//     least `w` deep inside a stroke of width `w` reads w or more.
//
// The transform is the exact separable squared-Euclidean one (Felzenszwalb & Huttenlocher,
// "Distance Transforms of Sampled Functions", with the lower-envelope construction), not a
// chamfer approximation: the harness checks it against brute force over every pixel. It runs
// on integers (the seeds are whole pixels, so every squared distance is a whole number and
// only the final sqrt rounds), which is what makes it exactly reproducible, and it is O(w*h)
// with one int32 of scratch per pixel.
//
// Cost and limits, measured at -O2 on this machine: 25 ms for a 1365x478 plane, 220 ms for a
// full 3840x1240 layer, ~0.95 s for 8K, with 4 bytes of scratch per pixel; the plane itself is
// 1 byte per pixel (652 KB for the 1365x478 case) and travels with the resource.
//   - 8-bit distances saturate at 255 px. Photoshop's Stroke tops out at 250 px, so that is
//     enough for strokes; a bevel profile coarser than 1 px would need a wider channel.
//   - only the INSIDE distance is kept: an outside or centre stroke would need the outside
//     distance as well (a second plane, or a signed field), and is reported rather than drawn.
//   - the field is DERIVED state. It is valid only while the pixels it was measured from are
//     current, so ResourceData tracks the image revision it came from
//     (ResourceData::isDistanceStale) and the render path re-bakes it when a resource reload
//     has replaced the pixels, or after asking for it when it is stale (ResourceData::derived,
//     reached from ImageKey::syncDistanceTexture once per image change).
class DistanceField {
public:
    // builds the field for an 8-bit alpha plane of aSize; false when the input is unusable
    // (empty size, mismatched buffer) or the shape has no outline inside the image (a fully
    // transparent or fully opaque plane has no edge to measure from, which the caller treats
    // as "no field")
    static bool build(const uint8* aAlpha, const QSize& aSize, std::vector<uint8>& aOut, int aThreshold = 1);

    // size of the plane `build` produces for aSize (one byte per pixel)
    static size_t sizeFor(const QSize& aSize) {
        return (size_t)qMax(0, aSize.width()) * (size_t)qMax(0, aSize.height());
    }

    // Bakes the field for an RGBA8 image's alpha and stores it on the resource
    // (ResourceData::grabDistance, which records which image revision it came from, so a later
    // pixel replacement makes it stale). false when the image has no pixels left (a slimmed
    // project), is not RGBA8, or has no outline inside its rect.
    static bool bakeForResource(const img::Buffer& aImage, img::ResourceData& aResource, int aThreshold = 128);

    // The resource's DERIVER (ResourceData::setDeriver): computes every derived channel the
    // resource says it wants. Installed on a resource by whoever owns its pixels - the importer,
    // and the project loader for a file that arrives without one - and then run by the resource
    // itself after every pixel change. That is what keeps the derivation out of the render path:
    // nothing here is called from a draw.
    static void deriveResourceChannels(img::ResourceData& aResource);
};

} // namespace core

#endif // CORE_DISTANCEFIELD_H
