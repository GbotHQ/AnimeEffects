#ifndef IMG_UTIL_H
#define IMG_UTIL_H

#include <QSize>
#include <QRect>
#include <QColor>
#include <QImage>
#include "XC.h"
#include "util/TextUtil.h"
#include "img/PSDFormat.h"
#include "img/ResourceNode.h"

namespace img {

class Util {
public:
    static bool isShiftJisCode(const img::PSDFormat& aFormat);

    class TextFilter {
    public:
        TextFilter(const img::PSDFormat& aFormat);
        QString get(const std::string& aText) const;

    private:
        util::TextUtil::ShiftJisDecoder mShiftJisDecoder;
        bool mIsShiftJis;
    };

    static void expandAlpha1Pixel(uint8* aImage, const QSize& aSize);

    /// Returns a copy of the image whose fully transparent texels carry the RGB of the nearest
    /// opaque neighbour, so a bilinear read across the silhouette mixes the layer's own colour
    /// instead of black. Alpha is untouched. This matters for the straight-alpha consumers (the
    /// editor's mesh overlay draws the image itself); it is inert for the layer texture, which is
    /// premultiplied before upload (see ImageKey::resetTextureCache).
    ///
    /// The image is NOT padded, and that is deliberate. A mip level is a box of 2^l base texels
    /// whose grid is anchored to multiples of 2^l IN THE TEXTURE, so the artwork's own 2^l blocks
    /// fall inside single level-l texels only while its origin is a multiple of 2^l. Any fixed
    /// padding ring breaks that from level log2(margin)+1 upward: with the ring the artwork's
    /// origin sits at (margin, margin), so at level l >= log2(margin)+1 every level-l texel is
    /// half artwork and half ring, and a minified pixel - whose footprint IS one level-l texel -
    /// is answered by a mix of two of them, i.e. a box twice as wide as the level it asked for.
    /// Measured: a ring of 8 leaves level 4 (16x) off by a worst case of 111 of 255 and a mean of
    /// 10.7 on random artwork, against 0.5 and 0.47 with the artwork at the origin (M14 in
    /// tools/verify_mipmap).
    ///
    /// The transparent neighbour a bilinear read needs at the artwork's edge - at every level -
    /// comes from the sampler instead: the layer texture is created with GL_CLAMP_TO_BORDER and a
    /// transparent border colour (ImageKey::resetTextureCache), which the hardware applies to all
    /// mip levels. The padding ring predates that: it exists to give the RGB bleed above somewhere
    /// to bleed into under a clamping wrap mode.
    static XCMemBlock prepareForSampling(XCMemBlock& aGrabbedImage, const QSize& aSize);

    static void
    copyImage(uint8* aDst, const QSize& aDstSize, const QPoint& aTopLeft, const uint8* aSrc, const QSize& aSrcSize);

    static std::pair<XCMemBlock, QRect>
    createTextureImage(const PSDFormat::Header& aHeader, const PSDFormat::Layer& aLayer);
    static std::pair<XCMemBlock, QRect> createTextureImage(const QImage& aImage);

    static ResourceNode* createResourceNodes(PSDFormat& aFormat, bool aLoadImage);
    static ResourceNode* createResourceNodes(bool merged, const std::string& aFilePath, bool aLoadImage);
    static ResourceNode* createResourceNode(const QImage& aImage, const QString& aName, bool aLoadImage);
};

} // namespace img

#endif // IMG_UTIL_H
