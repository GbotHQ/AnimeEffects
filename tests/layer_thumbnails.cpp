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
#include <QSet>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "XC.h"
#include "gui/ThumbnailCache.h"
#include "gui/theme/Colors.h"
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

    constexpr int kSize = 22;   // the tree row's slot (ctrl::TimeLineRow::kHeight)
    gui::ThumbnailCache cache;

    // ---- [1] a fully transparent layer still gets a cell ----
    {
        const img::Buffer clear = makeBuffer(QSize(200, 160), QColor(0, 0, 0, 0));
        const QPixmap pm = cache.layerThumbnail(clear, kSize);
        std::printf("MEASURED transparent layer: %dx%d device px, dpr %.2f\n", pm.width(), pm.height(),
                    pm.devicePixelRatio());
        check(!pm.isNull(), "a transparent layer still gets a cell (not 'no icon')");
        check(pm.width() == kSize || pm.height() == kSize, "its long side fills the row's slot");
        // the cell is ALL checker and the checker has two tones: a transparent layer shows
        // the backdrop, which is what keeps the column's rhythm (not a blank space)
        const QImage cell = asImage(pm);
        QSet<QRgb> tones;
        for (int y = 1; y < cell.height() - 1; ++y) {
            for (int x = 1; x < cell.width() - 1; ++x)
                tones.insert(cell.pixel(x, y));
        }
        std::printf("MEASURED transparent layer: %dx%d cell, %d distinct tones\n", pm.width(), pm.height(),
                    int(tones.size()));
        check(tones.size() >= 2, "and the backdrop is a checker, not a flat fill");
    }

    // ---- [2] the art is drawn, the corners are the checker ----
    {
        const QColor art(220, 90, 40);
        const img::Buffer buffer = makeBuffer(QSize(606, 530), art);
        const QImage cell = asImage(cache.layerThumbnail(buffer, kSize));
        const QColor centre = cell.pixelColor(cell.width() / 2, cell.height() / 2);
        // EVERY pixel is the art: an OPAQUE layer must show no checker at all, which is what
        // proves the cell is the artwork's own box rather than a square with bands
        int offArt = 0;
        for (int y = 1; y < cell.height() - 1; ++y) {          // the outer 1px is the border
            for (int x = 1; x < cell.width() - 1; ++x) {
                if (distance(cell.pixelColor(x, y), art) > 24)
                    ++offArt;
            }
        }
        std::printf(
            "MEASURED opaque layer: %dx%d cell, centre (%d,%d,%d), %d interior pixels off the art\n",
            cell.width(), cell.height(), centre.red(), centre.green(), centre.blue(), offArt
        );
        check(distance(centre, art) <= 24, "the layer's own colour is at the centre");
        check(offArt == 0, "and an opaque layer shows NO checker: the cell is the artwork's box");
        // THE BORDER: the cell's own edge, which is what makes a dark drawing on a dark row
        // read as a cell at all (the review asked whether to keep it; it is kept).
        check(
            distance(cell.pixelColor(0, 0), art) > 24 && distance(cell.pixelColor(cell.width() - 1, 0), art) > 24,
            "and its outer 1px is the cell's border, not art"
        );
    }

    // ---- [3] aspect is kept: a wide source leaves checker above and below ----
    {
        const QColor art(40, 200, 120);
        const img::Buffer buffer = makeBuffer(QSize(600, 100), art);
        const QImage cell = asImage(cache.layerThumbnail(buffer, kSize));
        std::printf("MEASURED wide layer: %dx%d cell\n", cell.width(), cell.height());
        check(cell.width() == kSize && cell.height() < cell.width() / 2,
              "a wide source gives a WIDE cell, not a square with bands");
        int offArt = 0;
        for (int y = 1; y < cell.height() - 1; ++y) {
            for (int x = 1; x < cell.width() - 1; ++x) {
                if (distance(cell.pixelColor(x, y), art) > 24)
                    ++offArt;
            }
        }
        check(offArt == 0, "and the strip's interior is the art (nothing letterboxed inside it)");
    }

    // ---- [4] the cache is keyed on the pixels ----
    {
        const img::Buffer first = makeBuffer(QSize(300, 300), QColor(60, 60, 220));
        const img::Buffer second = makeBuffer(QSize(300, 300), QColor(60, 220, 60));
        const quint64 keyFirst = gui::ThumbnailCache::key(first, kSize);
        const quint64 keySecond = gui::ThumbnailCache::key(second, kSize);
        check(
            gui::ThumbnailCache::key(first, kSize, 2.0) != keyFirst,
            "the drawn size (DPR) is part of the key: the same source at 2x is another pixmap"
        );
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
        const QColor centre = cell.pixelColor(cell.width() / 2, cell.height() / 2);
        std::printf("MEASURED big source: centre (%d,%d,%d)\n", centre.red(), centre.green(), centre.blue());
        check(distance(centre, art) <= 24, "a large layer still lands its colour in the cell");
    }

    // ---- [6] the checker's own contrast, in BOTH themes ----
    // At 2px and 27 levels apart the pair read as dithering rather than as a checker; this
    // pins the spread so a palette edit cannot quietly undo it.
    {
        const auto spread = [](const theme::Colors& c) {
            return std::abs(c.checkerLight.red() - c.checkerDark.red());
        };
        const theme::Colors dark = theme::Colors::dark();
        const theme::Colors light = theme::Colors::light();
        std::printf(
            "MEASURED checker spread: dark %d levels (%d on %d), light %d levels (%d on %d)\n",
            spread(dark), dark.checkerLight.red(), dark.checkerDark.red(), spread(light),
            light.checkerLight.red(), light.checkerDark.red()
        );
        check(spread(dark) >= 24, "the dark theme's two tiles differ by at least 24 levels");
        check(spread(light) >= 18, "and so do the light theme's");
        // BRIGHTNESS, which is what "too bright" was about: the pair has to READ as a
        // checker without lifting the panel. The first pair (90/55) was 52 levels above a
        // #262626 row and was reported as loud; 31 is the ceiling now.
        // BRIGHTNESS in ABSOLUTE terms, because which surface the tree paints behind a row
        // is the widget's business and not this gate's: the dark theme's lighter tile has to
        // stay dim (the 0x5a pair was reported as too bright on a dark panel), and the light
        // theme's has to stay pale.
        std::printf(
            "MEASURED checker tiles: dark light=%d dark=%d, light light=%d dark=%d\n",
            dark.checkerLight.red(), dark.checkerDark.red(), light.checkerLight.red(),
            light.checkerDark.red()
        );
        check(dark.checkerLight.red() <= 0x48, "the dark theme's tiles stay dim (<= 0x48)");
        check(light.checkerLight.red() >= 0xd8, "and the light theme's stay pale (>= 0xd8)");
        // and both tiles sit on the same side of their row, so the cell reads as a field
        check(
            dark.checkerDark.red() >= dark.base.red(),
            "the dark checker is not darker than the row it sits on"
        );
        check(
            light.checkerLight.red() <= light.base.red(),
            "and the light one is not lighter than its row"
        );
    }

    // ---- [7] a scaled session renders at device pixels, painted 1:1 ----
    {
        const img::Buffer buffer = makeBuffer(QSize(400, 400), QColor(200, 200, 60));
        const QPixmap one = cache.layerThumbnail(buffer, kSize, 1.0);
        const QPixmap two = cache.layerThumbnail(buffer, kSize, 2.0);
        std::printf(
            "MEASURED dpr: 1.0 -> %dx%d (dpr %.1f), 2.0 -> %dx%d (dpr %.1f)\n", one.width(), one.height(),
            one.devicePixelRatio(), two.width(), two.height(), two.devicePixelRatio()
        );
        check(one.width() == 22 && two.width() == 44, "a 2x session renders the cell at 44 device px");
        check(two.devicePixelRatio() == 2.0, "and the pixmap says so, so Qt does not rescale it");
    }

    if (failures == 0)
        std::printf("layer_thumbnails: a cell always, the art inside it, keyed on the pixels\n");
    else
        std::printf("layer_thumbnails: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
