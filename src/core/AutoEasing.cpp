#include "core/AutoEasing.h"

#include <algorithm>
#include <cmath>

#include "core/TimeKey.h"
#include "core/MoveKey.h"
#include "core/RotateKey.h"
#include "core/ScaleKey.h"
#include "core/DepthKey.h"
#include "core/OpaKey.h"
#include "core/HsvKey.h"
#include "core/BlurKey.h"
#include "core/PoseKey.h"
#include "core/FFDKey.h"
#include "core/Bone2.h"

namespace {

// The component an Auto handle reads: the one that moves most between the
// segment's two bounding keys. Ties - and a segment that does not move at all -
// stay on the first component considered.
struct Dominant {
    int index = 0;
    float change = 0.0f;

    void consider(int aIndex, float aFrom, float aTo) {
        const float d = std::fabs(aTo - aFrom);
        if (d > change) {
            change = d;
            index = aIndex;
        }
    }
};

// A pose key's bone rotations in the order core::TimeKeyBlender blends them
// (each top bone's descendants, depth first, the top bone itself excluded), so
// the same index names the same bone on every key of the channel.
QVector<float> flattenPose(const core::PoseKey& aKey) {
    QVector<float> out;
    for (const core::Bone2* top : aKey.data().topBones()) {
        for (core::Bone2::ConstIterator itr(top); itr.hasNext();) {
            out.append(itr.next()->rotate());
        }
    }
    return out;
}

bool valueOf(const core::TimeKey& aKey, const core::TimeKey& aFrom, const core::TimeKey& aTo, float& aOut) {
    // The caller pairs keys out of one channel's map, but a hand-built call must
    // not read a Move key through a Scale cast.
    if (aKey.type() != aFrom.type() || aKey.type() != aTo.type())
        return false;

    switch (aKey.type()) {
    case core::TimeKeyType_Move: {
        const QVector2D from = static_cast<const core::MoveKey&>(aFrom).data().pos();
        const QVector2D to = static_cast<const core::MoveKey&>(aTo).data().pos();
        const QVector2D val = static_cast<const core::MoveKey&>(aKey).data().pos();
        Dominant d;
        d.consider(0, from.x(), to.x());
        d.consider(1, from.y(), to.y());
        aOut = (d.index == 0) ? val.x() : val.y();
        return true;
    }
    case core::TimeKeyType_Scale: {
        const QVector2D from = static_cast<const core::ScaleKey&>(aFrom).data().scale();
        const QVector2D to = static_cast<const core::ScaleKey&>(aTo).data().scale();
        const QVector2D val = static_cast<const core::ScaleKey&>(aKey).data().scale();
        Dominant d;
        d.consider(0, from.x(), to.x());
        d.consider(1, from.y(), to.y());
        aOut = (d.index == 0) ? val.x() : val.y();
        return true;
    }
    case core::TimeKeyType_Blur: {
        const core::BlurKey::Data& from = static_cast<const core::BlurKey&>(aFrom).data();
        const core::BlurKey::Data& to = static_cast<const core::BlurKey&>(aTo).data();
        const core::BlurKey::Data& val = static_cast<const core::BlurKey&>(aKey).data();
        Dominant d;
        d.consider(0, from.blurX(), to.blurX());
        d.consider(1, from.blurY(), to.blurY());
        d.consider(2, from.angleDeg(), to.angleDeg());
        aOut = (d.index == 0) ? val.blurX() : ((d.index == 1) ? val.blurY() : val.angleDeg());
        return true;
    }
    case core::TimeKeyType_HSV: {
        const QList<int>& from = static_cast<const core::HSVKey&>(aFrom).data().hsv();
        const QList<int>& to = static_cast<const core::HSVKey&>(aTo).data().hsv();
        const QList<int>& val = static_cast<const core::HSVKey&>(aKey).data().hsv();
        // Playback blends the first three entries; the fourth is a flag.
        if (from.size() < 3 || to.size() < 3 || val.size() < 3)
            return false;
        Dominant d;
        for (int i = 0; i < 3; ++i) {
            d.consider(i, static_cast<float>(from.at(i)), static_cast<float>(to.at(i)));
        }
        aOut = static_cast<float>(val.at(d.index));
        return true;
    }
    case core::TimeKeyType_Rotate:
        aOut = static_cast<const core::RotateKey&>(aKey).data().rotate();
        return true;
    case core::TimeKeyType_Depth:
        aOut = static_cast<const core::DepthKey&>(aKey).data().depth();
        return true;
    case core::TimeKeyType_Opa:
        aOut = static_cast<const core::OpaKey&>(aKey).data().opacity();
        return true;
    case core::TimeKeyType_Pose: {
        const QVector<float> from = flattenPose(static_cast<const core::PoseKey&>(aFrom));
        const QVector<float> to = flattenPose(static_cast<const core::PoseKey&>(aTo));
        const QVector<float> val = flattenPose(static_cast<const core::PoseKey&>(aKey));
        if (from.isEmpty() || from.size() != to.size() || from.size() != val.size())
            return false;
        Dominant d;
        for (int i = 0; i < from.size(); ++i) {
            d.consider(i, from.at(i), to.at(i));
        }
        aOut = val.at(d.index);
        return true;
    }
    case core::TimeKeyType_FFD: {
        const core::FFDKey::Data& from = static_cast<const core::FFDKey&>(aFrom).data();
        const core::FFDKey::Data& to = static_cast<const core::FFDKey&>(aTo).data();
        const core::FFDKey::Data& val = static_cast<const core::FFDKey&>(aKey).data();
        const int count = std::min(from.count(), std::min(to.count(), val.count()));
        if (count <= 0)
            return false;
        // The vertex AND axis that moves most across the segment decides, so a
        // deformation that travels in y is not shaped by a still x.
        Dominant d;
        for (int i = 0; i < count; ++i) {
            const gl::Vector3& f = from.positions()[i];
            const gl::Vector3& t = to.positions()[i];
            d.consider(i * 3 + 0, f.x, t.x);
            d.consider(i * 3 + 1, f.y, t.y);
            d.consider(i * 3 + 2, f.z, t.z);
        }
        const gl::Vector3& v = val.positions()[d.index / 3];
        switch (d.index % 3) {
        case 0: aOut = v.x; break;
        case 1: aOut = v.y; break;
        default: aOut = v.z; break;
        }
        return true;
    }
    default:
        // Bone and mesh carry no easing at all; image's offset is step-held, so
        // it has no rate for an Auto handle to shape.
        return false;
    }
}

} // namespace

namespace core {

bool autoEasingValue(const TimeKey& aKey, const TimeKey& aFrom, const TimeKey& aTo, float& aOut) {
    return valueOf(aKey, aFrom, aTo, aOut);
}

util::Easing::AutoContext autoEasingContext(
    const TimeKey& aFrom, const TimeKey& aTo, const TimeKey* aPrev, const TimeKey* aNext) {
    util::Easing::AutoContext ctx;

    float from = 0.0f, to = 0.0f;
    if (!valueOf(aFrom, aFrom, aTo, from) || !valueOf(aTo, aFrom, aTo, to))
        return ctx;

    // The segment's own change is the axis the neighbours are expressed in. A
    // channel that does not change across the segment has no shape to derive.
    const float change = to - from;
    if (change == 0.0f)
        return ctx;

    const float fromFrame = static_cast<float>(aFrom.frame());
    auto fill = [&](const TimeKey* aKey, util::Easing::AutoKey& aOut) {
        float value = 0.0f;
        if (!aKey || !valueOf(*aKey, aFrom, aTo, value))
            return;
        aOut.valid = true;
        aOut.frame = static_cast<float>(aKey->frame()) - fromFrame;
        aOut.value = (value - from) / change;
    };
    fill(aPrev, ctx.prev);
    fill(aNext, ctx.next);

    return ctx;
}

} // namespace core
