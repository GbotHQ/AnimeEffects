#ifndef CORE_MASKPYRAMID_H
#define CORE_MASKPYRAMID_H

#include <QImage>
#include <QRect>
#include <QSize>
#include <vector>
#include "XC.h"
#include "gl/Texture.h"
#include "util/NonCopyable.h"

namespace core {

/// THE MASK'S LEVELS, ON THE CPU.
///
/// A paged mask uploads its coarse chain and its pages from here: the GPU holds only what is
/// resident, and the level data lives where the authoring field does. The pyramid is CAPTURED
/// from the mask's GPU chain at build (the chain knows the resample rule, and a capture is exact
/// - see MaskMipChain), and after that a stroke's dirty box is rebuilt on the CPU: level 0 from
/// the field, one box per level above it, halved and rounded outwards. The rebuild uses the WHOLE
/// build's rule (each level from the one directly above), so a stroke's result is what a rebuild
/// would have produced up to the CPU's area arithmetic - measured at <=2 LSB against the GPU
/// chain (MX1); the chain's own box path uses the deepest exact level as its source instead,
/// which is cheaper but can differ from a rebuild at the high levels.
///
/// Why the CPU and not the GPU: the chain's levels ARE the full texture, so a GPU source defeats
/// paging; a transient chain needs the full allocation plus a boxed readback (GL 3.3 has none)
/// per stroke; and a full 4096^2 chain costs 96 ms on the GPU against 3.6 s on the CPU (Debug),
/// so the build is captured, not computed here. A stroke's box is thousands of texels.
class MaskPyramid: private util::NonCopyable {
public:
    MaskPyramid();

    /// Reads every level of aChain (R8) into the CPU. False on an empty chain.
    bool capture(const gl::Texture& aChain);
    void clear();

    bool isValid() const { return !mLevels.empty(); }
    QSize size() const { return mSizes.empty() ? QSize() : mSizes.front(); }
    int levelCount() const { return (int)mSizes.size(); }
    QSize levelSize(int aLevel) const;
    const std::vector<uint8>& levelBytes(int aLevel) const;

    /// The page fill: the page's content plus the 2-texel gutter, kPitch rows of R8. The gutter is
    /// the level's neighbouring texels; outside the LEVEL it is the nearest edge texel, not zero,
    /// because the mask's full texture is sampled GL_CLAMP_TO_EDGE (see MaskMipChain::create) -
    /// a zero gutter would darken the matte's outermost texels against the control's read.
    void fillPage(int aLevel, int aPageX, int aPageY, uint8* aDst) const;

    /// One whole level, tightly packed. Used to build the always-resident coarse chain from the
    /// same bytes the pages read.
    void copyLevel(int aLevel, uint8* aDst) const;

    /// The box one level is rebuilt over for a level-0 damage box: the same integer rule
    /// updateBox applies, exposed so the pager can upload exactly the coarse levels a stroke
    /// touched. Rounded outwards and clamped into the level.
    QRect boxAtLevel(int aLevel, const QRect& aBaseBox) const;

    /// Rebuilds the levels over the damage a stroke reports: level 0's box from the field and one
    /// box per level above it, halved and rounded outwards. The field must be Grayscale8 and the
    /// same size the pyramid was captured at.
    void updateBox(const QImage& aField, const QRect& aTexelBox);

private:
    static QSize levelSizeOf(const QSize& aBase, int aLevel);
    static QRect coveredBox(const QRect& aSrcBox, const QSize& aSrcSize, const QSize& aDstSize);
    void resampleBox(
        const std::vector<uint8>& aSrc, const QSize& aSrcSize, std::vector<uint8>& aDst,
        const QSize& aDstSize, const QRect& aDestBox
    ) const;

    std::vector<QSize> mSizes;
    std::vector<std::vector<uint8>> mLevels;
};

} // namespace core

#endif // CORE_MASKPYRAMID_H
