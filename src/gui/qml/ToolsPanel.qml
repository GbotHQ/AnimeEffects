pragma Translator: "AnimeEffects.Tools"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Tools dock, now a Qt Quick scene. The scene owns layout and ink; every tool
// decision lives in gui::ToolViewModel, which holds the same ctrl param structs the
// widget panels held as members.
//
// THE ORDER IS THE HIERARCHY, top to bottom:
//   1. the TOOLBOX - the dock's primary control, directly under the dock's own title.
//      It carries no heading, because the QDockWidget already says "Tools" (the widget
//      version's "Toolbox" group title was a QGroupBox artifact);
//   2. the VIEW strip - one fixed row of viewport settings, under its own heading. It
//      sits ABOVE the tool panel on purpose: everything above the panel is fixed, so
//      switching tools cannot move it, and the panel (which changes height with the
//      tool, and is the tallest block by far) takes the rest;
//   3. the ACTIVE TOOL's panel: its heading names the tool and the trailing word names
//      the mode, then the mode row, the tool's own rows, and the shared brush.
//
// The whole content is a ScrollView: the mask tool's panel (target, channel, value,
// four brush rows, two switches, Clear) is taller than a dock on a small window, and the
// widget version simply clipped it - there was no scrollbar anywhere in this dock.
Rectangle {
    id: root

    color: theme.base

    ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: AeScrollBar { width: 12 }
        contentWidth: width

        Column {
            id: content
            width: scroll.width
            topPadding: 6
            spacing: 0

            // ---- 1. the toolbox ---------------------------------------------
            Flow {
                id: toolFlow
                x: 12
                width: parent.width - 24
                spacing: 2
                Repeater {
                    model: toolModel.tools
                    delegate: ToolIconButton {
                        iconName: modelData.icon
                        label: modelData.tip
                        checkable: true
                        // the accent is the application's MODE and belongs to the one
                        // button that says which tool is in the artist's hand
                        accent: true
                        enabled: toolModel.driverActive
                        checked: toolModel.tool === modelData.type
                        onClicked: toolModel.activate(modelData.type)
                    }
                }
            }

            Item { width: 1; height: 8 }

            // ---- 2. the view strip ------------------------------------------
            ToolHeading { title: toolModel.viewTitle }
            Flow {
                id: viewFlow
                x: 12
                width: parent.width - 24
                spacing: 2
                // the two toggles that change how the ARTWORK is drawn
                ToolIconButton {
                    iconName: "cube"
                    label: toolModel.showMeshTip
                    checkable: true
                    checked: toolModel.showMesh
                    onClicked: toolModel.showMesh = !toolModel.showMesh
                }
                ToolIconButton {
                    iconName: "image"
                    label: toolModel.cutOutsideFrameTip
                    checkable: true
                    checked: toolModel.cutOutsideFrame
                    onClicked: toolModel.cutOutsideFrame = !toolModel.cutOutsideFrame
                }
                // the three canvas rotations: TRIGGERS, not states
                ToolIconButton {
                    iconName: "arrow-counter-clockwise"
                    label: toolModel.rotateCounterClockwiseTip
                    onClicked: toolModel.rotateCounterClockwise()
                }
                ToolIconButton {
                    iconName: "dot-outline"
                    label: toolModel.resetRotationTip
                    onClicked: toolModel.resetRotation()
                }
                ToolIconButton {
                    iconName: "arrow-clockwise"
                    label: toolModel.rotateClockwiseTip
                    onClicked: toolModel.rotateClockwise()
                }
                ToolIconButton {
                    iconName: "flip-horizontal"
                    label: toolModel.flipCanvasTip
                    checkable: true
                    checked: toolModel.flipCanvas
                    onClicked: toolModel.flipCanvas = !toolModel.flipCanvas
                }
            }

            Item { width: 1; height: 6 }

            // ---- 3. the active tool's panel ---------------------------------
            ToolHeading {
                visible: toolModel.panelVisible
                title: toolModel.title
                detail: toolModel.modeName

                // THE MASK'S FIELD, as the heading's own controls. Clear lives here in
                // both states, so it never moves; the disclosure appears only when a card
                // pinned the field (then the heading IS the field's name and there is no
                // chooser row at all).
                ToolIconButton {
                    visible: toolModel.maskPickersVisible && toolModel.maskPinned
                    iconName: "caret-down"
                    iconSize: 14
                    label: qsTr("Change what is painted")
                    onClicked: toolModel.unpinMaskTarget()
                }
                ToolTextAction {
                    visible: toolModel.maskPickersVisible
                    text: qsTr("Clear")
                    tip: qsTr("Clear the field being painted (one undo step)")
                    onClicked: toolModel.requestClearMask()
                }
            }
            Column {
                id: panel
                width: parent.width
                visible: toolModel.panelVisible

                ToolModeRow {
                    visible: toolModel.modes.length > 0
                    modes: toolModel.modes
                    current: toolModel.modeIndex
                    onChosen: (index) => toolModel.setMode(index)
                }

                // ---- paint weights: the authored field's state and actions ----
                // A conventional stat row: the label on the dock's grid, the state beside
                // it, and the count on a muted second line. The verbs are a real primary
                // button plus the overflow menu; the explanations are tooltips, not
                // paragraphs (a dock is not a manual).
                Item {
                    id: weightsPanel
                    visible: toolModel.weightsStateVisible
                    width: parent.width
                    // A plain Item does not follow its implicitHeight, and the Column
                    // below decides the block's height.
                    height: weightsStateColumn.implicitHeight + 10

                    Text {
                        id: weightsLabel
                        x: 12
                        anchors.top: parent.top
                        anchors.topMargin: 7
                        text: qsTr("Weights")
                        color: theme.text
                        font: uiFont
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    Column {
                        id: weightsStateColumn
                        x: 90
                        width: parent.width - 90 - 16
                        anchors.top: parent.top
                        anchors.topMargin: 5
                        spacing: 1

                        Text {
                            id: weightsStateValue
                            width: parent.width
                            text: toolModel.weightsStateText
                            color: toolModel.weightsStateRole === "mixed" ? theme.warning
                                 : (toolModel.weightsStateRole === "baked"
                                    || toolModel.weightsStateRole === "envelope") ? theme.text
                                 : theme.textMuted
                            font: uiFont
                            elide: Text.ElideRight
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                        Text {
                            id: weightsMeshWarning
                            width: parent.width
                            visible: toolModel.weightsMeshWarningVisible
                            text: qsTr("Mesh changed since bake")
                            color: theme.warning
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                        Text {
                            id: weightsErrorText
                            width: parent.width
                            visible: text !== ""
                            text: toolModel.weightsError
                            color: theme.warning
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }

                    HoverHandler { id: weightsHover }
                    ToolTip.visible: weightsHover.hovered && toolModel.weightsStateTip !== ""
                    ToolTip.delay: 500
                    ToolTip.text: toolModel.weightsStateTip
                }

                // ONE primary verb and the maintenance verbs behind the ellipsis. The
                // button is the inspector's action shape (height 22, full width) so the
                // dock has one action look; a row of bare words was not a control.
                RowLayout {
                    id: weightsActions
                    visible: toolModel.weightsActionsVisible
                    x: 12
                    width: parent.width - 28
                    height: 28
                    spacing: 6

                    AeButton {
                        id: weightsPrimary
                        Layout.fillWidth: true
                        height: 22
                        textFont: uiFont
                        // Envelope/Mixed: Bake fills the live nodes. Baked: Rebake is the
                        // primary verb. Mixed also offers Rebake in the menu.
                        text: toolModel.weightsBakeVisible ? qsTr("Bake") : qsTr("Rebake")
                        enabled: toolModel.weightsBakeVisible ? toolModel.weightsBakeEnabled
                                                             : toolModel.weightsRebakeEnabled
                        onClicked: toolModel.requestWeightsAction(toolModel.weightsBakeVisible ? 0 : 1)

                        property bool rowHovered: hovered
                        AeToolTip {
                            ownerItem: weightsPrimary
                            shown: weightsPrimary.rowHovered
                            text: toolModel.weightsBakeVisible
                                  ? qsTr("Evaluate the selected bone's live envelope once, so the brush can paint it")
                                  : qsTr("Replace the selected bone's authored field with the current envelope (one undo step)")
                        }
                    }

                    ToolMenu {
                        visible: toolModel.weightsRebakeVisible
                        // The menu mirrors the actions that are NOT the primary verb: on
                        // Mixed the primary is Bake, so Rebake lives here; on Baked Rebake
                        // is the primary and only the two maintenance verbs remain.
                        items: {
                            let rows = [];
                            if (toolModel.weightsBakeVisible) {
                                rows.push({ label: qsTr("Rebake All"), action: 1,
                                            enabled: toolModel.weightsRebakeEnabled });
                            }
                            rows.push({ label: qsTr("Revert to Envelope"), action: 2,
                                        enabled: toolModel.weightsRevertEnabled });
                            rows.push({ label: qsTr("Clear Weights"), action: 3,
                                        enabled: toolModel.weightsClearEnabled });
                            return rows;
                        }
                        label: qsTr("More weight actions")
                        onTriggered: (action) => toolModel.requestWeightsAction(action)
                    }
                }

                // WHAT a stamp does to the value it covers. The brush's own Size and
                // Hardness are below; the envelope brush's target Value sits with the
                // brush rows, and the weight brush has three deposit rules.
                ToolRow {
                    visible: toolModel.weightsStateVisible
                    label: qsTr("Paint")
                    AeComboBox {
                        Layout.fillWidth: true
                        height: 23
                        textFont: uiFont
                        options: toolModel.weightModes
                        index: toolModel.weightPaintMode
                        // A stable name for a reader/probe: the combo's own name is the
                        // current VALUE ("Add"), which changes as the mode does.
                        Accessible.name: qsTr("Paint mode")
                        onActivated: (index) => toolModel.weightPaintMode = index
                    }
                }

                // The bone-scoped brush needs to say how a bone becomes the target:
                // left is always the stroke, so the target is chosen with the other
                // button (or, for the first stroke, by the nearest bone).
                Text {
                    visible: toolModel.weightPaintHintVisible
                    x: 12
                    width: parent.width - 28
                    text: qsTr("Right-click a bone to target it; the brush edits only that bone")
                    color: theme.textMuted
                    font: uiFont
                    wrapMode: Text.WordWrap
                }

                // ---- SR transform: the key-creation toggles -----------------
                // "Transform" mode offers one toggle per channel it may create, and the
                // "Move Centroid" mode offers the centroid offset instead - which is why
                // the four rows are one data-driven set with a family flag rather than
                // four hand-bound checkboxes.
                Repeater {
                    model: toolModel.srtChecks
                    delegate: ToolCheckRow {
                        visible: modelData.key === "centroid" ? toolModel.srtCentroidVisible
                                                             : toolModel.srtCreateVisible
                        text: modelData.label
                        tip: modelData.tip
                        value: toolModel.srtCheckValues[index]
                        onToggled: (checked) => toolModel.setSrtCheck(index, checked)
                    }
                }

                // ---- pose: the drawing weight ------------------------------
                ToolSliderRow {
                    visible: toolModel.weightVisible
                    label: toolModel.weightLabel
                    from: 0
                    to: toolModel.percentMax
                    step: 1
                    decimals: 0
                    suffix: "%"
                    value: toolModel.weight * 100
                    onValueEdited: (value) => toolModel.weight = value / 100
                }

                // ---- FFD: the pencil's smoothing ---------------------------
                ToolSliderRow {
                    visible: toolModel.blurVisible
                    label: toolModel.blurLabel
                    from: 0
                    to: toolModel.percentMax
                    step: 1
                    decimals: 0
                    suffix: "%"
                    value: toolModel.blur * 100
                    onValueEdited: (value) => toolModel.blur = value / 100
                }

                // ---- mask: WHICH field --------------------------------------
                // ONE chooser, and only when the field is still a QUESTION. Entered from
                // a card the heading already names the field (a filter's "Paint mask", the
                // mask card's Paint) and this row is not drawn at all - the first version
                // asked "Mask" and then "Channel", which named one field in two steps and
                // asked a question the card had answered.
                Item {
                    visible: toolModel.maskPickersVisible && !toolModel.maskPinned
                    width: parent.width
                    height: 26

                    AeComboBox {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        height: 23
                        textFont: uiFont
                        options: toolModel.maskFields
                        index: toolModel.maskFieldIndex
                        onActivated: (index) => toolModel.setMaskField(index)
                    }
                }

                // WHAT the brush paints toward. The eyedropper (Alt + click on the
                // canvas) samples it, and the swap key flips it between the two ends -
                // both in the row's tooltip, because a keyboard or mouse gesture spelled
                // out on its own line is what made this panel busy.
                ToolSliderRow {
                    visible: toolModel.maskPickersVisible
                    label: qsTr("Value")
                    tip: toolModel.maskValueHint
                    from: 0
                    to: toolModel.percentMax
                    step: 1
                    decimals: 0
                    suffix: "%"
                    value: toolModel.maskValue * 100
                    onValueEdited: (value) => toolModel.maskValue = value / 100
                }

                Item { width: 1; height: toolModel.brushVisible ? 2 : 0 }

                ToolBrushSection {
                    visible: toolModel.brushVisible
                    // The weight brush is shown but disabled on a live bone; the
                    // state panel above explains the one action that unlocks it.
                    enabled: toolModel.brushEnabled
                }

                // The influence modes' second readout: the envelope polygons show
                // WHERE a bone reaches, this shows how strongly (and only the
                // influence modes have a bone to read).
                ToolCheckRow {
                    visible: toolModel.weightHeatmapVisible
                    text: qsTr("Show weights")
                    tip: qsTr("Colour the layers this bone deforms by its influence weight")
                    value: toolModel.weightHeatmap
                    onToggled: (checked) => toolModel.weightHeatmap = checked
                }

            }

            Item { width: 1; height: 12 }
        }
    }
}
