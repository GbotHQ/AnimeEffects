#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLContext>
#include <cmath>
#include "gl/Global.h"
#include "gl/Util.h"
#include "img/BlendMode.h"
#include "core/LayerNode.h"
#include "core/ObjectNodeUtil.h"
#include "core/TimeKeyExpans.h"
#include "core/DepthKey.h"
#include "core/ResourceEvent.h"
#include "core/ResourceUpdatingWorkspace.h"
#include "core/FFDKeyUpdater.h"
#include "core/ImageKeyUpdater.h"
#include "core/ClippingFrame.h"
#include "core/WorldBlurMath.h"
#include "core/DestinationTexturizer.h"
#include "core/FilterFrame.h"
#include "core/LayerEffects.h"

namespace core {

LayerNode::LayerNode(const QString& aName, ShaderHolder& aShaderHolder):
    mName(aName),
    mIsVisible(true),
    mIsSlimmedDown(),
    mInitialRect(),
    mTimeLine(),
    mShaderHolder(aShaderHolder),
    mIsClipped(),
    mMeshTransformer(":/shaders/MeshTransformVert.glsl"),
    mCurrentMesh(),
    mClippees() {}

void LayerNode::setDefaultImage(const img::ResourceHandle& aHandle) {
    setDefaultImage(aHandle, aHandle->blendMode());
}

void LayerNode::setDefaultImage(const img::ResourceHandle& aHandle, img::BlendMode aBlendMode) {
    /*XC_ASSERT(aHandle);
    XC_PTR_ASSERT(aHandle->image().data());
    XC_ASSERT(aHandle->image().pixelSize().isValid());*/
    if (!aHandle || !aHandle->image().data() || !aHandle->image().pixelSize().isValid())
        return; // fail safe

    auto key = new ImageKey();
    mTimeLine.grabDefaultKey(TimeKeyType_Image, key);
    key->setImage(aHandle, aBlendMode);
    key->resetGridMesh();
    key->setImageOffsetByCenter();

    mShaderHolder.reserveShaders(aBlendMode);
    mShaderHolder.reserveGridShader();
    mShaderHolder.reserveClipperShaders();
}

void LayerNode::setDefaultPosture(const QVector2D& aPos) {
    getOrCreateDefaultKey<MoveKey, TimeKeyType_Move>(mTimeLine)->data().setPos(aPos);
    getOrCreateDefaultKey<RotateKey, TimeKeyType_Rotate>(mTimeLine);
    getOrCreateDefaultKey<ScaleKey, TimeKeyType_Scale>(mTimeLine);
}

void LayerNode::setDefaultDepth(float aValue) {
    getOrCreateDefaultKey<DepthKey, TimeKeyType_Depth>(mTimeLine)->setDepth(aValue);
}

void LayerNode::setDefaultOpacity(float aValue) {
    getOrCreateDefaultKey<OpaKey, TimeKeyType_Opa>(mTimeLine)->setOpacity(aValue);
}

float LayerNode::initialDepth() const {
    auto key = (DepthKey*)mTimeLine.defaultKey(TimeKeyType_Depth);
    return key ? key->depth() : 0.0f;
}

void LayerNode::setClipped(bool aIsClipped) { mIsClipped = aIsClipped; }

bool LayerNode::isClipper()  const { return ObjectNodeUtil::isClipper(this); }

img::BlendMode LayerNode::blendMode() const {
    auto key = (ImageKey*)mTimeLine.defaultKey(TimeKeyType_Image);
    return key ? key->data().blendMode() : img::BlendMode_Normal;
}

void LayerNode::setBlendMode(img::BlendMode aMode) {
    auto key = (ImageKey*)mTimeLine.defaultKey(TimeKeyType_Image);
    if (key) {
        key->data().setBlendMode(aMode);
        mShaderHolder.reserveShaders(aMode);
    }
}

void LayerNode::prerender(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor) {
    mCurrentMesh = nullptr;

    if (!mIsVisible)
        return;

    // transform shape by current keys
    transformShape(aInfo, aAccessor);
}


void LayerNode::render(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor) {
    if (!mIsVisible || aAccessor.get(mTimeLine).opa().isZero()) return;

    bool useHSV = !mTimeLine.isEmpty(TimeKeyType_HSV) && aInfo.time.frame.get() >= mTimeLine.map(TimeKeyType_HSV).values().first()->frame();
    const QList<int> hsvData = useHSV ? aAccessor.get(mTimeLine).hsv().hsv() : QList<int>{};

    if (!aInfo.isGrid && (isCompositeLayer(aInfo.time, aAccessor) || hasSilhouetteEffects())) {
        renderComposite(aInfo, aAccessor);
    } else {
        renderLayer(aInfo, aAccessor, useHSV, hsvData);
    }

    if (aInfo.isGrid) return;

    renderClippees(aInfo, aAccessor);
}

bool LayerNode::hasActiveBlurKey(const TimeInfo& aTime) const {
    return !mTimeLine.isEmpty(TimeKeyType_Blur)
        && aTime.frame.get() >= mTimeLine.map(TimeKeyType_Blur).values().first()->frame();
}

bool LayerNode::hasSilhouetteEffects() const {
    // Photoshop's drop shadow / outer glow / chrome blur a copy of the layer's alpha, so
    // they need the same isolated slot the layer blur uses even when no blur key is set
    return mLayerEffects.hasSilhouettes();
}

bool LayerNode::isCompositeLayer(const TimeInfo& aTime, const TimeCacheAccessor& aAccessor) const {
    // a BlurKey whose blended radius is at or below the epsilon has no visible effect
    // (identity kernel, see FilterFrame::kMinActiveBlurRadius), so it must not force
    // the layer onto the composite path (mirrors the folder gate); the epsilon keeps
    // interpolations ramping in smoothly from zero
    return hasActiveBlurKey(aTime)
        && aAccessor.get(mTimeLine).maxBlurRadius() > FilterFrame::kMinActiveBlurRadius;
}

// Blurs the content of a slot IN PLACE (the slot's texture is overwritten) and returns the
// texture holding the result: the ladder path already writes back into the slot, the direct
// two-pass path ping-pongs through one scratch slot.
static GLuint fBlurSlotInPlace(
    FilterFrame& aFrame, int aSlot, const QVector2D& aDirX, float aRadiusX, const QVector2D& aDirY, float aRadiusY
) {
    if (aRadiusX <= FilterFrame::kMinActiveBlurRadius && aRadiusY <= FilterFrame::kMinActiveBlurRadius) {
        return aFrame.texture(aSlot);
    }

    if (FilterFrame::blurLadderLevel(aRadiusX, aRadiusY, aFrame.size()) > 0) {
        return aFrame.blurApply(aSlot, aDirX, aRadiusX, aDirY, aRadiusY);
    }

    // the composite slot holds the screen image y-flipped (texture row = H - screen y), so a
    // screen-space direction (dx, dy) appears in slot space as (dx, -dy); the ladder path
    // needs no adjustment (its first downsample flips back)
    FilterFrame::BlurParams h;
    h.direction = QVector2D(aDirX.x(), -aDirX.y());
    h.radiusTexels = aRadiusX;
    FilterFrame::BlurParams v;
    v.direction = QVector2D(aDirY.x(), -aDirY.y());
    v.radiusTexels = aRadiusY;

    auto& ggl = gl::Global::functions();
    const auto clearAndBind = [&](int aTargetSlot) {
        aFrame.bind(aTargetSlot);
        gl::Util::setViewportAsActualPixels(aFrame.size());
        gl::Util::resetRenderState();
        ggl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        ggl.glClear(GL_COLOR_BUFFER_BIT);
    };

    FilterFrame::ScopedSlot scratch(aFrame);
    clearAndBind(scratch.slot());
    aFrame.drawQuad(aFrame.texture(aSlot), FilterFrame::Kind_Blur, QList<int>(), h, 1.0f);

    // the second pass writes back into the source slot (the same reuse the ladder path
    // applies on its final upsample), so the direct blur holds one scratch slot
    clearAndBind(aSlot);
    aFrame.drawQuad(scratch.texture(), FilterFrame::Kind_Blur, QList<int>(), v, 1.0f);

    return aFrame.texture(aSlot);
}

static void fClearAndBindSlot(FilterFrame& aFrame, int aSlot) {
    aFrame.bind(aSlot);
    gl::Util::setViewportAsActualPixels(aFrame.size());
    gl::Util::resetRenderState();
    gl::Global::functions().glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl::Global::functions().glClear(GL_COLOR_BUFFER_BIT);
}

// The layer's own silhouette effects (Photoshop's drop shadow, outer glow and chrome):
// each one blurs a copy of the layer's alpha, tints it with the effect colour and
// composites THAT against the backdrop — under the layer's pixels for shadows and glows,
// over them for chrome. aOverTheLayer selects which half of the split runs, so the caller
// can sandwich the layer's own present between them. All positions are in document pixels
// scaled by the camera zoom, like the layer's own blur radius. Returns the number of
// effects presented.
static int fPresentSilhouettes(
    const RenderInfo& aInfo, FilterFrame& aFrame, GLuint aShapeTexture, const LayerEffects& aEffects,
    float aLayerOpacity, bool aOverTheLayer
) {
    if (!aEffects.hasSilhouettes()) {
        return 0;
    }

    auto& ggl = gl::Global::functions();
    const float zoom = aInfo.camera.scale();
    int drawn = 0;

    for (const auto& s : aEffects.silhouettes) {
        if (!s.isUsable() || s.isUnder() == aOverTheLayer) {
            continue;
        }

        const float radius = s.blur * zoom;
        const float angleRad = (float)(s.angleDeg * M_PI / 180.0);
        // Photoshop's effect angle is counter-clockwise with 0 pointing right, so the
        // silhouette lands at (distance*cos, distance*sin) from the layer. A draw samples
        // the source at (+offset), which moves the sampled content by (-offset), so the
        // offset passed down is the negated displacement.
        const QVector2D offset(
            -s.distance * zoom * std::cos(angleRad), -s.distance * zoom * std::sin(angleRad));

        // 1. a blurred copy of the layer's silhouette (the isolated content the layer blur
        //    produced), tinted with the effect colour; the copy is blurred through the same
        //    Gaussian the layer blur uses, with the effect's radius
        GLuint silhouette = 0;
        {
            // The offset is applied to the COPY, not to the tint pass: the tint pass also samples
            // the unblurred shape to clip an inner effect, and a source offset shifts every
            // sample of that pass - including the clip - which would move the clip along with the
            // silhouette and clip nothing (measured: the chrome then spilled a 15 px halo outside
            // the glyphs, 18-24 levels brighter than the file's own composite). Copying through
            // the offset and blurring the shifted copy is equivalent, because the Gaussian is
            // symmetric, and it leaves the tint pass unshifted so the clip lands on the real shape.
            FilterFrame::ScopedSlot work(aFrame);
            fClearAndBindSlot(aFrame, work.slot());
            aFrame.drawQuad(aShapeTexture, FilterFrame::Kind_Resample, QList<int>(), FilterFrame::BlurParams(),
                1.0f, false, QSizeF(), QSizeF(), false, img::BlendMode_Normal, 0, BlendIf(),
                QColor(255, 255, 255, 255), offset);
            silhouette = fBlurSlotInPlace(
                aFrame, work.slot(), QVector2D(1.0f, 0.0f), radius, QVector2D(0.0f, 1.0f), radius);

            FilterFrame::ScopedSlot tinted(aFrame);
            fClearAndBindSlot(aFrame, tinted.slot());
            // an inner effect (chrome) is capped by the unblurred shape, so it paints only inside
            // the layer; an outer one (shadow, glow) is free to spread past it
            const GLuint clipTexture = s.isInner() ? aShapeTexture : 0;
            aFrame.drawQuad(
                silhouette, FilterFrame::Kind_Tint, QList<int>(), FilterFrame::BlurParams(), 1.0f, false,
                QSizeF(), QSizeF(), false, img::BlendMode_Normal, 0, BlendIf(),
                QColor::fromRgbF(xc_clamp(s.r, 0.0f, 1.0f), xc_clamp(s.g, 0.0f, 1.0f), xc_clamp(s.b, 0.0f, 1.0f)),
                QVector2D(), clipTexture);

            // 2. composite the tinted silhouette against the scene behind the layer with the
            //    effect's own blend mode. The layer's opacity fades its effects with it.
            ggl.glBindFramebuffer(GL_FRAMEBUFFER, aInfo.framebuffer);
            const img::BlendMode mode = s.mode;
            GLuint destTexture = 0;
            if (mode != img::BlendMode_Normal) {
                XC_PTR_ASSERT(aInfo.destTexturizer);
                aInfo.destTexturizer->updateAll(aInfo.framebuffer, aInfo.dest);
                destTexture = aInfo.destTexturizer->texture().id();
            }
            aFrame.drawBlendPresent(tinted.texture(), mode, destTexture, aLayerOpacity * s.opacity);
            ++drawn;
        }
    }

    return drawn;
}

// A blurred layer is isolated: the layer draws into a composite slot (its own HSV, blend
// mode and clipping preserved), the isolated content is blurred, and the result is
// presented onto the scene with the layer's blend mode at the layer's accumulated opacity.
void LayerNode::renderComposite(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor) {
    XC_PTR_ASSERT(aInfo.filterFrame);
    auto& frame = *aInfo.filterFrame;
    if (!mCurrentMesh)
        return;

    auto& expans = aAccessor.get(mTimeLine);
    if (!expans.areaImageKey() || !expans.areaTexture())
        return;

    const float worldOpacity = expans.worldOpacity();
    if (worldOpacity <= 0.0f)
        return;

    const bool useHSV = !mTimeLine.isEmpty(TimeKeyType_HSV)
        && aInfo.time.frame.get() >= mTimeLine.map(TimeKeyType_HSV).values().first()->frame();
    const QList<int> hsvData = useHSV ? expans.hsv().hsv() : QList<int>{};

    // 1. render the layer into a composite slot at full opacity (its opacity is applied
    // at presentation so the blur samples the crisp shape, not a faded one)
    FilterFrame::ScopedSlot composite(frame);
    frame.bind(composite.slot());
    gl::Util::setViewportAsActualPixels(frame.size());
    gl::Util::resetRenderState();
    gl::Global::functions().glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl::Global::functions().glClear(GL_COLOR_BUFFER_BIT);

    {
        RenderInfo childInfo = aInfo;
        childInfo.framebuffer = composite.framebuffer();
        childInfo.dest = composite.texture();
        childInfo.opacityScale = 1.0f / worldOpacity;
        // the slot holds the layer alone: its Blend If ranges are applied when this content
        // is presented against the scene (step 3), not against the empty slot
        renderLayer(childInfo, aAccessor, useHSV, hsvData, false);
    }

    // 2. blur the isolated content (same radius space as the folder filter: the amount is
    // in the layer's content pixels - a content-space ellipse when directional; the
    // content ellipse maps to an ellipse in the composite via the accumulated world
    // transform M (composed as M*E), so the separable passes run along its principal axes
    // with the resulting singular values as radii, scaled by the camera zoom)
    // The layer's blur, if any: the ellipse lives in the layer's content space and is mapped
    // into the composite slot's screen space (the slot holds the scene as rendered through
    // the camera, whose view linear part is R(rotate) * diag(flip ? -1 : 1, 1); zoom is
    // isotropic, so directions stay unit). The silhouette pass reuses the same radii.
    const WorldBlurEllipse ellipse = worldBlurEllipse(
        aAccessor, this, expans.blurX(), expans.blurY(), expans.angleDeg());
    const float zoom = aInfo.camera.scale();
    const float radiusX = ellipse.majorRadius * zoom;
    const float radiusY = ellipse.minorRadius * zoom;
    const bool layerBlurActive =
        expans.maxBlurRadius() > FilterFrame::kMinActiveBlurRadius && hasActiveBlurKey(aInfo.time);
    const float flipX = aInfo.camera.flip ? -1.0f : 1.0f;
    const float cr = std::cos(aInfo.camera.rotate());
    const float sr = std::sin(aInfo.camera.rotate());
    const auto screenDir = [=](const QVector2D& d) {
        const float x = flipX * d.x();
        const float y = d.y();
        return QVector2D(cr * x - sr * y, sr * x + cr * y);
    };
    const QVector2D majorScreen = screenDir(ellipse.majorDir);
    const QVector2D minorScreen = screenDir(ellipse.minorDir);

    GLuint srcTexture = composite.texture();
    if (layerBlurActive) {
        srcTexture = fBlurSlotInPlace(frame, composite.slot(), majorScreen, radiusX, minorScreen, radiusY);
    }

    const float presentOpacity = worldOpacity * aInfo.opacityScale;

    // 3. the silhouette source for the layer's own effects: Photoshop feeds a layer's shadow
    // and glow the layer's SHAPE, so a fill opacity below 100 % must not thin the silhouette.
    // The content above already is that shape whenever the fill is opaque (the shader's inner
    // coverage is then exactly the fill coverage, leaving the alpha untouched); a partially
    // transparent fill needs its own pass, drawn without the fill opacity and without the
    // fill-replacing effects, and blurred like the content.
    FilterFrame::ScopedSlot shapeSlot(frame);
    GLuint shapeTexture = srcTexture;
    if (mLayerEffects.hasSilhouettes() && this->fillOpacity() < 1.0f) {
        fClearAndBindSlot(frame, shapeSlot.slot());
        RenderInfo shapeInfo = aInfo;
        shapeInfo.framebuffer = shapeSlot.framebuffer();
        shapeInfo.dest = shapeSlot.texture();
        shapeInfo.opacityScale = 1.0f / worldOpacity;
        renderLayer(shapeInfo, aAccessor, useHSV, hsvData, false, true);
        shapeTexture = shapeSlot.texture();
        if (layerBlurActive) {
            shapeTexture = fBlurSlotInPlace(frame, shapeSlot.slot(), majorScreen, radiusX, minorScreen, radiusY);
        }
    }

    // 4. the layer's silhouette effects that sit UNDER it (drop shadow, outer glow): they
    // composite against the backdrop first, so the layer's own pixels cover them
    fPresentSilhouettes(aInfo, frame, shapeTexture, mLayerEffects, presentOpacity, false);

    // 5. present onto the scene with the layer's blend mode and accumulated opacity.
    // non-Normal blend modes sample the captured scene behind the layer (DestinationText
    // capture), so the blend sees real background pixels instead of the composite itself.
    const img::BlendMode blendMode = expans.blendMode();
    const BlendIf blendIf = this->blendIf();
    auto& ggl = gl::Global::functions();
    ggl.glBindFramebuffer(GL_FRAMEBUFFER, aInfo.framebuffer);
    if (blendMode != img::BlendMode_Normal || blendIf.isActive()) {
        XC_PTR_ASSERT(aInfo.destTexturizer);
        aInfo.destTexturizer->updateAll(aInfo.framebuffer, aInfo.dest);
        frame.drawBlendPresent(
            srcTexture, blendMode, aInfo.destTexturizer->texture().id(), presentOpacity, blendIf);
    } else {
        frame.drawBlendPresent(srcTexture, blendMode, 0, presentOpacity, blendIf);
    }

    // 6. the silhouette effects that sit OVER the layer (chrome). Like the under-effects they
    // read the layer's SHAPE, not the fill-scaled content: a 0 % fill still has a chrome.
    fPresentSilhouettes(aInfo, frame, shapeTexture, mLayerEffects, presentOpacity, true);
}

void LayerNode::renderClippees(const RenderInfo& i, const TimeCacheAccessor& a)
{
    ObjectNodeUtil::renderClippees(*this, mClippees, i, a,
        [this](const auto& inf, const auto& acc, uint8 id){ renderClipper(inf, acc, id); });
}

void LayerNode::renderClipper(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor, uint8 aClipperId) {
    if (!mIsVisible || !aInfo.clippingFrame)
        return;
    if (!mCurrentMesh)
        return;

    gl::Global::Functions& ggl = gl::Global::functions();
    auto& shader = mShaderHolder.clipperShader(aInfo.clippingId != 0);
    auto& expans = aAccessor.get(mTimeLine);

    if (!expans.areaTexture())
        return;
    auto textureId = expans.areaTexture()->id();
    auto textureSize = expans.areaTexture()->size();
    auto texCoordOffset = mCurrentMesh->originOffset() - expans.imageOffset();

    core::ClippingFrame& frame = *aInfo.clippingFrame;
    frame.updateRenderStamp();

    // bind framebuffer
    frame.bind();
    frame.setupDrawBuffers();

    // bind textures
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, textureId);
    ggl.glActiveTexture(GL_TEXTURE1);
    ggl.glBindTexture(GL_TEXTURE_2D, frame.texture().id());

    // view matrix
    const QMatrix4x4 viewMatrix = aInfo.camera.viewMatrix();
    // color
    const float opacity = expans.worldOpacity() * aInfo.opacityScale;
    QColor color(255, 255, 255, xc_clamp((int)(255 * opacity), 0, 255));
    {
        shader.bind();

        shader.setAttributeBuffer("inPosition", mMeshTransformer.positions(), GL_FLOAT, 3);
        shader.setAttributeArray("inTexCoord", mCurrentMesh->texCoords(), mCurrentMesh->vertexCount());

        shader.setUniformValue("uViewMatrix", viewMatrix);
        shader.setUniformValue("uScreenSize", QSizeF(aInfo.camera.deviceScreenSize()));
        shader.setUniformValue("uImageSize", QSizeF(textureSize));
        shader.setUniformValue("uTexCoordOffset", texCoordOffset);
        shader.setUniformValue("uColor", color);
        shader.setUniformValue("uClipperId", (int)aClipperId);
        shader.setUniformValue("uTexture", 0);
        shader.setUniformValue("uDestTexture", 1);

        if (aInfo.clippingId != 0) {
            shader.setUniformValue("uClippingId", (int)aInfo.clippingId);
        }

        gl::Util::drawElements(mCurrentMesh->primitiveMode(), GL_UNSIGNED_INT, mCurrentMesh->getIndexBuffer());

        shader.release();
    }
    // unbind texture
    ggl.glActiveTexture(GL_TEXTURE0);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    // release framebuffer
    frame.release();

    // bind default framebuffer
    ggl.glBindFramebuffer(GL_FRAMEBUFFER, aInfo.framebuffer);

    ggl.glFlush();
}

void LayerNode::transformShape(const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor) {
    auto& expans = aAccessor.get(mTimeLine);

    // current mesh
    LayerMesh* mesh = expans.ffdMesh();
    if (!mesh || mesh->vertexCount() <= 0)
        return;

    // positions
    util::ArrayBlock<const gl::Vector3> positions;
    bool useInfluence = true;

    if (aInfo.originMesh) // ignore mesh deforming
    {
        useInfluence = false;
        if (expans.areaImageKey()) {
            mesh = &(expans.areaImageKey()->data().gridMesh());
            if (!mesh || mesh->vertexCount() <= 0)
                return;
        }
        positions = util::ArrayBlock<const gl::Vector3>(mesh->positions(), mesh->vertexCount());
    }
    else {
        useInfluence = (mesh == expans.bone().targetMesh());
        positions = util::ArrayBlock<const gl::Vector3>(expans.ffd().positions(), expans.ffd().count());
        XC_MSG_ASSERT(
            mesh->vertexCount() == positions.count(), "vtx count = %d, %d", mesh->vertexCount(), positions.count()
        );
    }
    XC_ASSERT(positions);

    // transform
    mMeshTransformer.callGL(
        expans, mesh->getMeshBuffer(), mesh->originOffset(), positions, aInfo.nonPosed, useInfluence
    );

    mCurrentMesh = mesh;
}

void LayerNode::renderLayer(
    const RenderInfo& aInfo, const TimeCacheAccessor& aAccessor, bool useHSV, QList<int> HSVData,
    bool aApplyBlendIf, bool aShapePass
) {
    if (!mCurrentMesh)
        return;

    gl::Global::Functions& ggl = gl::Global::functions();
    const bool isClippee = (aInfo.clippingFrame && aInfo.clippingId != 0);

    auto& expans = aAccessor.get(mTimeLine);
    if (!expans.areaImageKey() || !expans.areaTexture())
        return;

    auto textureId = expans.areaTexture()->id();
    auto textureSize = expans.areaTexture()->size();
    auto texCoordOffset = mCurrentMesh->originOffset() - expans.imageOffset();
    auto blendMode = expans.blendMode();
    const QMatrix4x4 viewMatrix = aInfo.camera.viewMatrix();

    auto& shader = aInfo.isGrid ? mShaderHolder.gridShader() : mShaderHolder.shader(blendMode, isClippee, useHSV);

    // Blend If reads the backdrop's channel values, so a Normal-blend layer with active
    // ranges captures the scene behind it too (not just the non-Normal blend modes)
    const BlendIf blendIf = aApplyBlendIf ? this->blendIf() : BlendIf();

    // update destination color
    XC_PTR_ASSERT(aInfo.destTexturizer);
    auto destTextureId = aInfo.destTexturizer->texture().id();
    if (!aInfo.isGrid && (blendMode != img::BlendMode_Normal || blendIf.isActive())) {
        aInfo.destTexturizer->update(
            aInfo.framebuffer, aInfo.dest, viewMatrix, *mCurrentMesh, mMeshTransformer.positions()
        );
    }

    // the layer's mask texture, only on the non-grid path (the grid uses a plain shader)
    const gl::Texture* maskTexture = !aInfo.isGrid ? expans.areaMaskTexture() : nullptr;
    // the outline distance field a stroke needs, from the same resource
    const gl::Texture* distanceTexture = !aInfo.isGrid ? expans.areaDistanceTexture() : nullptr;

    if (aInfo.isGrid) {
        ggl.glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        // blend func
        ggl.glEnable(GL_BLEND);
        ggl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        ggl.glActiveTexture(GL_TEXTURE0);
        ggl.glBindTexture(GL_TEXTURE_2D, textureId);
        ggl.glActiveTexture(GL_TEXTURE1);
        ggl.glBindTexture(GL_TEXTURE_2D, destTextureId);

        // layer mask on unit 2 (0 = unused); when the layer has none the image texture is
        // bound as a dummy, since the shader's uniform-conditional fetch never runs
        ggl.glActiveTexture(GL_TEXTURE2);
        ggl.glBindTexture(GL_TEXTURE_2D, maskTexture ? maskTexture->id() : textureId);

        if (isClippee) {
            ggl.glActiveTexture(GL_TEXTURE3);
            ggl.glBindTexture(GL_TEXTURE_2D, aInfo.clippingFrame->texture().id());
        }
    }

    {
        const float opacity = expans.worldOpacity() * aInfo.opacityScale;
        QColor color(255, 255, 255, xc_clamp((int)(255 * opacity), 0, 255));
        if (aInfo.isGrid)
            color = QColor(Qt::black);

        shader.bind();
        shader.setAttributeBuffer("inPosition", mMeshTransformer.positions(), GL_FLOAT, 3);
        shader.setAttributeArray("inTexCoord", mCurrentMesh->texCoords(), mCurrentMesh->vertexCount());

        shader.setUniformValue("uViewMatrix", viewMatrix);
        shader.setUniformValue("uScreenSize", QSizeF(aInfo.camera.deviceScreenSize()));
        shader.setUniformValue("uImageSize", QSizeF(textureSize));
        shader.setUniformValue("uTexCoordOffset", texCoordOffset);
        shader.setUniformValue("uColor", color);
        // the fill opacity scales the layer's own pixels inside the shader; a grid draw is a
        // debug view of the raw image, so it stays at 1 (an unset uniform reads as 0)
        shader.setUniformValue(
            "uFillOpacity", (aInfo.isGrid || aShapePass) ? 1.0f : this->fillOpacity());
        shader.setUniformValue("uTexture", 0);
        shader.setUniformValue("uDestTexture", 1);
        if (!aInfo.isGrid) {
            // the ranges live in the linked program, so every draw states them: a layer
            // without Blend If disables the weight explicitly (the grid shader has none)
            applyBlendIfUniforms(shader, blendIf);
            // fill-replacing layer effects ride the content pass, like the mask: they change
            // what the layer looks like, not how much of it the scene receives. A shape pass
            // (the silhouette source of the layer's own effects) leaves them off.
            applyEffectsUniforms(shader, aShapePass ? LayerEffects() : this->layerEffects());
        }

        if (!aInfo.isGrid) {
            // a disabled mask keeps its pixels but must not be applied; the mask texture
            // rides the image's texture coordinates, so no extra mapping is needed
            const auto& maskRes = *expans.areaImageKey()->data().resource();
            const int maskEnable =
                (maskTexture && maskRes.isMaskEnabled()) ? (maskRes.isMaskInverted() ? 2 : 1) : 0;
            shader.setUniformValue("uMaskTexture", 2);
            shader.setUniformValue("uMaskEnable", maskEnable);

            // the stroke's outline distance field, in the image's coordinates as well. A
            // layer without a usable field leaves the stroke pass off (a shape pass, which
            // draws the bare silhouette, never strokes).
            shader.setUniformValue("uDistanceTexture", 4);
            if (aShapePass || !distanceTexture) {
                shader.setUniformValue("uStrokeEnable", 0);
            } else {
                ggl.glActiveTexture(GL_TEXTURE4);
                ggl.glBindTexture(GL_TEXTURE_2D, distanceTexture->id());
            }
        }

        if (isClippee) {
            shader.setUniformValue("uClippingId", (int)aInfo.clippingId);
            shader.setUniformValue("uClippingTexture", 3);
        }

        if (useHSV && !HSVData.isEmpty()) {
            shader.setUniformValue("setColor", bool(HSVData[3]));

            float hue = (float)HSVData[0] / 360.0f;
            float saturation = (float)(HSVData[1]) / 100.0f;
            float value = (float)(HSVData[2]) / 100.0f;

            shader.setUniformValue("hue", hue);
            shader.setUniformValue("saturation", saturation);
            shader.setUniformValue("value", value);
        }

        gl::Util::drawElements(mCurrentMesh->primitiveMode(), GL_UNSIGNED_INT, mCurrentMesh->getIndexBuffer());

        shader.release();
    }

    if (aInfo.isGrid) {
        ggl.glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    } else {
        ggl.glActiveTexture(GL_TEXTURE0);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);

        // blend func
        ggl.glDisable(GL_BLEND);
    }

    ggl.glFlush();
}

cmnd::Vector LayerNode::createResourceUpdater(const ResourceEvent& aEvent) {
    cmnd::Vector result;

    if (aEvent.type() != ResourceEvent::Type_Reload) {
        return result;
    }

    ResourceUpdatingWorkspacePtr workspace = std::make_shared<ResourceUpdatingWorkspace>();
    const bool createTransitions = !mTimeLine.isEmpty(TimeKeyType_FFD);

    // image key
    result.push(ImageKeyUpdater::createResourceUpdater(*this, aEvent, workspace, createTransitions));

    // ffd key should be called finally
    if (createTransitions) {
        result.push(FFDKeyUpdater::createResourceUpdater(*this, workspace));
    }

    return result;
}

void LayerNode::reserveShadersFromTimeline() {
    mShaderHolder.reserveGridShader();
    mShaderHolder.reserveClipperShaders();

    auto reserveOne = [this](ImageKey* k) {
        mShaderHolder.reserveShaders(k->data().blendMode());
    };

    if (auto def = static_cast<ImageKey*>(mTimeLine.defaultKey(TimeKeyType_Image)))
        reserveOne(def);

    for (auto key : mTimeLine.map(TimeKeyType_Image))
        reserveOne(static_cast<ImageKey*>(key));
}

bool LayerNode::serialize(Serializer& aOut) const {
    static const std::array<uint8, 8> sig = {'L', 'a', 'y', 'e', 'r', 'N', 'd', '_'};
    if (!ObjectNodeUtil::writeObjectBlock(aOut, sig, mName, mIsVisible, mIsSlimmedDown,
            mInitialRect, mIsClipped, mTimeLine)) {
        return false;
    }
    // the Blend If ranges landed in AE_PROJECT_FORMAT_MINOR_VERSION 12; like the folder
    // blend mode they ride the object block, appended after the time line so a stream read
    // under an older version label stops exactly where that version's layout ended
    aOut.write(mBlendIf.dst.packed());
    aOut.write(mBlendIf.src.packed());

    // layer effects (minor 14): the effects this build draws, plus which effect keys the file
    // contained so the UI can report the ones that are only parsed
    {
        const int flags = (mLayerEffects.colorOverlay.enabled ? 1 : 0)
            | (mLayerEffects.gradientOverlay.enabled ? 2 : 0) | (mLayerEffects.stroke.isUsable() ? 4 : 0);
        aOut.write(flags);
        if (mLayerEffects.colorOverlay.enabled) {
            aOut.write(mLayerEffects.colorOverlay.r);
            aOut.write(mLayerEffects.colorOverlay.g);
            aOut.write(mLayerEffects.colorOverlay.b);
            aOut.write(mLayerEffects.colorOverlay.opacity);
        }
        if (mLayerEffects.gradientOverlay.enabled) {
            const auto& grad = mLayerEffects.gradientOverlay;
            aOut.write(grad.opacity);
            aOut.write(grad.angleDeg);
            aOut.write(grad.scale);
            aOut.write(grad.reverse);
            aOut.write(grad.offsetX);
            aOut.write(grad.offsetY);
            aOut.write((int)grad.stops.size());
            for (const auto& stop : grad.stops) {
                aOut.write(stop.location);
                aOut.write(stop.r);
                aOut.write(stop.g);
                aOut.write(stop.b);
                aOut.write(stop.opacity);
            }
        }
        if (mLayerEffects.stroke.isUsable()) {
            // the stroke's own parameters: its band, paint and gradient anchoring. A file
            // written before this field existed has no bit 2 in the flags word, so nothing is
            // read for it.
            const auto& st = mLayerEffects.stroke;
            aOut.write(st.size);
            aOut.write(QString::fromStdString(st.style));
            aOut.write(QString::fromStdString(st.paintType));
            aOut.write(st.r);
            aOut.write(st.g);
            aOut.write(st.b);
            aOut.write(st.opacity);
            aOut.write(QString::fromStdString(st.blendMode));
            aOut.write((int)st.mode);
            aOut.write((int)(st.blendModeKnown ? 1 : 0));
            aOut.write(st.angleDeg);
            aOut.write(st.scale);
            aOut.write(st.offsetX);
            aOut.write(st.offsetY);
            aOut.write(st.reverse);
            aOut.write((int)st.stops.size());
            for (const auto& stop : st.stops) {
                aOut.write(stop.location);
                aOut.write(stop.r);
                aOut.write(stop.g);
                aOut.write(stop.b);
                aOut.write(stop.opacity);
            }
        }
        int seen = 0;
        int active = 0;
        for (int i = 0; i < LayerEffects::Type_TERM; ++i) {
            if (mLayerEffects.seen[i]) {
                seen |= (1 << i);
            }
            if (mLayerEffects.active[i]) {
                active |= (1 << i);
            }
        }
        aOut.write(seen);
        aOut.write(active);
        // the blurred-silhouette effects the renderer draws
        aOut.write((int)mLayerEffects.silhouettes.size());
        for (const auto& s : mLayerEffects.silhouettes) {
            aOut.write((int)s.kind);
            aOut.write(s.blur);
            aOut.write(s.distance);
            aOut.write(s.angleDeg);
            aOut.write(s.r);
            aOut.write(s.g);
            aOut.write(s.b);
            aOut.write(s.opacity);
            aOut.write((int)s.blendMode.size());
            for (char c : s.blendMode) {
                aOut.write((int)(uint8)c);
            }
            aOut.write((int)s.mode);
            aOut.write((int)(s.blendModeKnown ? 1 : 0));
        }
        aOut.write(mFillOpacity);
    }
    return aOut.checkStream();
}

bool LayerNode::deserialize(Deserializer& aIn) {
    auto res = ObjectNodeUtil::readObjectBlock(aIn, "LayerNd_", mName, mIsVisible, mIsSlimmedDown,
                            mInitialRect, mIsClipped, mTimeLine);
    if (!res) return res;

    // projects saved before the Blend If bump (minor < 12) end here; the layer keeps the
    // default (inactive) ranges
    if (aIn.version().minorVersion() >= 12) {
        int packed = 0;
        aIn.read(packed);
        mBlendIf.dst = BlendIf::Range::unpacked(packed);
        aIn.read(packed);
        mBlendIf.src = BlendIf::Range::unpacked(packed);
    }
    // layer effects (minor 14)
    if (aIn.version().minorVersion() >= 14) {
        int flags = 0;
        aIn.read(flags);
        if (flags & 1) {
            mLayerEffects.colorOverlay.enabled = true;
            aIn.read(mLayerEffects.colorOverlay.r);
            aIn.read(mLayerEffects.colorOverlay.g);
            aIn.read(mLayerEffects.colorOverlay.b);
            aIn.read(mLayerEffects.colorOverlay.opacity);
        }
        if (flags & 2) {
            auto& grad = mLayerEffects.gradientOverlay;
            grad.enabled = true;
            aIn.read(grad.opacity);
            aIn.read(grad.angleDeg);
            aIn.read(grad.scale);
            aIn.read(grad.reverse);
            aIn.read(grad.offsetX);
            aIn.read(grad.offsetY);
            int stopCount = 0;
            aIn.read(stopCount);
            for (int i = 0; i < stopCount; ++i) {
                LayerEffects::Stop stop;
                aIn.read(stop.location);
                aIn.read(stop.r);
                aIn.read(stop.g);
                aIn.read(stop.b);
                aIn.read(stop.opacity);
                grad.stops.push_back(stop);
            }
            grad.style = "Lnr "; // the layout only carries the linear form this build draws
            grad.blendMode = "Nrml";
        }
        if (flags & 4) {
            auto& st = mLayerEffects.stroke;
            st.enabled = true;
            aIn.read(st.size);
            QString style, paintType, blendMode;
            aIn.read(style);
            aIn.read(paintType);
            st.style = style.toStdString();
            st.paintType = paintType.toStdString();
            aIn.read(st.r);
            aIn.read(st.g);
            aIn.read(st.b);
            aIn.read(st.opacity);
            aIn.read(blendMode);
            st.blendMode = blendMode.toStdString();
            int mode = 0;
            int known = 1;
            aIn.read(mode);
            aIn.read(known);
            st.mode = (img::BlendMode)mode;
            st.blendModeKnown = (known != 0);
            aIn.read(st.angleDeg);
            aIn.read(st.scale);
            aIn.read(st.offsetX);
            aIn.read(st.offsetY);
            aIn.read(st.reverse);
            int stopCount = 0;
            aIn.read(stopCount);
            for (int i = 0; i < stopCount && i < LayerEffects::kMaxStops; ++i) {
                LayerEffects::Stop stop;
                aIn.read(stop.location);
                aIn.read(stop.r);
                aIn.read(stop.g);
                aIn.read(stop.b);
                aIn.read(stop.opacity);
                st.stops.push_back(stop);
            }
        }
        int seen = 0;
        aIn.read(seen);
        for (int i = 0; i < LayerEffects::Type_TERM; ++i) {
            mLayerEffects.seen[i] = (seen & (1 << i)) != 0;
        }
        int active = 0;
        aIn.read(active);
        for (int i = 0; i < LayerEffects::Type_TERM; ++i) {
            mLayerEffects.active[i] = (active & (1 << i)) != 0;
        }
        int silhouetteCount = 0;
        aIn.read(silhouetteCount);
        for (int i = 0; i < silhouetteCount && i < 32; ++i) {
            LayerEffects::Silhouette s;
            int kind = 0;
            aIn.read(kind);
            s.kind = (LayerEffects::Silhouette::Kind)kind;
            s.enabled = true;
            aIn.read(s.blur);
            aIn.read(s.distance);
            aIn.read(s.angleDeg);
            aIn.read(s.r);
            aIn.read(s.g);
            aIn.read(s.b);
            aIn.read(s.opacity);
            int len = 0;
            aIn.read(len);
            for (int k = 0; k < len && k < 8; ++k) {
                int c = 0;
                aIn.read(c);
                s.blendMode.push_back((char)(uint8)c);
            }
            int mode = 0;
            int known = 1;
            aIn.read(mode);
            aIn.read(known);
            s.mode = (img::BlendMode)mode;
            s.blendModeKnown = (known != 0);
            mLayerEffects.silhouettes.push_back(s);
        }
        aIn.read(mFillOpacity);
    }
    reserveShadersFromTimeline();
    return res;
}

} // namespace core
