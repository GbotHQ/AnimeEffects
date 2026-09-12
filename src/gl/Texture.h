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
