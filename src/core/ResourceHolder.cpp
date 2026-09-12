#include <QDir>
#include <memory>
#include "img/BlendMode.h"
#include "core/DistanceField.h"
#include "core/ResourceHolder.h"

namespace core {

//-------------------------------------------------------------------------------------------------
ResourceHolder::ResourceHolder(): mImageTrees(), mRootPath() { mRootPath = QDir::currentPath(); }

ResourceHolder::~ResourceHolder() { destroy(); }

QString ResourceHolder::relativeFilePath(const QString& aAbsFilePath) const {
    QDir dir(mRootPath);
    return dir.relativeFilePath(aAbsFilePath);
}

void ResourceHolder::setRootPath(const QString& aRootPath) {
    QDir oldDir(mRootPath);
    QDir newDir(aRootPath);
    mRootPath = aRootPath;

    for (auto& data : mImageTrees) {
        auto absFilePath = oldDir.absoluteFilePath(data.filePath);
        data.filePath = newDir.relativeFilePath(absFilePath);
    }
}

void ResourceHolder::pushImageTree(img::ResourceNode& aGrabNode, const QString& aFilePath) {
    ImageTree data;
    data.topNode = &aGrabNode;
    data.filePath = relativeFilePath(aFilePath);
    mImageTrees.push_back(data);
}

ResourceHolder::ImageTree ResourceHolder::popImageTree() {
    ImageTree tree = mImageTrees.back();
    mImageTrees.pop_back();
    return tree;
}

void ResourceHolder::insertImageTree(const ImageTree& aTree, int aIndex) {
    int index = 0;
    for (auto itr = mImageTrees.begin(); itr != mImageTrees.end(); ++itr) {
        if (index == aIndex) {
            mImageTrees.insert(itr, aTree);
            return;
        }
        ++index;
    }
    XC_ASSERT(index == aIndex);
    mImageTrees.push_back(aTree);
}

void ResourceHolder::removeImageTree(int aIndex) {
    int index = 0;
    for (auto itr = mImageTrees.begin(); itr != mImageTrees.end(); ++itr) {
        if (index == aIndex) {
            mImageTrees.erase(itr);
            return;
        }
        ++index;
    }
    XC_ASSERT(0);
}

ResourceHolder::ImageTree ResourceHolder::imageTree(int aIndex) const {
    int index = 0;
    for (auto itr = mImageTrees.begin(); itr != mImageTrees.end(); ++itr) {
        if (index == aIndex)
            return *itr;
        ++index;
    }
    return ImageTree();
}

QString ResourceHolder::changeAbsoluteFilePath(img::ResourceNode& aTopNode, const QString& aAbsFilePath) {
    QDir dir(mRootPath);

    for (auto& data : mImageTrees) {
        if (data.topNode == &aTopNode) {
            data.filePath = dir.relativeFilePath(aAbsFilePath);
            return data.filePath;
        }
    }
    return QString();
}

QString ResourceHolder::findAbsoluteFilePath(const img::ResourceNode& aTopNode) const {
    for (auto& data : mImageTrees) {
        if (data.topNode == &aTopNode) {
            return QDir(mRootPath).absoluteFilePath(data.filePath);
        }
    }
    return QString();
}

QString ResourceHolder::findRelativeFilePath(const img::ResourceNode& aTopNode) const {
    for (auto& data : mImageTrees) {
        if (data.topNode == &aTopNode) {
            return data.filePath;
        }
    }
    return QString();
}

void ResourceHolder::destroy() {
    for (auto data : mImageTrees) {
        delete data.topNode;
    }
    mImageTrees.clear();
}

bool ResourceHolder::serialize(Serializer& aOut) const {
    static const std::array<uint8, 8> kSignature = {'R', 'e', 's', 'o', 'u', 'r', 'c', 'e'};

    // signature
    auto pos = aOut.beginBlock(kSignature);

    // top node count
    aOut.write((int)mImageTrees.size());

    for (auto data : mImageTrees) {
        // file path
        aOut.write(data.filePath);

        // write nodes
        if (!serializeNode(aOut, *data.topNode)) {
            return false;
        }
    }

    // end block
    aOut.endBlock(pos);

    return aOut.checkStream();
}

bool ResourceHolder::serializeNode(Serializer& aOut, const img::ResourceNode& aNode) const {
    static const std::array<uint8, 8> kSignature = {'R', 'e', 's', 'N', 'o', 'd', 'e', '_'};

    // block begin
    auto pos = aOut.beginBlock(kSignature);

    // identifier
    aOut.write(aNode.data().identifier());

    // child count
    aOut.write((int)aNode.children().size());

    // reference id
    aOut.writeID(&aNode);

    // is a layer
    aOut.write(aNode.data().isLayer());

    // rect
    aOut.write(QRect(aNode.data().pos(), aNode.data().image().pixelSize()));

    // blend mode
    aOut.writeFixedString(img::getQuadIdFromBlendMode(aNode.data().blendMode()), 4);

    // memory block(null image is also ok)
    aOut.writeImage(aNode.data().image().block(), aNode.data().image().pixelSize());

    // layer mask (minor version 13): presence flag, then a flags byte (bit 0 = enabled,
    // bit 1 = inverted) and the single-channel plane. Written unconditionally - the file
    // header carries the version, and older readers stop at the image block
    aOut.write(aNode.data().hasMask());
    if (aNode.data().hasMask()) {
        const int flags = (aNode.data().isMaskEnabled() ? 1 : 0) | (aNode.data().isMaskInverted() ? 2 : 0);
        aOut.write(flags);
        aOut.writeMaskImage(aNode.data().mask().block(), aNode.data().mask().pixelSize());
    }

    // stable identity (minor version 16): the id the source reload matches on, empty when the
    // source had none
    aOut.write(aNode.data().stableId());

    // derived channels (minor version 16): which ones a consumer wants, and the recipe each
    // carried plane was built with. The plane itself travels below; a reader can then tell
    // "wanted but absent", "built by an older recipe" and "measured from other pixels" apart
    // instead of trusting whatever bytes it finds.
    int wantedFlags = 0;
    for (int i = 0; i < img::ResourceData::Derivation_TERM; ++i) {
        if (aNode.data().isDerivationWanted((img::ResourceData::Derivation)i)) {
            wantedFlags |= (1 << i);
        }
    }
    aOut.write(wantedFlags);
    for (int i = 0; i < img::ResourceData::Derivation_TERM; ++i) {
        aOut.write(aNode.data().derived((img::ResourceData::Derivation)i).recipe);
    }

    // outline distance field (minor version 15): presence flag, then the A8 plane. It is
    // derived data, but it can only be computed from the CPU pixels, which a slimmed project
    // no longer holds - so it travels with the resource.
    aOut.write(aNode.data().hasDistance());
    if (aNode.data().hasDistance()) {
        aOut.writeMaskImage(aNode.data().distance().block(), aNode.data().distance().pixelSize());
    }

    // block end
    aOut.endBlock(pos);

    // check failure
    if (aOut.failure()) {
        return false;
    }

    // iterate children
    for (auto child : aNode.children()) {
        XC_PTR_ASSERT(child);

        if (!serializeNode(aOut, *child)) {
            return false;
        }
    }

    return aOut.checkStream();
}

bool ResourceHolder::deserialize(Deserializer& aIn) {
    destroy();

    // check block begin
    if (!aIn.beginBlock("Resource"))
        return aIn.errored("invalid signature of resources");

    // dive log scope
    aIn.pushLogScope("Resources");


    // top node count
    int topNodeCount = 0;
    aIn.read(topNodeCount);

    // each tree
    for (int i = 0; i < topNodeCount; ++i) {
        ImageTree data;
        // file path
        aIn.read(data.filePath);

        // deserialize node
        if (!deserializeNode(aIn, &data.topNode)) {
            return false;
        }

        mImageTrees.push_back(data);

        // progress report
        aIn.reportCurrent();
    }

    // check block end
    if (!aIn.endBlock())
        return aIn.errored("invalid end of resources");

    // rise log scope
    aIn.popLogScope();

    return aIn.checkStream();
}

bool ResourceHolder::deserializeNode(Deserializer& aIn, img::ResourceNode** aDst) {
    *aDst = nullptr;

    // check block begin
    if (!aIn.beginBlock("ResNode_"))
        return aIn.errored("invalid signature of resource node");

    // identifier
    QString identifier;
    aIn.read(identifier);

    // child count
    int childCount = 0;
    aIn.read(childCount);
    if (childCount < 0)
        return aIn.errored("invalid child count");

    int pendingWantedFlags = 0;
    int pendingRecipes[img::ResourceData::Derivation_TERM] = {};

    // create instance
    std::unique_ptr<img::ResourceNode> node(new img::ResourceNode(identifier));
    auto nodePtr = node.get();
    if (!node)
        return aIn.errored("failed to create resource node");
    // a loaded resource derives its own channels from here on: the deriver is not serialized (it
    // belongs to whoever owns the pixels), so the reader installs it
    nodePtr->data().setDeriver(&core::DistanceField::deriveResourceChannels);

    // reference id
    if (!aIn.bindIDData(nodePtr)) {
        return aIn.errored("failed to bind reference id");
    }

    // is a layer
    bool isLayer = false;
    aIn.read(isLayer);
    nodePtr->data().setIsLayer(isLayer);

    // rect
    QRect rect;
    aIn.read(rect);
    nodePtr->data().setPos(rect.topLeft());

    // blend mode
    {
        QString blendName;
        aIn.readFixedString(blendName, 4);
        auto blendMode = img::getBlendModeFromQuadId(blendName);
        if (blendMode == img::BlendMode_TERM) {
            return aIn.errored("invalid image blending mode");
        }
        nodePtr->data().setBlendMode(blendMode);
    }

    // memory block
    XCMemBlock block;
    if (!aIn.readImage(block)) {
        return aIn.errored("invalid image resource");
    }

    if (block.data) {
        nodePtr->data().grabImage(block, rect.size(), img::Format_RGBA8);
    }

    // layer mask: files from before the mask bump (minor < 13) end at the image block, so
    // the node simply has no mask
    if (aIn.version().minorVersion() >= 13) {
        bool hasMask = false;
        aIn.read(hasMask);
        if (hasMask) {
            int flags = 0;
            aIn.read(flags);
            XCMemBlock maskBlock;
            if (!aIn.readMaskImage(maskBlock))
                return aIn.errored("invalid layer mask image");
            if (maskBlock.data) {
                if (maskBlock.size != (size_t)rect.width() * (size_t)rect.height())
                    return aIn.errored("layer mask size does not match the image rect");
                nodePtr->data().grabMask(maskBlock, rect.size());
            }
            nodePtr->data().setMaskEnabled((flags & 1) != 0);
            nodePtr->data().setMaskInverted((flags & 2) != 0);
        }
    }

    // stable identity and derived channels: earlier files have neither, and the reload falls
    // back to the layer name
    if (aIn.version().minorVersion() >= 16) {
        QString stableId;
        aIn.read(stableId);
        nodePtr->data().setStableId(stableId);

        int wantedFlags = 0;
        aIn.read(wantedFlags);
        int recipes[img::ResourceData::Derivation_TERM] = {};
        for (int i = 0; i < img::ResourceData::Derivation_TERM; ++i) {
            aIn.read(recipes[i]);
        }
        // the plane itself is read further down, and reading it stamps it as built by the CURRENT
        // recipe; the state recorded in the file has to be applied afterwards for an old-recipe
        // plane to be recognisably stale. See below.
        pendingWantedFlags = wantedFlags;
        for (int i = 0; i < img::ResourceData::Derivation_TERM; ++i) {
            pendingRecipes[i] = recipes[i];
        }
    }

    // outline distance field: files from before the bump (minor < 15) end at the mask, and
    // the layer simply has none (its stroke/bevel are reported as not drawn)
    if (aIn.version().minorVersion() >= 15) {
        bool hasDistance = false;
        aIn.read(hasDistance);
        if (hasDistance) {
            XCMemBlock distanceBlock;
            if (!aIn.readMaskImage(distanceBlock))
                return aIn.errored("invalid layer distance field");
            if (distanceBlock.data) {
                if (distanceBlock.size != (size_t)rect.width() * (size_t)rect.height())
                    return aIn.errored("layer distance field size does not match the image rect");
                nodePtr->data().grabDistance(distanceBlock, rect.size());
            }
        }
    }

    // now that the plane (if any) is in place, apply what the file recorded: which channels are
    // wanted and which recipe their bytes came from. A wanted channel with no plane, or one built
    // by an older recipe, then reads as stale, and the resource derives it here - at load, from
    // the pixels that were just read - instead of during the first frame that needs it.
    if (aIn.version().minorVersion() >= 16) {
        nodePtr->data().restoreDerivationState(pendingWantedFlags, pendingRecipes);
        nodePtr->data().deriveChannels();
    }

    // check block end
    if (!aIn.endBlock())
        return aIn.errored("invalid end of resource node");

    // check failure
    if (aIn.failure())
        return aIn.errored("stream error");

    // iterate children
    for (int i = 0; i < childCount; ++i) {
        img::ResourceNode* child = nullptr;
        if (!deserializeNode(aIn, &child))
            return false;

        XC_PTR_ASSERT(child);
        nodePtr->children().pushBack(child);
    }

    // end
    if (!aIn.checkStream()) {
        return false;
    }

    // success
    *aDst = node.release();
    return true;
}

} // namespace core
