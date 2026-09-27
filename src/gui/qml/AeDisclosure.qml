pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A full-width disclosure header: a caret, a label, and a clickable row.
//
// The widget form used a QToolButton with setArrowType plus a sibling widget it
// toggled visible. QToolButton:checked paints the checked state with the QSS
// `@focus@` colour, so expanding "Advanced" filled the header with the accent -
// the one thing the user did NOT select. This draws nothing when open: the
// caret turns down, the label brightens, and the content below is the feedback.
//
// The whole row is the target (a 24px glyph is not), and the row is keyboard
// reachable and announces itself as a button that is expandable.
Item {
    id: root

    property string text: ""
    property bool open: false
    signal toggled()

    implicitHeight: 30
    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.description: root.open ? qsTr("Expanded") : qsTr("Collapsed")
    Accessible.onPressAction: root.toggled()

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: mouse.containsMouse ? theme.raised : "transparent"
    }
    Image {
        id: caret
        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 14
        source: theme.iconUrl(root.open ? "caret-down" : "caret-right")
        sourceSize.width: 64
        sourceSize.height: 64
        fillMode: Image.PreserveAspectFit
    }
    Text {
        anchors.left: caret.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: mouse.containsMouse || root.open ? theme.text : theme.textMuted
        font.pixelSize: 12
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()
    }
    activeFocusOnTab: true
    Keys.onSpacePressed: { root.toggled(); event.accepted = true }
    Keys.onReturnPressed: { root.toggled(); event.accepted = true }
}
