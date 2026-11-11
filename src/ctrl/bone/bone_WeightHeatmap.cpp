#include <algorithm>
#include <array>
#include <vector>
#include <QPainter>
#include <QPen>
#include "core/LayerMesh.h"
#include "core/TimeKeyBlender.h"
#include "ctrl/bone/bone_WeightHeatmap.h"

namespace ctrl {
namespace bone {

namespace {

// The ramp is a heatmap, not a hue wheel: cool = weak, hot = strong. The stops
// are deliberately far apart in hue so a mid weight cannot be confused with a
// neighbouring one at this alpha.
struct RampStop {
    float t;
    int r;
    int g;
    int b;
};

constexpr std::array<RampStop, 5> kRamp = {{
    {0.00f, 40, 70, 255},
    {0.25f, 0, 220, 255},
    {0.50f, 0, 230, 80},
    {0.75f, 255, 220, 0},
    {1.00f, 255, 40, 40},
}};

// Quantising into 16 buckets is what lets the painter batch the cloud: one
// draw call per bucket (per 8192-vertex chunk), instead of one per vertex.
constexpr int kBuckets = 16;
// More samples than this adds nothing a screen can show; a dense mesh is
// sampled on a stride instead.
constexpr int kMaxPoints = 120000;
constexpr float kPointSize = 6.0f;
// Below this a vertex is not a sample: the field is transparent rather than a
// wash of blue over the whole layer.
constexpr float kMinWeight = 0.002f;

QColor rampColor(float aWeight) {
    const float t = std::min(std::max(aWeight, 0.0f), 1.0f);
    for (size_t i = 1; i < kRamp.size(); ++i) {
        if (t <= kRamp[i].t) {
            const RampStop& a = kRamp[i - 1];
            const RampStop& b = kRamp[i];
            const float local = (t - a.t) / (b.t - a.t);
            return QColor(
                int(a.r + (b.r - a.r) * local + 0.5f), int(a.g + (b.g - a.g) * local + 0.5f),
                int(a.b + (b.b - a.b) * local + 0.5f), 150
            );
        }
    }
    return QColor(kRamp.back().r, kRamp.back().g, kRamp.back().b, 150);
}

} // namespace

void drawWeightHeatmap(
    QPainter& aPainter, const core::RenderInfo& aInfo, const core::BoneKey& aKey,
    const core::Bone2& aBone, const QMatrix4x4& aTargetMtx
) {
    const int boneIndex = aKey.data().boneIndex(aBone);
    if (boneIndex < 0)
        return;

    std::array<std::vector<QPointF>, kBuckets> buckets;
    int sampled = 0;

    for (core::BoneKey::Cache* cache : aKey.caches()) {
        if (!cache || !cache->node())
            continue;
        core::ObjectNode& node = *cache->node();
        const core::LayerMesh* mesh = core::TimeKeyBlender::getAreaMesh(node, aInfo.time);
        if (!mesh)
            continue;

        const core::BoneInfluenceMap& map = cache->influence();
        const int vertices = std::min(mesh->vertexCount(), map.vertexCount());
        if (vertices <= 0)
            continue;
        // boneIndex is the map's own pre-order index; a key past the map's cap
        // is not in it and cannot be shown.
        if (boneIndex >= map.boneCount())
            continue;

        const core::BoneInfluenceMap::Accessor accessor = map.accessor();
        if (accessor.slotCount() <= 0)
            continue;

        // EXACTLY the build's matrix: BoneKey::updateCaches transforms the
        // mesh positions by innerMatrix translated by the node's origin offset.
        QMatrix4x4 mapMtx = cache->innerMatrix();
        const QVector2D originOffset = core::TimeKeyBlender::getOriginOffset(node, aInfo.time);
        mapMtx.translate(originOffset.x(), originOffset.y());
        const QMatrix4x4 worldMtx = aTargetMtx * mapMtx;

        const gl::Vector3* positions = mesh->positions();
        const int stride = std::max(1, vertices / kMaxPoints);
        for (int i = 0; i < vertices; i += stride) {
            const float weight = accessor.weight(boneIndex, i);
            if (weight < kMinWeight)
                continue;

            const QVector2D pos = positions[i].pos2D();
            const QVector3D screen =
                aInfo.camera.toScreenPos(worldMtx.map(QVector3D(pos.x(), pos.y(), 0.0f)));
            const int bucket = std::min(
                kBuckets - 1, std::max(0, int(weight * float(kBuckets - 1) + 0.5f))
            );
            buckets[bucket].push_back(screen.toPointF());
            ++sampled;
            if (sampled >= kMaxPoints)
                break;
        }
        if (sampled >= kMaxPoints)
            break;
    }

    aPainter.setBrush(Qt::NoBrush);
    for (int i = 0; i < kBuckets; ++i) {
        if (buckets[i].empty())
            continue;
        const float t = float(i) / float(kBuckets - 1);
        aPainter.setPen(QPen(rampColor(t), kPointSize, Qt::SolidLine, Qt::RoundCap));
        aPainter.drawPoints(buckets[i].data(), int(buckets[i].size()));
    }
}

} // namespace bone
} // namespace ctrl
