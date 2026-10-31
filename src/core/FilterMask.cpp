#include <QBuffer>
#include <algorithm>
#include <QByteArray>
#include "gl/Global.h"
#include "core/Deserializer.h"
#include "core/FilterMask.h"
#include "core/Serializer.h"

namespace core {

FilterMask::FilterMask():
    mContentRect(), mImage(), mNeutral(0), mTexture(), mRevision(0), mUploadedRevision(0), mCreated(false),
    mDirtyAll(false), mDirtyBox() {}

void FilterMask::reset(const QRectF& aContentRect, int aNeutralValue) {
    mContentRect = aContentRect;
    mNeutral = std::min(std::max(aNeutralValue, 0), 255);
    if (aContentRect.isEmpty()) {
        clear();
        return;
    }
    // The texel grid is the content rectangle itself: one texel per content pixel (see the
    // header - no cap, the artwork's own resolution).
    const QSize size(
        qMax(1, qRound(aContentRect.width())), qMax(1, qRound(aContentRect.height()))
    );
    mImage = QImage(size, QImage::Format_Grayscale8);
    mImage.fill(uchar(mNeutral));
    mCreated = false;
    markDirtyAll();
}

void FilterMask::clear() {
    if (mImage.isNull())
        return;
    mImage.fill(uchar(mNeutral));
    markDirtyAll();
}

void FilterMask::markDirty(const QRect& aTexels) {
    if (mImage.isNull() || aTexels.isEmpty())
        return;
    const QRect box = aTexels.intersected(mImage.rect());
    if (box.isEmpty())
        return;
    mDirtyBox = mDirtyBox.isNull() ? box : mDirtyBox.united(box);
    ++mRevision;
}

void FilterMask::markDirtyAll() {
    if (mImage.isNull())
        return;
    mDirtyAll = true;
    ++mRevision;
}

GLuint FilterMask::texture() {
    if (mImage.isNull())
        return 0;
    if (!mCreated) {
        uploadAll();
        return mTexture.id();
    }
    if (mRevision != mUploadedRevision) {
        if (mDirtyAll) {
            uploadAll();
        } else if (!mDirtyBox.isNull()) {
            uploadBox(mDirtyBox);
        }
        mUploadedRevision = mRevision;
        mDirtyAll = false;
        mDirtyBox = QRect();
    }
    return mTexture.id();
}

void FilterMask::uploadAll() {
    if (mImage.isNull())
        return;
    // ONE BYTE PER TEXEL, uploaded as GL_R8: the shaders read .r, and the field is now the
    // content's own resolution - as RGBA it would be four times the RAM and four times the VRAM
    // (a 15000 px canvas would cost 900 MB instead of 225).
    gl::Global::Functions& ggl = gl::Global::functions();
    const int stride = mImage.bytesPerLine();
    // QImage pads each scanline to four bytes; GL reads the padding as texels unless it is told
    // the row length and that a texel is one byte
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    if (!mCreated) {
        mTexture.create(mImage.size(), mImage.constBits(), GL_RED, GL_R8, GL_UNSIGNED_BYTE);
        mTexture.setFilter(GL_LINEAR);
        mTexture.setWrap(GL_CLAMP_TO_EDGE);
        mCreated = true;
    } else {
        ggl.glBindTexture(GL_TEXTURE_2D, mTexture.id());
        ggl.glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, mImage.width(), mImage.height(), GL_RED, GL_UNSIGNED_BYTE, mImage.constBits()
        );
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
    }
    ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    mUploadedRevision = mRevision;
    mDirtyAll = false;
    mDirtyBox = QRect();
}

void FilterMask::uploadBox(const QRect& aTexels) {
    if (mImage.isNull())
        return;
    const QRect box = aTexels.intersected(mImage.rect());
    if (box.isEmpty())
        return;

    // the sub-image is cut on its own so the upload is one contiguous box (and its stride is the
    // box's, not the field's)
    const QImage sub = mImage.copy(box);
    gl::Global::Functions& ggl = gl::Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, mTexture.id());
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, sub.bytesPerLine());
    ggl.glTexSubImage2D(
        GL_TEXTURE_2D, 0, box.x(), box.y(), box.width(), box.height(), GL_RED, GL_UNSIGNED_BYTE, sub.constBits()
    );
    ggl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    ggl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
}

bool FilterMask::serialize(Serializer& aOut) const {
    aOut.write(mContentRect);
    aOut.write(mImage.size());
    aOut.write(mNeutral);

    QByteArray bytes;
    if (!mImage.isNull()) {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!mImage.save(&buffer, "PNG"))
            return false;
    }
    aOut.write(XCMemBlock((uint8*)bytes.data(), size_t(bytes.size())));
    return aOut.checkStream();
}

bool FilterMask::deserialize(Deserializer& aIn) {
    aIn.pushLogScope("FilterMask");
    aIn.read(mContentRect);
    QSize size;
    aIn.read(size);
    aIn.read(mNeutral);

    XCMemBlock blob;
    if (!aIn.readWithAlloc(blob)) {
        return aIn.errored("invalid filter mask bytes");
    }
    if (size.isEmpty() || size.width() > kMaxStoredTexels || size.height() > kMaxStoredTexels) {
        delete[] blob.data;
        return aIn.errored("invalid filter mask size");
    }

    QImage image = QImage::fromData(QByteArray::fromRawData((const char*)blob.data, int(blob.size)), "PNG");
    delete[] blob.data;
    if (image.isNull() || image.size() != size) {
        return aIn.errored("invalid filter mask image");
    }

    mImage = image.convertToFormat(QImage::Format_Grayscale8);
    mCreated = false;
    mDirtyAll = true;
    ++mRevision;

    aIn.popLogScope();
    return aIn.checkStream();
}

} // namespace core
