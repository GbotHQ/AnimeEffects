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
    // Texture.cpp) - so it needs no readback, but it does build the whole chain, with a tap cap that
    // bounds the cost of a very large base.
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

    /// The anchor rule for building a mip level with the area weights the grids imply, shared by
    /// every manual chain in the tree (this class's automatic tail, and the mask chain's whole-field
    /// build). ONE constant and ONE rule, so an image and a mask cannot be built to different
    /// standards.
    ///
    /// Level aLevel of a texture of aSize has max(1, side >> aLevel) texels over the whole [0, 1] of
    /// normalized coordinates, and a destination texel's footprint must be integrated over the FINE
    /// grid: a step whose boundaries cut a source texel uses that texel's average for the cut part,
    /// which is exact only when the source texel IS the finest grid (level 0). So the source is
    /// THE RULE, plainly:
    ///
    ///   step halves exactly        -> chain from the level above. A 2x2 box IS the true average.
    ///                                 This is the normal case and costs 4 taps per output texel.
    ///   step does not halve,       -> ANCHOR: rebuild that level from level 0 with the exact area
    ///   ratio (base/dst) <= 128       weights. A chained step there has a fractional ratio
    ///                                 (25 -> 12 means each output texel covers 25/12 = 2.08 input
    ///                                 texels, and those inputs are already block averages), so it
    ///                                 is not the box average. Anchoring is exact; it costs ONE
    ///                                 BASE READ per level: ratio^2 taps per output texel over
    ///                                 base/ratio^2 output texels.
    ///   step does not halve,       -> chain anyway. Approximate, and bounded: MEASURED at a
    ///   ratio > 128                   25 -> 12 step against the exact base-weighted average, worst
    ///                                 deviation 17/255 on white noise (independent texels, the
    ///                                 worst case for any averaging) and 6/255 on curved artwork,
    ///                                 <= 5/255 past the bound.
    ///
    /// WHY 128 AND NOT MORE: the ratio of level L is base/(base >> L) ~ 2^L, so "ratio <= 128" is
    /// "level <= 7". Anchoring costs a base pass per level, so the constant is simply HOW MANY BASE
    /// PASSES WE PAY AT IMPORT: <= 7 here, against ~14 for a 15001^2 field whose sides never halve.
    /// It is a cost knob, not a physical limit - raise it if import time ever stops mattering.
    ///
    /// WHY THE ERROR IS ACCEPTABLE THERE: a level with ratio > 128 is only sampled when the layer
    /// is drawn at less than 1/128 of its size, where one screen pixel already covers 128+ texels.
    /// Every ordinary zoom reads levels <= 7, and those are exact.
    ///
    /// The same rule serves a mask's whole-field build; a mask stroke's BOX may anchor freely at any
    /// depth because a box is box-sized, so exactness costs nothing there.
    static const int kMipTailMaxTaps = 128;

    /// The level to build level aLevel from: 0, or aLevel - 1 (see kMipTailMaxTaps).
    static int mipTailSourceLevel(const QSize& aSize, int aLevel);

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
