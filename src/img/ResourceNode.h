#ifndef IMG_RESOURCENODE_H
#define IMG_RESOURCENODE_H

#include <QRect>
#include "util/TreeNodeBase.h"
#include "util/TreeIterator.h"
#include "util/NonCopyable.h"
#include "img/ResourceHandle.h"

namespace img {

class ResourceNode: public util::TreeNodeBase<ResourceNode>, private util::NonCopyable {
public:
    typedef util::TreeNodeBase<ResourceNode>::Children ChildrenType;
    typedef util::TreeIterator<ResourceNode, ChildrenType::Iterator> Iterator;
    typedef util::TreeIterator<const ResourceNode, ChildrenType::ConstIterator> ConstIterator;

    ResourceNode(const QString& aIdentifier);
    virtual ~ResourceNode();

    ResourceData& data() { return *mHandle; }
    const ResourceData& data() const { return *mHandle; }
    void swapData(ResourceHandle& aRhs);
    ResourceHandle updateHandle(XCMemBlock aGrabbedImage, const QRect& aRect);

    // Everything a source reload can hand over for one resource, applied in a single swap so a
    // reload never leaves a resource half-updated (and never applies only part of what it
    // detected). The identifier is deliberately NOT part of it: the resource's name is this
    // app's own label for it (and the fallback identity), so a rename in the source file is
    // reported rather than applied - the reload (gui::res::ResourceUpdater) says so in the log.
    struct Content {
        XCMemBlock pixels;          // the layer's image in aRect (padded, like the importer's)
        QRect rect;                 // the resource's rect, from the file
        bool setMask = false;       // false = leave the mask alone
        XCMemBlock mask;            // A8 in aRect when setMask and hasMask
        bool hasMask = false;       // false with setMask = the file has no mask: drop it
        bool maskEnabled = true;
        bool maskInverted = false;
        bool setBlend = false;
        BlendMode blend = BlendMode_Normal;
        bool setVisible = false;
        bool visible = true;
    };
    ResourceHandle updateHandleContent(Content aContent);

    ResourceHandle handle() const { return mHandle; }

    bool isReferenced() const { return mHandle.referenceCount() > 1; }
    bool isKeeped() const { return mHandle.originKeepingCount() > 0; }
    int getCountOfSameSiblings() const;

    void setAbandon(bool aIsAbandoned) { mIsAbandoned = aIsAbandoned; }
    bool isAbandoned() const { return mIsAbandoned; }

    QString treePath() const;

private:
    ResourceHandle mHandle;
    bool mIsAbandoned;
};

} // namespace img

#endif // IMG_RESOURCENODE_H
