#include "gui/qml/NumberFieldAccessibility.h"

#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QAccessibleObject>
#include <QAccessibleValueInterface>
#include <QMetaObject>
#include <QPointF>
#include <QQuickItem>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

namespace gui {
namespace {

// The marker objectName AeNumberField.qml sets on its value node. A factory sees
// EVERY object in the process, so the check has to be a cheap identity test and
// everything else has to fall through to Qt's own handling.
constexpr auto kMarkerObjectName = "aeAccessibleValue";

class NumberFieldAccessible : public QAccessibleObject,
                              public QAccessibleValueInterface,
                              public QAccessibleActionInterface {
public:
    explicit NumberFieldAccessible(QQuickItem* aItem) : QAccessibleObject(aItem) {}

    QQuickItem* item() const { return static_cast<QQuickItem*>(object()); }

    // ---- QAccessibleInterface -------------------------------------------------
    QAccessible::Role role() const override { return QAccessible::SpinBox; }

    QString text(QAccessible::Text aType) const override {
        switch (aType) {
        case QAccessible::Name:
            return item()->property("accessibleName").toString();
        case QAccessible::Value:
            return currentValue().toString();
        // No Description: the RANGE belongs to the Value interface's own
        // minimumValue()/maximumValue(), which a tool asks for directly. A
        // hand-rolled sentence here would be a second, translatable source of the
        // same numbers.
        default:
            return QString();
        }
    }

    QAccessible::State state() const override {
        QAccessible::State s;
        s.focusable = true;
        s.focused = item()->hasActiveFocus();
        // The state bits are the NEGATIVE senses, as QAccessible defines them.
        s.invisible = !item()->isVisible();
        s.disabled = !item()->isEnabled();
        return s;
    }

    int childCount() const override { return 0; }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QAccessibleInterface* focusChild() const override { return nullptr; }

    QAccessibleInterface* parent() const override {
        // The QML tree's own parent, so this node sits where the eye says it does.
        return QAccessible::queryAccessibleInterface(item()->parent());
    }

    // QAccessibleObject::rect() is empty for a plain QObject; a QQuickItem knows
    // its own screen geometry, and that is what a probe clicks and a magnifier
    // frames.
    QRect rect() const override {
        const QPointF topLeft = item()->mapToGlobal(QPointF(0, 0));
        return QRect(topLeft.toPoint(),
                     QSize(qRound(item()->width()), qRound(item()->height())));
    }

    void* interface_cast(QAccessible::InterfaceType aType) override {
        if (aType == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        if (aType == QAccessible::ActionInterface)
            return static_cast<QAccessibleActionInterface*>(this);
        return QAccessibleObject::interface_cast(aType);
    }

    // ---- QAccessibleValueInterface --------------------------------------------
    QVariant currentValue() const override {
        return item()->property("accessibleValue");
    }
    QVariant maximumValue() const override {
        return item()->property("accessibleMaximum");
    }
    QVariant minimumValue() const override {
        return item()->property("accessibleMinimum");
    }
    QVariant minimumStepSize() const override {
        return item()->property("accessibleStep");
    }
    void setCurrentValue(const QVariant& aValue) override {
        // Through the item's own QML entry point, so the field's "value was
        // edited" path (and the model behind it) is the same one a typed edit
        // uses. Writing the `value` property directly would update the pixels and
        // nothing else.
        QMetaObject::invokeMethod(item(), "accessibleSetValue",
                                  Q_ARG(QVariant, aValue));
    }

    // ---- QAccessibleActionInterface -------------------------------------------
    // An AT "press" on a spin box means "let me edit it", exactly as a mouse click
    // does - the same contract AeNumberField's own onPressAction documented before
    // this class replaced it.
    QStringList actionNames() const override { return {pressAction()}; }
    void doAction(const QString& aActionName) override {
        if (aActionName == pressAction())
            QMetaObject::invokeMethod(item(), "accessibleBeginEdit");
    }
    QStringList keyBindingsForAction(const QString&) const override {
        return {};
    }
};

QAccessibleInterface* numberFieldFactory(const QString&, QObject* aObject) {
    auto* item = qobject_cast<QQuickItem*>(aObject);
    if (item == nullptr || item->objectName() != QLatin1String(kMarkerObjectName))
        return nullptr;
    return new NumberFieldAccessible(item);
}

} // namespace

void installNumberFieldAccessibility() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    QAccessible::installFactory(numberFieldFactory);
}

} // namespace gui
