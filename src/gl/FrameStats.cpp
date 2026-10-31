#include "gl/FrameStats.h"

#include <QFile>
#include <QtGlobal>

#include <cstdio>

namespace gl {

FrameStats& FrameStats::instance() {
    static FrameStats stats;
    return stats;
}

FrameStats::FrameStats() {
    const QByteArray path = qgetenv("AE_FRAME_STATS");
    mPath = QString::fromUtf8(path);
    mEnabled = !mPath.isEmpty();
}

void FrameStats::beginFrame() {
    if (!mEnabled)
        return;
    mLayersConsidered = 0;
    mLayersDrawn = 0;
    mLayersCulled = 0;
    mMeshPasses = 0;
    mFillPx = 0.0;
    mFrameTimer.start();
}

void FrameStats::layerDrawn(double aScreenAreaPx) {
    if (!mEnabled)
        return;
    ++mLayersDrawn;
    mFillPx += aScreenAreaPx;
}

void FrameStats::textureCreated(qint64 aBytes) {
    if (!mEnabled)
        return;
    mLiveTextureBytes += aBytes;
    mPeakTextureBytes = qMax(mPeakTextureBytes, mLiveTextureBytes);
}

void FrameStats::textureDestroyed(qint64 aBytes) {
    if (!mEnabled)
        return;
    mLiveTextureBytes -= aBytes;
    if (mLiveTextureBytes < 0)
        mLiveTextureBytes = 0;
}

void FrameStats::endFrame(int aViewWidth, int aViewHeight) {
    if (!mEnabled)
        return;
    ++mFrame;
    QFile file(mPath);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    // one line per frame: a probe can diff two frames or average a run without a parser
    const QString line = QStringLiteral(
        "frame=%1 ms=%2 view=%3x%4 layers=%5/%6/%7 mesh=%8 fill_px=%9 tex_live=%10 tex_peak=%11 mask_bytes=%12\n"
    ).arg(mFrame)
         .arg(mFrameTimer.isValid() ? mFrameTimer.nsecsElapsed() / 1000000.0 : 0.0, 0, 'f', 2)
         .arg(aViewWidth)
         .arg(aViewHeight)
         .arg(mLayersConsidered)
         .arg(mLayersDrawn)
         .arg(mLayersCulled)
         .arg(mMeshPasses)
         .arg(qint64(mFillPx))
         .arg(mLiveTextureBytes)
         .arg(mPeakTextureBytes)
         .arg(mMaskBytes);
    file.write(line.toUtf8());
    ++mFramesWritten;
}

QString FrameStats::summary() const {
    return QStringLiteral(
        "frames=%1 tex_live=%2 tex_peak=%3 mask_bytes=%4"
    ).arg(mFramesWritten).arg(mLiveTextureBytes).arg(mPeakTextureBytes).arg(mMaskBytes);
}

} // namespace gl
