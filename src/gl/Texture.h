#ifndef GL_TEXTURE_H
#define GL_TEXTURE_H

#include <QtOpenGL/QtOpenGL>
#include "XC.h"
#include "util/NonCopyable.h"

namespace gl {

class Texture: private util::NonCopyable {
public:
    // Whether create() also builds the mipmap chain. Opt-in, because mipmaps are only
    // correct for data that is ALPHA-PREMULTIPLIED: glGenerateMipmap box-filters each
    // channel independently, so averaging STRAIGHT color blends the RGB of transparent
    // texels into the edge texels of every level, which renders as a dark (or light)
    // fringe once the texture is minified. Render targets and glyph caches must not ask
    // for it.
    enum MipmapPolicy {
        NoMipmap,
        GenerateMipmap,
    };

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
    // written to both cannot express a mipmapped texture at all. The previous
    // one-argument setFilter() did exactly that, which is why the mipmapped path could
    // never have worked as written.
    void setMinFilter(GLint aParam);
    void setMagFilter(GLint aParam);
    void setFilter(GLint aMinParam, GLint aMagParam);

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
    /// Measured in M19 of tools/verify_mipmap, against the exact area integral of the footprint, in
    /// 255ths: a 4:1 footprint goes 4.443 -> 1.117, an 8:1 6.971 -> 0.741, a 16:1 12.683 -> 0.539,
    /// and a square one is 0.4817 at every value - the control that says this cannot make the
    /// ordinary case worse. The gain saturates at the footprint's own ratio because that is how many
    /// samples the driver takes; a large request is an upper bound, not a cost (a 4:1 footprint
    /// renders in the same 4.1 ms at 16 as at 1). Where the footprint is isotropic the anisotropic
    /// path still reads a marginally narrower kernel at FRACTIONAL levels - up to 0.155 of a level,
    /// nothing at an integer one - which the shipped corpus eval absorbs to within 0.0003 of 255.
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
    /// truth over eight corpus files (tools/verify_mipmap/README.md): a bias of -0.5 lowers the mean
    /// error from 1.631 to 0.968 of 255 at 4x and from 4.198 to 2.533 at 16x, with the per-file
    /// optimum between -0.5 and -1.0 and -1.5 past the point. The gain is in the fractional-phase
    /// cases; a footprint that already lands on a level-texel centre is exact at bias 0 and is made
    /// marginally softer by this, which is why the value is half a level and not a whole one.
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
};

} // namespace gl

#endif // GL_TEXTURE_H
