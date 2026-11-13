#include "core/TimeKeyExpans.h"
#include "core/ObjectNodeUtil.h"
#include "ctrl/SRTEditor.h"
#include "ctrl/srt/srt_MoveMode.h"
#include "ctrl/srt/srt_CentroidMode.h"

using namespace core;

namespace ctrl {


//-----------------------------------------------------------------------------------
SRTEditor::SRTEditor(Project& aProject, UILogger& aUILogger):
    mProject(aProject), mLifeLink(), mUILogger(aUILogger), mParam(), mTarget(), mKeyOwner() {}

SRTEditor::~SRTEditor() { finalize(); }

bool SRTEditor::initializeKey(TimeLine& aLine, QString* aMessage) {
    TimeKeyExpans& current = aLine.current();
    const int frame = mProject.animator().currentFrame().get();

    // A PROPERTY WITH NO EXPLICIT KEYS IS STATIC. Editing it writes the BASE VALUE,
    // exactly as the inspector's KeyAccessor::editFrame does; only an animated channel
    // (or a checked "Create ... key" box) gets a new key at the playhead. The old code
    // treated every missing frame key as owned and pushed it on the first drag, which
    // is the "it creates a key for no reason" report.
    const auto keyFor = [&](TimeKeyType aType, bool aForce, bool& aOwns, bool& aIsDefault) -> TimeKey* {
        const TimeLine::MapType& map = aLine.map(aType);
        const bool atFrame = map.contains(frame);
        const bool animated = !map.isEmpty();
        aIsDefault = false;
        aOwns = !atFrame && (animated || aForce);
        if (atFrame)
            return map.value(frame);
        if (aOwns)
            return nullptr; // the caller seeds its typed key from the current transform
        aIsDefault = true;
        switch (aType) {
        case TimeKeyType_Move: return getOrCreateBaseValue<MoveKey, TimeKeyType_Move>(aLine);
        case TimeKeyType_Rotate: return getOrCreateBaseValue<RotateKey, TimeKeyType_Rotate>(aLine);
        case TimeKeyType_Scale: return getOrCreateBaseValue<ScaleKey, TimeKeyType_Scale>(aLine);
        default: return nullptr;
        }
    };

    // moveKey
    mKeyOwner.isBound = aLine.current().bone().isUnderOfBinding() || aLine.working().bone().isUnderOfBinding();
    if (mKeyOwner.isBound) {
        if (aMessage) {
            *aMessage = UILog::tr("The transformed object has one or more bones bound to it, SRT keys may not work as expected.");
            mUILogger.pushLog(UILog::tr("SRT Editor : ") + *aMessage, UILogType_Warn);
        }
    }

    {
        TimeKey* base = keyFor(TimeKeyType_Move, mParam.necessarilyMove, mKeyOwner.ownsMoveKey,
                               mKeyOwner.moveIsBaseValue);
        if (mKeyOwner.ownsMoveKey) {
            mKeyOwner.moveKey = new MoveKey();
            const util::Easing::Param aParam = util::Easing::defaultParam();
            for (int component = 0; component < MoveKey::kComponents; ++component) {
                mKeyOwner.moveKey->data().easing(component) = aParam;
            }
            mKeyOwner.moveKey->setPos(current.srt().pos());
            mKeyOwner.moveKey->setCentroid(current.srt().centroid());
        } else {
            mKeyOwner.moveKey = static_cast<MoveKey*>(base);
        }
    }

    // rotateKey
    {
        TimeKey* base = keyFor(TimeKeyType_Rotate, mParam.necessarilyRotate, mKeyOwner.ownsRotateKey,
                               mKeyOwner.rotateIsBaseValue);
        if (mKeyOwner.ownsRotateKey) {
            mKeyOwner.rotateKey = new RotateKey();
            const util::Easing::Param aParam = util::Easing::defaultParam();
            for (int component = 0; component < RotateKey::kComponents; ++component) {
                mKeyOwner.rotateKey->data().easing(component) = aParam;
            }
            mKeyOwner.rotateKey->setRotate(current.srt().rotate());
        } else {
            mKeyOwner.rotateKey = static_cast<RotateKey*>(base);
        }
    }

    // scaleKey
    {
        TimeKey* base = keyFor(TimeKeyType_Scale, mParam.necessarilyScale, mKeyOwner.ownsScaleKey,
                               mKeyOwner.scaleIsBaseValue);
        if (mKeyOwner.ownsScaleKey) {
            mKeyOwner.scaleKey = new ScaleKey();
            const util::Easing::Param aParam = util::Easing::defaultParam();
            for (int component = 0; component < ScaleKey::kComponents; ++component) {
                mKeyOwner.scaleKey->data().easing(component) = aParam;
            }
            mKeyOwner.scaleKey->setScale(current.srt().scale());
        } else {
            mKeyOwner.scaleKey = static_cast<ScaleKey*>(base);
        }
    }

    // check validity
    XC_ASSERT(mKeyOwner);

    // setup matrix
    if (!mKeyOwner.updatePosture(current)) {
        mKeyOwner.deleteOwningKeys();
        if (aMessage) {
            *aMessage = UILog::tr("An object with invalid posture was given.");
        }
        return false;
    }

    return true;
}

void SRTEditor::finalize() {
    mCurrent.reset();
    mKeyOwner.deleteOwningKeys();
}

void SRTEditor::createMode() {
    mCurrent.reset();

    if (!mTarget || !mKeyOwner)
        return;

    switch (mParam.mode) {
    case 0:
        mCurrent.reset(new srt::MoveMode(mProject, *mTarget, mKeyOwner));
        break;
    case 1:
        mCurrent.reset(new srt::CentroidMode(mProject, *mTarget, mKeyOwner));
        break;
    default:
        break;
    }

    if (mCurrent) {
        mCurrent->updateParam(mParam);
    }
}

bool SRTEditor::setTarget(ObjectNode* aTarget) {
    finalize();

    mTarget = nullptr;

    if (aTarget && aTarget->timeLine()) {
        mTarget = aTarget;
        QString message;
        if (initializeKey(*mTarget->timeLine(), &message)) {
            createMode();
        } else {
            mTarget = nullptr;
            if (!message.isEmpty()) {
                mUILogger.pushLog(UILog::tr("SRT Editor : ") + message, UILogType_Warn);
            }
        }
    }
    return mTarget && mKeyOwner;
}

void SRTEditor::updateParam(const SRTParam& aParam) {
    const SRTParam prev = mParam;
    mParam = aParam;

    // The three create-key toggles decide whether a missing frame key is an owned
    // key or the static default, so changing one while the tool is up must rebuild
    // the key state. A mode change already rebuilt it.
    if (prev.mode != mParam.mode || prev.necessarilyMove != mParam.necessarilyMove
        || prev.necessarilyRotate != mParam.necessarilyRotate
        || prev.necessarilyScale != mParam.necessarilyScale) {
        resetCurrentTarget();
    }

    if (mCurrent) {
        mCurrent->updateParam(mParam);
    }
}

bool SRTEditor::updateCursor(const CameraInfo& aCamera, const AbstractCursor& aCursor) {
    if (mCurrent) {
        return mCurrent->updateCursor(aCamera, aCursor);
    }
    return false;
}

void SRTEditor::updateEvent(EventType) { resetCurrentTarget(); }

void SRTEditor::resetCurrentTarget() {
    finalize();

    if (mTarget) {
        initializeKey(*mTarget->timeLine());
        createMode();
    }
}

void SRTEditor::renderQt(const RenderInfo& aInfo, QPainter& aPainter) {
    if (mCurrent) {
        return mCurrent->renderQt(aInfo, aPainter);
    }
}

} // namespace ctrl
