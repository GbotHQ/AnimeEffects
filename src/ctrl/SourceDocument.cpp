#include <algorithm>
#include <cmath>
#include <QCoreApplication>
#include <QImage>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "util/TextUtil.h"
#include "util/MathUtil.h"
#include "img/PSDDescriptor.h"
#include "img/PSDUtil.h"
#include "img/ResourceNode.h"
#include "img/Util.h"
#include "img/oraParser.h"
#include "deps/zip_file.h"
#include "core/DistanceField.h"
#include "core/FolderNode.h"
#include "core/LayerNode.h"
#include "core/ObjectNode.h"
#include "core/ObjectNodeUtil.h"
#include "core/OpaKey.h"
#include "core/Project.h"
#include "core/Renderer.h"
#include "core/TimeKey.h"
#include "core/TimeLine.h"
#include "gl/Global.h"
#include "ctrl/SourceDocument.h"

namespace ctrl {

namespace {

using img::PSDFormat;

// PSD layer mask (the user-mask channel, id -2) -> an A8 plane in the LAYER's rect.
// The mask has its own rect (it can be smaller or larger than the layer) and a default
// value for the area it does not cover, so the plane is assembled here: pixels inside the
// mask rect are decoded, the rest take the record's default color. Photoshop's mask flags
// ride along (bit 1 = the mask is disabled, bit 2 = invert when rendering). Real (vector)
// masks are not modelled - only the user mask channel is.
bool loadLayerMask(
    const PSDFormat::Layer& aLayer, const QRect& aLayerRect, XCMemBlock& aOutBlock, bool& aOutEnabled,
    bool& aOutInverted
) {
    if (aLayerRect.isEmpty() || !aLayer.mask) {
        return false;
    }

    const PSDFormat::Channel* maskChannel = nullptr;
    for (const auto& channel : aLayer.channels) {
        if (channel->id == -2) {
            maskChannel = channel.get();
            break;
        }
    }
    if (!maskChannel || !maskChannel->data || maskChannel->dataLength == 0) {
        return false;
    }

    // the mask plane's own rect, clamped to the layer rect (negative edges are legal in PSD)
    const auto& maskRect = aLayer.mask->rect;
    const QRect planeRect(
        QPoint(maskRect.left(), maskRect.top()), QSize(maskRect.width(), maskRect.height()));
    if (planeRect.isEmpty()) {
        return false;
    }

    // decode the plane (raw or PackBits RLE) into a source buffer
    std::vector<uint8> plane((size_t)planeRect.width() * planeRect.height(), 0);
    if (maskChannel->compressionId == 0) {
        if (maskChannel->dataLength < plane.size()) {
            return false;
        }
        std::memcpy(plane.data(), maskChannel->data.get(), plane.size());
    } else if (maskChannel->compressionId == 1) {
        if (!img::PSDUtil::decodePlanePackBits(
                plane.data(), plane.size(), maskChannel->data.get(), maskChannel->dataLength, planeRect.width(),
                planeRect.height(), 1)) {
            return false;
        }
    } else {
        return false;
    }

    // assemble the layer-rect plane: the mask where it covers, the default color elsewhere
    const uint8 defaultColor = aLayer.mask->defaultColor;
    auto* dst = new uint8[(size_t)aLayerRect.width() * aLayerRect.height()];
    std::memset(dst, defaultColor, (size_t)aLayerRect.width() * aLayerRect.height());
    const QRect overlap = aLayerRect.intersected(planeRect);
    for (int y = overlap.top(); y <= overlap.bottom(); ++y) {
        const uint8* src = plane.data() + ((size_t)(y - planeRect.top()) * planeRect.width()
            + (overlap.left() - planeRect.left()));
        uint8* row = dst + ((size_t)(y - aLayerRect.top()) * aLayerRect.width()
            + (overlap.left() - aLayerRect.left()));
        std::memcpy(row, src, (size_t)overlap.width());
    }

    aOutBlock = XCMemBlock(dst, (size_t)aLayerRect.width() * aLayerRect.height());
    aOutEnabled = (aLayer.mask->flags & 0x02) == 0;  // bit 1 set = mask disabled
    aOutInverted = (aLayer.mask->flags & 0x04) != 0; // bit 2 set = invert when rendering
    return true;
}

// Photoshop "Blend If" -> core::BlendIf. The PSD record's blending-ranges section stores
// the source ("This Layer") pair first, then the destination ("Underlying Layer") pair, 4
// bytes each in black-lo, black-hi, white-lo, white-hi order (verified against a
// Photoshop-authored file: an independent compositor using this reading reproduces the
// file's own stored composite at mean 8.4/255, the swapped-white reading gives 19.9 and
// ignoring the ranges 42.4).
//
// The section continues with one more pair per channel (the R/G/B tabs of Photoshop's
// dialog). Those are NOT modelled: the composite range is used for every channel. That is
// faithful whenever the per-channel pairs are left at the full range, which is what the
// 114 MB test file does: its 7 layers that edit the composite range all leave their channel
// pairs at the default (0, 0, 255, 255), which the probe's layer dump prints - and a file that
// does edit one is reported rather than silently drawn with the composite range.
core::BlendIf toBlendIf(const PSDFormat::Layer& aLayer) {
    core::BlendIf blendIf;
    const auto* range = aLayer.compositeBlendingRange.get();
    if (!range || !range->isValid) {
        return blendIf;
    }
    blendIf.src = core::BlendIf::Range{range->src[0], range->src[1], range->src[2], range->src[3]};
    blendIf.dst = core::BlendIf::Range{range->dst[0], range->dst[1], range->dst[2], range->dst[3]};

    for (const auto& channel : aLayer.channels) {
        const auto& ch = channel->blendingRange;
        const bool sameSrc = ch.src[0] == range->src[0] && ch.src[1] == range->src[1]
            && ch.src[2] == range->src[2] && ch.src[3] == range->src[3];
        const bool sameDst = ch.dst[0] == range->dst[0] && ch.dst[1] == range->dst[1]
            && ch.dst[2] == range->dst[2] && ch.dst[3] == range->dst[3];
        if (!sameSrc || !sameDst) {
            XC_REPORT() << "layer '" << aLayer.name.c_str()
                        << "' sets a per-channel Blend If range; only the composite range is applied";
            break;
        }
    }
    return blendIf;
}

// Photoshop layer effects ("lfx2"): the block is a DescriptorBlock2, so its descriptor starts
// 8 bytes in (version + data version). Effects this build cannot draw are kept in the model's
// `seen` flags and reported by the caller.
core::LayerEffects toLayerEffects(const PSDFormat::Layer& aLayer) {
    core::LayerEffects effects;
    for (const auto& info : aLayer.additionalInfos) {
        if (info->key != "lfx2" || !info->data || info->dataLength == 0) {
            continue;
        }
        img::PSDDescriptor descriptor;
        if (!descriptor.parse(info->data.get(), info->dataLength)) {
            XC_REPORT() << "lfx2 block of layer '" << aLayer.name.c_str() << "' did not parse";
            continue;
        }
        core::parseLayerEffects(descriptor, effects);
    }
    return effects;
}

// Photoshop's fill opacity ("iOpa"): it scales the layer's own pixels without touching the
// layer effects, so it is kept as its own value on the node (LayerNode::setFillOpacity) and
// applied by the layer shader as uFillOpacity. Folding it into the layer opacity would be
// wrong, and visibly so: this file's 'title copy 7' has iOpa=0 with an outer glow, i.e. a
// glow-only layer whose fill is invisible but whose effect is not.
//
// Encoding: the block is undocumented (Adobe's spec does not list it; psd-tools calls it
// "Undocumented" and reads it as a single byte). The first byte holds the opacity — which is
// also what a little-endian 0..255 word gives, since Photoshop pads the rest with zeros.
// Evidence available here: scanning 15 PSDs on this machine (including 114 MB and 4 MB
// documents) found only the words ff000000 (255) and 00000000 (0), so no file distinguishes
// "first byte" from "little-endian word"; both readings agree on every observed value and on
// the value Photoshop would write for any 0..255 opacity. Bytes 1..3 are therefore expected
// to be zero, and a non-zero tail is logged rather than guessed at.
int toFillOpacity255(const PSDFormat::Layer& aLayer) {
    for (const auto& info : aLayer.additionalInfos) {
        if (info->key != "iOpa" || info->dataLength < 4) {
            continue;
        }
        if (info->data[1] != 0 || info->data[2] != 0 || info->data[3] != 0) {
            XC_REPORT() << "iOpa of layer '" << aLayer.name.c_str()
                        << "' has a non-zero tail; reading the first byte";
        }
        return (int)info->data[0];
    }
    return 255; // no iOpa block: fully opaque fill, Photoshop's default
}

// a layer entry with its pixels (when they are wanted) and its mask
SourceDocument::Entry describeLayer(
    const PSDFormat::Header& aHeader, const PSDFormat::Layer& aLayer, const QString& aName, bool aWantPixels,
    QString& aOutError
) {
    SourceDocument::Entry entry;
    entry.name = aName;
    entry.stableId = img::psdLayerStableId(aLayer);
    entry.isLayer = true;
    entry.blendMode = img::getBlendModeFromPSD(aLayer.blendMode, img::psdHasCspTslyFlag(aLayer));
    entry.visible = aLayer.isVisible();
    entry.clipped = aLayer.clipping != 0;
    entry.opacity255 = aLayer.opacity;
    entry.fillOpacity255 = toFillOpacity255(aLayer);
    entry.hasObjectData = true;
    entry.blendIf = toBlendIf(aLayer);
    entry.effects = toLayerEffects(aLayer);
    entry.rect = QRect(aLayer.rect.left(), aLayer.rect.top(), aLayer.rect.width(), aLayer.rect.height());

    if (!aWantPixels) {
        return entry;
    }

    // the pixels, assembled in the layer's own (padded) rect
    auto imagePair = img::Util::createTextureImage(aHeader, aLayer);
    entry.rect = imagePair.second;
    entry.pixels.adopt(imagePair.first);
    entry.pixelSize = imagePair.second.size();
    if (entry.pixels.isEmpty()) {
        aOutError = QString("Failed to decode the image data of a layer ('%1').").arg(aName);
        return entry;
    }

    // the layer mask, in the same rect
    XCMemBlock maskBlock;
    bool maskEnabled = true;
    bool maskInverted = false;
    if (loadLayerMask(aLayer, entry.rect, maskBlock, maskEnabled, maskInverted)) {
        entry.mask.adopt(maskBlock);
        entry.maskEnabled = maskEnabled;
        entry.maskInverted = maskInverted;
    }
    // the channel has now been consulted, whether or not it held anything: this file has a user
    // mask on this layer or it does not, and that is a fact the reload may act on (see the field's
    // note in the header - it sits here, not with the other PSD facts above, because a
    // structure-only description never looks at the channel and must not claim to)
    entry.hasMaskData = true;
    return entry;
}

// a group entry (no pixels of its own; its rect is the union of its children's, filled in by
// the walk as the group closes)
SourceDocument::Entry describeFolder(const PSDFormat::Layer& aLayer, const QString& aName) {
    SourceDocument::Entry entry;
    entry.name = aName;
    entry.stableId = img::psdLayerStableId(aLayer);
    entry.isLayer = false;
    entry.visible = aLayer.isVisible();
    entry.clipped = aLayer.clipping != 0;
    entry.opacity255 = aLayer.opacity;
    entry.hasObjectData = true;
    entry.blendMode = img::getBlendModeFromPSD(aLayer.blendMode, img::psdHasCspTslyFlag(aLayer));
    entry.blendIf = toBlendIf(aLayer);
    // a group has no pixels of its own, so no fill opacity either (Photoshop does not write
    // iOpa for a section divider); its rect is replaced by the union of its children's when the
    // group closes
    entry.rect = QRect(aLayer.rect.left(), aLayer.rect.top(), aLayer.rect.width(), aLayer.rect.height());
    return entry;
}

// the flattened case: a PSD with no layer records holds the whole bitmap in the merged image
// data section
SourceDocument::Entry describeMerged(const PSDFormat::Header& aHeader, const PSDFormat::ImageData& aImageData,
                                    const QSize& aCanvasSize, const QString& aName) {
    // The merged alpha channel (id -1) is only meaningful when the file declares
    // transparency (negative layer count). Flattened RGB files carry a dummy zero
    // alpha that would blank the content (and its white-matting would destroy the
    // RGB), so drop it and import the bitmap as opaque.
    PSDFormat::ChannelList channels;
    for (const PSDFormat::ChannelPtr& channel : aImageData.channels) {
        if (aImageData.hasTransparency || channel->id >= 0) {
            auto copy = new PSDFormat::Channel();
            copy->id = channel->id;
            copy->compressionId = channel->compressionId;
            copy->dataLength = channel->dataLength;
            if (channel->dataLength > 0) {
                copy->data.reset(new uint8[channel->dataLength]);
                std::memcpy(copy->data.get(), channel->data.get(), channel->dataLength);
            }
            channels.push_back(PSDFormat::ChannelPtr(copy));
        }
    }

    SourceDocument::Entry entry;
    entry.name = aName;
    entry.isLayer = true;

    auto mem = img::PSDUtil::makeInterleavedImage(
        aHeader, channels, img::PSDUtil::ColorFormat_RGBA8, aCanvasSize.width(), aCanvasSize.height());
    if (!mem.data) {
        return entry; // no pixels: the caller reports it
    }
    mem = img::Util::recreateForBiLinearSampling(mem, aCanvasSize);
    entry.rect = QRect(QPoint(-1, -1), aCanvasSize + QSize(2, 2));
    entry.pixels.adopt(mem);
    entry.pixelSize = entry.rect.size();
    return entry;
}

} // namespace

//-------------------------------------------------------------------------------------------------
SourceDocument describeImage(const QImage& aImage, const QString& aDocumentName, bool aWantPixels) {
    SourceDocument document;
    document.documentName = aDocumentName;
    document.canvasSize = aImage.size();
    document.top.name = "topnode";
    document.top.isLayer = false;
    document.top.rect = QRect(QPoint(), document.canvasSize);

    SourceDocument::Entry entry;
    entry.name = aDocumentName;
    entry.isLayer = true;
    // A bitmap has no layers, no masks and no effects: the one entry it describes is a Norma,
    // fully visible layer. hasObjectData stays false - there is nothing in the file to compare a
    // node's Blend If or effects against.
    entry.blendMode = img::BlendMode_Normal;
    entry.visible = true;

    if (aWantPixels && !aImage.isNull()) {
        auto image = aImage.convertToFormat(QImage::Format_RGBA8888);
        auto texImage = img::Util::createTextureImage(image);
        // the layer sits at the document's origin, in the padded rect the texel work needs
        entry.rect = QRect(QPoint(0, 0), texImage.second.size());
        entry.pixels.adopt(texImage.first);
        entry.pixelSize = texImage.second.size();
    }
    document.top.children.push_back(std::move(entry));
    return document;
}

//-------------------------------------------------------------------------------------------------
SourceDocument describeOra(
    const std::string& aFilePath, bool aMerged, const QString& aDocumentName, bool aWantPixels,
    const std::function<void(int, int)>& aProgress
) {
    SourceDocument document;
    document.documentName = aDocumentName;
    document.top.name = "topnode";
    document.top.isLayer = false;

    if (aMerged) {
        miniz_cpp::zip_file ora(aFilePath);
        if (!ora.has_file("mergedimage.png")) {
            document.error = "ORA has no mergedimage.png";
            return document;
        }
        QImage image = QImage::fromData(QByteArray::fromStdString(ora.read("mergedimage.png")));
        if (image.isNull()) {
            document.error = "Failed to load the merged image of the ORA";
            return document;
        }
        document.canvasSize = image.size();
        document.top.rect = QRect(QPoint(), document.canvasSize);

        SourceDocument::Entry entry;
        entry.name = aDocumentName;
        entry.isLayer = true;
        entry.blendMode = img::BlendMode_Normal;
        entry.visible = true;
        if (aWantPixels) {
            auto texImage = img::Util::createTextureImage(image.convertToFormat(QImage::Format_RGBA8888));
            // the resource's position for a merged import has always been the CENTRE of the padded
            // rect (img::Util::createResourceNode set it that way, and the object node took its
            // rect and posture from it) - kept, so a merged import lands where it always did
            entry.rect = QRect(texImage.second.center(), texImage.second.size());
            entry.pixels.adopt(texImage.first);
            entry.pixelSize = texImage.second.size();
        }
        document.top.children.push_back(std::move(entry));
        return document;
    }

    miniz_cpp::zip_file ora(aFilePath);
    oraParser reader(&ora);
    if (!reader.initialize()) {
        document.error = "Unable to read the ORA's stack.xml";
        return document;
    }
    const auto& image = reader.oraImage.oraImage;
    document.canvasSize = QSize(image.width, image.height);
    document.top.rect = QRect(QPoint(), document.canvasSize);

    // the flat list is pre-order, with `subtree` counting the entries that belong to each folder
    struct OpenFolder {
        SourceDocument::Entry* entry;
        int remaining;
    };
    std::vector<OpenFolder> stack;
    std::vector<size_t> path; // index path to the current folder, safe against reallocation

    const auto currentFolder = [&]() -> SourceDocument::Entry* {
        auto* entry = &document.top;
        for (size_t index : path) {
            entry = &entry->children[index];
        }
        return entry;
    };

    int progress = 0;
    if (aProgress) {
        aProgress(0, image.layerNumber);
    }

    for (const auto& lyr : reader.oraImage.layers) {
        // close every folder whose subtree is complete: the flat list is pre-order, so the
        // current element is the first one beyond them
        while (!stack.empty() && stack.back().remaining == 0) {
            auto* closed = stack.back().entry;
            stack.pop_back();
            path.pop_back();
            // a group takes the union of its children's object rects, like the importer did
            QRect rect;
            for (const auto& child : closed->children) {
                const QRect childRect = child.initialRect();
                rect = rect.isValid() ? rect.united(childRect) : childRect;
            }
            if (rect.isValid()) {
                closed->objectInitialRect = rect;
                closed->hasObjectInitialRect = true;
            }
        }
        // the current element belongs to every still-open folder
        for (auto& open : stack) {
            --open.remaining;
        }

        auto* parent = currentFolder();
        SourceDocument::Entry entry;
        entry.name = QString::fromStdString(lyr.name);
        entry.blendMode = oraParser::oraBlendToPSDBlend(lyr.composite.blend);
        entry.visible = lyr.isVisible;
        entry.opacity255 = (int)std::lround(lyr.opacity * 255.0f);
        // no Blend If ranges, layer effects or masks in the ORA format (and no stable id either)
        entry.hasObjectData = false;

        if (lyr.type == IMAGE) {
            entry.isLayer = true;
            // the object node's geometry is stack.xml's rect; the resource is the padded bitmap
            entry.objectInitialRect = lyr.rect;
            entry.hasObjectInitialRect = true;
            entry.rect = lyr.rect;
            if (aWantPixels && !lyr.image.isNull()) {
                auto texImage = img::Util::createTextureImage(lyr.image);
                entry.rect = texImage.second;
                entry.pixels.adopt(texImage.first);
                entry.pixelSize = texImage.second.size();
            }
            parent->children.push_back(std::move(entry));
        } else if (lyr.type == FOLDER) {
            entry.isLayer = false;
            entry.rect = lyr.rect; // the resource's position
            parent->children.push_back(std::move(entry));
            path.push_back(parent->children.size() - 1);
            stack.push_back(OpenFolder{&parent->children.back(), lyr.subtree});
        }

        ++progress;
        if (aProgress) {
            aProgress(progress, image.layerNumber);
        }
    }

    // any folder the list ended inside of
    while (!stack.empty()) {
        auto* closed = stack.back().entry;
        stack.pop_back();
        QRect rect;
        for (const auto& child : closed->children) {
            const QRect childRect = child.initialRect();
            rect = rect.isValid() ? rect.united(childRect) : childRect;
        }
        if (rect.isValid()) {
            closed->objectInitialRect = rect;
            closed->hasObjectInitialRect = true;
        }
    }

    return document;
}

//-------------------------------------------------------------------------------------------------
SourceDocument describePsd(
    const img::PSDFormat& aFormat, const QString& aDocumentName, bool aWantPixels,
    const std::function<void(int, int)>& aProgress
) {
    using ReverseIterator = PSDFormat::LayerList::const_reverse_iterator;

    SourceDocument document;
    document.documentName = aDocumentName;
    document.canvasSize = QSize((int)aFormat.header().width, (int)aFormat.header().height);
    document.top.name = "topnode";
    document.top.isLayer = false;
    document.top.rect = QRect(QPoint(), document.canvasSize);

    const PSDFormat::LayerList& layers = aFormat.layerAndMaskInfo().layers;
    const img::Util::TextFilter textFilter(aFormat);

    if (aProgress) {
        aProgress(0, (int)layers.size());
    }
    int progress = 0;

    if (layers.empty()) {
        // flattened PSD: one opaque layer holding the merged image data
        auto entry = describeMerged(aFormat.header(), aFormat.imageData(), document.canvasSize, aDocumentName);
        if (entry.pixels.isEmpty() && aWantPixels) {
            document.error = "failed to decode the merged image data";
            return document;
        }
        document.top.children.push_back(std::move(entry));
        return document;
    }

    // The records are stored bottom-first; the description is built top-first, like the object
    // and resource trees, so a group closes when its bounding record is met. The open groups are
    // tracked by an index PATH rather than by pointers: a group's children live in a vector that
    // grows as later siblings are pushed, and a pointer taken before that would dangle.
    std::vector<size_t> path;
    const auto current = [&]() -> SourceDocument::Entry* {
        auto* entry = &document.top;
        for (size_t index : path) {
            entry = &entry->children[index];
        }
        return entry;
    };
    for (ReverseIterator itr = layers.rbegin(); itr != layers.rend(); ++itr) {
        const PSDFormat::Layer& layer = *((*itr).get());
        const QString name = textFilter.get(layer.name);

        if (layer.entryType == PSDFormat::LayerEntryType_Bounding) {
            if (!path.empty()) {
                auto* closed = current();
                // a group takes the union of its children's rects
                QRect rect;
                for (const auto& child : closed->children) {
                    rect = rect.isValid() ? rect.united(child.rect) : child.rect;
                }
                if (rect.isValid()) {
                    closed->rect = rect;
                }
                path.pop_back();
            }
            continue;
        }

        if (layer.entryType == PSDFormat::LayerEntryType_Layer) {
            auto entry = describeLayer(aFormat.header(), layer, name, aWantPixels, document.error);
            if (!document.error.isEmpty()) {
                return document;
            }
            current()->children.push_back(std::move(entry));
        } else {
            auto* parent = current();
            parent->children.push_back(describeFolder(layer, name));
            path.push_back(parent->children.size() - 1);
        }

        ++progress;
        if (aProgress) {
            aProgress(progress, (int)layers.size());
        }
    }

    return document;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
img::ResourceNode* buildResourceNode(SourceDocument& aDocument, SourceDocument::Entry& aEntry) {
    auto* node = new img::ResourceNode(aEntry.name);
    auto& data = node->data();
    data.setPos(aEntry.rect.topLeft());
    data.setIsLayer(aEntry.isLayer);
    data.setBlendMode(aEntry.blendMode);
    data.setIsVisible(aEntry.visible);
    data.setStableId(aEntry.stableId);
    if (aEntry.isLayer) {
        // the resource keeps its own derived channels current after every pixel change (img
        // cannot compute them itself: the algorithms live in core)
        data.setDeriver(&core::DistanceField::deriveResourceChannels);
        if (aEntry.pixels) {
            data.grabImage(aDocument.takePixels(aEntry), aEntry.pixelSize, img::Format_RGBA8);
        }
        if (aEntry.mask) {
            data.grabMask(aEntry.mask.take(), aEntry.pixelSize);
            data.setMaskEnabled(aEntry.maskEnabled);
            data.setMaskInverted(aEntry.maskInverted);
        }
        // A stroke is a function of the outline distance, so a layer that has one will ask for
        // that plane as soon as it is drawn. Asking here too is a WARM-UP - derive it before the
        // first frame instead of during it - and nothing more: there is no state to record, so
        // being wrong about which layer needs it (or about whether this build draws the stroke at
        // all) costs at most one derivation, not a missing effect. A revision of this file
        // recorded the need as a flag on the resource instead, and a reload that added a stroke
        // then never set it: see ResourceData's note on derived channels.
        if (aEntry.effects.stroke.isUsable()) {
            (void)data.distance();
        }
    }
    return node;
}

img::ResourceNode* buildResourceTree(SourceDocument& aDocument) {
    auto* top = new img::ResourceNode(
        aDocument.top.name.isEmpty() ? QString("topnode") : aDocument.top.name);

    // one walker for both the first import and a reload, so a resource that came from a reload
    // is the resource the import would have built
    std::function<void(img::ResourceNode&, SourceDocument::Entry&)> walk =
        [&](img::ResourceNode& aParent, SourceDocument::Entry& aEntry) {
            for (auto& child : aEntry.children) {
                aParent.children().pushBack(buildResourceNode(aDocument, child));
                walk(*aParent.children().back(), child);
            }
        };

    walk(*top, aDocument.top);
    return top;
}

//-------------------------------------------------------------------------------------------------
bool matchesIdentity(const img::ResourceNode& aNode, const SourceDocument::Entry& aEntry) {
    const img::ResourceData& data = aNode.data();
    if (data.isLayer() != aEntry.isLayer) {
        return false;
    }
    if (!data.stableId().isEmpty() && !aEntry.stableId.isEmpty()) {
        return data.stableId() == aEntry.stableId;
    }
    return data.identifier() == aEntry.name;
}

ResourceDiff diffResource(const img::ResourceNode& aNode, const SourceDocument::Entry& aEntry) {
    ResourceDiff diff;
    const img::ResourceData& data = aNode.data();
    if (!aEntry.isLayer) {
        // a group has no pixels, no mask and no blend of its own in the resource tree
        diff.name = data.identifier() != aEntry.name;
        return diff;
    }
    diff.name = data.identifier() != aEntry.name;
    diff.blend = data.blendMode() != aEntry.blendMode;
    diff.visible = data.isVisible() != aEntry.visible;

    if (data.pos() != aEntry.rect.topLeft()) {
        diff.pixels = true;
    }
    if (diff.pixels || data.image().pixelSize() != aEntry.pixelSize
        || data.image().pixelSize() != aEntry.rect.size()) {
        diff.pixels = true;
    } else if (aEntry.pixels) {
        // the reader hands the same padded layout the importer stores, so a byte compare is the
        // comparison of the two images
        const size_t size = data.image().size();
        diff.pixels = size != aEntry.pixels.size()
            || !std::equal(data.image().data(), data.image().data() + size, aEntry.pixels.data());
    }

    // The mask is compared only when the description knows the file's mask channel (see
    // Entry::hasMaskData): otherwise there is nothing to compare against and the project's own
    // mask must be left exactly as it is.
    const bool fileHasMask = !aEntry.mask.isEmpty();
    if (!aEntry.hasMaskData) {
        diff.mask = false;
    } else if (data.hasMask() != fileHasMask) {
        diff.mask = true;
    } else if (fileHasMask) {
        if (data.isMaskEnabled() != aEntry.maskEnabled || data.isMaskInverted() != aEntry.maskInverted
            || data.mask().pixelSize() != aEntry.pixelSize) {
            diff.mask = true;
        } else {
            const size_t size = data.mask().size();
            diff.mask = size != aEntry.mask.size()
                || !std::equal(data.mask().data(), data.mask().data() + size, aEntry.mask.data());
        }
    }
    return diff;
}

ObjectDiff diffObject(const core::ObjectNode& aNode, const SourceDocument::Entry& aEntry) {
    ObjectDiff diff;
    if (!aEntry.hasObjectData) {
        // the file did not describe these properties, so a reload must leave them alone
        return diff;
    }
    diff.visibility = aNode.isVisible() != aEntry.visible;
    if (const core::Renderer* renderer = aNode.renderer()) {
        // for a layer the blend mode the renderer uses is the image key's, not the resource's
        diff.blend = renderer->blendMode() != aEntry.blendMode;
        diff.blendIf = renderer->blendIf() != aEntry.blendIf;
        diff.clipping = renderer->isClipped() != aEntry.clipped;
        if (aNode.type() == core::ObjectType_Layer) {
            const auto& layer = static_cast<const core::LayerNode&>(aNode);
            diff.effects = renderer->layerEffects() != aEntry.effects;
            diff.fillOpacity =
                std::abs(layer.fillOpacity() - (float)aEntry.fillOpacity255 / 255.0f) > 1.0f / 512.0f;
        }
    }
    // the node's opacity is an animatable key, so a reload compares the file against the key's
    // default (what an import wrote) rather than against the value at the current frame
    if (const core::TimeLine* timeLine = aNode.timeLine()) {
        if (const core::TimeKey* key = timeLine->defaultKey(core::TimeKeyType_Opa)) {
            const auto* opa = static_cast<const core::OpaKey*>(key);
            diff.opacity =
                std::abs(opa->opacity() - (float)aEntry.opacity255 / 255.0f) > 1.0f / 512.0f;
        }
    }
    return diff;
}

//-------------------------------------------------------------------------------------------------
void buildObjectTree(
    const SourceDocument& aDocument, core::FolderNode& aTopNode, const img::ResourceNode& aResourceTree,
    core::Project& aProject
) {
    // The depth is a RUNNING counter over the records, not a per-parent constant: the render
    // order is the depth order (core::SortAndRenderCall sorts by it), so two siblings that both
    // got their parent's depth render in tree order instead of the document's stacking order.
    float globalDepth = core::ObjectNodeUtil::getInitialWorldDepth(aTopNode);

    std::function<void(const SourceDocument::Entry&, const img::ResourceNode&, core::FolderNode&)> walk =
        [&](const SourceDocument::Entry& aEntry, const img::ResourceNode& aResource, core::FolderNode& aParent) {
            const float parentDepth = core::ObjectNodeUtil::getInitialWorldDepth(aParent);
            auto nodeItr = aResource.children().begin();
            for (const auto& child : aEntry.children) {
                XC_PTR_ASSERT(nodeItr != aResource.children().end());
                const auto& node = **nodeItr;
                ++nodeItr;

                if (child.isLayer) {
                    // a LayerNode eagerly compiles its shaders, so it needs a current GL context
                    gl::Global::makeCurrentIfReady();
                    auto* layerNode = new core::LayerNode(child.name, aProject.objectTree().shaderHolder());
                    layerNode->setVisibility(child.visible);
                    layerNode->setClipped(child.clipped);
                    layerNode->setInitialRect(child.initialRect());
                    layerNode->setDefaultImage(node.handle());
                    layerNode->setDefaultDepth(globalDepth - parentDepth);
                    layerNode->setDefaultOpacity(static_cast<float>(child.opacity255) / 255.0f);
                    layerNode->setFillOpacity(static_cast<float>(child.fillOpacity255) / 255.0f);
                    layerNode->setBlendIf(child.blendIf);
                    layerNode->setLayerEffects(child.effects);
                    aParent.children().pushBack(layerNode);
                } else {
                    auto* folderNode = new core::FolderNode(child.name);
                    folderNode->setVisibility(child.visible);
                    folderNode->setClipped(child.clipped);
                    folderNode->setInitialRect(child.initialRect());
                    folderNode->setDefaultDepth(globalDepth - parentDepth);
                    folderNode->setDefaultOpacity(static_cast<float>(child.opacity255) / 255.0f);
                    folderNode->setBlendIf(child.blendIf);
                    // 'diss' (dissolve) is the only group mode still unsupported, so only it
                    // (plus unknown 4CCs) reads back as TERM; hue/sat/colr/lum/dkCl/lgCl are
                    // fully mapped modes. Fall back to Normal for TERM, matching the
                    // pre-feature behavior that dropped the group mode entirely (TERM would
                    // index the shader arrays out of bounds at render time).
                    const auto blend = child.blendMode;
                    folderNode->setBlendMode(blend == img::BlendMode_TERM ? img::BlendMode_Normal : blend);
                    aParent.children().pushBack(folderNode);

                    globalDepth -= 1.0f;
                    walk(child, node, *folderNode);
                    continue;
                }

                globalDepth -= 1.0f;
            }
        };

    walk(aDocument.top, aResourceTree, aTopNode);
}

//-------------------------------------------------------------------------------------------------
QStringList undrawnEffectNotes(const SourceDocument& aDocument) {
    QStringList notes;

    std::function<void(const SourceDocument::Entry&, const QString&)> walk =
        [&](const SourceDocument::Entry& aEntry, const QString& aPath) {
            for (const auto& child : aEntry.children) {
                const QString path = aPath.isEmpty() ? child.name : aPath + "/" + child.name;
                if (child.isLayer && child.hasObjectData && child.effects.isActive()
                    && !child.effects.isDrawnAsConfigured()) {
                    // name the effects that are actually switched on, not everything the block
                    // mentions: Photoshop writes all seven style keys into every lfx2 with an
                    // enable flag, so listing the present ones would accuse a file of satin it
                    // has switched off.
                    QStringList names;
                    for (auto type : child.effects.activeTypes()) {
                        if (type == core::LayerEffects::Type_Bevel
                            || type == core::LayerEffects::Type_Satin
                            || type == core::LayerEffects::Type_InnerGlow
                            || type == core::LayerEffects::Type_InnerShadow) {
                            names << core::LayerEffects::typeName(type);
                        }
                    }
                    // a stroke or an overlay can also fall outside what this build draws (an
                    // outside/centre band, a pattern paint, a non-linear gradient, a blend mode
                    // it cannot name); when it is the cause, say so by name as well
                    for (auto type : child.effects.activeTypes()) {
                        if (type == core::LayerEffects::Type_Stroke && !child.effects.stroke.isDrawable()) {
                            names << QStringLiteral("%1 (%2 position, %3 paint)")
                                         .arg(core::LayerEffects::typeName(type))
                                         .arg(QString::fromStdString(
                                             child.effects.stroke.style.empty() ? "inside"
                                                                               : child.effects.stroke.style))
                                         .arg(QString::fromStdString(
                                             child.effects.stroke.paintType.empty() ? "unknown"
                                                                                   : child.effects.stroke.paintType));
                        }
                        if (type == core::LayerEffects::Type_GradientOverlay
                            && !child.effects.gradientOverlay.isLinear()) {
                            names << QStringLiteral("%1 (%2)")
                                         .arg(core::LayerEffects::typeName(type))
                                         .arg(QString::fromStdString(child.effects.gradientOverlay.style));
                        }
                    }
                    for (const auto& silhouette : child.effects.silhouettes) {
                        if (silhouette.isUsable() && !silhouette.blendModeKnown) {
                            names << QStringLiteral("%1 (unknown blend mode)")
                                         .arg(core::LayerEffects::silhouetteName(silhouette.kind));
                        }
                    }
                    if (names.isEmpty()) {
                        names << QCoreApplication::translate("Source Document", "an effect this build cannot name");
                    }
                    // dedupe: a stroke that is both outside and unnamed would otherwise be said
                    // twice, and the message is for a human reading a log
                    names.removeDuplicates();
                    notes << QCoreApplication::translate("Source Document",
                                     "Layer '%1': not drawn as the file configures it: %2")
                                 .arg(path, names.join(", "));
                }
                walk(child, path);
            }
        };
    walk(aDocument.top, QString());

    if (!notes.isEmpty()) {
        notes.prepend(QCoreApplication::translate(
            "Source Document", "The picture is not the file's where these effects are concerned:"));
    }
    return notes;
}

//-------------------------------------------------------------------------------------------------
core::FolderNode* createTopNode(const QString& aName, const QRect& aInitialRect) {
    auto* node = new core::FolderNode(aName);
    node->setInitialRect(aInitialRect);
    node->setDefaultOpacity(1.0f);
    node->setDefaultPosture(QVector2D());
    return node;
}

QRect calculateBoundingRectFromChildren(const core::FolderNode& aNode) {
    QRect rect;
    for (auto child : aNode.children()) {
        if (child->initialRect().isValid()) {
            rect = rect.isValid() ? rect.united(child->initialRect()) : child->initialRect();
        }
    }
    return rect;
}

void setDefaultPosturesFromInitialRects(core::FolderNode& aTopNode) {
    core::ObjectNode::Iterator itr(&aTopNode);
    while (itr.hasNext()) {
        auto node = itr.next();
        auto parent = node->parent();
        const bool isTop = !parent;
        const bool parentIsTop = parent != nullptr && !parent->parent();

        QVector2D pos;
        QVector2D parentPos;
        if (!isTop && parent != nullptr) {
            // parent position
            parentPos = (parent->initialRect().isValid() && !parentIsTop)
                ? util::MathUtil::getCenter(parent->initialRect())
                : QVector2D();

            // node position
            pos = (node->initialRect().isValid()) ? util::MathUtil::getCenter(node->initialRect()) : parentPos;
        }

        // set
        if (node->type() == core::ObjectType_Layer) {
            ((core::LayerNode*)node)->setDefaultPosture(pos - parentPos);
        } else if (node->type() == core::ObjectType_Folder) {
            ((core::FolderNode*)node)->setDefaultPosture(pos - parentPos);
        }
    }
}

} // namespace ctrl
