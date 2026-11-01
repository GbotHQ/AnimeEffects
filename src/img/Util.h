#ifndef IMG_UTIL_H
#define IMG_UTIL_H

#include <QSize>
#include <QRect>
#include <QColor>
#include <QImage>
#include <vector>
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

    /// @note each width and height increase 2 pixel
    static XCMemBlock recreateForBiLinearSampling(XCMemBlock& aGrabbedImage, const QSize& aSize);

    static void setEdgeColor(uint8* aImage, const QSize& aSize, const QColor& aColor);

    /// The largest power of two a texture side is padded up to (see mipAlignedTextureSize). This
    /// bounds how much padding the alignment costs - up to 127 wasted texels per axis - and nothing
    /// else: correctness past it is gl::Texture's job (see the rebuild in Texture::create).
    static const int kMipTextureAlignmentMax = 128;

    /// The size the layer image must be UPLOADED at, for the mip chain to be a box pyramid that
    /// lines up with the artwork at every level.
    ///
    /// Alignment is not the whole condition for an exact chain: level l has floor(size / 2^l) texels
    /// and normalized texture coordinates map [0,1] onto them, so unless the size is a multiple of
    /// 2^l the level-l grid is STRETCHED over the artwork - each of its texels covers a little more
    /// than 2^l base texels, and the displacement grows linearly to (size mod 2^l) base texels at the
    /// far edge. The chain also drops the last (size mod 2^l) base texels of that axis entirely, so
    /// the artwork's last rows and columns are represented by the neighbouring block instead of by
    /// themselves. Padding the upload up to a multiple of a power of two fixes both, and costs
    /// nothing in geometry: the mesh and the layer's rect come from the IMAGE (still aSize), while
    /// the shader divides texture coordinates by the TEXTURE's size (LayerNode sets uImageSize from
    /// areaTexture()->size()), so the artwork's texture coordinates land on [0, size/padded] by
    /// themselves.
    ///
    /// The padding is transparent and sits to the right and bottom, which is where it is needed: the
    /// sampler's border colour covers the other two sides. Each axis is aligned to the largest power
    /// of two that fits in it, capped, so a 1-pixel-wide image is not padded out to a cap-sized
    /// texture, a 1000-pixel side goes to 1024, and a 2049-pixel side goes to 2176 (17 blocks of the
    /// 128 cap). Levels up to the cap (128x minification) are then exact.
    ///
    /// Past the cap a level does not divide the size, and glGenerateMipmap does NOT fill it with the
    /// average its grid promises: the GL spec fixes the level sizes, not the filter, and what the
    /// reference driver does with a source that does not halve is weight only a band of it. Measured,
    /// level 8 of a 384-texel side came out as the average of rows [128, 256) - the outer two thirds
    /// carrying no weight at all - and the deepest level of a 1920-texel side weighted 7% of the
    /// side, for a worst deviation from the grid's own box of 118 of 255 (on a layer under two screen
    /// pixels), and 17 of 255 at the first level past the cap, where the layer can still be 7 pixels
    /// across. Those levels are now REBUILT by gl::Texture::create from the deepest level that does
    /// divide, with the area weights the grid implies, so every level of every chain is the average
    /// it claims to be whatever the size. So what the cap buys is memory, not correctness: it keeps
    /// the padding under 128 texels an axis, while the levels past it cost one readback of the
    /// deepest exact level - at most 128 texels across, because this padding makes the size a
    /// multiple of 128, and a multiple of 128 has seven trailing zero bits - plus a pass over it per
    /// level.
    ///
    /// @note the IMAGE this is given is the importers' buffer, which still carries
    /// recreateForBiLinearSampling's 1-texel transparent ring around the artwork. That ring puts the
    /// artwork's own texels at offset (1, 1) of the texture, i.e. off the multiple of 2^l the level-l
    /// grid is anchored to, so from level 1 upward a level-l texel straddles two of the artwork's 2^l
    /// blocks and a minified read averages a kernel up to twice the width the level asks for (the
    /// reference branch measured that: an odd origin is off by 112 of 255 at 2x). Removing the ring
    /// is the reference's fix, but it lives in the importers (ImageFileLoader and the rects the
    /// importers hand out), not here, so this change keeps the ring and the alignment it costs.
    static QSize mipAlignedTextureSize(const QSize& aImageSize);

    /// The bytes to hand to glTexImage2D for a layer: aData (aSize texels) copied into the top-left
    /// of a zeroed aTextureSize buffer, ALPHA-PREMULTIPLIED, with the padding transparent.
    ///
    /// Premultiplied because the texture is mipmapped - see ImageKey::resetTextureCache, and
    /// PREMULTIPLIED_INPUT in the layer shader, which divides the coverage back out. Returned by
    /// value because the caller owns it: this buffer is as large as the texture.
    static std::vector<uint8>
    makeTextureUpload(const uint8* aData, const QSize& aSize, const QSize& aTextureSize);

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
