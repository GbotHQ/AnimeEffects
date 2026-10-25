#ifndef GUI_PROP_CURRENTKEYPANEL_H
#define GUI_PROP_CURRENTKEYPANEL_H

#include "gui/GUIResources.h"
#include "core/Project.h"
#include "core/ObjectNode.h"
#include "gui/ViaPoint.h"
#include "gui/prop/prop_Panel.h"
#include "gui/prop/prop_KeyGroup.h"
#include "gui/prop/prop_KeyStateButton.h"
#include "gui/prop/prop_KeyKnocker.h"
#include "gui/prop/prop_Items.h"
#include "gui/prop/prop_KeyAccessor.h"
#include "gui/prop/prop_FilterStack.h"

namespace gui {
namespace prop {

    //-------------------------------------------------------------------------------------------------
    class MoveKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        MoveKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        CheckItem* mSmoothPath;
        Vector2DItem* mPosition;
        Vector2DItem* mCentroid;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class RotateKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        RotateKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        DecimalItem* mRotate;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class ScaleKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        ScaleKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        Vector2DItem* mScale;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class DepthKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        DepthKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        DecimalItem* mDepth;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class OpaKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        OpaKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        DecimalItem* mOpacity;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class PoseKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        PoseKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool, bool);
        void setKeyValue(const core::TimeKey* aKey);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class FFDKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        FFDKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool, bool);
        void setKeyValue(const core::TimeKey* aKey);
        bool keyExists() const;

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        bool mKeyExists;
    };

    //-------------------------------------------------------------------------------------------------
    class ImageKeyGroup: public KeyGroup {
        Q_OBJECT
    public:
        ImageKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, ViaPoint& aViaPoint, GUIResources* mGUIResources);
        void setKeyEnabled(bool);
        void setKeyExists(bool, bool);
        void setKeyValue(const core::TimeKey* aKey);
        void setKeyValue(const core::TimeLine& aTimeLine, int aFrame);
        bool keyExists() const;

    public:
        void knockNewKey();

    private:
        KeyAccessor& mAccessor;
        KeyKnocker* mKnocker;
        BrowseItem* mBrowse;
        Vector2DItem* mOffset;
        IntegerItem* mCellSize;
        bool mKeyExists;
        ViaPoint& mViaPoint;
    };

    //-------------------------------------------------------------------------------------------------
    class CurrentKeyPanel: public Panel {
        Q_OBJECT
    public:
        CurrentKeyPanel(ViaPoint& aViaPoint, core::Project& aProject, QWidget* aParent, GUIResources* mGUIResources);
        void setTarget(core::ObjectNode* aTarget);
        void setPlayBackActivity(bool aIsActive);
        void updateKey();
        void updateFrame();

    private:
        void build();
        void updateKeyExists();
        void updateKeyValue();

        ViaPoint& mViaPoint;
        GUIResources* mGUIResources;
        core::Project& mProject;
        core::ObjectNode* mTarget;
        KeyAccessor mKeyAccessor;
        int mLabelWidth;

        QScopedPointer<MoveKeyGroup> mMovePanel;
        QScopedPointer<RotateKeyGroup> mRotatePanel;
        QScopedPointer<ScaleKeyGroup> mScalePanel;
        QScopedPointer<DepthKeyGroup> mDepthPanel;
        QScopedPointer<OpaKeyGroup> mOpaPanel;
        QScopedPointer<FilterStackPanel> mFilterPanel;
        QScopedPointer<PoseKeyGroup> mPosePanel;
        QScopedPointer<FFDKeyGroup> mFFDPanel;
        QScopedPointer<ImageKeyGroup> mImagePanel;

        KeyStateButton* mMoveKey;
        KeyStateButton* mRotateKey;
        KeyStateButton* mScaleKey;
        KeyStateButton* mDepthKey;
        KeyStateButton* mOpaKey;
        KeyStateButton* mPoseKey;
        KeyStateButton* mFFDKey;
        KeyStateButton* mImageKey;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_CURRENTKEYPANEL_H
