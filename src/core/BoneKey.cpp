#include <cstddef>
#include <limits>
#include "util/TreeUtil.h"
#include "util/MathUtil.h"
#include "core/BoneKey.h"
#include "core/PosePalette.h"
#include "core/Project.h"
#include "core/TimeKeyBlender.h"
#include "core/ObjectNodeUtil.h"

namespace core {

//-------------------------------------------------------------------------------------------------
BoneKey::Data::Data(): mTopBones() {}

BoneKey::Data::Data(const Data& aRhs) {
    for (const Bone2* bone : aRhs.topBones()) {
        mTopBones.push_back(util::TreeUtil::createClone(bone));
    }
}

BoneKey::Data& BoneKey::Data::operator=(const Data& aRhs) {
    deleteAll();

    for (const Bone2* bone : aRhs.topBones()) {
        mTopBones.push_back(util::TreeUtil::createClone(bone));
    }
    return *this;
}

BoneKey::Data::~Data() { deleteAll(); }

QList<Bone2*>& BoneKey::Data::topBones() { return mTopBones; }

const QList<Bone2*>& BoneKey::Data::topBones() const { return mTopBones; }

void BoneKey::Data::deleteAll() {
    qDeleteAll(mTopBones);
    mTopBones.clear();
}

bool BoneKey::Data::isBinding(const ObjectNode& aNode) const {
    for (auto topBone : mTopBones) {
        Bone2::ConstIterator itr(topBone);
        while (itr.hasNext()) {
            if (itr.next()->isBinding(aNode))
                return true;
        }
    }
    return false;
}

Bone2* BoneKey::Data::findBinderBone(const ObjectNode& aNode) {
    for (auto topBone : mTopBones) {
        Bone2::Iterator itr(topBone);
        while (itr.hasNext()) {
            auto bone = itr.next();
            if (bone->isBinding(aNode))
                return bone;
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------------------------------
BoneKey::Cache::Cache(): mInfluence(), mNode(), mInnerMtx(), mFrameSign() {
    // One cap for the whole skinning path: the pose palette, the influence
    // build and the shader's uniform array (MeshTransformVert.glsl sizes it
    // from the same constant). A cache built past it would store bone indices
    // the palette cannot transform.
    mInfluence.setMaxBoneCount(PosePalette::kMaxCount);
}

void BoneKey::Cache::setNode(ObjectNode& aNode) { mNode = aNode.pointee(); }

//-------------------------------------------------------------------------------------------------
BoneKey::BindingCache::BindingCache(): node(), boneIndex(), innerMtx() {}

//-------------------------------------------------------------------------------------------------
BoneKey::BoneKey(): mData(), mCaches(), mCacheOwner(), mBindingCaches() {}

BoneKey::~BoneKey() { destroyCaches(); }

void BoneKey::updateCaches(Project& aProject, const QList<Cache*>& aTargets) {
    auto time = aProject.currentTimeInfo();
    time.frame.set(this->frame());

    auto owner = mCacheOwner.get();
    XC_PTR_ASSERT(owner);

    if (!aTargets.isEmpty()) {
        QWriteLocker lock(&aProject.objectTree().timeCacheLock().working);

        // update bone influence map
        for (auto cache : aTargets) {
            XC_PTR_ASSERT(cache->node());
            auto& node = *cache->node();

            const LayerMesh* mesh = TimeKeyBlender::getAreaMesh(node, time);
            if (!mesh)
                continue;

            cache->setFrameSign(mesh->frameSign());

            if (mesh->vertexCount() <= 0)
                continue;

            // set world matrix
            cache->setInnerMatrix(TimeKeyBlender::getRelativeMatrix(node, time, owner));
            auto originOffset = TimeKeyBlender::getOriginOffset(node, time);

            // influence map matrix
            auto mapMtx = cache->innerMatrix();
            mapMtx.translate(originOffset.x(), originOffset.y());

            BoneInfluenceMap& map = cache->influence();
            // allocate if necessary
            map.allocate(mesh->vertexCount(), false);
            // request writing
            map.writeAsync(aProject, mData.topBones(), mapMtx, *mesh);
        }

#ifndef UNUSE_PARALLEL
        // wake all worker threads
        aProject.paralleler().wakeAll();
#endif
    }


    // update binding caches
    {
        mBindingCaches.clear();
        for (auto topBone : mData.topBones()) {
            Bone2::Iterator itr(topBone);
            while (itr.hasNext()) {
                auto bone = itr.next();
                for (auto node : bone->bindingNodes()) {
                    BindingCache bc;
                    bc.node = node;
                    bc.boneIndex = PosePalette::getBoneIndex(mData, *bone);
                    XC_ASSERT(bc.boneIndex >= 0);
                    auto innerMtx = TimeKeyBlender::getRelativeMatrix(*node, time, owner);
                    bc.innerMtx = innerMtx;
                    mBindingCaches.push_back(bc);
                }
            }
        }
    }
}

void BoneKey::updateCaches(Project& aProject, ObjectNode& aOwner, const QVector<ObjectNode*>& aUniqueRoots) {
    QList<Cache*> caches;

    for (auto root : aUniqueRoots) {
        XC_PTR_ASSERT(root);
        if (!util::TreeUtil::leftContainsRight(aOwner, *root)) {
            continue;
        }

        for (ObjectNode::Iterator itr(root); itr.hasNext();) {
            ObjectNode* node = itr.next();
            XC_PTR_ASSERT(node);

            Cache* cache = findCache(*node);

            if (cache && !caches.contains(cache)) {
                caches.push_back(cache);
            }
        }
    }

    XC_ASSERT(!mCacheOwner || mCacheOwner.get() == &aOwner);
    mCacheOwner = aOwner.pointee();

    updateCaches(aProject, caches);
}

void BoneKey::resetCacheListRecursive(const TimeInfo& aTime, ObjectNode& aNode, CacheList& aNewList) {
    bool useMe = true;

    // A node with an area mesh — even a zero-vertex one — stays in the cache
    // list, so an incremental updateCaches can populate it once the mesh gains
    // vertices (otherwise only a full resetCaches would ever touch it). Nodes
    // with no area mesh (no image/mesh key, e.g. a folder) stay excluded: they
    // can never be a deform target, and keeping them out avoids the null-mesh
    // deref in TimeKeyBlender::setBoneInfluenceMaps.
    {
        const LayerMesh* mesh = TimeKeyBlender::getAreaMesh(aNode, aTime);
        if (!mesh)
            useMe = false;
    }

    // ignore
    {
        const BoneKey* areaBone = TimeKeyBlender::getAreaBone(aNode, aTime);
        // there are more local bone keys.
        if (areaBone && areaBone != this)
            return;
    }
    // ignore
    if (mData.isBinding(aNode))
        return;

    if (useMe) {
        // find a cache from old list
        Cache* cache = popCache(aNode);

        // create new cache
        if (!cache) {
            cache = new Cache();
            // set node
            cache->setNode(aNode);
        }

        aNewList.push_back(cache);
    }

    for (auto child : aNode.children()) {
        resetCacheListRecursive(aTime, *child, aNewList);
    }
}

void BoneKey::resetCaches(Project& aProject, ObjectNode& aOwner) {
#if 0
    // temp list
    CacheList newCaches;

    // find valid caches
    for (ObjectNode::Iterator itr(&aOwner); itr.hasNext();)
    {
        ObjectNode* node = itr.next();
        XC_PTR_ASSERT(node);

        {
            auto time = aProject.currentTimeInfo();
            time.frame.set(this->frame());
            const LayerMesh* mesh = TimeKeyBlender::getAreaMesh(*node, time);
            if (!mesh || mesh->vertexCount() <= 0) continue;
        }

        // find a cache from old list
        Cache* cache = popCache(*node);

        // create new cache
        if (!cache)
        {
            cache = new Cache();
            // set node
            cache->setNode(*node);
        }

        newCaches.push_back(cache);
    }
#else
    // temp list
    CacheList newCaches;
    // make new cache list (recycle old cache)
    {
        auto time = aProject.currentTimeInfo();
        time.frame.set(this->frame());
        resetCacheListRecursive(time, aOwner, newCaches);
    }
#endif

    // destroy unuse caches
    destroyCaches();

    // update list
    for (auto cache : newCaches) {
        mCaches.push_back(cache);
    }

    // set owner
    mCacheOwner = aOwner.pointee();

    updateCaches(aProject, mCaches);
}

BoneKey::Cache* BoneKey::popCache(ObjectNode& aNode) {
    for (auto itr = mCaches.begin(); itr != mCaches.end(); ++itr) {
        Cache* cache = *itr;
        if (cache->node() && cache->node() == &aNode) {
            mCaches.erase(itr);
            return cache;
        }
    }
    return nullptr;
}

BoneKey::Cache* BoneKey::findCache(const ObjectNode& aNode) const {
    for (auto itr = mCaches.begin(); itr != mCaches.end(); ++itr) {
        Cache* cache = *itr;
        if (cache->node() && cache->node() == &aNode) {
            return cache;
        }
    }
    return nullptr;
}

void BoneKey::destroyCaches() {
    qDeleteAll(mCaches);
    mCaches.clear();
    mCacheOwner.reset();
}

TimeKey* BoneKey::createClone() {
    auto newKey = new BoneKey();
    newKey->mData = this->mData;
    return newKey;
}

bool BoneKey::serialize(Serializer& aOut) const {
    // top bone count
    aOut.write(static_cast<int>(mData.topBones().count()));

    // serialize all bones
    for (const auto topBone : mData.topBones()) {
        XC_PTR_ASSERT(topBone);
        if (!serializeBone(aOut, topBone)) {
            return false;
        }
    }

    // The influence cache (cache owner, per-node influence maps, binding
    // caches) is DERIVED: it is rebuilt after load by ProjectLoader, never
    // persisted. Format 0.21 dropped it; see FormatVersion.h.
    return aOut.checkStream();
}

bool BoneKey::serializeBone(Serializer& aOut, const Bone2* aBone) const {
    if (!aBone)
        return true;

    // child count
    aOut.write((int)aBone->children().size());

    // serialize bone
    if (!aBone->serialize(aOut)) {
        return false;
    }

    // iterate children
    for (auto child : aBone->children()) {
        if (!serializeBone(aOut, child)) {
            return false;
        }
    }

    return aOut.checkStream();
}

bool BoneKey::deserialize(Deserializer& aIn) {
    mData.deleteAll();
    destroyCaches();
    mBindingCaches.clear();

    aIn.pushLogScope("BoneKey");

    // top bone count
    int topBoneCount = 0;
    aIn.read(topBoneCount);
    if (topBoneCount < 0) {
        return aIn.errored("invalid top bone count");
    }

    // deserialize all bones
    for (int i = 0; i < topBoneCount; ++i) {
        Bone2* topBone = new Bone2();
        mData.topBones().push_back(topBone);

        if (!deserializeBone(aIn, topBone)) {
            return false;
        }
    }

    // Derived influence caches. Format 0.21 stopped writing them; a <= 0.20
    // file still carries the section, so consume it only to stay in sync and
    // let ProjectLoader rebuild every map after the ID solver.
    // 21 is the LAYOUT BOUNDARY, not the current format: every later minor
    // still writes no section, so do not replace this with the version macro.
    const bool hasLegacyCaches =
        aIn.version().majorVersion() == 0 && aIn.version().minorVersion() < 21;
    if (hasLegacyCaches && !deserializeLegacyCaches(aIn)) {
        return false;
    }

    aIn.popLogScope();
    return aIn.checkStream();
}

bool BoneKey::deserializeLegacyCaches(Deserializer& aIn) {
    // RETIREMENT: delete this branch once the first release that writes 0.21
    // ships. Until then it is the only way an unreleased dev file opens.
    //
    // Read the scalar fields so the stream advances exactly as the 0.20 writer
    // laid them out, then skip the per-vertex arrays (64 B/vertex) without
    // allocating. Nothing here is retained: these are the caches the loader
    // rebuilds, and a mis-sized skip would desynchronise every later block.
    aIn.getRead<int>(); // cache owner reference id

    int cacheCount = 0;
    aIn.read(cacheCount);
    if (cacheCount < 0) {
        return aIn.errored("invalid cache count");
    }

    static const quint64 kBytesPerVertex = 64; // 2 x 4 GLint + 2 x 4 GLfloat
    for (int i = 0; i < cacheCount; ++i) {
        aIn.getRead<int>(); // node reference id

        aIn.getRead<QMatrix4x4>(); // inner matrix
        aIn.getRead<QVector2D>();  // obsolete: origin offset
        aIn.getRead<Frame>();      // frame sign

        int vertexCount = 0;
        aIn.read(vertexCount);
        if (vertexCount < 0) {
            return aIn.errored("invalid influence vertex count");
        }
        aIn.getRead<int>(); // max bone count

        if (quint64(vertexCount) > std::numeric_limits<size_t>::max() / kBytesPerVertex) {
            return aIn.errored("influence record too large");
        }
        const size_t bytes = static_cast<size_t>(vertexCount) * kBytesPerVertex;
        if (!aIn.skipBytes(bytes)) {
            return aIn.errored("influence record truncated");
        }
    }

    int bindingCacheCount = 0;
    aIn.read(bindingCacheCount);
    if (bindingCacheCount < 0) {
        return aIn.errored("invalid binding cache count");
    }
    for (int i = 0; i < bindingCacheCount; ++i) {
        aIn.getRead<int>();       // node reference id
        aIn.getRead<int>();       // bone index
        aIn.getRead<QMatrix4x4>(); // inner matrix
    }

    return aIn.checkStream();
}

bool BoneKey::deserializeBone(Deserializer& aIn, Bone2* aBone) {
    // child count
    const int childCount = aIn.getRead<int>();
    if (childCount < 0) {
        return aIn.errored("invalid child count");
    }

    // deserialize bone
    if (!aBone->deserialize(aIn)) {
        return false;
    }

    // iterate children
    for (int i = 0; i < childCount; ++i) {
        Bone2* child = new Bone2();
        aBone->children().pushBack(child);

        if (!deserializeBone(aIn, child)) {
            return false;
        }
    }

    return aIn.checkStream();
}

} // namespace core
