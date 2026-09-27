#ifndef GUI_QML_NUMBERFIELDACCESSIBILITY_H
#define GUI_QML_NUMBERFIELDACCESSIBILITY_H

namespace gui {

// Give the export form's hand-rolled QML numeric fields a real AT-SPI Value
// interface.
//
// WHY THIS EXISTS. Qt exposes a standards-compliant value protocol (current /
// minimum / maximum / step, readable AND writable) for its C++ value controls -
// QSpinBox had it, and scripts/gui/ae_ui.py's `set`/`value` are written against
// it. Qt Quick's `Accessible` attached type has no `value` property at all, so a
// QML Item that plays a spin box exposes its name and, at best, its value as a
// description: a screen reader cannot use the standard "adjust this value"
// protocol, and the number is not writable through the accessibility bus. The Qt
// Quick port of the export dialog therefore REGRESSED the numeric fields against
// the widget form it replaced.
//
// What this installs is the documented answer for a non-widget accessible object:
// a QAccessible factory that recognises the marker item inside AeNumberField.qml
// (`objectName: "aeAccessibleValue"`) and hands back an interface implementing
// QAccessibleValueInterface + QAccessibleActionInterface. The range and the value
// are read from that item's own properties, and a write calls its QML
// `accessibleSetValue`, so the model sees exactly what a typed edit would send.
//
// Idempotent, and safe to call from any constructor after QApplication exists.
void installNumberFieldAccessibility();

} // namespace gui

#endif // GUI_QML_NUMBERFIELDACCESSIBILITY_H
