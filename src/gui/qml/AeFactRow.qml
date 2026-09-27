pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// One row of the key-facts summary: a muted label in the first column and a
// normal-weight value in the second. The optional detail is the muted explanation
// behind the value (why there is no audio, the container a codec lands in).
//
// This is the typography the flat QStringList could not have: the widget form
// printed "Transparency: yes   Audio: no" as one run of text, so the label and the
// answer had the same weight and a reader had to parse the sentence to find the
// value. Two columns, two roles.
//
// The row gives the Layout a real implicitWidth AND takes its height from the
// value's contentHeight for the width it was actually given: an Item with no
// implicitWidth collapses to a zero-width cell that still reserves a row of height,
// which is a blank band where the summary should be.
Item {
    id: root

    property string label: ""
    property string value: ""
    property string detail: ""
    // "normal" | "warning" | "error"
    property string severity: "normal"

    implicitWidth: 340
    // contentHeight, not implicitHeight: the value WRAPS, so its height depends on
    // the width the layout gives it.
    implicitHeight: Math.max(labelText.contentHeight, valueText.contentHeight) + 5

    Text {
        id: labelText
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: 2
        width: 104
        text: root.label
        color: theme.textMuted
        font.pixelSize: 10
        font.letterSpacing: 0.4
        elide: Text.ElideRight
    }
    Text {
        id: valueText
        anchors.left: parent.left
        anchors.leftMargin: 112
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 1
        text: root.detail === "" ? root.value : root.value + qsTr(" · ") + root.detail
        color: root.severity === "error" ? theme.danger
             : root.severity === "warning" ? theme.warning
             : theme.text
        font.pixelSize: 12
        wrapMode: Text.WordWrap
    }
}
