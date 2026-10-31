#ifndef GL_FRAMESTATS_H
#define GL_FRAMESTATS_H

#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>

namespace gl {

// WHAT A FRAME COSTS, counted where the engine spends it.
//
// Every hypothesis about this renderer - "tiles would help", "culling the layer is the win",
// "the masks are what fills VRAM" - is an argument until these numbers exist. The counter is OFF
// unless `AE_FRAME_STATS=<path>` is set, and each call is one predictable branch, so a release
// build pays nothing for it.
//
// What is counted, and what each number means:
//   * layers - considered / drawn / culled. "Culled" counts the ones a bounds test rejects; with
//     no bounds test in the engine yet it stays 0, which is itself the finding.
//   * fill_px - the sum of the drawn layers' SCREEN-SPACE quad areas. That is the fill the frame
//     asks for (each screen pixel under a layer is a sample), not a GPU counter: the engine targets
//     GL 3.3 core, where per-draw sample queries are not available portably.
//   * tex_live_bytes / tex_peak_bytes - the bytes of every gl::Texture ever created minus the ones
//     destroyed: the texture residency the process is holding right now.
//   * mask_bytes - the RAM of the painted fields (core::MaskKey / core::FilterMask), which is not
//     a texture until it is uploaded.
//
// Output: one line per frame appended to the path in AE_FRAME_STATS, plus a final summary line.
class FrameStats {
public:
    static FrameStats& instance();

    bool enabled() const { return mEnabled; }

    // the viewport's frame
    void beginFrame();
    void endFrame(int aViewWidth, int aViewHeight);

    void layerConsidered() { if (mEnabled) ++mLayersConsidered; }
    void layerCulled() { if (mEnabled) ++mLayersCulled; }
    void layerDrawn(double aScreenAreaPx);
    // one transform-feedback vertex pass: the skinning every layer re-runs per painted frame,
    // even when only the camera moved (see MeshTransformer)
    void meshPass() { if (mEnabled) ++mMeshPasses; }
    void textureCreated(qint64 aBytes);
    void textureDestroyed(qint64 aBytes);
    void maskFieldBytes(qint64 aBytes) { if (mEnabled) mMaskBytes = aBytes; }

    // one line, for a probe or a log
    QString summary() const;

private:
    FrameStats();

    bool mEnabled;
    QString mPath;
    qint64 mFrame = 0;
    int mLayersConsidered = 0;
    int mLayersDrawn = 0;
    int mLayersCulled = 0;
    int mMeshPasses = 0;
    double mFillPx = 0.0;
    QElapsedTimer mFrameTimer;
    qint64 mLiveTextureBytes = 0;
    qint64 mPeakTextureBytes = 0;
    qint64 mMaskBytes = 0;
    qint64 mFramesWritten = 0;
};

} // namespace gl

#endif // GL_FRAMESTATS_H
