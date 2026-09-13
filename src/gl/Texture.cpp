#include <algorithm>
#include <cmath>
#include <vector>
#include "gl/Global.h"
#include "gl/Texture.h"

namespace gl {

namespace {

/// The number of trailing zero bits of a side, i.e. the largest l for which 2^l divides it. Level l
/// of a texture only has an exact grid - floor(side / 2^l) texels of exactly 2^l base texels each,
/// anchored at the texture's origin - while l is at most this.
int trailingZeroBits(int aValue) {
    int n = 0;
    while (aValue > 1 && (aValue & 1) == 0) {
        aValue >>= 1;
        ++n;
    }
    return n;
}

/// How much of each source texel each destination texel covers, when aSrc texels are resampled onto
/// aDst texels over the same span. Destination texel j covers [j/dst, (j+1)/dst] of that span, so
/// source texel i contributes the length of the overlap with [i/src, (i+1)/src]. The weights of one
/// destination texel sum to 1 by construction.
std::vector<std::vector<std::pair<int, double>>> axisWeights(int aSrc, int aDst) {
    std::vector<std::vector<std::pair<int, double>>> w(aDst);
    for (int j = 0; j < aDst; ++j) {
        const double lo = (double)j * aSrc / aDst;
        const double hi = (double)(j + 1) * aSrc / aDst;
        const int i0 = (int)std::floor(lo);
        const int i1 = std::min(aSrc - 1, (int)std::ceil(hi) - 1);
        for (int i = i0; i <= i1; ++i) {
            const double overlap = std::min(hi, (double)i + 1.0) - std::max(lo, (double)i);
            if (overlap > 0.0)
                w[j].emplace_back(i, overlap / (hi - lo));
        }
    }
    return w;
}

/// The area-weighted resample of one RGBA8 level onto a coarser grid covering the same span. Exact
/// for a box: every destination texel is the average of its own footprint, with the fractional
/// weights at the ends. Separable, so it is a horizontal pass into doubles and then a vertical one.
std::vector<uint8> resampleLevel(
    const std::vector<uint8>& aSrc, int aSrcW, int aSrcH, int aDstW, int aDstH
) {
    const auto wx = axisWeights(aSrcW, aDstW);
    const auto wy = axisWeights(aSrcH, aDstH);

    std::vector<double> rows((size_t)aDstW * aSrcH * 4, 0.0);
    for (int y = 0; y < aSrcH; ++y) {
        for (int j = 0; j < aDstW; ++j) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (const auto& t : wx[j]) {
                const uint8* p = aSrc.data() + ((size_t)y * aSrcW + t.first) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += p[c] * t.second;
            }
            for (int c = 0; c < 4; ++c)
                rows[((size_t)y * aDstW + j) * 4 + c] = acc[c];
        }
    }

    std::vector<uint8> dst((size_t)aDstW * aDstH * 4, 0);
    for (int i = 0; i < aDstH; ++i) {
        for (int x = 0; x < aDstW; ++x) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            for (const auto& t : wy[i]) {
                const double* p = rows.data() + ((size_t)t.first * aDstW + x) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += p[c] * t.second;
            }
            uint8* q = dst.data() + ((size_t)i * aDstW + x) * 4;
            for (int c = 0; c < 4; ++c)
                q[c] = (uint8)std::min(255.0, std::max(0.0, std::floor(acc[c] + 0.5)));
        }
    }
    return dst;
}

} // namespace

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

        // ...and then every level whose grid does not divide the size is rebuilt, because the ones
        // glGenerateMipmap just produced for those levels are not the averages they are supposed to
        // be. Which levels those are is arithmetic: level l has max(1, side >> l) texels spanning
        // the whole [0,1] of normalized coordinates, so it is an exact grid of 2^l-texel blocks only
        // while 2^l divides the side. For a side of 384 that holds up to level 7 and stops: level 8
        // is a single texel over the whole side, and the only way to fill it from level 7 is a 3-to-1
        // reduction, which glGenerateMipmap's behaviour for a source that does not halve is NOT
        // specified to get right - the GL spec defines the level sizes, not the filter. Measured on
        // this driver (M18 in tools/verify_mipmap): level 8 of a 384-texel side is the average of
        // rows [128, 256) - the middle third - and the outer two thirds carry no weight at all, so a
        // layer minified past the cap collapses to the colour of a band of itself. The worst measured
        // deviation from the grid's own box is 118 of 255 (on a 1920-texel side's deepest level,
        // where the layer is under 2 screen pixels), and 17 of 255 at the first non-dividing level,
        // where the layer can still be 7 pixels across.
        //
        // So: read the deepest level that DOES divide, and build the levels past it here, with the
        // area weights the level grid implies. That makes every level of every chain the exact
        // average it claims to be, for any size, and it costs one small readback - the deepest exact
        // level is at most 128ish texels across for any texture larger than that - plus a few
        // thousand 8-bit averages. A power-of-two size has no such levels and pays nothing.
        const int exactLevels =
            std::min(trailingZeroBits(mSize.width()), trailingZeroBits(mSize.height()));
        if (exactLevels + 1 < mMipmapLevels) {
            XC_ASSERT(aFormat == GL_RGBA && aChannelType == GL_UNSIGNED_BYTE);
            int w = std::max(1, mSize.width() >> exactLevels);
            int h = std::max(1, mSize.height() >> exactLevels);
            std::vector<uint8> level((size_t)w * h * 4);
            // the internal format is kept (it is what the sampler reads); only the client-side
            // layout of the rebuilt levels is fixed at RGBA8, which is what the readback gives
            ggl.glGetTexImage(GL_TEXTURE_2D, exactLevels, GL_RGBA, GL_UNSIGNED_BYTE, level.data());
            for (int l = exactLevels + 1; l < mMipmapLevels; ++l) {
                const int dw = std::max(1, mSize.width() >> l);
                const int dh = std::max(1, mSize.height() >> l);
                // from the exact level EVERY time, not from the level above: a level-l texel's
                // footprint is side / (texels of level l) base texels, which need not be a whole
                // number of level-(l-1) texels. Chaining the resamples would spread the fractional
                // remainder of a footprint over a whole texel of the level below, which is the same
                // too-wide-support error this rebuild exists to remove (measured: -13 of 255 at
                // level 8 of a 1920-texel side when chained, 0 when taken from the exact level).
                std::vector<uint8> next = resampleLevel(level, w, h, dw, dh);
                ggl.glTexImage2D(
                    GL_TEXTURE_2D, l, aInternalFormat, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    next.data()
                );
            }
        }
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

void Texture::setLodBias(GLfloat aBias) {
    Global::Functions& ggl = Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mId);
    ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, aBias);
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
