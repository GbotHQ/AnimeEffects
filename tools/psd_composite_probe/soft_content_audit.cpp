// Audits the proposed mesh content rule against real artwork: what would "seed + growth" drop?
//
// The rule (BLEND_IF_ASSESSMENT.md 19) keeps every connected component of alpha > 0 that contains
// a pixel >= T and drops the rest, on the theory that a glow's skirt belongs to a component with
// a solid core while erase residue is a core-less island. That theory is a claim about
// DISTRIBUTION and SPACE, so this tool measures both instead of assuming either:
//
//   * per core-less component: size, bounding box, peak/mean/sigma of alpha, the share of its
//     pixels sitting at its modal alpha (a flat leftover plateau vs a ramp), the strength of its
//     outer edge (alpha jump from 0 to the component across one pixel: a hard erase edge vs a
//     soft glow), and its distance to the nearest pixel of a seeded component.
//   * crops, amplified, so the dropped pixels can be LOOKED at: a drop that is invisible in a
//     histogram is obvious in a picture.
//
// usage: soft_audit <file.psd> <outDir> [--seed T] [--min-size N] [--top N]
#include <QDir>
#include <QImage>
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <vector>
#include "XC.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/PSDReader.h"
#include "img/PSDUtil.h"

XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

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
    bool isContent(int aI) const { return mParent[aI] >= 0; }
    int n() const { return mW * mH; }

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

struct Stat {
    double rampSumA = 0, rampSumD = 0, rampSumAD = 0, rampSumAA = 0, rampSumDD = 0;
    long rampN = 0;
    double ramp() const { // Pearson correlation of alpha against interior depth
        if (rampN < 8) return 0.0;
        const double n = rampN;
        const double cov = rampSumAD / n - (rampSumA / n) * (rampSumD / n);
        const double va = rampSumAA / n - (rampSumA / n) * (rampSumA / n);
        const double vd = rampSumDD / n - (rampSumD / n) * (rampSumD / n);
        if (va <= 1e-9 || vd <= 1e-9) return 0.0;
        return cov / std::sqrt(va * vd);
    }
    long pixels = 0;
    long long mass = 0;
    int peak = 0, lo = 255;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    long modal = 0;
    int modalValue = 0;
    long edgePixels = 0;
    double edgeJump = 0.0;  // mean alpha just inside the outer boundary
    long nearestSeed = -1;  // distance in pixels to the closest seeded pixel
};

// multi-source BFS distance to the nearest seeded (alpha >= T) pixel, -1 where none exists
std::vector<int> seedDistance(const std::vector<uint8>& aRgba, int aW, int aH, int aSeed) {
    std::vector<int> dist((size_t)aW * aH, -1);
    std::deque<int> queue;
    for (int i = 0; i < aW * aH; ++i) {
        if (aRgba[(size_t)i * 4 + 3] >= aSeed) {
            dist[i] = 0;
            queue.push_back(i);
        }
    }
    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        const int x = i % aW, y = i / aW;
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : nb) {
            const int nx = x + d[0], ny = y + d[1];
            if (nx < 0 || aW <= nx || ny < 0 || aH <= ny) {
                continue;
            }
            const int n = ny * aW + nx;
            if (dist[n] < 0) {
                dist[n] = dist[i] + 1;
                queue.push_back(n);
            }
        }
    }
    return dist;
}

} // namespace

int main(int argc, char** argv) {
    int seed = 10, minSize = 1, top = 8;
    const char* path = nullptr;
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--seed") == 0 && i + 1 < argc) {
            seed = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--min-size") == 0 && i + 1 < argc) {
            minSize = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--top") == 0 && i + 1 < argc) {
            top = std::atoi(argv[++i]);
        } else if (!path) {
            path = a;
        } else {
            outDir = a;
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: %s <file.psd> <outDir> [--seed T] [--min-size N] [--top N]\n", argv[0]);
        return 64;
    }
    if (outDir) {
        QDir().mkpath(outDir);
    }
    std::ifstream file(path, std::ios::binary);
    img::PSDReader reader(file);
    if (reader.resultCode() != img::PSDReader::ResultCode_Success) {
        std::fprintf(stderr, "parse failed\n");
        return 1;
    }
    const img::PSDFormat& format = *reader.format();
    std::printf("%s  (T=%d, core-less components of at least %d px)\n", path, seed, minSize);

    // global histogram of core-less component sizes, in powers of two
    long buckets[24] = {};
    long rampBuckets[10] = {};
    // the two populations a core-less component falls into, by its own distribution:
    //   uniform veil : nearly all pixels at one very low level (no gradient at all)
    //   soft gradient: a genuine ramp, i.e. shaped like painted soft content
    //   between      : neither cleanly
    long veilComps = 0, veilPixels = 0, gradComps = 0, gradPixels = 0, midComps = 0, midPixels = 0;
    double veilEdge = 0, gradEdge = 0, midEdge = 0;
    long veilN = 0, gradN = 0, midN = 0;
    // mean alpha over the component's alpha > 0 pixels: the component's own "ink per pixel".
    // Histogrammed in 0.25 buckets up to 12, per class, plus the pixel mass that a cutoff on it
    // would remove from each class.
    enum { kMeanBuckets = 48 };
    long veilMean[kMeanBuckets] = {}, gradMean[kMeanBuckets] = {}, midMean[kMeanBuckets] = {};
    long veilMass = 0, gradMass = 0, midMass = 0;
    long veilDropByCut[9] = {}, gradDropByCut[9] = {}, midDropByCut[9] = {};
    // components every pixel of which is exactly alpha 1 (equivalently: mean alpha == 1, since
    // alpha >= 1 throughout a component). This is the class-free population the floor rule
    // addresses, and its size distribution is what says whether the rule removes specks or areas.
    long pure1Comps = 0, pure1Pixels = 0;
    long pure1Sizes[16] = {}; // 0: 1 px, then powers of two
    long totalOrphan = 0, totalOrphanPixels = 0;
    long bigOrphan = 0, bigOrphanPixels = 0;
    long sharpOrphan = 0; // components whose outer edge jumps to their peak within one pixel
    std::vector<std::pair<long, QString>> biggest;

    for (const auto& layerPtr : format.layerAndMaskInfo().layers) {
        const img::PSDFormat::Layer& layer = *layerPtr;
        if (layer.entryType != img::PSDFormat::LayerEntryType_Layer || layer.rect.width() <= 0
            || layer.rect.height() <= 0) {
            continue;
        }
        const int w = layer.rect.width(), h = layer.rect.height();
        XCMemBlock block = img::PSDUtil::makeInterleavedImage(format.header(), layer, img::PSDUtil::ColorFormat_RGBA8);
        if (!block.data || block.size < (size_t)w * h * 4) {
            continue;
        }
        std::vector<uint8> rgba(block.data, block.data + block.size);

        Labeller lab(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (rgba[((size_t)y * w + x) * 4 + 3] != 0) {
                    lab.add(x, y);
                }
            }
        }
        std::unordered_map<int, Stat> comps;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (!lab.isContent(i)) {
                    continue;
                }
                const int a = rgba[(size_t)i * 4 + 3];
                Stat& s = comps[lab.root(i)];
                ++s.pixels;
                s.mass += a;
                s.peak = std::max(s.peak, a);
                s.lo = std::min(s.lo, a);
                s.x0 = std::min(s.x0, x);
                s.y0 = std::min(s.y0, y);
                s.x1 = std::max(s.x1, x);
                s.y1 = std::max(s.y1, y);
            }
        }
        // modal alpha per component (a flat plateau shows up as a large modal share)
        {
            std::unordered_map<int, std::vector<int>> hist;
            for (auto& kv : comps) {
                hist[kv.first].assign(256, 0);
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int i = y * w + x;
                    if (!lab.isContent(i)) {
                        continue;
                    }
                    ++hist[lab.root(i)][rgba[(size_t)i * 4 + 3]];
                }
            }
            for (auto& kv : comps) {
                const auto& hv = hist[kv.first];
                for (int a = 0; a < 256; ++a) {
                    if (hv[a] > kv.second.modal) {
                        kv.second.modal = hv[a];
                        kv.second.modalValue = a;
                    }
                }
            }
        }
        // interior depth: distance from each content pixel to the nearest pixel that is not in
        // its own component (its outer boundary), by BFS seeded at those boundary pixels
        std::vector<int> depth((size_t)w * h, -1);
        {
            std::deque<int> queue;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int i = y * w + x;
                    if (!lab.isContent(i)) continue;
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    bool boundary = false;
                    for (const auto& d : nb) {
                        const int nx = x + d[0], ny = y + d[1];
                        if (nx < 0 || w <= nx || ny < 0 || h <= ny || !lab.isContent(ny * w + nx)) {
                            boundary = true;
                        }
                    }
                    if (boundary) { depth[i] = 0; queue.push_back(i); }
                }
            }
            while (!queue.empty()) {
                const int i = queue.front(); queue.pop_front();
                const int x = i % w, y = i / w;
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : nb) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || w <= nx || ny < 0 || h <= ny) continue;
                    const int n = ny * w + nx;
                    if (lab.isContent(n) && depth[n] < 0) { depth[n] = depth[i] + 1; queue.push_back(n); }
                }
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (!lab.isContent(i) || depth[i] < 0) continue;
                const double a = rgba[(size_t)i * 4 + 3], d = depth[i];
                Stat& s = comps[lab.root(i)];
                s.rampSumA += a; s.rampSumD += d; s.rampSumAD += a * d;
                s.rampSumAA += a * a; s.rampSumDD += d * d; ++s.rampN;
            }
        }

        const std::vector<int> dist = seedDistance(rgba, w, h, seed);
        // outer edge: component pixels that touch a pixel outside the component
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (!lab.isContent(i)) {
                    continue;
                }
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                bool edge = false;
                for (const auto& d : nb) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || w <= nx || ny < 0 || h <= ny) {
                        continue;
                    }
                    if (!lab.isContent(ny * w + nx)) {
                        edge = true;
                    }
                }
                if (!edge) {
                    continue;
                }
                Stat& s = comps[lab.root(i)];
                ++s.edgePixels;
                s.edgeJump += rgba[(size_t)i * 4 + 3];
                const int dd = dist[i];
                if (dd >= 0 && (s.nearestSeed < 0 || dd < s.nearestSeed)) {
                    s.nearestSeed = dd;
                }
            }
        }

        std::vector<std::pair<int, Stat>> orphans;
        for (const auto& kv : comps) {
            if (kv.second.peak >= seed) {
                continue;
            }
            if (kv.second.pixels >= minSize) {
                orphans.emplace_back(kv.first, kv.second);
            }
        }
        if (orphans.empty()) {
            continue;
        }
        for (const auto& o : orphans) {
            const Stat& s = o.second;
            int b = 0;
            while ((1L << b) < s.pixels && b < 23) {
                ++b;
            }
            ++buckets[b];
            totalOrphanPixels += s.pixels;
            if (s.pixels >= 64) {
                ++bigOrphan;
                bigOrphanPixels += s.pixels;
            }
            // a hard outer edge: the boundary alpha is already at (or near) the component's peak
            if (s.edgePixels > 0 && s.edgeJump / s.edgePixels >= 0.9 * s.peak) {
                ++sharpOrphan;
            }
            {
                const double modalShare = s.pixels ? (double)s.modal / s.pixels : 0.0;
                const double edge = (s.edgePixels > 0 && s.peak > 0)
                    ? (s.edgeJump / s.edgePixels) / s.peak : 1.0;
                const double mean = s.pixels ? (double)s.mass / s.pixels : 0.0;
                if (s.peak == 1 && s.modal == s.pixels) { // every pixel exactly alpha 1
                    ++pure1Comps;
                    pure1Pixels += s.pixels;
                    int b = 0;
                    while (b < 15 && (1L << b) < s.pixels) ++b;
                    ++pure1Sizes[b];
                }
                int mb = (int)(mean * 4.0);
                if (mb >= kMeanBuckets) {
                    mb = kMeanBuckets - 1;
                }
                const int cut = std::min(8, (int)std::ceil(mean));
                if (modalShare >= 0.75 && s.peak <= 3) {
                    ++veilComps; veilPixels += s.pixels; veilEdge += edge; ++veilN;
                    ++veilMean[mb]; veilMass += s.mass;
                    for (int c = cut; c <= 8; ++c) veilDropByCut[c] += s.pixels;
                } else if (modalShare < 0.60) {
                    ++gradComps; gradPixels += s.pixels; gradEdge += edge; ++gradN;
                    ++gradMean[mb]; gradMass += s.mass;
                    for (int c = cut; c <= 8; ++c) gradDropByCut[c] += s.pixels;
                } else {
                    ++midComps; midPixels += s.pixels; midEdge += edge; ++midN;
                    ++midMean[mb]; midMass += s.mass;
                    for (int c = cut; c <= 8; ++c) midDropByCut[c] += s.pixels;
                }
                int rb = (int)((s.ramp() + 1.0) * 5.0);
                rb = std::max(0, std::min(9, rb));
                ++rampBuckets[rb];
            }
            biggest.push_back({s.pixels, QString("%1 @%2,%3 %4x%5 ramp%6 flat%7 peak%8 dist%9").arg(QString::fromStdString(layer.name))
                .arg(s.x0).arg(s.y0).arg(s.x1 - s.x0 + 1).arg(s.y1 - s.y0 + 1)
                .arg(s.ramp(), 0, 'f', 2).arg(s.pixels ? (double)s.modal / s.pixels : 0.0, 0, 'f', 2)
                .arg(s.peak).arg(s.nearestSeed)});
        }
        totalOrphan += (long)orphans.size();

        // crops for the largest few, so the dropped pixels can be seen rather than inferred
        if (outDir) {
            std::sort(orphans.begin(), orphans.end(), [](const auto& a, const auto& b) {
                return a.second.pixels > b.second.pixels;
            });
            for (int k = 0; k < std::min<int>(top, (int)orphans.size()); ++k) {
                const Stat& s = orphans[k].second;
                const int m = 24;
                const int cx0 = std::max(0, s.x0 - m), cy0 = std::max(0, s.y0 - m);
                const int cx1 = std::min(w - 1, s.x1 + m), cy1 = std::min(h - 1, s.y1 + m);
                const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
                QImage alpha8(cw, ch, QImage::Format_RGB888);
                QImage marked(cw, ch, QImage::Format_RGB888);
                QImage plain(cw, ch, QImage::Format_RGB888);
                const int root = orphans[k].first;
                for (int y = 0; y < ch; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        const int sx = cx0 + x, sy = cy0 + y;
                        const int i = sy * w + sx;
                        const int a = rgba[(size_t)i * 4 + 3];
                        const int amp = std::min(255, a * 8);
                        const bool inComp = lab.isContent(i) && lab.root(i) == root;
                        alpha8.setPixel(x, y, qRgb(amp, amp, amp));
                        marked.setPixel(x, y, inComp ? qRgb(255, 60, 60) : qRgb(amp, amp, amp));
                        plain.setPixel(x, y, qRgb(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1],
                                                  rgba[(size_t)i * 4 + 2]));
                    }
                }
                const QString base = QString("%1/%2_%3_px%4_peak%5_dist%6")
                    .arg(outDir).arg(QString::fromStdString(layer.name).replace('/', '_'))
                    .arg(k).arg(s.pixels).arg(s.peak).arg(s.nearestSeed);
                alpha8.scaled(cw * 4, ch * 4, Qt::IgnoreAspectRatio, Qt::FastTransformation).save(base + "_alpha_x8.png");
                marked.scaled(cw * 4, ch * 4, Qt::IgnoreAspectRatio, Qt::FastTransformation).save(base + "_marked.png");
                plain.scaled(cw * 4, ch * 4, Qt::IgnoreAspectRatio, Qt::FastTransformation).save(base + "_rgba.png");
            }
        }
    }

    std::printf("core-less components (>= %d px): %ld, %ld pixels\n", minSize, totalOrphan, totalOrphanPixels);
    std::printf("size histogram (powers of two):\n");
    for (int b = 0; b < 24; ++b) {
        if (buckets[b]) {
            std::printf("   [2^%2d, 2^%2d) : %8ld\n", b, b + 1, buckets[b]);
        }
    }
    std::printf("components >= 64 px: %ld (%ld pixels)\n", bigOrphan, bigOrphanPixels);
    std::printf("\ncomponents whose every pixel is exactly alpha 1 (mean alpha == 1): %ld (%ld px)\n",
        pure1Comps, pure1Pixels);
    std::printf("   by size: ");
    for (int b = 0; b < 16; ++b) {
        if (pure1Sizes[b]) {
            std::printf("%s%s%ld:%ld", b ? ", " : "", b == 0 ? "" : ">=", (long)(1L << b), pure1Sizes[b]);
        }
    }
    std::printf("\n\nthe dropped set splits by its own distribution:\n");
    std::printf("   uniform veil (modal share >= 0.75, peak <= 3) : %8ld comps %10ld px\n", veilComps, veilPixels);
    std::printf("   soft gradient (modal share < 0.60)           : %8ld comps %10ld px\n", gradComps, gradPixels);
    std::printf("   in between                                   : %8ld comps %10ld px\n", midComps, midPixels);
    const auto quantiles = [](const long* aHist, long aTotal, double aLo, double aHi) {
        // returns p10 / p50 / p90 of the component-count distribution of mean alpha
        if (aTotal <= 0) return std::array<double, 3>{0, 0, 0};
        const long want[3] = {aTotal / 10, aTotal / 2, (aTotal * 9) / 10};
        double out[3] = {0, 0, 0};
        long seen = 0;
        int k = 0;
        for (int b = 0; b < 48 && k < 3; ++b) {
            seen += aHist[b];
            while (k < 3 && seen >= want[k]) {
                out[k++] = aLo + (b + 0.5) * (aHi - aLo) / 48.0;
            }
        }
        return std::array<double, 3>{out[0], out[1], out[2]};
    };
    const auto vm = quantiles(veilMean, veilN, 0.0, 12.0);
    const auto gm = quantiles(gradMean, gradN, 0.0, 12.0);
    const auto mm = quantiles(midMean, midN, 0.0, 12.0);
    std::printf("   mean alpha over alpha>0 pixels, per class (component-weighted p10/p50/p90):\n");
    std::printf("      veil        %.2f / %.2f / %.2f   (px-weighted %.2f, %ld components)\n",
        vm[0], vm[1], vm[2], veilPixels ? (double)veilMass / veilPixels : 0.0, veilN);
    std::printf("      soft grad   %.2f / %.2f / %.2f   (px-weighted %.2f, %ld components)\n",
        gm[0], gm[1], gm[2], gradPixels ? (double)gradMass / gradPixels : 0.0, gradN);
    std::printf("      in between  %.2f / %.2f / %.2f   (px-weighted %.2f, %ld components)\n",
        mm[0], mm[1], mm[2], midPixels ? (double)midMass / midPixels : 0.0, midN);
    std::printf("   pixels a cutoff on mean alpha would remove, by class:\n");
    std::printf("      %-8s %12s %12s %12s\n", "cutoff", "veil px", "soft grad px", "between px");
    for (int c = 1; c <= 8; ++c) {
        std::printf("      mean<=%-2d %12ld %12ld %12ld\n", c, veilDropByCut[c], gradDropByCut[c],
            midDropByCut[c]);
    }
    std::printf("   mean boundary/peak alpha (1.0 = the edge stops at the component's own level):\n");
    std::printf("      veil %.2f   soft gradient %.2f   in between %.2f\n",
        veilN ? veilEdge / veilN : 0.0, gradN ? gradEdge / gradN : 0.0, midN ? midEdge / midN : 0.0);
    std::printf("ramp correlation (alpha vs interior depth), dropped components:\n");
    for (int b = 0; b < 10; ++b) {
        const double lo = -1.0 + b * 0.2, hi = lo + 0.2;
        std::printf("   [%+.1f,%+.1f) : %8ld\n", lo, hi, rampBuckets[b]);
    }
    std::printf("components whose outer edge is already at ~their peak (hard-edged): %ld of %ld\n",
        sharpOrphan, totalOrphan);
    std::printf("\nlargest core-less components:\n");
    std::sort(biggest.begin(), biggest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (int i = 0; i < std::min<int>(20, (int)biggest.size()); ++i) {
        std::printf("   %8ld px  %s\n", biggest[i].first, biggest[i].second.toUtf8().constData());
    }
    return 0;
}
