pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A quiet section heading: a small uppercase muted label with a hairline rule
// running out to the right.
//
// The widget form drew its headings as a title between two full-width hairlines
// ("--- WHAT TO EXPORT ---"), which reads as a separator rather than a heading:
// it has the same weight as the rules above and below the frame rows. This is one
// dim word (the role the eye skips) plus one rule (the role the eye follows), so
// the two sections are legible as structure without competing with their own
// contents.
//
// NOT a Controls Label, for the same reason every control in this dialog is hand
// rolled: a QC2 control brings its own chrome and palette into a QQuickWidget.
Item {
    id: root

    property string text: ""
    implicitHeight: 24
    implicitWidth: label.implicitWidth + 12

    Text {
        id: label
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.text.toUpperCase()
        color: theme.textMutedDim
        font.pixelSize: 10
        font.letterSpacing: 1.1
        font.bold: true
        // Exposed as a real heading, so a screen reader can navigate the form by
        // section instead of reading one flat run of controls.
        Accessible.role: Accessible.Heading
        Accessible.name: root.text
    }
    Rectangle {
        anchors.left: label.right
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 1
        color: theme.hairline
    }
}
