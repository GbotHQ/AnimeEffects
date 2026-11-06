#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include "core/ImageKey.h"
#include "core/MaskPyramid.h"
#include "core/VirtualTexture.h"
#include "gl/EasyShaderProgram.h"
#include "gl/Global.h"
#include "gl/VertexArrayObject.h"

namespace core {

namespace {

    //------------------------------------------------------------------------------
    // THE DEMAND PASS
    //------------------------------------------------------------------------------
    //
    // One triangle -> the set of pages the fragment shader may sample for it, marked in the
    // resource's demand map. It runs once per drawn layer, before the layer's own draw, on the
    // skinned position buffer the draw uses; it needs no readback of its own and produces no image.
    //
    // The level it computes is the level sampleLayer() will pick. The fragment shader derives
    // d(texel)/d(screen) from the interpolated uv; for an affine triangle that 2x2 matrix is the
    // inverse of the triangle's own d(screen)/d(texel), so the footprint's minor singular value is
    // 1 / s_max(screen Jacobian). Both sides round that log2 the same way and add the same bias -
    // see LayerDrawingFrag.glsl's sampleLayer, which is the authority on the formula.
    const char* const kDemandVertexShader = R"(#version 330
in vec3 inPosition;
in vec2 inTexCoord;
uniform mat4 uViewMatrix;
uniform vec2 uScreenSize;
// MASKS sample a screen->content affine, not the mesh's own uv (LayerSampling.glsl's
// sampleMask): the demand must name the pages THAT mapping reaches, so the vertex shader
// computes the mask's texel-space coordinate from the SAME matrix the fragment shader uses.
uniform int uUvFromScreen;
uniform mat4 uScreenToContent;
uniform vec4 uMaskRect;
uniform vec2 uMaskTexelSize;
out vec2 vScreen;
out vec2 vTexCoord;
void main() {
    // DEVICE pixels - the space the layer shader's derivatives are taken in, and exactly the
    // layer vertex shader's vDestCoord (the matrix uniforms are built for that convention).
    vec4 p = uViewMatrix * vec4(inPosition, 1.0);
    vec3 ndc = p.xyz / p.w;
    vScreen = (ndc.xy + 1.0) * uScreenSize * 0.5;
    if (uUvFromScreen != 0) {
        vec2 q = (uScreenToContent * vec4(vScreen, 0.0, 1.0)).xy;
        vTexCoord = ((q - uMaskRect.xy) / uMaskRect.zw) * uMaskTexelSize;
    } else {
        vTexCoord = inTexCoord;
    }
    gl_Position = vec4(0.0);
}
)";

    const char* const kDemandGeometryShader = R"(#version 330
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
in vec2 vScreen[];
in vec2 vTexCoord[];
uniform vec2 uLayerSize;
uniform float uBias;
uniform int uMaxLevel;
uniform int uCoarseLevel;
uniform vec2 uPageMapSize;
uniform int uPageLevelRow[16];
uniform float uMinUvArea;
uniform float uPageTexels;
uniform vec2 uTexCoordOffset;
uniform int uDemandLayer;

void emitPageRect(ivec2 aMin, ivec2 aMax, int aRow) {
    // the shared demand array: this resource's page grid is one layer of it
    gl_Layer = uDemandLayer;
    vec2 size = uPageMapSize;
    float x0 = 2.0 * float(aMin.x) / size.x - 1.0;
    float x1 = 2.0 * float(aMax.x + 1) / size.x - 1.0;
    float y0 = 2.0 * float(aRow + aMin.y) / size.y - 1.0;
    float y1 = 2.0 * float(aRow + aMax.y + 1) / size.y - 1.0;
    gl_Position = vec4(x0, y0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(x1, y0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(x0, y1, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(x1, y1, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}

void markLevel(int pageLevel, float aTapReach, vec2 t0, vec2 t1, vec2 t2) {
    if (pageLevel < 0 || pageLevel >= uCoarseLevel)
        return;
    int w = max(1, int(uLayerSize.x) >> pageLevel);
    int h = max(1, int(uLayerSize.y) >> pageLevel);
    ivec2 grid = ivec2((w + int(uPageTexels) - 1) / int(uPageTexels),
                       (h + int(uPageTexels) - 1) / int(uPageTexels));
    float levelTexels = exp2(float(pageLevel));
    float scale = levelTexels * uPageTexels;
    vec2 p0 = t0 / scale, p1 = t1 / scale, p2 = t2 / scale;
    vec2 reach = vec2(min(aTapReach, uPageTexels) / uPageTexels);
    vec2 lo = min(min(p0, p1), p2) - reach;
    vec2 hi = max(max(p0, p1), p2) + reach;
    ivec2 minP = ivec2(floor(lo));
    ivec2 maxP = ivec2(floor(hi));
    minP = max(minP, ivec2(0));
    maxP = min(maxP, grid - 1);
    if (minP.x > maxP.x || minP.y > maxP.y)
        return;
    emitPageRect(minP, maxP, uPageLevelRow[pageLevel]);
}

void main() {
    // the layer shader samples (inTexCoord + uTexCoordOffset) / uImageSize; the demand must
    // name the pages THOSE coordinates fall in, not the untranslated mesh uvs (a mesh origin
    // offset or an edited image offset moves the sample by a whole part of the image)
    vec2 t0 = vTexCoord[0] + uTexCoordOffset, t1 = vTexCoord[1] + uTexCoordOffset,
         t2 = vTexCoord[2] + uTexCoordOffset;
    vec2 d1 = t1 - t0, d2 = t2 - t0;
    float detU = d1.x * d2.y - d2.x * d1.y;
    float uvArea = abs(detU) * 0.5;
    // the same sliver rule as the density meter: a triangle under a quarter of a uv pixel has
    // no visible surface to own a page, and its ratios are float noise
    if (uvArea < uMinUvArea)
        return;

    vec2 e1 = vScreen[1] - vScreen[0];
    vec2 e2 = vScreen[2] - vScreen[0];
    vec2 j1 = (e1 * d2.y - e2 * d1.y) / detU;
    vec2 j2 = (e2 * d1.x - e1 * d2.x) / detU;
    float a = dot(j1, j1) + dot(j2, j2);
    float det = j1.x * j2.y - j2.x * j1.y;
    float disc = max(a * a - 4.0 * det * det, 0.0);
    float smax = sqrt(max(0.5 * (a + sqrt(disc)), 0.0));
    float smin = sqrt(max(0.5 * (a - sqrt(disc)), 0.0));
    if (!(smax > 0.0))
        return;

    float lod = log2(1.0 / smax) + uBias;
    // the magnified path in the fragment shader returns before picking a level: it samples
    // level 0, so that is the page this triangle wants
    int lodLevel = (lod <= 0.0) ? 0 : int(clamp(floor(lod + 0.5), 0.0, float(uMaxLevel)));

    // MARK THE LEVEL THIS TRIANGLE COMPUTES, and only that level. The fragment shader's level
    // comes from derivatives; on a 2x2 quad that straddles two triangles it is a mix of the two
    // planes, so it rounds to one of THESE TRIANGLES' levels, and both triangles mark their own.
    // A one-level margin each side was built and measured: on nene it demanded 474 pages instead
    // of 79, and it did NOT improve the frame's difference from the full-resolution control
    // (mean 0.83 against 0.61 for this tighter set); the finer neighbour also costs four pages
    // for every one of this level. The measured residual is not a missing level - see the
    // design doc's open item.
    float aniso = smax / max(smin, 1.0e-6);
    markLevel(lodLevel, 0.5 * aniso + 1.0, t0, t1, t2);
}
)";

    const char* const kDemandFragmentShader = R"(#version 330
out float oDemand;
void main() { oDemand = 1.0; }
)";

    /// THE SHARED DEMAND MAP: one R8 layer of a 2D array per paged resource, one clear, one
    /// readback and one fence per FRAME however many resources there are.
    ///
    /// The per-resource map was the obvious shape, and the measurement killed it: on nene (105
    /// paged resources) 105 glGetTexImage calls and 105 fences cost ~40 ms of a 94 ms frame, while
    /// the geometry pass that marks them costs ~8 ms. One array makes the copy one command and the
    /// fence one; the parsing stays per resource, because the layers are read back in layer order
    /// and each resource knows its own grid.
    ///
    /// The array grows by reallocation, never by copy: it is cleared every frame, so a lost
    /// previous frame's marks are harmless (the demand is a set of page timestamps, not state the
    /// GPU must preserve). A resource keeps its layer index across a growth.
    class DemandMap {
    public:
        enum { kMaxLayers = 256 };

        struct Params {
            QSize layerSize;
            int coarseLevel = 0;
            int maxLevel = 0;
            float bias = 0.0f;
            int levelRow[VirtualTexture::kMaxPageLevels] = {};
            // a MASK's uv is the fragment shader's screen->content mapping, not the mesh's own
            // texture coordinates (see the vertex shader)
            bool uvFromScreen = false;
            QMatrix4x4 screenToContent;
            QVector4D maskRect;
            QVector2D maskTexelSize;
        };

        static DemandMap& instance() {
            static DemandMap* map = new DemandMap();
            return *map;
        }

        /// A layer for a resource's page grid; holes are reused. Reallocates the array when the
        /// grid is larger than the current cell or no layer is free.
        int allocateLayer(const QSize& aGrid) {
            if (!ensureBuilt())
                return -1;
            int layer = -1;
            if (!mFree.empty()) {
                layer = mFree.back();
                mFree.pop_back();
                mGrids[(size_t)layer] = aGrid;
            } else {
                if ((int)mGrids.size() >= kMaxLayers) {
                    qWarning("VirtualTexture: demand map is full at %d layers", kMaxLayers);
                    return -1;
                }
                layer = (int)mGrids.size();
                mGrids.push_back(aGrid);
            }
            ensureCapacity((int)mGrids.size(), aGrid);
            return layer;
        }

        void releaseLayer(int aLayer) {
            if (aLayer < 0 || aLayer >= (int)mGrids.size())
                return;
            mGrids[(size_t)aLayer] = QSize();
            mFree.push_back(aLayer);
        }

        QSize grid(int aLayer) const {
            if (aLayer < 0 || aLayer >= (int)mGrids.size())
                return QSize();
            return mGrids[(size_t)aLayer];
        }

        QSize cellSize() const { return mCell; }
        int capacity() const { return mCapacity; }
        int stride() const { return mCell.width(); }
        quint64 mappedFrame() const { return mMappedFrame; }
        bool isMapped() const { return mMapped != nullptr; }

        const uint8* layerData(int aLayer) const {
            if (!mMapped || aLayer < 0 || aLayer >= mCapacity)
                return nullptr;
            return mMapped + (size_t)aLayer * mCell.width() * mCell.height();
        }

        /// Opens the frame: the array is cleared ONCE for every resource. `mark` then only draws.
        void beginFrame(quint64 aFrame) {
            mFrame = aFrame;
            if (mClearedFrame == aFrame || !ensureBuilt())
                return;
            // no resource has allocated a layer yet: there is no array to clear
            if (mTexture == 0 || mCell.isEmpty()) {
                mClearedFrame = aFrame;
                return;
            }
            auto& ggl = gl::Global::functions();
            GLint prevDrawFbo = 0;
            GLint prevViewport[4] = {0, 0, 0, 0};
            GLboolean prevColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            ggl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
            ggl.glGetIntegerv(GL_VIEWPORT, prevViewport);
            ggl.glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

            bindFramebuffer();
            ggl.glViewport(0, 0, mCell.width(), mCell.height());
            ggl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            ggl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            ggl.glClear(GL_COLOR_BUFFER_BIT);

            ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
            ggl.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
            ggl.glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
            mClearedFrame = aFrame;
        }

        /// Marks one resource's pages into its layer. `aViewMatrix` and `aDeviceSize` are the
        /// draw's own; the page map's NDC uses the ARRAY CELL, so the layer's sub-rect lands at
        /// its grid position and the rest stays cleared.
        void mark(
            int aLayer,
            const QSize& aGrid,
            const Params& aParams,
            const QMatrix4x4& aViewMatrix,
            const QSize& aDeviceSize,
            const QVector2D& aTexCoordOffset,
            gl::BufferObject& aPositions,
            const gl::Vector2* aTexCoords,
            int aVtxCount,
            gl::BufferObject& aIndexBuffer,
            int aIndexCount
        ) {
            if (aLayer < 0 || aGrid.isEmpty() || aVtxCount <= 0 || aIndexCount < 3
                || (!aTexCoords && !aParams.uvFromScreen))
                return;
            if (!ensureBuilt() || !ensureCapacity((int)mGrids.size(), aGrid))
                return;

            auto& ggl = gl::Global::functions();
            GLint prevDrawFbo = 0;
            GLint prevViewport[4] = {0, 0, 0, 0};
            GLint prevVao = 0;
            GLint prevProgram = 0;
            GLboolean prevBlend = ggl.glIsEnabled(GL_BLEND);
            GLboolean prevDepth = ggl.glIsEnabled(GL_DEPTH_TEST);
            GLboolean prevScissor = ggl.glIsEnabled(GL_SCISSOR_TEST);
            GLboolean prevCull = ggl.glIsEnabled(GL_CULL_FACE);
            GLboolean prevColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            ggl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
            ggl.glGetIntegerv(GL_VIEWPORT, prevViewport);
            ggl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
            ggl.glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
            ggl.glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

            bindFramebuffer();
            ggl.glViewport(0, 0, mCell.width(), mCell.height());
            ggl.glDisable(GL_BLEND);
            ggl.glDisable(GL_DEPTH_TEST);
            ggl.glDisable(GL_SCISSOR_TEST);
            ggl.glDisable(GL_CULL_FACE);
            ggl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            mProgram->bind();
            mProgram->setUniformValue("uViewMatrix", aViewMatrix);
            mProgram->setUniformValue(
                "uScreenSize", QVector2D((float)aDeviceSize.width(), (float)aDeviceSize.height())
            );
            mProgram->setUniformValue(
                "uLayerSize", QVector2D((float)aParams.layerSize.width(), (float)aParams.layerSize.height())
            );
            mProgram->setUniformValue("uBias", aParams.bias);
            mProgram->setUniformValue("uMaxLevel", aParams.maxLevel);
            mProgram->setUniformValue("uCoarseLevel", aParams.coarseLevel);
            mProgram->setUniformValue(
                "uPageMapSize", QVector2D((float)mCell.width(), (float)mCell.height())
            );
            mProgram->setUniformValueArray(
                "uPageLevelRow", aParams.levelRow, VirtualTexture::kMaxPageLevels
            );
            mProgram->setUniformValue("uMinUvArea", 0.25f);
            mProgram->setUniformValue("uPageTexels", (float)gl::PagePool::kPageSize);
            mProgram->setUniformValue("uTexCoordOffset", aTexCoordOffset);
            mProgram->setUniformValue("uDemandLayer", aLayer);
            mProgram->setUniformValue("uUvFromScreen", aParams.uvFromScreen ? 1 : 0);
            mProgram->setUniformValue("uScreenToContent", aParams.screenToContent);
            mProgram->setUniformValue("uMaskRect", aParams.maskRect);
            mProgram->setUniformValue("uMaskTexelSize", aParams.maskTexelSize);

            mVAO->bind();
            ggl.glBindBuffer(GL_ARRAY_BUFFER, aPositions.id());
            ggl.glEnableVertexAttribArray(mPositionLocation);
            mProgram->setAttributeArray(mPositionLocation, GL_FLOAT, nullptr, 3, 0);
            if (aParams.uvFromScreen) {
                // the mask's uv is derived from the vertex's screen position; no per-vertex
                // texcoord exists to bind, and leaving a stale one enabled would feed the
                // attribute a buffer this draw did not write
                ggl.glDisableVertexAttribArray(mTexCoordLocation);
            } else {
                ggl.glBindBuffer(GL_ARRAY_BUFFER, mTexCoordVbo);
                ggl.glBufferData(
                    GL_ARRAY_BUFFER, (GLsizeiptr)aVtxCount * 2 * sizeof(GLfloat), aTexCoords,
                    GL_DYNAMIC_DRAW
                );
                ggl.glEnableVertexAttribArray(mTexCoordLocation);
                mProgram->setAttributeArray(mTexCoordLocation, GL_FLOAT, nullptr, 2, 0);
            }
            aIndexBuffer.bind();
            ggl.glDrawElements(GL_TRIANGLES, aIndexCount, GL_UNSIGNED_INT, nullptr);
            aIndexBuffer.release();
            mProgram->release();

            ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFbo);
            ggl.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
            ggl.glUseProgram((GLuint)prevProgram);
            ggl.glBindVertexArray((GLuint)prevVao);
            ggl.glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
            const auto restore = [&ggl](GLenum aState, GLboolean aWasEnabled) {
                if (aWasEnabled)
                    ggl.glEnable(aState);
                else
                    ggl.glDisable(aState);
            };
            restore(GL_BLEND, prevBlend);
            restore(GL_DEPTH_TEST, prevDepth);
            restore(GL_SCISSOR_TEST, prevScissor);
            restore(GL_CULL_FACE, prevCull);
            mDrawFrame = mFrame;
        }

        /// One copy of every layer into a PBO, one fence. Skipped when no layer drew this frame.
        void issue() {
            if (!ensureBuilt() || mDrawFrame != mFrame || mCapacity <= 0 || mIssuedFrame == mFrame)
                return;
            Slot& pending = mSlots[mWriteSlot];
            if (pending.pending)
                return;   // the previous copy has not been mapped yet
            auto& ggl = gl::Global::functions();
            ggl.glBindTexture(GL_TEXTURE_2D_ARRAY, mTexture);
            ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, pending.pbo);
            ggl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
            ggl.glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            ggl.glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            pending.fence = ggl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            pending.pending = true;
            pending.frame = mFrame;
            mIssuedFrame = mFrame;
            mWriteSlot = (mWriteSlot + 1) % 2;
        }

        /// Maps the pending copy (optionally waiting for it). The pointer stays valid until
        /// endConsume(); callers parse their own layer in between.
        bool consume(bool aWait) {
            if (!ensureBuilt() || mMapped || mTexture == 0)
                return false;
            for (int i = 0; i < 2; ++i) {
                Slot& slot = mSlots[i];
                if (!slot.pending)
                    continue;
                auto& ggl = gl::Global::functions();
                GLenum status = GL_ALREADY_SIGNALED;
                if (slot.fence) {
                    if (aWait) {
                        for (int attempt = 0; attempt < 1000; ++attempt) {
                            status = ggl.glClientWaitSync(slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
                            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED
                                || status == GL_WAIT_FAILED)
                                break;
                        }
                    } else {
                        status = ggl.glClientWaitSync(slot.fence, 0, 0);
                    }
                }
                if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
                    return false;
                ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
                uint8* map = (uint8*)ggl.glMapBufferRange(
                    GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)mCell.width() * mCell.height() * mCapacity,
                    GL_MAP_READ_BIT
                );
                ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                if (!map)
                    return false;
                mMapped = map;
                mMappedSlot = i;
                mMappedFrame = slot.frame;
                return true;
            }
            return false;
        }

        void endConsume() {
            if (!mMapped || mMappedSlot < 0)
                return;
            auto& ggl = gl::Global::functions();
            Slot& slot = mSlots[mMappedSlot];
            ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            ggl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (slot.fence) {
                ggl.glDeleteSync(slot.fence);
                slot.fence = nullptr;
            }
            slot.pending = false;
            mMapped = nullptr;
            mMappedSlot = -1;
        }

        private:
        DemandMap() {}

        bool ensureBuilt() {
            QOpenGLContext* context = QOpenGLContext::currentContext();
            if (!context)
                return false;
            if (context != mContext) {
                // a recreated context invalidates the program, the VAO and the array; the layers
                // and their grids survive on the CPU, the texture is rebuilt empty on demand
                mProgram.reset();
                mVAO.reset();
                mTexCoordVbo = 0;
                mTexture = 0;
                mFbo = 0;
                mClearedFrame = 0;
                mContext = context;
            }
            if (!mProgram) {
                std::unique_ptr<QOpenGLShaderProgram> program(new QOpenGLShaderProgram());
                bool ok = program->addShaderFromSourceCode(QOpenGLShader::Vertex, kDemandVertexShader);
                ok = ok && program->addShaderFromSourceCode(QOpenGLShader::Geometry, kDemandGeometryShader);
                ok = ok && program->addShaderFromSourceCode(QOpenGLShader::Fragment, kDemandFragmentShader);
                ok = ok && program->link();
                if (!ok) {
                    // demand failing must not take the renderer down: without demand the paged
                    // path degrades to the always-resident coarse chain, which is blur, not a hole
                    qWarning("VirtualTexture demand pass disabled: %s", qPrintable(program->log()));
                    return false;
                }
                mPositionLocation = program->attributeLocation("inPosition");
                mTexCoordLocation = program->attributeLocation("inTexCoord");
                if (mPositionLocation < 0 || mTexCoordLocation < 0) {
                    qWarning("VirtualTexture demand pass: attribute missing");
                    return false;
                }
                mProgram.reset(program.release());
                auto& ggl = gl::Global::functions();
                ggl.glGenFramebuffers(1, &mFbo);
                ggl.glGenBuffers(1, &mTexCoordVbo);
                mVAO.reset(new gl::VertexArrayObject());
            }
            return mProgram != nullptr;
        }

        bool ensureCapacity(int aLayers, const QSize& aGrid) {
            if (!ensureBuilt())
                return false;
            const QSize cell = mCell.expandedTo(aGrid);
            int capacity = mCapacity;
            while (capacity < aLayers)
                capacity = capacity ? capacity * 2 : 8;
            capacity = std::min(capacity, (int)kMaxLayers);
            if (mTexture != 0 && cell == mCell && capacity == mCapacity)
                return true;

            auto& ggl = gl::Global::functions();
            if (mMapped) {
                // a reallocation while a copy is mapped: close it first
                endConsume();
            }
            for (Slot& slot : mSlots) {
                if (slot.fence) {
                    ggl.glDeleteSync(slot.fence);
                    slot.fence = nullptr;
                }
                if (slot.pbo) {
                    ggl.glDeleteBuffers(1, &slot.pbo);
                    slot.pbo = 0;
                }
                slot.pending = false;
            }
            if (mTexture != 0)
                ggl.glDeleteTextures(1, &mTexture);
            mTexture = 0;
            mCell = cell;
            mCapacity = capacity;
            if (mCell.isEmpty() || mCapacity <= 0)
                return false;

            ggl.glGenTextures(1, &mTexture);
            ggl.glBindTexture(GL_TEXTURE_2D_ARRAY, mTexture);
            ggl.glTexImage3D(
                GL_TEXTURE_2D_ARRAY, 0, GL_R8, mCell.width(), mCell.height(), mCapacity, 0, GL_RED,
                GL_UNSIGNED_BYTE, nullptr
            );
            ggl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            ggl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ggl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            ggl.glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            ggl.glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

            ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFbo);
            ggl.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mTexture, 0);
            ggl.glDrawBuffer(GL_COLOR_ATTACHMENT0);
            const bool complete = ggl.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            if (!complete) {
                qWarning("VirtualTexture: layered demand framebuffer is incomplete");
                ggl.glDeleteTextures(1, &mTexture);
                mTexture = 0;
                return false;
            }

            const GLsizeiptr bytes = (GLsizeiptr)mCell.width() * mCell.height() * mCapacity;
            for (Slot& slot : mSlots) {
                ggl.glGenBuffers(1, &slot.pbo);
                ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
                ggl.glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
                ggl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            mClearedFrame = 0;
            return true;
        }

        void bindFramebuffer() {
            auto& ggl = gl::Global::functions();
            ggl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFbo);
            ggl.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mTexture, 0);
            ggl.glDrawBuffer(GL_COLOR_ATTACHMENT0);
        }

        QOpenGLContext* mContext = nullptr;
        std::unique_ptr<QOpenGLShaderProgram> mProgram;
        std::unique_ptr<gl::VertexArrayObject> mVAO;
        GLuint mTexCoordVbo = 0;
        GLuint mTexture = 0;
        GLuint mFbo = 0;
        int mPositionLocation = -1;
        int mTexCoordLocation = -1;

        QSize mCell;
        int mCapacity = 0;
        std::vector<QSize> mGrids;
        std::vector<int> mFree;
        quint64 mFrame = 0;
        quint64 mClearedFrame = 0;
        quint64 mDrawFrame = 0;
        quint64 mIssuedFrame = 0;
        struct Slot {
            GLuint pbo = 0;
            GLsync fence = nullptr;
            bool pending = false;
            quint64 frame = 0;
        };
        Slot mSlots[2];
        int mWriteSlot = 0;
        const uint8* mMapped = nullptr;
        int mMappedSlot = -1;
        quint64 mMappedFrame = 0;
    };

    //------------------------------------------------------------------------------

    /// A DIAGNOSTIC, not a policy: with AE_VT_DEMAND=0 the demand pass and its readbacks are
/// skipped, so every paged layer samples the coarse chain through the VT shader. That is the
/// control for "is the cost the shader or the demand machinery" - both were measured with it.
bool demandEnabled() {
    static const bool on = qgetenv("AE_VT_DEMAND") != "0";
    return on;
}

std::vector<VirtualTexture*>& registry() {
        static std::vector<VirtualTexture*> resources;
        return resources;
    }

    void registerResource(VirtualTexture* aResource) {
        auto& all = registry();
        if (std::find(all.begin(), all.end(), aResource) == all.end())
            all.push_back(aResource);
    }

    void unregisterResource(VirtualTexture* aResource) {
        auto& all = registry();
        all.erase(std::remove(all.begin(), all.end(), aResource), all.end());
    }

} // namespace

bool VirtualTexture::pageable(const QSize& aSize) {
    if (!gl::PagePool::enabled() || aSize.isEmpty())
        return false;
    return std::max(aSize.width(), aSize.height()) > gl::PagePool::coarseSize();
}

int VirtualTexture::instanceCount() { return (int)registry().size(); }

VirtualTexture::VirtualTexture(): mResource(), mSize(), mPyramid(), mCoarse(), mTable() {}
VirtualTexture::~VirtualTexture() { destroy(); }

void VirtualTexture::beginFrame() {
    if (!gl::PagePool::enabled())
        return;
    // First the previous frame's demand copy (it updates what each page last wanted), then the
    // clear that starts this frame's map, then the uploads: a page about to be sampled goes in
    // before the frame's draws.
    gl::PagePool::instance().beginFrame();
    DemandMap& demandMap = DemandMap::instance();
    if (demandMap.consume(false)) {
        for (VirtualTexture* resource : registry())
            resource->consumeDemand();
        demandMap.endConsume();
    }
    demandMap.beginFrame(gl::PagePool::instance().frame());
    for (VirtualTexture* resource : registry())
        resource->resolveDemand();
    // TABLES GO OUT AFTER EVERY UPLOAD, not at the end of each resource: an upload for resource
    // B can evict a page of resource A, and A's table was already rebuilt by then. Publishing in
    // a second pass means no table in the frame names a slot some other resource has taken.
    for (VirtualTexture* resource : registry())
        resource->flushTable();
}

void VirtualTexture::endFrame() {
    if (!gl::PagePool::enabled())
        return;
    DemandMap::instance().issue();
    gl::PagePool::instance().endFrame();
    if (qEnvironmentVariableIsSet("AE_VT_STATS")) {
        int wanted = 0, resident = 0;
        for (VirtualTexture* resource : registry()) {
            wanted += resource->wantedPageCount();
            resident += resource->residentWantedCount();
        }
        qWarning(
            "VT frame %llu resources=%d wanted=%d resident=%d poolPages=%d poolBytes=%lld",
            (unsigned long long)gl::PagePool::instance().frame(),
            (int)registry().size(),
            wanted,
            resident,
            gl::PagePool::instance().residentPages(),
            (long long)gl::PagePool::instance().residentBytes()
        );
    }
}

void VirtualTexture::synchronize() {
    if (!gl::PagePool::enabled())
        return;
    DemandMap& demandMap = DemandMap::instance();
    demandMap.issue();
    if (demandMap.consume(true)) {
        for (VirtualTexture* resource : registry())
            resource->consumeDemand();
        demandMap.endConsume();
    }
    for (VirtualTexture* resource : registry())
        resource->resolveDemand();
    for (VirtualTexture* resource : registry())
        resource->flushTable();
}

int VirtualTexture::pageGridX(int aLevel) const {
    if (aLevel < 0 || aLevel >= mCoarseLevel)
        return 0;
    return mLevelGridX[aLevel];
}

int VirtualTexture::pageGridY(int aLevel) const {
    if (aLevel < 0 || aLevel >= mCoarseLevel)
        return 0;
    return mLevelGridY[aLevel];
}

int VirtualTexture::pageCount() const { return mTableWidth * mTableRows; }

int VirtualTexture::pageIndex(int aLevel, int aPageX, int aPageY) const {
    XC_ASSERT(0 <= aLevel && aLevel < mCoarseLevel);
    return (mLevelRow[aLevel] + aPageY) * mTableWidth + aPageX;
}

gl::PagePool::PageId VirtualTexture::pageId(int aLevel, int aPageX, int aPageY) const {
    gl::PagePool::PageId id;
    id.owner = (quint64)(uintptr_t)this;
    id.level = aLevel;
    id.x = aPageX;
    id.y = aPageY;
    return id;
}

int VirtualTexture::levelOfIndex(int aIndex, int* aPageX, int* aPageY) const {
    const int row = aIndex / mTableWidth;
    for (int l = 0; l < mCoarseLevel; ++l) {
        if (row >= mLevelRow[l] && row < mLevelRow[l] + mLevelGridY[l]) {
            if (aPageX)
                *aPageX = aIndex % mTableWidth;
            if (aPageY)
                *aPageY = row - mLevelRow[l];
            return l;
        }
    }
    if (aPageX)
        *aPageX = 0;
    if (aPageY)
        *aPageY = 0;
    return 0;
}

gl::PagePool::PageId VirtualTexture::pageIdForIndex(int aIndex) const {
    int px = 0, py = 0;
    const int level = levelOfIndex(aIndex, &px, &py);
    return pageId(level, px, py);
}

bool VirtualTexture::build(const img::ResourceHandle& aResource, const QSize& aSize, gl::Texture& aFullChain) {
    destroy();
    if (!pageable(aSize) || aFullChain.id() == 0 || aFullChain.size() != aSize)
        return false;
    if (!gl::PagePool::enabled())
        return false;

    mResource = aResource;
    mSize = aSize;
    mMaxLevel = aFullChain.mipmapLevels() - 1;
    if (mMaxLevel < 1) {
        mSize = QSize();
        return false;
    }

    // the coarse level: the first level whose longest side fits the coarse threshold. Every
    // level from it down to 1x1 is resident as a plain mipmapped texture; the sharper levels
    // above it are paged.
    mCoarseLevel = 0;
    QSize coarseSize = aSize;
    while (std::max(coarseSize.width(), coarseSize.height()) > gl::PagePool::coarseSize() && mCoarseLevel < mMaxLevel) {
        coarseSize = QSize(std::max(1, coarseSize.width() >> 1), std::max(1, coarseSize.height() >> 1));
        ++mCoarseLevel;
    }
    if (mCoarseLevel < 1 || mCoarseLevel >= kMaxPageLevels) {
        mSize = QSize();
        return false;
    }

    // Capture EVERY level, the coarse tail included: the tail is small, and building the coarse
    // texture from the same bytes the pages read is what makes the fallback the same filter on
    // the same data. The tail is dropped from the pyramid as soon as the coarse texture has it.
    if (!mPyramid.capture(aFullChain, mMaxLevel)) {
        mSize = QSize();
        return false;
    }
    if (!buildCoarse(aFullChain)) {
        mPyramid.clear();
        mSize = QSize();
        return false;
    }
    mPyramid.truncate(mCoarseLevel - 1);
    if (!createTargets()) {
        destroy();
        return false;
    }

    registerResource(this);
    if (qEnvironmentVariableIsSet("AE_VT_HASH")) {
        const int level = qEnvironmentVariableIntValue("AE_VT_HASH");
        qWarning("VTHASH %dx%d level=%d pyramid=%016llx", mSize.width(), mSize.height(), level,
                 (unsigned long long)mPyramid.hashLevel(level));
    }
    return true;
}

bool VirtualTexture::buildMask(MaskPyramid& aPyramid) {
    destroy();
    if (!aPyramid.isValid() || !pageable(aPyramid.size()) || !gl::PagePool::enabled())
        return false;

    mFormat = gl::PagePool::Format_R8;
    mMaskPyramid = &aPyramid;
    mSize = aPyramid.size();
    mMaxLevel = aPyramid.levelCount() - 1;
    if (mMaxLevel < 1) {
        mMaskPyramid = nullptr;
        mSize = QSize();
        return false;
    }

    mCoarseLevel = 0;
    QSize coarseSize = mSize;
    while (std::max(coarseSize.width(), coarseSize.height()) > gl::PagePool::coarseSize()
           && mCoarseLevel < mMaxLevel) {
        coarseSize = QSize(std::max(1, coarseSize.width() >> 1), std::max(1, coarseSize.height() >> 1));
        ++mCoarseLevel;
    }
    if (mCoarseLevel < 1 || mCoarseLevel >= kMaxPageLevels) {
        qWarning("VT mask: coarse level %d out of range", mCoarseLevel);
        mMaskPyramid = nullptr;
        mSize = QSize();
        mFormat = gl::PagePool::Format_RGBA8;
        return false;
    }
    if (!buildMaskCoarse()) {
        qWarning("VT mask: coarse build failed (coarse %d of %d)", mCoarseLevel, mMaxLevel);
        mMaskPyramid = nullptr;
        mSize = QSize();
        mFormat = gl::PagePool::Format_RGBA8;
        return false;
    }
    if (!createTargets()) {
        qWarning("VT mask: targets failed (table %dx%d)", mTableWidth, mTableRows);
        destroy();
        return false;
    }
    registerResource(this);
    return true;
}

bool VirtualTexture::buildCoarse(gl::Texture& aFullChain) {
    (void)aFullChain;
    const QSize coarseSize = mPyramid.levelSize(mCoarseLevel);
    XC_ASSERT(!coarseSize.isEmpty());

    // The first coarse level goes in through create()'s Manual allocation; the rest are
    // uploaded level by level. Manual is the policy whose point is "the caller owns every
    // level" - and every one of them is a level of the chain that was just captured, not a
    // rebuild, so the fallback and the control path cannot disagree about a texel.
    std::vector<uint8> levelBytes((size_t)coarseSize.width() * coarseSize.height() * 4);
    mPyramid.copyLevel(mCoarseLevel, levelBytes.data(), coarseSize);

    mCoarse.create(coarseSize, levelBytes.data(), GL_RGBA, GL_RGBA8, GL_UNSIGNED_BYTE, gl::Texture::Manual);
    if (mCoarse.id() == 0)
        return false;

    auto& ggl = gl::Global::functions();
    for (int rel = 1; rel <= mMaxLevel - mCoarseLevel; ++rel) {
        const QSize levelSize = mPyramid.levelSize(mCoarseLevel + rel);
        levelBytes.resize((size_t)levelSize.width() * levelSize.height() * 4);
        mPyramid.copyLevel(mCoarseLevel + rel, levelBytes.data(), levelSize);
        ggl.glBindTexture(GL_TEXTURE_2D, mCoarse.id());
        ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        ggl.glTexSubImage2D(
            GL_TEXTURE_2D,
            rel,
            0,
            0,
            levelSize.width(),
            levelSize.height(),
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            levelBytes.data()
        );
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
    }

    // The full texture's own sampling policy: trilinear minification, LINEAR magnification,
    // transparent outside the content. The shader samples a specific level with textureLod, so
    // the level blend never runs - what matters is the bilinear read inside a level, which is
    // the same read a page gets, and the border colour at the content's edge.
    mCoarse.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    mCoarse.setWrap(GL_CLAMP_TO_BORDER, QColor(0, 0, 0, 0));
    mCoarse.setLodBias(0.0f);

    return true;
}

bool VirtualTexture::buildMaskCoarse() {
    XC_PTR_ASSERT(mMaskPyramid);
    const QSize coarseSize = mMaskPyramid->levelSize(mCoarseLevel);
    if (coarseSize.isEmpty())
        return false;

    // THE SAME BYTES THE PAGES READ: the coarse tier is copied out of the pyramid, not rebuilt,
    // so a page miss and a page hit are the same field - the same rule the image's coarse chain
    // follows (buildCoarse).
    std::vector<uint8> levelBytes((size_t)coarseSize.width() * coarseSize.height());
    mMaskPyramid->copyLevel(mCoarseLevel, levelBytes.data());
    auto& ggl = gl::Global::functions();
    // BEFORE the level-0 upload, not after it: an R8 row of 450 texels is not a multiple of the
    // default 4-byte alignment, and glTexImage2D would skew every row by the padding (measured:
    // the coarse held the field shifted by two texels per row)
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    mCoarse.create(
        coarseSize, levelBytes.data(), GL_RED, GL_R8, GL_UNSIGNED_BYTE, gl::Texture::Manual
    );
    if (mCoarse.id() == 0) {
        ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return false;
    }

    for (int rel = 1; rel <= mMaxLevel - mCoarseLevel; ++rel) {
        const QSize levelSize = mMaskPyramid->levelSize(mCoarseLevel + rel);
        levelBytes.resize((size_t)levelSize.width() * levelSize.height());
        mMaskPyramid->copyLevel(mCoarseLevel + rel, levelBytes.data());
        ggl.glBindTexture(GL_TEXTURE_2D, mCoarse.id());
        ggl.glTexSubImage2D(
            GL_TEXTURE_2D, rel, 0, 0, levelSize.width(), levelSize.height(), GL_RED,
            GL_UNSIGNED_BYTE, levelBytes.data()
        );
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
    }
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // The mask's chain is sampled GL_CLAMP_TO_EDGE (MaskMipChain::create), and the page gutter
    // reproduces that at the level's border (MaskPyramid::fillPage): a zero border here would
    // make the fallback differ from the page path at every field edge.
    mCoarse.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    mCoarse.setWrap(GL_CLAMP_TO_EDGE);
    mCoarse.setLodBias(0.0f);
    return true;
}

bool VirtualTexture::createTargets() {
    mTableWidth = 0;
    mTableRows = 0;
    for (int l = 0; l < mCoarseLevel; ++l) {
        // the level's own grid, from whichever source holds the levels: the image's capture or
        // the mask's CPU pyramid
        const QSize size = isMask() ? mMaskPyramid->levelSize(l) : mPyramid.levelSize(l);
        mLevelGridX[l] = (size.width() + gl::PagePool::kPageSize - 1) / gl::PagePool::kPageSize;
        mLevelGridY[l] = (size.height() + gl::PagePool::kPageSize - 1) / gl::PagePool::kPageSize;
        mLevelRow[l] = mTableRows;
        mTableRows += mLevelGridY[l];
        mTableWidth = std::max(mTableWidth, mLevelGridX[l]);
    }
    if (mTableWidth <= 0 || mTableRows <= 0)
        return false;

    const int count = mTableWidth * mTableRows;
    mEntries.assign((size_t)count, 0);
    mDemandFrame.assign((size_t)count, 0);
    mActiveFlags.assign((size_t)count, 0);
    mActivePages.clear();
    mActivePages.reserve(512);
    mPageBuffer.resize((size_t)gl::PagePool::kPitch * gl::PagePool::kPitch * bytesPerTexel());

    const QSize mapSize(mTableWidth, mTableRows);
    std::vector<quint16> tableZeros((size_t)count, 0);
    mTable.create(
        mapSize,
        reinterpret_cast<const uint8*>(tableZeros.data()),
        GL_RED_INTEGER,
        GL_R16UI,
        GL_UNSIGNED_SHORT,
        gl::Texture::NoMipmap
    );
    // integer textures only accept NEAREST
    mTable.setFilter(GL_NEAREST);
    mTable.setWrap(GL_CLAMP_TO_EDGE);

    mTableDirty = false;
    mPoolGeneration = gl::PagePool::instance().generation();

    // the shared demand map's layer for this resource's own page grid
    mDemandLayer = DemandMap::instance().allocateLayer(mapSize);
    mDemandParsedFrame = 0;
    return mTable.id() != 0;
}

void VirtualTexture::destroy() {
    const bool wasValid = mSize.isValid();
    if (wasValid) {
        unregisterResource(this);
        if (gl::PagePool::enabled())
            gl::PagePool::instance().releaseOwner((quint64)(uintptr_t)this);
    }

    if (mDemandLayer >= 0)
        DemandMap::instance().releaseLayer(mDemandLayer);
    mDemandLayer = -1;
    if (QOpenGLContext::currentContext()) {
        mTable.destroy();
        mCoarse.destroy();
    }
    mPyramid.clear();
    mMaskPyramid = nullptr;
    mMaskField = QImage();
    mResource.reset();
    mSize = QSize();
    mMaxLevel = 0;
    mCoarseLevel = 0;
    mFormat = gl::PagePool::Format_RGBA8;
    mTableWidth = 0;
    mTableRows = 0;
    mEntries.clear();
    mDemandFrame.clear();
    mActiveFlags.clear();
    mActivePages.clear();
    mPageBuffer.clear();
    mTableDirty = false;
    mDemandParsedFrame = 0;
}

void VirtualTexture::pageEvicted(const gl::PagePool::PageId& aPage) {
    if (aPage.level < 0 || aPage.level >= mCoarseLevel)
        return;
    const int index = pageIndex(aPage.level, aPage.x, aPage.y);
    if (index >= 0 && index < (int)mEntries.size() && mEntries[(size_t)index] != 0) {
        mEntries[(size_t)index] = 0;
        mTableDirty = true;
    }
}

void VirtualTexture::consumeDemand() {
    DemandMap& demandMap = DemandMap::instance();
    if (mDemandLayer < 0 || !demandMap.isMapped()
        || demandMap.mappedFrame() == mDemandParsedFrame)
        return;
    const QSize grid = demandMap.grid(mDemandLayer);
    const uint8* base = demandMap.layerData(mDemandLayer);
    if (grid.isEmpty() || !base)
        return;
    mDemandParsedFrame = demandMap.mappedFrame();
    const quint64 frame = gl::PagePool::instance().frame();
    const int stride = demandMap.stride();
    for (int l = 0; l < mCoarseLevel; ++l) {
        const int row0 = mLevelRow[l];
        for (int py = 0; py < mLevelGridY[l]; ++py) {
            const int row = row0 + py;
            const uint8* line = base + (size_t)row * stride;
            for (int px = 0; px < mLevelGridX[l]; ++px) {
                if (line[px] == 0)
                    continue;
                const int index = row * mTableWidth + px;
                if (mDemandFrame[(size_t)index] == (int)frame)
                    continue;
                mDemandFrame[(size_t)index] = (int)frame;
                if (!mActiveFlags[(size_t)index]) {
                    mActiveFlags[(size_t)index] = 1;
                    mActivePages.push_back((quint32)index);
                }
            }
        }
    }
}

void VirtualTexture::resolveDemand() {
    auto& pool = gl::PagePool::instance();
    const quint64 frame = pool.frame();

    // A purged pool (or a recreated one) has thrown every slot away: any entry still naming one
    // is a dangling slot number and must be cleared before this frame samples it.
    if (mPoolGeneration != pool.generation()) {
        mPoolGeneration = pool.generation();
        std::fill(mEntries.begin(), mEntries.end(), 0);
        mTableDirty = true;
    }

    // Touch every page still wanted BEFORE any upload can evict one: the pool's LRU must have
    // this frame's pages at the front, so the victim is a page nobody is sampling now.
    std::vector<quint32> kept;
    kept.reserve(mActivePages.size());
    for (quint32 value : mActivePages) {
        const int index = (int)value;
        const int when = mDemandFrame[(size_t)index];
        if (when <= 0 || frame > (quint64)when + kKeepFrames) {
            // stale: still resident, but now an eviction candidate
            mDemandFrame[(size_t)index] = 0;
            mActiveFlags[(size_t)index] = 0;
            continue;
        }
        kept.push_back(value);
        pool.touch(pageIdForIndex(index));
    }
    mActivePages.swap(kept);

    // Upload what is missing, sharpest level first: a zoom-in asks for level 0 and has less
    // time to converge than the fit view's level 3.
    std::stable_sort(mActivePages.begin(), mActivePages.end());
    for (quint32 value : mActivePages) {
        const int index = (int)value;
        int px = 0, py = 0;
        const int level = levelOfIndex(index, &px, &py);
        const gl::PagePool::PageId id = pageId(level, px, py);
        gl::PagePool::Slot slot;
        if (pool.find(id, &slot))
            continue;
        if (!pool.acquire(id, this, &slot, mFormat))
            break; // budget or upload cap: the rest waits, the coarse chain covers it
        if (!uploadPage(level, px, py, slot)) {
            pool.cancel(slot);
            break;
        }
    }
}

void VirtualTexture::flushTable() {
    if (mTableDirty)
        rebuildTable();
}

bool VirtualTexture::uploadPage(int aLevel, int aPageX, int aPageY, const gl::PagePool::Slot& aSlot) {
    fillPageBuffer(aLevel, aPageX, aPageY);
    if (!isMask() && aLevel > 0 && qEnvironmentVariableIsSet("AE_VT_PAGE_HASH")) {
        const QSize levelSize = mPyramid.levelSize(aLevel);
        const int w = std::min((int)gl::PagePool::kPageSize, levelSize.width() - aPageX * (int)gl::PagePool::kPageSize);
        const int h = std::min((int)gl::PagePool::kPageSize, levelSize.height() - aPageY * (int)gl::PagePool::kPageSize);
        quint64 bufferHash = 1469598103934665603ull;
        for (int y = 0; y < h; ++y) {
            const uint8* row = mPageBuffer.data()
                + ((size_t)(y + gl::PagePool::kGutter) * gl::PagePool::kPitch + gl::PagePool::kGutter) * 4;
            for (int i = 0; i < w * 4; ++i) {
                bufferHash ^= row[i];
                bufferHash *= 1099511628211ull;
            }
        }
        const quint64 levelHash =
            mPyramid.hashRegion(aLevel, aPageX * (int)gl::PagePool::kPageSize, aPageY * (int)gl::PagePool::kPageSize, w, h);
        if (bufferHash != levelHash) {
            qWarning("VTPAGE level=%d page=(%d,%d) %dx%d buffer=%016llx level=%016llx", aLevel, aPageX,
                     aPageY, w, h, (unsigned long long)bufferHash, (unsigned long long)levelHash);
        }
    }
    auto& ggl = gl::Global::functions();
    const GLuint atlas = gl::PagePool::instance().atlasTextureId(aSlot.atlas);
    if (atlas == 0)
        return false;
    ggl.glBindTexture(GL_TEXTURE_2D, atlas);
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ggl.glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        aSlot.x - gl::PagePool::kGutter,
        aSlot.y - gl::PagePool::kGutter,
        gl::PagePool::kPitch,
        gl::PagePool::kPitch,
        isMask() ? GL_RED : GL_RGBA,
        GL_UNSIGNED_BYTE,
        mPageBuffer.data()
    );
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    const int index = pageIndex(aLevel, aPageX, aPageY);
    // the shader's atlas branch is PER STACK: the entry names 0..3 inside the resource's own
    // format, not the pool's global atlas index (R8 lives at 4..7 - see PagePool::Format)
    const int shaderAtlas = aSlot.atlas - (int)mFormat * gl::PagePool::kMaxAtlasCount;
    mEntries[(size_t)index] = (quint16)((shaderAtlas << 8) | (aSlot.slot + 1));
    mTableDirty = true;
    return true;
}

qint64 VirtualTexture::pyramidBytes() const {
    return isMask() && mMaskPyramid ? mMaskPyramid->bytes() : mPyramid.bytes();
}

void VirtualTexture::fillPageBuffer(int aLevel, int aPageX, int aPageY) {
    if (isMask()) {
        XC_PTR_ASSERT(mMaskPyramid);
        mMaskPyramid->fillPage(aLevel, aPageX, aPageY, mMaskField, mPageBuffer.data());
        return;
    }
    const uint8* base = nullptr;
    QSize baseSize;
    if (aLevel == 0 && mResource && mResource->hasImage()) {
        base = mResource->image().data();
        baseSize = mResource->image().pixelSize();
    }
    mPyramid.fillPage(aLevel, aPageX, aPageY, base, baseSize, mPageBuffer.data());
}

void VirtualTexture::updateMaskBox(const QImage& aField, const QRect& aTexelBox) {
    if (!isMask() || !mMaskPyramid || aField.isNull() || aField.size() != mSize)
        return;
    const QRect box = aTexelBox.intersected(QRect(QPoint(0, 0), mSize));
    if (box.isEmpty())
        return;

    // THE PYRAMID IS THE AUTHORITY FOR LEVELS 1+; the field is level 0 and is kept here for the
    // page fills. What is RESIDENT is brought in step: a level the box misses does not change,
    // and a page that is not resident needs nothing - its next fill reads the current field and
    // pyramid.
    mMaskField = aField;
    mMaskPyramid->updateBox(aField, box);

    auto& ggl = gl::Global::functions();
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // the always-resident coarse chain: one box per level, halved and rounded outwards exactly
    // like the pyramid's own rule
    if (mCoarse.id() != 0) {
        ggl.glBindTexture(GL_TEXTURE_2D, mCoarse.id());
        for (int rel = 0; rel <= mMaxLevel - mCoarseLevel; ++rel) {
            const int level = mCoarseLevel + rel;
            const QSize levelSize = mMaskPyramid->levelSize(level);
            const QRect levelBox = mMaskPyramid->boxAtLevel(level, box);
            if (levelBox.isEmpty())
                break;
            const std::vector<uint8>& bytes = mMaskPyramid->levelBytes(level);
            const uint8* base =
                bytes.data() + (size_t)levelBox.top() * levelSize.width() + levelBox.left();
            ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, levelSize.width());
            ggl.glTexSubImage2D(
                GL_TEXTURE_2D, rel, levelBox.left(), levelBox.top(), levelBox.width(),
                levelBox.height(), GL_RED, GL_UNSIGNED_BYTE, base
            );
        }
        ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
    }

    // the pages: only those already resident, and only the ones the box's level-box touches
    for (int level = 0; level < mCoarseLevel; ++level) {
        const QRect levelBox = mMaskPyramid->boxAtLevel(level, box);
        if (levelBox.isEmpty())
            break;
        const int px0 = levelBox.left() / gl::PagePool::kPageSize;
        const int px1 = levelBox.right() / gl::PagePool::kPageSize;
        const int py0 = levelBox.top() / gl::PagePool::kPageSize;
        const int py1 = levelBox.bottom() / gl::PagePool::kPageSize;
        for (int py = py0; py <= py1; ++py) {
            for (int px = px0; px <= px1; ++px) {
                gl::PagePool::Slot slot;
                if (!gl::PagePool::instance().find(pageId(level, px, py), &slot))
                    continue;
                const GLuint atlas = gl::PagePool::instance().atlasTextureId(slot.atlas);
                if (atlas == 0)
                    continue;
                fillPageBuffer(level, px, py);
                ggl.glBindTexture(GL_TEXTURE_2D, atlas);
                ggl.glTexSubImage2D(
                    GL_TEXTURE_2D, 0, slot.x - gl::PagePool::kGutter,
                    slot.y - gl::PagePool::kGutter, gl::PagePool::kPitch, gl::PagePool::kPitch,
                    GL_RED, GL_UNSIGNED_BYTE, mPageBuffer.data()
                );
                ggl.glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
    }
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void VirtualTexture::rebuildTable() {
    if (mTable.id() == 0)
        return;
    auto& ggl = gl::Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mTable.id());
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ggl.glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, mTableWidth, mTableRows, GL_RED_INTEGER, GL_UNSIGNED_SHORT, mEntries.data()
    );
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
    mTableDirty = false;
}

void VirtualTexture::markDemand(
    const QMatrix4x4& aViewMatrix,
    const QSize& aDeviceScreenSize,
    const QVector2D& aTexCoordOffset,
    const gl::BufferObject& aPositions,
    const gl::Vector2* aTexCoords,
    int aVtxCount,
    const gl::BufferObject& aIndexBuffer,
    int aIndexCount
) {
    if (!isValid() || aVtxCount <= 0 || aIndexCount < 3 || !demandEnabled())
        return;
    if (mDemandLayer < 0)
        return;

    DemandMap::Params params;
    params.layerSize = mSize;
    params.coarseLevel = mCoarseLevel;
    params.maxLevel = mMaxLevel;
    params.bias = ImageKey::layerLodBias();
    for (int i = 0; i < kMaxPageLevels; ++i)
        params.levelRow[i] = mLevelRow[i];

    DemandMap::instance().mark(
        mDemandLayer, QSize(mTableWidth, mTableRows), params, aViewMatrix, aDeviceScreenSize,
        aTexCoordOffset, const_cast<gl::BufferObject&>(aPositions), aTexCoords, aVtxCount,
        const_cast<gl::BufferObject&>(aIndexBuffer), aIndexCount
    );
}

void VirtualTexture::markMaskDemand(
    const QMatrix4x4& aViewMatrix,
    const QMatrix4x4& aScreenToContent,
    const QRectF& aMaskRect,
    const QSize& aDeviceScreenSize,
    const gl::BufferObject& aPositions,
    int aVtxCount,
    const gl::BufferObject& aIndexBuffer,
    int aIndexCount
) {
    if (!isMask() || mDemandLayer < 0 || aIndexCount < 3 || aVtxCount <= 0
        || !demandEnabled() || aMaskRect.isEmpty())
        return;

    DemandMap::Params params;
    params.layerSize = mSize;
    params.coarseLevel = mCoarseLevel;
    params.maxLevel = mMaxLevel;
    params.bias = ImageKey::layerLodBias();
    for (int i = 0; i < kMaxPageLevels; ++i)
        params.levelRow[i] = mLevelRow[i];
    params.uvFromScreen = true;
    params.screenToContent = aScreenToContent;
    params.maskRect = QVector4D(
        float(aMaskRect.x()), float(aMaskRect.y()), float(aMaskRect.width()),
        float(aMaskRect.height())
    );
    params.maskTexelSize = QVector2D((float)mSize.width(), (float)mSize.height());

    DemandMap::instance().mark(
        mDemandLayer, QSize(mTableWidth, mTableRows), params, aViewMatrix, aDeviceScreenSize,
        QVector2D(0.0f, 0.0f), const_cast<gl::BufferObject&>(aPositions), nullptr, aVtxCount,
        const_cast<gl::BufferObject&>(aIndexBuffer), aIndexCount
    );
}

void VirtualTexture::bind(gl::EasyShaderProgram& aShader) const {
    auto& ggl = gl::Global::functions();
    // A HOLE in the atlas list is bound to the coarse texture, never left incomplete: a slot
    // that names a released atlas is a bug, but an incomplete sampler is a driver-dependent
    // colour. The count the shader checks is the NUMBER OF ADDRESSABLE ATLAS SLOTS, not the
    // number currently allocated - a released atlas 0 with atlas 1 still full would otherwise
    // make every atlas-1 entry look out of range.
    for (int i = 0; i < gl::PagePool::kMaxAtlasCount; ++i) {
        const GLuint id = gl::PagePool::instance().atlasTextureId(i);
        ggl.glActiveTexture(GL_TEXTURE6 + i);
        ggl.glBindTexture(GL_TEXTURE_2D, id != 0 ? id : mCoarse.id());
    }
    ggl.glActiveTexture(GL_TEXTURE4);
    ggl.glBindTexture(GL_TEXTURE_2D, mCoarse.id());
    ggl.glActiveTexture(GL_TEXTURE5);
    ggl.glBindTexture(GL_TEXTURE_2D, mTable.id());
    ggl.glActiveTexture(GL_TEXTURE0);

    int rows[kMaxPageLevels];
    for (int i = 0; i < kMaxPageLevels; ++i)
        rows[i] = mLevelRow[i];

    aShader.setUniformValue("uLayerSize", QVector2D((float)mSize.width(), (float)mSize.height()));
    aShader.setUniformValue("uLayerMaxLod", (float)mMaxLevel);
    aShader.setUniformValue("uCoarseLevel", mCoarseLevel);
    aShader.setUniformValue("uCoarseTexture", 4);
    aShader.setUniformValue("uPageTable", 5);
    aShader.setUniformValueArray("uPageLevelRow", rows, kMaxPageLevels);
    aShader.setUniformValue("uPageAtlasSize", (float)gl::PagePool::kAtlasSize);
    aShader.setUniformValue("uPageSlotsPerAxis", gl::PagePool::kSlotsPerAxis);
    aShader.setUniformValue("uPagePitch", (float)gl::PagePool::kPitch);
    aShader.setUniformValue("uPageGutter", (float)gl::PagePool::kGutter);
    aShader.setUniformValue("uPageTexels", (float)gl::PagePool::kPageSize);
    aShader.setUniformValue("uAtlasCount", (int)gl::PagePool::kMaxAtlasCount);
    aShader.setUniformValue("uAtlas0", 6);
    aShader.setUniformValue("uAtlas1", 7);
    aShader.setUniformValue("uAtlas2", 8);
    aShader.setUniformValue("uAtlas3", 9);
}

void VirtualTexture::bindMask(gl::EasyShaderProgram& aShader) const {
    auto& ggl = gl::Global::functions();
    // a hole in the atlas list is bound to the coarse chain, never left incomplete (the same
    // rule as bind)
    for (int i = 0; i < gl::PagePool::kMaxAtlasCount; ++i) {
        const GLuint id =
            gl::PagePool::instance().atlasTextureId((int)mFormat * gl::PagePool::kMaxAtlasCount + i);
        ggl.glActiveTexture(GL_TEXTURE12 + i);
        ggl.glBindTexture(GL_TEXTURE_2D, id != 0 ? id : mCoarse.id());
    }
    ggl.glActiveTexture(GL_TEXTURE10);
    ggl.glBindTexture(GL_TEXTURE_2D, mCoarse.id());
    ggl.glActiveTexture(GL_TEXTURE11);
    ggl.glBindTexture(GL_TEXTURE_2D, mTable.id());
    ggl.glActiveTexture(GL_TEXTURE0);

    int rows[kMaxPageLevels];
    for (int i = 0; i < kMaxPageLevels; ++i)
        rows[i] = mLevelRow[i];

    aShader.setUniformValue("uMaskCoarseTexture", 10);
    aShader.setUniformValue("uMaskPageTable", 11);
    aShader.setUniformValueArray("uMaskPageLevelRow", rows, kMaxPageLevels);
    aShader.setUniformValue("uMaskSize", QVector2D((float)mSize.width(), (float)mSize.height()));
    aShader.setUniformValue("uMaskMaxLod", (float)mMaxLevel);
    aShader.setUniformValue("uMaskCoarseLevel", mCoarseLevel);
    aShader.setUniformValue("uMaskAtlas0", 12);
    aShader.setUniformValue("uMaskAtlas1", 13);
    aShader.setUniformValue("uMaskAtlas2", 14);
    aShader.setUniformValue("uMaskAtlas3", 15);

    // the page geometry both stacks share; a draw that pages only the mask (the rubylith
    // overlay) has no image VT to set them
    aShader.setUniformValue("uPageAtlasSize", (float)gl::PagePool::kAtlasSize);
    aShader.setUniformValue("uPageSlotsPerAxis", gl::PagePool::kSlotsPerAxis);
    aShader.setUniformValue("uPagePitch", (float)gl::PagePool::kPitch);
    aShader.setUniformValue("uPageGutter", (float)gl::PagePool::kGutter);
    aShader.setUniformValue("uPageTexels", (float)gl::PagePool::kPageSize);
    aShader.setUniformValue("uAtlasCount", (int)gl::PagePool::kMaxAtlasCount);
}

int VirtualTexture::residentWantedCount() const {
    int resident = 0;
    for (quint32 value : mActivePages) {
        const int index = (int)value;
        int px = 0, py = 0;
        const int level = levelOfIndex(index, &px, &py);
        gl::PagePool::Slot slot;
        if (gl::PagePool::instance().find(pageId(level, px, py), &slot))
            ++resident;
    }
    return resident;
}

} // namespace core
