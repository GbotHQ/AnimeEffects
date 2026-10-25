#ifndef CTRL_MASKEDITOR_H
#define CTRL_MASKEDITOR_H

#include "core/AbstractCursor.h"
#include "ctrl/AssignPixels.h"
#include "ctrl/IEditor.h"
#include "ctrl/MaskParam.h"
#include "ctrl/UILogger.h"

namespace core {
class FilterMask;
}

namespace ctrl {

// The PAINT tool for a filter mask (core/FilterMask.h). One editor for every
// filter lane: the lane and the channel come from MaskParam, so the brush is the
// same one the FFD and influence tools use and only the TARGET differs.
//
// The stroke is the shared BrushStroke (ctrl/Brush.h): stamps on the press, spaced
// along every drag, ended on the release. Each stamp writes the mask's texels
// through the shared falloff and records the box it touched, so the whole stroke
// is ONE undo step (ctrl/AssignPixels.h).
class MaskEditor: public IEditor {
public:
    // No UILogger: this editor is a brush over a field and never logs, so the parameter
    // would be a dead dependency on the driver's logger.
    explicit MaskEditor(core::Project& aProject);

    void setParam(const MaskParam& aParam);
    const MaskParam& param() const { return mParam; }

    // from IEditor
    virtual bool setTarget(core::ObjectNode* aTarget) override;
    virtual bool updateCursor(const core::CameraInfo& aCamera, const core::AbstractCursor& aCursor) override;
    virtual void updateEvent(EventType aType) override;
    virtual void renderQt(const core::RenderInfo& aInfo, QPainter& aPainter) override;

    // the mask the tool would write, or null when the target cannot carry one
    core::FilterMask* currentMask();
    // clears the field being edited, as one undo step
    void clearMask();
    // THE TEMPORARY EYEDROPPER: the field's value under a world position, or a
    // negative number when there is no field there. The caller (the viewport, which
    // owns the modifier state) puts it back into the panel.
    float sampleValue(const QVector2D& aWorldPos) const;
    // the preview the viewport draws while this tool is active (null when the target
    // carries no field for the selected lane/channel)
    const core::ObjectNode* previewTarget() const { return mTarget; }
    const core::FilterMask* previewMask() const;

private:
    // The node's CONTENT rectangle: a layer's image box, a folder's canvas-shaped
    // local box. Empty when the node cannot carry a mask.
    QRectF contentRect() const;
    // world (canvas) position -> content pixels, through the same world matrix the
    // renderer draws the node with
    bool toContent(const QVector2D& aWorldPos, QVector2D& aContentPos) const;
    // the world-space length of one content pixel along the node's X axis
    float contentScale() const;
    // the filter lane's mask, allocated on first use at the node's content rect
    core::FilterMask* ensureMask();
    bool paintStamp(const BrushStamp& aStamp);
    void beginStroke();
    void endStroke();

    core::Project& mProject;
    core::ObjectNode* mTarget;
    MaskParam mParam;
    BrushStroke mStroke;
    QVector2D mCursorPos;
    bool mPainting;
    AssignPixels* mCommand;
};

} // namespace ctrl

#endif // CTRL_MASKEDITOR_H
