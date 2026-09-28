// The destination chooser scene, rendered full-bleed inside the DestinationPicker
// QDialog's QQuickWidget.
//
// It mixes the two, deliberately and per element: the list is QtQuick core (a
// ListView over a flat row model), its scroll bar is a QtQuick.Controls
// ScrollBar with this design's two visual items, and the rest is the app's own
// Ae* components. An earlier version of this comment claimed a QQuickWidget
// cannot render a Controls popup outside its own bounds; that was never tested
// and is not true for Qt 6.8+ with Popup.Window.
//
// The scene is a pure VIEW. Everything a person can decide - which rows exist, what
// is open, what the search reveals, what is selected, what Enter means - is in the
// "picker" context property (gui::DestinationPickerModel), which the headless test
// (tests/destination_picker.cpp) drives with no QML engine, so the rules have one
// implementation and one place to break.
//
// The dialog's own words come from the model as translated strings
// (picker.searchPlaceholder, picker.okText, ...): they live in the catalog under
// the picker class's own context (gui::DestinationPicker), which is the one the
// shipped catalogs have already translated. The module pragma is kept so a future
// scene-local string has a home; there is deliberately none today.
pragma Translator: "AnimeEffects.Dialogs"

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Item {
    id: root
    // Context properties: picker (DestinationPickerModel) and theme (QmlTheme).

    // A QML item only has ACTIVE focus inside an ACTIVE window, and this scene is
    // built before the dialog is shown - so the search field's initial
    // forceInputFocus() lands on an inactive window and the field opens caret-less.
    // Re-assert it the moment the window becomes active. (The C++ shell also
    // activates the window on show; this is the QML half of the same fix.)
    readonly property bool windowActive: Window.active
    onWindowActiveChanged: if (windowActive) search.forceInputFocus()

    Rectangle {
        anchors.fill: parent
        color: theme.base
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8

        // ---- the search field ----
        // Typing filters; the arrows move the list WITHOUT leaving the field. The
        // field keeps the text cursor; the list keeps the highlight.
        AeTextField {
            id: search
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            placeholderText: picker.searchPlaceholder
            accessibleName: picker.searchAccessibleName
            leadingIcon: "magnifying-glass"
            clearButton: true
            navigationKeys: true
            Component.onCompleted: forceInputFocus()
            onNavigate: (key) => {
                switch (key) {
                case Qt.Key_Down: picker.step(1); break;
                case Qt.Key_Up: picker.step(-1); break;
                case Qt.Key_PageDown: picker.step(10); break;
                case Qt.Key_PageUp: picker.step(-10); break;
                case Qt.Key_Left: picker.collapseCurrentSection(); break;
                case Qt.Key_Right: picker.expandCurrentSection(); break;
                default: break;
                }
            }
            onAccepted: picker.activateCurrent()
        }
        // The filter follows the TEXT, not the edited() signal: a clear button and
        // the accessibility layer both write the text without a keystroke, and the
        // widget version filtered on QLineEdit::textChanged for the same reason.
        Connections {
            target: search
            function onTextChanged() { picker.setQuery(search.text) }
        }

        // ---- the list ----
        Item {
            id: listArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            property int hoveredRow: -1
            property string hoveredTooltip: ""

            ListView {
                id: list
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                // Reserve the lane the ScrollBar below stands in, so a row's
                // hover highlight stops short of it.
                width: parent.width - 12
                clip: true
                model: picker
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: 4000
                // The list can take focus with Tab; the search field is the usual
                // key holder, and forwards the navigation keys into the same calls.
                activeFocusOnTab: true
                // One handler, not one per key: Keys has no onPageUpPressed/
                // onPageDownPressed handler, and naming a handler that does not
                // exist makes the whole scene fail to load (silently white).
                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_Down: picker.step(1); event.accepted = true; break;
                    case Qt.Key_Up: picker.step(-1); event.accepted = true; break;
                    case Qt.Key_PageDown: picker.step(10); event.accepted = true; break;
                    case Qt.Key_PageUp: picker.step(-10); event.accepted = true; break;
                    case Qt.Key_Left: picker.collapseCurrentSection(); event.accepted = true; break;
                    case Qt.Key_Right: picker.expandCurrentSection(); event.accepted = true; break;
                    case Qt.Key_Return:
                    case Qt.Key_Enter: picker.activateCurrent(); event.accepted = true; break;
                    default: break;
                    }
                }

                // Every tree node is a row; one that is not on screen collapses to
                // zero height, so expansion and filtering never renumber anything.
                delegate: Rectangle {
                    id: rowItem
                    width: list.width
                    height: model.onScreen ? 30 : 0
                    visible: model.onScreen
                    radius: 4
                    color: {
                        if (model.isSection)
                            return rowHover.hovered ? theme.raised : "transparent";
                        if (model.selected)
                            return theme.selection;
                        return rowHover.hovered ? theme.raised : "transparent";
                    }

                    // The disclosure caret: the one affordance that says a platform
                    // opens. Quiet, like the platform row itself.
                    Image {
                        visible: model.isSection
                        x: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 12
                        source: theme.iconUrl(model.expanded ? "caret-down" : "caret-right")
                        sourceSize.width: 64
                        sourceSize.height: 64
                        fillMode: Image.PreserveAspectFit
                    }
                    // A platform's badge is its destinations' badge, so the closed
                    // list still says which platform each line is.
                    Image {
                        visible: model.iconName !== ""
                        x: model.isSection ? 24 : 38
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        source: model.iconName === "" ? "" : theme.iconUrl(model.iconName)
                        sourceSize.width: 64
                        sourceSize.height: 64
                        fillMode: Image.PreserveAspectFit
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: model.isSection ? 48 : 62
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.label
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        font.bold: model.isSection
                        color: {
                            // The broken-table statement is not a choice: dim it, so
                            // it does not read as a destination.
                            if (model.id === "" && !model.isSection) return theme.textMuted;
                            if (model.isSection) return theme.textMuted;
                            if (model.selected) return theme.selectionText;
                            return theme.text;
                        }
                    }

                    Accessible.role: model.isSection ? Accessible.Button : Accessible.ListItem
                    Accessible.name: model.label
                    Accessible.onPressAction: {
                        if (model.isSection) picker.toggleSection(index);
                        else picker.select(index);
                    }

                    HoverHandler {
                        id: rowHover
                        cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: {
                            if (hovered) {
                                listArea.hoveredRow = index;
                                listArea.hoveredTooltip = model.tooltip;
                            } else if (listArea.hoveredRow === index) {
                                listArea.hoveredRow = -1;
                                listArea.hoveredTooltip = "";
                            }
                        }
                    }
                    // One click on a platform opens or closes it; one click on a
                    // destination selects it; a double click on a destination is the
                    // answer. A platform never answers, whatever is clicked.
                    TapHandler {
                        onTapped: {
                            if (model.isSection) picker.toggleSection(index);
                            else picker.select(index);
                        }
                        onDoubleTapped: {
                            if (!model.isSection) picker.activate(index);
                        }
                    }
                }

                // The list's scroll bar is a QtQuick.Controls ScrollBar ATTACHED to
                // the ListView, so its position, size, active and its
                // press/drag/paging come from the ListView's own visibleArea (D23)
                // instead of from arithmetic over contentY. Two things are stated
                // rather than inherited, both measured from the Basic style's
                // ScrollBar.qml:
                //
                //  - the style's contentItem is TRANSIENT (opacity 0 with a fade-in
                //    state that times out 450ms after the last interaction). The
                //    design has no fade, so contentItem is replaced outright, which
                //    also removes the state that drove it;
                //  - with the fade gone, the style's `visible: policy !== AlwaysOff`
                //    would draw an empty track when nothing overflows. So visible is
                //    the ratio test the design asks for: show it only when the
                //    content does not fit (D23).
                //
                // It is parented to the list AREA rather than the list: the attached
                // bar would otherwise re-parent itself to the ListView and sit inside
                // its clip. The Qt docs name this as the way to keep an attached bar
                // outside a clipping Flickable, and position/size/active stay
                // automatic.
                ScrollBar.vertical: ScrollBar {
                    id: vbar
                    parent: listArea
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 8
                    orientation: Qt.Vertical
                    policy: ScrollBar.AsNeeded
                    visible: size < 1.0
                    hoverEnabled: true
                    // The design's minimum thumb length, expressed in the units
                    // minimumSize takes: a fraction of the bar's own length.
                    minimumSize: height > 0 ? 28 / height : 0
                    background: Rectangle { color: "transparent" }
                    contentItem: Rectangle {
                        implicitWidth: 6
                        radius: 3
                        color: vbar.pressed ? theme.textMuted
                             : vbar.hovered ? theme.textMuted
                             : theme.raisedHover
                    }
                }
            }

            // The row tooltip (the family and the id), drawn by the list AREA so the
            // ListView's own clip cannot cut it off at the first row.
            AeToolTip {
                text: listArea.hoveredTooltip
                shown: listArea.hoveredRow >= 0 && text !== ""
                x: {
                    const item = list.itemAtIndex(listArea.hoveredRow);
                    return item ? list.x + Math.round(item.x + item.width / 2 - width / 2) : 0;
                }
                y: {
                    const item = list.itemAtIndex(listArea.hoveredRow);
                    return item ? list.y + item.y - height - 6 : 0;
                }
            }
        }

        // ---- "My platform isn't listed" ----
        // The escape hatch for a platform the tree does not know is a button, not a
        // destination, so no search can filter it away.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: manual.implicitHeight
            AeButton {
                id: manual
                anchors.fill: parent
                text: picker.manualText
                onClicked: picker.acceptManually()
            }
            HoverHandler { id: manualHover }
            AeToolTip { text: picker.manualTooltip; shown: manualHover.hovered }
        }

        // ---- OK / Cancel ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            AeButton {
                text: picker.cancelText
                onClicked: picker.reject()
            }
            AeButton {
                text: picker.okText
                kind: "primary"
                onClicked: picker.activateCurrent()
            }
        }
    }

    // Keep the selection on screen when it moves by keyboard or by a search.
    Connections {
        target: picker
        function onCurrentRowChanged() {
            if (picker.currentRow >= 0)
                list.positionViewAtIndex(picker.currentRow, ListView.Contain);
        }
    }
}
