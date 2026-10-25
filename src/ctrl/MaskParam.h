#ifndef CTRL_MASKPARAM_H
#define CTRL_MASKPARAM_H

#include "ctrl/Brush.h"
#include "core/TimeKeyType.h"
#include "core/FilterMask.h"

namespace ctrl {

// The mask paint tool's parameters: the shared brush, plus WHICH field it writes
// (the filter lane and which of that filter's two mask inputs).
class MaskParam {
public:
    MaskParam(): brush(), lane(core::TimeKeyType_Wave), channel(core::FilterMaskChannel_Amount), value(0.0f) {
        brush.radius = 50.0f;
        brush.hardness = 0.5f;
        brush.flow = 1.0f;
        brush.opacity = 1.0f;
        brush.spacing = 0.125f;
        // The field starts at its channel's neutral value, so a VALUE of 0 makes the
        // first stroke reduce the amount (the useful default for an amount mask);
        // picking the time-offset channel sets 1 instead (see setChannel).
    }

    BrushParam brush;
    core::TimeKeyType lane;
    core::FilterMaskChannel channel;
    // Where the brush takes the field: 0 = no effect, 1 = full effect. A stamp moves
    // the texel TOWARD this value, so painting over an area converges instead of
    // accumulating, and the temporary eyedropper has an obvious thing to pick.
    float value;

    // The channel decides the useful value to start from: restricting an amount mask
    // means painting toward 0; adding a delay to a time-offset mask means toward 1.
    void setChannel(core::FilterMaskChannel aChannel) {
        if (channel == aChannel)
            return;
        channel = aChannel;
        value = (aChannel == core::FilterMaskChannel_TimeOffset) ? 1.0f : 0.0f;
    }
};

} // namespace ctrl

#endif // CTRL_MASKPARAM_H
