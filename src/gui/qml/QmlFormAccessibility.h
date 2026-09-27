#ifndef GUI_QML_QMLFORMACCESSIBILITY_H
#define GUI_QML_QMLFORMACCESSIBILITY_H

namespace gui {

// Give the export form's hand-rolled Qt Quick controls the accessibility
// protocols Qt only provides for C++ widgets.
//
// WHY THIS EXISTS. Qt Quick's `Accessible` attached type can name a control, give
// it a role and expose a press action - but it has no `value` property, and Qt
// only exposes AT-SPI's Value interface for C++ value controls. So the port of
// the export dialog REGRESSED three things the deleted widget form had:
//
//   * a numeric field could not be read or SET through the standard value
//     protocol (a screen reader could not adjust it, and scripts/gui/ae_ui.py's
//     `set`/`value` could not reach it);
//   * a combo box's items could not be chosen: Qt exposes a press action for
//     Button/CheckBox/RadioButton/Switch roles, NOT for a list item, so a screen
//     reader could open the list and then not pick from it;
//   * and Qt reports a QML item's screen rectangle in the QQuickWidget's
//     coordinates, so "click where the control says it is" lands beside it.
//
// A QAccessible factory is the documented answer for a non-widget accessible
// object. It recognises three marker items the QML sets objectName on - the value
// field (AeNumberField), the combo field and its options (AeComboBox) - and hands
// back an interface that implements the missing protocols: a Value interface for
// the field, a press action for all three, and a rect() taken from the item's own
// mapToGlobal, which is screen-correct.
//
// Idempotent, and safe to call from any constructor after QApplication exists.
void installQmlFormAccessibility();

} // namespace gui

#endif // GUI_QML_QMLFORMACCESSIBILITY_H
