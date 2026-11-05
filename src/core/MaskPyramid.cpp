#include "core/MaskPyramid.h"
#include <QtCore/qfloat16.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "gl/Global.h"
#include "gl/PagePool.h"

namespace core {

namespace {
    int trailingZeroBits(int aValue) {
        int count = 0;
        while (aValue > 1 && (aValue & 1) == 0) {
            aValue >>= 1;
            ++count;
        }
        return count;
    }
} // namespace

MaskPyramid::MaskPyramid(): mSizes(), mLevels() {}

void MaskPyramid::clear() {
    mSizes.clear();
    mLevels.clear();
}

QSize MaskPyramid::levelSizeOf(const QSize& aBase, int aLevel) {
    return QSize(std::max(1, aBase.width() >> aLevel), std::max(1, aBase.height() >> aLevel));
}

QSize MaskPyramid::levelSize(int aLevel) const {
    if (aLevel < 0 || aLevel >= (int)mSizes.size())
        return QSize();
    return mSizes[(size_t)aLevel];
}

const std::vector<uint8>& MaskPyramid::levelBytes(int aLevel) const {
    XC_ASSERT(0 <= aLevel && aLevel < (int)mLevels.size());
    return mLevels[(size_t)aLevel];
}

bool MaskPyramid::capture(const gl::Texture& aChain) {
    clear();
    if (aChain.id() == 0 || aChain.size().isEmpty())
        return false;

    const int levels = std::max(1, aChain.mipmapLevels());
    const QSize base = aChain.size();
    auto& ggl = gl::Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, aChain.id());
    ggl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int l = 0; l < levels; ++l) {
        const QSize size = levelSizeOf(base, l);
        std::vector<uint8> bytes((size_t)size.width() * size.height(), 0);
        ggl.glGetTexImage(GL_TEXTURE_2D, l, GL_RED, GL_UNSIGNED_BYTE, bytes.data());
        mSizes.push_back(size);
        mLevels.push_back(std::move(bytes));
    }
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// The box on aDstSize's grid that the damage in aSrcBox reaches: every destination texel whose
// footprint touches the source range, rounded OUTWARDS at both ends, clamped into the grid. The
// same integer rule as MaskMipChain's, so a stroke paints the same texels here and there.
QRect MaskPyramid::coveredBox(const QRect& aSrcBox, const QSize& aSrcSize, const QSize& aDstSize) {
    if (aSrcBox.isEmpty() || aSrcSize.isEmpty() || aDstSize.isEmpty())
        return QRect();
    const auto span = [](int aLo, int aHi, int aSrc, int aDst, int& aFirst, int& aLast) {
        const qint64 src = aSrc;
        const qint64 dst = aDst;
        const int lo = int((qint64(aLo) * dst) / src);
        const int hi = int(((qint64(aHi) * dst) + src - 1) / src) - 1;
        aFirst = std::max(0, std::min(lo, aDst - 1));
        aLast = std::max(0, std::min(hi, aDst - 1));
    };
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    span(aSrcBox.left(), aSrcBox.right() + 1, aSrcSize.width(), aDstSize.width(), x0, x1);
    span(aSrcBox.top(), aSrcBox.bottom() + 1, aSrcSize.height(), aDstSize.height(), y0, y1);
    if (x1 < x0 || y1 < y0)
        return QRect();
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

// One box of one level, the GPU's two-pass rule mirrored: the x pass averages one source row at
// a time and stores the NORMALIZED average in half precision (the chain's R16F scratch), the y
// pass averages those halves and rounds once into R8. Reproducing the half store is what keeps
// the CPU within 1-2 LSB of the GPU chain instead of the 3 a single 2D average gives.
void MaskPyramid::resampleBox(
    const std::vector<uint8>& aSrc, const QSize& aSrcSize, std::vector<uint8>& aDst,
    const QSize& aDstSize, const QRect& aDestBox
) const {
    if (aDestBox.isEmpty() || aSrc.empty() || aDst.empty())
        return;
    const QRect srcBox = coveredBox(aDestBox, aDstSize, aSrcSize);
    if (srcBox.isEmpty())
        return;

    const int xw = aDestBox.width();
    const int sh = srcBox.height();
    std::vector<float> xpass((size_t)xw * (size_t)sh, 0.0f);
    const int xTaps = int(std::ceil(double(aSrcSize.width()) / double(aDstSize.width()))) + 1;
    for (int i = 0; i < xw; ++i) {
        const int g = aDestBox.left() + i;
        const float lo = float(g) * float(aSrcSize.width()) / float(aDstSize.width());
        const float hi = float(g + 1) * float(aSrcSize.width()) / float(aDstSize.width());
        const int first = int(std::floor(lo));
        for (int sy = 0; sy < sh; ++sy) {
            const int row = srcBox.top() + sy;
            float acc = 0.0f;
            float wsum = 0.0f;
            for (int t = 0; t < xTaps; ++t) {
                const int k = first + t;
                if (k < 0 || k >= aSrcSize.width())
                    continue;
                const float w = std::min(float(k + 1), hi) - std::max(float(k), lo);
                if (w <= 0.0f)
                    continue;
                acc += (aSrc[(size_t)row * aSrcSize.width() + k] / 255.0f) * w;
                wsum += w;
            }
            xpass[(size_t)sy * xw + i] = wsum > 0.0f ? float(qfloat16(acc / wsum)) : 0.0f;
        }
    }

    const int yTaps = int(std::ceil(double(aSrcSize.height()) / double(aDstSize.height()))) + 1;
    for (int j = 0; j < aDestBox.height(); ++j) {
        const int g = aDestBox.top() + j;
        const float lo = float(g) * float(aSrcSize.height()) / float(aDstSize.height());
        const float hi = float(g + 1) * float(aSrcSize.height()) / float(aDstSize.height());
        const int first = int(std::floor(lo));
        for (int i = 0; i < xw; ++i) {
            float acc = 0.0f;
            float wsum = 0.0f;
            for (int t = 0; t < yTaps; ++t) {
                const int k = first + t;
                if (k < srcBox.top() || k > srcBox.bottom())
                    continue;
                const float w = std::min(float(k + 1), hi) - std::max(float(k), lo);
                if (w <= 0.0f)
                    continue;
                acc += xpass[(size_t)(k - srcBox.top()) * xw + i] * w;
                wsum += w;
            }
            const float v = wsum > 0.0f ? acc / wsum : 0.0f;
            aDst[(size_t)g * aDstSize.width() + (aDestBox.left() + i)] =
                (uint8)std::lround(std::max(0.0f, std::min(1.0f, v)) * 255.0f);
        }
    }
}

void MaskPyramid::updateBox(const QImage& aField, const QRect& aTexelBox) {
    if (!isValid() || aField.isNull() || aField.format() != QImage::Format_Grayscale8)
        return;
    if (aField.size() != size())
        return;
    const QRect box = aTexelBox.intersected(QRect(QPoint(0, 0), size()));
    if (box.isEmpty())
        return;

    // level 0: the field's own box
    std::vector<uint8>& base = mLevels[0];
    for (int y = box.top(); y <= box.bottom(); ++y)
        std::memcpy(
            base.data() + (size_t)y * mSizes[0].width() + box.left(), aField.constScanLine(y) + box.left(),
            (size_t)box.width()
        );

    QRect prevBox = box;
    QSize prevSize = mSizes[0];
    for (int l = 1; l < (int)mLevels.size(); ++l) {
        const QSize size = mSizes[(size_t)l];
        const QRect destBox = coveredBox(prevBox, prevSize, size);
        if (destBox.isEmpty())
            break;
        resampleBox(mLevels[(size_t)l - 1], prevSize, mLevels[(size_t)l], size, destBox);
        prevBox = destBox;
        prevSize = size;
    }
}

void MaskPyramid::fillPage(int aLevel, int aPageX, int aPageY, uint8* aDst) const {
    XC_PTR_ASSERT(aDst);
    if (aLevel < 0 || aLevel >= (int)mLevels.size())
        return;
    const int pitch = gl::PagePool::kPitch;
    const int gutter = gl::PagePool::kGutter;
    const QSize size = mSizes[(size_t)aLevel];
    const std::vector<uint8>& level = mLevels[(size_t)aLevel];
    const int originX = aPageX * (int)gl::PagePool::kPageSize - gutter;
    const int originY = aPageY * (int)gl::PagePool::kPageSize - gutter;

    for (int y = 0; y < pitch; ++y) {
        const int gy = originY + y;
        uint8* row = aDst + (size_t)y * pitch;
        if (gy < 0 || gy >= size.height()) {
            std::memset(row, 0, (size_t)pitch);
            continue;
        }
        const int x0 = std::max(0, -originX);
        const int x1 = std::min(pitch, size.width() - originX);
        if (x0 > 0)
            std::memset(row, 0, (size_t)x0);
        if (x1 < pitch)
            std::memset(row + x1, 0, (size_t)(pitch - x1));
        if (x1 > x0)
            std::memcpy(row + x0, level.data() + (size_t)gy * size.width() + (originX + x0), (size_t)(x1 - x0));
    }
}

} // namespace core
