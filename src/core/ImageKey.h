#ifndef CORE_IMAGEKEY_H
#define CORE_IMAGEKEY_H

#include "util/Easing.h"
#include "gl/Texture.h"
#include "img/ResourceHandle.h"
#include "img/BlendMode.h"
#include "core/GridMesh.h"
#include "core/TimeKey.h"
#include "gui/obj/obj_Item.h"

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

    class Cache {
        gl::Texture mTexture;
        gl::Texture mMaskTexture;
        int mMaskRevision;
        gl::Texture mDistanceTexture;
        int mDistanceRevision;
        int mDistanceImageRevision;

    public:
        Cache();
        gl::Texture& texture() { return mTexture; }
        const gl::Texture& texture() const { return mTexture; }
        // single-channel (GL_R8) copy of the layer mask; only created when the resource
        // carries one (see ResourceData::hasMask)
        gl::Texture& maskTexture() { return mMaskTexture; }
        const gl::Texture& maskTexture() const { return mMaskTexture; }
        int maskRevision() const { return mMaskRevision; }
        void setMaskRevision(int aRevision) { mMaskRevision = aRevision; }
        // single-channel (GL_R8) copy of the layer's outline distance field
        gl::Texture& distanceTexture() { return mDistanceTexture; }
        const gl::Texture& distanceTexture() const { return mDistanceTexture; }
        int distanceRevision() const { return mDistanceRevision; }
        void setDistanceRevision(int aRevision) { mDistanceRevision = aRevision; }
        // the image revision the distance plane was last derived from (or last attempted for):
        // bounds the re-bake to once per pixel change instead of once per frame
        int distanceImageRevision() const { return mDistanceImageRevision; }
        void setDistanceImageRevision(int aRevision) { mDistanceImageRevision = aRevision; }
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
    void syncDistanceTexture();
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
