pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A flat button, the QML counterpart of the app's borderless fields.
//
// Deliberately NOT Qt Quick Controls: a QC2 Button draws its own control-style
// chrome, and the app has no such chrome anywhere. The QSS `QPushButton` rule is
// the reason the widget export dialog read as a different application: it puts a
// 1px @outline@ border on every button, while the fields, toolbars and tabs
// around it are borderless. This control keeps the raised fill (or the accent,
// for the one primary action) and leaves the border to focus only.
//
// "kind" is one of:
//   secondary - the default action button: raised fill, no border
//   primary   - the accent fill, exactly one per surface (Export)
//   ghost     - no fill at rest; for low-priority actions (Add range)
Item {
    id: root

    property string text: ""
    property string kind: "secondary"
    // A tinted glyph name from theme.iconUrl() ("plus-bold", "trash"); empty
    // for a text-only button. See QmlTheme::iconUrl.
    property string iconName: ""
    // NO `property bool enabled`: QQuickItem already has one, and redeclaring it
    // SHADOWS the base property (qmllint: [property-override]) so the base state
    // stays "enabled" and a disabled button never reports itself disabled to the
    // accessibility tree - which is how a GUI probe tells a frozen control from a
    // live one. Item.enabled already propagates to the MouseArea and the text.
    signal clicked()

    implicitWidth: Math.max(72, content.implicitWidth + 24)
    implicitHeight: 28
    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.onPressAction: if (root.enabled) root.clicked()

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 4
        color: {
            if (!root.enabled) return theme.recessed;
            if (mouse.pressed) {
                if (root.kind === "primary") return theme.accentBright;
                if (root.kind === "ghost") return theme.active;
                return theme.active;
            }
            if (mouse.containsMouse) {
                if (root.kind === "primary") return theme.accentHover;
                if (root.kind === "ghost") return theme.raised;
                return theme.raisedHover;
            }
            if (root.kind === "primary") return theme.accent;
            if (root.kind === "ghost") return "transparent";
            return theme.raised;
        }
        // The focus ring is the ONLY border this button ever draws.
        border.width: visualFocus ? 1 : 0
        border.color: theme.focus
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    readonly property bool visualFocus: activeFocus

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 7
        Image {
            visible: root.iconName !== ""
            width: 14
            height: 14
            anchors.verticalCenter: parent.verticalCenter
            source: root.iconName === "" ? "" : theme.iconUrl(root.iconName)
            sourceSize.width: 64
            sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
            opacity: root.enabled ? 1.0 : 0.5
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: {
                if (!root.enabled) return theme.textDisabled;
                return root.kind === "primary" ? theme.accentText : theme.text;
            }
            font.pixelSize: 12
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    activeFocusOnTab: true
    focus: false
    Keys.onSpacePressed: if (root.enabled) { root.clicked(); event.accepted = true }
    Keys.onReturnPressed: if (root.enabled) { root.clicked(); event.accepted = true }
}
