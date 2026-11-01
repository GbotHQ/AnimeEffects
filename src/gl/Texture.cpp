#include <QVector2D>
#include <algorithm>
#include <cmath>
#include "gl/DeviceInfo.h"
#include "gl/EasyShaderProgram.h"
#include "gl/FrameStats.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/VertexArrayObject.h"

// see DeviceInfo.cpp: Qt's GL header set provides this through qopenglext.h today, and the guard
// covers a build whose set omits it
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

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

/// The shader that builds the mip levels glGenerateMipmap cannot be trusted to fill: one destination
/// texel is the area-weighted average of the source texels its footprint overlaps, with the
/// fractional weights at the ends. A level-to-level step is always between 2:1 and 3:1 - level l has
/// floor(side / 2^l) texels and floor(floor(side / 2^l) / 2) == floor(side / 2^(l+1)) - so the
/// footprint never spans more than 3 source texels per axis. When the step IS exactly 2:1 the four
/// weights are all 1/4, i.e. a plain 2x2 box (what a linear blit would also give).
///
/// This is the only mip-level builder left: the CPU rebuild that read the deepest exact level back
/// (glGetTexImage + a resample per level) could not be used for a size where nothing halves - a
/// 15001^2 image would read back its whole 900 MB base, and a 1023^2 one would do it for each of its
/// ten levels. The GPU path costs no readback and no sync: the tail levels together hold a third of
/// the base texels, each built with 4 fetches (2x2 step) or at most 9, i.e. a few milliseconds for a
/// 15001^2 base at import and well under a millisecond for a mask stroke's dirty box.
const char* kMipTailVertexShader = R"(#version 330
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kMipTailFragmentShader = R"(#version 330
uniform sampler2D uSrc;
uniform vec2 uSrcSize;
uniform vec2 uDstSize;
out vec4 fragColor;

void main() {
    // destination texel j (gl_FragCoord.xy is its centre, j + 0.5), and the source interval it
    // covers: [j * src / dst, (j + 1) * src / dst] in source texel coordinates
    vec2 j = floor(gl_FragCoord.xy);
    vec2 lo = j * uSrcSize / uDstSize;
    vec2 hi = (j + vec2(1.0)) * uSrcSize / uDstSize;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int ty = 0; ty < 3; ++ty) {
        int i = int(floor(lo.y)) + ty;
        if (i >= int(uSrcSize.y))
            continue;
        float oy = min(hi.y, float(i) + 1.0) - max(lo.y, float(i));
        if (oy <= 0.0)
            continue;
        for (int tx = 0; tx < 3; ++tx) {
            int k = int(floor(lo.x)) + tx;
            if (k >= int(uSrcSize.x))
                continue;
            float ox = min(hi.x, float(k) + 1.0) - max(lo.x, float(k));
            if (ox <= 0.0)
                continue;
            acc += texelFetch(uSrc, ivec2(k, i), 0) * (ox * oy);
            wsum += ox * oy;
        }
    }
    // the weights sum to the footprint's area, which normalizes them exactly
    fragColor = (wsum > 0.0) ? acc / wsum : vec4(0.0);
}
)";

/// One shader program per GL context, compiled on first use. Deliberately never destroyed: a static
/// gl object's destructor would run at process exit, after the context is gone, and deleting a
/// program while another context is current is invalid. A context recreation therefore leaks one
/// program - bounded by how many contexts a process makes, and its storage dies with the context.
gl::EasyShaderProgram* gMipTailProgram = nullptr;
QOpenGLContext* gMipTailProgramContext = nullptr;

gl::EasyShaderProgram& mipTailProgram() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (gMipTailProgram && gMipTailProgramContext == context)
        return *gMipTailProgram;

    auto* program = new gl::EasyShaderProgram();
    const bool built =
        program->setVertexSource(QString::fromLatin1(kMipTailVertexShader))
        && program->setFragmentSource(QString::fromLatin1(kMipTailFragmentShader))
        && program->link();
    XC_MSG_ASSERT(built && program->id() != 0, "mip tail shader failed: %s", qPrintable(program->log()));

    gMipTailProgram = program;
    gMipTailProgramContext = context;
    return *gMipTailProgram;
}

/// Builds levels aExactLevels+1 .. aLevelCount-1 from level aExactLevels, on the GPU, with the area
/// weights the level grids imply.
///
/// The source of every step is the level immediately above it, copied into a scratch texture first:
/// level-to-level area integration is exact (each source texel already holds the exact average of its
/// own footprint, and the overlap weights decompose that footprint exactly), while a single step from
/// the deepest exact level would need side/2^l taps per axis - 15001 of them at the deepest level of
/// a 15001^2 image. The copy also keeps the shader from sampling the texture it is rendering into
/// (a rendering feedback loop, undefined by the spec).
///
/// Every piece of GL state this touches is saved and restored: a texture is created inside paint and
/// import code that has its own framebuffer, viewport, vertex array and render state set up.
void buildMipTailOnGpu(
    GLuint aTextureId, const QSize& aSize, int aExactLevels, int aLevelCount, GLint aInternalFormat
) {
    Global::Functions& ggl = Global::functions();

    GLint prevDrawFbo = 0;
    GLint prevReadFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    GLint prevVao = 0;
    GLint prevProgram = 0;
    GLint prevActiveTexture = GL_TEXTURE0;
    GLint prevPolygonMode[2] = {GL_FILL, GL_FILL};
    GLboolean prevColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    ggl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    ggl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    ggl.glGetIntegerv(GL_VIEWPORT, prevViewport);
    ggl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    ggl.glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    ggl.glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    ggl.glGetIntegerv(GL_POLYGON_MODE, prevPolygonMode);
    ggl.glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

    struct Restore {
        GLboolean blend, depth, scissor, cull, dither, stencil;
    };
    Restore enabled = {
        ggl.glIsEnabled(GL_BLEND), ggl.glIsEnabled(GL_DEPTH_TEST), ggl.glIsEnabled(GL_SCISSOR_TEST),
        ggl.glIsEnabled(GL_CULL_FACE), ggl.glIsEnabled(GL_DITHER), ggl.glIsEnabled(GL_STENCIL_TEST),
    };

    // unit 0 is where create() left this texture bound, so that is the unit this pass may clobber
    // (and it restores the caller's unit number at the end)
    ggl.glActiveTexture(GL_TEXTURE0);

    // the scratch holds the level being read; it is never attached, so nothing is both read and
    // written
    const int srcW0 = std::max(1, aSize.width() >> aExactLevels);
    const int srcH0 = std::max(1, aSize.height() >> aExactLevels);
    GLuint scratch = 0;
    ggl.glGenTextures(1, &scratch);
    ggl.glBindTexture(GL_TEXTURE_2D, scratch);
    ggl.glTexImage2D(
        GL_TEXTURE_2D, 0, aInternalFormat, srcW0, srcH0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
    );
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    GLuint fbo = 0;
    ggl.glGenFramebuffers(1, &fbo);
    ggl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    gl::VertexArrayObject vao;
    gl::EasyShaderProgram& program = mipTailProgram();

    // fixed-function state the pass must not inherit: blending would blend the average with what the
    // level already held, the scissor/stencil would mask texels, culling could drop the full-screen
    // triangle, polygon-mode LINE would draw it as an outline, and dithering would perturb the
    // 8-bit result
    ggl.glDisable(GL_BLEND);
    ggl.glDisable(GL_DEPTH_TEST);
    ggl.glDisable(GL_SCISSOR_TEST);
    ggl.glDisable(GL_CULL_FACE);
    ggl.glDisable(GL_STENCIL_TEST);
    ggl.glDisable(GL_DITHER);
    ggl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    ggl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    program.bind();
    program.setUniformValue("uSrc", 0);
    ggl.glActiveTexture(GL_TEXTURE0);
    vao.bind();

    for (int l = aExactLevels + 1; l < aLevelCount; ++l) {
        const int srcW = std::max(1, aSize.width() >> (l - 1));
        const int srcH = std::max(1, aSize.height() >> (l - 1));
        const int dstW = std::max(1, aSize.width() >> l);
        const int dstH = std::max(1, aSize.height() >> l);

        // 1. copy the source level out, so this pass never reads what it renders into
        ggl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aTextureId, l - 1);
        XC_ASSERT(
            ggl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
        );
        ggl.glBindTexture(GL_TEXTURE_2D, scratch);
        ggl.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, srcW, srcH);

        // 2. render the destination level as the area average of that source
        ggl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aTextureId, l);
        XC_ASSERT(
            ggl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
        );
        ggl.glViewport(0, 0, dstW, dstH);
        program.setUniformValue("uSrcSize", QVector2D((float)srcW, (float)srcH));
        program.setUniformValue("uDstSize", QVector2D((float)dstW, (float)dstH));
        ggl.glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    vao.release();
    program.release();

    // 3. back to the caller's state, then release what this pass owned
    ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    ggl.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    ggl.glUseProgram(prevProgram);
    ggl.glBindVertexArray(prevVao);
    // core profile accepts only GL_FRONT_AND_BACK here, and both faces share one mode
    ggl.glPolygonMode(GL_FRONT_AND_BACK, prevPolygonMode[0]);
    ggl.glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    ggl.glActiveTexture(prevActiveTexture);
    const auto restore = [&ggl](GLenum aState, GLboolean aWasEnabled) {
        if (aWasEnabled)
            ggl.glEnable(aState);
        else
            ggl.glDisable(aState);
    };
    restore(GL_BLEND, enabled.blend);
    restore(GL_DEPTH_TEST, enabled.depth);
    restore(GL_SCISSOR_TEST, enabled.scissor);
    restore(GL_CULL_FACE, enabled.cull);
    restore(GL_DITHER, enabled.dither);
    restore(GL_STENCIL_TEST, enabled.stencil);

    ggl.glDeleteFramebuffers(1, &fbo);
    ggl.glDeleteTextures(1, &scratch);
}

} // namespace

Texture::Texture(): mId(0), mSize(), mMipmapLevels(1), mBytes(0) {}

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

        // ...and then every level whose grid does not halve its source is built on the GPU, because
        // the ones glGenerateMipmap just produced for those levels are not the averages they are
        // supposed to be. Which levels those are is arithmetic: level l has max(1, side >> l) texels
        // spanning the whole [0,1] of normalized coordinates, so it is an exact grid of 2^l-texel
        // blocks only while 2^l divides the side. For a side of 384 that holds up to level 7 and
        // stops: level 8 is a single texel over the whole side, and the only way to fill it from
        // level 7 is a 3-to-1 reduction, which glGenerateMipmap's behaviour for a source that does
        // not halve is NOT specified to get right - the GL spec defines the level sizes, not the
        // filter. Measured on the reference driver (tools/verify_mipmap's M18 mutation control):
        // level 8 of a 384-texel side is the average of rows [128, 256) - the middle third - and the
        // outer two thirds carry no weight at all, so a layer minified past that collapses to the
        // colour of a band of itself. The worst measured deviation from the grid's own box is 118 of
        // 255 (on a 1920-texel side's deepest level, where the layer is under 2 screen pixels), and
        // 17 of 255 at the first non-dividing level, where the layer can still be 7 pixels across.
        //
        // So the tail is built here, from the deepest level that DOES halve, with the area weights
        // the level grid implies - on the GPU, level by level (see buildMipTailOnGpu). Every level of
        // every chain is then the average it claims to be, for any size. A power-of-two size has no
        // such levels and pays nothing; an odd side (exactLevels 0) has nothing glGenerateMipmap can
        // be trusted for and gets every level from level 0 - which costs a copy per level and no
        // readback, so it stays cheap where the CPU resample this replaced would have read back the
        // whole base (900 MB for a 15001^2 image, and again for each level of a 1023^2 one).
        const int exactLevels =
            std::min(trailingZeroBits(mSize.width()), trailingZeroBits(mSize.height()));
        if (exactLevels + 1 < mMipmapLevels) {
            XC_ASSERT(aFormat == GL_RGBA && aChannelType == GL_UNSIGNED_BYTE);
            buildMipTailOnGpu(mId, mSize, exactLevels, mMipmapLevels, aInternalFormat);
        }
    } else {
        // Pin the chain to level 0. Without this, GL_TEXTURE_MAX_LEVEL defaults to 1000, so
        // a texture holding only level 0 is *incomplete* and any mipmap minification filter
        // samples it as opaque black instead of falling back to level 0.
        ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }

    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    XC_ASSERT(ggl.glGetError() == GL_NO_ERROR);

    if (mId != 0) {
        qint64 bytesPerPixel = 4;

        switch (aFormat) {
        case GL_RED: bytesPerPixel = 1; break;
        case GL_RG: bytesPerPixel = 2; break;
        case GL_RGB: bytesPerPixel = 3; break;
        case GL_RGBA: bytesPerPixel = 4; break;
        default: break;
        }

        // every level the chain holds, so the residency the counter reports is the residency the
        // texture occupies: level l is max(1, side >> l) texels on each axis
        mBytes = 0;
        for (int l = 0; l < mMipmapLevels; ++l) {
            mBytes += (qint64)std::max(1, mSize.width() >> l) * std::max(1, mSize.height() >> l)
                * bytesPerPixel;
        }
        FrameStats::instance().textureCreated(mBytes);
    }
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

void Texture::setFilter(GLint aParam) { setFilter(aParam, aParam); }

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

void Texture::setMaxAnisotropy(GLfloat aValue) {
    const GLfloat limit =
        DeviceInfo::validInstanceExists() ? DeviceInfo::instance().maxAnisotropy : 1.0f;
    if (limit <= 1.0f)   // the device cannot do it: leave the sampler exactly as it was
        return;

    const GLfloat value = std::max(1.0f, std::min(aValue, limit));

    Global::Functions& ggl = Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mId);
    ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, value);
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
        FrameStats::instance().textureDestroyed(mBytes);
        mBytes = 0;
        GL_CHECK_ERROR();
    }
}

} // namespace gl
