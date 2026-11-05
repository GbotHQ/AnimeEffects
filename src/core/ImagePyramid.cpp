#include <algorithm>
#include <cstring>
#include "core/ImagePyramid.h"
#include "gl/Global.h"
#include "gl/PagePool.h"
#include "gl/Texture.h"

namespace core {

ImagePyramid::ImagePyramid(): mSize(), mChainLevels(0), mMaxLevel(0), mLevels() {}
ImagePyramid::~ImagePyramid() { clear(); }

void ImagePyramid::clear() {
    mSize = QSize();
    mChainLevels = 0;
    mMaxLevel = 0;
    mLevels.clear();
}

void ImagePyramid::truncate(int aMaxLevel) {
    const int keep = std::max(0, std::min(aMaxLevel, mMaxLevel));
    if (keep + 1 < (int)mLevels.size()) {
        mLevels.resize((size_t)keep + 1);
        mLevels.shrink_to_fit();
    }
    mMaxLevel = keep;
}

QSize ImagePyramid::levelSize(int aLevel) const {
    if (!mSize.isValid() || aLevel < 0)
        return QSize();
    return QSize(std::max(1, mSize.width() >> aLevel), std::max(1, mSize.height() >> aLevel));
}

bool ImagePyramid::capture(gl::Texture& aChain, int aLastLevel) {
    clear();
    if (aChain.id() == 0 || aChain.size().isEmpty())
        return false;

    mSize = aChain.size();
    mChainLevels = aChain.mipmapLevels();
    const int last = std::max(1, std::min(aLastLevel, mChainLevels - 1));
    if (last < 1)
        return false;

    mLevels.resize((size_t)last + 1);
    auto& ggl = gl::Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, aChain.id());
    // Rows are uploaded with the default 4-byte pack alignment; every level here is RGBA8 and
    // width*4 is a multiple of 4, so the line length is the level's own width.
    ggl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int l = 1; l <= last; ++l) {
        const QSize size = levelSize(l);
        auto& data = mLevels[(size_t)l];
        data.resize((size_t)size.width() * size.height() * 4);
        ggl.glGetTexImage(GL_TEXTURE_2D, l, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    }
    ggl.glBindTexture(GL_TEXTURE_2D, 0);

    mMaxLevel = last;
    return true;
}

qint64 ImagePyramid::bytes() const {
    qint64 total = 0;
    for (size_t l = 1; l < mLevels.size(); ++l)
        total += (qint64)mLevels[l].size();
    return total;
}

quint64 ImagePyramid::hashLevel(int aLevel) const {
    if (aLevel <= 0 || aLevel > mMaxLevel || (size_t)aLevel >= mLevels.size())
        return 0;
    quint64 hash = 1469598103934665603ull;
    for (uint8 byte : mLevels[(size_t)aLevel]) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

quint64 ImagePyramid::hashRegion(int aLevel, int aX, int aY, int aWidth, int aHeight) const {
    if (aLevel <= 0 || aLevel > mMaxLevel || (size_t)aLevel >= mLevels.size())
        return 0;
    const QSize size = levelSize(aLevel);
    const int x0 = std::max(0, aX), y0 = std::max(0, aY);
    const int x1 = std::min(size.width(), aX + aWidth), y1 = std::min(size.height(), aY + aHeight);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    quint64 hash = 1469598103934665603ull;
    const uint8* level = mLevels[(size_t)aLevel].data();
    for (int y = y0; y < y1; ++y) {
        const uint8* row = level + ((size_t)y * size.width() + x0) * 4;
        for (int i = 0; i < (x1 - x0) * 4; ++i) {
            hash ^= row[i];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void ImagePyramid::copyLevel(int aLevel, uint8* aDst, const QSize& aSize) const {
    XC_PTR_ASSERT(aDst);
    XC_ASSERT(aLevel >= 1 && aLevel <= mMaxLevel);
    XC_ASSERT((size_t)aLevel < mLevels.size());
    const QSize size = levelSize(aLevel);
    XC_ASSERT(size == aSize);
    std::memcpy(aDst, mLevels[(size_t)aLevel].data(), (size_t)size.width() * size.height() * 4);
}

void ImagePyramid::fillPage(
    int aLevel, int aPageX, int aPageY, const uint8* aBase, const QSize& aBaseSize, uint8* aDst
) const {
    XC_PTR_ASSERT(aDst);
    const int pitch = gl::PagePool::kPitch;
    const int gutter = gl::PagePool::kGutter;
    const QSize levelSize = this->levelSize(aLevel);
    const uint8* level = nullptr;
    if (aLevel > 0) {
        XC_ASSERT(aLevel <= mMaxLevel);
        XC_ASSERT((size_t)aLevel < mLevels.size());
        level = mLevels[(size_t)aLevel].data();
    }
    // A level-0 page whose resource image is not loaded (a key kept alive by the undo stack can
    // have its pixels released) has no source: leave the page transparent. The shader's
    // clamp-to-border would have read the same thing, and the coarse chain still covers it.
    const bool haveBase = aLevel > 0 || aBase != nullptr;

    // The page's content origin, in the level's own texels, minus the gutter: destination texel
    // (x, y) holds level texel (aPageX*256 + x - gutter, ...). One loop covers the gutter and the
    // content, so the two cannot disagree about where the content starts.
    const int originX = aPageX * gl::PagePool::kPageSize - gutter;
    const int originY = aPageY * gl::PagePool::kPageSize - gutter;
    const int w = levelSize.width();
    const int h = levelSize.height();

    for (int y = 0; y < pitch; ++y) {
        const int gy = originY + y;
        uint8* row = aDst + (size_t)y * pitch * 4;
        if (gy < 0 || gy >= h) {
            std::memset(row, 0, (size_t)pitch * 4);
            continue;
        }
        // The columns that have level texels: [x0, x1). The rest of the row is the page's
        // gutter, transparent outside the level. ONE memcpy for the run, not one per texel:
        // a page is 67k texels and the per-texel copy was the dominant cost of a 32 MB upload
        // frame (measured on nene's first paged frames).
        const int x0 = std::max(0, -originX);
        const int x1 = std::min(pitch, w - originX);
        if (x0 > 0)
            std::memset(row, 0, (size_t)x0 * 4);
        if (x1 < pitch)
            std::memset(row + (size_t)x1 * 4, 0, (size_t)(pitch - x1) * 4);
        if (x1 <= x0)
            continue;
        if (level) {
            std::memcpy(
                row + (size_t)x0 * 4, level + ((size_t)gy * w + (originX + x0)) * 4,
                (size_t)(x1 - x0) * 4
            );
        } else if (haveBase) {
            for (int x = x0; x < x1; ++x) {
                const int gx = originX + x;
                uint8* dst = row + (size_t)x * 4;
                XC_ASSERT(gx < aBaseSize.width() && gy < aBaseSize.height());
                const uint8* src = aBase + ((size_t)gy * aBaseSize.width() + gx) * 4;
                const uint32 a = src[3];
                if (a == 255) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                } else {
                    dst[0] = (uint8)((src[0] * a + 127) / 255);
                    dst[1] = (uint8)((src[1] * a + 127) / 255);
                    dst[2] = (uint8)((src[2] * a + 127) / 255);
                    dst[3] = (uint8)a;
                }
            }
        }
    }
}

} // namespace core