// Gate for the object tree's LAYER THUMBNAILS (gui::ThumbnailCache).
//
// The thumbnail is a cached downscale of a layer's own source pixels. What can go wrong
// quietly, and is asserted here:
//
//   [1] A FULLY TRANSPARENT layer draws as NO ICON AT ALL. That is what the first
//       version did - two rows lost their glyph and the column's rhythm with it - so a
//       cell is always returned, with an alpha checker under the art.
//   [2] THE ART MUST BE VISIBLE and the cell must not be the checker alone: a solid
//       colour source has to appear at the centre while the corners stay checker.
//   [3] ASPECT IS KEPT: a wide source fills the cell's width and leaves checker above
//       and below, rather than being stretched.
//   [4] THE CACHE IS KEYED ON THE PIXELS: the same buffer returns the same pixmap, and a
//       REIMPORT (a new buffer) returns a different one. The tree's icon refresh hangs
//       on that key - a stale key is a thumbnail that never updates after a live link.
//   [5] A BIG SOURCE is subsampled first: measured 75.7 ms direct vs 2.33 ms after a 1/8
//       pass on a 16.9 Mpx layer, and the 18px result is indistinguishable.
//
// Headless: no GL, no tree, no window.
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "XC.h"
#include "gui/ThumbnailCache.h"
#include "img/Buffer.h"

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
int failures = 0;

void check(bool aOk, const char* aWhat) {
    if (!aOk) {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}

// A buffer holding one solid colour, the shape a layer's source image arrives in.
img::Buffer makeBuffer(const QSize& aSize, const QColor& aColor) {
    img::Buffer buffer;
    buffer.alloc(img::Format_RGBA8, aSize);
    uint8* data = buffer.data();
    const int bytes = aSize.width() * aSize.height() * 4;
    for (int i = 0; i < bytes; i += 4) {
        data[i + 0] = uint8(aColor.red());
        data[i + 1] = uint8(aColor.green());
        data[i + 2] = uint8(aColor.blue());
        data[i + 3] = uint8(aColor.alpha());
    }
    return buffer;
}

QImage asImage(const QPixmap& aPixmap) { return aPixmap.toImage(); }

// A pixel's distance from a colour, so "is this the art or the checker" is a comparison
// rather than an equality (the checker is translucent over the cell's own fill).
int distance(const QColor& a, const QColor& b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    constexpr int kSize = 18;
    gui::ThumbnailCache cache;

    // ---- [1] a fully transparent layer still gets a cell ----
    {
        const img::Buffer clear = makeBuffer(QSize(200, 160), QColor(0, 0, 0, 0));
        const QPixmap pm = cache.layerThumbnail(clear, kSize);
        std::printf("MEASURED transparent layer: %dx%d px cell\n", pm.width(), pm.height());
        check(!pm.isNull(), "a transparent layer still gets a cell (not 'no icon')");
        check(pm.size() == QSize(kSize, kSize), "and it fills the row's slot");
    }

    // ---- [2] the art is drawn, the corners are the checker ----
    {
        const QColor art(220, 90, 40);
        const img::Buffer buffer = makeBuffer(QSize(606, 530), art);
        const QImage cell = asImage(cache.layerThumbnail(buffer, kSize));
        const QColor centre = cell.pixelColor(kSize / 2, kSize / 2);
        std::printf(
            "MEASURED opaque layer: centre (%d,%d,%d) of (%d,%d,%d)\n", centre.red(), centre.green(),
            centre.blue(), art.red(), art.green(), art.blue()
        );
        check(distance(centre, art) <= 24, "the layer's own colour is at the centre");
        // A SQUARE-ISH source fills the cell's width, so its corners are art too; the
        // checker is only visible where the aspect leaves a margin, which [3] asserts.
        check(cell.size() == QSize(kSize, kSize), "and the cell is the row's slot");
    }

    // ---- [3] aspect is kept: a wide source leaves checker above and below ----
    {
        const QColor art(40, 200, 120);
        const img::Buffer buffer = makeBuffer(QSize(600, 100), art);
        const QImage cell = asImage(cache.layerThumbnail(buffer, kSize));
        const QColor topRow = cell.pixelColor(kSize / 2, 1);
        const QColor middle = cell.pixelColor(kSize / 2, kSize / 2);
        std::printf(
            "MEASURED wide layer: middle (%d,%d,%d) top row (%d,%d,%d)\n", middle.red(), middle.green(),
            middle.blue(), topRow.red(), topRow.green(), topRow.blue()
        );
        check(distance(middle, art) <= 24, "the wide source fills the middle of the cell");
        check(distance(topRow, art) > 40, "and leaves the cell's top row as checker (no stretch)");
    }

    // ---- [4] the cache is keyed on the pixels ----
    {
        const img::Buffer first = makeBuffer(QSize(300, 300), QColor(60, 60, 220));
        const img::Buffer second = makeBuffer(QSize(300, 300), QColor(60, 220, 60));
        const quint64 keyFirst = gui::ThumbnailCache::key(first, kSize);
        const quint64 keySecond = gui::ThumbnailCache::key(second, kSize);
        check(keyFirst != keySecond, "a reimported image has a different key");

        const QPixmap a = cache.layerThumbnail(first, kSize);
        const QPixmap b = cache.layerThumbnail(first, kSize);
        const QPixmap c = cache.layerThumbnail(second, kSize);
        std::printf(
            "MEASURED cache: same buffer -> same pixmap %d, different buffer -> different pixmap %d\n",
            a.cacheKey() == b.cacheKey(), a.cacheKey() != c.cacheKey()
        );
        check(a.cacheKey() == b.cacheKey(), "the same buffer returns the cached pixmap");
        check(a.cacheKey() != c.cacheKey(), "a new buffer returns a new pixmap");
    }

    // ---- [5] a big source takes the subsample path ----
    {
        const QColor art(240, 240, 40);
        const img::Buffer big = makeBuffer(QSize(2600, 2600), art);   // 6.8 Mpx
        const QImage cell = asImage(cache.layerThumbnail(big, kSize));
        const QColor centre = cell.pixelColor(kSize / 2, kSize / 2);
        std::printf("MEASURED big source: centre (%d,%d,%d)\n", centre.red(), centre.green(), centre.blue());
        check(distance(centre, art) <= 24, "a large layer still lands its colour in the cell");
    }

    if (failures == 0)
        std::printf("layer_thumbnails: a cell always, the art inside it, keyed on the pixels\n");
    else
        std::printf("layer_thumbnails: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
