pragma Translator: "AnimeEffects.Properties"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One filter is one card. Its header is the row; expanding it grows the card
// under that same row instead of moving the settings into a detached editor.
Item {
    id: root

    // A delegate is torn down while its model role is cleared; every binding
    // below then runs once against an undefined object and writes a TypeError to
    // the log. The empty row keeps that teardown silent without hiding a real
    // missing role (the delegate always assigns the actual row in FilterStack).
    QtObject {
        id: emptyRow
        property string kind: ""
        property string name: ""
        property bool enabled: true
        property bool expanded: false
        property string keyState: ""
        property color keyColor: "transparent"
        property string summary: ""
        property int hue: 0
        property int saturation: 100
        property int value: 100
        property bool absolute: false
        property double amount: 0
        property bool directional: false
        property double blurX: 0
        property double blurY: 0
        property double blurAngle: 0
        function toggleExpanded() {}
        function toggleKey() {}
        function toggleEnabled() {}
        function remove() {}
        function setHue(v) {}
        function setSaturation(v) {}
        function setValue(v) {}
        function setAbsolute(v) {}
        function setAmount(v) {}
        function setDirectional(v) {}
        function setBlurX(v) {}
        function setBlurY(v) {}
        function setBlurAngle(v) {}
    }
    property var filterRow: emptyRow
    property int cardIndex: 0
    signal dragStarted(int index)
    signal dragMoved(int index, real sceneY)
    signal dragEnded(int index, real sceneY)

    width: parent ? parent.width : 0
    height: headerItem.height + body.height + (filterRow.expanded ? 2 : 0)
    Accessible.role: Accessible.Grouping
    Accessible.name: filterRow.name

    // One card, one quiet outline. The frame is INSET by its 1px stroke on both
    // children, and BOTH the header and the body carry rounded corners; the body
    // being square was what covered the bottom corners of the border.
    Rectangle {
        id: cardFrame
        anchors.fill: parent
        visible: filterRow.expanded
        radius: 4
        color: "transparent"
        border.width: 1
        border.color: theme.hairline
    }

    Rectangle {
        id: headerItem
        x: filterRow.expanded ? 1 : 0
        y: filterRow.expanded ? 1 : 0
        width: parent.width - (filterRow.expanded ? 2 : 0)
        height: 30
        radius: filterRow.expanded ? 3 : 4
        color: filterRow.expanded || headerMouse.containsMouse ? theme.raised : "transparent"

        // Expanded: the header's top corners stay rounded, its bottom edge
        // becomes square so it joins the body as one card. Without this patch
        // the radius leaves a second rounded edge over the body.
        Rectangle {
            visible: filterRow.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 4
            color: headerItem.color
        }

        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            // A filter drag is vertical and the panel is a Flickable; without
            // this the page steals the gesture after a few pixels.
            preventStealing: true
            property real pressY: 0
            property bool dragging: false
            onPressed: (mouse) => {
                pressY = mouse.y;
                dragging = false;
            }
            onPositionChanged: (mouse) => {
                if (!pressed) return;
                if (!dragging && Math.abs(mouse.y - pressY) > 4) {
                    dragging = true;
                    root.dragStarted(root.cardIndex);
                }
                if (dragging)
                    root.dragMoved(root.cardIndex, headerItem.mapToItem(null, mouse.x, mouse.y).y);
            }
            onReleased: (mouse) => {
                if (dragging)
                    root.dragEnded(root.cardIndex, headerItem.mapToItem(null, mouse.x, mouse.y).y);
                else
                    filterRow.toggleExpanded();
                dragging = false;
            }
            onCanceled: {
                if (dragging)
                    root.dragEnded(root.cardIndex, headerItem.mapToItem(null, width / 2, height / 2).y);
                dragging = false;
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 6
            spacing: 4

            Image {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                source: theme.iconUrl(filterRow.expanded ? "caret-down" : "caret-right")
                sourceSize.width: 64
                sourceSize.height: 64
                fillMode: Image.PreserveAspectFit
                opacity: headerMouse.containsMouse ? 0.9 : 0.5
            }

            AeIconButton {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                size: 20
                iconSize: 14
                iconName: filterRow.enabled ? "eye-open" : "eye-close"
                label: filterRow.enabled ? qsTr("Disable %1").arg(filterRow.name)
                                         : qsTr("Enable %1").arg(filterRow.name)
                showTip: true
                opacity: headerMouse.containsMouse || filterRow.expanded ? 1.0 : 0.5
                onClicked: filterRow.toggleEnabled()
            }

            InspectorKeyDot {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                state: filterRow.keyState
                ink: filterRow.keyColor
                label: filterRow.name
                onClicked: filterRow.toggleKey()
            }

            Text {
                text: filterRow.name
                color: filterRow.enabled ? theme.text : theme.textDisabled
                font.family: uiFont.family
                font.pointSize: uiFont.pointSize
                font.bold: true
                    elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: filterRow.summary
                color: theme.textMuted
                font: uiFont
                    horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }

            AeIconButton {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                size: 20
                iconSize: 16
                iconName: "trash"
                label: qsTr("Remove %1").arg(filterRow.name)
                showTip: true
                // Always present so revealing it cannot reflow the header; it
                // brightens with the pointer and the expanded card.
                opacity: headerMouse.containsMouse || filterRow.expanded ? 0.9 : 0.45
                onClicked: filterRow.remove()
            }
        }
    }

    Rectangle {
        id: body
        x: filterRow.expanded ? 1 : 0
        y: headerItem.height + (filterRow.expanded ? 1 : 0)
        width: parent.width - (filterRow.expanded ? 2 : 0)
        height: filterRow.expanded ? bodyColumn.implicitHeight + 18 : 0
        visible: filterRow.expanded
        color: theme.base
        radius: filterRow.expanded ? 3 : 0

        // Round the BOTTOM of the body only: the header joins it squarely and
        // the header's own bottom patch does the same at the junction.
        Rectangle {
            visible: filterRow.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 4
            color: body.color
        }

        Column {
            id: bodyColumn
            y: 8
            width: parent.width
            spacing: 0

            // HSV
            InspectorRow {
                visible: filterRow.kind === "hsv"
                label: filterRow.hueLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.hue
                    from: 0
                    to: inspector.hsvHueMax
                    step: 1
                    decimals: 0
                    label: filterRow.hueLabel
                    prefix: "H"
                    onValueEdited: (value) => filterRow.setHue(Math.round(value))
                }
            }
            InspectorRow {
                visible: filterRow.kind === "hsv"
                label: filterRow.saturationLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.saturation
                    from: 0
                    to: inspector.hsvComponentMax
                    step: 1
                    decimals: 0
                    label: filterRow.saturationLabel
                    prefix: "S"
                    onValueEdited: (value) => filterRow.setSaturation(Math.round(value))
                }
            }
            InspectorRow {
                visible: filterRow.kind === "hsv"
                label: filterRow.valueLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.value
                    from: 0
                    to: inspector.hsvComponentMax
                    step: 1
                    decimals: 0
                    label: filterRow.valueLabel
                    prefix: "V"
                    onValueEdited: (value) => filterRow.setValue(Math.round(value))
                }
            }
            InspectorRow {
                visible: filterRow.kind === "hsv"
                label: filterRow.colorLabel
                AeCheckBox {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 23
                    text: filterRow.absoluteLabel
                    textFont: uiFont
                                checked: filterRow.absolute
                    onToggled: filterRow.setAbsolute(checked)
                }
            }

            // Blur
            InspectorRow {
                visible: filterRow.kind === "blur"
                label: filterRow.amountLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.amount
                    from: 0
                    to: inspector.blurMax
                    step: 0.5
                    decimals: 1
                    label: qsTr("Blur amount")
                    onValueEdited: (value) => filterRow.setAmount(value)
                }
            }
            InspectorRow {
                visible: filterRow.kind === "blur"
                label: filterRow.modeLabel
                AeCheckBox {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 23
                    text: filterRow.directionalLabel
                    textFont: uiFont
                                checked: filterRow.directional
                    onToggled: filterRow.setDirectional(checked)
                }
            }
            InspectorRow {
                visible: filterRow.kind === "blur" && filterRow.directional
                label: filterRow.blurXLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.blurX
                    from: 0
                    to: inspector.blurMax
                    step: 0.5
                    decimals: 1
                    label: filterRow.blurXLabel
                    prefix: "X"
                    onValueEdited: (value) => filterRow.setBlurX(value)
                }
            }
            InspectorRow {
                visible: filterRow.kind === "blur" && filterRow.directional
                label: filterRow.blurYLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.blurY
                    from: 0
                    to: inspector.blurMax
                    step: 0.5
                    decimals: 1
                    label: filterRow.blurYLabel
                    prefix: "Y"
                    onValueEdited: (value) => filterRow.setBlurY(value)
                }
            }
            InspectorRow {
                visible: filterRow.kind === "blur" && filterRow.directional
                label: filterRow.angleLabel
                InspectorNumberField {
                    Layout.fillWidth: true
                    value: filterRow.blurAngle
                    from: inspector.blurAngleMin
                    to: inspector.blurAngleMax
                    step: 1
                    decimals: 1
                    label: qsTr("Blur angle")
                    prefixIcon: "angle"
                    suffix: "°"
                    onValueEdited: (value) => filterRow.setBlurAngle(value)
                }
            }
        }
    }
}
