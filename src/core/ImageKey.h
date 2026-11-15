#ifndef CORE_IMAGEKEY_H
#define CORE_IMAGEKEY_H

#include "util/Easing.h"
#include "gl/Texture.h"
#include "img/ResourceHandle.h"
#include "img/BlendMode.h"
#include "core/GridMesh.h"
#include "core/TimeKey.h"
#include "gui/obj/obj_Item.h"

namespace img {
class ResourceNode;
}

namespace core {

class ImageKey: public TimeKey {
public:
    class Data {
        util::Easing::Param mEasing;
        img::ResourceHandle mResHandle;
        img::BlendMode mBlendMode;
        QVector2D mImageOffset;
        GridMesh mGridMesh;

    public:
        Data();
        Data(const Data& aRhs);
        Data& operator=(const Data& aRhs);
        util::Easing::Param& easing() { return mEasing; }
        const util::Easing::Param& easing() const { return mEasing; }
        img::ResourceHandle& resource() { return mResHandle; }
        const img::ResourceHandle& resource() const { return mResHandle; }
        void setBlendMode(img::BlendMode aMode) { mBlendMode = aMode; }
        img::BlendMode blendMode() const { return mBlendMode; }
        void setImageOffset(const QVector2D& aOffset);
        const QVector2D& imageOffset() const { return mImageOffset; }
        GridMesh& gridMesh() { return mGridMesh; }
        const GridMesh& gridMesh() const { return mGridMesh; }
    };

    // What an uploaded cache texture was built from: the RESOURCE it came from, and the revision
    // that resource gave its own pixels. A revision alone is not an identity - every resource
    // numbers its mask from zero, and a resource update alternates the node between two data
    // objects as it is undone and redone (img::ResourceNode::swapData) - so a cache comparing
    // numbers only could keep one resource's mask (or distance plane) uploaded and draw it on a
    // key that now points at another. The pair is what makes "unchanged" mean unchanged; the
    // revision side stays necessary because a mask is edited in place (core/LayerMaskBake).
    class SourceKey {
    public:
        bool matches(const img::ResourceNode* aResource, int aRevision) const {
            return mResource == aResource && mRevision == aRevision;
        }
        void set(const img::ResourceNode* aResource, int aRevision) {
            mResource = aResource;
            mRevision = aRevision;
        }

    private:
        const img::ResourceNode* mResource = nullptr;
        int mRevision = -1;
    };

    class Cache {
        gl::Texture mTexture;
        gl::Texture mMaskTexture;
        SourceKey mMaskSource;
        gl::Texture mDistanceTexture;
        SourceKey mDistanceSource;

    public:
        Cache();
        gl::Texture& texture() { return mTexture; }
        const gl::Texture& texture() const { return mTexture; }
        // single-channel (GL_R8) copy of the layer mask; only created when the resource
        // carries one (see ResourceData::hasMask)
        gl::Texture& maskTexture() { return mMaskTexture; }
        const gl::Texture& maskTexture() const { return mMaskTexture; }
        SourceKey& maskSource() { return mMaskSource; }
        const SourceKey& maskSource() const { return mMaskSource; }
        // single-channel (GL_R8) copy of the layer's outline distance field
        gl::Texture& distanceTexture() { return mDistanceTexture; }
        const gl::Texture& distanceTexture() const { return mDistanceTexture; }
        SourceKey& distanceSource() { return mDistanceSource; }
        const SourceKey& distanceSource() const { return mDistanceSource; }
    };

    enum { kDefaultMeshCellSize = 16 };

    ImageKey();

    Data& data() { return mData; }
    const Data& data() const { return mData; }

    Cache& cache() { return mCache; }
    const Cache& cache() const { return mCache; }

    void setImage(const img::ResourceHandle& aResource, img::BlendMode aMode);
    void setImage(const img::ResourceHandle& aResource);
    void setImageOffset(const QVector2D& aOffset);
    void setImageOffsetByCenter();
    bool hasImage() const { return mData.resource() && mData.resource()->hasImage(); }
    void resetGridMesh(int CellSize = kDefaultMeshCellSize);

    // Rebuilds the mask texture if the resource's mask pixels changed since the last build.
    // Called from the render path (where a GL context is current), which is what lets
    // authoring commands edit mask pixels without touching GL themselves.
    void syncMaskTexture();
    // Uploads the resource's outline distance field, deriving it first when it is absent or does
    // not describe the current pixels (that is the demand for the channel - nobody declares it).
    // Returns whether a usable field is now bound.
    bool syncDistanceTexture();
    QJsonObject serializeToJson() const;
    bool deserializeFromJson(QJsonObject json, util::LifeLink::Pointee<Project> project);
    virtual TimeKeyType type() const { return TimeKeyType_Image; }
    virtual bool canHoldChild() const { return true; }
    virtual TimeKey* createClone();
    virtual bool serialize(Serializer& aOut) const;
    virtual bool deserialize(Deserializer& aIn);

    virtual void sleep();
    virtual void awake();

private:
    void resetTextureCache();

    Data mData;
    Cache mCache;
    int mSleepCount;
};

} // namespace core

#endif // CORE_IMAGEKEY_H
