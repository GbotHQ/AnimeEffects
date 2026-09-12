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
