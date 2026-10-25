#include "cmnd/ScopedMacro.h"
#include "cmnd/BasicCommands.h"
#include "core/Constant.h"
#include "core/TimeKeyExpans.h"
#include "core/DepthKey.h"
#include "ctrl/TimeLineUtil.h"
#include "gui/ResourceDialog.h"
#include "gui/prop/prop_CurrentKeyPanel.h"
#include "gui/prop/prop_Items.h"

namespace gui {
namespace prop {

    //-------------------------------------------------------------------------------------------------
    MoveKeyGroup::MoveKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Position"), aLabelWidth, mGUIResources),
        mAccessor(aAccessor),
        mKnocker(),
        mSmoothPath(),
        mPosition(),
        mCentroid(),
        mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("Move"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewMove();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // position
            mPosition = new Vector2DItem(this);
            mPosition->setRange(core::Constant::transMin(), core::Constant::transMax());
            mPosition->onValueUpdated = [=](QVector2D, QVector2D aNext) { this->mAccessor.assignMovePosition(aNext); };
            this->addItem(QString(), mPosition);

            // centroid
            mCentroid = new Vector2DItem(this);
            mCentroid->setRange(core::Constant::transMin(), core::Constant::transMax());
            mCentroid->onValueUpdated = [=](QVector2D, QVector2D aNext) { this->mAccessor.assignMoveCentroid(aNext); };
            this->addItem(tr("Centroid"), mCentroid);

            // path shape: one meaningful choice, not an interpolation enum
            mSmoothPath = new CheckItem(this);
            mSmoothPath->box().setText(tr("Smooth"));
            mSmoothPath->box().setToolTip(
                tr("Curve the position path between keys instead of straight segments.")
            );
            mSmoothPath->onValueUpdated = [=](bool aNext) {
                this->mAccessor.assignMoveSpline(
                    aNext ? core::MoveKey::SplineType_CatmullRom : core::MoveKey::SplineType_Linear
                );
            };
            this->addItem(tr("Path"), mSmoothPath);
        }
        setKeyEnabled(false);
        setKeyExists(false);
    }

    void MoveKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void MoveKeyGroup::setKeyExists(bool aIsExists) {
        mKeyExists = aIsExists;
        // The header diamond is the affordance now; the old row stays
        // constructed but hidden so no second add control can drift.
        mKnocker->setVisible(false);
        // Model 2: the group is the property's row, not a frame-local key
        // editor. The knocker above it is the "add a key here" affordance.
        this->setVisible(true);
    }

    void MoveKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Move);
        const core::MoveKey::Data data = ((const core::MoveKey*)aKey)->data();

        mSmoothPath->setValue(data.spline() == core::MoveKey::SplineType_CatmullRom, false);
        mPosition->setValue(data.pos());
        mCentroid->setValue(data.centroid());
    }

    void MoveKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Move, aFrame)) {
            setKeyValue(key);
            return;
        }
        // Animated between keys: show what the playhead actually evaluates.
        if (!aTimeLine.map(core::TimeKeyType_Move).isEmpty()) {
            const auto* defaultKey = (const core::MoveKey*)aTimeLine.defaultKey(core::TimeKeyType_Move);
            mSmoothPath->setValue(
                defaultKey && defaultKey->data().spline() == core::MoveKey::SplineType_CatmullRom, false
            );
            mPosition->setValue(aTimeLine.current().srt().pos());
            mCentroid->setValue(aTimeLine.current().srt().centroid());
            return;
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Move)) {
            setKeyValue(key);
        }
    }

    bool MoveKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    RotateKeyGroup::RotateKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Rotation"), aLabelWidth, mGUIResources), mAccessor(aAccessor), mKnocker(), mRotate(), mKeyExists(false) {
        using util::MathUtil;
        mKnocker = new KeyKnocker(tr("Rotate"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewRotate();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // rotate
            mRotate = new DecimalItem(this);
            mRotate->setRange(
                MathUtil::getDegreeFromRadian(core::Constant::rotateMin()),
                MathUtil::getDegreeFromRadian(core::Constant::rotateMax())
            );
            mRotate->onValueUpdated = [=](double, double aNext) {
                this->mAccessor.assignRotateAngle(MathUtil::getRadianFromDegree(aNext));
            };
            this->addItem(QString(), mRotate);
        }
        setKeyEnabled(false);
        setKeyExists(false);
    }

    void RotateKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void RotateKeyGroup::setKeyExists(bool aIsExists) {
        mKeyExists = aIsExists;
        // The header diamond is the affordance now; the old row stays
        // constructed but hidden so no second add control can drift.
        mKnocker->setVisible(false);
        // Model 2: the group is the property's row, not a frame-local key
        // editor. The knocker above it is the "add a key here" affordance.
        this->setVisible(true);
    }

    void RotateKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Rotate);
        const core::RotateKey::Data data = ((const core::RotateKey*)aKey)->data();

        mRotate->setValue(util::MathUtil::getDegreeFromRadian(data.rotate()));
    }

    void RotateKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Rotate, aFrame)) {
            setKeyValue(key);
            return;
        }
        if (!aTimeLine.map(core::TimeKeyType_Rotate).isEmpty()) {
            mRotate->setValue(aTimeLine.current().srt().rotate());
            return;
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Rotate)) {
            setKeyValue(key);
        }
    }

    bool RotateKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    ScaleKeyGroup::ScaleKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Scale"), aLabelWidth, mGUIResources), mAccessor(aAccessor), mKnocker(), mScale(), mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("Scale"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewScale();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // scale
            mScale = new Vector2DItem(this);
            mScale->setRange(core::Constant::scaleMin(), core::Constant::scaleMax());
            mScale->onValueUpdated = [=](QVector2D, QVector2D aNext) { this->mAccessor.assignScaleRate(aNext); };
            this->addItem(QString(), mScale);
        }
        setKeyEnabled(false);
        setKeyExists(false);
    }

    void ScaleKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void ScaleKeyGroup::setKeyExists(bool aIsExists) {
        mKeyExists = aIsExists;
        // The header diamond is the affordance now; the old row stays
        // constructed but hidden so no second add control can drift.
        mKnocker->setVisible(false);
        // Model 2: the group is the property's row, not a frame-local key
        // editor. The knocker above it is the "add a key here" affordance.
        this->setVisible(true);
    }

    void ScaleKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Scale);
        const core::ScaleKey::Data data = ((const core::ScaleKey*)aKey)->data();

        mScale->setValue(data.scale());
    }

    void ScaleKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Scale, aFrame)) {
            setKeyValue(key);
            return;
        }
        if (!aTimeLine.map(core::TimeKeyType_Scale).isEmpty()) {
            mScale->setValue(aTimeLine.current().srt().scale());
            return;
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Scale)) {
            setKeyValue(key);
        }
    }

    bool ScaleKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    DepthKeyGroup::DepthKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Depth"), aLabelWidth, mGUIResources), mAccessor(aAccessor), mKnocker(), mDepth(), mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("Depth"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewDepth();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // depth
            mDepth = new DecimalItem(this);
            mDepth->setRange(core::Constant::transMin(), core::Constant::transMax());
            mDepth->onValueUpdated = [=](double, double aNext) { this->mAccessor.assignDepthPosition(aNext); };
            this->addItem(QString(), mDepth);
        }
        setKeyEnabled(false);
        setKeyExists(false);
    }

    void DepthKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void DepthKeyGroup::setKeyExists(bool aIsExists) {
        mKeyExists = aIsExists;
        // The header diamond is the affordance now; the old row stays
        // constructed but hidden so no second add control can drift.
        mKnocker->setVisible(false);
        // Model 2: the group is the property's row, not a frame-local key
        // editor. The knocker above it is the "add a key here" affordance.
        this->setVisible(true);
    }

    void DepthKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Depth);
        const core::DepthKey::Data& data = ((const core::DepthKey*)aKey)->data();

        mDepth->setValue(data.depth());
    }

    void DepthKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Depth, aFrame)) {
            setKeyValue(key);
            return;
        }
        if (!aTimeLine.map(core::TimeKeyType_Depth).isEmpty()) {
            mDepth->setValue(aTimeLine.current().depth());
            return;
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Depth)) {
            setKeyValue(key);
        }
    }

    bool DepthKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    OpaKeyGroup::OpaKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Opacity"), aLabelWidth, mGUIResources),
        mAccessor(aAccessor),
        mKnocker(),
        mOpacity(),
        mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("Opacity"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewOpacity();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // opacity
            mOpacity = new DecimalItem(this);
            mOpacity->setRange(0.0f, 1.0f);
            mOpacity->box().setSingleStep(0.1);
            mOpacity->onValueUpdated = [=](double, double aNext) { this->mAccessor.assignOpacity(aNext); };
            this->addItem(QString(), mOpacity);
        }
        setKeyEnabled(false);
        setKeyExists(false);
    }

    void OpaKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void OpaKeyGroup::setKeyExists(bool aIsExists) {
        mKeyExists = aIsExists;
        // The header diamond is the affordance now; the old row stays
        // constructed but hidden so no second add control can drift.
        mKnocker->setVisible(false);
        // Model 2: the group is the property's row, not a frame-local key
        // editor. The knocker above it is the "add a key here" affordance.
        this->setVisible(true);
    }

    void OpaKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Opa);
        const core::OpaKey::Data& data = ((const core::OpaKey*)aKey)->data();

        mOpacity->setValue(data.opacity());
    }

    void OpaKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Opa, aFrame)) {
            setKeyValue(key);
            return;
        }
        if (!aTimeLine.map(core::TimeKeyType_Opa).isEmpty()) {
            mOpacity->setValue(aTimeLine.current().opa().opacity());
            return;
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Opa)) {
            setKeyValue(key);
        }
    }

    bool OpaKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    PoseKeyGroup::PoseKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("Pose"), aLabelWidth, mGUIResources), mAccessor(aAccessor), mKnocker(), mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("Pose"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewPose();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);
        }
        setKeyEnabled(false);
        setKeyExists(false, false);
    }

    void PoseKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void PoseKeyGroup::setKeyExists(bool aIsExists, bool aIsKnockable) {
        mKeyExists = aIsExists;
        mKnocker->setVisible(false);
        this->setVisible(aIsExists || aIsKnockable);
    }

    void PoseKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        // The group's easing was its only value, and the timeline header's chip
        // names the selection's now (see gui::TimeLineHeader): the type check is
        // all that is left to do here.
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Pose);
    }

    bool PoseKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    FFDKeyGroup::FFDKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, GUIResources* mGUIResources):
        KeyGroup(tr("FFD"), aLabelWidth, mGUIResources), mAccessor(aAccessor), mKnocker(), mKeyExists(false) {
        mKnocker = new KeyKnocker(tr("FFD"), mGUIResources);
        mKnocker->set([=]() {
            this->mAccessor.knockNewFFD();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);
        }
        setKeyEnabled(false);
        setKeyExists(false, false);
    }

    void FFDKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void FFDKeyGroup::setKeyExists(bool aIsExists, bool aIsKnockable) {
        mKeyExists = aIsExists;
        mKnocker->setVisible(false);
        this->setVisible(aIsExists || aIsKnockable);
    }

    void FFDKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        // The group's easing was its only value, and the timeline header's chip
        // names the selection's now (see gui::TimeLineHeader): the type check is
        // all that is left to do here.
        TIMEKEY_PTR_TYPE_ASSERT(aKey, FFD);
    }

    bool FFDKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    ImageKeyGroup::ImageKeyGroup(gui::prop::Panel& aPanel, KeyAccessor& aAccessor, int aLabelWidth, ViaPoint& aViaPoint, GUIResources* mGUIResources):
        KeyGroup(tr("Image"), aLabelWidth, mGUIResources),
        mAccessor(aAccessor),
        mKnocker(),
        mBrowse(),
        mOffset(),
        mCellSize(),
        mKeyExists(false),
        mViaPoint(aViaPoint) {
        mKnocker = new KeyKnocker(tr("Image"), mGUIResources);
        mKnocker->set([=]() {
            this->knockNewKey();
            this->makeSureExpand();
        });
        aPanel.addGroup(mKnocker);

        {
            aPanel.addGroup(this);

            // browse
            mBrowse = new BrowseItem(this);
            mBrowse->onButtonPushed = [=]() {
                auto resNode = this->mViaPoint.requireOneResource();
                if (resNode) {
                    this->mBrowse->setValue(resNode->data().identifier());
                    this->mAccessor.assignImageResource(*resNode);
                }
            };
            // The key type's primary field lives on its header row, exactly like
            // Position: Image shows Asset inline, then Center and Mesh cell below.
            this->addItem(QString(), mBrowse);

            // offset
            mOffset = new Vector2DItem(this);
            mOffset->setRange(core::Constant::transMin(), core::Constant::transMax());
            mOffset->onValueUpdated = [=](QVector2D, QVector2D aNext) { this->mAccessor.assignImageOffset(-aNext); };
            this->addItem(tr("Center"), mOffset);

            // cell size
            mCellSize = new IntegerItem(this);
            mCellSize->setRange(core::Constant::imageCellSizeMin(), core::Constant::imageCellSizeMax());
            mCellSize->onValueUpdated = [=](int, int aNext) { this->mAccessor.assignImageCellSize(aNext); };
            mCellSize->box().setToolTip(tr("Pixel size of each image mesh cell."));
            this->addItem(tr("Mesh cell"), mCellSize);
        }
        setKeyEnabled(false);
        setKeyExists(false, false);
    }

    void ImageKeyGroup::knockNewKey() {
        auto resNode = mViaPoint.requireOneResource();
        if (resNode) {
            this->mAccessor.knockNewImage(resNode->handle());
        }
    }

    void ImageKeyGroup::setKeyEnabled(bool aEnabled) {
        mKnocker->setEnabled(aEnabled);
        this->setEnabled(aEnabled);
    }

    void ImageKeyGroup::setKeyExists(bool aIsExists, bool aIsKnockable) {
        mKeyExists = aIsExists;
        mKnocker->setVisible(false);
        this->setVisible(aIsExists || aIsKnockable);
    }

    void ImageKeyGroup::setKeyValue(const core::TimeKey* aKey) {
        TIMEKEY_PTR_TYPE_ASSERT(aKey, Image);
        const core::ImageKey::Data& data = ((const core::ImageKey*)aKey)->data();
        mBrowse->setValue(data.resource()->identifier());
        mOffset->setValue(-data.imageOffset());
        mCellSize->setValue(data.gridMesh().cellSize());
    }

    void ImageKeyGroup::setKeyValue(const core::TimeLine& aTimeLine, int aFrame) {
        if (const core::TimeKey* key = aTimeLine.timeKey(core::TimeKeyType_Image, aFrame)) {
            setKeyValue(key);
            return;
        }
        if (!aTimeLine.map(core::TimeKeyType_Image).isEmpty()) {
            if (const core::TimeKey* key = aTimeLine.current().areaImageKey()) {
                setKeyValue(key);
                return;
            }
        }
        if (const core::TimeKey* key = aTimeLine.defaultKey(core::TimeKeyType_Image)) {
            setKeyValue(key);
        }
    }

    bool ImageKeyGroup::keyExists() const { return mKeyExists; }

    //-------------------------------------------------------------------------------------------------
    CurrentKeyPanel::CurrentKeyPanel(
        ViaPoint& aViaPoint, core::Project& aProject, QWidget* aParent, GUIResources* mGUIResources
    ):
        Panel(tr("Properties"), aParent, mGUIResources),
        mViaPoint(aViaPoint),
        mGUIResources(mGUIResources),
        mProject(aProject),
        mTarget(),
        mKeyAccessor(),
        mLabelWidth(),
        mMovePanel(),
        mRotatePanel(),
        mScalePanel(),
        mDepthPanel(),
        mOpaPanel(),
        mFilterPanel(),
        mPosePanel(),
        mFFDPanel(),
        mImagePanel(),
        mMoveKey(nullptr),
        mRotateKey(nullptr),
        mScaleKey(nullptr),
        mDepthKey(nullptr),
        mOpaKey(nullptr),
        mPoseKey(nullptr),
        mFFDKey(nullptr),
        mImageKey(nullptr) {
        mKeyAccessor.setProject(&aProject);
        // The dock already says "Properties"; a second header would repeat it.
        this->setHeaderVisible(false);
        // One fixed label column for every property row. Measuring a sample string
        // made each group's title width differ, which misaligned every key dot.
        mLabelWidth = 72;

        build();
        this->hide();
    }

    void CurrentKeyPanel::setTarget(core::ObjectNode* aTarget) {
        mTarget = aTarget;
        mKeyAccessor.setTarget(aTarget);
        if (mFilterPanel)
            mFilterPanel->setTarget(aTarget);

        if (mTarget && mTarget->timeLine()) {
            this->show();
        } else {
            this->hide();
        }

        updateKey();
    }

    void CurrentKeyPanel::setPlayBackActivity(bool aIsActive) {
        // resume
        if (!this->isEnabled() && !aIsActive) {
            updateKeyExists();
            updateKeyValue();
        }
        this->setEnabled(!aIsActive);
    }

    void CurrentKeyPanel::updateKey() {
        updateKeyExists();
        updateKeyValue();
        if (mFilterPanel)
            mFilterPanel->updateKey();
    }

    void CurrentKeyPanel::updateFrame() {
        if (this->isEnabled()) {
            updateKeyExists();
            updateKeyValue();
            if (mFilterPanel)
                mFilterPanel->updateFrame();
        }
    }

    void CurrentKeyPanel::build() {
        using core::Constant;

        mMovePanel.reset(new MoveKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mRotatePanel.reset(new RotateKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mScalePanel.reset(new ScaleKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mDepthPanel.reset(new DepthKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mOpaPanel.reset(new OpaKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mImagePanel.reset(new ImageKeyGroup(*this, mKeyAccessor, mLabelWidth, mViaPoint, mGUIResources));
        mPosePanel.reset(new PoseKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mFFDPanel.reset(new FFDKeyGroup(*this, mKeyAccessor, mLabelWidth, mGUIResources));
        mFilterPanel.reset(new FilterStackPanel(mProject, this, mGUIResources));
        this->addGroup(mFilterPanel.data());

        // These are property rows, not sections. A single Angle/Rate/Position
        // under its own collapsible header was a second label and a pointless
        // disclosure; the header now IS the property label and stays open.
        for (KeyGroup* group : { (KeyGroup*)mMovePanel.data(), (KeyGroup*)mRotatePanel.data(),
                                 (KeyGroup*)mScalePanel.data(), (KeyGroup*)mDepthPanel.data(),
                                 (KeyGroup*)mOpaPanel.data(), (KeyGroup*)mPosePanel.data(),
                                 (KeyGroup*)mFFDPanel.data(), (KeyGroup*)mImagePanel.data() }) {
            group->setHeaderCollapsible(false);
        }
        // Single-value rows keep the value on the header line.
        mRotatePanel->inlineFields();
        mScalePanel->inlineFields();
        mDepthPanel->inlineFields();
        mOpaPanel->inlineFields();

        auto addKey = [this](KeyGroup* aGroup, const QString& aName, core::TimeKeyType aType,
                             const std::function<void()>& aKnock) {
            auto* button = new KeyStateButton(aName, aType, this);
            aGroup->addHeaderAction(button);
            this->connect(button, &QToolButton::clicked, this, [this, button, aType, aKnock]() {
                if (button->state() == KeyStateButton::State::Keyed)
                    mKeyAccessor.removeKey(aType);
                else
                    aKnock();
            });
            return button;
        };
        mMoveKey = addKey(mMovePanel.data(), tr("Position"), core::TimeKeyType_Move,
                          [this]() { mKeyAccessor.knockNewMove(); });
        mRotateKey = addKey(mRotatePanel.data(), tr("Rotation"), core::TimeKeyType_Rotate,
                            [this]() { mKeyAccessor.knockNewRotate(); });
        mScaleKey = addKey(mScalePanel.data(), tr("Scale"), core::TimeKeyType_Scale,
                           [this]() { mKeyAccessor.knockNewScale(); });
        mDepthKey = addKey(mDepthPanel.data(), tr("Depth"), core::TimeKeyType_Depth,
                           [this]() { mKeyAccessor.knockNewDepth(); });
        mOpaKey = addKey(mOpaPanel.data(), tr("Opacity"), core::TimeKeyType_Opa,
                         [this]() { mKeyAccessor.knockNewOpacity(); });
        mPoseKey = addKey(mPosePanel.data(), tr("Pose"), core::TimeKeyType_Pose,
                          [this]() { mKeyAccessor.knockNewPose(); });
        mFFDKey = addKey(mFFDPanel.data(), tr("FFD"), core::TimeKeyType_FFD,
                         [this]() { mKeyAccessor.knockNewFFD(); });
        mImageKey = addKey(mImagePanel.data(), tr("Image"), core::TimeKeyType_Image,
                           [this]() { mImagePanel->knockNewKey(); });

        this->adjustSize();
        this->addStretch();
    }

    void CurrentKeyPanel::updateKeyExists() {
        const bool enabled = mTarget && mTarget->timeLine();

        mMovePanel->setKeyEnabled(enabled);
        mRotatePanel->setKeyEnabled(enabled);
        mScalePanel->setKeyEnabled(enabled);
        mDepthPanel->setKeyEnabled(enabled);
        mOpaPanel->setKeyEnabled(enabled);
        mFilterPanel->setEnabled(enabled);
        mPosePanel->setKeyEnabled(enabled);
        mFFDPanel->setKeyEnabled(enabled);
        mImagePanel->setKeyEnabled(enabled);

        if (enabled) {
            const core::TimeLine& timeLine = *mTarget->timeLine();
            const int frame = mProject.animator().currentFrame().get();
            const bool hasAreaBone = timeLine.current().bone().areaKey();
            const bool hasAnyMesh = mTarget->hasAnyMesh();
            const bool hasAnyImage = mTarget->hasAnyImage();

            mMovePanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Move, frame));
            mRotatePanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Rotate, frame));
            mScalePanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Scale, frame));
            mDepthPanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Depth, frame));
            mOpaPanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Opa, frame));
            mPosePanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Pose, frame), hasAreaBone);
            mFFDPanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_FFD, frame), hasAnyMesh);
            mImagePanel->setKeyExists(timeLine.hasTimeKey(core::TimeKeyType_Image, frame), hasAnyImage);

            auto state = [&timeLine, frame](core::TimeKeyType aType) {
                if (timeLine.map(aType).isEmpty())
                    return KeyStateButton::State::Static;
                return timeLine.hasTimeKey(aType, frame) ? KeyStateButton::State::Keyed
                                                         : KeyStateButton::State::Animated;
            };
            mMoveKey->setState(state(core::TimeKeyType_Move));
            mRotateKey->setState(state(core::TimeKeyType_Rotate));
            mScaleKey->setState(state(core::TimeKeyType_Scale));
            mDepthKey->setState(state(core::TimeKeyType_Depth));
            mOpaKey->setState(state(core::TimeKeyType_Opa));
            mPoseKey->setState(state(core::TimeKeyType_Pose));
            mFFDKey->setState(state(core::TimeKeyType_FFD));
            mImageKey->setState(state(core::TimeKeyType_Image));
        }
    }

    void CurrentKeyPanel::updateKeyValue() {
        if (mTarget && mTarget->timeLine()) {
            auto& timeLine = *mTarget->timeLine();
            const int frame = mProject.animator().currentFrame().get();

            // One value path for every state: an exact key, the evaluated value
            // between keys, or the default key of a static property.
            mMovePanel->setKeyValue(timeLine, frame);
            mRotatePanel->setKeyValue(timeLine, frame);
            mScalePanel->setKeyValue(timeLine, frame);
            mDepthPanel->setKeyValue(timeLine, frame);
            mOpaPanel->setKeyValue(timeLine, frame);
            mImagePanel->setKeyValue(timeLine, frame);

            // Structural keys have no field of their own; only their key state
            // and their knocker matter here.
            if (mPosePanel->keyExists()) {
                mPosePanel->setKeyValue(timeLine.timeKey(core::TimeKeyType_Pose, frame));
            }
            if (mFFDPanel->keyExists()) {
                mFFDPanel->setKeyValue(timeLine.timeKey(core::TimeKeyType_FFD, frame));
            }
        }
    }

} // namespace prop
} // namespace gui
