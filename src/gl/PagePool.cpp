#include <QOpenGLContext>
#include <algorithm>
#include <cstdio>
#include "gl/FrameStats.h"
#include "gl/Global.h"
#include "gl/PagePool.h"

namespace gl {

namespace {
    bool envSwitchOn(const char* aName, bool aDefault) {
        const QByteArray value = qgetenv(aName);
        if (value.isEmpty())
            return aDefault;
        const bool on = value != "0" && value.compare("false", Qt::CaseInsensitive) != 0;
        return on;
    }

    int envInt(const char* aName, int aDefault) {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(aName, &ok);
        return ok && value > 0 ? value : aDefault;
    }
} // namespace

PagePool& PagePool::instance() {
    // Deliberately never destroyed: its GL objects belong to a context that dies before a
    // static destructor would run (see gl/Texture.cpp's program cache for the same reason).
    static PagePool* pool = new PagePool();
    return *pool;
}

bool PagePool::enabled() {
    // Read ONCE: the caches are built under one policy, so a switch that changed halfway through
    // a session would leave half the images texture-bound and half paged. It is a startup control.
    static const bool on = envSwitchOn("AE_VIRTUAL_TEXTURE", true);
    return on;
}

int PagePool::coarseSize() {
    static const int size = std::max(64, envInt("AE_VT_COARSE", 512));
    return size;
}

qint64 PagePool::defaultBudgetBytes() {
    // Four atlases is what the shader can address; the default asks for all of them. A machine
    // with less VRAM lowers this with AE_VT_BUDGET_MB, and an allocation that fails lowers it
    // for the session (see makeAtlas).
    const qint64 mb = envInt("AE_VT_BUDGET_MB", 256);
    return std::min<qint64>(mb * 1024 * 1024, (qint64)kMaxAtlasCount * kAtlasBytes);
}

qint64 PagePool::defaultUploadCapBytes() {
    const qint64 mb = envInt("AE_VT_UPLOAD_MB", 8);
    return std::max<qint64>(kPageBytes, mb * 1024 * 1024);
}

PagePool::PagePool() {
    mBudgetBytes = defaultBudgetBytes();
    mUploadCapBytes = defaultUploadCapBytes();
    mAtlas.resize(kMaxAtlasCount, 0);
    mAtlasUsed.resize(kMaxAtlasCount, 0);
    mAtlasLastUsed.resize(kMaxAtlasCount, 0);
    mFree.resize(kMaxAtlasCount);
}

bool PagePool::ensureContext() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == mContext)
        return mContext != nullptr;

    // A different context means every GL object this pool named belongs to a context that is
    // no longer current. Nothing here can delete those objects (that would need the old context
    // made current), so the CPU state is dropped and the ids forgotten - the same bounded leak
    // gl/Texture.cpp's shared program takes on a context recreation. There is no page table left
    // pointing at the old slots because every VirtualTexture rebuilds against the new context.
    mContext = context;
    resetState();
    return mContext != nullptr;
}

void PagePool::resetState() {
    ++mGeneration;
    mPages.clear();
    mLru.clear();
    for (int i = 0; i < kMaxAtlasCount; ++i) {
        mAtlas[i] = 0;
        mAtlasUsed[i] = 0;
        mAtlasLastUsed[i] = 0;
        mFree[i].clear();
    }
    mAtlasCount = 0;
    mFrameUploadBytes = 0;
    mFrameUploads = 0;
    mFrameEvictions = 0;
    mFrameFailures = 0;
}

void PagePool::beginFrame() {
    if (!ensureContext())
        return;
    ++mFrame;
    mFrameUploadBytes = 0;
    mFrameUploads = 0;
    mFrameEvictions = 0;
    mFrameFailures = 0;
}

void PagePool::endFrame() {
    if (!ensureContext())
        return;

    // SHRINK: an atlas with no resident page and no use for a sustained period is released.
    // A window resized smaller (or a project closed) must return the memory without a restart,
    // and a whole atlas is the only unit that can be released.
    enum { kIdleFramesBeforeRelease = 600 }; // 10 s at 60 Hz
    for (int i = 0; i < kMaxAtlasCount; ++i) {
        if (mAtlas[i] != 0 && mAtlasUsed[i] == 0 && mFrame > mAtlasLastUsed[i] &&
            mFrame - mAtlasLastUsed[i] > kIdleFramesBeforeRelease) {
            releaseAtlas(i);
        }
    }

    FrameStats::instance().virtualTexture(residentPages(), residentBytes(), mFrameUploadBytes, mFrameEvictions);
}

void PagePool::setBudgetBytes(qint64 aBytes) { mBudgetBytes = std::max<qint64>(0, aBytes); }

bool PagePool::makeAtlas() {
    if (!ensureContext())
        return false;
    if (mAtlasCount >= kMaxAtlasCount)
        return false;
    if (allocatedBytes() + kAtlasBytes > mBudgetBytes)
        return false;

    int index = -1;
    for (int i = 0; i < kMaxAtlasCount; ++i) {
        if (mAtlas[i] == 0) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;

    auto& ggl = Global::functions();
    GLuint id = 0;
    ggl.glGenTextures(1, &id);
    if (id == 0)
        return false;
    ggl.glBindTexture(GL_TEXTURE_2D, id);
    // No mip chain and no mip filter: a page IS the level, and the shader reads it with
    // texture() at level 0. NEAREST between slots would break the gutter, so the atlas is
    // LINEAR and the gutter is what keeps the filter inside a page's own content.
    ggl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSize, kAtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    if (ggl.glGetError() != GL_NO_ERROR) {
        ggl.glDeleteTextures(1, &id);
        // The allocation was refused (VRAM, size, driver): this is exactly the case the design
        // says must degrade, not crash. Drop the budget to what is already allocated so the
        // next frame does not try again, and let the caller keep the coarse fallback.
        mBudgetBytes = allocatedBytes();
        return false;
    }

    mAtlas[index] = id;
    ++mAtlasCount;
    mAtlasUsed[index] = 0;
    mAtlasLastUsed[index] = mFrame;
    mFree[index].clear();
    mFree[index].reserve(kSlotsPerAtlas);
    // High slot indices first, so a fresh atlas hands out slot 0, 1, 2... in that order.
    for (int s = kSlotsPerAtlas - 1; s >= 0; --s)
        mFree[index].push_back(s);

    FrameStats::instance().textureCreated(kAtlasBytes);
    return true;
}

void PagePool::releaseAtlas(int aAtlas) {
    if (aAtlas < 0 || aAtlas >= kMaxAtlasCount || mAtlas[aAtlas] == 0)
        return;
    auto& ggl = Global::functions();
    ggl.glDeleteTextures(1, &mAtlas[aAtlas]);
    mAtlas[aAtlas] = 0;
    mAtlasUsed[aAtlas] = 0;
    mFree[aAtlas].clear();
    --mAtlasCount;
    FrameStats::instance().textureDestroyed(kAtlasBytes);
}

bool PagePool::find(const PageId& aPage, Slot* aSlot) const {
    auto itr = mPages.find(aPage);
    if (itr == mPages.end())
        return false;
    if (aSlot)
        *aSlot = itr->second.slot;
    return true;
}

bool PagePool::acquire(const PageId& aPage, Listener* aListener, Slot* aSlot) {
    XC_PTR_ASSERT(aSlot);
    if (!ensureContext())
        return false;
    if (mPages.find(aPage) != mPages.end())
        return false; // already resident: the caller should have used find()
    if (mFrameUploadBytes + kPageBytes > mUploadCapBytes) {
        ++mFrameFailures;
        return false;
    }

    // A page is only acquired once, but the same page may be demanded by several layers of the
    // same resource in one frame; the caller touches residents before acquiring, so a page
    // touched this frame is at the front of the LRU and is never the victim here.
    int atlas = -1;
    int slot = -1;
    for (int i = 0; i < kMaxAtlasCount && atlas < 0; ++i) {
        if (!mFree[i].empty()) {
            atlas = i;
            slot = mFree[i].back();
            mFree[i].pop_back();
        }
    }
    if (atlas < 0) {
        if (makeAtlas()) {
            // the fresh atlas has every slot free; take one (there may be several free
            // atlases after a shrink, so scan rather than assume an index)
            for (int i = 0; i < kMaxAtlasCount; ++i) {
                if (!mFree[i].empty()) {
                    atlas = i;
                    slot = mFree[i].back();
                    mFree[i].pop_back();
                    break;
                }
            }
        } else {
            // no room under the budget: reuse the least recently touched page
            if (mLru.empty()) {
                ++mFrameFailures;
                return false;
            }
            const PageId victim = mLru.back();
            auto itr = mPages.find(victim);
            XC_ASSERT(itr != mPages.end());
            if (itr == mPages.end()) {
                ++mFrameFailures;
                return false;
            }
            atlas = itr->second.slot.atlas;
            slot = itr->second.slot.slot;
            Listener* listener = itr->second.listener;
            mLru.pop_back();
            mPages.erase(victim);
            --mAtlasUsed[atlas];
            ++mFrameEvictions;
            // the owner clears its table entry BEFORE the slot is rewritten
            if (listener)
                listener->pageEvicted(victim);
        }
    }
    if (atlas < 0 || slot < 0) {
        ++mFrameFailures;
        return false;
    }

    const int sx = (slot % kSlotsPerAxis) * kPitch;
    const int sy = (slot / kSlotsPerAxis) * kPitch;
    Slot entrySlot;
    entrySlot.atlas = atlas;
    entrySlot.slot = slot;
    entrySlot.x = sx + kGutter;
    entrySlot.y = sy + kGutter;

    Entry entry;
    entry.slot = entrySlot;
    entry.listener = aListener;
    entry.lru = mLru.insert(mLru.begin(), aPage);
    mPages.emplace(aPage, entry);
    ++mAtlasUsed[atlas];
    mAtlasLastUsed[atlas] = mFrame;
    mFrameUploadBytes += kPageBytes;
    ++mFrameUploads;
    *aSlot = entrySlot;
    return true;
}

void PagePool::cancel(const Slot& aSlot) {
    if (!aSlot.isValid() || aSlot.atlas < 0 || aSlot.atlas >= kMaxAtlasCount)
        return;
    // Find the entry that names this slot (there is at most one) and forget it, returning the
    // slot to its atlas' free stack. The upload budget was counted in acquire; refund it here
    // so a failed upload does not eat the frame.
    for (auto itr = mPages.begin(); itr != mPages.end(); ++itr) {
        if (itr->second.slot.atlas == aSlot.atlas && itr->second.slot.slot == aSlot.slot) {
            mLru.erase(itr->second.lru);
            mPages.erase(itr);
            mFree[aSlot.atlas].push_back(aSlot.slot);
            --mAtlasUsed[aSlot.atlas];
            if (mFrameUploads > 0)
                --mFrameUploads;
            mFrameUploadBytes = std::max<qint64>(0, mFrameUploadBytes - kPageBytes);
            return;
        }
    }
}

void PagePool::touch(const PageId& aPage) {
    auto itr = mPages.find(aPage);
    if (itr == mPages.end())
        return;
    if (itr->second.lru != mLru.begin())
        mLru.splice(mLru.begin(), mLru, itr->second.lru);
    mAtlasLastUsed[itr->second.slot.atlas] = mFrame;
}

void PagePool::purge() {
    ++mGeneration;
    mPages.clear();
    mLru.clear();
    for (int i = 0; i < kMaxAtlasCount; ++i) {
        if (mAtlas[i] != 0)
            releaseAtlas(i);
        mFree[i].clear();
    }
    mAtlasCount = 0;
}

void PagePool::releaseOwner(quint64 aOwner) {
    for (auto itr = mPages.begin(); itr != mPages.end();) {
        if (itr->first.owner == aOwner) {
            mLru.erase(itr->second.lru);
            mFree[itr->second.slot.atlas].push_back(itr->second.slot.slot);
            --mAtlasUsed[itr->second.slot.atlas];
            itr = mPages.erase(itr);
        } else {
            ++itr;
        }
    }
}

GLuint PagePool::atlasTextureId(int aAtlas) const {
    if (aAtlas < 0 || aAtlas >= kMaxAtlasCount)
        return 0;
    return mAtlas[aAtlas];
}

bool PagePool::readSlot(const Slot& aSlot, uint8* aDst) const {
    if (aSlot.atlas < 0 || aSlot.atlas >= kMaxAtlasCount || mAtlas[aSlot.atlas] == 0 || !aDst)
        return false;
    auto& ggl = Global::functions();

    GLint prevRead = 0;
    ggl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    GLuint fbo = 0;
    ggl.glGenFramebuffers(1, &fbo);
    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    ggl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mAtlas[aSlot.atlas], 0);
    ggl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ggl.glReadPixels(aSlot.x - kGutter, aSlot.y - kGutter, kPitch, kPitch, GL_RGBA, GL_UNSIGNED_BYTE, aDst);
    ggl.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
    ggl.glDeleteFramebuffers(1, &fbo);
    return true;
}

} // namespace gl
