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

    /// The transparent margin every texture image is padded with, in texels, on each side.
    ///
    /// A mip level is a box of 2^l base texels, so its texel grid is aligned to multiples of 2^l
    /// in base-texel coordinates. The artwork has to sit at a multiple of 2^l for its own 2^l
    /// blocks to fall inside single level-l texels; with an odd margin it sits at an odd texel and
    /// a 2x minified pixel - whose footprint is exactly one level-1 texel - is instead split
    /// between two of them, so the level-1 read averages FOUR texels where two are wanted and the
    /// render comes out softer than plain level-0 bilinear at exactly 2x. A margin of 8 aligns
    /// levels 1..3 (2x, 4x and 8x minification), which is the range where a layer is still large
    /// on screen.
    ///
    /// The same number is what the margin is FOR: at level l a bilinear read at the artwork's edge
    /// reaches 2^l base texels outward, so level l needs a margin of 2^l texels of real transparent
    /// space to average instead of the clamp-to-border colour.
    static const int kSamplingMargin = 8;

    /// @note each width and height increase by 2 * kSamplingMargin pixels
    static XCMemBlock recreateForBiLinearSampling(XCMemBlock& aGrabbedImage, const QSize& aSize);

    /// clears the whole kSamplingMargin-wide ring, leaving the image content alone
    static void clearSamplingMargin(uint8* aImage, const QSize& aSize, const QColor& aColor);

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
