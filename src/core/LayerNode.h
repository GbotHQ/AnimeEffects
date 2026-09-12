#ifndef CORE_LAYERNODE_H
#define CORE_LAYERNODE_H

#include <QVector>
#include "img/ResourceHandle.h"
#include "core/ObjectNode.h"
#include "core/Renderer.h"
#include "core/TimeLine.h"
#include "core/BoneInfluenceMap.h"
#include "core/MeshTransformer.h"
#include "core/ShaderHolder.h"

namespace core {

class LayerNode: public ObjectNode, public Renderer {
public:
    LayerNode(const QString& aName, ShaderHolder& aShaderHolder);

    util::LifeLink::Pointee<LayerNode> pointee() { return lifeLink().pointee<LayerNode>(this); }

    // default image
    void setDefaultImage(const img::ResourceHandle& aHandle);
    void setDefaultImage(const img::ResourceHandle& aHandle, img::BlendMode aBlendMode);
    // default posture
    void setDefaultPosture(const QVector2D& aPos);
    // default depth
    void setDefaultDepth(float aValue);
    // default opacity
    void setDefaultOpacity(float aValue);

    // from ObjectNode
    virtual ObjectType type() const { return ObjectType_Layer; }
    virtual void setName(const QString& aName) { mName = aName; }
    virtual const QString& name() const { return mName; }
    virtual float initialDepth() const;
    virtual void setVisibility(bool aIsVisible) { mIsVisible = aIsVisible; }
    virtual bool isVisible() const { return mIsVisible; }
    virtual void setSlimDown(bool aIsSlimmed) { mIsSlimmedDown = aIsSlimmed; }
    virtual bool isSlimmedDown() const { return mIsSlimmedDown; }
    virtual bool canHoldChild() const { return false; }
    virtual void setInitialRect(const QRect& aRect) { mInitialRect = aRect; }
    virtual QRect initialRect() const { return mInitialRect; }
    virtual Renderer* renderer() { return this; }
    virtual const Renderer* renderer() const { return this; }
    virtual TimeLine* timeLine() { return &mTimeLine; }
    virtual const TimeLine* timeLine() const { return &mTimeLine; }
    virtual bool hasAnyMesh() const { return true; }
    virtual bool hasAnyImage() const { return true; }

    virtual cmnd::Vector createResourceUpdater(const ResourceEvent& aEvent);

    void reserveShadersFromTimeline();

    virtual bool serialize(Serializer& aOut) const;
    virtual bool deserialize(Deserializer& aIn);

    // from Renderer
    virtual void prerender(const RenderInfo&, const TimeCacheAccessor&);
    virtual void render(const RenderInfo&, const TimeCacheAccessor&);
    virtual void renderClipper(const RenderInfo&, const TimeCacheAccessor&, uint8 aClipperId);
    virtual void setClipped(bool aIsClipped);
    virtual bool isClipped() const { return mIsClipped; }
    virtual bool hasBlendMode() const { return true; }
    virtual img::BlendMode blendMode() const;
    virtual void setBlendMode(img::BlendMode);
    virtual BlendIf blendIf() const { return mBlendIf; }
    virtual void setBlendIf(const BlendIf& aBlendIf) { mBlendIf = aBlendIf; }
    virtual LayerEffects layerEffects() const { return mLayerEffects; }

    // Photoshop's FILL opacity (the PSD "iOpa" key): it scales the layer's own pixels only,
    // so it is NOT the layer opacity (which also fades the layer's effects). 1.0 = opaque.
    virtual float fillOpacity() const override { return mFillOpacity; }
    void setFillOpacity(float aValue) { mFillOpacity = aValue; }
    virtual void setLayerEffects(const LayerEffects& aEffects) { mLayerEffects = aEffects; }
    
    private:
    void transformShape(const RenderInfo& aInfo, const TimeCacheAccessor&);
    // aApplyBlendIf=false when the caller renders this layer as ISOLATED content (the
    // composite/blur path draws into a slot with no scene behind it): the Blend If
    // destination range has no backdrop there, so the weight is left to the presentation
    // pass, which applies it against the captured scene (mirroring Photoshop's
    // "Underlying Layer" being the backdrop the layer finally composites onto)
    // aShapePass=true draws ONLY the layer's shape coverage: the fill opacity and the
    // fill-replacing effects are left out, because Photoshop derives a layer's shadow / glow
    // silhouettes from the layer's pixels, not from its fill opacity (a 0 % fill still glows)
    void renderLayer(const RenderInfo& aInfo, const TimeCacheAccessor&, bool useHSV, QList<int> HSVData,
        bool aApplyBlendIf = true, bool aShapePass = false);
    void renderComposite(const RenderInfo& aInfo, const TimeCacheAccessor&);
    void renderClippees(const RenderInfo& aInfo, const TimeCacheAccessor&);
    bool isClipper() const;
    bool hasActiveBlurKey(const TimeInfo& aTime) const;
    bool isCompositeLayer(const TimeInfo& aTime, const TimeCacheAccessor& aAccessor) const;

    // true when the layer's effects need the isolated composite slot even without a blur
    // key: Photoshop's drop shadow / outer glow / chrome blur a copy of the layer's alpha
    bool hasSilhouetteEffects() const;

    float mFillOpacity = 1.0f;
    
    QString mName;
    bool mIsVisible;
    bool mIsSlimmedDown;
    QRect mInitialRect;
    TimeLine mTimeLine;
    ShaderHolder& mShaderHolder;
    bool mIsClipped;
    BlendIf mBlendIf;
    LayerEffects mLayerEffects;

    MeshTransformer mMeshTransformer;
    LayerMesh* mCurrentMesh;
    std::vector<Renderer::SortUnit> mClippees; // a cache for performance
};

} // namespace core

#endif // CORE_LAYERNODE_H
