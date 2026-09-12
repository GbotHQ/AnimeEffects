#ifndef CORE_LAYERMASKBAKE_H
#define CORE_LAYERMASKBAKE_H

#include <cstdint>
#include <vector>
#include <QSize>
#include "img/Format.h"
#include "core/BlendIf.h"
#include "core/LayerNode.h"

namespace core {

// Undoable "bake this layer's Blend If SOURCE range into its mask".
//
// Only the source half is bakeable: it is a function of the layer's own pixels. The
// destination range depends on whatever is beneath the layer, which changes as the document
// animates, so it stays live and is deliberately kept (see the assessment note in
// tools/psd_composite_probe).
//
// prepare() captures everything undo needs (the previous mask pixels and flags plus the
// previous ranges) and computes the baked mask; bake()/restore() swap the two states. The
// mask texture is rebuilt lazily by the render path (ImageKey::syncMaskTexture), so this
// class touches no GL.
class LayerMaskBake {
public:
    LayerMaskBake();

    // cheap check for the UI: the layer has an image and a non-default source range
    static bool isAvailable(const ObjectNode& aNode);

    // false when the layer has no image or its source range is already default
    static bool prepare(const ObjectNode& aNode, LayerMaskBake& aOut);

    void bake(LayerNode& aLayer) const;
    void restore(LayerNode& aLayer) const;

    const BlendIf& bakedBlendIf() const { return mBakedBlendIf; }
    const QSize& maskSize() const { return mBakedMaskSize; }

private:
    BlendIf mPrevBlendIf;
    BlendIf mBakedBlendIf;
    std::vector<uint8_t> mPrevMask;
    std::vector<uint8_t> mBakedMask;
    QSize mPrevMaskSize;
    QSize mBakedMaskSize;
    bool mPrevHasMask = false;
    bool mPrevEnabled = true;
    bool mPrevInverted = false;
    bool mBakedHasMask = false;
};

} // namespace core

#endif // CORE_LAYERMASKBAKE_H
