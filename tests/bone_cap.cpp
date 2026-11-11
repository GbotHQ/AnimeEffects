// The skeleton bone cap: one number shared by the pose palette, the influence
// build and the skinning shader.
//
// `core::PosePalette::kMaxCount` sizes the palette that poses bones and binds
// rigid layers, `BoneKey::Cache` passes the same constant to the influence
// map's bone list, and `MeshTransformerResource` sets the shader's
// `MAX_BONES` variation from it. Before this gate the three limits were 32,
// and a 33-bone skeleton (skel_practice_12.anie in the dataset) silently lost
// its last bone: no influence weight, no posed transform, no rigid binding.
//
// What is pinned here:
//   * a 33-bone chain, the dataset's boundary: the influence build keeps all
//     33 bones and a vertex inside the 33rd bone's envelope stores index 32
//     (the old 32-bone cap stored the default bone 0 weight 1 instead);
//   * the palette builds 33 entries and the 33rd pose transform reaches
//     matrices()[32] and dualQuaternions()[32];
//   * the cap still exists: a 65-bone chain stops at kMaxCount and a vertex
//     only the 65th bone covers gets the default influence.
//
// The shader half (a bone index of 32 selecting uBoneDualQuat[64..65]) cannot
// be compiled without a GL context; the real project's frames are the GL gate
// (see the influence plan's §7), and MeshTransformerResource ties the uniform
// size to the same constant this file pins.
//
// No project, no GL, no window. `Project.cpp` references the GUI's audio
// sidecar, so the stub below is the same one bone_influence_storage uses.
#include <QApplication>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QPolygonF>
#include <QVector2D>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "XCAssert.h"
#include "core/Bone2.h"
#include "core/BoneKey.h"
#include "core/BoneShape.h"
#include "core/PoseKey.h"
#include "core/PosePalette.h"
#include "gui/AudioPlaybackWidget.h"
#include "util/Segment2D.h"

// Project.cpp references this GUI entry point; the test never opens a .aemus.
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

struct core::BoneInfluenceMapTestProbe {
    /// Sets up the work buffer the way writeAsync does, then runs the real
    /// makeBoneList -> writeWeights -> writeVertexAttribute path. Returns the
    /// number of bone parameters the cap allowed into the map.
    static int buildFromBones(
        core::BoneInfluenceMap& aMap, const QList<core::Bone2*>& aTopBones,
        const std::vector<QVector2D>& aVertices
    ) {
        aMap.mVertexCount = int(aVertices.size());
        aMap.freeStorage();
        aMap.mWorks.reset(new core::BoneInfluenceMap::WorkAttribute[aMap.mVertexCount]);
        for (size_t i = 0; i < aVertices.size(); ++i) {
            aMap.mWorks[i].clear();
            aMap.mWorks[i].vertex = aVertices[i];
        }

        aMap.makeBoneList(aTopBones);
        aMap.writeWeights();
        aMap.writeVertexAttribute();

        const int params = aMap.mBoneList.params.size();
        aMap.mWorks.reset();
        return params;
    }
};

namespace {

class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;
int failures = 0;

void check(bool aCondition, const char* aWhat) {
    if (aCondition) {
        std::printf("PASS  %s\n", aWhat);
    } else {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}

core::Bone2* boneAtIndex(const QList<core::Bone2*>& aTopBones, int aIndex) {
    int index = 0;
    for (core::Bone2* top : aTopBones) {
        for (core::Bone2::Iterator itr(top); itr.hasNext();) {
            core::Bone2* bone = itr.next();
            if (index++ == aIndex)
                return bone;
        }
    }
    return nullptr;
}

/// A chain of `aCount` bones, one unit apart along x, owned by a real BoneKey so
/// the palette sees a real BoneKey::Data. Bone i owns a unit envelope at its
/// root, so a vertex at (i, 0) is influenced by bone i only; Bone2's destructor
/// deletes the children through the root.
struct Chain {
    core::BoneKey key;

    explicit Chain(int aCount) {
        XC_ASSERT(aCount >= 1);
        QList<core::Bone2*>& tops = key.data().topBones();

        auto* root = new core::Bone2();
        tops.push_back(root);
        root->updateWorldTransform();

        core::Bone2* parent = root;
        for (int i = 1; i < aCount; ++i) {
            auto* bone = new core::Bone2();
            parent->children().pushBack(bone);
            bone->setWorldPos(QVector2D(float(i), 0.0f), parent);
            bone->updateWorldTransform();
            parent = bone;

            // Geometry is in world space; the influence build only reads shapes.
            core::BoneShape shape;
            shape.setSegment(util::Segment2D(QVector2D(float(i), 0.0f), QVector2D(0.0f, 1.0f)));
            shape.setRadius(QVector2D(0.25f, 0.25f), QVector2D(0.25f, 0.25f));
            QPolygonF polygon;
            polygon << QPointF(i - 0.5, -0.5) << QPointF(i + 0.5, -0.5) << QPointF(i + 0.5, 0.5)
                    << QPointF(i - 0.5, 0.5);
            shape.setPolygon(polygon);
            bone->setShape(shape);
            bone->setRange(0, QVector2D(0.25f, 0.25f));
            bone->setRange(1, QVector2D(0.25f, 0.25f));
        }
    }
};

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using core::BoneInfluenceMap;
    using Probe = core::BoneInfluenceMapTestProbe;

    // The uniform budget: 64 dual quaternions are 128 vec4 = 512 of the
    // guaranteed 1024 vertex uniform components, and the index attribute is
    // uint8, so 255 is the hard ceiling. The shader's default MAX_BONES
    // literal is the same number; MeshTransformerResource overrides it from
    // this constant, but a change here must also update that comment and the
    // plan's uniform arithmetic.
    static_assert(core::PosePalette::kMaxCount == 64, "the skinning uniform budget assumes 64 bones");
    static_assert(core::PosePalette::kMaxCount <= 255, "bone indices are stored as uint8");

    // THE DATASET'S BOUNDARY: 33 bones. skel_practice_12.anie's "wings 1" key
    // holds 33; with the old cap its last bone was dropped from the influence
    // build and from the palette, silently.
    {
        Chain chain(33);
        const QList<core::Bone2*>& tops = chain.key.data().topBones();

        // Influence side, through the production holder: BoneKey::Cache sets
        // the map's cap.
        core::BoneKey::Cache cache;
        const std::vector<QVector2D> vertices = {QVector2D(32.0f, 0.0f)};
        const int params = Probe::buildFromBones(cache.influence(), tops, vertices);
        check(params == 33, "the influence build keeps all 33 bones");
        const BoneInfluenceMap::Accessor accessor = cache.influence().accessor();
        check(accessor.slotCount() == 1, "the 33rd-bone vertex has one influence");
        check(
            accessor.indices(0) && accessor.indices(0)[0] == 32,
            "a vertex inside the 33rd bone stores bone index 32"
        );

        // CONTROL: the same chain under the old 32-bone limit loses that bone.
        // This is what the gate would read if the cap regressed, and it is the
        // exact defect the dataset project showed (index 0, weight 1 instead).
        {
            BoneInfluenceMap oldCap;
            oldCap.setMaxBoneCount(32);
            const int oldParams = Probe::buildFromBones(oldCap, tops, vertices);
            const BoneInfluenceMap::Accessor oldAccessor = oldCap.accessor();
            check(oldParams == 32, "the 32-bone control stops at 32 parameters");
            check(
                oldAccessor.indices(0) && oldAccessor.indices(0)[0] == 0,
                "the 32-bone control drops the 33rd bone to the default influence"
            );
        }

        // Palette side: the 33rd pose bone's transform is built and uploaded.
        core::PoseKey::Data pose;
        pose.createBonesBy(chain.key);
        core::Bone2* posed = boneAtIndex(pose.topBones(), 32);
        check(posed != nullptr, "the pose has a 33rd bone");
        if (posed) {
            posed->setRotate(0.5f);
            for (core::Bone2* top : pose.topBones())
                top->updateWorldTransform();
        }

        core::Bone2* keyBone32 = boneAtIndex(tops, 32);
        check(keyBone32 != nullptr, "the key has a 33rd bone");
        if (keyBone32) {
            check(
                core::PosePalette::getBoneIndex(chain.key.data(), *keyBone32) == 32,
                "the 33rd bone's palette index is 32"
            );
        }

        core::PosePalette palette;
        core::PosePalette::KeyPairs pairs;
        pairs.push_back(core::PosePalette::KeyPair{&chain.key.data(), &pose});
        palette.build(pairs);

        check(
            palette.matrices().count() == core::PosePalette::kMaxCount,
            "the palette uploads kMaxCount matrices"
        );
        check(
            palette.dualQuaternions().count() == core::PosePalette::kMaxCount,
            "the palette uploads kMaxCount dual quaternions"
        );
        if (posed && keyBone32) {
            const QMatrix4x4 mtx = palette.matrices()[32];
            check(
                std::abs(mtx(0, 0) - std::cos(0.5f)) < 1e-4f && std::abs(mtx(1, 0) - std::sin(0.5f)) < 1e-4f,
                "the 33rd pose rotation reaches matrices()[32]"
            );
            const core::PosePalette::DualQuaternion dq = palette.dualQuaternions()[32];
            check(
                std::abs(dq.real.x - std::cos(0.25f)) < 1e-4f,
                "the 33rd pose rotation reaches dualQuaternions()[32]"
            );
        } else {
            check(false, "the 33rd pose bone exists (palette check skipped)");
        }
    }

    // THE CAP STILL EXISTS: 65 bones stop at kMaxCount. This is the warning
    // boundary, not an assumption that every rig fits.
    {
        Chain chain(core::PosePalette::kMaxCount + 1);
        core::BoneKey::Cache cache;
        const std::vector<QVector2D> vertices = {QVector2D(float(core::PosePalette::kMaxCount), 0.0f)};
        const int params = Probe::buildFromBones(cache.influence(), chain.key.data().topBones(), vertices);
        check(
            params == core::PosePalette::kMaxCount,
            "a skeleton beyond the cap stops at kMaxCount bone parameters"
        );
        const BoneInfluenceMap::Accessor accessor = cache.influence().accessor();
        check(
            accessor.indices(0) && accessor.indices(0)[0] == 0,
            "a vertex only the dropped bone covers gets the default influence"
        );
    }

    if (failures == 0) {
        std::printf("PASS: bone cap\n");
    } else {
        std::printf("FAILED %d check(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
