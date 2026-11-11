#ifndef CTRL_BONE_WEIGHTHEATMAP_H
#define CTRL_BONE_WEIGHTHEATMAP_H

#include <QMatrix4x4>
#include "core/BoneKey.h"
#include "core/RenderInfo.h"

class QPainter;

namespace ctrl {
namespace bone {

    /// The normalised weight of one bone over every skinned mesh of a key, as
    /// a colour-ramped point cloud. The points sit exactly where the influence
    /// map's vertices were built: the cache's inner matrix times the node's
    /// origin offset, then the key owner's target matrix and the camera. It is
    /// a diagnostic over the artwork, so it is drawn before the envelopes and
    /// the bones - the skeleton must stay legible above it.
    ///
    /// The point cloud is one QPainter::drawPoints() per colour bucket; the
    /// overlay's GL engine batches each call into GL_POINTS, so the cost is one
    /// draw per non-empty bucket per 8192-vertex chunk - not one per vertex.
    void drawWeightHeatmap(
        QPainter& aPainter, const core::RenderInfo& aInfo, const core::BoneKey& aKey,
        const core::Bone2& aBone, const QMatrix4x4& aTargetMtx
    );

} // namespace bone
} // namespace ctrl

#endif // CTRL_BONE_WEIGHTHEATMAP_H
