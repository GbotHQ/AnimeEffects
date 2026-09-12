#ifndef CORE_RENDERER_H
#define CORE_RENDERER_H

#include <QVector3D>
#include <QList>
#include "img/BlendMode.h"
#include "core/RenderInfo.h"
#include "core/BlendIf.h"
#include "core/LayerEffects.h"
#include "core/TimeCacheAccessor.h"

namespace core {

class Renderer {
public:
    struct SortUnit {
        SortUnit(): renderer(), depth() {}
        Renderer* renderer;
        float depth;
        TimeLine* timeline;
    };

    virtual ~Renderer() {}

    virtual void prerender(const RenderInfo& aInfo, const TimeCacheAccessor&) = 0;

    virtual void render(const RenderInfo& aInfo, const TimeCacheAccessor&) = 0;

    virtual void renderClipper(const RenderInfo& aInfo, const TimeCacheAccessor&, uint8 aClipperId) = 0;

    virtual void setClipped(bool aIsClipped) = 0;
    virtual bool isClipped() const = 0;

    virtual bool hasBlendMode() const { return false; }
    virtual img::BlendMode blendMode() const { return img::BlendMode_TERM; }
    virtual void setBlendMode(img::BlendMode) {}

    // Photoshop "Blend If" ranges of this layer/folder (see core/BlendIf.h). Returned by
    // value: the default (inactive) value is the common case and keeps the interface free
    // of a storage requirement.
    virtual BlendIf blendIf() const { return BlendIf(); }
    virtual void setBlendIf(const BlendIf&) {}

    // the node's Photoshop layer effects (core/LayerEffects.h); the default is "none"
    virtual LayerEffects layerEffects() const { return LayerEffects(); }
    virtual void setLayerEffects(const LayerEffects&) {}

    // Photoshop's FILL opacity (the PSD "iOpa" key) as a 0..1 factor: it scales the node's
    // own pixels only, unlike the layer opacity which fades the layer's effects too
    virtual float fillOpacity() const { return 1.0f; }
};

} // namespace core

#endif // CORE_RENDERER_H
