pragma Translator: "AnimeEffects.Dialogs"

import QtQuick

// A hover label for an icon-only control.
//
// It is a CHILD of the control it labels, which is what gives it a position; the
// owner drives it:
//   AeToolTip { text: qsTr("Remove this range"); shown: mouse.containsMouse }
//
// Hand-rolled for the design's sake, not for want of Controls: contract line E30
// specifies that it is drawn above its owner and does not intercept the pointer, a
// Popup would take a window of its own and the shared instance behind Controls'
// attached ToolTip cannot be positioned per site. (The old header gave a different
// reason - "a QQuickWidget cannot render a popup outside its own bounds" - which is
// untested and false for Qt 6.8+ with `Popup.Window`; that is not why this exists.)
//
// TIMING (contract line E26): it appears after a delay and never instantly, and it
// hides after a timeout or as soon as the pointer leaves.
//
// The delay is a CONSTANT of the design rather than a value read from Qt, and that
// is measured, not assumed: on Qt 6.11.2 there is nothing to read. Qt 6.11 ships no
// tooltip-delay API at all - the whole installed header tree matches no showDelay,
// toolTipDelay or hideDelay - Qt Quick Controls' ToolTip.delay defaults to 0, no
// installed style overrides it, and Qt's own QToolTip fires 1-2 ms after the hover
// (measured, this spike: ENTER 2846 ms / TOOLTIP 2848 ms). QStyleHints::
// toolTipWakeUpDelay only exists from 6.12. So "the platform's wake-up delay" here
// means the conventional desktop one: 500 ms here, 10 s before it retires itself.
Item {
    id: root

    property string text: ""
    // Driven by the owner: true while the pointer is on the control.
    property bool shown: false

    // The two E26 timings. Named and at the top because the contract calls them
    // out, and because if the project moves to Qt 6.12 the first one should become
    // `Qt.styleHints.toolTipWakeUpDelay` (read-only there) rather than a constant.
    property int delay: 500
    property int timeout: 10000

    // Whether the label is actually on screen: `shown` only REQUESTS it, after
    // `delay`. Keeping the two apart is what makes "the pointer left while the
    // tooltip was still waiting" simply cancel, instead of showing a tooltip for
    // something the pointer has already left.
    property bool revealed: false
    visible: revealed && text !== ""
    z: 1000
    implicitWidth: label.implicitWidth + 14
    implicitHeight: 22
    x: parent ? Math.round(parent.width / 2 - width / 2) : 0
    y: parent ? -height - 6 : 0

    function disarm() {
        showTimer.stop();
        hideTimer.stop();
        revealed = false;
    }
    function arm() {
        disarm();
        showTimer.start();
    }
    onShownChanged: { if (shown) arm(); else disarm(); }
    // NEW TEXT WHILE STILL HOVERED IS A NEW TOOLTIP and gets its own delay. The
    // picker is the case that matters: its rows set `shown: hoveredRow >= 0`, which
    // stays true while the pointer scans from row to row and only `text` changes.
    // Without this the label would follow the pointer instantly - the same defect
    // E26 names, arriving through the back door. Re-arming also restarts the hide
    // timeout, so every tooltip gets the full 10 s.
    onTextChanged: { if (shown) arm(); }
    Component.onCompleted: { if (shown) arm(); }

    Timer {
        id: showTimer
        interval: root.delay
        // Re-checked here rather than trusted: `shown` may have gone false while the
        // delay ran (a fast pointer), and the text can be cleared the same way.
        onTriggered: {
            if (root.shown && root.text !== "") {
                root.revealed = true;
                hideTimer.restart();
            }
        }
    }
    Timer {
        id: hideTimer
        interval: root.timeout
        onTriggered: root.revealed = false
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: theme.floaterBody
        border.width: 1
        border.color: theme.floaterEdge
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: theme.text
        font.pixelSize: 11
    }
}
