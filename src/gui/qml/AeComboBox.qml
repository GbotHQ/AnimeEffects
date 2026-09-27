pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A raised dropdown with an IN-SCENE popup.
//
// Deliberately not Qt Quick Controls' ComboBox: its popup is a Popup, which is a
// separate window and cannot render outside the QQuickWidget that hosts this
// dialog (the dialog is a widget-hosted scene, not a QQuickWindow). So the list is
// a plain Rectangle in the same scene, parented to the scene's root item, anchored
// under the field and flipped ABOVE it when there is no room below - which is the
// whole reason this control exists rather than a one-line import.
//
// `options` is a list of { label, value } maps (the shape the view model exposes),
// `index` is the current row. `overlay` is the scene's root item: the list is
// parented there and positioned in ITS coordinates, so a field near the bottom of
// the dialog still opens onto a list that is fully on screen.
Item {
    id: root

    property var options: []
    property int index: -1
    // True for a field that reports a frozen fact: same shape, recessed fill.
    property bool flat: false
    property Item overlay: null

    readonly property string value: (index >= 0 && index < options.length)
                                    ? String(options[index].value) : ""
    readonly property string text: (index >= 0 && index < options.length)
                                   ? String(options[index].label) : ""

    signal activated(int index)

    implicitHeight: 28
    implicitWidth: 160
    Accessible.role: Accessible.ComboBox
    // A combo exposes its current VALUE to a screen reader (Qt's own rule), and the
    // dialog's probes address it by what it IS - so the accessible name carries the
    // role and the value carries the state.
    Accessible.name: root.text
    Accessible.description: root.objectName
    Accessible.onPressAction: root.popupOpen = !root.popupOpen

    property bool popupOpen: false

    // The popup is reparented to `overlay` as soon as there is one, and sits above
    // everything else in the scene.
    function popupParent() { return root.overlay ? root.overlay : root; }

    function openPopup() {
        if (!root.enabled || options.length === 0) return;
        root.popupOpen = true;
        root.updatePopupGeometry();
    }

    function togglePopup() {
        if (root.popupOpen) { root.popupOpen = false; return; }
        root.openPopup();
    }

    // Place the list under the field, in the OVERLAY's coordinates, flipping it
    // above when there is no room below. Computed when the list opens (and when the
    // overlay resizes) rather than bound: mapToItem() has no change notification of
    // its own, so a binding over it caches the position from creation time - which
    // parked the list in the dialog's top-left corner.
    function updatePopupGeometry() {
        const parent = root.popupParent();
        if (!parent) return;
        const topLeft = root.mapToItem(parent, 0, 0);
        popup.x = topLeft.x;
        const h = popup.height;
        const below = topLeft.y + root.height + 2;
        if (below + h <= parent.height - 4) {
            popup.y = below;
        } else {
            const above = topLeft.y - h - 2;
            popup.y = above >= 4 ? above : Math.max(4, parent.height - h - 4);
        }
    }

    onPopupOpenChanged: if (popupOpen) updatePopupGeometry()
    onHeightChanged: if (popupOpen) updatePopupGeometry()
    onWidthChanged: if (popupOpen) updatePopupGeometry()

    Rectangle {
        id: field
        anchors.fill: parent
        radius: 4
        color: !root.enabled || root.flat ? theme.recessed
             : mouse.containsMouse ? theme.raisedHover : theme.raised
        border.width: 1
        border.color: root.popupOpen ? theme.focus
                    : mouse.containsMouse ? theme.hover
                    : (root.enabled && !root.flat ? theme.raised : theme.recessed)

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.right: caret.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.enabled ? theme.text : theme.textDisabled
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        Image {
            id: caret
            anchors.right: parent.right
            anchors.rightMargin: 7
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            source: theme.iconUrl("caret-down")
            sourceSize.width: 64
            sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
            opacity: root.enabled ? 0.85 : 0.4
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.togglePopup()
    }
    activeFocusOnTab: true
    Keys.onDownPressed: if (root.enabled && root.index + 1 < root.options.length) root.activated(root.index + 1)
    Keys.onUpPressed: if (root.enabled && root.index > 0) root.activated(root.index - 1)
    Keys.onSpacePressed: { root.togglePopup(); event.accepted = true }
    Keys.onEscapePressed: { root.popupOpen = false; event.accepted = true }

    // ---- the popup ---------------------------------------------------------
    // A backstop that swallows a click outside the list, so the dropdown closes
    // the way every dropdown does.
    Rectangle {
        id: scrim
        parent: root.popupParent()
        visible: root.popupOpen
        x: 0
        y: 0
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        color: "transparent"
        z: 900
        MouseArea {
            anchors.fill: parent
            onClicked: root.popupOpen = false
        }
    }

    Rectangle {
        id: popup
        parent: root.popupParent()
        visible: root.popupOpen
        z: 901
        width: root.width
        height: Math.min(220, list.contentHeight + 8)
        radius: 4
        color: theme.floaterBody
        border.width: 1
        border.color: theme.floaterEdge

        // x/y are set by updatePopupGeometry() (see its note): mapToItem() through
        // the whole parent chain, because the field sits inside a layout and
        // `root.x/y` is not its position in the window.

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: root.options
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                required property int index
                required property var modelData
                width: list.width
                height: 24
                radius: 3
                // Each option is its own addressable node: a probe (and a screen
                // reader) has to be able to pick "Best" or "PNG sequence" by name
                // while the list is open, not only read the closed field.
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.label
                Accessible.onPressAction: {
                    root.popupOpen = false;
                    root.activated(index);
                }
                color: rowMouse.containsMouse ? theme.raisedHover
                     : (index === root.index ? theme.active : "transparent")
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: tick.left
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    color: theme.text
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Image {
                    id: tick
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    visible: index === root.index
                    source: theme.iconUrl("check-bold")
                    sourceSize.width: 64
                    sourceSize.height: 64
                    fillMode: Image.PreserveAspectFit
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.popupOpen = false;
                        root.activated(index);
                    }
                }
            }
        }
    }
}
