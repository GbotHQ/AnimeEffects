#include "util/CollDetect.h"
#include "cmnd/ScopedMacro.h"
#include "ctrl/CmndName.h"
#include "ctrl/bone/bone_PaintInflMode.h"
#include "ctrl/bone/bone_Renderer.h"
#include "ctrl/bone/bone_WeightHeatmap.h"
#include "ctrl/bone/bone_Notifier.h"

using namespace core;

namespace ctrl {
namespace bone {

    PaintInflMode::PaintInflMode(Project& aProject, const Target& aTarget, KeyOwner& aKey):
        mProject(aProject),
        mTarget(*aTarget.node),
        mKeyOwner(aKey),
        mTargetMtx(aTarget.mtx),
        mTargetInvMtx(aTarget.invMtx),
        mFocuser(),
        mActiveBone(),
        mHoverBone(),
        mCommandRef(),
        mBrush(),
        mParam(),
        mStroke(),
        mCursorPos(),
        mIsBrushDrawing() {
        XC_PTR_ASSERT(mKeyOwner.key);
        mFocuser.setTopBones(mKeyOwner.key->data().topBones());
        mFocuser.setFocusConnector(true);
        mFocuser.setTargetMatrix(mTargetMtx);
    }

    void PaintInflMode::updateParam(const BoneParam& aParam) {
        mParam = aParam;
        mBrush.setRadius(aParam.brush.radius);
    }

    bool PaintInflMode::updateCursor(const CameraInfo& aCamera, const AbstractCursor& aCursor) {
        mCursorPos = aCursor.worldPos();
        mBrush.setCenter(mCursorPos);
        mBrush.setRadius(mParam.brush.radius);

        // Hovering a joint is the cheap way to say which bone the next stroke is
        // about; the heatmap keeps the last one after the pointer leaves.
        Bone2* hovered = mFocuser.update(aCamera, aCursor.screenPos());
        if (hovered) {
            mHoverBone = hovered;
        }

        // RIGHT CLICK SELECTS; LEFT IS ALWAYS THE STROKE. Clicking a joint with
        // the left button used to be indistinguishable from starting a stroke,
        // so the target had to be chosen by a separate mode.
        if (aCursor.emitsRightPressedEvent()) {
            if (!mIsBrushDrawing && hovered) {
                selectBone(hovered);
            }
            return true;
        }
        if (aCursor.emitsRightDraggedEvent() || aCursor.emitsRightReleasedEvent()) {
            return true;
        }

        if (aCursor.emitsLeftPressedEvent()) {
            if (!mActiveBone) {
                Bone2* chosen = hovered ? hovered : mKeyOwner.key->data().nearestBone(targetPos(aCursor));
                selectBone(chosen);
            }
            mIsBrushDrawing = true;
            mCommandRef = nullptr;
            mStroke.begin(mParam.brush, mCursorPos, aCursor.pressure(), [this](const BrushStamp& aStamp) {
                updatePaint(aStamp);
            });
        } else if (aCursor.emitsLeftDraggedEvent()) {
            mStroke.move(mCursorPos, aCursor.pressure());
        } else if (aCursor.emitsLeftReleasedEvent()) {
            mStroke.end();
            mIsBrushDrawing = false;
            mCommandRef = nullptr;
        }

        return true;
    }

    bool PaintInflMode::updatePaint(const BrushStamp& aStamp) {
        Bone2* bone = mActiveBone;
        if (!bone || !bone->parent())
            return false;

        const QVector2D brushPos = (mTargetInvMtx.map(QVector3D(aStamp.center))).toVector2D();
        const float brushRange = (mTargetInvMtx.map(QVector3D(aStamp.radius, 0.0f, 0.0f)) - mTargetInvMtx.map(QVector3D()))
                                     .toVector2D()
                                     .length();
        // the hard core is the shared brush hardness (was a hard-wired 0.3)
        const float brushCoreRange = aStamp.hardness * brushRange;

        const QVector2D ppos = bone->parent()->worldPos();
        const QVector2D cpos = bone->worldPos();
        const util::Segment2D boneSeg(ppos, cpos - ppos);
        const float ratio = util::CollDetect::getRawSegmentRate(boneSeg, brushPos);

        if (!xc_contains(ratio, 0.0f, 1.0f))
            return false;

        util::Segment2D v;
        v.start = boneSeg.start + boneSeg.dir * ratio;
        v.dir = brushPos - v.start;
        const float vlen = v.dir.length();

        auto linearRange = bone->blendedRange(ratio);
        const float append = (vlen + brushRange) - linearRange.x();

        if (append < 0)
            return false;
        if (vlen - brushRange > std::max(linearRange.x(), brushCoreRange))
            return false;

        // the shared falloff over the distance from the bone to the stamp
        const float weight = aStamp.amount * brushFalloff(vlen, brushRange, aStamp.hardness);

        std::array<QVector2D, 2> prevRange = {bone->range(0), bone->range(1)};
        std::array<QVector2D, 2> nextRange = prevRange;
        for (int i = 0; i < 2; ++i) {
            const float rangeAdd = weight * append * (i == 0 ? (1.0f - ratio) : ratio);
            nextRange[i].setX(std::min(nextRange[i].x() + rangeAdd, brushRange));
            nextRange[i].setY(std::min(nextRange[i].y() + rangeAdd, brushRange));
        }
        assignInfluence(*bone, prevRange, nextRange);
        notifyAssign();
        return true;
    }

    void PaintInflMode::renderQt(const RenderInfo& aInfo, QPainter& aPainter) {
        // Under the envelope ink and the bones: the weights are the field the
        // envelopes produce, and the skeleton has to stay readable above it.
        if (mParam.showWeightHeatmap) {
            Bone2* active = mActiveBone ? mActiveBone : mHoverBone;
            if (active) {
                drawWeightHeatmap(aPainter, aInfo, *mKeyOwner.key, *active, mTargetMtx);
            }
        }

        bone::Renderer renderer(aPainter, aInfo);
        renderer.setAntialiasing(true);
        renderer.setFocusConnector(true);
        renderer.setTargetMatrix(mTargetMtx);

        for (auto bone : mKeyOwner.key->data().topBones()) {
            renderer.renderInfluence(bone);
        }

        for (auto bone : mKeyOwner.key->data().topBones()) {
            renderer.renderBones(bone);
        }

        renderer.renderBrush(mBrush, mIsBrushDrawing, mParam.brush.hardness);
    }

    void PaintInflMode::selectBone(Bone2* aBone) {
        if (!aBone)
            return;
        mFocuser.select(*aBone);
        mActiveBone = aBone;
    }

    QVector2D PaintInflMode::targetPos(const AbstractCursor& aCursor) const {
        return (mTargetInvMtx.map(QVector3D(aCursor.worldPos()))).toVector2D();
    }

    void PaintInflMode::assignInfluence(
        core::Bone2& aTarget, const std::array<QVector2D, 2>& aPrev, const std::array<QVector2D, 2>& aNext
    ) {
        XC_ASSERT(!mKeyOwner.owns());
        auto eventType = TimeLineEvent::Type_ChangeKeyValue;
        cmnd::Stack& stack = mProject.commandStack();

        // modify
        if (mCommandRef && stack.isModifiable(mCommandRef)) {
            mCommandRef->push(aTarget, aPrev, aNext);
        } else {
            cmnd::ScopedMacro macro(stack, CmndName::tr("Update bone influence"));
            macro.grabListener(new Notifier(mProject, mTarget, *mKeyOwner.key, eventType));

            // push command
            mCommandRef = new FuzzyAssignInfluence();
            mCommandRef->push(aTarget, aPrev, aNext);
            stack.push(mCommandRef);
        }
    }

    void PaintInflMode::notifyAssign() {
        // singleshot notify
        auto eventType = TimeLineEvent::Type_ChangeKeyValue;
        Notifier notifier(mProject, mTarget, *mKeyOwner.key, eventType);
        notifier.notify();
    }

} // namespace bone
} // namespace ctrl
