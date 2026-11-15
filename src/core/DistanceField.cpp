#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "core/DistanceField.h"
#include "img/Buffer.h"
#include "img/ResourceData.h"

namespace {

// The transform runs on INTEGERS: the seeds are whole pixel coordinates, so every squared
// distance in the two 1D passes is a whole number and the envelope arithmetic below never
// rounds. Only the final sqrt (and its round-to-nearest) is floating point, which is what
// makes the field exactly reproducible - the harness checks it against brute force over every
// pixel. The scratch is one int32 per pixel (4 bytes), and the largest value that can appear
// is (w^2 + h^2), which stays well inside int32 for any texture this app can hold.
typedef int32_t Dist;

// "no seed on this line": only ever compared and skipped, never added to a parabola index
const Dist kNoSeed = std::numeric_limits<Dist>::max();

// 1D squared-distance transform of a strided line (Felzenszwalb & Huttenlocher §4):
// given f[i] (0 for a seed, kNoSeed elsewhere) it fills d[i] = min_j (f[j] + (i - j)^2).
// The lower envelope of the parabolas is built in one pass with an intersection stack, so the
// whole 2D transform is O(w*h) instead of O((wh)^2). aV/aRootF/aZ are caller-owned scratch, so
// no line allocates.
//
// The transform runs IN PLACE on the grid (both passes rewrite the line they read), through a
// caller-owned output line: every position is evaluated from the untouched input before the
// line is copied back, so a root's own value can never be read after being overwritten.
// aV/aZ/aOut are caller scratch, so no line allocates.
void edt1d(Dist* aF, int aStride, int aN, int* aV, double* aZ, Dist* aOut) {
    int k = -1;

    // Only parabolas that exist can be on the lower envelope; kNoSeed entries (a line with no
    // seed at all) are skipped instead of pushed, because their intersection with a neighbour
    // is not a number and would silently stop the stack walk. A line whose input is entirely
    // kNoSeed legitimately stays kNoSeed, and the second (perpendicular) pass then resolves it
    // against the other axis' seeds.
    const auto rootTop = [&](int aRoot) {
        return (double)aF[(size_t)aRoot * aStride] + (double)aRoot * aRoot;
    };
    for (int q = 0; q < aN; ++q) {
        const Dist fq = aF[(size_t)q * aStride];
        if (fq == kNoSeed) {
            aOut[q] = kNoSeed; // stays unseeded; the perpendicular pass resolves it
            continue;
        }
        const double qNum = (double)fq + (double)q * q;
        if (k < 0) {
            ++k;
            aV[k] = q;
            aZ[k] = -std::numeric_limits<double>::infinity();
            aZ[k + 1] = std::numeric_limits<double>::infinity();
            continue;
        }
        // where the parabola rooted at q takes over from the one rooted at v[k]
        double s = (qNum - rootTop(aV[k])) / (2.0 * q - 2.0 * aV[k]);
        while (s <= aZ[k]) {
            --k;
            if (k < 0) {
                break;
            }
            s = (qNum - rootTop(aV[k])) / (2.0 * q - 2.0 * aV[k]);
        }
        ++k;
        aV[k] = q;
        aZ[k] = s;
        aZ[k + 1] = std::numeric_limits<double>::infinity();
    }
    if (k < 0) {
        return; // nothing on this line (the parallel skip above already filled aOut)
    }

    // evaluate every position from the envelope, then copy the line back
    k = 0;
    for (int q = 0; q < aN; ++q) {
        while (aZ[k + 1] < (double)q) {
            ++k;
        }
        const Dist d = (Dist)(q - aV[k]);
        aOut[q] = d * d + aF[(size_t)aV[k] * aStride]; // the input line is untouched until the copy-back
    }
    for (int q = 0; q < aN; ++q) {
        aF[(size_t)q * aStride] = aOut[q];
    }
}

} // namespace

namespace core {

bool DistanceField::build(const uint8* aAlpha, const QSize& aSize, std::vector<uint8>& aOut, int aThreshold) {
    const int w = aSize.width();
    const int h = aSize.height();
    if (!aAlpha || w <= 0 || h <= 0) {
        return false;
    }
    // the two passes index a line of at most max(w, h) entries
    const int longest = qMax(w, h);
    if ((int64_t)w * h > (int64_t)std::numeric_limits<Dist>::max() / 4) {
        return false; // cannot happen for a texture-sized plane, but keep the math honest
    }

    // seeds: every OUTSIDE pixel is a source of distance 0; inside pixels start unseeded
    bool anyInside = false;
    bool anyOutside = false;
    std::vector<Dist> grid((size_t)w * h, kNoSeed);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            if ((int)aAlpha[i] >= aThreshold) {
                anyInside = true;
            } else {
                anyOutside = true;
                grid[i] = 0;
            }
        }
    }
    // a shape with no edge inside this image has no distance to measure: a fully transparent
    // plane has no inside, and a fully opaque one has no outside pixel to measure from (the
    // importer pads its rects with a transparent border, so this only happens for planes that
    // were not built by the importer)
    if (!anyInside || !anyOutside) {
        return false;
    }

    // two separable passes, each one running the 1D transform along one axis; the strided walk
    // does both directions without copying a line out and back
    std::vector<int> v((size_t)longest, 0);
    std::vector<double> z((size_t)longest + 1, 0.0);
    std::vector<Dist> line((size_t)longest, kNoSeed);
    for (int x = 0; x < w; ++x) {
        edt1d(&grid[x], w, h, v.data(), z.data(), line.data());
    }
    for (int y = 0; y < h; ++y) {
        edt1d(&grid[(size_t)y * w], 1, w, v.data(), z.data(), line.data());
    }

    aOut.resize((size_t)w * h);
    for (size_t i = 0; i < aOut.size(); ++i) {
        const double d = std::sqrt((double)grid[i]);
        aOut[i] = (uint8)std::min(255.0, std::floor(d + 0.5));
    }
    return true;
}

bool DistanceField::bakeForResource(const img::Buffer& aImage, img::ResourceData& aResource, int aThreshold) {
    if (!aImage.data() || aImage.format() != img::Format_RGBA8) {
        return false;
    }
    const QSize size = aImage.pixelSize();
    if (size.isEmpty()) {
        return false;
    }
    const size_t count = (size_t)size.width() * (size_t)size.height();
    std::vector<uint8> alpha(count, 0);
    for (size_t i = 0; i < count; ++i) {
        alpha[i] = aImage.data()[i * 4 + 3];
    }
    std::vector<uint8> field;
    if (!build(alpha.data(), size, field, aThreshold) || field.size() != count) {
        return false;
    }
    auto* data = new uint8[field.size()];
    std::copy(field.begin(), field.end(), data);
    // stamp the plane with the recipe this BUILD computes, which is what a plane this build just
    // derived means; a plane read back from a project is stamped with the recipe it carries there
    aResource.grabDistance(
        XCMemBlock(data, field.size()), size, img::ResourceData::derivationRecipe(img::ResourceData::Derivation_Distance));
    return true;
}

void DistanceField::deriveResourceChannels(img::ResourceData& aResource) {
    for (int i = 0; i < img::ResourceData::Derivation_TERM; ++i) {
        const auto kind = (img::ResourceData::Derivation)i;
        if (!aResource.isDerivationStale(kind)) {
            continue;
        }
        const bool built = bakeForResource(aResource.image(), aResource);
        if (!built) {
            // no pixels (a slimmed project), a shape with no outline, or nothing to measure
            // from: the consumer is told (the renderer refuses to draw a stroke without it)
            // instead of being handed a stale or invented plane
            XC_REPORT() << "could not derive the " << img::ResourceData::derivationName(kind)
                        << " channel of '" << aResource.identifier().toUtf8().constData() << "'";
            aResource.freeDerived(kind);
        }
    }
}

} // namespace core
