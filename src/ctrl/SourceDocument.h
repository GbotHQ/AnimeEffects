#ifndef CTRL_SOURCEDOCUMENT_H
#define CTRL_SOURCEDOCUMENT_H

#include <functional>
#include <memory>
#include <vector>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include "XCType.h"
#include "img/BlendMode.h"
#include "img/PSDFormat.h"
#include "core/BlendIf.h"
#include "core/LayerEffects.h"

namespace img {
class ResourceNode;
}
namespace core {
class FolderNode;
class ObjectNode;
class Project;
}

namespace ctrl {

// A description's byte block, WITH its owner. XCMemBlock is a pair of raw fields (`uint8* data,
// size_t size`) whose memory is `new uint8[]` (img::Buffer::alloc and every reader here allocate
// that way), and nothing in it says who frees it.
//
// Why that mattered: the reload applies a description to the resources and, for an entry it does
// not touch, simply leaves the blocks alone - the document was a stack local in
// gui::res::ResourceUpdater::reconcile, so every reload leaked the pixels AND the mask of every
// unchanged layer (a 2000x2000 layer: 20 MB per reload, for the life of the process). The
// description is the only thing that knows what its readers allocated, so the fix belongs here
// rather than in a cleanup pass at the call sites: the fields own their memory, the document
// frees whatever a caller did not take, and no path has to remember to.
//
// Move-only on purpose: a copy would either double-free or share a buffer with no owner, and
// neither is a thing this description wants - handing pixels over is `take()`. The compiler
// enforces it, including inside the entry vector, so an accidental copy is a build error instead
// of a double free.
class OwnedBlock {
public:
    OwnedBlock() = default;
    OwnedBlock(OwnedBlock&& aRhs) noexcept: mBlock(aRhs.take()) {}
    OwnedBlock& operator=(OwnedBlock&& aRhs) noexcept {
        if (this != &aRhs) {
            free();
            mBlock = aRhs.take();
        }
        return *this;
    }
    OwnedBlock(const OwnedBlock&) = delete;
    OwnedBlock& operator=(const OwnedBlock&) = delete;
    ~OwnedBlock() { free(); }

    // takes a freshly allocated block (freeing whatever this held)
    void adopt(const XCMemBlock& aBlock) {
        free();
        mBlock = aBlock;
    }
    // gives the block up: whoever calls this owns it now
    XCMemBlock take() {
        XCMemBlock block = mBlock;
        mBlock = XCMemBlock();
        return block;
    }
    void free() {
        delete[] mBlock.data;
        mBlock = XCMemBlock();
    }

    const XCMemBlock& block() const { return mBlock; }
    const uint8* data() const { return mBlock.data; }
    size_t size() const { return mBlock.size; }
    bool isEmpty() const { return mBlock.data == nullptr; }
    explicit operator bool() const { return mBlock.data != nullptr; }

private:
    XCMemBlock mBlock;
};

// A format-neutral description of one SOURCE document - a PSD, an ORA, a plain bitmap.
//
// Why it exists: the first import and a source reload used to walk the file format separately,
// by different code, and populated the resource tree differently - one set the layer masks, the
// derivation hook and the derived-channel needs and the other did not, so a resource that came
// from a reload was not the resource the import would have built. Each also kept raw pointers
// back into the parse it came from (the old PSD walker handed the layer record's address to
// `ResourceData::setUserData` and captured it in a lazy image loader, so a node that outlived
// that parse - every node a reload appends - dangled into the next one). Both walkers are gone:
// one description is produced per container format, by the readers below, and consumed by BOTH
// paths. Nothing built from a description keeps a pointer back into it - the pixels and the mask
// are handed over - so a document can be destroyed as soon as its trees are built. Whatever the
// caller did NOT take, the description frees on destruction (see OwnedBlock): "destroyed as soon
// as its trees are built" is therefore also what happens to the memory of the entries a reload
// decided not to apply.
//
// The entries are deliberately the union of what the two trees need - the resource side (name,
// stable id, rect, pixels, mask, blend, visible) and the object side (clipping, opacity, fill
// opacity, Blend If, layer effects) - because that union is exactly what a reload has to
// reconcile and what used to be dropped on the floor.
class SourceDocument {
public:
    struct Entry {
        QString name;                 // display name (Photoshop's `luni`)
        QString stableId;             // Photoshop's `lyid`: empty when the format has none
        bool isLayer = false;         // false = a group/folder
        QRect rect;                   // the resource's rect in DOCUMENT coordinates (the pixels and
                                      // the mask live in it, one transparent border included)
        // The OBJECT node's initial rect, when it is not the resource's. The ORA reader takes a
        // layer's from stack.xml while the resource is the padded bitmap, and a group's from the
        // union of its children; the PSD reader uses one rect for both. It is what the object
        // tree's geometry (and therefore the imported picture) is built from, so it is carried
        // explicitly rather than derived.
        QRect objectInitialRect;
        bool hasObjectInitialRect = false;

        QRect initialRect() const { return hasObjectInitialRect ? objectInitialRect : rect; }
        img::BlendMode blendMode = img::BlendMode_Normal;
        bool visible = true;
        bool clipped = false;         // Photoshop's clipping mask ("clipped to the layer below")
        int opacity255 = 255;         // the layer's opacity, as stored
        int fillOpacity255 = 255;     // Photoshop's fill opacity (`iOpa`)
        // Which of the OPTIONAL object-side properties this entry actually describes. A reader
        // that cannot read them (the ORA and bitmap readers do not read Blend If or layer
        // effects) must say so, because a reload applies what the description says: without this
        // a reader that never saw the file's ranges would look like "the file has none" and the
        // reload would wipe the ones the artist set here. "Detect == apply" holds for what the
        // description knows, and only for that.
        bool hasObjectData = false;
        core::BlendIf blendIf;        // the layer's "Blend If" ranges
        core::LayerEffects effects;   // the layer's Photoshop layer effects (`lfx2`)

        // The same question for the MASK, which is a channel of the RESOURCE and not of the
        // object: did this description actually look at the file's user-mask channel? Only the
        // PSD reader interprets one, and only when it decoded the layer's pixels, so an ORA, a
        // plain bitmap or a structure-only description must say no. Without this the reload read
        // "no mask in the description" as "the file has no mask" and DELETED the mask the artist
        // baked in the app (core/LayerMaskBake writes into this same channel) on every reload of
        // such a file - the destructive version of the mistake hasObjectData exists to prevent.
        // With it, "detect == apply" holds for the mask too: a reader that describes the channel
        // has its description applied, presence and absence included, and one that does not leaves
        // the project's mask alone.
        bool hasMaskData = false;

        // pixels: the layer's image, already assembled in the entry's rect (the importer pads a
        // layer by one transparent pixel and bleeds its colour, see Util::createTextureImage).
        // Empty when the caller asked for a structure-only document; handed over - not shared -
        // when the resource tree is built (see OwnedBlock for why it owns itself).
        OwnedBlock pixels;
        QSize pixelSize;

        // the layer mask (Photoshop's user-mask channel), A8 in the entry's rect, empty when the
        // layer has none
        OwnedBlock mask;
        bool maskEnabled = true;
        bool maskInverted = false;

        std::vector<Entry> children;
    };

    // document-level facts the trees need
    QSize canvasSize;
    QString documentName;
    Entry top;                        // top.children are the document's roots

    // A document that could not be described (an unsupported mode, a layer whose pixels did not
    // decode) says so here rather than building half a tree; the loaders surface it.
    QString error;

    bool isValid() const { return error.isEmpty(); }

    // hands the entry's pixel/mask blocks over (the entry is left empty)
    XCMemBlock takePixels(Entry& aEntry) { return aEntry.pixels.take(); }
};

// ---------------------------------------------------------------------------- readers
// One reader per container format. Each interprets the format completely - names, ids,
// geometry, masks, blend modes, Blend If ranges, layer effects - so that nothing downstream
// has to know which container it came from. `aWantPixels` false produces a structure-only
// document (names, ids, rects, metadata), which is what a caller that only needs to match
// identities can ask for.

// PSD/PSB. The container is read by img::PSDReader; this is the interpretation. `aProgress` is
// called with (done, total) as the layer records are described, when it is set.
SourceDocument describePsd(
    const img::PSDFormat& aFormat, const QString& aDocumentName, bool aWantPixels,
    const std::function<void(int, int)>& aProgress = std::function<void(int, int)>()
);

// OpenRaster. `aMerged` describes the merged image as one layer, otherwise the stack from
// stack.xml (folders and layers, with their blend modes, opacities and visibilities). The ORA
// format carries no Blend If ranges, layer effects or masks, so those entries say
// hasObjectData = false.
SourceDocument describeOra(
    const std::string& aFilePath, bool aMerged, const QString& aDocumentName, bool aWantPixels,
    const std::function<void(int, int)>& aProgress = std::function<void(int, int)>()
);

// A plain bitmap: one layer, filling the document.
SourceDocument describeImage(const QImage& aImage, const QString& aDocumentName, bool aWantPixels);

// ---------------------------------------------------------------------------- builders
// The two trees, each built from a description by ONE walker. The resource tree is shared by
// the first import and by a reload, so both see the same population; the object tree is the
// import's alone (a reload reconciles the existing object nodes through the resource events).

// Takes ownership of the entries' pixel and mask blocks (see Entry). Entries without pixels
// (a structure-only document) produce resources with no image, which is what a caller that
// only matches identities wants.
img::ResourceNode* buildResourceTree(SourceDocument& aDocument);

// ONE entry -> ONE resource node, with the same population buildResourceTree gives it (mask,
// derivation hook, declared derived-channel needs). Both go through this, so a resource a
// reload appends is exactly the resource the import would have built.
img::ResourceNode* buildResourceNode(SourceDocument& aDocument, SourceDocument::Entry& aEntry);

// ---------------------------------------------------------------- reconciliation
// The identity rule a reload matches with, in one place: the kind must agree, the stable id
// decides when BOTH sides have one, otherwise the name - i.e. the same policy
// img::ResourceData::matchesIdentity applies between two resources (see the S21 suite), asked
// here between a resource in the project and an entry in the file.
bool matchesIdentity(const img::ResourceNode& aNode, const SourceDocument::Entry& aEntry);

// What a resource in the project and an entry in the file disagree about. This is the "detect"
// side of a reload's contract: whatever shows up here has to be APPLIED (img::ResourceNode's
// content update), and nothing may be applied that is not here.
struct ResourceDiff {
    bool pixels = false;      // the image (or its rect) differs
    bool mask = false;        // the mask, its flags, or its presence differs
    bool blend = false;       // the blend mode differs
    bool visible = false;     // the resource's visibility flag differs
    bool name = false;        // the display name differs (reported, not applied - see the header
                              // of ResourceNode::Content)
    bool any() const { return pixels || mask || blend || visible; }
    bool anyReported() const { return name; }
};
ResourceDiff diffResource(const img::ResourceNode& aNode, const SourceDocument::Entry& aEntry);

// The same contract for the OBJECT side (the node's own properties, which live on the node and
// not on the resource): what the node and the file disagree about.
struct ObjectDiff {
    bool blend = false;        // the node's own blend mode (the ImageKey's, which the renderer reads)
    bool blendIf = false;
    bool effects = false;      // layers only: folders carry no layer effects
    bool fillOpacity = false;
    bool opacity = false;
    bool visibility = false;
    bool clipping = false;
    bool any() const { return blend || blendIf || effects || fillOpacity || opacity || visibility || clipping; }
};
ObjectDiff diffObject(const core::ObjectNode& aNode, const SourceDocument::Entry& aEntry);

// Creates the object tree under aTopNode, linking each layer to its resource by walking the
// resource tree in the same order. The document must have been built first.
void buildObjectTree(
    const SourceDocument& aDocument, core::FolderNode& aTopNode, const img::ResourceNode& aResourceTree,
    core::Project& aProject
);

// One line per layer whose effects this build does not draw as the file configures them, for
// the import log and the reload log. It exists because a report nobody calls is not a report:
// core::LayerEffects::isDrawnAsConfigured() is the fact, and this is the sentence, so the
// import path, the reload path and the tools all say the same thing instead of each deciding
// whether to mention it (an earlier revision of this file answered the predicate only in the
// test harness, so an imported bevel was silently missing from the picture).
// The lines are stable and human-readable; an empty list means the file is drawn as configured.
QStringList undrawnEffectNotes(const SourceDocument& aDocument);

// the object tree's top node, with the canvas rect as its initial rect
core::FolderNode* createTopNode(const QString& aName, const QRect& aInitialRect);

// the bounding rect of a folder's children - the rule every reader uses to close a group
QRect calculateBoundingRectFromChildren(const core::FolderNode& aNode);

// sets every node's default posture from its initial rect (the import's last step)
void setDefaultPosturesFromInitialRects(core::FolderNode& aTopNode);

} // namespace ctrl

#endif // CTRL_SOURCEDOCUMENT_H
