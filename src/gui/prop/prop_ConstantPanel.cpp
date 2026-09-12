#include "cmnd/ScopedMacro.h"
#include "cmnd/BasicCommands.h"
#include "core/Constant.h"
#include "core/ImageKey.h"
#include "core/LayerMaskBake.h"
#include "core/TimeLine.h"
#include "ctrl/CmndName.h"
#include "gui/ResourceDialog.h"
#include "gui/prop/prop_ConstantPanel.h"
#include "gui/prop/prop_Items.h"
#include "img/ResourceData.h"
#include <QEvent>
#include <QTimer>

namespace {
class ObjectNodeAttrNotifier: public cmnd::Listener {
public:
    ObjectNodeAttrNotifier(core::Project& aProject, core::ObjectNode& aNode): mProject(aProject), mNode(aNode) {}
    virtual void onExecuted() { mProject.onNodeAttributeModified(mNode, false); }
    virtual void onUndone() { mProject.onNodeAttributeModified(mNode, true); }
    virtual void onRedone() { mProject.onNodeAttributeModified(mNode, false); }

private:
    core::Project& mProject;
    core::ObjectNode& mNode;
};

// The theme stylesheets declare their own QComboBox min-width, which is
// re-applied during style polish and overrides any minimumWidth set in C++
// (the panel is built before the theme stylesheets are loaded). Re-assert a
// content-based floor after every polish/style/font change so the longest
// mode names ("Linear Dodge", "Lighter Color") never ellipsize in narrow
// docks. sizeHint() is recomputed at that point, so the floor tracks the
// active locale font.
class BlendComboWidthGuard: public QObject {
public:
    BlendComboWidthGuard(QComboBox* aCombo): QObject(aCombo) {
        aCombo->installEventFilter(this);
    }

    bool eventFilter(QObject* aWatched, QEvent* aEvent) override {
        if (aEvent->type() == QEvent::Polish ||
            aEvent->type() == QEvent::StyleChange ||
            aEvent->type() == QEvent::FontChange) {
            auto* combo = static_cast<QComboBox*>(aWatched);
            QTimer::singleShot(0, combo, [combo]() {
                combo->setMinimumWidth(combo->sizeHint().width() + 8);
            });
        }
        return QObject::eventFilter(aWatched, aEvent);
    }
};
// the layer's image resource (the mask lives there, next to the pixels)
const img::ResourceData* imageResourceOf(const core::ObjectNode* aNode) {
    if (!aNode) {
        return nullptr;
    }
    const core::TimeLine* timeLine = aNode->timeLine();
    if (!timeLine) {
        return nullptr;
    }
    const core::TimeKey* key = timeLine->defaultKey(core::TimeKeyType_Image);
    return key ? static_cast<const core::ImageKey*>(key)->data().resource().get() : nullptr;
}

img::ResourceData* imageResourceOf(core::ObjectNode* aNode) {
    return const_cast<img::ResourceData*>(imageResourceOf(const_cast<const core::ObjectNode*>(aNode)));
}

} // namespace

namespace gui {
namespace prop {

    //-------------------------------------------------------------------------------------------------
    ConstantPanel::ConstantPanel(ViaPoint& aViaPoint, core::Project& aProject, QWidget* aParent, GUIResources* aGUIResources):
        Panel(tr("Constants"), aParent, aGUIResources),
        mViaPoint(aViaPoint),
        mProject(aProject),
        mGUIResources(aGUIResources),
        mTarget(),
        mLabelWidth(),
        mRenderingAttributes(),
        mBlendMode(),
        mClipped(),
        mDstBlendIfGroup(),
        mSrcBlendIfGroup(),
        mDstBlendIf(),
        mSrcBlendIf(),
        mDstBlendIfReset(),
        mSrcBlendIfReset(),
        mMaskGroup(),
        mMaskEnabled(),
        mMaskInverted(),
        mMaskBake() {
        mLabelWidth = this->fontMetrics().boundingRect(tr("MaxTextWidth")).width();

        build();
        this->hide();
    }

    void ConstantPanel::setTarget(core::ObjectNode* aTarget) {
        mTarget = aTarget;

        if (mTarget) {
            this->show();
        } else {
            this->hide();
        }

        updateAttribute();
    }

    void ConstantPanel::setPlayBackActivity(bool aIsActive) { this->setEnabled(!aIsActive); }

    void ConstantPanel::build() {
        using core::Constant;

        mRenderingAttributes = new AttrGroup(tr("Rendering"), mLabelWidth, mGUIResources);
        {
            this->addGroup(mRenderingAttributes);

            // blend mode
            // grouped like Photoshop (darken / lighten / contrast / difference /
            // color), with the CSP-only modes next to their plain equivalents;
            // groups are split by thin separator lines. Each mode carries its
            // img::BlendMode value in Qt::UserRole so the row index does not
            // need to match the enum order.
            mBlendMode = new ComboItem(mRenderingAttributes);
            {
                auto& box = mBlendMode->box();
                box.setSizeAdjustPolicy(QComboBox::AdjustToContents);
                auto addMode = [&](img::BlendMode aMode) {
                    box.addItem(img::getBlendNameFromBlendMode(aMode), (int)aMode);
                };
                auto addSeparator = [&]() { box.insertSeparator(box.count()); };

                addMode(img::BlendMode_Normal);
                addSeparator();
                addMode(img::BlendMode_Darken);
                addMode(img::BlendMode_Multiply);
                addMode(img::BlendMode_ColorBurn);
                addMode(img::BlendMode_LinearBurn);
                addMode(img::BlendMode_DarkerColor);
                addSeparator();
                addMode(img::BlendMode_Lighten);
                addMode(img::BlendMode_Screen);
                addMode(img::BlendMode_ColorDodge);
                addMode(img::BlendMode_GlowDodge);
                addMode(img::BlendMode_LinearDodge);
                addMode(img::BlendMode_AddGlow);
                addMode(img::BlendMode_LighterColor);
                addSeparator();
                addMode(img::BlendMode_Overlay);
                addMode(img::BlendMode_SoftLight);
                addMode(img::BlendMode_HardLight);
                addMode(img::BlendMode_VividLight);
                addMode(img::BlendMode_LinearLight);
                addMode(img::BlendMode_PinLight);
                addMode(img::BlendMode_HardMix);
                addSeparator();
                addMode(img::BlendMode_Difference);
                addMode(img::BlendMode_Exclusion);
                addMode(img::BlendMode_Subtract);
                addMode(img::BlendMode_Divide);
                addSeparator();
                addMode(img::BlendMode_Hue);
                addMode(img::BlendMode_Saturation);
                addMode(img::BlendMode_Color);
                addMode(img::BlendMode_Luminosity);

                new BlendComboWidthGuard(&box);

                mBlendMode->onValueUpdated = [=](int, int aNext) {
                    auto mode = (img::BlendMode)mBlendMode->box().itemData(aNext).toInt();
                    assignBlendMode(this->mProject, this->mTarget, mode);
                };
            }
            mRenderingAttributes->addItem(tr("Blend"), mBlendMode);

            // clipped
            mClipped = new CheckItem(mRenderingAttributes);
            mClipped->onValueUpdated = [=](bool aNext) { assignClipped(this->mProject, this->mTarget, aNext); };
            mRenderingAttributes->addItem(tr("Clipped"), mClipped);
        }

        // Blend If: Photoshop's per-layer blending ranges. The weights are applied by the
        // renderer against the captured backdrop, so they stay live (and are imported from
        // PSD files); values are the split-slider pairs, 0..255, default = no restriction.
        auto buildBlendIfGroup = [&](bool aDst) {
            auto* group = new AttrGroup(
                aDst ? tr("Blend If: Underlying Layer") : tr("Blend If: This Layer"), mLabelWidth, mGUIResources);
            this->addGroup(group);

            IntegerItem** items = aDst ? mDstBlendIf : mSrcBlendIf;
            // literal tr() calls (not a label table) so lupdate extracts them
            const QString labels[4] = {tr("Black low"), tr("Black high"), tr("White low"), tr("White high")};
            for (int i = 0; i < 4; ++i) {
                items[i] = new IntegerItem(group);
                items[i]->setRange(0, 255);
                items[i]->onValueUpdated = [=](int, int) {
                    core::BlendIf::Range range;
                    range.blackLo = (uint8_t)items[0]->value();
                    range.blackHi = (uint8_t)items[1]->value();
                    range.whiteLo = (uint8_t)items[2]->value();
                    range.whiteHi = (uint8_t)items[3]->value();
                    assignBlendIfSide(this->mProject, this->mTarget, aDst, range);
                };
                group->addItem(labels[i], items[i]);
            }

            auto* reset = new ButtonItem(group);
            reset->button().setText(tr("Reset (no restriction)"));
            reset->onPressed = [=]() { resetBlendIfSide(this->mProject, this->mTarget, aDst); };
            group->addItem(tr("Range"), reset);
            if (aDst) {
                mDstBlendIfReset = reset;
            } else {
                mSrcBlendIfReset = reset;
            }
            return group;
        };
        mDstBlendIfGroup = buildBlendIfGroup(true);
        mSrcBlendIfGroup = buildBlendIfGroup(false);

        // layer mask: imported from the PSD's user-mask channel, or baked from the source
        // range below
        mMaskGroup = new AttrGroup(tr("Mask"), mLabelWidth, mGUIResources);
        {
            this->addGroup(mMaskGroup);

            mMaskEnabled = new CheckItem(mMaskGroup);
            mMaskEnabled->onValueUpdated = [=](bool aNext) {
                assignMaskEnabled(this->mProject, this->mTarget, aNext);
            };
            mMaskGroup->addItem(tr("Apply"), mMaskEnabled);

            mMaskInverted = new CheckItem(mMaskGroup);
            mMaskInverted->onValueUpdated = [=](bool aNext) {
                assignMaskInverted(this->mProject, this->mTarget, aNext);
            };
            mMaskGroup->addItem(tr("Invert"), mMaskInverted);

            mMaskBake = new ButtonItem(mMaskGroup);
            mMaskBake->button().setText(tr("Bake This Layer range"));
            mMaskBake->onPressed = [=]() { bakeSourceRange(this->mProject, this->mTarget); };
            mMaskGroup->addItem(tr("Create mask"), mMaskBake);
        }

        this->addStretch();
    }

    void ConstantPanel::updateBlendIfItems() {
        const bool available = mTarget && mTarget->renderer();
        const core::BlendIf blendIf = available ? mTarget->renderer()->blendIf() : core::BlendIf();

        IntegerItem** dst = mDstBlendIf;
        IntegerItem** src = mSrcBlendIf;
        const core::BlendIf::Range ranges[2] = {blendIf.dst, blendIf.src};
        for (int side = 0; side < 2; ++side) {
            IntegerItem** items = side == 0 ? dst : src;
            const core::BlendIf::Range& r = ranges[side];
            const int values[4] = {r.blackLo, r.blackHi, r.whiteLo, r.whiteHi};
            for (int i = 0; i < 4; ++i) {
                items[i]->setItemEnabled(available);
                items[i]->setValue(values[i]);
            }
        }
        mDstBlendIfReset->setItemEnabled(available);
        mSrcBlendIfReset->setItemEnabled(available);
    }

    void ConstantPanel::updateMaskItems() {
        const img::ResourceData* res = imageResourceOf(mTarget);
        const bool hasMask = res && res->hasMask();
        mMaskEnabled->setItemEnabled(hasMask);
        mMaskInverted->setItemEnabled(hasMask);
        mMaskEnabled->setValue(hasMask && res->isMaskEnabled(), false);
        mMaskInverted->setValue(hasMask && res->isMaskInverted(), false);
        // the bake action needs an image and a non-default source range
        mMaskBake->setItemEnabled(mTarget && core::LayerMaskBake::isAvailable(*mTarget));
    }

    void ConstantPanel::updateAttribute() {
        if (mTarget) {
            if (mTarget->renderer()) {
                auto& renderer = *(mTarget->renderer());
                if (renderer.hasBlendMode()) {
                    mBlendMode->setItemEnabled(true);
                    auto mode = renderer.blendMode();
                    auto row = mBlendMode->box().findData((int)mode);
                    mBlendMode->setValue(row >= 0 ? row : 0, false);
                } else {
                    mBlendMode->setItemEnabled(false);
                }

                mClipped->setItemEnabled(true);
                mClipped->setValue(renderer.isClipped(), false);
            } else {
                mBlendMode->setItemEnabled(false);
                mClipped->setItemEnabled(false);
            }
        }

        updateBlendIfItems();
        updateMaskItems();
    }

    void ConstantPanel::assignBlendMode(core::Project& aProject, core::ObjectNode* aTarget, img::BlendMode aValue) {
        XC_ASSERT(aTarget);
        XC_PTR_ASSERT(aTarget->renderer());
        if (!aTarget || !aTarget->renderer())
            return; // fail-safe code

        auto prev = aTarget->renderer()->blendMode();
        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("update a blending mode"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        auto exec = [=]() { aTarget->renderer()->setBlendMode(aValue); };
        auto undo = [=]() { aTarget->renderer()->setBlendMode(prev); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

    void ConstantPanel::assignBlendIfSide(
        core::Project& aProject, core::ObjectNode* aTarget, bool aDst, const core::BlendIf::Range& aRange
    ) {
        if (!aTarget || !aTarget->renderer()) {
            return;
        }

        const core::BlendIf prev = aTarget->renderer()->blendIf();
        if ((aDst ? prev.dst : prev.src) == aRange) {
            return; // no change (the spin boxes also fire on programmatic updates)
        }
        core::BlendIf next = prev;
        (aDst ? next.dst : next.src) = aRange;

        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("update blend if ranges"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        auto exec = [=]() { aTarget->renderer()->setBlendIf(next); };
        auto undo = [=]() { aTarget->renderer()->setBlendIf(prev); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

    void ConstantPanel::resetBlendIfSide(core::Project& aProject, core::ObjectNode* aTarget, bool aDst) {
        assignBlendIfSide(aProject, aTarget, aDst, core::BlendIf::Range());
    }

    void ConstantPanel::assignMaskEnabled(core::Project& aProject, core::ObjectNode* aTarget, bool aValue) {
        img::ResourceData* res = imageResourceOf(aTarget);
        if (!res || !res->hasMask() || res->isMaskEnabled() == aValue) {
            return;
        }

        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("update a layer mask"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        auto exec = [=]() { res->setMaskEnabled(aValue); };
        auto undo = [=]() { res->setMaskEnabled(!aValue); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

    void ConstantPanel::assignMaskInverted(core::Project& aProject, core::ObjectNode* aTarget, bool aValue) {
        img::ResourceData* res = imageResourceOf(aTarget);
        if (!res || !res->hasMask() || res->isMaskInverted() == aValue) {
            return;
        }

        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("update a layer mask"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        auto exec = [=]() { res->setMaskInverted(aValue); };
        auto undo = [=]() { res->setMaskInverted(!aValue); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

    void ConstantPanel::bakeSourceRange(core::Project& aProject, core::ObjectNode* aTarget) {
        core::LayerMaskBake bake;
        if (!aTarget || !core::LayerMaskBake::prepare(*aTarget, bake)) {
            return; // fail-safe: nothing to bake
        }
        auto* layer = dynamic_cast<core::LayerNode*>(aTarget);
        if (!layer) {
            return;
        }

        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("bake blend if range into a mask"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        // the mask pixels are edited in place; the GPU copy is rebuilt lazily by the render
        // path (ImageKey::syncMaskTexture), so no GL context is needed here
        auto exec = [=]() { bake.bake(*layer); };
        auto undo = [=]() { bake.restore(*layer); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

    void ConstantPanel::assignClipped(core::Project& aProject, core::ObjectNode* aTarget, bool aValue) {
        XC_ASSERT(aTarget);
        XC_PTR_ASSERT(aTarget->renderer());
        if (!aTarget || !aTarget->renderer())
            return; // fail-safe code

        const bool prev = aTarget->renderer()->isClipped();
        cmnd::ScopedMacro macro(aProject.commandStack(), CmndName::tr("update a clipping flag"));
        macro.grabListener(new ObjectNodeAttrNotifier(aProject, *aTarget));

        auto exec = [=]() { aTarget->renderer()->setClipped(aValue); };
        auto undo = [=]() { aTarget->renderer()->setClipped(prev); };
        aProject.commandStack().push(new cmnd::Delegatable(exec, undo));
    }

} // namespace prop
} // namespace gui
