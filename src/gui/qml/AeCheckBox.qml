pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A checkbox with its words beside it: a raised box, an accent tick when on, and
// a label that is part of the click target.
//
// Hand rolled rather than Qt Quick Controls' CheckBox for the reason the whole
// dialog is: a QC2 indicator draws its own chrome, and the app's checkboxes are a
// raised box with a centred accent tick (see AnimationSettingDialog.qml, whose
// inline loop box this generalises).
Item {
    id: root

    property string text: ""
    property bool checked: false
    signal toggled()

    implicitHeight: 24
    implicitWidth: box.width + (root.text === "" ? 0 : 8 + label.implicitWidth)
    // The checkbox carries the words it is labelled with, so a screen reader (and a
    // probe) addresses it by the same name the widget form gave the QCheckBox.
    Accessible.role: Accessible.CheckBox
    Accessible.name: root.text
    Accessible.description: root.checked ? qsTr("Checked") : qsTr("Unchecked")
    Accessible.onPressAction: {
        if (root.enabled) { root.checked = !root.checked; root.toggled(); }
    }

    Rectangle {
        id: box
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        height: 16
        radius: 3
        color: !root.enabled ? theme.recessed
             : mouse.pressed ? theme.active
             : root.checked ? theme.accent
             : theme.raised
        border.width: 1
        border.color: !root.enabled ? theme.outline
                    : mouse.containsMouse ? theme.hover
                    : root.checked ? theme.accent : theme.outline

        Canvas {
            anchors.fill: parent
            visible: root.checked
            opacity: root.enabled ? 1.0 : 0.45
            onPaint: {
                const c = getContext("2d");
                c.reset();
                c.strokeStyle = theme.accentText;
                c.lineWidth = 2;
                c.lineCap = "round";
                c.lineJoin = "round";
                c.beginPath();
                c.moveTo(width * 0.22, height * 0.52);
                c.lineTo(width * 0.43, height * 0.74);
                c.lineTo(width * 0.78, height * 0.28);
                c.stroke();
            }
        }
    }
    Text {
        id: label
        anchors.left: box.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.text !== ""
        text: root.text
        color: root.enabled ? theme.text : theme.textDisabled
        font.pixelSize: 12
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: { root.checked = !root.checked; root.toggled(); }
    }
    activeFocusOnTab: true
    Keys.onSpacePressed: {
        if (root.enabled) { root.checked = !root.checked; root.toggled(); }
        event.accepted = true;
    }
}
