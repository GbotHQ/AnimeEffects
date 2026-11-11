#include <algorithm>
#include <float.h>
#include <QtMath>
#include "core/Project.h"
#include "core/LayerMesh.h"
#include "core/BoneInfluenceMap.h"

namespace core {

namespace {

// The weight a vertex with no (or one) influence carries.
constexpr GLfloat kWeightOne = 1.0f;
// Below this, the normalisation adds the deficit instead of dividing (unchanged
// from the dense writer: the arithmetic order is part of the stored result).
constexpr GLfloat kMinPowerSum = 0.01f;

} // namespace

//-------------------------------------------------------------------------------------------------
BoneInfluenceMap::BoneInfluenceMap():
    mVertexCount(0), mSlotCount(0), mMaxBoneCount(0), mGroupMtx(), mBoneList(), mSparseIndices(),
    mSparseWeights(), mIndices(), mWeights(), mWorks(), mBuildTask() {}

BoneInfluenceMap::~BoneInfluenceMap() {
    // A queued or running task points at this map (and at mWorks/mIndices).
    // Task::~Task() waits only for a RUNNING task, so an idle one still in the
    // paralleler's queue would outlive the map; cancel removes it (and joins a
    // running one).
    if (mBuildTask) {
        mBuildTask->cancel();
    }
}

void BoneInfluenceMap::setMaxBoneCount(int aBoneCount) {
    XC_ASSERT(mBuildTask.isNull());
    // Indices are stored as uint8 for the GPU. The palette caps at 32 today;
    // this assert is the contract that a future cap cannot silently truncate.
    XC_ASSERT(aBoneCount <= 255);
    mMaxBoneCount = aBoneCount;
}

void BoneInfluenceMap::allocate(int aVertexCount, bool aInitialize) {
    // cancel previous task
    if (mBuildTask) {
        mBuildTask->cancel();
    }

    // reallocate
    if (mVertexCount != aVertexCount) {
        mWorks.reset();
        freeStorage();
        mVertexCount = aVertexCount;
    }

    // Leave the map usable before the build joins: one slot, bone 0, weight 1
    // (implicit). The build then stores exactly the slots the mesh needs.
    if (aInitialize || mSlotCount == 0) {
        ensureSparseStorage(1);
        if (mSparseIndices) {
            for (int i = 0; i < mVertexCount; ++i) {
                mSparseIndices[i] = 0;
            }
        }
    }
}

void BoneInfluenceMap::freeStorage() {
    mSparseIndices.reset();
    mSparseWeights.reset();
    for (int t = 0; t < 2; ++t) {
        mIndices[t].reset();
        mWeights[t].reset();
    }
    mSlotCount = 0;
}

void BoneInfluenceMap::ensureSparseStorage(int aSlotCount) {
    XC_ASSERT(aSlotCount >= 1 && aSlotCount < kBonePerVtxMaxAll);

    // Only one representation lives at a time.
    for (int t = 0; t < 2; ++t) {
        mIndices[t].reset();
        mWeights[t].reset();
    }

    if (mSlotCount == aSlotCount && mSparseIndices) {
        return;
    }

    mSparseIndices.reset();
    mSparseWeights.reset();

    if (mVertexCount > 0) {
        const int count = aSlotCount * mVertexCount;
        mSparseIndices.reset(new IndicesType[count]);
        XC_PTR_ASSERT(mSparseIndices);
        if (!mSparseIndices) {
            mSlotCount = 0;
            return;
        }

        // A one-slot map needs no weights: every vertex weight is exactly 1.
        if (aSlotCount >= 2) {
            mSparseWeights.reset(new WeightsType[count]);
            XC_PTR_ASSERT(mSparseWeights);
            if (!mSparseWeights) {
                mSparseIndices.reset();
                mSlotCount = 0;
                return;
            }
        }
    }

    mSlotCount = aSlotCount;
}

void BoneInfluenceMap::ensureDenseStorage() {
    mSparseIndices.reset();
    mSparseWeights.reset();

    if (mSlotCount == 8 && mIndices[0]) {
        return;
    }

    for (int t = 0; t < 2; ++t) {
        mIndices[t].reset();
        mWeights[t].reset();
    }

    if (mVertexCount > 0) {
        const int count = kBonePerVtxMaxEach * mVertexCount;
        for (int t = 0; t < 2; ++t) {
            mIndices[t].reset(new IndicesType[count]);
            XC_PTR_ASSERT(mIndices[t]);
            if (!mIndices[t]) {
                mSlotCount = 0;
                return;
            }

            mWeights[t].reset(new WeightsType[count]);
            XC_PTR_ASSERT(mWeights[t]);
            if (!mWeights[t]) {
                mSlotCount = 0;
                return;
            }
        }
    }

    mSlotCount = 8;
}

void BoneInfluenceMap::writeAsync(
    Project& aProject, const QList<Bone2*>& aTopBones, const QMatrix4x4& aGroupMtx, const LayerMesh& aMesh
) {
    XC_ASSERT(mMaxBoneCount > 0);

    // cancel previous task
    if (mBuildTask) {
        mBuildTask->cancel();
    }

    // set group matrix
    mGroupMtx = aGroupMtx;

    // allocate work buffer
    mWorks.reset(new WorkAttribute[mVertexCount]);
    XC_PTR_ASSERT(mWorks.data());
    if (!mWorks)
        return;

    // initialize work buffer
    const gl::Vector3* positions = aMesh.positions();
    for (int i = 0; i < mVertexCount; ++i) {
        mWorks[i].clear();
        mWorks[i].vertex = positions[i].pos2D();
    }

    // make bone list
    makeBoneList(aTopBones);

#ifdef UNUSE_PARALLEL
    build();
    (void)aProject;
#else
    // create task
    mBuildTask.reset(new BuildTask(aProject, *this));
    aProject.paralleler().push(*mBuildTask);
#endif
}

BoneInfluenceMap::Accessor BoneInfluenceMap::accessor() const {
    waitBuilding();
    return Accessor(*this);
}

void BoneInfluenceMap::makeBoneList(const QList<Bone2*>& aTopBones) {
    mBoneList.params.clear();

    // per skeleton
    for (const Bone2* topBone : aTopBones) {
        XC_PTR_ASSERT(topBone);

        // per bone
        for (Bone2::ConstIterator itr = topBone; itr.hasNext();) {
            const Bone2* bone = itr.next();
            XC_PTR_ASSERT(bone);

            if (bone && mBoneList.params.size() < mMaxBoneCount) {
                BoneParam param;
                param.hasParent = (bool)(bone->parent());

                if (param.hasParent) {
                    param.hasRange = bone->hasValidRange();
                    param.shape = bone->shape();
                }
                mBoneList.params.push_back(param);
            }
        }
    }
}

void BoneInfluenceMap::build() {
    // world matrix * vertices
    transformVertices();
    if (isBuildCanceled())
        return;

    // write bone weight
    writeWeights();
    if (isBuildCanceled())
        return;

    // write vertex attribute
    writeVertexAttribute();
    if (isBuildCanceled())
        return;

    // free work buffer
    mWorks.reset();
}

void BoneInfluenceMap::transformVertices() {
    for (int i = 0; i < mVertexCount; ++i) {
        mWorks[i].vertex = (mGroupMtx.map(QVector3D(mWorks[i].vertex))).toVector2D();
    }
}

void BoneInfluenceMap::writeWeights() {
    // Bone order is the order in which weights reach a vertex, and
    // tryPushBoneWeight() keeps the first slot on a tie, so this loop order is
    // part of the stored result.
    const int boneCount = mBoneList.params.size();
    for (int i = 0; i < boneCount; ++i) {
        const BoneParam& param = mBoneList.params[i];
        if (!param.hasParent || !param.hasRange)
            continue;

        // calculate weights
        for (int k = 0; k < mVertexCount; ++k) {
            const float weight = param.shape.influence(mWorks[k].vertex);

            if (weight >= FLT_EPSILON) {
                mWorks[k].tryPushBoneWeight(i, weight);
            }
        }

        // check canceling
        if (isBuildCanceled())
            return;
    }
}

void BoneInfluenceMap::writeVertexAttribute() {
    // Store only the slots this map actually uses. A map whose busiest vertex
    // needs one influence carries one index byte per vertex and no weights;
    // the dense eight-slot form is kept for the top-slot case (the scalar
    // attributes would exceed the guaranteed 16 vertex attributes).
    int slotCount = 1;
    for (int i = 0; i < mVertexCount; ++i) {
        if (mWorks[i].count > slotCount) {
            slotCount = mWorks[i].count;
        }
    }
    XC_ASSERT(slotCount <= kBonePerVtxMaxAll);

    if (slotCount >= kBonePerVtxMaxAll) {
        writeDenseAttributes();
    } else {
        writeSparseAttributes(slotCount);
    }
}

void BoneInfluenceMap::writeSparseAttributes(int aSlotCount) {
    XC_ASSERT(aSlotCount >= 1 && aSlotCount < kBonePerVtxMaxAll);
    ensureSparseStorage(aSlotCount);
    if (mSlotCount != aSlotCount || !mSparseIndices)
        return;
    if (aSlotCount >= 2) {
        XC_PTR_ASSERT(mSparseWeights);
        if (!mSparseWeights)
            return;
    }

    const int vertices = mVertexCount;
    const int count = aSlotCount * vertices;
    std::fill(mSparseIndices.data(), mSparseIndices.data() + count, IndicesType(0));
    if (mSparseWeights) {
        std::fill(mSparseWeights.data(), mSparseWeights.data() + count, WeightsType(0));
    }

    // each vertex
    for (int i = 0; i < vertices; ++i) {
        const WorkAttribute& work = mWorks[i];

        if (work.count <= 1) {
            // no or single bone influence; slot 0 carries the whole weight
            if (work.count == 1) {
                XC_ASSERT(work.id[0] >= 0 && work.id[0] < mBoneList.params.size() && work.id[0] <= 0xFF);
                mSparseIndices[i] = static_cast<IndicesType>(work.id[0]);
            }
            if (aSlotCount >= 2) {
                mSparseWeights[i] = kWeightOne;
            }
            continue;
        }

        // some bones influence - the normalisation is the dense writer's,
        // term by term, so every stored weight is the same float
        float powerSum = 0.0f;
        for (int k = 0; k < work.count; ++k) {
            powerSum += work.weight[k];
        }

        float weightRate = 1.0f;
        float weightAdd = 0.0f;

        if (powerSum >= kMinPowerSum) {
            weightRate /= powerSum;
        } else {
            weightAdd = (1.0f - powerSum) / work.count;
        }

        float weightSum = 0.0f;
        for (int k = 0; k < work.count; ++k) {
            XC_ASSERT(work.id[k] >= 0 && work.id[k] < mBoneList.params.size() && work.id[k] <= 0xFF);
            mSparseIndices[k * vertices + i] = static_cast<IndicesType>(work.id[k]);
            const float weight = work.weight[k] * weightRate + weightAdd;
            mSparseWeights[k * vertices + i] = weight;
            weightSum += weight;
        }
        // Build-time invariant: the used slots still partition unity.
        XC_ASSERT(qAbs(weightSum - kWeightOne) <= 1e-5f);
    }
}

void BoneInfluenceMap::writeDenseAttributes() {
    ensureDenseStorage();
    if (mSlotCount != kBonePerVtxMaxAll)
        return;

    // clear
    for (int t = 0; t < 2; ++t) {
        IndicesType* indices = mIndices[t].data();
        WeightsType* weights = mWeights[t].data();
        const int count = mVertexCount * kBonePerVtxMaxEach;

        for (int i = 0; i < count; ++i) {
            indices[i] = 0;
        }
        for (int i = 0; i < count; ++i) {
            weights[i] = 0.0f;
        }
    }

    // each vertex
    for (int i = 0; i < mVertexCount; ++i) {
        const int ixc = i * kBonePerVtxMaxEach;
        const WorkAttribute& work = mWorks[i];

        if (work.count == 0) {
            // no bone influence
            mIndices[0][ixc] = 0;
            mWeights[0][ixc] = kWeightOne;
        } else if (work.count == 1) {
            // single bone influence
            XC_ASSERT(work.id[0] >= 0 && work.id[0] < mBoneList.params.size() && work.id[0] <= 0xFF);
            mIndices[0][ixc] = static_cast<IndicesType>(work.id[0]);
            mWeights[0][ixc] = kWeightOne;
        } else {
            // some bones influence

            float powerSum = 0.0f;
            for (int k = 0; k < work.count; ++k) {
                powerSum += work.weight[k];
            }

            float weightRate = 1.0f;
            float weightAdd = 0.0f;

            if (powerSum >= kMinPowerSum) {
                weightRate /= powerSum;
            } else {
                weightAdd = (1.0f - powerSum) / work.count;
            }

            float weightSum = 0.0f;
            for (int k = 0; k < work.count; ++k) {
                const int t = k / kBonePerVtxMaxEach;
                const int each = ixc + k - t * kBonePerVtxMaxEach;

                XC_ASSERT(work.id[k] >= 0 && work.id[k] < mBoneList.params.size() && work.id[k] <= 0xFF);
                mIndices[t][each] = static_cast<IndicesType>(work.id[k]);
                const float weight = work.weight[k] * weightRate + weightAdd;
                mWeights[t][each] = weight;
                weightSum += weight;
            }
            // Build-time invariant: the used slots still partition unity.
            XC_ASSERT(qAbs(weightSum - kWeightOne) <= 1e-5f);
        }
    }
}

bool BoneInfluenceMap::isBuildCanceled() const {
    if (mBuildTask) {
        return mBuildTask->isCanceling();
    }
    return false;
}

void BoneInfluenceMap::waitBuilding() const {
#ifndef UNUSE_PARALLEL
    // wait current task
    if (mBuildTask) {
        mBuildTask->wait();
    }
#endif
}

//-------------------------------------------------------------------------------------------------
BoneInfluenceMap::BoneParam::BoneParam(): hasParent(false), hasRange(false), shape() {}

//-------------------------------------------------------------------------------------------------
void BoneInfluenceMap::WorkAttribute::clear() { count = 0; }

void BoneInfluenceMap::WorkAttribute::tryPushBoneWeight(int aId, float aWeight) {
    if (count < kBonePerVtxMaxAll) {
        id[count] = aId;
        weight[count] = aWeight;
        ++count;
    } else {
        int minId = -1;
        float minWeight = aWeight;
        for (int i = 0; i < kBonePerVtxMaxAll; ++i) {
            if (weight[i] < minWeight) {
                minId = i;
                minWeight = weight[i];
            }
        }

        if (minId != -1) {
            id[minId] = aId;
            weight[minId] = aWeight;
        }
    }
}

//-------------------------------------------------------------------------------------------------
BoneInfluenceMap::BuildTask::BuildTask(Project& aProject, BoneInfluenceMap& aOwner):
    mProject(aProject), mOwner(aOwner) {}

void BoneInfluenceMap::BuildTask::run() { mOwner.build(); }

void BoneInfluenceMap::BuildTask::cancel() { mProject.paralleler().cancel(*this); }

//-------------------------------------------------------------------------------------------------
BoneInfluenceMap::Accessor::Accessor(): mOwner() {}

BoneInfluenceMap::Accessor::Accessor(const BoneInfluenceMap& aOwner): mOwner(&aOwner) {}

int BoneInfluenceMap::Accessor::slotCount() const {
    XC_PTR_ASSERT(mOwner);
    return mOwner->mSlotCount;
}

bool BoneInfluenceMap::Accessor::dense() const { return slotCount() >= kBonePerVtxMaxAll; }

const BoneInfluenceMap::IndicesType* BoneInfluenceMap::Accessor::indices(int aSlot) const {
    XC_PTR_ASSERT(mOwner);
    XC_ASSERT(!dense());
    XC_ASSERT(aSlot >= 0 && aSlot < slotCount());
    if (!mOwner->mSparseIndices || mOwner->mVertexCount <= 0)
        return nullptr;
    return mOwner->mSparseIndices.data() + qsizetype(aSlot) * mOwner->mVertexCount;
}

const BoneInfluenceMap::WeightsType* BoneInfluenceMap::Accessor::weights(int aSlot) const {
    XC_PTR_ASSERT(mOwner);
    XC_ASSERT(!dense());
    XC_ASSERT(aSlot >= 0 && aSlot < slotCount());
    if (slotCount() < 2 || !mOwner->mSparseWeights || mOwner->mVertexCount <= 0)
        return nullptr;
    return mOwner->mSparseWeights.data() + qsizetype(aSlot) * mOwner->mVertexCount;
}

const BoneInfluenceMap::IndicesType* BoneInfluenceMap::Accessor::denseIndices(int aGroup) const {
    XC_PTR_ASSERT(mOwner);
    XC_ASSERT(dense());
    XC_ASSERT(aGroup >= 0 && aGroup < 2);
    return mOwner->mIndices[aGroup].data();
}

const BoneInfluenceMap::WeightsType* BoneInfluenceMap::Accessor::denseWeights(int aGroup) const {
    XC_PTR_ASSERT(mOwner);
    XC_ASSERT(dense());
    XC_ASSERT(aGroup >= 0 && aGroup < 2);
    return mOwner->mWeights[aGroup].data();
}

qint64 BoneInfluenceMap::Accessor::storedBytes() const {
    XC_PTR_ASSERT(mOwner);
    return mOwner->storedBytes();
}

} // namespace core
