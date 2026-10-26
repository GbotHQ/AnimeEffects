#ifndef GUI_THUMBNAILCACHE_H
#define GUI_THUMBNAILCACHE_H

#include <QHash>
#include <QPixmap>
#include <QSize>
#include "img/Buffer.h"

namespace gui {

// LAYER THUMBNAILS for the object tree: a layer's own source pixels at row size,
// cached, so the tree's repaint never scales a full image.
//
// The semantic is RECOGNITION, not "the current result": a layer's source image is what
// identifies it ("which layer is this?"), it never changes behind the user's back, and
// it needs no GL. What the matte hides is the matte chip's job, not the thumbnail's.
//
// Folders deliberately have no thumbnail. A folder has no source pixels of its own - its
// content IS a composite - so a folder thumbnail could only be a render through the real
// renderer or a second, CPU compositor that would drift from it (blend modes, filters,
// mattes, clipping). Leaves have nothing to composite, which is exactly why this is
// cheap for them and not worth doing for folders.
//
// COST, measured (2026-10-26, this machine):
//   * 606x530 source -> 18px, direct smooth:            0.408 ms
//   * 16.9 Mpx source -> 18px, direct smooth:           75.7 ms   <- a hitch
//   * 16.9 Mpx source -> 1/8 fast -> 18px smooth:        2.33 ms  <- what this does
// so a source above kSubsamplePixels is reduced by a power of two FIRST, which costs
// quality nothing at 18px and bounds the worst case at a couple of milliseconds.
class ThumbnailCache {
public:
    // The pixmap for this buffer at the given square size, scaled to fit (aspect kept).
    // Repeat calls with the same image return the same pixmap.
    QPixmap layerThumbnail(const img::Buffer& aImage, int aSize);

    // The key a caller can store beside its row to know when the image behind a
    // thumbnail has changed (a reimport replaces the buffer).
    static quint64 key(const img::Buffer& aImage, int aSize);

    void clear() { mCache.clear(); }

private:
    static QImage toImage(const img::Buffer& aImage);

    QHash<quint64, QPixmap> mCache;

    // Above this many source pixels the image is halved (repeatedly) before the smooth
    // scale: measured 32x cheaper on a 16.9 Mpx layer and indistinguishable at 18px.
    static constexpr int kSubsamplePixels = 1024 * 1024;
};

} // namespace gui

#endif // GUI_THUMBNAILCACHE_H
