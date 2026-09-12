#ifndef GUI_PROP_CONSTANTPANEL_H
#define GUI_PROP_CONSTANTPANEL_H

#include "core/BlendIf.h"
#include "core/Project.h"
#include "core/ObjectNode.h"
#include "gui/ViaPoint.h"
#include "gui/prop/prop_Panel.h"
#include "gui/prop/prop_AttrGroup.h"
#include "gui/prop/prop_Items.h"

namespace gui {
namespace prop {

    class ConstantPanel: public Panel {
        Q_OBJECT
    public:
        ConstantPanel(ViaPoint& aViaPoint, core::Project& aProject, QWidget* aParent, GUIResources* aGUIResources);
        void setTarget(core::ObjectNode* aTarget);
        void setPlayBackActivity(bool aIsActive);
        void updateAttribute();

        // Read-only handles for the automated panel test (tests/panel_blendif.cpp), so it can
        // assert what the panel actually shows without reaching into the widget tree: the four
        // split-slider items of one side (black-low, black-high, white-low, white-high) and
        // that side's reset button.
        IntegerItem* blendIfItem(bool aDst, int aIndex) {
            return (aDst ? mDstBlendIf : mSrcBlendIf)[aIndex];
        }
        const IntegerItem* blendIfItem(bool aDst, int aIndex) const {
            return (aDst ? mDstBlendIf : mSrcBlendIf)[aIndex];
        }
        ButtonItem* blendIfReset(bool aDst) { return aDst ? mDstBlendIfReset : mSrcBlendIfReset; }

    private:
        void assignBlendMode(core::Project&, core::ObjectNode*, img::BlendMode);
        void assignClipped(core::Project&, core::ObjectNode*, bool);
        // Blend If: one side (underlying layer / this layer) at a time; the whole struct is
        // written so a single edit is one undo step
        void assignBlendIfSide(core::Project&, core::ObjectNode*, bool aDst, const core::BlendIf::Range&);
        void resetBlendIfSide(core::Project&, core::ObjectNode*, bool aDst);
        void assignMaskEnabled(core::Project&, core::ObjectNode*, bool);
        void assignMaskInverted(core::Project&, core::ObjectNode*, bool);
        // bakes the layer's Blend If SOURCE range into its mask (the destination range
        // depends on the backdrop and stays live)
        void bakeSourceRange(core::Project&, core::ObjectNode*);

        void build();
        void updateBlendIfItems();
        void updateMaskItems();

        ViaPoint& mViaPoint;
        core::Project& mProject;
        GUIResources* mGUIResources;
        core::ObjectNode* mTarget;
        int mLabelWidth;

        AttrGroup* mRenderingAttributes;
        ComboItem* mBlendMode;
        CheckItem* mClipped;

        // Blend If ranges, four split-slider values per side (0..255); the PSD default
        // (0, 0, 255, 255) means "no restriction"
        AttrGroup* mDstBlendIfGroup;
        AttrGroup* mSrcBlendIfGroup;
        IntegerItem* mDstBlendIf[4];
        IntegerItem* mSrcBlendIf[4];
        ButtonItem* mDstBlendIfReset;
        ButtonItem* mSrcBlendIfReset;

        // layer mask (imported from PSD, or baked from the source range)
        AttrGroup* mMaskGroup;
        CheckItem* mMaskEnabled;
        CheckItem* mMaskInverted;
        ButtonItem* mMaskBake;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_CONSTANTPANEL_H
