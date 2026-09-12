// Shows what an alpha cutoff on a region actually removes, as pictures and as numbers.
//
// The question this answers is not "which rule is right" but "what does this cutoff do to real
// artwork". A coverage rule of the form the artist proposed - a region's MAX or MEAN alpha
// compared against a cutoff C - is a family, and C can only be chosen by looking at what each
// value deletes. So per dropped region this tool writes a four-panel strip:
//
//   [ before ]  [ after ]  [ removed ]  [ difference x16 ]
//
// "before" is the layer composited over mid grey (what the layer contributes to the picture),
// "after" is the same with the dropped pixels taken out, "removed" paints them red, and the last
// panel amplifies the actual change by 16 so that anything the eye misses at 1:1 still shows up.
// Strips are written at a magnification so single-pixel residue is visible, and each carries the
// numbers for its region (size, peak, mean, and the per-pixel contribution it was making).
//
// usage: threshold_audit <file.psd> <outDir> [--policy max|mean] [--cut C] [--top N] [--scale S]
#include <QDir>
#include <QImage>
#include <algorithm>
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
#include "img/GridMeshCreator.h"
#include "core/ImageKey.h"
#include "core/Constant.h"

XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {

// write the three-panel comparison sheet for one layer: [ alpha x8 ] [ what the mean verdict
// cuts, red ] [ what the ratio counts: alpha==1 cyan, 2..10 yellow, above 10 white ]
bool gSheet = false;

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

struct Region {
    long pixels = 0;
    long long mass = 0;      // sum of alpha over pixels with alpha > 0
    long pxGt1 = 0;          // pixels with alpha > 1
    long long massGt1 = 0;   // sum of alpha over pixels with alpha > 1
    int peakGt1 = 0;         // max alpha over pixels with alpha > 1 (0 = none)
    int peak = 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double mean() const { return pixels ? (double)mass / pixels : 0.0; }
};

// The candidate formulas, each one a single comparison on ONE statistic of a region.
//
//   mean       (this repository's earlier variant)  drop if  mass(alpha>0) / count(alpha>0) <= C
//   max        (this repository's earlier variant)  drop if  max(alpha>0)             <= C
//   mean-gt1   (the artist's)                       drop if  count(alpha>1) == 0  OR
//                                                            mass(alpha>1)/count(alpha>1) <= C
//   max-gt1    (the artist's)                       drop if  count(alpha>1) == 0  OR
//                                                            max(alpha>1) <= C
//
// The difference that matters: the "gt1" pair computes its statistic ONLY over pixels with
// alpha > 1, so a region made entirely of alpha-1 pixels has no statistic at all and is dropped
// by construction, and a region that is a ramp of 1..k is judged on its k-level core rather than
// on the average of the ramp (which the "mean" variant dilutes towards 1).
enum class Policy { Mean, Max, MeanGt1, MaxGt1 };

bool droppedBy(const Region& aR, Policy aPolicy, int aCut) {
    switch (aPolicy) {
    case Policy::Mean:
        return aR.mean() <= (double)aCut + 1e-9;
    case Policy::Max:
        return aR.peak <= aCut;
    case Policy::MeanGt1:
        if (aR.pxGt1 == 0) {
            return true; // no pixel above 1: dropped by construction
        }
        return (double)aR.massGt1 / aR.pxGt1 <= (double)aCut + 1e-9;
    case Policy::MaxGt1:
        return aR.pxGt1 == 0 || aR.peakGt1 <= aCut;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Policy policy = Policy::Mean;
    const char* policyName = "mean";
    int cut = 2, top = 6, scale = 3;
    QStringList onlyLayers;   // --layers a,b,c : restrict the run to these layers
    const char* path = nullptr;
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--policy") == 0 && i + 1 < argc) {
            policyName = argv[++i];
            if (std::strcmp(policyName, "max") == 0) {
                policy = Policy::Max;
            } else if (std::strcmp(policyName, "mean") == 0) {
                policy = Policy::Mean;
            } else if (std::strcmp(policyName, "mean-gt1") == 0) {
                policy = Policy::MeanGt1;
            } else if (std::strcmp(policyName, "max-gt1") == 0) {
                policy = Policy::MaxGt1;
            } else {
                std::fprintf(stderr, "unknown --policy %s\n", policyName);
                return 64;
            }
        } else if (std::strcmp(a, "--layers") == 0 && i + 1 < argc) {
            onlyLayers = QString::fromLatin1(argv[++i]).split(',');
        } else if (std::strcmp(a, "--sheet") == 0) {
            gSheet = true;
        } else if (std::strcmp(a, "--cut") == 0 && i + 1 < argc) {
            cut = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--top") == 0 && i + 1 < argc) {
            top = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--scale") == 0 && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
        } else if (!path) {
            path = a;
        } else {
            outDir = a;
        }
    }
    if (!path || !outDir) {
        std::fprintf(stderr,
            "usage: %s <file.psd> <outDir> [--policy max|mean] [--cut C] [--top N] [--scale S]\n", argv[0]);
        return 64;
    }
    QDir().mkpath(outDir);
    std::ifstream file(path, std::ios::binary);
    img::PSDReader reader(file);
    if (reader.resultCode() != img::PSDReader::ResultCode_Success) {
        std::fprintf(stderr, "parse failed\n");
        return 1;
    }
    const img::PSDFormat& format = *reader.format();
    std::printf("%s   policy=%s cut=%d\n", path, policyName, cut);

    long totalRegions = 0, totalDropped = 0, totalDroppedPixels = 0;
    long long totalDroppedMass = 0;
    long largest = 0;

    for (const auto& layerPtr : format.layerAndMaskInfo().layers) {
        const img::PSDFormat::Layer& layer = *layerPtr;
        if (layer.entryType != img::PSDFormat::LayerEntryType_Layer || layer.rect.width() <= 0
            || layer.rect.height() <= 0) {
            continue;
        }
        if (!onlyLayers.isEmpty() && !onlyLayers.contains(QString::fromStdString(layer.name))) {
            continue;
        }
        const int w = layer.rect.width(), h = layer.rect.height();
        XCMemBlock block = img::PSDUtil::makeInterleavedImage(format.header(), layer, img::PSDUtil::ColorFormat_RGBA8);
        if (!block.data || block.size < (size_t)w * h * 4) {
            continue;
        }
        std::vector<uint8> rgba(block.data, block.data + block.size);

        // The ratio the artist proposed, per layer: how many faint pixels (2 < alpha <= 10) there
        // are against strong ones (alpha > 10). Note it is a LAYER statistic: for a region with no
        // strong pixel the denominator is 0 by definition, which is exactly the regions in
        // question, so the ratio cannot be formed per region - only per layer, or per region that
        // contains a strong pixel.
        long pxStrong = 0, pxMid = 0, pxOne = 0;
        long hist[256] = {};          // alpha histogram of the layer's own pixels
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const int a = rgba[i * 4 + 3];
            ++hist[a];
            if (a > 10) {
                ++pxStrong;
            } else if (a > 2) {
                ++pxMid;
            } else if (a == 1) {
                ++pxOne;
            }
        }
        // the statistics under discussion, and what a cut derived from each would remove.
        // M = mean(alpha : alpha > 2); amax = the layer's maximum alpha.
        long long massGt2 = 0, countGt2 = 0, totalInk = 0;
        int amax = 0;
        for (int t = 1; t < 256; ++t) {
            totalInk += (long long)t * hist[t];
            if (t > 2) {
                massGt2 += (long long)t * hist[t];
                countGt2 += hist[t];
            }
            if (hist[t] > 0) {
                amax = t;
            }
        }
        const double mean2 = countGt2 ? (double)massGt2 / countGt2 : 0.0;
        const auto cutAt = [&](int aT, long& aPixels, double& aInkFrac) {
            aPixels = 0;
            long long ink = 0;
            for (int t = 1; t <= aT && t < 256; ++t) {
                aPixels += hist[t];
                ink += (long long)t * hist[t];
            }
            aInkFrac = totalInk ? (double)ink / totalInk : 0.0;
        };
        // The ratio's convention when a layer has no strong pixel at all is "undefined -> treat as
        // soft" (R = 1), which is what the renderer's --ratio policy does, so the two agree by
        // construction. Computed here because both the table and the sheet below need it.
        const double rr = pxStrong ? (double)pxMid / pxStrong : 1.0;
        const auto tForAnchor = [&](double a) {
            return std::max(1, std::min(10, (int)std::lround(10.0 * (1.0 - rr / a))));
        };
        const int tR60 = tForAnchor(0.60);
        const int tR375 = tForAnchor(0.375);
        // CELLS, not pixels: the cutoff decides whether a CELL exists (a cell whose whole content
        // is at or below T stops being drawn), while a faint pixel sharing a cell with paint is
        // still drawn. So the number that predicts what the render does is cells lost, and the
        // grid is the one the importer would use (ImageKey::resetGridMesh's cell-size search).
        long cellsAll = 0, cellsLost10 = 0, cellsLostMean = 0, cellsLostRatio = 0;
        {
            int cell = std::max((int)core::ImageKey::kDefaultMeshCellSize, core::Constant::imageCellSizeMin());
            while (img::GridMeshCreator::getCellTableCount(QSize(w, h), cell) > core::Constant::imageCellCountMax()) {
                ++cell;
            }
            cell = std::min(cell, core::Constant::imageCellSizeMax());
            const int tMeanC = std::max(1, std::min(10, (int)std::lround(0.05 * mean2)));
            const int tRatioC = std::max(1, std::min(10, (int)std::lround(10.0 * (1.0 - rr / 0.60))));
            for (int by = 0; by < h; by += cell) {
                for (int bx = 0; bx < w; bx += cell) {
                    int mx = 0;
                    for (int y = by; y < std::min(h, by + cell); ++y) {
                        for (int x = bx; x < std::min(w, bx + cell); ++x) {
                            mx = std::max(mx, (int)rgba[((size_t)y * w + x) * 4 + 3]);
                        }
                    }
                    if (mx == 0) {
                        continue; // no cell here to begin with
                    }
                    ++cellsAll;
                    if (mx <= 10) ++cellsLost10;
                    if (mx <= tMeanC) ++cellsLostMean;
                    if (mx <= tRatioC) ++cellsLostRatio;
                }
            }
        }
        if (totalInk > 0) {
            long p10 = 0, pB = 0, pC = 0;
            double i10 = 0, iB = 0, iC = 0;
            const int tB = (int)std::lround(0.05 * mean2);
            const int tC = (int)std::lround(0.05 * amax);
            cutAt(10, p10, i10);
            cutAt(tB, pB, iB);
            cutAt(tC, pC, iC);
            long pR60 = 0, pR375 = 0;
            double iR60 = 0, iR375 = 0;
            cutAt(tR60, pR60, iR60);
            cutAt(tR375, pR375, iR375);
            const bool agree = (tB == 10) && (tR60 == 10);
            // the last three columns are cells LOST (the number that predicts the render), not
            // pixels removed
            std::printf("CELLS %s\t%s\t%ld\t%ld\t%ld\t%ld\n", path,
                QString::fromStdString(layer.name).toUtf8().constData(), cellsAll, cellsLost10,
                cellsLostMean, cellsLostRatio);
            std::printf("CUT %s\t%s\t%.1f\t%.4f\t%d\t%ld\t%ld\t%d\t%ld\t%d\t%ld\t%s\n",
                path, QString::fromStdString(layer.name).toUtf8().constData(), mean2, rr,
                /*T_const*/ 10, p10, p10,
                /*T_mean*/ tB, pB, tR60, pR60, agree ? "same" : "DIFFER");
            // STATS: the layer's own alpha statistics, exactly as the artist asked for them, so the
            // table can be read without decoding anything again. Five columns, per layer:
            //   amax            max(alpha) over the whole layer
            //   meanGt2         mean(alpha : alpha > 2)
            //   pxStrong        count(alpha > 10)      - what "the mesh treats this as content" means
            //   pxMid           count(2 < alpha <= 10) - the band a cutoff at 10 sits in
            //   A               pxMid / pxStrong, or -1 when the layer has no pixel above 10 (the
            //                   ratio is undefined there; the renderer treats it as fully soft)
            // plus the level each candidate rule derives from those numbers.
            const double aRaw = pxStrong ? (double)pxMid / pxStrong : -1.0;
            std::printf("STATS %s\t%s\t%d\t%.4f\t%ld\t%ld\t%.4f\t%d\t%d\t%d\n",
                path, QString::fromStdString(layer.name).toUtf8().constData(), amax, mean2,
                pxStrong, pxMid, aRaw,
                /*T_const*/ 10,
                std::max(1, std::min(10, (int)std::lround(0.05 * mean2))), tR60);
            const double ratio = pxStrong ? (double)pxMid / pxStrong : -1.0; // -1 = no strong px
            if (gSheet) {
                const int tMean = std::max(1, std::min(10, (int)std::lround(0.05 * mean2)));
                int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (rgba[((size_t)y * w + x) * 4 + 3] != 0) {
                            bx0 = std::min(bx0, x); by0 = std::min(by0, y);
                            bx1 = std::max(bx1, x); by1 = std::max(by1, y);
                        }
                    }
                }
                if (bx1 >= bx0) {
                    const int m = 4;
                    bx0 = std::max(0, bx0 - m); by0 = std::max(0, by0 - m);
                    bx1 = std::min(w - 1, bx1 + m); by1 = std::min(h - 1, by1 + m);
                    const int cw = bx1 - bx0 + 1, ch = by1 - by0 + 1;
                    int sc = 1;
                    while ((3 * cw) / sc > 1500 || ch / sc > 450) {
                        ++sc;
                    }
                    const int ow = (3 * cw) / sc, oh = ch / sc;
                    QImage sheet(std::max(1, ow), std::max(1, oh), QImage::Format_RGB888);
                    sheet.fill(QColor(25, 25, 25));
                    for (int oy = 0; oy < oh; ++oy) {
                        for (int ox = 0; ox < ow; ++ox) {
                            const int panel = ox / std::max(1, cw / sc);
                            const int sx = bx0 + (ox % std::max(1, cw / sc)) * sc;
                            const int sy = by0 + oy * sc;
                            if (sx > bx1 || sy > by1) {
                                continue;
                            }
                            const int a = rgba[((size_t)sy * w + sx) * 4 + 3];
                            // Stretch ONLY the band under discussion (1..10) across the grey
                            // range and paint everything above it white. A plain x8
                            // amplification saturates as soon as alpha >= 32, which is exactly
                            // what a soft layer is made of, so it showed a white rectangle.
                            int amp = 0;
                            if (a > 10) {
                                amp = 255;
                            } else if (a >= 1) {
                                amp = 8 + a * 24;   // 32 .. 248, one visible step per level
                            }
                            int r = amp, g = amp, b = amp;
                            if (panel == 1) {
                                if (a <= tMean) { r = 220; g = 40; b = 40; }
                            } else if (panel == 2) {
                                if (a == 1) { r = 0; g = 200; b = 220; }
                                else if (a <= 10) { r = 230; g = 210; b = 40; }
                                else { r = 255; g = 255; b = 255; }
                            }
                            sheet.setPixel(ox, oy, qRgb(r, g, b));
                        }
                    }
                    const QString name = QString("%1/stats_%2_mean%3_ratio%4_Tmean%5.png")
                        .arg(outDir).arg(QString::fromStdString(layer.name).replace('/', '_'))
                        .arg(mean2, 0, 'f', 1).arg(ratio, 0, 'f', 3).arg(tMean);
                    sheet.save(name);
                    std::printf("    sheet -> %s\n", qPrintable(name));
                }
            }
            std::printf(
                "  %-20s mean(a>2) %6.1f  ratio(2..10/>10) %6.3f  amax %3d | T=10: %8ld px %5.2f%% ink | "
                "T=0.05*mean %2d: %8ld px %5.2f%% ink\n",
                QString::fromStdString(layer.name).left(20).toUtf8().constData(), mean2, ratio, amax,
                p10, 100.0 * i10, tB, pB, 100.0 * iB);
        }

        Labeller lab(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (rgba[((size_t)y * w + x) * 4 + 3] != 0) {
                    lab.add(x, y);
                }
            }
        }
        std::unordered_map<int, Region> regions;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (!lab.isContent(i)) {
                    continue;
                }
                const int a = rgba[(size_t)i * 4 + 3];
                Region& r = regions[lab.root(i)];
                ++r.pixels;
                r.mass += a;
                r.peak = std::max(r.peak, a);
                if (a > 1) {
                    ++r.pxGt1;
                    r.massGt1 += a;
                    r.peakGt1 = std::max(r.peakGt1, a);
                }
                r.x0 = std::min(r.x0, x);
                r.y0 = std::min(r.y0, y);
                r.x1 = std::max(r.x1, x);
                r.y1 = std::max(r.y1, y);
            }
        }
        if (regions.empty()) {
            continue;
        }

        // which regions the policy removes
        std::unordered_map<int, bool> drop;
        std::vector<std::pair<long, int>> dropped;
        for (const auto& kv : regions) {
            ++totalRegions;
            if (!droppedBy(kv.second, policy, cut)) {
                continue;
            }
            drop[kv.first] = true;
            ++totalDropped;
            totalDroppedPixels += kv.second.pixels;
            totalDroppedMass += kv.second.mass;
            largest = std::max(largest, kv.second.pixels);
            dropped.push_back({kv.second.pixels, kv.first});
        }
        if (dropped.empty()) {
            continue;
        }

        // "after": the same layer with the dropped regions taken out
        std::vector<uint8> after = rgba;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (lab.isContent(i) && drop.count(lab.root(i))) {
                    after[(size_t)i * 4 + 3] = 0;
                }
            }
        }

        std::sort(dropped.begin(), dropped.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        // per layer: the proposed ratio, and what THIS cutoff removes from it. maxPeakDropped
        // bounds the visible cost of the removal without any backdrop: a pixel of alpha p can
        // change a composited result by at most p levels of 255.
        long droppedPixels = 0;
        int maxPeakDropped = 0;
        for (const auto& d : dropped) {
            droppedPixels += d.first;
            maxPeakDropped = std::max(maxPeakDropped, regions[d.second].peak);
        }
        std::printf(
            "  layer %-20s ratio(2<a<=10)/(a>10) %s   dropped %8ld px in %5zu regions   largest %7ld px   max peak dropped %3d\n",
            QString::fromStdString(layer.name).left(20).toUtf8().constData(),
            pxStrong ? QString::number((double)pxMid / pxStrong, 'f', 3).toUtf8().constData() : "  inf",
            droppedPixels, dropped.size(), dropped.empty() ? 0L : dropped.front().first, maxPeakDropped);
        for (int k = 0; k < std::min<int>(top, (int)dropped.size()); ++k) {
            const Region& r = regions[dropped[k].second];
            // A dropped region can be enormous (at a high cutoff it is an entire soft area), so
            // the crop window is CAPPED: a centered window of at most kMaxCrop per side. An
            // earlier version sized the strip from the region's bounding box and tried to
            // allocate a multi-hundred-megabyte image for a whole-layer region, which aborted.
            const int kMaxCrop = 320;
            const int m = 20;
            int cx0 = std::max(0, r.x0 - m), cy0 = std::max(0, r.y0 - m);
            int cx1 = std::min(w - 1, r.x1 + m), cy1 = std::min(h - 1, r.y1 + m);
            if (cx1 - cx0 + 1 > kMaxCrop) {
                const int mid = (cx0 + cx1) / 2;
                cx0 = std::max(0, mid - kMaxCrop / 2);
                cx1 = std::min(w - 1, cx0 + kMaxCrop - 1);
            }
            if (cy1 - cy0 + 1 > kMaxCrop) {
                const int mid = (cy0 + cy1) / 2;
                cy0 = std::max(0, mid - kMaxCrop / 2);
                cy1 = std::min(h - 1, cy0 + kMaxCrop - 1);
            }
            const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
            const bool regionBiggerThanCrop = (r.x1 - r.x0 + 1 > cw) || (r.y1 - r.y0 + 1 > ch);
            // Panels are composed by hand: QPainter::drawText needs a QGuiApplication (the
            // font database aborts without one) and this tool deliberately has none, so the
            // numbers go to stdout and into the file name instead of onto the picture.
            QImage strip(cw * 4 * scale, ch * scale, QImage::Format_RGB888);
            strip.fill(QColor(20, 20, 20));
            const auto overGrey = [](int aR, int aG, int aB, int aA, int aChannel) {
                // the layer composited over mid grey: what this layer actually contributes
                const double a = aA / 255.0;
                const int c = aChannel == 0 ? aR : (aChannel == 1 ? aG : aB);
                return (int)std::lround(c * a + 128.0 * (1.0 - a));
            };
            for (int panel = 0; panel < 4; ++panel) {
                for (int y = 0; y < ch; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        const int i = (cy0 + y) * w + (cx0 + x);
                        const int a = rgba[(size_t)i * 4 + 3];
                        const bool isDropped = lab.isContent(i) && drop.count(lab.root(i));
                        int r = 0, g = 0, b = 0;
                        if (panel == 0) {
                            r = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], a, 0);
                            g = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], a, 1);
                            b = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], a, 2);
                        } else if (panel == 1) {
                            const int alpha = after[(size_t)i * 4 + 3];
                            r = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], alpha, 0);
                            g = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], alpha, 1);
                            b = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1], rgba[(size_t)i * 4 + 2], alpha, 2);
                        } else if (panel == 2) {
                            r = isDropped ? 220 : 40;
                            g = 40;
                            b = 40;
                        } else {
                            const int alpha = isDropped ? 0 : a;
                            int amp = 0;
                            for (int c = 0; c < 3; ++c) {
                                const int v0 = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1],
                                    rgba[(size_t)i * 4 + 2], a, c);
                                const int v1 = overGrey(rgba[(size_t)i * 4], rgba[(size_t)i * 4 + 1],
                                    rgba[(size_t)i * 4 + 2], alpha, c);
                                amp = std::max(amp, std::abs(v0 - v1) * 16);
                            }
                            amp = std::min(255, amp);
                            r = g = b = amp;
                        }
                        // nearest-neighbour magnification, written directly into the strip
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                strip.setPixel((panel * cw + x) * scale + sx, y * scale + sy, qRgb(r, g, b));
                            }
                        }
                    }
                }
            }
            const QString name = QString("%1/%2_cut%3_%4_rank%5_px%6_peak%7_mean%8%9.png")
                .arg(outDir).arg(QString::fromStdString(layer.name).replace('/', '_'))
                .arg(cut).arg(policyName).arg(k).arg(r.pixels).arg(r.peak)
                .arg(r.mean(), 0, 'f', 2)
                .arg(regionBiggerThanCrop ? "_bigregion" : "");
            strip.save(name);
            std::printf("    strip %-44s region %7ld px  %4dx%-4d peak %3d mean %5.2f%s\n",
                QString("%1_%2_%3").arg(QString::fromStdString(layer.name).left(20))
                    .arg(policyName).arg(cut).toUtf8().constData(),
                r.pixels, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1, r.peak, r.mean(),
                regionBiggerThanCrop ? "  (region larger than the crop)" : "");
        }
    }

    std::printf("  regions %ld, dropped %ld (%ld px, alpha mass %lld)\n", totalRegions, totalDropped,
        totalDroppedPixels, totalDroppedMass);
    std::printf("  largest dropped region %ld px; strips in %s\n", largest, outDir);
    return 0;
}
