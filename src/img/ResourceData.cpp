#include <algorithm>
#include "util/MathUtil.h"
#include "img/ResourceNode.h"
#include "img/ResourceHandle.h"

namespace img {

ResourceData::ResourceData(const QString& aIdentifier, const ResourceNode* aSerialAddress):
    mBuffer(),
    mMask(),
    mMaskEnabled(true),
    mMaskInverted(false),
    mMaskRevision(0),
    mDerived(),
    mDeriver(),
    mImageRevision(0),
    mPos(),
    mUserData(),
    mIsLayer(),
    mIdentifier(aIdentifier),
    mStableId(),
    mBlendMode(BlendMode_Normal),
    mImageLoader(),
    mSerialAddress(aSerialAddress) {}

void ResourceData::grabImage(const XCMemBlock& aBlock, const QSize& aSize, Format aFormat) {
    mBuffer.grab(aFormat, aBlock, aSize);
    ++mImageRevision; // anything derived from these pixels is stale now, and that is enough
}

XCMemBlock ResourceData::releaseImage() {
    ++mImageRevision;
    for (int i = 0; i < Derivation_TERM; ++i) {
        freeDerived((Derivation)i); // nothing to measure from any more
    }
    return mBuffer.release();
}

void ResourceData::freeImage() {
    ++mImageRevision;
    mBuffer.free();
}

void ResourceData::grabMask(const XCMemBlock& aBlock, const QSize& aSize) {
    mMask.grab(Format_A8, aBlock, aSize);
    ++mMaskRevision;
}

XCMemBlock ResourceData::releaseMask() {
    ++mMaskRevision;
    return mMask.release();
}

void ResourceData::freeMask() {
    ++mMaskRevision;
    mMask.free();
}

int ResourceData::derivationRecipe(Derivation aKind) {
    switch (aKind) {
    case Derivation_Distance:
        // recipe 1: exact Euclidean distance from the alpha's 128 level (the visible outline),
        // 8-bit, saturated at 255 px. Bump this whenever the computation changes meaning, so
        // planes built by the old one are recognisably out of date.
        return 1;
    default:
        return 0;
    }
}

const char* ResourceData::derivationName(Derivation aKind) {
    switch (aKind) {
    case Derivation_Distance: return "outline distance";
    default: return "?";
    }
}

// A channel that is absent, or present but measured from other pixels / built by another recipe,
// is not current: it has to be derived before anybody may read it.
bool ResourceData::isDerivationStale(Derivation aKind) const {
    const DerivedChannel& channel = mDerived[aKind];
    if (!channel.isPresent()) {
        return true;
    }
    return channel.sourceImageRevision != mImageRevision || channel.recipe != derivationRecipe(aKind);
}

void ResourceData::deriveChannels() {
    if (!mDeriver) {
        return;
    }
    bool anyStale = false;
    for (int i = 0; i < Derivation_TERM; ++i) {
        anyStale = anyStale || isDerivationStale((Derivation)i);
    }
    if (anyStale) {
        mDeriver(*this);
    }
}

const Buffer& ResourceData::derived(Derivation aKind) {
    // the demand: bring this channel up to date, then hand it over whatever the outcome was. A
    // channel that cannot be built (no pixels, no outline) stays absent - the deriver reports it,
    // and the consumer draws nothing rather than something it measured from the wrong pixels.
    deriveChannels();
    return mDerived[aKind].buffer;
}

void ResourceData::grabDerived(Derivation aKind, const XCMemBlock& aBlock, const QSize& aSize, int aRecipe) {
    DerivedChannel& channel = mDerived[aKind];
    channel.buffer.grab(Format_A8, aBlock, aSize);
    channel.sourceImageRevision = mImageRevision; // measured from these very pixels
    // the recipe the bytes were BUILT with, which is not always this build's current one: a plane
    // read back from a project carries the recipe of the build that wrote it, and comparing it
    // against derivationRecipe() is what makes an out-of-date plane recognisably stale instead of
    // silently consumed
    channel.recipe = aRecipe;
    ++channel.revision;
}

void ResourceData::freeDerived(Derivation aKind) {
    DerivedChannel& channel = mDerived[aKind];
    channel.buffer.free();
    channel.sourceImageRevision = -1;
    ++channel.revision;
}

void ResourceData::setPos(const QPoint& aPos) { mPos = aPos; }

void ResourceData::setBlendMode(BlendMode aMode) {
    if (aMode != BlendMode_TERM) {
        mBlendMode = aMode;
    } else {
        mBlendMode = BlendMode_Normal;
    }
}

bool ResourceData::matchesIdentity(const ResourceData& aRhs) const {
    if (mIsLayer != aRhs.mIsLayer) {
        return false;
    }
    if (!mStableId.isEmpty() && !aRhs.mStableId.isEmpty()) {
        return mStableId == aRhs.mStableId;
    }
    return mIdentifier == aRhs.mIdentifier;
}

QRect ResourceData::rect() const { return QRect(mPos, mBuffer.pixelSize()); }

QVector2D ResourceData::center() const { return util::MathUtil::getCenter(rect()); }

} // namespace img
