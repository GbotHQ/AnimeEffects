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
    // which is authored data that simply travels with it). The renderer's consumers - Photoshop's
    // Stroke and Bevel/Emboss both need a distance from the layer's outline - declare that they
    // want one; the resource carries the plane, the image revision and the recipe it was built
    // from, and recomputes it when any of those change.
    //
    // Why the bookkeeping is explicit:
    //   - WANTED vs PRESENT. Inferring the need from "a plane exists" cannot express "wanted but
    //     absent" (a project written before the plane existed, or a derivation that failed), so
    //     the consumer could silently get no effect at all. The flag is persisted, so a project
    //     knows what it needs before it has it.
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
        bool wanted = false;              // a consumer declared it needs this channel
        Buffer buffer;                    // the plane itself (empty = not built)
        int sourceImageRevision = -1;     // the image revision it was measured from
        int recipe = 0;                   // the recipe it was built with
        int revision = 0;                 // bumped when the plane changes (the GPU cache keys on it)

        bool isPresent() const { return buffer.data() != nullptr; }
    };

    // The computation itself, when one is registered (see setDeriver): the resource cannot
    // compute a derived channel on its own - the algorithms live above img - so the owner of the
    // pixels installs a callback that fills whatever is wanted and stale. It runs after every
    // pixel change (grabImage) and when a need is first declared, which is why no render-time
    // CPU work is needed anywhere.
    typedef std::function<void(ResourceData&)> Deriver;
    void setDeriver(const Deriver& aDeriver) { mDeriver = aDeriver; }
    // runs the registered deriver for every wanted-and-stale channel
    void deriveChannels();

    void setDerivationWanted(Derivation aKind, bool aWanted);
    bool isDerivationWanted(Derivation aKind) const { return mDerived[aKind].wanted; }
    const DerivedChannel& derived(Derivation aKind) const { return mDerived[aKind]; }
    // true when a wanted channel is missing, measured from other pixels, or built by another
    // recipe: a consumer must not use it (and the renderer reports instead of drawing with it)
    bool isDerivationStale(Derivation aKind) const;
    void grabDerived(Derivation aKind, const XCMemBlock& aBlock, const QSize& aSize);
    void freeDerived(Derivation aKind);
    int derivedRevision(Derivation aKind) const { return mDerived[aKind].revision; }

    // Restores what a saved file recorded about its derived channels: which are wanted, and the
    // recipe each carried plane was built with. Called by the reader AFTER the planes are read,
    // so the resource's own state (present + source revision) is not disturbed; a wanted channel
    // with no plane is simply stale, and the resource derives it on the next pixel change or when
    // deriveChannels() is asked.
    void restoreDerivationState(int aWantedFlags, const int* aRecipes);
    // the distance channel, named for its consumer (the stroke pass)
    void grabDistance(const XCMemBlock& aBlock, const QSize& aSize) {
        grabDerived(Derivation_Distance, aBlock, aSize);
    }
    bool hasDistance() const { return mDerived[Derivation_Distance].isPresent(); }
    const Buffer& distance() const { return mDerived[Derivation_Distance].buffer; }
    int distanceRevision() const { return derivedRevision(Derivation_Distance); }
    bool isDistanceStale() const { return isDerivationStale(Derivation_Distance); }

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
    void copyFrom(const ResourceData& aData);

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
