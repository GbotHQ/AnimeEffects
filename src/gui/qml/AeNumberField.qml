pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A numeric field with an optional icon-only reset beside it.
//
// The value field is ScrubSpinBox (drag to scrub, click to type) because that is
// what the rest of the app's numeric inputs are, and a dialog is a bad place to
// invent a second way to enter a number. The reset is an AeIconButton with no
// words on it: the widget form spelled it out ("Set to native") three times, so a
// form whose actual decisions are four numbers was mostly labels for buttons that
// undo an edit. The words live in `resetLabel`, where a screen reader and the
// tooltip still find them.
Item {
    id: root

    property real value: 0
    property real from: 0
    property real to: 100
    property real step: 1
    // The words a probe or a screen reader addresses this field by
    // ("Width", "Frame rate", ...). Required: a bare spin box announces nothing.
    property string label: ""
    // The icon-only reset: hidden when the words are empty.
    property string resetLabel: ""
    property bool resetVisible: true
    // Shown in place of the number when the value is the sentinel -1, which is how
    // the still writer's "let QImage decide" and the codec's own sentinel are shown
    // as a word rather than as a number nobody chose.
    property string placeholder: ""

    signal valueEdited(real value)
    signal resetRequested()

    implicitHeight: 28
    // The field and its reset are one control on one row.
    implicitWidth: field.implicitWidth + (resetter.visible ? resetter.width + 4 : 0)

    Accessible.role: Accessible.SpinBox
    Accessible.name: root.label
    Accessible.description: String(root.value)
    Accessible.onPressAction: field.forceActiveFocus()

    ScrubSpinBox {
        id: field
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - (resetter.visible ? resetter.width + 4 : 0)
        height: 28
        value: root.value
        from: root.from
        to: root.to
        step: root.step
        decimals: 0
        enabled: root.enabled
        // ScrubSpinBox writes `value` on a drag (which drops the caller's
        // binding), so the caller re-syncs it; changed() is what carries the value
        // back out.
        onChanged: (value) => { root.valueEdited(Math.round(value)) }
    }
    AeIconButton {
        id: resetter
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: root.resetLabel !== "" && root.resetVisible
        enabled: root.enabled
        iconName: "arrow-clockwise"
        label: root.resetLabel
        onClicked: root.resetRequested()
    }
    // The word for the sentinel, on top of the field's own "-1".
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        visible: root.placeholder !== "" && root.value <= root.from && root.from < 0
        text: root.placeholder
        color: theme.textMuted
        font.pixelSize: 11
    }
}
