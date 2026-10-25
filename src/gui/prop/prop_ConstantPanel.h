#ifndef GUI_PROP_CONSTANTPANEL_H
#define GUI_PROP_CONSTANTPANEL_H

#include "core/Project.h"
#include "core/ObjectNode.h"
#include "gui/ViaPoint.h"
#include "gui/prop/prop_Panel.h"
#include "gui/prop/prop_Items.h"

class QWidget;

namespace gui {
namespace prop {

    class PropertyRow;

    class ConstantPanel: public Panel {
        Q_OBJECT
    public:
        ConstantPanel(ViaPoint& aViaPoint, core::Project& aProject, QWidget* aParent, GUIResources* aGUIResources);
        void setTarget(core::ObjectNode* aTarget);
        void setPlayBackActivity(bool aIsActive);
        void updateAttribute();

    private:
        void assignBlendMode(core::Project&, core::ObjectNode*, img::BlendMode);
        void assignClipped(core::Project&, core::ObjectNode*, bool);

        void build();

        ViaPoint& mViaPoint;
        core::Project& mProject;
        GUIResources* mGUIResources;
        core::ObjectNode* mTarget;

        QWidget* mRowsHost;
        PropertyRow* mBlendRow;
        PropertyRow* mClippedRow;
        ComboItem* mBlendMode;
        CheckItem* mClipped;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_CONSTANTPANEL_H
