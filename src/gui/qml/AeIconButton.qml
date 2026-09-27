pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A square icon-only button: the shape every "reset to default", "add" and
// "remove" affordance in the export UI takes.
//
// Icon-only is the point. The widget dialog spelled every one of these out in
// words - "Set to native", "Set to auto", "Insert range", "Remove range" - so a
// form whose actual decisions are four numbers was mostly labels for buttons
// that undo an edit. The accessible name and the tooltip carry the words
// instead, where a screen reader and a hovering mouse can still find them.
Item {
    id: root

    // A tinted glyph name from theme.iconUrl() ("arrow-clockwise", "trash").
    property string iconName: ""
    // The words a screen reader announces and the tooltip shows. Required:
    // an icon with no name is a control nobody can identify.
    property string label: ""
    // NO `property bool enabled`: QQuickItem already has one and redeclaring it
    // shadows the base property (qmllint: [property-override]), so a disabled
    // button never reports disabled to the accessibility tree. See AeButton.
    property int size: 26
    property int iconSize: 15
    // "ghost" (default) or "danger": the latter only warns on hover, in the
    // background, because the glyph itself is a single pre-tinted file.
    property string kind: "ghost"
    signal clicked()

    implicitWidth: size
    implicitHeight: size
    Accessible.role: Accessible.Button
    Accessible.name: root.label
    Accessible.onPressAction: if (root.enabled) root.clicked()

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 4
        color: {
            if (!root.enabled) return "transparent";
            if (mouse.pressed) return theme.active;
            if (mouse.containsMouse) {
                if (root.kind === "danger") return Qt.rgba(theme.danger.r, theme.danger.g, theme.danger.b, 0.16);
                return theme.raisedHover;
            }
            return "transparent";
        }
    }
    Image {
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        source: root.iconName === "" ? "" : theme.iconUrl(root.iconName)
        sourceSize.width: 64
        sourceSize.height: 64
        fillMode: Image.PreserveAspectFit
        opacity: root.enabled ? 1.0 : 0.45
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
    AeToolTip { text: root.label; shown: mouse.containsMouse && root.enabled }

    activeFocusOnTab: true
    Keys.onSpacePressed: if (root.enabled) { root.clicked(); event.accepted = true }
    Keys.onReturnPressed: if (root.enabled) { root.clicked(); event.accepted = true }
}
