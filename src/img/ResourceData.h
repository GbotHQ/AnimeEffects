#ifndef IMG_RESOURCEDATA_H
#define IMG_RESOURCEDATA_H

#include <functional>
#include <QPoint>
#include <QString>
#include "img/Buffer.h"
#include "img/BlendMode.h"
namespace img {
class ResourceNode;
}

namespace img {

class ResourceData {
public:
    typedef std::function<bool(ResourceData& aData)> ImageLoader;

    ResourceData(const QString& aIdentifier, const ResourceNode* aSerialAddress);
    virtual ~ResourceData() {}

    void grabImage(const XCMemBlock& aBlock, const QSize& aSize, Format aFormat);
    XCMemBlock releaseImage();
    void freeImage();

    // Layer mask (Photoshop's user-mask channel). Stored per resource, like the image, and
    // in the IMAGE's rect/coordinates (the importer resamples a mask with its own rect into
    // the layer rect and fills the remainder with the mask's default color), so the shader
    // can sample it with the image's texture coordinates. Format_A8, one byte per pixel.
    void grabMask(const XCMemBlock& aBlock, const QSize& aSize);
    XCMemBlock releaseMask();
    void freeMask();
    bool hasMask() const { return mMask.data(); }
    const Buffer& mask() const { return mMask; }
    // a disabled mask stays on the layer (Photoshop keeps the pixels) but is not applied
    void setMaskEnabled(bool aEnabled) { mMaskEnabled = aEnabled; }
    bool isMaskEnabled() const { return mMaskEnabled; }
    // an inverted mask renders 255 - value
    void setMaskInverted(bool aInverted) { mMaskInverted = aInverted; }
    bool isMaskInverted() const { return mMaskInverted; }
    // bumped whenever the mask PIXELS change: the render path rebuilds the GPU copy when it
    // sees a newer revision, so editing commands need no GL context of their own
    int maskRevision() const { return mMaskRevision; }

    // ---------------------------------------------------------------- DERIVED CHANNELS
    // A derived channel is a per-resource plane computed FROM the image (as opposed to the mask,
    // which is authored data that simply travels with it). Photoshop's Stroke and Bevel/Emboss are
    // both functions of a distance from the layer's outline, so the plane is a CACHE OF A PURE
    // FUNCTION of the pixels, and that is the whole model:
    //
    //   - NOTHING DECLARES A NEED. A consumer that wants the plane asks for it (derived()); the
    //     resource derives one if it has none, or if the one it has was measured from other pixels
    //     or built by another recipe. There is no "wanted" flag, because a want is a fact about a
    //     CONSUMER: two objects can share one resource (so the flag is really a union nobody
    //     reference-counts), it changes when an effect is switched on or off or undone, and it was
    //     persisted with the project - three ways for the resource's copy of somebody else's
    //     configuration to be wrong. An earlier revision had that flag, and a source reload that
    //     gave a layer a stroke left the resource never asked again: the stroke silently did not
    //     draw. Asking is not something a caller can forget to do.
    //   - PRESENT vs ABSENT is therefore an outcome, not a state: a plane that cannot be computed
    //     (no pixels in a slimmed project, a shape with no outline) stays absent, the deriver says
    //     so in the log, and the consumer draws nothing instead of something invented.
    //   - RECIPE. The plane's meaning depends on how it was computed (the outline threshold, and
    //     anything a future version adds - a signed/outside distance, a different metric). A
    //     recipe number means a change invalidates old planes instead of silently consuming them.
    //   - SOURCE REVISION. Every pixel replacement goes through grabImage, which bumps the image
    //     revision, so a plane measured from other pixels is recognisably stale.
    enum Derivation {
        Derivation_Distance, // A8 outline distance (core/DistanceField.h): 0 outside the shape,
                             // else round(pixels) to the nearest outside pixel, saturated at 255
        Derivation_TERM
    };

    // how a channel is computed: a per-kind recipe number, so a change in the computation
    // invalidates every plane built by the previous one
    static int derivationRecipe(Derivation aKind);
    static const char* derivationName(Derivation aKind);

    struct DerivedChannel {
        Buffer buffer;                    // the plane itself (empty = not built, or not computable)
        int sourceImageRevision = -1;     // the image revision it was measured from
        int recipe = 0;                   // the recipe it was built with
        int revision = 0;                 // bumped when the plane changes (the GPU cache keys on it)

        bool isPresent() const { return buffer.data() != nullptr; }
    };

    // The computation itself, when one is registered (see setDeriver): the resource cannot
    // compute a derived channel on its own - the algorithms live above img - so the owner of the
    // pixels installs a callback that fills whatever a consumer asks for and is not current. It
    // runs from derived(), i.e. inside the caller that wants the plane (the render path for a
    // stroke), which is the only place that knows it is about to be used.
    typedef std::function<void(ResourceData&)> Deriver;
    void setDeriver(const Deriver& aDeriver) { mDeriver = aDeriver; }

    // THE demand: returns the plane for aKind, deriving it first when the resource has none or the
    // one it has does not describe the current pixels and recipe. Non-const for that reason; call
    // it from a context that may do CPU work (the render path already does its GPU uploads there).
    const Buffer& derived(Derivation aKind);
    // true when a plane is present but does not describe the current pixels or recipe: derived()
    // never returns one of these, so this is for reporting, not for deciding
    bool isDerivationStale(Derivation aKind) const;
    // what the cache holds right now, WITHOUT deriving: for a caller that reports state instead of
    // consuming it (the project writer, which must not compute anything while saving)
    const DerivedChannel& cached(Derivation aKind) const { return mDerived[aKind]; }
    // how many times the plane has changed (the GPU cache keys on it)
    int derivedRevision(Derivation aKind) const { return mDerived[aKind].revision; }
    void grabDerived(Derivation aKind, const XCMemBlock& aBlock, const QSize& aSize, int aRecipe);
    void freeDerived(Derivation aKind);

    // the distance channel, named for its consumer (the stroke pass)
    const Buffer& distance() { return derived(Derivation_Distance); }
    void grabDistance(const XCMemBlock& aBlock, const QSize& aSize, int aRecipe) {
        grabDerived(Derivation_Distance, aBlock, aSize, aRecipe);
    }
    int distanceRevision() const { return derivedRevision(Derivation_Distance); }

    int imageRevision() const { return mImageRevision; }

    void setIdentifier(const QString& aId) { mIdentifier = aId; }

    // A STABLE identity, when the source format provides one: Photoshop writes a "lyid" layer id
    // that survives renames and is unique within its document. The identifier above is a display
    // name and only a fallback key - it changes when the artist renames a layer, and two layers
    // may share one - so a reload that matched on it alone lost correspondence on a rename and
    // refused to proceed on a duplicate. An empty stable id means "no id available" (a project
    // written before ids existed, or a format that has none), where the name is all there is.
    void setStableId(const QString& aId) { mStableId = aId; }
    const QString& stableId() const { return mStableId; }

    // true when this resource and aRhs are the same resource as far as a source reload is
    // concerned: the kind must agree, the stable id decides when BOTH sides have one, and
    // otherwise the display name is used (the pre-id behaviour). This is the whole identity
    // policy, in one place, so the reload path and the "is this a duplicate sibling?" check
    // cannot drift apart.
    bool matchesIdentity(const ResourceData& aRhs) const;
    void setPos(const QPoint& aPos);
    void setUserData(void* aData) { mUserData = aData; }
    void setIsLayer(bool aIsLayer) { mIsLayer = aIsLayer; }
    void setBlendMode(BlendMode aMode);
    void setIsVisible(bool aIsVisible) { mIsVisible = aIsVisible; }

    bool isLayer() const { return mIsLayer; }
    bool isVisible() const {return mIsVisible; }
    bool hasImage() const { return mBuffer.data(); }
    const QString& identifier() const { return mIdentifier; }
    const img::Buffer& image() const { return mBuffer; }
    const QPoint& pos() const { return mPos; }
    void* userData() const { return mUserData; }
    BlendMode blendMode() const { return mBlendMode; }
    const ResourceNode* serialAddress() const { return mSerialAddress; }

    void setImageLoader(const ImageLoader& aLoader) { mImageLoader = aLoader; }
    bool loadImage() { return (mImageLoader && mImageLoader(*this)); }

    QRect rect() const;
    QVector2D center() const;

private:
    // runs the registered deriver for every channel that is not current
    void deriveChannels();

    img::Buffer mBuffer;
    img::Buffer mMask;
    bool mMaskEnabled;
    bool mMaskInverted;
    int mMaskRevision;
    DerivedChannel mDerived[Derivation_TERM];
    Deriver mDeriver;
    int mImageRevision;
    QPoint mPos;
    void* mUserData;
    bool mIsLayer;
    QString mIdentifier;
    QString mStableId;
    BlendMode mBlendMode;
    bool mIsVisible;
    ImageLoader mImageLoader;
    const ResourceNode* mSerialAddress;
};

} // namespace img

#endif // IMG_RESOURCEDATA_H
