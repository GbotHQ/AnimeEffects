#ifndef CORE_VIRTUALTEXTURE_H
#define CORE_VIRTUALTEXTURE_H

#include <QMatrix4x4>
#include <QSize>
#include <QtGlobal>
#include <memory>
#include <vector>
#include "XC.h"
#include "core/ImagePyramid.h"
#include "gl/BufferObject.h"
#include "gl/PagePool.h"
#include "gl/Texture.h"
#include "img/ResourceHandle.h"
#include "util/NonCopyable.h"

namespace gl {
class EasyShaderProgram;
struct Vector2;
} // namespace gl

namespace core {

/// ONE PAGED IMAGE: the always-resident coarse chain, the page table, the demand map and the
/// frame's upload work. Owned by the ImageKey cache that built it (one per key, like the texture
/// it replaces), registered with the static frame loop so demand, upload and readback happen
/// even while the key is not the one being drawn.
///
/// THE THREE TIERS, and why each exists:
///   - the COARSE CHAIN is every level from the one whose longest side is at most
///     gl::PagePool::coarseSize() (512) down to 1x1, held as one ordinary mipmapped texture.
///     It is always resident, so a page that is not there samples it: blur, never a hole. Its
///     levels are the full chain's levels, copied, not rebuilt - a fallback that is a different
///     filter is a second thing to debug.
///   - the PAGE TABLE is one R16UI texel per page of every level sharper than the coarse chain.
///     0 means "not resident"; otherwise (atlas << 8) | (slot + 1). It is rebuilt from the
///     resident set whenever that set changes, in one upload - the set is a few KB, and an
///     incremental table is a second source of truth that can disagree with the pool.
///   - the DEMAND MAP is one R8 texel per page of the same grid, marked by a geometry pass that
///     walks the layer's own triangles with their deformed positions (see markDemand). It is one
///     LAYER of a shared GL_TEXTURE_2D_ARRAY owned by the demand map (DemandMap in the .cpp), so
///     the whole scene is read back with one copy and one fence however many resources there are;
///     the pages stay wanted for kKeepFrames so a one-frame occlusion or a boundary jitter cannot
///     evict a page that is about to be sampled again.
///
/// WHY THE DEMAND PASS IS GEOMETRY, NOT A TRANSFORM ESTIMATE: a layer with FFD, bone or mesh
/// deformation has a different texel-to-pixel ratio across its own surface, and a whole-layer
/// level would have to be the worst triangle's - blurring the 95 % of the area that is not
/// stretched. Per triangle, the ratio is exact: the fragment shader's derivatives of an affine
/// interpolant are the triangle's own edge ratios, so the level the demand pass computes IS the
/// level the fragment shader will pick (see assets/shaders/LayerDrawingFrag.glsl). The pass also
/// rasterizes the triangle's page rectangle into the map, so the demand is a page set, not a
/// number.
class VirtualTexture: public gl::PagePool::Listener, private util::NonCopyable {
public:
    enum {
        // the number of levels below the coarse chain a table/uniform array can name
        kMaxPageLevels = 16,
        // a page demanded in the last N frames stays wanted (see the class comment)
        kKeepFrames = 3,
    };

    /// True when the resource is big enough to be paged at all. A resource whose longest side is
    /// at most the coarse size is entirely coarse, so a full texture of it is the same residency
    /// with none of the machinery.
    static bool pageable(const QSize& aSize);

    /// The frame boundary. beginFrame maps last frame's demand readbacks and resolves/upload
    /// pages; endFrame issues this frame's readbacks. Both are no-ops when the pool is off.
    static void beginFrame();
    static void endFrame();
    /// Blocks on this frame's demand readbacks, resolves and uploads them, and returns when
    /// every wanted page has been published. The EXPORT policy: a frame is rendered once and
    /// cannot wait two frames for a readback, so the exporter runs a demand-only pass, calls
    /// this, then renders. Never call it from the viewport - the stall it takes is a frame.
    static void synchronize();

    /// How many live paged images there are (for a diagnostic or a gate).
    static int instanceCount();

    VirtualTexture();
    ~VirtualTexture() override;

    /// Builds everything for `aResource`: captures the chain's levels below the coarse threshold,
    /// copies the coarse tail out of `aFullChain`, creates the table and demand textures.
    /// `aFullChain` must be the production chain for the same pixels (created with
    /// gl::Texture::GenerateMipmap). Returns false with nothing built when the size is not
    /// pageable or the capture failed.
    bool build(const img::ResourceHandle& aResource, const QSize& aSize, gl::Texture& aFullChain);
    void destroy();

    bool isValid() const { return mSize.isValid(); }
    const QSize& size() const { return mSize; }
    int maxLevel() const { return mMaxLevel; }
    int coarseLevel() const { return mCoarseLevel; }
    qint64 pyramidBytes() const { return mPyramid.bytes(); }

    const gl::Texture& coarseTexture() const { return mCoarse; }
    const gl::Texture& pageTable() const { return mTable; }

    /// Binds the coarse texture, the table and the atlases, and sets every uniform the
    /// VIRTUAL_TEXTURE shader variation reads. Unit assignment is fixed: 4 coarse, 5 table,
    /// 6..9 atlases.
    void bind(gl::EasyShaderProgram& aShader) const;

    /// Marks the pages this layer's triangles sample this frame. `aPositions` is the skinned
    /// position buffer the layer is drawn from; `aTexCoords` its mesh uv (content pixels).
    void markDemand(
        const QMatrix4x4& aViewMatrix,
        const QSize& aDeviceScreenSize,
        const QVector2D& aTexCoordOffset,
        const gl::BufferObject& aPositions,
        const gl::Vector2* aTexCoords,
        int aVtxCount,
        const gl::BufferObject& aIndexBuffer,
        int aIndexCount
    );

    int tableWidth() const { return mTableWidth; }
    int tableRows() const { return mTableRows; }
    int pageGridX(int aLevel) const;
    int pageGridY(int aLevel) const;
    int pageCount() const;

    /// Diagnostics: the pages currently wanted (demanded within kKeepFrames) and how many of
    /// those are resident. The gate uses it to assert no sampled page fell back.
    int wantedPageCount() const { return (int)mActivePages.size(); }
    int residentWantedCount() const;

private:
    // gl::PagePool::Listener
    void pageEvicted(const gl::PagePool::PageId& aPage) override;

    bool buildCoarse(gl::Texture& aFullChain);
    bool createTargets();
    void fillPageBuffer(int aLevel, int aPageX, int aPageY);
    bool uploadPage(int aLevel, int aPageX, int aPageY, const gl::PagePool::Slot& aSlot);
    void resolveDemand();
    void flushTable();
    void consumeDemand();
    void rebuildTable();
    int pageIndex(int aLevel, int aPageX, int aPageY) const;
    gl::PagePool::PageId pageId(int aLevel, int aPageX, int aPageY) const;
    int levelOfIndex(int aIndex, int* aPageX, int* aPageY) const;
    gl::PagePool::PageId pageIdForIndex(int aIndex) const;

    img::ResourceHandle mResource;
    QSize mSize;
    int mMaxLevel = 0;
    int mCoarseLevel = 0;
    ImagePyramid mPyramid;
    gl::Texture mCoarse;
    gl::Texture mTable;

    int mTableWidth = 0;
    int mTableRows = 0;
    int mLevelRow[kMaxPageLevels] = {};
    int mLevelGridX[kMaxPageLevels] = {};
    int mLevelGridY[kMaxPageLevels] = {};

    // the resident set, as the table texture holds it: 0 or (atlas << 8) | (slot + 1)
    std::vector<quint16> mEntries;
    bool mTableDirty = false;
    // pages demanded within kKeepFrames, and the frame each was last marked in
    std::vector<quint32> mActivePages;
    // membership in mActivePages: without it the same page is appended once per frame and the
    // wanted set grows without bound (every duplicate would be resident, inflating the count
    // and the per-frame work)
    std::vector<quint8> mActiveFlags;
    std::vector<int> mDemandFrame;
    // the shared demand map's layer for this resource (-1 until built)
    int mDemandLayer = -1;
    // the mapped frame this resource's layer was last parsed in
    quint64 mDemandParsedFrame = 0;
    quint64 mPoolGeneration = 0;

    // one page's upload scratch, kept between uploads (one resource is resolved at a time)
    std::vector<uint8> mPageBuffer;
};

} // namespace core

#endif // CORE_VIRTUALTEXTURE_H
