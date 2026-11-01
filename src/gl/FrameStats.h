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
//   * dens / dens_max - the DISTRIBUTION of the texel density the deformation produced, not just
//     its worst case. A triangle's density is its deformed content-space area divided by its uv
//     area in content pixels: 1.0 for a layer that is merely scaled uniformly, above 1 where the
//     deformation stretches it, below 1 where it compresses it. The 16 bins are centred on
//     log2(density) = -4, -3.5 ... +3.5 (so a density of 1.0, an undeformed triangle, is in the
//     middle of bin 8; bin 15 is open-ended and takes everything above it) and each holds the
//     SUM OF UV AREAS that fell into it, so an area-weighted percentile is a prefix sum over
//     them - which is what a mip level policy needs, and why a maximum alone is not enough.
//     dens_max is the largest density any triangle of the frame reached. Triangles whose uv
//     area is under a quarter of a pixel are not measured at all (see kMinUvArea in
//     MeshTransformer.cpp). Measured every frame (playback changes the blend and the
//     deformation every frame) by the density pass in MeshTransformer::callGL; see
//     DensityMeter there.
//
// Output: one line per frame appended to the path in AE_FRAME_STATS, plus a final summary line.
class FrameStats {
public:
    // the density histogram's shape (see the header comment); the density pass and this class
    // must agree on it, so both read these. Bins are CENTRED on base + b * step, so the
    // densities this engine actually produces (1.0 for an undeformed triangle) land in the
    // middle of a bin instead of on the boundary between two.
    enum {
        kDensityBins = 16,
    };
    static constexpr float kDensityBinBase = -4.0f; // log2(density) of bin 0's centre
    static constexpr float kDensityBinStep = 0.5f;  // one bin per half octave

    static FrameStats& instance();

    bool enabled() const { return mEnabled; }

    // Advances in beginFrame. The density readback in MeshTransformer owns a GPU-side
    // accumulator that has to be copied out and cleared once per frame, and this is the only
    // frame boundary it can see without a second clock of its own.
    quint64 frameIndex() const { return mFrameIndex; }

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

    // One frame's measured distribution, replacing whatever the previous readback left.
    // `aBins` holds kDensityBins uv-area sums in bin order (bin b covers
    // [kDensityBinBase + b * kDensityBinStep, kDensityBinBase + (b + 1) * kDensityBinStep)
    // in log2 density), `aMaxDensity` the largest density measured (0 when nothing was).
    void densityHistogram(const float* aBins, float aMaxDensity);

    // one line, for a probe or a log
    QString summary() const;

private:
    FrameStats();

    bool mEnabled;
    QString mPath;
    qint64 mFrame = 0;
    // one per beginFrame; see frameIndex()
    quint64 mFrameIndex = 0;
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

    // this frame's density distribution (see the header comment), reset in beginFrame
    double mDensityBins[kDensityBins] = {};
    double mDensityMax = 0.0;
    // the largest density any frame of this run produced
    double mSessionDensityMax = 0.0;
};

} // namespace gl

#endif // GL_FRAMESTATS_H
