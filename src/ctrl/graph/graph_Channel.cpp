#include "ctrl/graph/graph_Channel.h"

#include <QObject>

#include "core/TimeLine.h"
#include "core/MoveKey.h"
#include "core/RotateKey.h"
#include "core/ScaleKey.h"
#include "core/DepthKey.h"
#include "core/OpaKey.h"
#include "core/BlurKey.h"
#include "core/HsvKey.h"
#include "core/ImageKey.h"

namespace ctrl {
namespace graph {

Channel::Channel(): mType(core::TimeKeyType_Move), mComponent(0) {}

Channel::Channel(core::TimeKeyType aType, int aComponent): mType(aType), mComponent(aComponent) {}

int Channel::componentCount(core::TimeKeyType aType) {
    // The count is also the component numbering used by value() and
    // componentName(): 0..count-1 in the order the concrete Data class exposes
    // its fields. Bone, pose, mesh and FFD keys are structural (no easing
    // param), and TERM is not a type, so all of them count 0.
    switch (aType) {
    case core::TimeKeyType_Move: return 2;   // pos x, y
    case core::TimeKeyType_Rotate: return 1; // rotate
    case core::TimeKeyType_Scale: return 2;  // scale x, y
    case core::TimeKeyType_Depth: return 1;  // depth
    case core::TimeKeyType_Opa: return 1;    // opacity
    case core::TimeKeyType_Blur: return 3;   // blurX, blurY, angleDeg
    case core::TimeKeyType_HSV: return 3;    // hue, saturation, value
    case core::TimeKeyType_Image: return 2;  // imageOffset x, y
    default: return 0;
    }
}

QVector<Channel> Channel::channelsOf(core::TimeKeyType aType) {
    const int count = componentCount(aType);
    QVector<Channel> channels;
    channels.reserve(count);
    for (int i = 0; i < count; ++i) {
        channels.append(Channel(aType, i));
    }
    return channels;
}

QVector<core::TimeKeyType> Channel::graphableTypes() {
    // The timeline's own order of operations is the display order, so the graph
    // lists the key types exactly as the timeline tracks do.
    QVector<core::TimeKeyType> types;
    types.reserve(core::TimeKeyType_TERM);
    for (int i = 0; i < core::TimeKeyType_TERM; ++i) {
        const core::TimeKeyType type = core::TimeLine::getTimeKeyTypeInOrderOfOperations(i);
        if (componentCount(type) > 0) {
            types.append(type);
        }
    }
    return types;
}

float Channel::value(const core::TimeKey& aKey) const {
    // A component outside the type's count is not a channel any caller can get
    // from channelsOf(), but a Channel is a plain copyable value, so a hand-made
    // one must stay harmless rather than read past a vector's end.
    if (mComponent < 0 || componentCount(mType) <= mComponent) {
        return 0.0f;
    }

    // Each read mirrors core::TimeKeyBlender's accessor for the same channel,
    // because the graph has to plot the number playback blends. The cast is
    // valid for the caller's pairing of a channel with a key of its own type.
    switch (mType) {
    case core::TimeKeyType_Move: {
        const core::MoveKey& key = static_cast<const core::MoveKey&>(aKey);
        return (mComponent == 0) ? key.data().pos().x() : key.data().pos().y();
    }
    case core::TimeKeyType_Rotate: {
        const core::RotateKey& key = static_cast<const core::RotateKey&>(aKey);
        return key.data().rotate();
    }
    case core::TimeKeyType_Scale: {
        const core::ScaleKey& key = static_cast<const core::ScaleKey&>(aKey);
        return (mComponent == 0) ? key.data().scale().x() : key.data().scale().y();
    }
    case core::TimeKeyType_Depth: {
        const core::DepthKey& key = static_cast<const core::DepthKey&>(aKey);
        return key.data().depth();
    }
    case core::TimeKeyType_Opa: {
        const core::OpaKey& key = static_cast<const core::OpaKey&>(aKey);
        return key.data().opacity();
    }
    case core::TimeKeyType_Blur: {
        const core::BlurKey& key = static_cast<const core::BlurKey&>(aKey);
        switch (mComponent) {
        case 0: return key.data().blurX();
        case 1: return key.data().blurY();
        default: return key.data().angleDeg();
        }
    }
    case core::TimeKeyType_HSV: {
        // The stored channel is an int and playback blends it back through the
        // int setters, so the graph reads the same int (never a rescaled hue).
        const core::HSVKey& key = static_cast<const core::HSVKey&>(aKey);
        return static_cast<float>(key.data().hsv().at(mComponent));
    }
    case core::TimeKeyType_Image: {
        const core::ImageKey& key = static_cast<const core::ImageKey&>(aKey);
        return (mComponent == 0) ? key.data().imageOffset().x() : key.data().imageOffset().y();
    }
    default:
        return 0.0f;
    }
}

bool Channel::isInterpolated() const {
    // The image offset is the one graphable value playback does not blend:
    // blendImageKey step-holds the last preceding key, so a curve there would
    // draw an interpolation the document does not have.
    return mType != core::TimeKeyType_Image;
}

QString Channel::componentName() const {
    switch (mType) {
    case core::TimeKeyType_Move:
    case core::TimeKeyType_Scale:
    case core::TimeKeyType_Image:
        // Two components, always exposed as an x/y pair.
        return (mComponent == 1) ? QObject::tr("Y") : QObject::tr("X");
    case core::TimeKeyType_Rotate:
        return QObject::tr("Angle");
    case core::TimeKeyType_Depth:
        return QObject::tr("Depth");
    case core::TimeKeyType_Opa:
        return QObject::tr("Opacity");
    case core::TimeKeyType_Blur:
        switch (mComponent) {
        case 1: return QObject::tr("Radius Y");
        case 2: return QObject::tr("Angle");
        default: return QObject::tr("Radius X");
        }
    case core::TimeKeyType_HSV:
        switch (mComponent) {
        case 1: return QObject::tr("Saturation");
        case 2: return QObject::tr("Value");
        default: return QObject::tr("Hue");
        }
    default:
        // Not a graphable type: only reachable through a hand-made Channel, and
        // never drawn.
        return QObject::tr("Value");
    }
}

QString Channel::name() const {
    // A single-component channel needs no component half ("Rotate"); the type
    // name is the project's own term for the key type, so only the component is
    // translated.
    if (componentCount(mType) <= 1) {
        return core::TimeLine::getTimeKeyName(mType);
    }
    return core::TimeLine::getTimeKeyName(mType) + QLatin1Char(' ') + componentName();
}

} // namespace graph
} // namespace ctrl
