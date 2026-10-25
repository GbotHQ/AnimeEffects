#ifndef GUI_PROP_KEYSTATEBUTTON_H
#define GUI_PROP_KEYSTATEBUTTON_H

#include <QToolButton>
#include "core/TimeKeyType.h"

namespace gui {
namespace prop {

    // The per-property key affordance in a KeyGroup's header. Three states,
    // because one diamond cannot say both "has keys" and "this frame is one":
    //   Static   - no explicit keys; editing the fields changes the base value.
    //   Animated - the lane has keys elsewhere; this frame is interpolated.
    //   Keyed    - this frame owns a key.
    // A press is always "add a key at the playhead" (the same operation the old
    // knocker row performed), so it never destroys a value by accident.
    class KeyStateButton: public QToolButton {
        Q_OBJECT
    public:
        enum class State { Static, Animated, Keyed };

        explicit KeyStateButton(const QString& aName, core::TimeKeyType aType, QWidget* aParent = nullptr);

        void setState(State aState);
        State state() const { return mState; }

    protected:
        void paintEvent(QPaintEvent* aEvent) override;

    private:
        State mState;
        core::TimeKeyType mType;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_KEYSTATEBUTTON_H
