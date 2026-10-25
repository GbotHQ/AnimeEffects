pragma Translator: "AnimeEffects.Properties"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The effect stack: one section header, a card per filter, and a searchable
// picker for adding one. Cards expand in place; there is no detached editor.
Item {
    id: root

    property bool collapsed: false
    property int dragIndex: -1
    property int dropIndex: -1
    property int insertionIndex: -1

    width: parent ? parent.width : 0
    implicitHeight: collapsed ? header.height : header.height + body.height
    height: implicitHeight

    function addEntry(entry) {
        picker.close();
        inspector.filters.addFilter(entry.type);
    }
    function addFirstMatching() {
        for (let i = 0; i < catalogRepeater.count; ++i) {
            const item = catalogRepeater.itemAt(i);
            if (item && item.visible && item.enabledEntry)
                return addEntry(item.entry);
        }
    }
    function insertionAt(sceneY) {
        for (let i = 0; i < cards.count; ++i) {
            const card = cards.itemAt(i);
            if (!card) continue;
            const centre = card.mapToItem(null, 0, card.height / 2).y;
            if (sceneY < centre)
                return i;
        }
        return cards.count;
    }
    function updateDropLine(insertion) {
        insertionIndex = insertion;
        if (cards.count === 0) {
            dropLine.y = 0;
        } else if (insertion < cards.count) {
            const card = cards.itemAt(insertion);
            dropLine.y = card ? card.y : 0;
        } else {
            const card = cards.itemAt(cards.count - 1);
            dropLine.y = card ? card.y + card.height : 0;
        }
    }

    Item {
        id: header
        width: parent.width
        height: 32

        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.collapsed = !root.collapsed
        }

        Image {
            id: caret
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 14
            source: theme.iconUrl(root.collapsed ? "caret-right" : "caret-down")
            sourceSize.width: 64
            sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
            opacity: headerMouse.containsMouse ? 0.9 : 0.55
        }

        Text {
            x: 36
            anchors.verticalCenter: parent.verticalCenter
            text: inspector.filters.title
            color: theme.text
            font.family: Qt.application.font.family
            font.pointSize: Qt.application.font.pointSize
            font.bold: true
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        AeIconButton {
            id: addButton
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 22
            height: 22
            size: 22
            iconSize: 14
            iconName: "plus-bold"
            label: inspector.filters.addLabel
            showTip: true
            onClicked: {
                picker.open();
                search.forceActiveFocus();
            }
        }
    }

    Item {
        id: body
        y: header.height
        width: parent.width
        height: root.collapsed ? 0 : bodyColumn.implicitHeight + 8
        visible: !root.collapsed

        Column {
            id: bodyColumn
            x: 12
            y: 0
            width: parent.width - 24
            spacing: 2

            Repeater {
                id: cards
                model: inspector.filters.rows.length
                delegate: InspectorFilterCard {
                    required property int index
                    filterRow: inspector.filters.rows[index]
                    cardIndex: index
                    width: bodyColumn.width
                    onDragStarted: (index) => {
                        root.dragIndex = index;
                        root.updateDropLine(index);
                        dropLine.visible = true;
                    }
                    onDragMoved: (index, sceneY) => {
                        if (root.dragIndex < 0) return;
                        const insertion = root.insertionAt(sceneY);
                        root.updateDropLine(insertion);
                        root.dropIndex = insertion > root.dragIndex ? insertion - 1 : insertion;
                        root.dropIndex = Math.max(0, Math.min(cards.count - 1, root.dropIndex));
                        dropLine.visible = true;
                    }
                    onDragEnded: (index, sceneY) => {
                        if (root.dragIndex < 0) return;
                        const source = root.dragIndex;
                        if (root.dropIndex >= 0 && root.dropIndex !== source) {
                            const row = cards.itemAt(source) ? cards.itemAt(source).filterRow : null;
                            if (row)
                                inspector.filters.moveFilter(row.type, root.dropIndex);
                        }
                        root.dragIndex = -1;
                        root.dropIndex = -1;
                        root.insertionIndex = -1;
                        dropLine.visible = false;
                    }
                }
            }

            Text {
                visible: cards.count === 0
                x: 4
                topPadding: 4
                bottomPadding: 4
                text: inspector.filters.noFiltersText
                color: theme.textMuted
                font: Qt.application.font
            }
        }

        Rectangle {
            id: dropLine
            visible: false
            x: 12
            width: bodyColumn.width
            height: 2
            color: theme.accent
            z: 10
        }
    }

    Popup {
        id: picker
        parent: root
        width: Math.min(320, root.width - 16)
        x: Math.max(8, root.width - width - 12)
        y: header.height
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        modal: false
        dim: false
        onOpened: search.forceActiveFocus()
        onClosed: search.text = ""

        height: Math.min(420, contentItem.implicitHeight + 12)
        background: Rectangle {
            radius: 4
            color: theme.floaterBody
            border.width: 1
            border.color: theme.floaterEdge
        }

        contentItem: Column {
            x: 6
            width: picker.width - 12
            spacing: 4

            AeTextField {
                id: search
                width: parent.width
                implicitHeight: 28
                leadingIcon: "magnifying-glass"
                textFont: Qt.application.font
                placeholderText: inspector.filters.searchLabel
                accessibleName: inspector.filters.searchLabel
                Keys.onReturnPressed: root.addFirstMatching()
            }

            Repeater {
                id: catalogRepeater
                model: inspector.filters.catalog.length
                delegate: Item {
                    id: catalogItem
                    required property int index
                    property var catalogEntry: inspector.filters.catalog[index]
                    property var modelData: catalogEntry
                    // `rows.length` is read as a dependency: hasFilter() is a call,
                    // and a call registers no binding dependency in QML.
                    property bool added: modelData.available && inspector.filters.rows.length >= 0
                                         && inspector.filters.hasFilter(modelData.type)
                    property bool enabledEntry: modelData.available && !added
                    width: parent.width
                    height: visible ? (modelData.available ? 34 : 38) : 0
                    visible: search.text.trim() === ""
                          || String(modelData.name).toLowerCase().indexOf(search.text.trim().toLowerCase()) >= 0
                          || String(modelData.description).toLowerCase().indexOf(search.text.trim().toLowerCase()) >= 0
                    opacity: enabledEntry ? 1.0 : 0.55

                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: mouse.containsMouse && parent.enabledEntry ? theme.raisedHover : "transparent"
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            width: parent.width
                            text: modelData.name
                                     + (!modelData.available ? "  ·  " + inspector.filters.comingSoonLabel
                                                             : catalogItem.added ? "  ·  " + inspector.filters.addedLabel : "")
                            color: theme.text
                            font: Qt.application.font
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: modelData.available
                            text: modelData.description
                            color: theme.textMuted
                            font: Qt.application.font
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: parent.enabledEntry
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.addEntry(parent.catalogEntry)
                    }
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name
                    Accessible.description: modelData.description
                }
            }

            Text {
                width: parent.width
                visible: {
                    for (let i = 0; i < catalogRepeater.count; ++i) {
                        const item = catalogRepeater.itemAt(i);
                        if (item && item.visible) return false;
                    }
                    return true;
                }
                text: inspector.filters.noMatchLabel
                color: theme.textMuted
                font: Qt.application.font
                leftPadding: 4
                topPadding: 4
            }
        }
    }
}
