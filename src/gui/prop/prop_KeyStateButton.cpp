#include <QPainter>
#include <QPolygonF>

#include "gui/prop/prop_KeyStateButton.h"
#include "gui/theme/Colors.h"

namespace gui {
namespace prop {

    KeyStateButton::KeyStateButton(const QString& aName, core::TimeKeyType aType, QWidget* aParent):
        QToolButton(aParent), mState(State::Static), mType(aType) {
        this->setObjectName("keyStateButton");
        this->setFixedSize(20, 24);
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::NoFocus);
        // English literals until the inspector's copy is final; extracting now
        // would add unfinished catalog entries for a wording we may still move.
        this->setToolTip(QStringLiteral("Static: editing changes the base value. Click to add a key."));
        this->setAccessibleName(aName + QStringLiteral(" key"));
    }

    void KeyStateButton::setState(State aState) {
        if (mState == aState)
            return;
        mState = aState;
        switch (mState) {
        case State::Static:
            this->setToolTip(QStringLiteral("Static: editing changes the base value. Click to add a key."));
            break;
        case State::Animated:
            this->setToolTip(QStringLiteral("Animated between keys. Click to add a key at this frame."));
            break;
        case State::Keyed:
            this->setToolTip(QStringLiteral("Key exists at the current frame. Click to remove it."));
            break;
        }
        update();
    }

    void KeyStateButton::paintEvent(QPaintEvent* aEvent) {
        // Let the stylesheet paint the hover/pressed pill; this class owns only
        // the dot itself.
        QToolButton::paintEvent(aEvent);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const theme::Colors& colors = theme::Colors::current();
        // A key is the timeline's DOT, not a second vocabulary: same circular
        // shape and the same per-type hue the timeline lane uses. The accent is
        // reserved for the playhead/selection; a key's colour names its TYPE.
        QColor ink = theme::keyTypeColor(colors, mType);
        if (!isEnabled())
            ink = colors.textDisabled;

        const QPointF c(rect().center().x() + 0.5, rect().center().y() + 0.5);
        const qreal r = 4.0;
        const QRectF dot(c.x() - r, c.y() - r, 2 * r, 2 * r);

        painter.setPen(QPen(ink, 1.1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(dot);

        if (mState == State::Keyed) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(dot);
        } else if (mState == State::Animated) {
            // Half-filled: a key lane exists, but this frame is interpolated.
            painter.save();
            painter.setClipRect(QRectF(c.x() - r, c.y() - r, r, 2 * r));
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(dot);
            painter.restore();
        }
    }

} // namespace prop
} // namespace gui
