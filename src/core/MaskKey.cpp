#include <QBuffer>
#include <QByteArray>
#include "core/Deserializer.h"
#include <QString>
#include "core/MaskKey.h"
#include "core/MaskPyramid.h"
#include "core/Serializer.h"
#include "core/VirtualTexture.h"

namespace core {

MaskKey::Data::Data(): mEasings(), mField(), mContentRect() {}

MaskKey::Data::TextureHolder::~TextureHolder() = default;

const gl::Texture* MaskKey::Data::textureId() const {
    if (mField.isNull())
        return nullptr;
    if (!mTexture)
        mTexture = std::make_shared<TextureHolder>();

    // A holder that does not describe THIS image is dropped rather than patched: it can be shared
    // with another Data (a blend result, an undo state) that still points at the image it was
    // built from, and rebuilding over it would replace THEIR texture with this one's pixels. See
    // markDirty for how a holder comes to be ahead of an image.
    if (mTexture->built() && mTexture->key != mField.cacheKey())
        mTexture = std::make_shared<TextureHolder>();

    if (!mTexture->uploaded) {
        // the whole field, once: ONE BYTE PER TEXEL as GL_R8 (see core/FilterMask.cpp) - the pass
        // multiplies .r, so an RGBA upload would be four times the VRAM - plus the mip chain its
        // minified sampling needs. A PAGED holder keeps its pool resource: both forms describe the
        // field, and markDirty keeps both in step, so a caller that wants the full chain pays for
        // it but does not destroy the paged one.
        mTexture->mip.create(mTexture->texture, mField);
        mTexture->uploaded = true;
        mTexture->key = mField.cacheKey();
        return &mTexture->texture;
    }

    // the accumulated damage: level 0's box from the image and one box per level above it
    mTexture->mip.update(mTexture->texture, mField);
    return &mTexture->texture;
}

VirtualTexture* MaskKey::Data::virtualTexture() const {
    if (mField.isNull() || !mPainted)
        return nullptr;
    if (!VirtualTexture::pageable(mField.size()))
        return nullptr;
    if (!mTexture)
        mTexture = std::make_shared<TextureHolder>();
    const quint64 fieldKey = mField.cacheKey();
    if (mTexture->built() && mTexture->key != fieldKey)
        mTexture = std::make_shared<TextureHolder>();

    // THE DAMAGE IS APPLIED HERE, not in markDirty: the brush paints outside any guaranteed GL
    // context, and this accessor is called from the render path (LayerNode's demand pass), which
    // is exactly the point where an upload is safe.
    if (mTexture->paged) {
        if (mTexture->dirtyBoxValid) {
            mTexture->paged->updateMaskBox(mField, mTexture->dirtyBox);
            mTexture->dirtyBox = QRect();
            mTexture->dirtyBoxValid = false;
        }
        return mTexture->paged.get();
    }

    if (!mTexture->pyramid) {
        // THE ONE GPU BUILD. The chain owns the resample rule (MaskMipChain), so the pyramid is
        // captured from it rather than computed here - see MaskPyramid's class note. Reuse the
        // full chain when the holder already has one for this field (a mask that was drawn before
        // the pool got to it, or a tool's preview), otherwise build a transient one and drop it.
        gl::Texture transient;
        MaskMipChain transientMip;
        const gl::Texture* source = nullptr;
        if (mTexture->uploaded && mTexture->key == fieldKey && mTexture->texture.id() != 0) {
            mTexture->mip.update(mTexture->texture, mField);
            source = &mTexture->texture;
        } else {
            transientMip.create(transient, mField);
            source = &transient;
        }
        auto pyramid = std::make_unique<MaskPyramid>();
        const bool ok = pyramid->capture(*source);
        if (transient.id() != 0)
            transient.destroy();
        if (!ok)
            return nullptr;
        mTexture->pyramid = std::move(pyramid);
    }

    auto paged = std::make_unique<VirtualTexture>();
    if (!paged->buildMask(*mTexture->pyramid)) {
        mTexture->pyramid.reset();
        return nullptr;
    }
    mTexture->paged = std::move(paged);
    mTexture->key = fieldKey;
    // the pyramid is the authority from here: the full chain is not what a page fill reads, and
    // keeping it would double the field's GPU cost for nothing
    if (mTexture->uploaded) {
        mTexture->texture.destroy();
        mTexture->uploaded = false;
    }
    return mTexture->paged.get();
}

void MaskKey::Data::markDirty(const QRect& aTexels) {
    // the brush wrote into the pixels, so the matte now hides something
    mPainted = true;
    if (!mTexture || !mTexture->built())
        return;   // nothing built yet: the next build reads the field it finds

    // THE BOX IS THE WHOLE DIFFERENCE. A paint writes through the image's scanlines, which detaches
    // the pixels (QImage's copy-on-write) and changes cacheKey, but the holder's chain/pyramid
    // still describe the pre-stamp image and the only pixels that differ are the box the brush
    // reports. So those are patched there - one box per level - and the holder is moved on to the
    // detached image. A copy taken before the stamp keeps the old key, so undo rebuilds.
    bool patched = false;
    if (mTexture->paged && mTexture->pyramid && mTexture->pyramid->size() == mField.size()) {
        // GPU work is DEFERRED to virtualTexture() (the brush has no guaranteed context); the box
        // is a union, so a fast drag's stamps cost one update per frame, not one per stamp
        mTexture->dirtyBox = mTexture->dirtyBoxValid
            ? mTexture->dirtyBox.united(aTexels)
            : aTexels;
        mTexture->dirtyBoxValid = true;
        patched = true;
    }
    if (mTexture->uploaded && mTexture->mip.isValid() && mTexture->mip.baseSize() == mField.size()) {
        mTexture->mip.markDirty(aTexels);
        patched = true;
    }
    if (patched)
        mTexture->key = mField.cacheKey();
    else if (mTexture->built())
        mTexture.reset();   // a different grid: this chain is not this field's
}

void MaskKey::Data::alloc(const QSize& aSize, const QRectF& aContentRect, uchar aValue) {
    mTexture.reset();   // a new image always gets its own texture
    const QSize size(aSize.expandedTo(QSize(1, 1)));
    mField = QImage(size, QImage::Format_Grayscale8);
    mField.fill(aValue);
    mContentRect = aContentRect;
    mPainted = (aValue != kNeutral);
}

void MaskKey::Data::write(const QImage& aField, const QRectF& aContentRect) {
    mTexture.reset();
    if (aField.isNull()) {
        clear();
        return;
    }
    QImage image = aField;
    // NO DOWNSCALE HERE: the field is the content's own resolution (see kMaxStoredTexels), so
    // whatever a caller hands over is kept as it is - a resampled matte is a blurrier matte, and
    // the caller (TimeLineUtil::maskTexelSize) already builds the right grid.
    if (image.format() != QImage::Format_Grayscale8)
        image = image.convertToFormat(QImage::Format_Grayscale8);
    mField = image;
    mContentRect = aContentRect;
    // an AUTHORING path (an import, an invert): scan once here rather than per frame
    mPainted = false;
    for (int y = 0; y < image.height() && !mPainted; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            if (row[x] != kNeutral) {
                mPainted = true;
                break;
            }
        }
    }
}

void MaskKey::Data::assign(const Data& aRhs) {
    mField = aRhs.mField;
    mContentRect = aRhs.mContentRect;
    mEasings = aRhs.mEasings;
    mPainted = aRhs.mPainted;

    // ONE HOLDER FOR ONE FIELD, AND THE SOURCE IS LINKED TO IT. The per-frame expansion assignment
    // used to take the source's holder only when it was already built; a holder built on the
    // EXPANS side was then invisible to the key, so the brush's markDirty (which runs on the KEY)
    // could not reach the paged resource the frame draws from. The rules below keep a holder
    // BUILT for this field when either side has one - preferring ours, which may be the one
    // carrying the page pool - and otherwise share a placeholder between the two, so whichever
    // side builds first builds for both.
    //
    // Sharing is only ever with a holder that describes these pixels: a holder built for another
    // image is dropped rather than patched, exactly as textureId()/markDirty require, because a
    // copy taken before a stamp must rebuild rather than show pixels its own image does not have.
    const quint64 fieldKey = mField.cacheKey();
    const auto builtForField = [fieldKey](const std::shared_ptr<TextureHolder>& aHolder) {
        return aHolder && aHolder->built() && aHolder->key == fieldKey;
    };
    if (builtForField(mTexture)) {
        // ours is live: let the source render from it too
        aRhs.mTexture = mTexture;
    } else if (builtForField(aRhs.mTexture)) {
        mTexture = aRhs.mTexture;
    } else {
        // neither describes this field yet: one placeholder, shared, that the first build fills
        if (!aRhs.mTexture)
            aRhs.mTexture = std::make_shared<TextureHolder>();
        mTexture = aRhs.mTexture;
    }
}

void MaskKey::Data::swap(Data& aRhs) {
    mField.swap(aRhs.mField);
    mTexture.swap(aRhs.mTexture);
    std::swap(mPainted, aRhs.mPainted);
    std::swap(mContentRect, aRhs.mContentRect);
    std::swap(mEasings, aRhs.mEasings);
}

void MaskKey::Data::clear() {
    mTexture.reset();
    mPainted = false;
    mField = QImage();
    mContentRect = QRectF();
}

MaskKey::MaskKey(): TimeKey(), mData() {}

TimeKey* MaskKey::createClone() {
    auto newKey = new MaskKey();
    newKey->mData.assign(mData);
    return newKey;
}

bool MaskKey::serialize(Serializer& aOut) const {
    // easing
    aOut.writeEasings(&mData.easing(0), kComponents);

    // the field: presence, where it lives, its texel grid, then a PNG blob. The grid is the
    // CONTENT's own - the mip chain is an allocation detail of the texture and never reaches the
    // file.
    const bool valid = mData.isValid();
    aOut.write(valid);
    if (valid) {
        aOut.write(mData.contentRect());
        aOut.write(mData.texelSize());

        QByteArray bytes;
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!mData.field().save(&buffer, "PNG")) {
                return false;
            }
        }
        aOut.write(XCMemBlock((uint8*)bytes.data(), size_t(bytes.size())));
    }
    return aOut.checkStream();
}

bool MaskKey::deserialize(Deserializer& aIn) {
    aIn.pushLogScope("MaskKey");

    if (!aIn.readEasings(&mData.easing(0), kComponents)) {
        return aIn.errored("invalid easing param");
    }

    bool valid = false;
    aIn.read(valid);
    if (!valid) {
        mData.clear();
        aIn.popLogScope();
        return aIn.checkStream();
    }

    QRectF rect;
    QSize size;
    aIn.read(rect);
    aIn.read(size);

    XCMemBlock blob;
    if (!aIn.readWithAlloc(blob)) {
        return aIn.errored("invalid mask bytes");
    }
    if (size.isEmpty() || size.width() > kMaxStoredTexels || size.height() > kMaxStoredTexels) {
        delete[] blob.data;
        return aIn.errored("invalid mask size");
    }

    QImage image = QImage::fromData(QByteArray::fromRawData((const char*)blob.data, int(blob.size)), "PNG");
    delete[] blob.data;
    if (image.isNull() || image.size() != size) {
        return aIn.errored("invalid mask image");
    }

    mData.write(image, rect);
    aIn.popLogScope();
    return aIn.checkStream();
}

} // namespace core
