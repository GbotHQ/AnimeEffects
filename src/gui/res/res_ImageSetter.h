#ifndef GUI_RES_IMAGESETTER
#define GUI_RES_IMAGESETTER

#include "cmnd/Stack.h"
#include "img/ResourceNode.h"

namespace gui {
namespace res {

    // Applies one resource's whole content in a single swap: the pixels and rect a reload read
    // from the file, plus whichever of the mask / blend mode / visibility it found different
    // (img::ResourceNode::Content). Reusing the node's handle - rather than mutating the current
    // data in place - is what lets undo restore all of it at once, and what makes the object
    // nodes that reference the resource see the update without a command of their own.
    class ContentSetter: public cmnd::Stable {
        img::ResourceNode& mNode;
        img::ResourceNode::Content mContent;
        img::ResourceHandle mAnotherHandle;
        bool mDoneOnce;

    public:
        ContentSetter(img::ResourceNode& aNode, img::ResourceNode::Content aContent):
            mNode(aNode), mContent(std::move(aContent)), mAnotherHandle(), mDoneOnce(false) {
            XC_PTR_ASSERT(mContent.pixels.data);
        }

        ~ContentSetter() {
            if (!mDoneOnce) {
                delete[] mContent.pixels.data;
            }
        }

        void exec() {
            mAnotherHandle = mNode.updateHandleContent(std::move(mContent));
            mDoneOnce = true;
        }

        void redo() { mNode.swapData(mAnotherHandle); }

        void undo() { mNode.swapData(mAnotherHandle); }
    };

    class ImageSetter: public cmnd::Stable {
        img::ResourceNode& mNode;
        XCMemBlock mBlock;
        QRect mRect;
        img::ResourceHandle mAnotherHandle;
        bool mDoneOnce;

    public:
        ImageSetter(img::ResourceNode& aNode, const XCMemBlock& aBlock, const QRect& aRect):
            mNode(aNode), mBlock(aBlock), mRect(aRect), mAnotherHandle(), mDoneOnce(false) {
            XC_PTR_ASSERT(aBlock.data);
        }

        ~ImageSetter() {
            if (!mDoneOnce) {
                delete[] mBlock.data;
            }
        }

        void exec() {
            mAnotherHandle = mNode.updateHandle(mBlock, mRect);
            mDoneOnce = true;
        }

        void redo() { mNode.swapData(mAnotherHandle); }

        void undo() { mNode.swapData(mAnotherHandle); }
    };

} // namespace res
} // namespace gui

#endif // GUI_RES_IMAGESETTER
