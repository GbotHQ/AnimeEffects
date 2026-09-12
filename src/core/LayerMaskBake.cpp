#include <algorithm>
#include "img/ResourceData.h"
#include "core/ImageKey.h"
#include "core/LayerMaskBake.h"
#include "core/TimeLine.h"

namespace core {

namespace {

const img::ResourceData* resourceOf(const ObjectNode& aNode) {
    if (const TimeLine* timeLine = aNode.timeLine()) {
        if (const TimeKey* key = timeLine->defaultKey(TimeKeyType_Image)) {
            return static_cast<const ImageKey*>(key)->data().resource().get();
        }
    }
    return nullptr;
}

img::ResourceData* resourceOf(ObjectNode& aNode) {
    return const_cast<img::ResourceData*>(resourceOf(const_cast<const ObjectNode&>(aNode)));
}

// the mask is stored in the layer image's rect, so the source weight is evaluated per pixel
// of the image (straight RGBA8; the padded sampling border is part of that rect)
void buildBakedMask(const img::ResourceData& aRes, const BlendIf& aBlendIf, std::vector<uint8_t>& aOut,
    QSize& aOutSize) {
    const QSize size = aRes.image().pixelSize();
    const uint8_t* src = aRes.image().data();
    const size_t count = (size_t)size.width() * size.height();
    const uint8_t* prev = aRes.hasMask() ? aRes.mask().data() : nullptr;

    aOut.assign(count, 255);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* px = src + i * 4;
        const float weight = blendIfSourceWeight(aBlendIf, px[0], px[1], px[2]);
        int value = (int)(weight * 255.0f + 0.5f);
        if (prev) {
            // an existing mask and the baked range both scale the coverage, so they compose
            value = (value * prev[i] + 127) / 255;
        }
        aOut[i] = (uint8_t)std::min(255, std::max(0, value));
    }
    aOutSize = size;
}

} // namespace

LayerMaskBake::LayerMaskBake() = default;

bool LayerMaskBake::isAvailable(const ObjectNode& aNode) {
    const img::ResourceData* res = resourceOf(aNode);
    if (!res || !res->hasImage()) {
        return false;
    }
    const BlendIf blendIf = aNode.renderer() ? aNode.renderer()->blendIf() : BlendIf();
    return !blendIf.src.isDefault();
}

bool LayerMaskBake::prepare(const ObjectNode& aNode, LayerMaskBake& aOut) {
    const img::ResourceData* res = resourceOf(aNode);
    if (!res || !res->hasImage()) {
        return false;
    }

    const BlendIf blendIf = aNode.renderer() ? aNode.renderer()->blendIf() : BlendIf();
    if (blendIf.src.isDefault()) {
        return false; // nothing to bake
    }

    aOut.mPrevBlendIf = blendIf;
    aOut.mBakedBlendIf = blendIf;
    aOut.mBakedBlendIf.src = BlendIf::Range(); // the range becomes pixels

    aOut.mPrevHasMask = res->hasMask();
    aOut.mPrevEnabled = res->isMaskEnabled();
    aOut.mPrevInverted = res->isMaskInverted();
    if (res->hasMask()) {
        aOut.mPrevMaskSize = res->mask().pixelSize();
        aOut.mPrevMask.assign(res->mask().data(), res->mask().data() + res->mask().size());
    } else {
        aOut.mPrevMaskSize = QSize();
        aOut.mPrevMask.clear();
    }

    buildBakedMask(*res, blendIf, aOut.mBakedMask, aOut.mBakedMaskSize);
    // the baked mask is a coverage plane, so it is enabled and not inverted
    aOut.mBakedHasMask = true;
    return !aOut.mBakedMask.empty();
}

void LayerMaskBake::bake(LayerNode& aLayer) const {
    auto* res = resourceOf(aLayer);
    if (!res) {
        return;
    }

    uint8_t* data = new uint8_t[mBakedMask.size()];
    std::copy(mBakedMask.begin(), mBakedMask.end(), data);
    res->grabMask(XCMemBlock(data, mBakedMask.size()), mBakedMaskSize);
    res->setMaskEnabled(true);
    res->setMaskInverted(false);

    if (aLayer.renderer()) {
        aLayer.renderer()->setBlendIf(mBakedBlendIf);
    }
}

void LayerMaskBake::restore(LayerNode& aLayer) const {
    auto* res = resourceOf(aLayer);
    if (!res) {
        return;
    }

    if (mPrevHasMask) {
        uint8_t* data = new uint8_t[mPrevMask.size()];
        std::copy(mPrevMask.begin(), mPrevMask.end(), data);
        res->grabMask(XCMemBlock(data, mPrevMask.size()), mPrevMaskSize);
    } else {
        res->freeMask();
    }
    res->setMaskEnabled(mPrevEnabled);
    res->setMaskInverted(mPrevInverted);

    if (aLayer.renderer()) {
        aLayer.renderer()->setBlendIf(mPrevBlendIf);
    }
}

} // namespace core
