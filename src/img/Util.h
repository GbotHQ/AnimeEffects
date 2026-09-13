#ifndef IMG_UTIL_H
#define IMG_UTIL_H

#include <QSize>
#include <vector>
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

    /// The largest power of two a texture side is padded up to (see mipAlignedTextureSize). This
    /// bounds how much padding the alignment costs - up to 127 wasted texels per axis - and nothing
    /// else: correctness past it is gl::Texture's job (see the rebuild in Texture::create).
    static const int kMipTextureAlignmentMax = 128;

    /// The size the layer image must be UPLOADED at, for the mip chain to be a box pyramid that
    /// lines up with the artwork at every level.
    ///
    /// prepareForSampling() keeps the artwork at the texture's origin, which is the alignment
    /// condition for level l (the origin has to be a multiple of 2^l). Alignment is not the whole
    /// condition: level l has floor(size / 2^l) texels and normalized texture coordinates map [0,1]
    /// onto them, so unless the size is a multiple of 2^l the level-l grid is STRETCHED over the
    /// artwork - each of its texels covers a little more than 2^l base texels, and the displacement
    /// grows linearly to (size mod 2^l) base texels at the far edge. The chain also drops the last
    /// (size mod 2^l) base texels of that axis entirely, so the artwork's last rows and columns are
    /// represented by the neighbouring block instead of by themselves. Padding the upload up to a
    /// multiple of a power of two fixes both, and costs nothing in geometry: the mesh and the
    /// layer's rect come from the IMAGE (still aSize), while the shader divides texture coordinates
    /// by the TEXTURE's size (LayerNode sets uImageSize from areaTexture()->size()), so the
    /// artwork's texture coordinates land on [0, size/padded] by themselves.
    ///
    /// The padding is transparent and sits to the right and bottom, which is where it is needed:
    /// the sampler's border colour covers the other two sides. Each axis is aligned to the largest
    /// power of two that fits in it, capped, so a 1-pixel-wide image is not padded out to a
    /// cap-sized texture, a 1000-pixel side goes to 1024, and a 2049-pixel side goes to 2112.
    /// Levels up to the cap (128x minification) are then exact.
    ///
    /// Past the cap a level does not divide the size, and glGenerateMipmap does NOT fill it with the
    /// average its grid promises: the GL spec fixes the level sizes, not the filter, and what this
    /// driver does with a source that does not halve is weight only a band of it. Measured, level 8
    /// of a 384-texel side came out as the average of rows [128, 256) - the outer two thirds carrying
    /// no weight at all - and the deepest level of a 1920-texel side weighted 7% of the side, for a
    /// worst deviation from the grid's own box of 118 of 255 (on a layer under two screen pixels),
    /// and 17 of 255 at the first level past the cap, where the layer can still be 7 pixels across.
    /// Those levels are now REBUILT by gl::Texture::create from the deepest level that does divide,
    /// with the area weights the grid implies, so every level of every chain is the average it claims
    /// to be whatever the size - measured size by size in M18 of tools/verify_mipmap, and pinned
    /// there by a mutation test. So what the cap buys is memory, not correctness: it keeps the
    /// padding under 128 texels an axis, while the levels past it cost one small readback and a few
    /// thousand 8-bit averages.
    static QSize mipAlignedTextureSize(const QSize& aImageSize);

    /// The bytes to hand to glTexImage2D for a layer: aData (aSize texels) copied into the top-left
    /// of a zeroed aTextureSize buffer, ALPHA-PREMULTIPLIED, with the padding transparent.
    ///
    /// Premultiplied because the texture is mipmapped - see ImageKey::resetTextureCache. Returned by
    /// value because the caller owns it: this buffer is as large as the texture, and the previous
    /// XCMemBlock-returning version of this leaked it (XCMemBlock has no destructor).
    static std::vector<uint8> makeTextureUpload(
        const uint8* aData, const QSize& aSize, const QSize& aTextureSize);

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
