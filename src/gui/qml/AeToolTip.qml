pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A hover label for an icon-only control.
//
// Hand-rolled rather than Qt Quick Controls' ToolTip, which is a Popup: a
// QQuickWidget cannot render a popup outside its own bounds, and the export
// dialog is a widget-hosted scene (gui/ExportDialog). This is a plain item drawn
// above its owner, so it needs no extra window and no Controls import - the same
// reason ScrubSpinBox is hand-rolled.
//
// It is meant to be a CHILD of the control it labels, which is what gives it a
// position; the owner drives it:
//   AeToolTip { text: qsTr("Remove this range"); shown: mouse.containsMouse }
Item {
    id: root

    property string text: ""
    property bool shown: false

    visible: shown && text !== ""
    z: 1000
    implicitWidth: label.implicitWidth + 14
    implicitHeight: 22
    x: parent ? Math.round(parent.width / 2 - width / 2) : 0
    y: parent ? -height - 6 : 0

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: theme.floaterBody
        border.width: 1
        border.color: theme.floaterEdge
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: theme.text
        font.pixelSize: 11
    }
}
