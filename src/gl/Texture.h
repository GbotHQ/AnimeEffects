#ifndef GL_TEXTURE_H
#define GL_TEXTURE_H

#include <QtOpenGL/QtOpenGL>
#include "XC.h"
#include "util/NonCopyable.h"

namespace gl {

class Texture: private util::NonCopyable {
public:
    // Whether create() also gives the texture a mipmap chain, and who fills it.
    //
    // NoMipmap: level 0 only, GL_TEXTURE_MAX_LEVEL pinned to 0, filters and wrap untouched. The
    // default, and what render targets, glyph caches and straight (non-premultiplied) data use.
    //
    // GenerateMipmap: mipmaps are only correct for data that is ALPHA-PREMULTIPLIED, because
    // glGenerateMipmap box-filters each channel independently and averaging STRAIGHT colour blends
    // the RGB of transparent texels into the edge texels of every level (the dark/light fringe once
    // the texture is minified). create() generates the levels that halve exactly and BUILDS every
    // level that does not, on the GPU, with the area weights the level grid implies (see
    // Texture.cpp) - so it needs no readback, but it does build the whole chain, with a reach-back
    // span that bounds the cost of a very large base.
    //
    // Manual: the whole chain is allocated and booked (levels 1..n-1 exist at their GL-mandated
    // sizes, GL_TEXTURE_MAX_LEVEL is n-1, mipmapLevels() and the residency counter report every
    // level), but the levels above 0 hold nothing until the caller fills them. create() builds and
    // draws nothing, and does not touch the filters or the wrap mode - the caller owns the sampling
    // policy and the per-level uploads (a mask patched a box at a time, for which a whole-chain
    // build would be the wrong unit of work).
    enum MipmapPolicy {
        NoMipmap,
        GenerateMipmap,
        Manual,
    };

    /// The rule for building a mip level with the area weights the grids imply, shared by every
    /// manual chain in the tree (this class's automatic tail, and the mask chain's whole-field
    /// build). ONE rule, so an image and a mask cannot be built to different standards.
    ///
    /// A destination texel's footprint must be integrated over the FINE grid: a step whose boundaries
    /// cut a source texel uses that source texel's average for the cut part, which is exact only when
    /// the source texel IS the finest grid (level 0). Level 0 is expensive though - each base-sourced
    /// level is a full pass over the base - so the chain reaches back only kMipChainSpan levels:
    ///
    ///   level <= 3   -> build from level 0. Exact. Three base reads.
    ///   level >= 4   -> build from level - 3: an 8:1 step whose source texels are themselves
    ///                   averages. Not exact, and MEASURED indistinguishable - 50 samples from a
    ///                   photograph / illustration / PSD / text corpus (tools/mip_compare), worst
    ///                   per-texel deviation 8/255, mean 0.34/255 over every level and sample,
    ///                   against 65/255 worst and mean 5.16 for a step-by-step chain (level - 1),
    ///                   which is what the old rule did past its anchor bound and what a driver's
    ///                   own generateMipmap does everywhere.
    ///
    /// WHY 3 AND NOT 1 (the cheaper chain): a chained level's error is its SOURCE's error averaged
    /// over a wider box, so reaching back one level keeps the source's spikes. Measured on the same
    /// corpus: chain-from-1 mean 5.16/255 on flat artwork and 0.59 on a text page; chain-from-2 only
    /// 0.56 on that page - barely better than 0.59; chain-from-3 0.18, and its delta map is almost
    /// black. Cost of the three: 1.33 base reads (span 1), 3.33 (span 3: levels 1-3 from base, then
    /// base/4 + base/16 + ...), 8.33 for the old anchor-through-level-7 rule.
    ///
    /// WHEN A CHAINED LEVEL IS EXACT, stated so it can be checked: level L is the box of the base
    /// exactly while both sides are divisible by 2^L. The first step that does not divide is where
    /// the source texel stops being an exact block - the builder must hand over a WHOLE source texel
    /// for a partial footprint, so each destination texel borrows from one neighbour and loses part of
    /// its own, i.e. a 1-source-texel smear and therefore blur, not noise. MEASURED as high-frequency
    /// energy against the exact level: level 1 identical (0.0 % on all 45 corpus samples), level 2 of a
    /// 1575x1050 image -10.6 % (its height does not divide by 4), level 2 of a 1920x1080 image +0.1 %
    /// (it does). Levels 1..3 are built from level 0 under this rule, so the blur can only start at
    /// level 4 - 16x minification - and only below the first dividing power of two.
    ///
    /// The same rule serves a mask's whole-field build; a mask stroke's BOX may anchor freely at any
    /// depth because a box is box-sized, so exactness costs nothing there.
    static const int kMipChainSpan = 3;

    /// The level to build level aLevel from: 0 while aLevel <= kMipChainSpan, else aLevel -
    /// kMipChainSpan (see kMipChainSpan).
    static int mipSourceLevel(int aLevel);

    Texture();
    ~Texture();

    void create(
        const QSize& aSize,
        const uint8* aData = nullptr,
        GLenum aFormat = GL_RGBA,
        GLint aInternalFormat = GL_RGBA8,
        GLenum aChannelType = GL_UNSIGNED_BYTE,
        MipmapPolicy aMipmap = NoMipmap
    );

    // Minification and magnification are set separately and deliberately: a mipmapped
    // minification filter must be paired with a NON-mipmap magnification filter (GL
    // rejects GL_*_MIPMAP_* for GL_TEXTURE_MAG_FILTER with GL_INVALID_ENUM), so one value
    // written to both cannot express a mipmapped texture at all.
    void setMinFilter(GLint aParam);
    void setMagFilter(GLint aParam);
    void setFilter(GLint aMinParam, GLint aMagParam);

    // Writes one value to both filters. Only for textures with no mip chain; a mipmapped
    // texture must use the two-argument form, because a mipmap enum is invalid for
    // GL_TEXTURE_MAG_FILTER.
    void setFilter(GLint aParam);

    void setWrap(GLint aParam, QColor aBorderColor = QColor(0, 0, 0, 0));

    /// Asks the sampler to filter anisotropically: up to aValue samples along the footprint's major
    /// axis, each at the level the MINOR axis asks for.
    ///
    /// Trilinear minification has to pick ONE level, and it picks it from the larger of the two
    /// screen-space derivatives, so an elongated footprint - a non-uniformly scaled layer, which this
    /// app makes in one keyframe - is filtered at the level the long axis needs and is therefore
    /// over-blurred along the short one by the ratio. Anisotropic filtering resolves that by taking
    /// several samples along the long axis, each at the level the short one asks for.
    ///
    /// Measured in the reference branch (M19 of tools/verify_mipmap), against the exact area integral
    /// of the footprint, in 255ths: a 4:1 footprint goes 4.443 -> 1.117, an 8:1 6.971 -> 0.741, a
    /// 16:1 12.683 -> 0.539, and a square one is 0.4817 at every value - the control that says this
    /// cannot make the ordinary case worse. The gain saturates at the footprint's own ratio because
    /// that is how many samples the driver takes; a large request is an upper bound, not a cost (a
    /// 4:1 footprint renders in the same 4.1 ms at 16 as at 1). Where the footprint is isotropic the
    /// anisotropic path still reads a marginally narrower kernel at FRACTIONAL levels - up to 0.155
    /// of a level, nothing at an integer one.
    ///
    /// The value is clamped to the device's limit (gl::DeviceInfo::maxAnisotropy) and is a no-op where
    /// the device reports 1, i.e. where EXT_texture_filter_anisotropic - or the ARB promotion of it -
    /// is absent. GL would clamp the value itself, but the app treats a GL error as a hard failure and
    /// there is no reason to send a value the device cannot honour.
    void setMaxAnisotropy(GLfloat aValue);

    /// Offsets the level the minification filter picks, in log2 units. Only the minimum filter uses
    /// it; magnification ignores it.
    ///
    /// A layer texture needs a negative one because of how a mip chain is read, not because of how it
    /// is built. Bilinear filtering at level m takes two taps that are each a whole level-m texel, and
    /// a level-m texel is the average over 2^m base texels, so the kernel it integrates is 2*2^m base
    /// texels wide. A footprint of F = 2^lambda base texels is therefore matched by m = lambda - 1,
    /// one level finer than the lambda = log2(F) that the derivatives pick - that value is right for
    /// a nearest read and twice too wide for a bilinear one. Measured against supersampled ground
    /// truth over the reference corpus: a bias of -0.5 lowers the mean error from 1.631 to 0.968 of
    /// 255 at 4x and from 4.198 to 2.533 at 16x, with the per-file optimum between -0.5 and -1.0 and
    /// -1.5 past the point. The gain is in the fractional-phase cases; a footprint that already lands
    /// on a level-texel centre is exact at bias 0 and is made marginally softer by this, which is why
    /// the value is half a level and not a whole one.
    void setLodBias(GLfloat aBias);

    void destroy();

    GLuint id() const { return mId; }
    QSize size() const { return mSize; }

    // 1 when no chain was built, otherwise the number of levels (including level 0).
    int mipmapLevels() const { return mMipmapLevels; }

private:
    GLuint mId;
    QSize mSize;
    int mMipmapLevels;
    qint64 mBytes;
};

} // namespace gl

#endif // GL_TEXTURE_H
