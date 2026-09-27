pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A single-line text field matching the app's raised-input look.
//
// Hand-rolled TextInput rather than Qt Quick Controls' TextField, for the same
// reason ScrubSpinBox is: QC2 brings its own chrome and its own palette, and the
// app's fields are one raised surface that only draws a border on hover/focus.
//
// `text` is an ALIAS, so a caller can both read the committed value and reset it
// from outside (the destination picker clears its query on a refresh).
Item {
    id: root

    property alias text: input.text
    property string placeholderText: ""
    // The name a screen reader announces and the accessibility tree answers to.
    // Defaults to the placeholder - which is what it always was - but the two are
    // different jobs: a search field's placeholder is the sentence that explains
    // what to type, its accessible name is what a probe (scripts/gui/ae_ui.py) can
    // address it by. An EMPTY placeholder is announced as "text" and cannot be
    // addressed by name at all. An explicit name is for a field whose placeholder
    // is not its name (the export dialog's Bitrate field shows "Auto").
    property string accessibleName: ""
    // Optional leading glyph, e.g. "magnifying-glass" for a search field.
    property string leadingIcon: ""
    // Show an x button that empties the field (the widget picker had
    // QLineEdit::setClearButtonEnabled).
    property bool clearButton: false
    // Forward the tree-navigation keys instead of letting the TextInput keep them,
    // and announce each as navigate(key). A chooser you have to Tab out of is a
    // chooser you fight: the search field keeps the text cursor, and Up/Down move
    // the selection while Left/Right open and close platforms. Left/Right are the
    // ones that need this: a TextInput consumes them for the caret.
    property bool navigationKeys: false
    // NO `property bool enabled`: QQuickItem already has one and redeclaring it
    // shadows the base property (qmllint: [property-override]), so a disabled
    // field never reports disabled to the accessibility tree. See AeButton.
    signal edited(string text)
    signal accepted(string text)
    signal navigate(int key)

    implicitHeight: 28
    implicitWidth: 160
    Accessible.role: Accessible.EditableText
    Accessible.name: root.accessibleName !== "" ? root.accessibleName : root.placeholderText
    Accessible.onPressAction: input.forceActiveFocus()

    // Focus the editable TEXT, not the wrapping Item: an Item with focus has no
    // caret and drops every keystroke. A caller that wants the field focused when
    // its scene opens calls this from Component.onCompleted.
    function forceInputFocus() { input.forceActiveFocus() }

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 4
        color: !root.enabled ? theme.recessed
              : mouse.containsMouse ? theme.raisedHover : theme.raised
        border.width: 1
        border.color: input.activeFocus ? theme.focus
                      : mouse.containsMouse ? theme.hover : theme.raised
    }
    // FIRST of the interactive children, so the field, the leading glyph and the
    // clear button all sit above it: a click on empty space focuses the input, a
    // click on the text goes to the TextInput itself.
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        onClicked: input.forceActiveFocus()
    }
    Image {
        id: lead
        visible: root.leadingIcon !== ""
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 14
        source: root.leadingIcon === "" ? "" : theme.iconUrl(root.leadingIcon)
        sourceSize.width: 64
        sourceSize.height: 64
        fillMode: Image.PreserveAspectFit
        opacity: 0.8
    }
    TextInput {
        id: input
        anchors.left: lead.visible ? lead.right : parent.left
        anchors.leftMargin: lead.visible ? 7 : 9
        anchors.right: clearBtn.visible ? clearBtn.left : parent.right
        anchors.rightMargin: clearBtn.visible ? 4 : 9
        anchors.verticalCenter: parent.verticalCenter
        color: root.enabled ? theme.text : theme.textDisabled
        font.pixelSize: 12
        enabled: root.enabled
        selectByMouse: true
        selectionColor: theme.selection
        selectedTextColor: theme.selectionText
        clip: true
        onTextEdited: root.edited(text)
        onAccepted: root.accepted(text)
        // The TextInput is the item with the active focus, so the accessible name
        // and the key filter have to live on IT: a name set only on the wrapping
        // Item leaves the node that actually holds the editable text nameless, and
        // a Keys handler on the parent never sees Left/Right, which the TextInput
        // consumes for the caret.
        Accessible.name: root.accessibleName
        Keys.onPressed: (event) => {
            if (!root.navigationKeys) return;
            switch (event.key) {
            case Qt.Key_Down:
            case Qt.Key_Up:
            case Qt.Key_PageDown:
            case Qt.Key_PageUp:
            case Qt.Key_Left:
            case Qt.Key_Right:
                root.navigate(event.key);
                event.accepted = true;
                break;
            default:
                break;
            }
        }
    }
    Text {
        anchors.left: input.left
        anchors.right: input.right
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text.length === 0
        text: root.placeholderText
        color: theme.textMutedDim
        font.pixelSize: 12
        elide: Text.ElideRight
    }
    AeIconButton {
        id: clearBtn
        visible: root.clearButton && input.text.length > 0 && root.enabled
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        size: 22
        iconSize: 12
        iconName: "x"
        label: qsTr("Clear")
        onClicked: { input.text = ""; root.edited(""); input.forceActiveFocus(); }
    }
}
