#include <memory>
#include "img/ResourceNode.h"

namespace img {

ResourceNode::ResourceNode(const QString& aIdentifier):
    TreeNodeBase(this), mHandle(new ResourceData(aIdentifier, this), new int(0)), mIsAbandoned() {}

ResourceNode::~ResourceNode() { qDeleteAll(children()); }

ResourceHandle ResourceNode::updateHandle(XCMemBlock aGrabbedImage, const QRect& aRect) {
    ResourceHandle oldHandle = mHandle;
    auto id = mHandle->identifier();

    mHandle = ResourceHandle(new ResourceData(id, this), new int(0));
    *mHandle = *oldHandle;
    mHandle->setPos(aRect.topLeft());
    mHandle->grabImage(aGrabbedImage, aRect.size(), img::Format_RGBA8);

    return oldHandle;
}

ResourceHandle ResourceNode::updateHandleContent(Content aContent) {
    ResourceHandle oldHandle = mHandle;
    auto id = mHandle->identifier();

    mHandle = ResourceHandle(new ResourceData(id, this), new int(0));
    *mHandle = *oldHandle;
    if (aContent.setBlend) {
        mHandle->setBlendMode(aContent.blend);
    }
    if (aContent.setVisible) {
        mHandle->setIsVisible(aContent.visible);
    }
    mHandle->setPos(aContent.rect.topLeft());
    if (aContent.setMask) {
        if (aContent.hasMask) {
            mHandle->grabMask(aContent.mask, aContent.rect.size());
            mHandle->setMaskEnabled(aContent.maskEnabled);
            mHandle->setMaskInverted(aContent.maskInverted);
        } else {
            mHandle->freeMask();
            mHandle->setMaskEnabled(true);
            mHandle->setMaskInverted(false);
        }
    }
    // last: grabbing the pixels re-derives whatever derived channels are wanted, from THESE
    // pixels (see ResourceData::grabImage)
    mHandle->grabImage(aContent.pixels, aContent.rect.size(), img::Format_RGBA8);

    return oldHandle;
}

void ResourceNode::swapData(ResourceHandle& aRhs) {
    XC_ASSERT(aRhs->identifier() == mHandle->identifier());
    XC_ASSERT(aRhs->serialAddress() == mHandle->serialAddress());
    mHandle.swapData(aRhs);
}

int ResourceNode::getCountOfSameSiblings() const {
    int count = 0;
    auto parent = this->parent();

    if (parent) {
        for (auto child : parent->children()) {
            // the same identity rule the reload matches with, so "which sibling is this?" has one
            // answer everywhere: a stable id when both sides have one, else the name
            if (child != this && child->mHandle->matchesIdentity(*this->mHandle)) {
                ++count;
            }
        }
    }
    return count;
}

QString ResourceNode::treePath() const {
    QStringList path;
    for (auto p = this; p != nullptr; p = p->parent()) {
        path.push_front(p->data().identifier());
    }
    return path.join("/");
}

} // namespace img
