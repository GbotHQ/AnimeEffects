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
    ++mImageRevision; // anything derived from these pixels is stale now
    // EVERY pixel replacement goes through here (import, project load, a resource reload's
    // ImageSetter -> ResourceNode::updateHandle), so this is the one place that has to keep the
    // derived channels current - not the render path, which only uploads them.
    deriveChannels();
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

void ResourceData::setDerivationWanted(Derivation aKind, bool aWanted) {
    DerivedChannel& channel = mDerived[aKind];
    if (channel.wanted == aWanted) {
        return;
    }
    channel.wanted = aWanted;
    if (!aWanted) {
        // nobody needs it any more: drop the plane so it stops travelling with the file
        freeDerived(aKind);
        return;
    }
    deriveChannels();
}

bool ResourceData::isDerivationStale(Derivation aKind) const {
    const DerivedChannel& channel = mDerived[aKind];
    if (!channel.wanted) {
        return false;
    }
    if (!channel.isPresent()) {
        return true;
    }
    return channel.sourceImageRevision != mImageRevision || channel.recipe != derivationRecipe(aKind);
}

void ResourceData::restoreDerivationState(int aWantedFlags, const int* aRecipes) {
    for (int i = 0; i < Derivation_TERM; ++i) {
        DerivedChannel& channel = mDerived[i];
        channel.wanted = (aWantedFlags & (1 << i)) != 0;
        if (aRecipes) {
            channel.recipe = aRecipes[i];
        }
    }
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

void ResourceData::grabDerived(Derivation aKind, const XCMemBlock& aBlock, const QSize& aSize) {
    DerivedChannel& channel = mDerived[aKind];
    channel.buffer.grab(Format_A8, aBlock, aSize);
    channel.sourceImageRevision = mImageRevision; // measured from these very pixels
    channel.recipe = derivationRecipe(aKind);
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

void ResourceData::copyFrom(const ResourceData& aData) {
    mBuffer = aData.mBuffer;
    mMask = aData.mMask;
    mMaskEnabled = aData.mMaskEnabled;
    mMaskInverted = aData.mMaskInverted;
    ++mMaskRevision; // a copy is a new mask as far as the GPU cache is concerned
    for (int i = 0; i < Derivation_TERM; ++i) {
        // the planes are copied, but a copy is a new resource as far as the GPU cache is
        // concerned; the deriver is NOT copied - it belongs to whoever owns the pixels
        mDerived[i] = aData.mDerived[i];
        ++mDerived[i].revision;
    }
    mDeriver = aData.mDeriver;
    mUserData = aData.mUserData;
    mIdentifier = aData.mIdentifier;
    mStableId = aData.mStableId;
    mPos = aData.mPos;
    mIsLayer = aData.mIsLayer;
    mBlendMode = aData.mBlendMode;
    mImageLoader = aData.mImageLoader;
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
