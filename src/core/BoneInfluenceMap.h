#ifndef CORE_BONEINFLUENCEMAP_H
#define CORE_BONEINFLUENCEMAP_H

#include <QtOpenGL/QtOpenGL>
#include <QVector>
#include <QVector2D>
#include <QRectF>
#include <QMatrix4x4>
#include "util/NonCopyable.h"
#include "thr/Task.h"
#include "gl/Vector4.h"
#include "gl/Vector4I.h"
#include "core/Bone2.h"
namespace core {
class Project;
}
namespace core {
class LayerMesh;
}

namespace core {

class BoneInfluenceMap: private util::NonCopyable {
public:
    // Attribute storage: 4 bone indices as uint8 (the shader reads an ivec4,
    // bound through glVertexAttribIPointer) and 4 float32 weights (a vec4).
    // 40 B/vertex for both sets, against 64 B with int32 indices and the same
    // float weights. The uint16 weight experiment is recorded in the plan: it
    // exceeded the 1-LSB gate on 2.anie (max 12 on 61 pixels), so weights stay
    // float32. Nothing here is persisted (format 0.21).
    typedef GLubyte IndicesType;
    typedef GLfloat WeightsType;

    class Accessor {
    public:
        Accessor();
        Accessor(const BoneInfluenceMap& aOwner);
        // Each points at vertexCount * 4 tightly packed components.
        const IndicesType* indices0() const;
        const IndicesType* indices1() const;
        const WeightsType* weights0() const;
        const WeightsType* weights1() const;

    private:
        const BoneInfluenceMap* mOwner;
    };

    enum { kBonePerVtxMaxEach = 4, kBonePerVtxMaxAll = 8 };
    static_assert(kBonePerVtxMaxEach == 4, "the skinning shader reads one ivec4/vec4 per set");
    static_assert(sizeof(IndicesType) == 1, "bone indices are packed uint8");
    static_assert(sizeof(WeightsType) == 4, "bone weights stay float32");

    BoneInfluenceMap();
    ~BoneInfluenceMap();

    void setMaxBoneCount(int aBoneCount);
    void allocate(int aVertexCount, bool aInitialize = true);
    int vertexCount() const { return mVertexCount; }

    void
    writeAsync(Project& aProject, const QList<Bone2*>& aTopBones, const QMatrix4x4& aGroupMtx, const LayerMesh& aMesh);

    Accessor accessor() const;

private:
    class BoneParam {
    public:
        BoneParam();
        bool hasParent;
        bool hasRange;
        BoneShape shape;
    };

    class BoneList {
    public:
        BoneList() {}
        QVector<BoneParam> params;
    };

    struct WorkAttribute {
        int id[kBonePerVtxMaxAll];
        float weight[kBonePerVtxMaxAll];
        short count;
        QVector2D vertex;
        void clear();
        void tryPushBoneWeight(int aId, float aWeight);
    };

    class BuildTask: public thr::Task {
    public:
        BuildTask(Project& aProject, BoneInfluenceMap& aOwner);
        void run() override;
        void cancel();

    private:
        Project& mProject;
        BoneInfluenceMap& mOwner;
    };

    void makeBoneList(const QList<Bone2*>& aTopBones);
    void build();
    void transformVertices();
    void writeWeights();
    void writeVertexAttribute();
    bool isBuildCanceled() const;
    void waitBuilding() const;

    int mVertexCount;
    int mMaxBoneCount;
    QMatrix4x4 mGroupMtx;
    BoneList mBoneList;
    QScopedArrayPointer<IndicesType> mIndices[2];
    QScopedArrayPointer<WeightsType> mWeights[2];
    QScopedArrayPointer<WorkAttribute> mWorks;
    QScopedPointer<BuildTask> mBuildTask;
};

} // namespace core

#endif // CORE_BONEINFLUENCEMAP_H
