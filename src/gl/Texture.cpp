#include <algorithm>
#include <cmath>
#include "gl/Global.h"
#include "gl/Texture.h"

namespace gl {


Texture::Texture(): mId(0), mSize(), mMipmapLevels(1) {}

Texture::~Texture() { destroy(); }

void Texture::create(
    const QSize& aSize,
    const uint8* aData,
    GLenum aFormat,
    GLint aInternalFormat,
    GLenum aChannelType,
    MipmapPolicy aMipmap
) {
    destroy();

    XC_ASSERT(!aSize.isNull());
    // a mip chain can only be built from uploaded content
    XC_ASSERT(aMipmap != GenerateMipmap || aData != nullptr);

    mSize = aSize;
    mMipmapLevels = 1;

    Global::Functions& ggl = Global::functions();
    ggl.glGenTextures(1, &mId);
    ggl.glBindTexture(GL_TEXTURE_2D, mId);

    ggl.glTexImage2D(GL_TEXTURE_2D, 0, aInternalFormat, mSize.width(), mSize.height(), 0, aFormat, aChannelType, aData);

    if (aMipmap == GenerateMipmap) {
        ggl.glGenerateMipmap(GL_TEXTURE_2D);
        const float maxDim = (float)std::max(mSize.width(), mSize.height());
        mMipmapLevels = 1 + (int)std::floor(std::log2(maxDim));
        // State the extent of the chain explicitly instead of leaving GL_TEXTURE_MAX_LEVEL
        // at its default 1000: the sampled level range is then exactly the levels that
        // exist, rather than "whatever the driver happens to find".
        ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mMipmapLevels - 1);
    } else {
        // Pin the chain to level 0. Without this, GL_TEXTURE_MAX_LEVEL defaults to 1000, so
        // a texture holding only level 0 is *incomplete* and any mipmap minification filter
        // samples it as opaque black instead of falling back to level 0.
        ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }

    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    XC_ASSERT(ggl.glGetError() == GL_NO_ERROR);
}

void Texture::setMinFilter(GLint aParam) {
    Global::Functions& ggl = Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mId);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, aParam);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::setMagFilter(GLint aParam) {
    XC_ASSERT(aParam != GL_NEAREST_MIPMAP_NEAREST && aParam != GL_NEAREST_MIPMAP_LINEAR &&
              aParam != GL_LINEAR_MIPMAP_NEAREST && aParam != GL_LINEAR_MIPMAP_LINEAR);

    Global::Functions& ggl = Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mId);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, aParam);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::setFilter(GLint aMinParam, GLint aMagParam) {
    setMinFilter(aMinParam);
    setMagFilter(aMagParam);
}

void Texture::setWrap(GLint aParam, QColor aBorderColor) {
    Global::Functions& ggl = Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mId);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, aParam);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, aParam);

    if (aParam == GL_CLAMP_TO_BORDER) {
        const GLfloat colorF[] = {
            (GLfloat)aBorderColor.redF(),
            (GLfloat)aBorderColor.greenF(),
            (GLfloat)aBorderColor.blueF(),
            (GLfloat)aBorderColor.alphaF()};

        ggl.glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, colorF);
    }
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::destroy() {
    if (mId != 0) {
        Global::functions().glDeleteTextures(1, &mId);
        mId = 0;
        mSize = QSize();
        mMipmapLevels = 1;
        GL_CHECK_ERROR();
    }
}

} // namespace gl
