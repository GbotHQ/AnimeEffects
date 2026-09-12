// What does the mesh's "is there content here?" test cost and drop?
//
// Build and run: tools/psd_composite_probe/build_and_run.sh --mesh [<psd>] [--seed T]. Measured
// on the 114 MB test file (51 layers, 3840x1240) the policy "seed + growth" costs 0.16 % more
// vertices than "all" and 7.8 % more than "threshold", while keeping the 9.0 M faint pixels that
// belong to a feature with a solid core - i.e. the threshold's geometry saving is nearly all
// bought by deleting soft content, not by skipping residue. The residue itself (core-less
// islands) is ~6 700 islands of ~3.5 px each, 0.26 % of the faint pixels and 0.16 % of the mesh,
// and the split is insensitive to the seed: from T = 2 to T = 32 the attached share stays
// between 98.9 % and 99.9 % and the island count stays between 6 364 and 6 728. See
// BLEND_IF_ASSESSMENT.md section 19.
//
// The content test (GridMeshCreator::Image::hasRawAlpha) is BOTH the fidelity rule - a cell
// exists only where it is true, and the mesh is what draws the layer, so anything it calls empty
// is not drawn at all - and the geometry rule. That is why one number cannot serve: an alpha
// threshold low enough not to cut a soft glow's skirt also meshes every faint speck left behind
// by erasing with Flow < 100 %, and a threshold high enough to drop the residue also cuts the
// glow.
//
// This tool measures the three candidate policies on a real PSD. It decodes each layer's pixels,
// labels the connected components of alpha > 0, and then realises each policy by ZEROING the
// alpha of the pixels that policy calls empty - so the vertex and index counts it prints come
// from the production mesh builder itself (hasAlpha is a bounds-checked hasRawAlpha, so a zeroed
// alpha byte is exactly "not content" to it), not from a re-implementation.
//
//   policy "all"        : alpha != 0            (the current code)
//   policy "threshold"  : alpha >  T            (the old code, T = 10)
//   policy "seed+growth": every connected component of alpha > 0 that contains at least one
//                         pixel >= T. This never cuts a feature that has a solid core, so a soft
//                         glow's skirt survives intact, while a faint island with no solid pixel
//                         anywhere in it is dropped.
//
// Reported per layer: the faint pixels (1..T-1) that belong to a component with a solid core
// (what "threshold" wrongly cuts) vs those in a core-less island (what "all" needlessly meshes),
// and the mesh cost of each policy in vertices and indices.
#include <QElapsedTimer>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>
#include "XC.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/PSDReader.h"
#include "img/PSDUtil.h"
#include "img/GridMeshCreator.h"
#include "core/Constant.h"
#include "core/ImageKey.h"

XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

struct CompStats {
    long pixels = 0;
    int maxAlpha = 0;
    long long mass = 0;   // sum of alpha, in 0..255 units
    long faint = 0;       // pixels with 1 <= alpha < kSeed
};

// union-find over the pixels with alpha > 0, 8-connected
class Labeller {
public:
    Labeller(int aW, int aH): mW(aW), mH(aH), mParent((size_t)aW * aH, -1) {}

    void add(int aX, int aY) {
        const int i = aY * mW + aX;
        mParent[i] = i;
        const int nb[4][2] = {{-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
        for (const auto& d : nb) {
            const int x = aX + d[0], y = aY + d[1];
            if (x < 0 || mW <= x || y < 0 || mH <= y) {
                continue;
            }
            const int n = y * mW + x;
            if (mParent[n] >= 0) {
                unite(i, n);
            }
        }
    }

    int root(int aI) {
        while (mParent[aI] != aI) {
            mParent[aI] = mParent[mParent[aI]];
            aI = mParent[aI];
        }
        return aI;
    }

    int parentAt(int aI) const { return mParent[aI]; }
    bool isContent(int aI) const { return mParent[aI] >= 0; }

private:
    void unite(int a, int b) {
        a = root(a);
        b = root(b);
        if (a != b) {
            mParent[b] = a;
        }
    }
    int mW, mH;
    std::vector<int> mParent;
};

struct LayerReport {
    QString name;
    QSize size;
    long faintTotal = 0, faintSeeded = 0, faintOrphan = 0;
    int orphanComps = 0;
    int orphanMaxAlpha = 0;
    long orphanPixels = 0;
    long long orphanMass = 0;
    int vertsAll = 0, vertsThreshold = 0, vertsSeed = 0;
    int idxAll = 0, idxThreshold = 0, idxSeed = 0;
};

int gCell = core::ImageKey::kDefaultMeshCellSize;
bool gFull = false;

void meshStats(std::vector<uint8>& aRgba, const QSize& aSize, int& aOutVerts, int& aOutIdx) {
    img::GridMeshCreator creator(aRgba.data(), aSize, gCell);
    aOutVerts = creator.vertexCount();
    aOutIdx = creator.indexCount();
}

} // namespace

int main(int argc, char** argv) {
    int seed = 10; // T: the level at which content counts as deliberate
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--full") == 0) {
            // "skip nothing at all": every pixel counts as content, so the mesh is a plain grid
            // over each layer's rectangle. The price of never asking the content question.
            gFull = true;
        } else if (std::strcmp(argv[i], "--cell") == 0 && i + 1 < argc) {
            // the mesh's granularity: the other way to make faint content cheap, without
            // deleting it. Every policy below is measured at this cell size.
            gCell = std::atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: %s [--seed T] <file.psd>\n", argv[0]);
        return 64;
    }
    std::ifstream file(path, std::ios::binary);
    img::PSDReader reader(file);
    if (reader.resultCode() != img::PSDReader::ResultCode_Success) {
        std::fprintf(stderr, "parse failed\n");
        return 1;
    }
    const img::PSDFormat& format = *reader.format();
    std::printf("%s   (seed T = %d, mesh cell = %d px%s)\n\n", path, seed, gCell,
        gFull ? ", EVERY pixel counted as content" : "");

    long faintTotal = 0, faintSeeded = 0, faintOrphan = 0;
    int orphanComps = 0;
    long orphanPixels = 0, orphanMax = 0;
    long long orphanMass = 0;
    long long vAll = 0, vThr = 0, vSeed = 0, iAll = 0, iThr = 0, iSeed = 0;
    int layers = 0, layersWithFaint = 0;

    for (const auto& layerPtr : format.layerAndMaskInfo().layers) {
        const img::PSDFormat::Layer& layer = *layerPtr;
        if (layer.entryType != img::PSDFormat::LayerEntryType_Layer || layer.rect.width() <= 0
            || layer.rect.height() <= 0) {
            continue;
        }
        ++layers;
        const QSize size(layer.rect.width(), layer.rect.height());
        XCMemBlock block = img::PSDUtil::makeInterleavedImage(format.header(), layer, img::PSDUtil::ColorFormat_RGBA8);
        if (!block.data || block.size < (size_t)size.width() * size.height() * 4) {
            continue;
        }
        std::vector<uint8> rgba(block.data, block.data + block.size);
        if (gFull) {
            for (size_t i = 0; i < (size_t)size.width() * size.height(); ++i) {
                rgba[i * 4 + 3] = 255;
            }
        }

        Labeller lab(size.width(), size.height());
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                if (rgba[((size_t)y * size.width() + x) * 4 + 3] != 0) {
                    lab.add(x, y);
                }
            }
        }

        std::unordered_map<int, CompStats> comps;
        long layerFaint = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const int i = y * size.width() + x;
                if (!lab.isContent(i)) {
                    continue;
                }
                const int a = rgba[(size_t)i * 4 + 3];
                auto& c = comps[lab.root(i)];
                ++c.pixels;
                c.mass += a;
                if (a > c.maxAlpha) {
                    c.maxAlpha = a;
                }
                if (a < seed) {
                    ++c.faint;
                    ++layerFaint;
                }
            }
        }

        LayerReport rep;
        rep.name = QString::fromStdString(layer.name);
        rep.size = size;
        rep.faintTotal = layerFaint;
        for (const auto& kv : comps) {
            const CompStats& c = kv.second;
            if (c.maxAlpha >= seed) {
                rep.faintSeeded += c.faint;
            } else {
                rep.faintOrphan += c.faint;
                ++rep.orphanComps;
                rep.orphanPixels += c.pixels;
                rep.orphanMass += c.mass;
                rep.orphanMaxAlpha = std::max(rep.orphanMaxAlpha, c.maxAlpha);
            }
        }

        if (layerFaint > 0) {
            ++layersWithFaint;
        }

        // geometry: the real mesh builder, on the pixels each policy calls content
        std::vector<uint8> buf;
        meshStats(rgba, size, rep.vertsAll, rep.idxAll);

        buf = rgba;
        for (size_t i = 0; i < (size_t)size.width() * size.height(); ++i) {
            if (buf[i * 4 + 3] <= seed) { // the old test: > T
                buf[i * 4 + 3] = 0;
            }
        }
        meshStats(buf, size, rep.vertsThreshold, rep.idxThreshold);

        buf = rgba;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const int i = y * size.width() + x;
                if (!lab.isContent(i)) {
                    continue;
                }
                const auto it = comps.find(lab.root(i));
                if (it != comps.end() && it->second.maxAlpha < seed) {
                    buf[(size_t)i * 4 + 3] = 0; // a core-less island: not content
                }
            }
        }
        meshStats(buf, size, rep.vertsSeed, rep.idxSeed);

        if (rep.faintTotal > 0 || rep.orphanComps > 0) {
            std::printf(
                "  %-24s %5dx%-5d  faint(1..%d) %8ld : attached %8ld  orphan %7ld in %5d islands"
                "  |  vertices all %7d  threshold %7d  seed+grow %7d\n",
                rep.name.left(24).toUtf8().constData(), size.width(), size.height(), seed - 1,
                rep.faintTotal, rep.faintSeeded, rep.faintOrphan, rep.orphanComps, rep.vertsAll,
                rep.vertsThreshold, rep.vertsSeed);
        }

        faintTotal += rep.faintTotal;
        faintSeeded += rep.faintSeeded;
        faintOrphan += rep.faintOrphan;
        orphanComps += rep.orphanComps;
        orphanPixels += rep.orphanPixels;
        orphanMass += rep.orphanMass;
        orphanMax = std::max(orphanMax, (long)rep.orphanMaxAlpha);
        vAll += rep.vertsAll;
        vThr += rep.vertsThreshold;
        vSeed += rep.vertsSeed;
        iAll += rep.idxAll;
        iThr += rep.idxThreshold;
        iSeed += rep.idxSeed;
    }

    std::printf("\n%d layers (%d with any faint pixel), seed T = %d\n", layers, layersWithFaint, seed);
    std::printf("  faint pixels (alpha 1..%d)                     %10ld\n", seed - 1, faintTotal);
    std::printf("    ... in a component that has a solid core     %10ld  (%.2f%% - 'threshold' cuts these)\n",
        faintSeeded, faintTotal ? 100.0 * faintSeeded / faintTotal : 0.0);
    std::printf("    ... in a core-less island                    %10ld  (%.2f%% in %d islands; 'all' meshes these)\n",
        faintOrphan, faintTotal ? 100.0 * faintOrphan / faintTotal : 0.0, orphanComps);
    std::printf("  core-less islands: %ld pixels, peak alpha %ld, alpha mass %lld\n", orphanPixels,
        orphanMax, orphanMass);
    std::printf("\n  mesh geometry, summed over every layer:\n");
    std::printf("    %-18s %12s %12s\n", "policy", "vertices", "indices");
    std::printf("    %-18s %12lld %12lld\n", "all (alpha != 0)", vAll, iAll);
    std::printf("    %-18s %12lld %12lld   (%+.2f%% vertices)\n", "threshold (alpha > T)", vThr, iThr,
        vAll ? 100.0 * (vThr - vAll) / vAll : 0.0);
    std::printf("    %-18s %12lld %12lld   (%+.2f%% vertices)\n", "seed + growth", vSeed, iSeed,
        vAll ? 100.0 * (vSeed - vAll) / vAll : 0.0);
    return 0;
}
