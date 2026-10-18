#!/bin/bash
# DOES THE VIEW SETTINGS GRAPH TOGGLE PUT THE ANIMATION-GRAPH HUD ON THE VIEWPORT?
#
# WHAT IT PROVES, AND WHY NOTHING ELSE CAN. tests/graph_curve.cpp drives the graph value
# model, tests/graph_drag.cpp the edit path and tests/graph_panel_model.cpp the
# engine-neutral panel geometry; tests/graph_panel_pixels.cpp is the pixel gate, and it
# hands gui::drawPanel a SYNTHETIC model inside one real ImGui frame and reads the
# framebuffer back with glReadPixels; tests/overlay_engine_primitives.cpp pins the GL paint
# engine's primitive subset and tests/graph_image_step.cpp the image-offset channel's step
# against a headless GL context. Every one of them passes whether or not the panel is ever
# put on screen - and tests/imgui_a11y.cpp says as much itself: it decides that the
# published tree says what the model says, and names its own gap as "whether the AT-SPI
# bridge forwards this tree, and whether a real screen reader sees it - that needs the live
# overlay under Xvfb plus a dump of the real window". That is this probe. What no test can
# see is the thing asserted here: whether ctrl::Driver draws that panel OVER the live canvas
# inside gui::MainDisplayWidget, for the curves of the layers the object tree ACTUALLY has
# selected, inside the CENTRAL widget - and whether the icon-only toggle in the View
# Settings panel is wired to it at all. That composition happens in the GL paint path of a
# real, mapped window.
#
# WHY THE PROJECT IS PINNED, AND WHAT THE PIN REALLY LOADS. This probe used to take
# whatever _lib.sh found - "the smallest .anie under ~/Downloads" - so its own SUBJECT moved
# when the filesystem did: one run measured object-tree cells named `running thru my mind`
# and `Layer1` where the recorded run had `Folder16`, and a phase-3 assertion flipped with
# it. A probe whose data comes from the current contents of ~/Downloads cannot be compared
# between runs, and its thresholds cannot be re-measured. It now passes the repository's own
# fixture (tests/fixtures/e2e_stage.png, the file tests/media_exporter_e2e and
# probe_exportbg use) as its project ARGUMENT - and that argument is NOT what the
# application opens: Main.cpp:212 opens only a `.anie` (arguments.last().contains(".anie")),
# so MEASURED, the app keeps the Debug build's own startup project assets/sample.psd
# (AE_SAMPLE_PSD, Main.cpp:208) and the fixture's name appears nowhere in its log. That file
# is in the repository too, so the probe's subject is a committed file either way; the pin
# ASSERTIONS say which one it really was, from the app's own log ("image path = ...") and
# from the object tree's first row, so a run that silently loads something else fails
# instead of quietly re-recording. Pass arg 1 or AE_PROJECT to test another project: the pin
# assertions are skipped then, because the subject is the caller's choice.
#
# THE HONEST STATEMENT OF THAT GAP: the subject is an accident of the BUILD CONFIGURATION,
# not the fixture. There is no .anie file anywhere in this repository (MEASURED: `find . -name
# '*.anie'` returns nothing, and only the command line's .anie branch reaches
# MainWindow::onOpenRecentTriggered), and the only way to give the probe another subject
# would be to make the application open one - a source change, which this probe may not
# make. So: the fixture argument's ONLY job is to keep _lib.sh from searching the filesystem,
# the loaded project is the Debug build's own `assets/sample.psd`, and the header says so
# here and in the pin assertion's message. If AE_SAMPLE_PSD is ever changed or removed, the
# probe fails loudly at those two assertions rather than measuring something else - which is
# the whole point of pinning it - and a run that must use a project of its own says
# AE_PROJECT=<path> (template: the pin assertions are skipped, and the header's measured
# numbers stop applying).
#
# THE RENDERER, since thresholds are pixels. These captures are llvmpipe. MEASURED:
# `xvfb-run -a --server-args="-screen 0 1440x900x24" glxinfo -B` (the same wrapper this
# probe runs the app in) reports Device: llvmpipe (LLVM 22.1.8, 256 bits), Mesa 26.2.2,
# Accelerated: no - while the developer's own session reports the real GPU (AMD Radeon
# R9 380, radeonsi, tonga). So every number below is a software-rasterised Mesa
# measurement, and the same probe on a radeonsi or llvmpipe-with-other-LLVM build is
# expected to agree on counts and disagree on a handful of antialiased edge pixels: this is
# why the assertions that matter are exact-zero identities or order-of-magnitude bars, and
# never a two-pixel tolerance.
#
# WHAT IT MEASURES, recorded on 2026-10-13 on this machine's 1440x900 probe window, with
# the probe's own pinned project (see above: the fixture argument, so the loaded project is
# assets/sample.psd - a project whose top node carries NO keys and whose child layers carry
# none, which is why phase 2 adds one and phase 3 can then delete the very layer that
# carries it):
#   * the window's client area is 702x455 - the top-level frame reports [0 0 702 455] -
#     and inside it the accessibility tree names the three regions this probe measures
#     against: `Properties` [382 21 320 434] (docked right) and `Animation`
#     [0 185 382 270] (docked bottom), plus the graph toggle itself [8 88 24 24].
#   * OFF -> ON changes 2332 pixels with the tree's top row selected and 2200 with
#     nothing selected - all of it inside the crop. RE-MEASURED after the contrast-outline
#     pass landed, which is why these are not the 3681/3585 recorded against the halo
#     build: the same states move fewer pixels inside the same box. The toggle's own 24x24
#     checked highlight is 568 px = 0.91% of the crop and the bar is 1000, so the bar cannot
#     be cleared by the button repainting itself; the per-column split recorded earlier (0 px
#     across the Tools dock) is NOT re-measured here - the claim this bullet carries is the
#     bar, not the split.
#   * the changed region's bounding box is 370x143+8+39 on BOTH of those states: x 8..377
#     and y 39..181, i.e. LEFT of the Properties dock (x=382) and ABOVE the Animation
#     panel (y=185). Everything the panel draws - frame ticks, frame numbers, the value
#     labels, the zero rule, the 1px playhead, the centred state line and the channel
#     button - is in there: the Tools dock (x 0..176) changes 0 pixels apart from the toggle.
#   * OFF -> ON -> OFF restores the pixels exactly: the two OFF shots differ by 0.
#   * the empty state, with the panel ON and NO layer selected, publishes the model's own
#     sentence as an accessible label ("Select a layer to show its curves"), and its
#     change inside the crop is 2200 px = 3.51% (3585 = 5.72% before the outline pass) -
#     the smallest of the states this probe measures, as a
#     text-only panel should be.
#   * PHASE 2, one explicit key added to a child layer by pressing the Current Keys knob:
#     the control pair is 0 px, and the plot repaints by 2162 px = 3.45% inside the crop.
#   * the channel list the press produces is a real control, and it is the panel's ONE
#     control: a PushButton whose own NAME is the count it changes ("1 channel(s)" for the
#     one channel the press keyed), which opens the application's own QMenu - a native
#     window, one row per channel plus a disabled heading per layer, each row carrying a
#     hue swatch (filled = shown, hollow = hidden). Hovering a row focuses that channel
#     (the panel's line then reads "..., focused: <layer> : Rotate"); activating it removes
#     its curve from the crop, 2078 px = 3.32% inside the crop and 0 outside it, and moves
#     the button's own name to "0 of 1" while the model says the channels are hidden;
#     activating it again restores the crop EXACTLY (0 px). The per-row CheckBoxes the
#     panel used to publish are gone with the model's legend label, so the row COUNT claim
#     is now the button's own count.
#   * PHASE 3, THE DANGLING TARGET - the gate the rest of this probe cannot be. The layer
#     the panel is DRAWING is deleted through the object tree while the panel is on
#     screen, and the app has to come out of it alive. MEASURED on the state phase 2
#     leaves behind: the model names the layer ("Folder16 : Rotate"), the tree publishes
#     it as a cell at 40 221 115 22, a right-click on the LEFT EDGE of that cell (not its
#     centre: see WHERE THE MEASUREMENTS ARE MADE) opens the object tree's row menu at the
#     click point (window 0x200036, 158x223+44+232, Map State IsViewable), and Up+Return
#     with the input focus pointed at that window presses its last entry, which is Delete.
#     Afterwards: the process is ALIVE, no `table cell` named Folder16 answers, the panel
#     publishes one of the model's sentences again ("No keys on the selected layers") with
#     NO channel control at all (the model publishes the button only while there is a channel
#     to list), and the crop changes 9043 px = 14.43% against a control pair of 0 px (28944 px whole screen).
#     The crop number is NOT the curve alone and is not claimed to be - the deleted layer
#     also drew in the crop's canvas strip (the same crop still differs from the panel's ON
#     empty state by 12799 px) - which is why the MODEL is what pins the panel here (the
#     sentence, and a channel control that went from one count to none) and the pixels
#     corroborate it.
#     What would make THAT claim false is a target left in GraphEditor's raw-pointer list:
#     the draw after the deletion walks it, and a fault there is a dead process, which the
#     alive assertion reports (and which no AT-SPI assertion could, since a dead app just
#     stops answering).
#   * PHASE 4, THE KEY CONTEXT MENU, WHICH IS THE EASING PICKER. A right-click on a key
#     hands the viewport the application's key-menu entry point
#     (ImGuiOverlay::openKeyMenuAt -> MainDisplayWidget::showGraphKeyMenu ->
#     TimeLineEditorWidget::showKeyContextMenu -> openEasingPicker), and that opens the ONE
#     gui::prop::EasingPickWidget popover AT THE POINTER - the SAME control the dock header's
#     easing chip opens from its own anchor (prop::EasingItem::openPicker), so the panel and
#     the timeline still share ONE list of easings and one edit path. The flat 14-row QMenu
#     and its builder (mSelectEasing) are DELETED with that change, and the pick's edit is
#     still the timeline's undoable one, committed as a SINGLE step when the popover closes
#     (EasingPickWidget::hideEvent emits applied() once). What opens is a real Qt popup: a
#     top-level window of its own, NOT pixels inside the panel.
#     WHAT IDENTIFIES IT NOW, AND WHY THE OLD WITNESS CANNOT. This phase used "the window
#     that did not exist before" (a set difference over the root window's children), which
#     worked because every right-click built a FRESH QMenu. The picker is ONE widget that the
#     dock header owns and reuses for every open (prop_Items.cpp: "The one place the popover
#     is opened"), so its native window outlives a close and that set difference only fires on
#     the first open of a session. The witness is therefore what the window PUBLISHES about
#     itself, read from the X server with no window manager in the way:
#       - its own geometry, which is the PICKER'S: the phase reads kPopoverWidth and
#         kNormalHeight/kCustomHeight OUT OF EasingPickWidget's SOURCE and asserts the window
#         it accepts is exactly that wide and exactly that tall (300 x 202 for the non-Custom
#         easings this phase works with; 442 in Custom);
#       - Map State: IsViewable while it is up, and not viewable once it is dismissed;
#       - Override Redirect State: yes - the X server's own answer that this top-level is
#         UNMANAGED, which is what a Qt::Popup is (Qt's xcb backend ORs
#         X11BypassWindowManagerHint for Qt::Popup) and NOT what the application's normal
#         window is.
#     NOT the WM window type: Qt's automatic _NET_WM_WINDOW_TYPE selection covers Tool,
#     ToolTip, Dialog and Splash but NOT Popup, so the picker's window carries no popup atom;
#     the property is RECORDED in the geometry file rather than asserted. The popup's origin
#     is still the anchor - popUpAt moves it to (clamped anchor.x, anchor.y) - so the click
#     point is the window's top-left, the same claim the QMenu phase made.
#     THE TILES ARE THE ENTRY POINT, NOT KEYBOARD ROWS. The picker is a widget popover, not a
#     QMenu: it has no row map, no highlight and no Down/Return navigation, so the three
#     things this phase used to do to the menu - reading its rows by hovering them, asserting
#     its height holds exactly the 14 entries of util::Easing's display order minus Custom, and
#     reaching an entry with Down/Down/Return - are all gone with that control, and are listed
#     as DROPPED below rather than quietly deleted. What the picker offers is its TILE CATALOG
#     (kTileOrder in EasingPickWidget.cpp: Linear, Floor, Round, Cubic, Sine, ...), one
#     checkable QToolButton per entry in a flow row, and this phase PRESSES A TILE with the
#     REAL POINTER at the position the picker's OWN layout constants put it at (root margin,
#     fixed header, body margins, seg row, motion-preview bar, then the flow of kTile-sized
#     tiles kTileGap apart with the vsep after tile 3 - all read from the source, none of them
#     hardcoded), so the control it drives is the one a user clicks.
#     AND IT COMMITS ON CLOSE, which is the other half of what changed: a tile (like the seg
#     buttons and the blend slider) edits a DRAFT, and the model is written once when the
#     popover goes away (EasingPickWidget::hideEvent -> applied(), one undo step for the whole
#     session). So every pick here is a PRESS followed by a DISMISSAL, and the dismissal this
#     phase uses is Escape - which Qt itself makes the popup's cancel key
#     (QWidget::keyPressEvent closes a Qt::Popup on QKeySequence::Cancel, and the picker does
#     not override it). An outside click would do as well (QWidget::mousePressEvent closes a
#     Qt::Popup pressed outside its rect, which prop_Items.cpp records: "the popup is a
#     Qt::Popup, so an outside click hides it on its own"); Escape is used because it is the
#     key this phase already drove at the control it replaces.
#     THE PICKS, NAMED BY THE CATALOG AND ASSERTED BY THE DOCUMENT. The phase presses the SINE
#     tile (kTileOrder index 4, derived from the source rather than assumed) with the pointer -
#     the key carries Linear before it, so the pick is a real change - and then, at a fresh
#     open, the ROUND tile (index 2), which leaves phase 4b re-shaping the key from Sine. Both
#     are read back from the panel's own state line, which names the selected key and the easing
#     it carries and is built from the DOCUMENT, not from the picker's return value. The dock's
#     Easing chip is NOT a readback: its accessible name is the static tooltip "Easing" and no
#     type name (recorded, together with whatever the accessibility tree says about the popup
#     itself while it is up, in this phase's dump).
#     THE POPUP IS MODAL, and this phase still pins both halves of that, because a Qt::Popup
#     grabs the keyboard just as the QMenu did: with the popover OPEN the application's own
#     Delete and Undo key commands do NOT reach the timeline (Delete is not the popup's cancel
#     key, so QWidget::keyPressEvent ignores it and a top-level has no parent to propagate to;
#     and ctrl+Z is consumed by the picker's own session-undo event filter), and with it
#     CLOSED they DO (ctrl+Z moves the line back to the easing the last pick replaced). The
#     popup's own existence is read from the window list each time, never from a pixel bar.
#     THE KEY IS FOUND, NOT PREDICTED, and the reading the phase moves is made decidable
#     first: the phase clears the panel's selection with the graph's OWN deselect gesture (a
#     Ctrl-press in the plot on no key and no band, which GraphEditor::beginMarquee owns and
#     which changes no document byte), then right-clicks along the fit rect's CENTRE ROW -
#     and that row then +/- 8 and +/- 4 px, in 6 px steps, which cannot straddle the 20 px
#     band ctrl::graph::kKeyHitRadius = 10 gives the right-click - and accepts a sample only
#     when its click put the PICKER'S OWN WINDOW on screen (its geometry and map state, as
#     above; the window is NOT viewable before the click, so a sample is still attributable to
#     its own right-click even though the window object outlives the first close). Why the
#     centre ROW: the value axis is fitted, so a single key is vertically centred, while its
#     frame position is the FIT's business, which this phase deliberately does not assert. The
#     right-click that opens the popup SELECTS the key it opens on (ImGuiOverlay::openKeyMenuAt
#     calls editor.selectKey BEFORE it hands the selection on), so the panel's own state line
#     names that key afterwards - and it is a REAL selection, not the timeline's temporary one,
#     so it survives the popover's close (only endTemporarySelection runs there). The assertion
#     is that the selected-key half MOVED from the empty reading taken before the scan. THE ONE
#     LEFT PRESS the phase then makes is the editor's own gesture on the same point, with the
#     selection emptied again first, so it too is asserted by the half moving; it and its
#     deselect run LAST, after phase 4b, because of a MEASURED side effect of the Ctrl-press
#     gesture itself: it leaves the dock's Angle spin box unable to accept typed text for the
#     rest of the session (the measurement, and the fact that it is NOT a stuck modifier or a
#     focus change, are recorded beside the block at the end of 4b; the phase 4b typed write is
#     the instrument that shows it, so the write is made first).
#     THE NEGATIVE is the true claim, not "changes nothing": a right-click that hits no key
#     opens NO popover at all and leaves the state line alone - the witness is the window list,
#     which for an inert right-click holds no viewable picker window (the picker's own window
#     is already in that list, unmapped, from the first open of the session).
#     DROPPED WITH THE DELETED CONTROL, rather than silently: the menu's row map (two rows
#     hovered for their inset, pitch and width), the assertion that its height held exactly the
#     14 display-order entries minus Custom, the Down-arrow highlight measurement, and the
#     Down/Down/Return keyboard pick - none of them can hold for a popover that has no rows.
#     What they carried that CAN: the modality pair above, and a pixel witness inside the
#     popup's own rectangle for the press (pressing the Round tile with the popover up changes
#     the popup's own pixels - the checked tile's accent fill and glyph - and the changed region
#     is asserted to lie inside the popup's own window). The picker's motion-preview bar
#     animates between ANY two captures, so that assertion is the claim "the press landed on the
#     picker's own surface" rather than a claim about the tile's fill alone; WHICH entry was
#     pressed is the state line's business, as it was for the menu.
#     AND WHY THERE IS A PHASE 4b UNDER IT. The state line is the only readback the pick's
#     effect HAS while the panel shows ONE key: graph::Curve::buildPolyline emits nothing for
#     a single-key map, so no segment is drawn at all; the panel paints its summary line only
#     while the model is EMPTY; and MEASURED, changing that one key's easing moves 0 pixels.
#     Phase 4b therefore MAKES the segment - a second key one frame right of the first,
#     carrying a DIFFERENT value, added through the dock's own add-key knob and its Angle spin
#     box with a real edit (the box's handler hangs off editingFinished, which an
#     accessibility write does not fire) - and then asserts the CURVE'S own pixels: the
#     segment appears with the second key, re-shaping it from Sine to Linear moves the
#     polyline, and picking the SAME easing a second time moves exactly 0 px, which is what
#     makes the first assertion about the EASING rather than about the picker having been used.
#     THE TYPED WRITE IS ALSO THE INSTRUMENT FOR A DEFECT THE IMGUI PHASE RECORDED and which
#     the Qt popover is expected to have removed: typing into the dock's Angle field used to stop
#     committing after the key menu had been used. Phase 4b's write happens AFTER five popover
#     openings, and it is asserted to commit (the field's own Value interface is read back and
#     compared with what was typed), with the in-progress readings beside it so a failure says
#     whether the characters arrived and the commit was refused or never arrived at all.
#     THE MUTATION THIS HALF WAS RECORDED AGAINST no longer exists (the ImGui
#     ImGuiConfigFlags_NavEnableKeyboard toggle and its SetNextFrameWantCaptureKeyboard line
#     are deleted with the popup). What replaces it as the control is the application's own
#     modality, which is not this probe's code: deleting the key the popover was opened on with
#     the popover closed (phase 3's next-door behaviour) is what the blocked-Delete assertion
#     would have done instead, and the window-list half of that assertion goes red if the
#     popover is not really up.
#
#   * PHASE 5, THE WHEEL. Which side gets a wheel over the viewport is decided in
#     ctrl::Driver::updateGraphWheel, and MEASURED here in both directions at ONE point (the
#     viewport's centre) with the same 3 notches: with the panel VISIBLE the wheel moves the
#     panel's own pixels (798 px = 1.27% of the crop - the frame ladder relaid out) and leaves
#     every pixel outside the panel's ink box byte-identical (0 px), while with the panel
#     HIDDEN the same wheel scales the artwork (12842 px = 20.5% of the crop, changed region
#     96x137+223+18 - the viewport). The first is the claim "the wheel is the graph's while it
#     is up, unconditionally"; the second is its control, since it is the same region and the
#     same gesture with the other owner. The wheel is also the ONLY thing in this probe that
#     writes the panel's frame window, so the phase restores it (unasserted) and phase 4's own
#     Fit graph settles it after that. NEITHER WHEEL CAPTURE FORCES A PAINT: the frame is the
#     application's own (MainDisplayWidget::wheelEvent asks for it when the graph took the
#     event), so the visible case would go red if that request were dropped - the same way it
#     did before it was added, when the whole phase needed a toggle cycle to see the zoom.
#   * THE RING CAPTURE, which is NOT a gate and does not carry a threshold. One frame is taken
#     for the EYE: the panel drawn over the artwork ZOOMED IN, so that its white ink carries
#     its 1px dark outline ring (the contrast-outline pass) over something bright. The recipe
#     is the wheel phase 5 has just driven in both directions, used the other way round: the
#     graph hidden so the wheel is the CANVAS's, zoomed IN at the brightest area the artwork
#     has (found by blurring a capture of it taken with the graph hidden), and then the graph
#     shown again over that. MEASURED in the run recorded here, the anchor lands on the
#     character's face and hair and 12 notches (about 3x) puts them under the whole panel: the
#     tick row, the value column, the zero rule and the state line all sit on skin and hair.
#     The only assertions about it are that the file exists and is not a black frame; the mean
#     luminance of the artwork under the panel's ink box is RECORDED beside it in the geometry
#     file. Whether the ring READS at that zoom is the reader's judgement, which is exactly
#     why it is not an assertion here. See the (iii) comment in the body for the derivation of
#     the anchor and for why the zoom round-trips exactly.
# WHERE THE MEASUREMENTS ARE MADE, AND WHAT THE CROP IS (this probe used to ask for the
# project tab strip and crop from its bottom edge; that is gone, and this is why).
# MEASURED: the tab strip is not in the accessibility tree of this build AT ALL. The full
# role inventory of `ae dump` (the whole window, every node) contains no `page tab`, `tab`
# or `page tab list` role under any name, with the panel on and with it off; the only
# table-ish nodes are the object tree's own (`table column header`, `table cell`), and the
# roles that do answer are `frame`, `menu bar`, `menu item`, `panel`, `label`, `button`,
# `check box`, `spin button`, `slider`, `list`, `tree` and the fillers around them. Earlier
# passes of this probe did not notice because they ran against projects whose FILE name and
# tab title agreed, so `ae extents` matched a node that happened to carry the project's
# name. The crop is therefore derived from nodes that DO answer:
#   * the window's client area, `ae extents AnimeEffects frame` - the top-level frame
#     [0 0 702 455] - which bounds the crop on the left and provides the x origin the
#     screen capture is cropped at (the window is at the screen origin under Xvfb, but the
#     crop says so rather than assuming it);
#   * `Properties` [382 21 320 434]: its LEFT edge is where the viewport ends;
#   * `Animation` [0 185 382 270]: its TOP edge is where the viewport ends;
#   * the menu bar's bottom, which is where the crop starts. The menu bar itself has NO
#     accessible name (measured: the frame's own row is `menu bar` with an empty name
#     beside `panel Properties`, `panel Animation` and `panel Tools`), so it cannot be
#     asked for; the dock area starts exactly under it, so the TOP EDGE OF THE
#     PROPERTIES DOCK is that line (measured y=21, the same as the Tools dock's
#     [0 21 172 164]), and the frame's top is asserted to be above it.
# ONE KIND OF RECTANGLE THIS PROBE DOES NOT TRUST, and phase 3 is the only place it
# matters: the object tree's rows report their full LOGICAL width, not what the tree's
# viewport shows. MEASURED: "Folder16" reports 40 221 115 22 while the dock's splitter
# leaves the tree about 68 px wide, so a click at the CENTRE of such a rect lands in the
# timeline beside the tree - and opens the TIMELINE's menu (read off a screenshot: Copy
# key / Copy key to clipboard / Paste key / Select spacing between keys / Change key(s)
# easing to / Change key(s) range to / Delete key) instead of the tree's. Phase 3
# therefore clicks the LEFT edge of a row rect + 4 px, which is inside the row and inside
# the tree. Nothing above uses a tree row's rect as a bound: the crop comes from the two
# docks and the frame (this paragraph is why).
# So the crop is 382x164+0+21 = 62648 px: the full width left of the Properties dock,
# from the menu bar's bottom to the Animation panel's top. It contains the whole drawn
# panel (measured x 192..377) and the toggle's highlight, and nothing that changes for
# any other reason.
#
# WHAT IS NOT COVERED, stated so nobody has to measure it again:
#   * the panel's OWN accessible rectangles do not agree with where it draws. MEASURED:
#     the state line reports [222 25 156 156] (the plot), while the pixels the panel
#     changes are at x 192..377, y 39..181.
#     The x is consistent with the drawn panel - 192 is where its leftmost ink is (the
#     value column and its labels, left of the plot at 222) - but the published top (25)
#     is 14 px ABOVE the highest pixel the panel paints (39), so the two spaces are offset
#     vertically. Nothing here uses those rectangles as geometry: the crop comes from the
#     docks, the panel's channel button is activated through the AT-SPI ACTION interface
#     (never at a coordinate - that action opens the list, and the LIST is then driven by
#     its own window and pixels), and the assertions are about pixels. The offset is the
#     accessibility layer's business, not this probe's, and it is recorded here rather than
#     worked around.
#   * the tab-strip role is asserted nowhere here; a build that starts enumerating the tab
#     strip would neither fix nor fail this probe.
#   * the object tree's CONTEXT MENU is not published to AT-SPI, so phase 3 cannot address
#     its entries by accessible name - the route every other step of this probe uses.
#     MEASURED with the menu open on screen and its window mapped IsViewable: the tree of
#     the app still held exactly the five menu-bar popups and no `menu item` named Delete,
#     while a desktop-wide walk found "Delete" only in OTHER applications on the bus. The
#     menu is a QMenu created on the stack inside ObjectTreeWidget::onContextMenuRequested
#     and, unlike the menus of the menu bar (which are published even while closed), it is
#     never exposed. Phase 3 therefore finds the menu by its WINDOW - the app opens it at
#     the pointer, so the window whose origin is the click point is it - and drives it with
#     Up+Return after pointing the input focus at that window: that is the mechanism this
#     probe can actually drive, because there is no window manager (the menu takes no focus
#     by itself and keys sent without focusing it do nothing - MEASURED, Up+Return left the
#     row in place), and the menu type-ahead is no name route either (MEASURED: typing
#     d,e,l,e,t,e then Return deleted nothing). WHICH entry that presses is bounded by the
#     SOURCE (onContextMenuRequested adds mDeleteAction last) and by a screenshot read of
#     the menu (Contract / Rename / Create layer / Create folder / Paste from clipboard /
#     Duplicate node / Add default keys / Delete - exactly ONE wording means
#     remove-this-layer), and then by the EFFECT: a wrong entry leaves the row in place,
#     which is an assertion rather than a hope.
#
# ---- POSITIVE CONTROL: PROOF THAT THIS PROBE CAN GO RED ------------------------------
# A gate that cannot be shown to fail is not a gate. The recipe below is written, not run
# here, because it needs a build and this machine runs one build at a time.
#
# THE MUTATION: ctrl::GraphEditor::setVisible turned into a no-op. The toggle still
# flips the view setting, MainDisplayWidget still calls the driver, the setting still
# reaches setVisible - and nothing ever becomes visible, so MainDisplayWidget::paintEvent
# keeps passing `editor && editor->isVisible()` = false to gui::ImGuiOverlay::paint and
# the panel is never drawn nor published. It is the smallest break that removes the
# FEATURE while leaving its wiring intact, and it touches one line.
#
# WITH scripts/mutation_check.sh (it takes a tab-separated spec, builds the mutants itself,
# restores the file byte-exactly with `cmp`, and REQUIRES the reverted tree to be green
# again - a run that cannot be restored stops with REVERT-FAILED):
#     cat > /tmp/graph_overlay_mutation.tsv <<'TSV'
#     graph_overlay_setvisible_noop<TAB>src/ctrl/GraphEditor.cpp<TAB>s/void GraphEditor::setVisible\(bool aVisible\) \{/void GraphEditor::setVisible(bool aVisible) { (void)aVisible; return;/<TAB>src/gui/Debug/AnimeEffects<TAB>bash scripts/gui/probe_graph_overlay.sh
#     TSV
#     scripts/mutation_check.sh /tmp/graph_overlay_mutation.tsv graph_overlay
#   (<TAB> is a real tab; the harness refuses to start while another ninja builds or while
#   an AnimeEffects instance is running, both of which would poison its verdict.)
#
# WITHOUT the harness, the same break by hand - patch that one line of
# src/ctrl/GraphEditor.cpp, then:
#     ninja -C build src/gui/Debug/AnimeEffects
#     bash scripts/gui/probe_graph_overlay.sh ; echo "exit=$?"
#     git checkout -- src/ctrl/GraphEditor.cpp
#     ninja -C build src/gui/Debug/AnimeEffects
#     bash scripts/gui/probe_graph_overlay.sh ; echo "exit=$?"   # green again
#
# WHAT A WORKING PROBE MUST REPORT UNDER THAT MUTATION (all of it is expected, and the
# run must end non-zero - `=== probe_graph_overlay: N FAILED ===` and exit 1, or the
# mutation is a survivor and this probe is the finding). The lines below are the probe's
# own messages, so a red run can be read against them one for one:
#   * FAIL  toggling the graph ON redraws the viewport (0 px changed on the whole screen, bar 1000)
#   * FAIL  the panel drew inside the crop (0 px = 0.00% of 382x164, floor 2%)
#   * FAIL  the changed region is not empty (0x0)
#   * FAIL  the empty state line names the empty state (the model's own sentence is the accessible label, rect none)
#   * FAIL  the empty panel still drew its state line (0 px = 0.00% of the crop, floor 2%)
#   * FAIL  the Current Keys add-key knob is addressable for the layer the tree had selected (last row activated: none; rect none)
#   * FAIL  the press turned the knob row into the channel rows, i.e. the key now exists (Angle row none)
#   * FAIL  and the phase-2 key makes the panel publish it, with its own name counting exactly one channel (none)
#   * FAIL  activating the row toggles that channel's visibility, and the panel's own control says so ... (none)
#   * FAIL  hiding the channel from the list removes its curve from the crop (0 px = 0.00% gone, bar 200)
#   * FAIL  the panel falls back to one of the model's own empty-state sentences after the
#     deletion (none; rects: empty none, no-keys none, all-hidden none)
#   * FAIL  the right-click opened a popup whose pixels are a menu, not a sliver (none)
#   * FAIL  with the graph visible a wheel over the viewport moves the panel alone (0 px ...)
#     and FAIL  and the canvas is byte-identical ... (the panel never being visible hands the
#     wheel to the canvas, so BOTH wheel assertions go red under this mutation, in opposite
#     directions - the visible case stops moving and the hidden case starts)
#   * FAIL  the second key reached the drawn panel ... (0 px: nothing was drawn at all)
# EXPECTED, not measured here (the build is yours to run): the whole-screen change under
# that mutation is the toggle's own checked highlight and nothing else - 568 px is what
# that highlight measures on this build, which is exactly why the bars sit above it. The
# ceiling assertions (the panel is not a wash, the empty state is text only) are expected
# to still PASS on 0 px, and that is correct rather than a gap: a panel that draws nothing
# cannot be a wash. What the ceiling buys is the other direction - it cannot be cleared by
# a panel that fills its background. A run that ends GREEN under this mutation is the
# finding: it would mean no assertion above depends on the panel being drawn.
#
# PHASE 3 UNDER THAT SAME MUTATION, read the same way. The Current Keys knob is a widget of
# the Properties dock, not of this overlay, so the deletion still happens: the tree row is
# found, the menu opens at it, the row goes, and the process survives - those three
# assertions are expected to PASS, and they are about the tree rather than about the panel.
# The one line that must go red is the sentence one above: with setVisible a no-op the
# overlay publishes nothing at all, so no empty-state sentence is there to be found, while
# the channel-control absence passes vacuously (the control is published by the same model
# the mutation silences). Note also what this mutation does NOT
# exercise: it never leaves a destroyed node in a live target list, so the assertion that
# carries THIS gate's verdict - the process still being alive after the deletion - is
# expected to pass under it. THAT control is the other direction and is named rather than
# written out, because it changes different code: the gate exists for the ORDERING that
# takes the node out of ctrl::GraphEditor::mTargets before the next draw (the tree's
# selection change, MainWindow's setGraphTargets), so anything that lets a destroyed node
# stay in that list across a repaint - dropping the selection re-emit on the deletion path,
# or moving setGraphTargets off the tree's selection signal - is what phase 3 is aimed at,
# and a use-after-free there is a dead process rather than a red assertion.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# ---- PIN THE PROJECT, OR THE PROBE'S OWN SUBJECT DRIFTS (see the header) ----
# _lib.sh, given no project, opens "the smallest .anie under ~/Downloads" - so the
# probe's data was whatever the filesystem happened to hold: a run recorded against
# `running through one_s head.anie` measured object-tree cells named `Folder16` and a
# phase-3 assertion flipped when a different project became the smallest. A probe whose
# data comes from the current contents of ~/Downloads cannot be compared between runs.
#
# The pin is the repository's own fixture (the one tests/media_exporter_e2e and
# probe_exportbg use). It cannot be the project the app OPENS: the application's command
# line opens only .anie (Main.cpp:212), so this argument is NOT opened - MEASURED, the
# app's log then names its own Debug startup project assets/sample.psd (AE_SAMPLE_PSD,
# Main.cpp:208), which is also in the repository. That is what the probe runs on, and it
# says so below rather than pretending the fixture is loaded. What the argument buys is
# the one thing the probe needs: it is a real file, so _lib.sh never falls back to the
# drifting search. Pass arg 1 or AE_PROJECT to test another project; the pin assertions
# are skipped then, because the subject is the caller's choice and not this pin.
PIN_FIXTURE="$SCRIPT_DIR/../../tests/fixtures/e2e_stage.png"
PINNED=0
if [ -z "${1:-}" ] && [ -z "${AE_PROJECT:-}" ]; then
    set -- "$PIN_FIXTURE"
    PINNED=1
fi
source "$SCRIPT_DIR/_lib.sh" "$@"

rm -f /tmp/probe_graph_overlay_geom.txt

# SCRATCH ON THE REPOSITORY FILESYSTEM, NOT THE QUOTA-ED tmpfs. /tmp on this machine is
# a tmpfs with a 19 GB per-user quota, and this probe's own runs have filled it: a 20 GB
# /tmp produced "Disk quota exceeded" mid-run (measured 2026-10-14), which is a failure
# of the filesystem, not of the panel, and it surfaces as a mysteriously dead run.
# Everything that honours TMPDIR - ImageMagick above all, which this probe calls for
# every pixel comparison - now writes beside the repository, and the directory goes away
# with the script.
#
# AND IT MUST BE A DIRECTORY THAT EXISTS: the first version of this used
# "$AE_PROBE_DIR/build/tmp-probe-XXXXXX", and $AE_PROBE_DIR is a bare `mktemp -d` with no
# `build` in it, so the mktemp FAILED ("no such file or directory"), TMPDIR came out EMPTY,
# every scratch consumer fell back to /tmp - the quota-ed tmpfs this block exists to avoid -
# and the trap removed nothing. Measured 2026-10-14 on the run that still died at 37 s.
# $ROOT/build is the repository's own build tree: it exists whenever the binary this probe
# needs exists, and it is on the same 23 GB filesystem, so no quota can reach it.
export TMPDIR="$(mktemp -d "$ROOT/build/tmp-probe-XXXXXX")"
trap 'rm -rf "$TMPDIR"' EXIT

# THE BODY GOES IN ON STDIN, NOT AS A `bash -c '<body>'` ARGUMENT. THIS IS NOT STYLE.
#
# MEASURED 2026-10-14, the failure this replaces: the body used to be one single-quoted
# argument, and an APOSTROPHE in any of those comments - the body is 1800 lines and its
# comments are full of them - ENDS that quoted string. The shell then went on splitting
# what followed into ordinary words, so `bash -c` was handed THIRTEEN arguments: it ran the
# first 12355 bytes of the 101876-byte body as the script and turned the remaining 89453
# bytes into positional parameters that nothing ever read. The body therefore died at the end
# of that comment, which is 37 s in (wait_ready had just returned), writing NOTHING:
#   * no /tmp/probe_graph_overlay_geom.txt (the body's first write is its geometry one),
#   * an empty /tmp/probe_graph_overlay_body.log (nothing had failed, so nothing was said),
#   * Xvfb killed by xvfb-run's own teardown and the application logging "The X11 connection
#     broke (error 1). Did the X11 server die?" - which is what sent this investigation after
#     an X server that was never the problem.
# The two apostrophes that did it were in the comment "THE FIELD'S OWN ... a spin box's
# Value" at the top of angle_value(); the git-era snapshot /tmp/probe_graph_overlay.sh.good
# (Oct 13) has a body of ONE 79243-byte word and runs, so this was a comment-only regression.
#
# A QUOTED heredoc cannot suffer this: `<<'AE_PROBE_BODY_END'` is verbatim, apostrophes and
# `$` and backticks included, and its length is bounded by nothing (a single ARGUMENT is
# limited to MAX_ARG_STRLEN = 128 KB, which this body was within ~40 KB of). The terminator
# must be a line that appears NOWHERE in the body; keep it that way.
#
# Two consequences to hold on to when editing the body:
#   * `$SHOTS`, `$ROOT`, `$APP` and `$PROJ` are EXPORTED, so the body expands them itself -
#     the `"$SHOTS/x.png"` dance the body used to need is gone (it existed only because
#     the OUTER shell expanded it). Write `"$SHOTS/x.png"` there now.
#   * the body is fed on stdin, so a bare `read` in it would eat the script; every `read`
#     in the body is redirected (`<<<` / `done < <(...)`), and a new one must be too.
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -s 2>&1 <<'AE_PROBE_BODY_END' | tee /tmp/probe_graph_overlay_body.log | tail -4
  cd "$AE_PROBE_DIR"
  "$APP" "$PROJ" >/tmp/probe_graph_overlay.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID
  if ! wait_ready 180; then
      echo "TIMEOUT waiting for the main window" > /tmp/probe_graph_overlay_geom.txt
      kill $app 2>/dev/null; wait $app 2>/dev/null; exit 0
  fi
  W=$(xdotool getactivewindow 2>/dev/null || xdotool search --name AnimeEffects | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  sleep 1

  G=/tmp/probe_graph_overlay_geom.txt

  # Park the pointer off every control before every capture: a hover highlight on a
  # button or on a row is a pixel change of its own.
  park() { xdotool mousemove --sync 900 700; sleep 0.8; }

  # NO FORCED REPAINT ANY MORE. This probe used to force the panel to draw its current
  # content - by toggling the graph off and on - before every capture of a CHANGED panel
  # state, because MEASURED WHEN IT WAS FIRST WRITTEN the HUD did not redraw for a change
  # made while it was already on screen (across a key press the central widget changed
  # 0 px), and because the graph own WHEEL asked for no paint at all: updateGraphWheel
  # returned from MainDisplayWidget::wheelEvent before the viewport own updateRender, so
  # the new frames-per-pixel stayed invisible until something unrelated repainted.
  #
  # EVERY ONE OF THOSE PATHS NOW SCHEDULES THE FRAME ITSELF, and each was re-measured in a
  # run of this script with every toggle removed: the wheel asks for the frame in
  # MainDisplayWidget::wheelEvent (the same request the cursor path makes), a key push and
  # an undo reach MainDisplayWidget::onVisualUpdated through DriverHolder (the slot it keeps
  # on onTimeLineModified, which MainWindow connects to the viewport), a channel-list row asks
  # for one through the menu action (MainDisplayWidget::showGraphChannelMenu -> updateRender),
  # and the key menu is the application own
  # easing picker - a top-level popup the overlay does not draw at all, so the captures of the
  # panel are taken with that popup already gone and the edit its pick committed reached the
  # viewport through that same onTimeLineModified slot.
  # So the captures below are taken straight after the state change with nothing in
  # between, and a phase whose pixels then do NOT move is a real finding rather than a
  # missing toggle.
  #
  # AND ONE THING THE REMOVED CYCLE DID BESIDES PAINT, which is why three recorded numbers
  # moved in the same pass instead of only being re-measured: the toggle ran the panel own
  # fit on the way past - GraphEditor::setVisible applies the pending focus fit when
  # hiding and syncs the value axis when showing - so a capture taken after one was a
  # capture of a window that had been refitted. Re-recorded from the no-toggle run: the
  # key push moves 2162 px inside the crop (was 2637), the whole screen 12807 (was
  # 13282), and hiding the channel from its list 2078 (was 2116). The bars and the shapes of the
  # assertions are untouched.

  # ---- the overlay DRIVES its own items through accessibility, and so does this probe
  #
  # The geometry assertions below are about pixels, but the panel is a drawn surface with
  # no widgets in it: the only handles on its controls are the accessible items it
  # publishes (a11y::sync, see gui/ImGuiOverlay.cpp). The helpers here read the tree through
  # the repository own tooling (scripts/gui/ae_ui.py, imported rather than re-implemented)
  # and are pid-guarded by it, so a second AnimeEffects on the bus cannot be addressed.
  #
  # THE PANEL HAS ONE CONTROL NOW, AND IT IS A BUTTON, NOT A RACK OF CHECK BOXES. It used
  # to publish one check box per legend row - "<layer> : <channel>", one per channel that
  # had a key. It publishes ONE PushButton instead (gui::ImGuiOverlay::publishAccessibility,
  # role PushButton, id kChannelsItemId): its accessible NAME IS THE COUNT the button
  # changes - "N channel(s)" while every channel is shown, "shown of total" once one is
  # hidden (GraphPanelModel::buildChannels) - its VALUE says what it does ("shows the
  # channel list"), and its RECTANGLE is the box the model laid out for it. The button is
  # published only while there is something to list, so with no channel to show there is NO
  # control at all - which is what makes its appearance and its disappearance assertions
  # rather than a count of rows.
  #
  # AND THE LIST ITSELF IS A NATIVE QMenu, NOT ACCESSIBLE ITEMS: pressing the button opens
  # gui::MainDisplayWidget::showGraphChannelMenu - the application's own menu, one row per
  # channel plus a DISABLED heading per layer, each channel row carrying a 12px hue swatch
  # (filled while the channel is shown, hollow while it is hidden) and the channel's own
  # name, with the focused channel in bold. Activating a row toggles that channel's
  # visibility; HOVERING one focuses it (the app's own hover handler), which the panel then
  # reports in its own line ("..., focused: <layer> : <channel>") and draws by emphasising
  # that channel's handles. MEASURED the way the tree's own QMenus are: the menu is NOT in
  # the accessibility tree (ae dump publishes no row of it), so this probe drives it by its
  # WINDOW and its pixels - the same route phase 3 takes to the object tree's menu - and
  # reads the state it changes from the MODEL (the panel's line, the button's own name) and
  # from the pixels.
  #
  # qmenu_at <x> <y>   -> "<id> <w> <h> <x> <y>" for a viewable, menu-shaped top-level that
  #                       is anchored at that point (empty when there is none)
  # row_rect <png>     -> "x y w h": the rectangle a diff image's changed pixels occupy
  # focused_layer      -> the "<layer>" half of the panel's own "focused: <layer> : ..."
  #                       (empty when no channel is focused)
  qmenu_at() { # qmenu_at <x> <y> -> "<id> <w> <h> <x> <y>"
      local id g w h gx gy info
      for id in $(top_windows); do
          g=$(win_geo "$id"); [ -n "$g" ] || continue
          read -r w h gx gy <<< "$g"
          # menu-shaped: narrower than the main window, at least a row tall
          [ "$w" -ge 40 ] && [ "$w" -le 600 ] && [ "$h" -ge 20 ] && [ "$h" -le 600 ] || continue
          # anchored at the point the caller pressed (Qt may nudge a popup to keep it on
          # screen, so this is a bound and the caller asserts the measured numbers)
          [ "$gx" -ge $(( ${1:-0} - 4 )) ] && [ "$gx" -le $(( ${1:-0} + 4 )) ] || continue
          [ "$gy" -ge $(( ${2:-0} - 40 )) ] && [ "$gy" -le $(( ${2:-0} + 40 )) ] || continue
          info=$(win_info "$id")
          case "$info" in *"Map State: IsViewable"*) ;; *) continue ;; esac
          echo "$id $w $h $gx $gy"
          return 0
      done
      return 1
  }
  # The rectangle a diff image's changed pixels occupy, as "x y w h" (0s when empty).
  row_rect() {
      local bb x y w h
      bb=$(magick identify -format "%@" "${1:-}" 2>/dev/null)
      read -r w h x y <<< "$(printf "%s" "${bb:-0x0+0+0}" | tr "x+" "  ")"
      echo "${x:-0} ${y:-0} ${w:-0} ${h:-0}"
  }
  # The layer the panel says it is working on: the "<layer>" of its own
  # "..., focused: <layer> : <channel>" half, which the model builds from the editor's
  # focused channel (ImGuiOverlay::publishAccessibility). Empty when no channel is focused -
  # the caller says which of the two it used rather than assuming one is there.
  focused_layer() { panel_line | sed -n "s/.*focused: \(.*\) : .*/\1/p" | head -1; }

  # The Angle field current value, read from the UNNAMED spin box that sits beside the
  # "Angle" label in the Current Keys panel. It cannot be reached by name - measured: the
  # spin button has no accessible name at all, and `ae value Angle` therefore answers with
  # the label own text - so it is read here from the Value interface of the first spin
  # button that follows a SHOWING "Angle" label in document order (the Rotate group comes
  # before the Pose group in the tree, and only the expanded one is on screen). Empty
  # output and a non-zero exit when the row is not there, which is exactly what "the key
  # does not exist yet" looks like.
  #
  # THE SHOWING FILTER IS THE POINT, and it was measured the hard way: Qt keeps a HIDDEN
  # widget in the accessibility tree, so the first version of this helper read the value
  # "0" out of the Angle field of the Rotate group BEFORE the press, while
  # prop::CurrentKeyPanel::RotateKeyGroup had that whole group hidden and its own knob row
  # standing in for it. `ae extents` does not have that problem because it asks for
  # visible nodes only (ae_ui.find) - so this helper asks the same question through
  # ae_ui.showing, which is what makes "the value is not readable before the press" a
  # statement about the panel rather than about the walk.
  angle_value() {
    python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui

app = ae_ui._app()
if app is None:
    sys.exit(1)
for _depth, role, name, node in ae_ui._walk(app):
    if role != "label" or name != "Angle":
        continue
    if not ae_ui.showing(node):
        continue
    parent = node.get_parent()
    if parent is None:
        continue
    seen = False
    for i in range(parent.get_child_count()):
        child = parent.get_child_at_index(i)
        if child is None:
            continue
        crole = child.get_role_name() or ""
        if not seen:
            if crole == "label" and (child.get_name() or "") == "Angle":
                seen = True
            continue
        if crole != "spin button" or not ae_ui.showing(child):
            continue
        iface = child.get_value_iface()
        if iface is None:
            continue
        value = iface.get_current_value()
        print(int(value) if float(value).is_integer() else value)
        sys.exit(0)
sys.exit(1)
PY
  }

  # THE SAME NODES SCREEN RECTANGLE, for the one place this probe WRITES to the dock
  # (phase 4b own second key needs a value the knob own seeding cannot give it). The write is
  # a real edit - click, select all, type, Return - because the item own change handler hangs
  # off QAbstractSpinBox::editingFinished (prop_Items.cpp:543) and an AT-SPI value write
  # does not fire it. The rectangle comes from the widget itself (ae_ui own own idiom: the
  # SCREEN extents, which for a QWidget in this window are already in window coordinates),
  # so a layout change moves the click with it.
  angle_rect() {
    python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui
from gi.repository import Atspi

app = ae_ui._app()
if app is None:
    sys.exit(1)
for _depth, role, name, node in ae_ui._walk(app):
    if role != "label" or name != "Angle":
        continue
    if not ae_ui.showing(node):
        continue
    parent = node.get_parent()
    if parent is None:
        continue
    seen = False
    for i in range(parent.get_child_count()):
        child = parent.get_child_at_index(i)
        if child is None:
            continue
        crole = child.get_role_name() or ""
        if not seen:
            if crole == "label" and (child.get_name() or "") == "Angle":
                seen = True
            continue
        if crole != "spin button" or not ae_ui.showing(child):
            continue
        comp = child.get_component_iface()
        if comp is None:
            continue
        rect = comp.get_extents(Atspi.CoordType.SCREEN)
        print("%d %d %d %d" % (rect.x, rect.y, rect.width, rect.height))
        sys.exit(0)
sys.exit(1)
PY
  }

  # WHO HOLDS THE KEYBOARD FOCUS, recorded around the typed write in phase 4b. A property
  # field commits on QAbstractSpinBox::editingFinished - Enter, or the focus leaving the
  # field - so a write that lands and does not commit is either a FOCUS problem (the field
  # never saw the Return) or a TARGET problem (it wrote a key the readback does not name).
  # The accessibility tree is the only instrument this probe has for the focus, and this
  # reads the two things that decide it: every node that holds FOCUSED at that moment (the
  # viewport or the panel holding it is the failure mode that matters, because gui::ImGuiOverlay
  # takes the Qt focus while an ImGui text field wants it and must hand it back - and while the
  # easing picker (a Qt::Popup) is up it owns the keyboard itself, which is why this is read
  # with the popover closed),
  # and the Angle field own states.
  dock_focus() {
    python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui
from gi.repository import Atspi

def states_of(node):
    out = []
    try:
        ss = node.get_state_set()
    except Exception:
        return "?"
    for attr in ("SHOWING", "ENABLED", "FOCUSED", "EDITABLE"):
        if not hasattr(Atspi.StateType, attr):
            continue
        try:
            if ss.contains(getattr(Atspi.StateType, attr)):
                out.append(attr.lower())
        except Exception:
            pass
    return " ".join(out) if out else "none"

def holds_focus(node):
    try:
        return node.get_state_set().contains(Atspi.StateType.FOCUSED)
    except Exception:
        return False

app = ae_ui._app()
if app is None:
    sys.exit(1)
focused = []
angle_label = None
for _depth, role, name, node in ae_ui._walk(app):
    if holds_focus(node):
        focused.append("%s|%s" % (role, (name or "")[:36]))
    if angle_label is None and role == "label" and (name or "") == "Angle":
        angle_label = node
print("focus-holders: %s" % ("; ".join(focused) if focused else "none"))
if angle_label is None:
    print("angle-field: absent")
    sys.exit(0)
parent = angle_label.get_parent()
if parent is None:
    print("angle-field: no-parent")
    sys.exit(0)
seen = False
for i in range(parent.get_child_count()):
    child = parent.get_child_at_index(i)
    if child is None:
        continue
    crole = child.get_role_name() or ""
    if not seen:
        if crole == "label" and (child.get_name() or "") == "Angle":
            seen = True
        continue
    if crole != "spin button":
        continue
    print("angle-field: %s" % states_of(child))
    sys.exit(0)
print("angle-field: no-spin-button")
PY
  }

  # THE FIELD'S OWN IN-PROGRESS TEXT, which is NOT what angle_value reads: a spin box's Value
  # interface answers QAbstractSpinBox::value(), i.e. the COMMITTED value, so it reads 0 while
  # the edit is still open even when the line edit already holds the typed characters. Qt
  # exposes the editable line edit as a CHILD of the spin button, and its accessible name is
  # its text - that is what says whether the typed characters ARRIVED. Read-only.
  angle_text() {
    python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui
from gi.repository import Atspi

app = ae_ui._app()
if app is None:
    sys.exit(1)
for _depth, role, name, node in ae_ui._walk(app):
    if role != "label" or name != "Angle":
        continue
    if not ae_ui.showing(node):
        continue
    parent = node.get_parent()
    if parent is None:
        continue
    seen = False
    for i in range(parent.get_child_count()):
        child = parent.get_child_at_index(i)
        if child is None:
            continue
        crole = child.get_role_name() or ""
        if not seen:
            if crole == "label" and (child.get_name() or "") == "Angle":
                seen = True
            continue
        if crole != "spin button":
            continue
        out = []
        for j in range(child.get_child_count()):
            sub = child.get_child_at_index(j)
            if sub is None:
                continue
            srole = sub.get_role_name() or ""
            sname = sub.get_name() or ""
            text = sname
            try:
                tiface = sub.get_text_iface()
                if tiface is not None:
                    text = tiface.get_text(0, -1) or sname
            except Exception:
                pass
            out.append("%s[%s]" % (srole, text))
        print("; ".join(out) if out else "no-children")
        sys.exit(0)
print("no-field")
PY
  }

  knock_rotate() {
    python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui

app = ae_ui._app()
if app is None:
    sys.exit(1)
for _depth, role, name, node in ae_ui._walk(app):
    if role != "label" or name != "Rotate":
        continue
    if not ae_ui.showing(node):
        continue
    parent = node.get_parent()
    if parent is None:
        continue
    for i in range(parent.get_child_count()):
        child = parent.get_child_at_index(i)
        if child is None:
            continue
        if (child.get_role_name() or "") != "push button":
            continue
        if not ae_ui.showing(child):
            continue
        if ae_ui.do(child, "click") or ae_ui.do(child, "press"):
            print("pressed")
            sys.exit(0)
sys.exit(1)
PY
  }

  # WAIT FOR THE PROJECT, NOT FOR A GUESSED NUMBER OF SECONDS. The main window - and the
  # menu wait_ready saw - exists before the project named on the command line finishes
  # loading, and everything below needs that project. The object tree is the signal: its
  # first row appears only once the project is in, and that row is also the one the first
  # selection below is made on. The object tree is the only "table cell" in the whole
  # accessibility tree, and its first row is the top layer.
  ROW=""
  for i in $(seq 1 60); do
      ROW=$(ae dump 2>/dev/null | sed -n "s/^ *table cell *| //p" | head -1)
      [ -n "$ROW" ] && break
      sleep 1
  done

  # ---- THE BOUNDS, ASKED FOR BY NAME (see the header for why not the tab strip) ----
  # Each rectangle is written as an assignment line so the outer shell can eval it back
  # into one "x y w h" value. The frame is asked for by ROLE as well as name: the
  # application node carries the same name, and the frame is the window.
  FRAME=$(ae extents "AnimeEffects" "frame")
  PROP=$(ae extents "Properties")
  ANIM=$(ae extents "Animation")
  [ -n "$FRAME" ] || echo "NOGEOM frame"      >> $G
  [ -n "$PROP" ]  || echo "NOGEOM properties" >> $G
  [ -n "$ANIM" ]  || echo "NOGEOM animation"  >> $G
  echo "FRAME=\"$FRAME\"" >> $G
  echo "PROP=\"$PROP\""   >> $G
  echo "ANIM=\"$ANIM\""   >> $G
  echo "ROW=\"$ROW\""     >> $G

  # ---- EMPTY STATE, BEFORE ANYTHING IS SELECTED ----
  # Nothing is selected at startup, so this is the one moment the panel can be asked for
  # its "no target" sentence: ObjectTreeWidget has no current row until one is clicked.
  park
  import -window root "$SHOTS/graph_probe_01_off_empty.png"

  ae_press "Show animation graph" "the graph toggle is pressable by name"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_02_on_empty.png"
  EMPTY_LINE=$(ae extents "Select a layer to show its curves" "label")
  NOKEYS_LINE=$(ae extents "No keys on the selected layers" "label")
  echo "EMPTY_LINE=\"$EMPTY_LINE\""   >> $G
  echo "NOKEYS_LINE=\"$NOKEYS_LINE\"" >> $G

  ae_press "Show animation graph" "the graph toggle is pressable a second time"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_03_off_empty.png"

  # ---- THE TOP ROW SELECTED: THE PLAIN PANEL, AND THE OFF/ON/OFF TRIPLE ----
  if [ -n "$ROW" ]; then
      ae_activate "$ROW" "the first object-tree row is selected ($ROW)" "table cell"
  else
      check 1 "the object tree answered with a first row to select (no table cell in ae dump after 60s)"
  fi
  park
  import -window root "$SHOTS/graph_probe_04_off_top.png"

  ae_press "Show animation graph" "the graph toggle is pressable a third time"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_05_on_top.png"
  echo "TOP_LINE=\"$(ae extents "No keys on the selected layers" "label")\"" >> $G

  ae_press "Show animation graph" "the graph toggle is pressable a fourth time"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_06_off_top.png"

  # ---- PHASE 5: THE WHEEL - THE GRAPHS WHILE IT IS VISIBLE, THE CANVASS WHEN IT IS NOT ----
  # WHAT THIS PROVES, AND WHY NOTHING ELSE CAN. ctrl::Driver::updateGraphWheel hands the
  # wheel to the graph whenever the panel is VISIBLE, unconditionally: a rect test against
  # the panel own rectangle (which is the whole viewport), not the hit test the cursor uses,
  # and it returns false while the panel is hidden so the canvas gets the wheel back
  # (Driver.cpp:215, and MainDisplayWidget::wheelEvent is the only route to either). That
  # routing exists only in a mapped window own QWheelEvent, so no unit test reaches it.
  #
  # WHAT IS ASSERTED, IN BOTH DIRECTIONS, AT ONE POINT INSIDE THE VIEWPORT:
  #   * the graph VISIBLE: the wheel moves the PANEL own own pixels (its frame ticks and
  #     their labels are re-laid-out for the new frames-per-pixel) and the CANVAS region
  #     is BYTE-IDENTICAL - the artwork did not move at all;
  #   * the graph HIDDEN: the same wheel moves the CANVAS region (the artwork scales),
  #     which is also the control that proves the region this phase calls the canvas
  #     really is canvas;
  #   * and the panel publishes nothing at all while it is hidden ("Fit graph" absent),
  #     which is what makes the second case about the canvas rather than about the panel.
  # THE TWO REGIONS ARE MEASURED, NOT ASSUMED, and the outer shell names them: the PANEL
  # region is the bounding box of the OFF->ON change (the panel own own ink, dilated by 2 px
  # so the outline pass own 1 px rings cannot straddle the edge) and the CANVAS region is
  # the crop minus that box. The crop itself is the viewport plus the Tools dock, so the
  # canvas region is the strip of the crop the panel never paints in - the region a canvas
  # zoom would move and nothing else could.
  #
  # THE POINT IS DERIVED FROM THE DOCKS: the viewport own centre, which is right of the
  # Tools dock, left of the Properties dock, below the menu bar own row and above the
  # Animation dock - the same three rectangles the crop comes from, so a layout change
  # moves the point with them rather than leaving it on a dock.
  WHEEL_TOOLS=$(ae extents "Tools")
  read -r WTX WTY WTW WTH <<< "${WHEEL_TOOLS:-0 0 0 0}"
  read -r WPX WPY WPW WPH <<< "${PROP:-0 0 0 0}"
  read -r WAX WAY WAW WAH <<< "${ANIM:-0 0 0 0}"
  WHEEL_X=$(( (WTX + WTW + WPX) / 2 ))
  WHEEL_Y=$(( (WPY + WAY) / 2 ))
  # The viewport rectangle in screen coordinates: left of the Properties dock, right of the
  # Tools dock, under the menu bar row, above the Animation dock. It is the region the RING
  # capture below is cropped to; the key menu's popover is NOT checked against it any more (it
  # is a top-level window, and popUpAt clamps it into the SCREEN, not into the viewport).
  VIEW_L=$((WTX + WTW)); VIEW_T=$WPY; VIEW_R=$WPX; VIEW_B=$WAY
  WHEEL_NOTCHES=3
  echo "WHEEL_TOOLS=\"$WHEEL_TOOLS\""        >> $G
  echo "WHEEL_POINT=\"$WHEEL_X $WHEEL_Y\""   >> $G
  echo "WHEEL_NOTCHES=\"$WHEEL_NOTCHES\""    >> $G
  echo "VIEW_L=\"$VIEW_L\""                   >> $G
  echo "VIEW_T=\"$VIEW_T\""                   >> $G
  echo "VIEW_R=\"$VIEW_R\""                   >> $G
  echo "VIEW_B=\"$VIEW_B\""                   >> $G

  # One wheel gesture: the pointer at the viewport own centre, then the notches. IN BOTH
  # DIRECTIONS the paint is the application own now: the graph own zoom is a PANEL state,
  # and MainDisplayWidget::wheelEvent asks for the widget own frame when the graph took the
  # event, exactly as the cursor path does - while the canvas mover own path calls
  # updateRender itself, which is why the artwork own zoom always painted. The sleeps are
  # only settle time for the frame that was asked for; nothing here forces a paint.
  wheel_here() { # wheel_here <capture path> <direction: up|down>
      xdotool mousemove --sync $WHEEL_X $WHEEL_Y
      sleep 0.4
      if [ "$2" = "up" ]; then xdotool click --repeat $WHEEL_NOTCHES --delay 120 4
      else                    xdotool click --repeat $WHEEL_NOTCHES --delay 120 5
      fi
      sleep 0.6
      park
      sleep 1.0
      import -window root "$1"
  }

  # UNDO THAT ZOOM, so every phase AFTER this one works on the window the earlier phases
  # recorded (the wheel is the only thing in the probe that writes the frame window, and
  # the key phases that follow add keys at frames the pre-wheel window is looking at).
  # Not asserted - it is hygiene, not evidence - and the notches are the same count in the
  # opposite direction at the same anchor, which is what the scaler own own per-notch factor
  # makes reversible. The undo paints itself the same way the wheel above does.
  wheel_restore() {
      xdotool mousemove --sync $WHEEL_X $WHEEL_Y
      sleep 0.3
      xdotool click --repeat $WHEEL_NOTCHES --delay 120 5
      sleep 0.6
      park
      sleep 0.4
  }

  # (i) THE GRAPH VISIBLE. Two captures of the same state first, so the pair below can
  # only be the wheel. The zoom goes IN: the frame ladder own rung is at its floor for a
  # project nobody has fitted yet, so zooming OUT would be a request the scaler may clamp
  # while zooming in cannot be.
  ae_press "Show animation graph" "the graph toggle is pressable (the wheel phase, on)"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_20_wheel_on_a.png"
  import -window root "$SHOTS/graph_probe_21_wheel_on_b.png"
  wheel_here "$SHOTS/graph_probe_22_wheel_on_after.png" up
  wheel_restore

  # (ii) THE GRAPH HIDDEN: same point, same notches, the other owner.
  ae_press "Show animation graph" "the graph toggle is pressable (the wheel phase, off)"
  sleep 1.2; park
  import -window root "$SHOTS/graph_probe_23_wheel_off_a.png"
  import -window root "$SHOTS/graph_probe_24_wheel_off_b.png"
  WHEEL_HIDDEN_FIT=$(ae extents "Fit graph")
  echo "WHEEL_HIDDEN_FIT=\"$WHEEL_HIDDEN_FIT\"" >> $G
  wheel_here "$SHOTS/graph_probe_25_wheel_off_after.png" up
  wheel_restore

  # ---- (iii) THE RING OVER BRIGHT ARTWORK: ONE CAPTURE FOR THE EYE, NOT FOR THE GATE ----
  # WHAT THIS IS, AND WHY IT ASSERTS NOTHING. The panel draws its ink in white with a 1px
  # dark outline ring (the contrast-outline pass whose re-measured pixel counts the header
  # records). On this project the ring mostly has nothing to do: the ink of the panel falls
  # on the dark ground the character stands on. So this takes ONE capture with the ink where
  # the ring matters - over BRIGHT artwork - and it is a capture to LOOK AT, not a gate:
  # whether a ring reads at a given zoom is a judgement, and any threshold for it would have
  # to be re-measured per renderer (see the llvmpipe note in the header). What is asserted
  # about it below is only that the file exists and is not a black frame.
  #
  # THE RECIPE, every step of it a measurement rather than a coordinate:
  #   * the graph is HIDDEN here - the canvas case of phase 5 just left it that way - so the
  #     wheel is the CANVAS own, at the same point and in the same notches phase 5 measured
  #     in both directions;
  #   * the anchor is the BRIGHTEST AREA the artwork has, found by blurring the canvas region
  #     of a capture taken with the graph hidden (so what is measured is the artwork and not
  #     the panel) and asking ImageMagick where its maximum is. The blur is what makes it an
  #     area rather than a 1px highlight. The crop is INSET by 20 px on every side, for two
  #     measured reasons: a blurred window must not run off the edge of the region being
  #     measured, and the project tab bar is drawn over the first 18 px of the viewport (the
  #     panel own ink begins at y=39, i.e. 18 px below the dock top at 21) - so an un-inset
  #     crop would find the brightest thing in the frame to be that bar and not the artwork;
  #   * the wheel zooms IN at that anchor - button 5, because MEASURED the other way: the own
  #     canvas capture of phase 5 shows the artwork SHRINK under button 4 - by 12 notches of
  #     about 10% each (ctrl::CanvasMover keys the zoom on an integer notch index, one notch
  #     per 120 units of wheel delta, so the factor is 1.1 per notch in and 0.9 out), which is
  #     what puts a bright area larger than the viewport under the panel rather than a patch
  #     inside it;
  #   * the graph is then shown again through its own toggle, so the labels, the tick row, the
  #     zero rule and the channel button sit over that bright artwork: that frame is the capture;
  #   * and the canvas zoom is UNDONE with the same notches at the same anchor. The round trip
  #     is exact rather than approximate - CanvasMover re-centres about the pointer
  #     ((scale/pre)*(center - cursor) + cursor, whose inverse is itself, and the notch index
  #     is an integer) - which is why every phase after this one still sees the window phase 5
  #     restored.
  #
  # THE ONE NUMBER BESIDE IT IS RECORDED, NOT ASSERTED: the mean luminance of the artwork
  # under the ink box of the panel, where that box is measured the same way the header
  # measures it - the bounding box of the graph-off -> graph-on change. A reader can then
  # tell a capture that had something to show from one that did not.
  RING_SHOT="$SHOTS/graph_probe_31_ring_over_art.png"
  RING_NOTCHES=12
  RING_INSET=20
  RING_BRIGHT=0
  RING_INK_BOX=""
  RING_POINT=""
  import -window root /tmp/probe_ring_art.png
  RING_MAX=$(magick /tmp/probe_ring_art.png \
      -crop $((VIEW_R - VIEW_L - 2 * RING_INSET))x$((VIEW_B - VIEW_T - 2 * RING_INSET))\
+$((VIEW_L + RING_INSET))+$((VIEW_T + RING_INSET)) +repage \
      -colorspace Gray -blur 0x10 /tmp/probe_ring_root.png 2>/dev/null; \
      magick identify -define identify:locate=maximum -define identify:limit=1 \
      /tmp/probe_ring_root.png 2>/dev/null | awk "/:/ {print \$NF}" | tail -1)
  read -r RING_MX RING_MY <<< "$(printf "%s" "${RING_MAX:-}" | tr "," " ")"
  if [ -n "${RING_MX:-}" ] && [ -n "${RING_MY:-}" ]; then
      RING_X=$((RING_MX + VIEW_L + RING_INSET))
      RING_Y=$((RING_MY + VIEW_T + RING_INSET))
      RING_POINT="$RING_X $RING_Y"
      # THE ZOOM, while the graph is hidden: the face of the artwork this project ships is
      # what the measurement above finds, and 12 notches put its bright area under the whole
      # panel.
      xdotool mousemove --sync $RING_X $RING_Y
      sleep 0.4
      xdotool click --repeat $RING_NOTCHES --delay 120 5
      sleep 1.0
      park
      import -window root /tmp/probe_ring_art_zoomed.png
      # THE FRAME TO LOOK AT: the same toggle press every capture above is taken behind.
      ae_press "Show animation graph" "the graph toggle is pressable (the ring capture, on)"
      sleep 1.4; park
      import -window root "$RING_SHOT"
      # THE ARTWORK THE INK SITS ON, measured in the box that ink occupies. The box is the
      # graph-off -> graph-on change INSIDE THE VIEWPORT - the crop keeps the Tools dock out
      # of it, because the toggle own checked highlight changes there and is not panel ink -
      # and the mean is then taken from the ZOOMED capture (which has no panel in it at all),
      # so it is the brightness of the artwork itself.
      magick /tmp/probe_ring_art_zoomed.png -crop $((VIEW_R - VIEW_L))x$((VIEW_B - VIEW_T))\
+${VIEW_L}+${VIEW_T} +repage /tmp/probe_ring_off.png 2>/dev/null
      magick "$RING_SHOT" -crop $((VIEW_R - VIEW_L))x$((VIEW_B - VIEW_T))\
+${VIEW_L}+${VIEW_T} +repage /tmp/probe_ring_on.png 2>/dev/null
      magick compare -compose src /tmp/probe_ring_off.png /tmp/probe_ring_on.png /tmp/probe_ring_ink.png 2>/dev/null
      RING_INK_BOX=$(magick identify -format "%@" /tmp/probe_ring_ink.png 2>/dev/null)
      read -r RIBW RIBH RIBX RIBY <<< "$(printf "%s" "${RING_INK_BOX:-0x0+0+0}" | tr "x+" "  ")"
      if [ "${RIBW:-0}" -gt 0 ] && [ "${RIBH:-0}" -gt 0 ]; then
          RING_BRIGHT=$(magick /tmp/probe_ring_art_zoomed.png \
              -crop "${RIBW}x${RIBH}+$((RIBX + VIEW_L))+$((RIBY + VIEW_T))" +repage -colorspace Gray \
              -format "%[fx:mean*255]" info: 2>/dev/null)
      fi
      # UNDO, with the same notches at the same anchor (see the note above): the graph first,
      # because with the panel up the wheel belongs to the PANEL and would write the frame
      # window.
      ae_press "Show animation graph" "the graph toggle is pressable (the ring capture, off)"
      sleep 1.0; park
      xdotool mousemove --sync $RING_X $RING_Y
      sleep 0.4
      xdotool click --repeat $RING_NOTCHES --delay 120 4
      sleep 1.0
      park
  fi
  echo "RING_POINT=\"$RING_POINT\""     >> $G
  echo "RING_NOTCHES=\"$RING_NOTCHES\"" >> $G
  echo "RING_INK_BOX=\"$RING_INK_BOX\"" >> $G
  echo "RING_BRIGHT=\"$RING_BRIGHT\""   >> $G
  echo "ring capture: anchor ${RING_POINT:-none}, $RING_NOTCHES notches of canvas zoom in and the same out; the ink box of the panel over the zoomed artwork ${RING_INK_BOX:-none}, its mean luminance ${RING_BRIGHT:-?} (recorded, not asserted)"


  # ---- PHASE 2: DOES THE DATA OF THE SELECTED LAYER REACH THAT PANEL? ----
  # Everything above measures the PANEL. It draws frame ticks, the value axis and the
  # state line whether or not the layer behind it has a single key - measured on this
  # machine: the projects this probe defaults to (the smallest .anie under ~/Downloads)
  # expose one top node whose channel maps are EMPTY, so a screenshot of it is chrome and
  # nothing else. That would let a broken target feed - the panel drawing no curve for any
  # selection, ever - pass every assertion above.
  #
  # So swing the data instead of hunting for a project that has it: the Properties
  # dock Current Keys panel offers one add-key knob per channel, and pressing it puts
  # an EXPLICIT key at the current frame. If the key reaches the panel the plot
  # repaints: the fit basis gains a channel and the curve appears. Four measured facts
  # stand between this probe and that press, and every one of them is asserted rather
  # than hoped for:
  #
  #   * the knob is NOT the check box named "Create rotate key". That one is the
  #     Transform tool option (gui::tool::SRTPanel), inside the Tools dock Transform
  #     panel, and it is not even showing while the Toolbox is the active tool panel
  #     (its a11y rect is 0x0 and its role is check box, ancestor panel->Transform).
  #     The knob is a prop::KeyKnocker row: a label named after the channel (Rotate)
  #     plus an UNNAMED plus button, and it exists only while the frame has no key for
  #     that channel. Pressing it is a click anywhere on the row, because the label
  #     forwards its click to the row.
  #   * the whole Current Keys panel is hidden while the selection is the tree TOP
  #     node: ObjectTreeWidget::findSelectingRepresentNode returns nullptr for it, the
  #     dock calls setTarget(nullptr) and every panel hides. So the probe selects a
  #     CHILD row first (the branch indicator is the only expander, 12px left of the
  #     row own cell rect).
  #   * the board is taller than the dock viewport, so the Current Keys rows sit BELOW
  #     the fold: a click there hits the log pane and adds nothing (recorded when this
  #     probe was first written: the key count stayed 0), the wheel does not move the board
  #     (recorded the same way: 8 notches at the dock centre changed 0 px), but the panels
  #     above it collapse through their own title labels, and collapsing Constants and
  #     Default Keys lifts the row into the viewport. MEASURED IN THIS RUN, and asserted
  #     below rather than hoped for: the Rotate knob row is at y=394 after the row is
  #     selected and at y=194 after the two collapses - so the press that follows is a
  #     press on a row that MOVED into reach, not on whatever used to be at those
  #     coordinates.
  #   * a "Rotate" node that is not the knob is the View Settings strip toggle, whose
  #     accessible name is the group name and whose DESCRIPTION is "Rotate": pressing
  #     it activates a tool and changes the canvas, which is why no assertion here is
  #     made against a whole-screen change alone.
  #
  # The control matters as much as the effect: the panel is captured TWICE after the
  # row is selected and the panels above are collapsed, and before the key is added.
  # Those two shots are the same state, so they must be identical - the change below
  # can then only be the key.
  ae_press "Show animation graph" "the graph toggle is pressable a fifth time (back on)"
  sleep 1.0; park

  # A CHILD ROW, because the top node hides every panel of the dock.
  FIRST=$(ae dump 2>/dev/null | sed -n "s/^ *table cell *| //p" | head -1)
  FRECT=$(ae extents "$FIRST" "table cell")
  if [ -n "$FRECT" ]; then
      read -r FX FY FW FH <<< "$FRECT"
      xdotool mousemove --sync $((FX - 12)) $((FY + FH / 2)) click 1
      sleep 1.5
  fi

  KEY_ROW=""
  KNOB_BEFORE=""
  while IFS= read -r cand; do
      [ -n "$cand" ] || continue
      ae click "$cand" "table cell" >/dev/null 2>&1
      park
      KNOB_BEFORE=$(ae extents "Rotate" "label")
      if [ -n "$KNOB_BEFORE" ]; then KEY_ROW="$cand"; break; fi
  done < <(ae dump 2>/dev/null | sed -n "s/^ *table cell *| //p" | head -8)

  # BEFORE THE PRESS, and this is the pair of states the press has to move:
  #   * the knob ROW exists (the channel has no key at this frame);
  #   * the channel ROWS do not - the Angle label is part of the group the knob stands
  #     in for, and prop_CurrentKeyPanel::RotateKeyGroup::setKeyExists hides it while
  #     mKnocker is visible. MEASURED on the default project: Angle resolves to nothing
  #     here, and to a row at [382 245 82 23] after the press.
  ANGLE_BEFORE=$(ae extents "Angle" "label")
  ANGLE_VALUE_BEFORE=$(angle_value 2>/dev/null)
  # THE PANEL'S ONE CONTROL, BEFORE THE KEY. The list is published only while there is a
  # channel to list, and the model lists a channel only once it HAS a key - so before this
  # press there is no control at all. It is read by the name the button carries while every
  # channel is shown ("1 channel(s)"), which is also what the read after the press compares
  # against; a check of BOTH names is made after the deletion in phase 3, where a channel
  # that existed but were hidden would answer to the other one.
  CHANNELS_BEFORE=$(ae extents "1 channel(s)" "push button")

  # AND INTO THE VIEWPORT, through the title labels of the panels above it.
  for title in "Constants" "Default Keys"; do
      TRECT=$(ae extents "$title" "label")
      [ -n "$TRECT" ] || continue
      read -r CX CY CW CH <<< "$TRECT"
      xdotool mousemove --sync $((CX + CW / 2)) $((CY + CH / 2)) click 1
      sleep 1.2
      park
  done
  KNOB=$(ae extents "Rotate" "label")
  ANIM=$(ae extents "Animation")
  echo "KEY_ROW=\"$KEY_ROW\""           >> $G
  echo "KNOB_BEFORE=\"$KNOB_BEFORE\""   >> $G
  echo "KNOB=\"$KNOB\""                 >> $G
  echo "ANGLE_BEFORE=\"$ANGLE_BEFORE\"" >> $G
  echo "ANGLE_VALUE_BEFORE=\"$ANGLE_VALUE_BEFORE\"" >> $G
  echo "CHANNELS_BEFORE=\"$CHANNELS_BEFORE\"" >> $G
  echo "ANIM=\"$ANIM\""                 >> $G

  # READ THE PANEL TWICE, WITH NOTHING IN BETWEEN: no toggle, no forced paint. The pair is
  # still expected to be exactly 0 px, which is the control that makes the press below the
  # only difference between it and the keyed capture.
  import -window root "$SHOTS/graph_probe_07_on_control.png"
  import -window root "$SHOTS/graph_probe_08_on_control.png"

  # THE PRESS: the centre of the row rect AT-SPI reports for the knob.
  if [ -n "$KNOB" ]; then
      read -r KX KY KW KH <<< "$KNOB"
      xdotool mousemove --sync $((KX + KW / 2)) $((KY + KH / 2)) click 1
      sleep 1.2; park
  fi
  # The key exists once the knob row has become the channel rows: the knob is the
  # stand-in for the panel header only while the frame has no key.
  ANGLE=$(ae extents "Angle" "label")
  ANGLE_VALUE=$(angle_value 2>/dev/null)
  # The capture right after the push: the key reaches the document through the timeline own
  # notifier (core::Project::onTimeLineModified), and the DriverHolder slot on that signal
  # asks the viewport for the frame (DriverHolder::onTimeKeyUpdated -> onVisualUpdated ->
  # MainDisplayWidget::onVisualUpdated), so the panel own pixels move without a nudge.
  import -window root "$SHOTS/graph_probe_09_after_key.png"
  echo "ANGLE=\"$ANGLE\""               >> $G
  echo "ANGLE_VALUE=\"$ANGLE_VALUE\""   >> $G
  CHANNELS_AFTER=$(ae extents "1 channel(s)" "push button")
  echo "CHANNELS_AFTER=\"$CHANNELS_AFTER\"" >> $G

  # ---- THE CHANNEL LIST, DRIVEN THROUGH THE PANEL'S ONE CONTROL ----
  # WHAT REPLACED THE LEGEND CHECK BOXES, AND WHY THE ROUTE IS THE POINTER AND THE WINDOW.
  # This block used to read one checkable row per channel out of the accessibility tree and
  # flip it through the AT-SPI Toggle action. The panel publishes ONE PushButton now, and the
  # list behind it is the application's own QMenu - a native window that is NOT in the
  # accessibility tree, exactly like the object tree's menu - so the list is driven the way
  # phase 3 drives that one: through its WINDOW and its own pixels. Every claim the old block
  # made is still made, on the surface that replaced it:
  #   * the row count    -> the BUTTON's own name, which IS the count the control changes
  #     ("1 channel(s)" while every channel is shown, "0 of 1" once one is hidden), plus the
  #     button's absence before any channel had a key;
  #   * the row's name   -> the panel's own line: hovering the list's channel row focuses
  #     that channel, and the model then says which one it is ("..., focused: <layer> :
  #     <channel>");
  #   * the toggle state -> the same button name plus the model's own all-hidden sentence,
  #     read either side of each activation;
  #   * the ink          -> the two captures below, unchanged: hiding removes the curve from
  #     the crop, and showing it again restores the crop exactly.
  CHAN_MENU=""
  CHAN_MENU_AFTER=""
  CHAN_ROWS=""
  CHAN_ROW=""
  CHAN_HOVER_LINE=""
  CHAN_BTN_HIDDEN=""
  CHAN_BTN_SHOWN=""
  if [ -n "$CHANNELS_AFTER" ]; then
      read -r BX BY BW BH <<< "$CHANNELS_AFTER"
      # THE PRESS IS THE CONTROL'S OWN ACTION (ImGuiOverlay::onAccessibilityAction ->
      # openChannelMenuAt), which is the very action a pointer press on the box performs, at
      # the same widget position - so the list is popped at the box's top-left, which is what
      # the window's own origin is asserted against outside.
      ae_activate "1 channel(s)" "the panel's channel-list button opens the list"
      sleep 1.0
      CHAN_MENU=$(qmenu_at "$BX" "$BY")
      read -r CHAN_MENU_ID CHAN_MENU_W CHAN_MENU_H CHAN_MENU_X CHAN_MENU_Y <<< "${CHAN_MENU:-0 0 0 0 0}"
      if [ -n "$CHAN_MENU_ID" ]; then
          # THE ROW IS FOUND, NOT ASSUMED. The list's first row is the layer HEADING, a
          # disabled action, so the walk starts at the top and takes the first hover that
          # lights a rectangle INSIDE the list's own window: that is the channel row, and
          # nothing about the style's row pitch is hardcoded. Every candidate is diffed
          # against a capture of the same list with the pointer parked, so what moved is the
          # row under the pointer.
          park
          import -window root /tmp/probe_chan_ctl.png
          for off in 6 14 22 30 38 46 54 62 70; do
              HY=$((CHAN_MENU_Y + off))
              [ "$HY" -le $((CHAN_MENU_Y + CHAN_MENU_H - 2)) ] || break
              xdotool mousemove --sync $((CHAN_MENU_X + CHAN_MENU_W / 2)) $HY
              sleep 0.7
              import -window root /tmp/probe_chan_hov.png
              magick compare -compose src /tmp/probe_chan_ctl.png /tmp/probe_chan_hov.png /tmp/probe_chan_diff.png 2>/dev/null
              ROW=$(row_rect /tmp/probe_chan_diff.png)
              read -r RX RY RW RH <<< "$ROW"
              CHAN_ROWS="${CHAN_ROWS}${CHAN_ROWS:+ }${off}=${ROW}"
              if [ -z "$CHAN_ROW" ] && [ "$RW" -gt 0 ] && [ "$RH" -gt 0 ] \
                 && [ "$RY" -ge "$CHAN_MENU_Y" ] && [ $((RY + RH)) -le $((CHAN_MENU_Y + CHAN_MENU_H)) ]; then
                  CHAN_ROW="$ROW"
              fi
          done
          # HOVERING THE ROW IS THE LIST'S OWN FOCUS GESTURE: the model then names that
          # channel in the panel's line, which is the readback for "focus changed what is
          # emphasised" (the panel draws the focused channel's handles; with the single key
          # this phase has there is no segment, so the line is the instrument that exists).
          CHAN_HOVER_LINE=$(panel_line)
          park
      fi
  fi

  # THE ACTIVATION: the row's centre, with the real pointer - which is what a user does with
  # the list. It toggles the channel's visibility, and the list closes on the activation.
  if [ -n "$CHAN_ROW" ]; then
      read -r RX RY RW RH <<< "$CHAN_ROW"
      xdotool mousemove --sync $((RX + RW / 2)) $((RY + RH / 2))
      sleep 0.5
      xdotool click 1
      sleep 1.2
      park
  fi
  import -window root "$SHOTS/graph_probe_10_legend_hidden.png"
  CHAN_BTN_HIDDEN=$(ae extents "0 of 1" "push button")
  ALLHIDDEN=$(ae extents "All channels of the selected layers are hidden" "label")

  # AND THE SAME ROW AGAIN, at a fresh opening of the list: the button's own name has moved
  # to the hidden count, so the press that reopens it is addressed by that name, and the
  # second opening's window is asserted to be the first one's place outside.
  if [ -n "$CHAN_BTN_HIDDEN" ] && [ -n "$CHAN_ROW" ]; then
      ae_activate "0 of 1" "the button, now counting the hidden channel, reopens the list"
      sleep 1.0
      CHAN_MENU_AFTER=$(qmenu_at "$BX" "$BY")
      xdotool mousemove --sync $((RX + RW / 2)) $((RY + RH / 2))
      sleep 0.5
      xdotool click 1
      sleep 1.2
      park
  fi
  import -window root "$SHOTS/graph_probe_11_legend_shown.png"
  CHAN_BTN_SHOWN=$(ae extents "1 channel(s)" "push button")
  ALLHIDDEN_BACK=$(ae extents "All channels of the selected layers are hidden" "label")
  echo "CHAN_MENU=\"$CHAN_MENU\""             >> $G
  echo "CHAN_MENU_AFTER=\"$CHAN_MENU_AFTER\"" >> $G
  echo "CHAN_ROWS=\"$CHAN_ROWS\""             >> $G
  echo "CHAN_ROW=\"$CHAN_ROW\""               >> $G
  echo "CHAN_HOVER_LINE=\"$CHAN_HOVER_LINE\"" >> $G
  echo "CHAN_BTN_HIDDEN=\"$CHAN_BTN_HIDDEN\"" >> $G
  echo "CHAN_BTN_SHOWN=\"$CHAN_BTN_SHOWN\""   >> $G
  echo "ALLHIDDEN=\"$ALLHIDDEN\""             >> $G
  echo "ALLHIDDEN_BACK=\"$ALLHIDDEN_BACK\""   >> $G

  # ---- PHASE 4: THE KEY CONTEXT MENU, WHICH IS THE EASING PICKER ----
  # WHAT THIS PROVES, AND WHY NOTHING ELSE CAN. The panel had no way to change the easing
  # TYPE of a key; a right-click on a key now hands the viewport the application own key-menu
  # entry point (ImGuiOverlay::openKeyMenuAt -> MainDisplayWidget::showGraphKeyMenu ->
  # TimeLineEditorWidget::showKeyContextMenu -> openEasingPicker), and that opens the ONE
  # gui::prop::EasingPickWidget popover AT THE POINTER - the SAME control the dock header own
  # easing chip opens from its own anchor (prop::EasingItem::openPicker), so one list of
  # easings and one edit path serve both views. The flat 14-row QMenu and its builder
  # (mSelectEasing) are DELETED with that change, and the pick's edit is still the timeline
  # own undoable one, committed as a SINGLE step when the popover closes
  # (EasingPickWidget::hideEvent emits applied() once). No headless test can see any of it.
  #
  # THE POPUP IS A WINDOW, NOT PIXELS IN THE PANEL, AND WHAT IDENTIFIES IT CHANGED WITH THE
  # CONTROL. A Qt popup is not drawn inside the application window, so it is not in the panel
  # own pixels at all: it is a top-level child of the ROOT window, and its rectangle comes
  # from the X server rather than from a capture. The phase used to find it the way AGENTS.md
  # finds a dialog - "the window that did not exist before", a set difference over the root
  # own children - which worked because every right-click built a FRESH QMenu. The picker is
  # ONE widget the dock header owns and reuses for every open (prop_Items.cpp: "The one place
  # the popover is opened"), so its native window survives a close and that set difference
  # only fires on the first open of a session. The witness is therefore WHAT THE WINDOW
  # PUBLISHES ABOUT ITSELF, which the phase reads from the X server with no window manager in
  # the way (xwininfo/xprop, the same tools phase 3 uses on the object tree own menu):
  #   * its own GEOMETRY, which is the picker own: kPopoverWidth wide and one of
  #     kNormalHeight/kCustomHeight tall. Both numbers are READ OUT OF THE PICKER OWN SOURCE
  #     (picker_const below), not hardcoded here, so a widget whose size changes is a red
  #     assertion naming the numbers rather than a scan that silently finds nothing. The
  #     easings this phase works with are not Custom, so the height expected is the normal one
  #     (300 x 202 on a DPR-1 X server; a scaled environment would show up as exactly that
  #     mismatch);
  #   * Map State: IsViewable while it is up, and NOT viewable once it is dismissed - which is
  #     how "the popup is gone" stays a fact about the window list and not a pixel bar;
  #   * Override Redirect State: yes - the X server own answer that this top-level is
  #     UNMANAGED, which is what a Qt::Popup is (Qt own xcb backend ORs
  #     X11BypassWindowManagerHint for Qt::Popup) and NOT what the application own normal
  #     window is. The WM window type is RECORDED instead of asserted: Qt own automatic
  #     _NET_WM_WINDOW_TYPE selection covers Tool, ToolTip, Dialog and Splash but not Popup, so
  #     the picker window carries no popup atom for a probe to accept on.
  # The origin is still the anchor: popUpAt moves the popup to (clamped anchor.x, anchor.y), so
  # the click point is the window own top-left, which is the same claim the QMenu phase made.
  #
  # THE TILES ARE THE ENTRY POINT, AND THE MODALITY PAIR STILL PINS THE POPUP. The picker is
  # a widget popover, not a QMenu: it has no rows, no highlight and no keyboard navigation, so
  # the three things this phase used to do to the menu - MAP ITS ROWS by hovering them,
  # ASSERT ITS HEIGHT holds exactly the 14 entries of util::Easing's display order minus
  # Custom, and REACH AN ENTRY with Down/Down/Return - cannot hold here, and are dropped with
  # the control (the phase own echo and the assertions say so rather than leaving a reader to
  # wonder). What the picker offers is its TILE CATALOG: kTileOrder in EasingPickWidget.cpp
  # names one easing per tile in display order with the vsep after tile 3, each a checkable
  # QToolButton. The phase PRESSES A TILE with the REAL POINTER, at the position the picker
  # own layout constants put it at: root margin, fixed header, the body own top and left
  # margins, the seg row, the motion-preview bar and the body gap above the tile row, then one
  # kTile per entry kTileGap apart (picker_tile_mid below, all of it read from the source).
  # It does NOT address the tiles by NAME, and says so rather than implying it: a QToolButton
  # whose text is empty has an empty accessible name, so the name route does not exist for
  # them - the values are read back from the panel own state line instead, exactly as for the
  # menu.
  #
  # AND A PICK COMMITS ON CLOSE, WHICH IS WHY EVERY PICK HERE IS A PRESS PLUS A DISMISSAL. A
  # tile edits a DRAFT; the model is written once when the popover goes away
  # (EasingPickWidget::hideEvent -> applied(), one undo step for the whole session). The
  # dismissal this phase uses is Escape, which Qt itself makes the popup own cancel key
  # (QWidget::keyPressEvent closes a Qt::Popup on QKeySequence::Cancel and the picker does not
  # override it, so the key reaches it even from a focused tile - an ignored key event
  # propagates to the parent). An outside press would do as well (QWidget::mousePressEvent
  # closes a Qt::Popup pressed outside its rect, which prop_Items.cpp records: "the popup is a
  # Qt::Popup, so an outside click hides it on its own"); Escape is used because it is the key
  # this phase already drove at the control it replaces.
  #
  # THE POPUP IS MODAL, WHICH IS THE APPLICATION OWN BEHAVIOUR AND NOT THE PANEL OWN. With the
  # popover OPEN the application own Delete and Undo key commands do NOT reach the timeline -
  # Delete is not the popup own cancel key, so QWidget::keyPressEvent ignores it and a
  # top-level has no parent to propagate to, and ctrl+Z is consumed by the picker own session
  # undo filter (EasingPickWidget::eventFilter) - and with it CLOSED they do: ctrl+Z with no
  # popover up moves the line back to the easing the last pick had replaced. Those two halves
  # replace the ImGui keyboard-capture pair this phase used to assert, which the ImGui popup
  # and its ImGuiConfigFlags_NavEnableKeyboard toggle no longer exist to make. The popup own
  # existence is read from the window list each time, never from a pixel bar.
  #
  # THE ONE LEFT PRESS THE PHASE MAKES IS THE EDITOR OWN, AND ITS READING IS MADE EMPTY
  # FIRST. A plain press on a key SELECTS it (ctrl::GraphEditor::ownsCursor claims it, so no
  # viewport tool sees it) and commits nothing, and the press is accepted only if the panel
  # own selected-key half MOVED from the reading taken immediately before it. That reading
  # has to start EMPTY, and the phase has one honest way there: THE GRAPH OWN DESELECT
  # GESTURE, a Ctrl-press in the plot on no key and no band, which reaches
  # GraphEditor::beginMarquee and applies a rectangle that selects nothing, changing no
  # document byte. The phase clears the selection with it TWICE - once before the scan, so
  # that the right-click which opens the popover can be shown to have SELECTED the key
  # (ImGuiOverlay::openKeyMenuAt calls editor.selectKey before it hands the selection on, and
  # that is a REAL selection, so the popover own close does not end it), and once
  # before the left press - and asserts the empty reading each time, so each "it moved"
  # statement is about the gesture that follows it.
  # AND THE SECOND PAIR RUNS LAST, AFTER PHASE 4b, because of a MEASURED SIDE EFFECT of that
  # gesture: it leaves the dock own Angle spin box unable to accept typed text for the rest of
  # the session (the measurement is beside the block itself, at the end of 4b). Phase 4b own
  # typed write is the instrument that exposes it, so the write is made first; nothing the
  # block asserts depends on the order.
  #
  # HOW THE CHANGE IS READ BACK, AND WHY THE PIXELS ALONE ARE NOT ENOUGH HERE.
  #   * the curve PIXELS cannot show it AT THIS POINT: this layer has ONE key, and
  #     graph::Curve::buildPolyline emits nothing for a map of one key (there is no segment),
  #     so the panel draws a key square and no curve at all - MEASURED, changing the easing
  #     on that state changes 0 pixels. Worse, the drawn summary line the state sentence
  #     comes from is NOT drawn while a curve exists (ImGuiOverlay.cpp:843 paints it only
  #     for an empty model), so at this point the pick own effect is invisible on screen
  #     altogether. Phase 4b below is where a curve is MADE so it is not: a second key one
  #     frame later carrying a different value, after which the segment is drawn and
  #     re-shaping it moves the curve own own pixels. (The field that gives a second key
  #     another value is the dock own unnamed Angle spin box: this probe reads it through its
  #     Value interface, and phase 4b writes it with a real edit - see angle_value and
  #     angle_rect above.)
  #   * the panel own STATE LINE is therefore the readback HERE: gui::ImGuiOverlay publishes
  #     the panel summary to the accessibility tree, and it names the selected key and the
  #     easing it carries ("... selected key: Rotate Linear"). The easing name comes from
  #     util::Easing::getTypeName - the same type names the picker own tile catalog is built
  #     from - and the line is built from the DOCUMENT (the key the selection names), not
  #     from the picker return value, so "the line changed" is a statement about the document.
  #     The tile cannot be the readback instead: its state is a checked flag on a QToolButton
  #     with no text and therefore no accessible name, and the widget writes the model only on
  #     close - which is exactly why the line exists. The PRESS own own pixels are asserted
  #     beside it, inside the popup own rectangle (the second pick below).
  #   * the dock Easing chip is read beside it as a DIAGNOSTIC, not an assertion: it is
  #     prop::EasingChip, a self-painting QAbstractButton with no text and no accessibleName,
  #     so there may be no easing name in the tree to read. The dump below records what the
  #     tree actually says about it, so the next reader does not have to re-measure.
  #
  # AND THE NEGATIVE: a right-click on empty plot space opens NO POPOVER at all - no viewable
  # picker window appears, and the panel own state line is untouched by it. Both halves are
  # asserted, and they are what make the scan own acceptance a fact about the key rather than
  # about right-clicks in the plot in general.
  # THE PANEL OWN STATE LINE, READ IN FULL - AND WHY `ae dump` CANNOT BE USED FOR IT.
  # `ae dump` prints "<role> | <name>" with the NAME CUT AT 70 CHARACTERS (ae_ui.py), and
  # MEASURED on this build the line grows past that as soon as the panel also names the
  # focused channel: "1 channel(s) shown, focused: FIGURE : Rotate, selected key: Rotate Sine"
  # is 78 characters, so the easing at its END - the very thing this phase reads back - comes
  # back as "Sin", and a comparison against the easing's own name can never hold. That is
  # exactly what happened in the run this phase was re-derived from: the pointer pick and the
  # keyboard pick both WORKED and both assertions failed on the cut. This helper walks the
  # accessibility tree itself - the same walk angle_value() and dock_focus() use -
  # and prints the WHOLE name of the first label carrying one of the model own four sentences
  # (the same strings the empty-state assertions above address by name, and the same rule the
  # old grep applied, now without the cut).
  panel_full() {
      python3 - <<PY
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui

MARKERS = ("channel(s) shown",
           "Select a layer to show its curves",
           "No keys on the selected layers",
           "All channels of the selected layers are hidden")
app = ae_ui._app()
if app is None:
    sys.exit(1)
for _depth, role, name, node in ae_ui._walk(app):
    if role != "label":
        continue
    for marker in MARKERS:
        if marker in name:
            print(name)
            sys.exit(0)
sys.exit(1)
PY
  }
  # The panel own line as summary_line() means it: only a line that CARRIES the selected-key
  # half.
  summary_line() { panel_full | grep "selected key" | head -1; }
  # ... and the line whether or not it carries it. summary_line() can only see a line that
  # HAS the half - which is exactly the reading the press below has to be able to move FROM an
  # empty one - so the half is read from the line itself, and the base sentences are the model
  # own four (the GraphPanel model own emptyState and the three counted ones).
  panel_line() { panel_full | head -1; }
  # The selected-key half of that line, and EMPTY when the line has none at all (which is
  # the reading the strengthened pre-condition below needs).
  selected_half() {
      local line
      line=$(panel_line)
      case "$line" in
          *"selected key:"*) printf "%s" "${line##*selected key: }" ;;
          *)                 printf "" ;;
      esac
  }
  # ---- THE POPUP OWN WINDOW, MEASURED AND NOT ASSUMED ----
  # Every child of the ROOT window, by id: the set a popup own opening and closing has to
  # change, and the list every witness below is a query over (see the phase comment for why
  # "the window that did not exist before" is no longer the witness).
  top_windows() { xwininfo -root -children 2>/dev/null | sed -n "s/^ *\(0x[0-9a-f]*\) .*/\1/p" | sort; }
  # A window own rectangle, from the X server own own numbers rather than from any capture:
  # "w h x y" (empty when there is no such window).
  win_geo() {
      xwininfo -root -children 2>/dev/null \
        | awk -v id="${1:-}" '$1 == id { for (i = 2; i <= NF; i++) \
              if ($i ~ /^[0-9]+x[0-9]+\+/) { gsub(/x|\+/, " ", $i); print $i; exit } }'
  }
  # WHAT THE WINDOW ITSELF REPORTS when asked about it: the map state and override-redirect
  # state the phase asserts on, and the width/height it checks against the picker own source
  # constants. One xwininfo call, so the answer is one reading and not four.
  win_info() { # win_info <window-id> -> "Map State: ...;Override Redirect State: ...;Width: ...;Height: ..."
      xwininfo -id "${1:-}" 2>/dev/null \
        | grep -E "Map State|Override Redirect State|Width:|Height:" | tr -s " " | tr "\n" ";"
  }
  # The WM window type, RECORDED rather than asserted (see the phase comment: Qt own automatic
  # selection does not give a Qt::Popup a popup atom).
  win_type() { # win_type <window-id> -> the atoms of _NET_WM_WINDOW_TYPE (empty when absent)
      xprop -id "${1:-}" _NET_WM_WINDOW_TYPE 2>/dev/null \
        | sed -n "s/^_NET_WM_WINDOW_TYPE(ATOM) = //p" | head -1
  }
  # ---- THE PICKER OWN NUMBERS, READ OUT OF ITS SOURCE (the probe own convention) ----
  # picker_const reads one integer constant out of the picker own source: the phase asserts
  # the WINDOW the application maps against the width and heights the widget pins itself to,
  # and it computes where a tile is from the layout constants, rather than copying numbers
  # that a change to the widget would quietly outdate (which is what "92x294" was).
  picker_const() { # picker_const <file> <name> -> the constant's integer value (empty if absent)
      sed -n "s/.*[^A-Za-z0-9_]${2:-} *= *\([0-9][0-9]*\)[^0-9].*/\1/p" "${1:-}" | head -1
  }
  PICKER_CPP="$ROOT/src/gui/prop/EasingPickWidget.cpp"
  PICKER_HDR="$ROOT/src/gui/prop/EasingPickWidget.h"
  PICKER_W=$(picker_const "$PICKER_CPP" kPopoverWidth)
  PICKER_H_NORMAL=$(picker_const "$PICKER_HDR" kNormalHeight)
  PICKER_H_CUSTOM=$(picker_const "$PICKER_HDR" kCustomHeight)
  PICKER_ROOTM=$(picker_const "$PICKER_CPP" kRootMargin)
  PICKER_HEADER=$(picker_const "$PICKER_CPP" kHeaderH)
  PICKER_SEG_PAD=$(picker_const "$PICKER_CPP" kSegPad)
  PICKER_SEG_BTN=$(picker_const "$PICKER_CPP" kSegBtnH)
  PICKER_PIPE=$(picker_const "$PICKER_CPP" kPipelineH)
  PICKER_BODY_TOP=$(picker_const "$PICKER_CPP" kBodyMarginTop)
  PICKER_BODY_LR=$(picker_const "$PICKER_CPP" kBodyMarginLr)
  PICKER_BODY_GAP=$(picker_const "$PICKER_CPP" kBodyGapSegPipe)
  PICKER_TILE=$(picker_const "$PICKER_CPP" kTile)
  PICKER_TILE_GAP=$(picker_const "$PICKER_CPP" kTileGap)
  PICKER_VSEP=$(picker_const "$PICKER_CPP" kVsepW)
  # The tile catalog, in the picker own display order. The phase names an ENTRY by its type
  # and derives the tile it sits on from this list, so a re-ordering of the catalog is read
  # rather than assumed; the vsep the widget inserts after tile 3 is not an entry.
  picker_tile_types() {
      sed -n "/kTileOrder\[\] *= *{/,/};/p" "$PICKER_CPP" \
        | tr -d "\n" | sed -n "s/.*{\(.*\)};.*/\1/p" | tr "," "\n" \
        | sed -n "s/.*util::Easing::\(Type_[A-Za-z]*\).*/\1/p"
  }
  picker_tile_idx() { # picker_tile_idx <Name> -> Type_<Name>'s 0-based tile index (empty if absent)
      picker_tile_types | grep -nx "Type_${1:-}" | head -1 | cut -d: -f1 \
        | awk "{ if (\$1 != \"\") print \$1 - 1 }"
  }
  PICKER_TILES=$(picker_tile_types | grep -c .)
  # WHICH WINDOWS ARE THE PICKER: exactly the ones that answer with the picker own geometry and
  # are ON SCREEN. Applied by the scan below and by every later opening/dismissal, so the two
  # can never disagree. It is deliberately not "a new window" (the picker own window survives a
  # close) and not a pixel rule (a popup is a top-level window, not pixels in the panel).
  picker_windows() {
      local id g w h info
      for id in $(top_windows); do
          g=$(win_geo "$id"); [ -n "$g" ] || continue
          read -r w h _ _ <<< "$g"
          [ "$w" -eq "${PICKER_W:-0}" ] || continue
          [ "$h" -eq "${PICKER_H_NORMAL:-0}" ] || [ "$h" -eq "${PICKER_H_CUSTOM:-0}" ] || continue
          info=$(win_info "$id")
          case "$info" in *"Map State: IsViewable"*) ;; *) continue ;; esac
          echo "$id"
      done
  }
  # WHERE A TILE IS INSIDE THE POPUP, from the picker own layout constants: the root margin,
  # the fixed header, the body own top and left margins, the seg row (its padding and the
  # button height), the motion-preview bar and the two body gaps down to the tile row, then
  # one kTile per entry kTileGap apart with the vsep inserted after tile 3
  # (EasingPickWidget::buildTrow). The caller adds the window own measured origin, so this is
  # a window-relative centre.
  picker_tile_mid() { # picker_tile_mid <index> -> "x y"
      local i=$1 n x y
      x=$((PICKER_ROOTM + PICKER_BODY_LR))
      y=$((PICKER_ROOTM + PICKER_HEADER + PICKER_BODY_TOP + PICKER_SEG_PAD + PICKER_SEG_BTN \
           + PICKER_SEG_PAD + PICKER_BODY_GAP + PICKER_PIPE + PICKER_BODY_GAP))
      for ((n = 0; n < i; n++)); do
          x=$((x + PICKER_TILE + PICKER_TILE_GAP))
          if [ "$n" -eq 3 ]; then x=$((x + PICKER_VSEP + PICKER_TILE_GAP)); fi
      done
      echo "$((x + PICKER_TILE / 2)) $((y + PICKER_TILE / 2))"
  }
  # PRESS A TILE with the real pointer, at the point the picker own constants give for that
  # index. Nothing is echoed; the caller reads the state line and the popup own window.
  press_picker_tile() { # press_picker_tile <index> <window-x> <window-y>
      local tx ty
      read -r tx ty <<< "$(picker_tile_mid "$1")"
      park
      xdotool mousemove --sync $(( $2 + tx )) $(( $3 + ty ))
      sleep 0.4
      xdotool click 1
      sleep 0.6
      park
  }
  # DISMISS the popover the way this phase does it everywhere: Escape, which Qt makes the
  # popup own cancel key (QWidget::keyPressEvent handles QKeySequence::Cancel for a Qt::Popup)
  # and which COMMITS the session's draft on close (EasingPickWidget::hideEvent -> applied(),
  # one undo step). Nothing is echoed: the caller reads the window list and the state line.
  popup_dismiss() {
      park
      xdotool key --clearmodifiers Escape
      sleep 1.2
  }
  # Whether the picker is STILL ON SCREEN: "yes" while a window with the picker own geometry is
  # viewable. This is how the phase reads "the popup is still open" and "the popup closed" -
  # the window own published state, not a pixel bar, and not the window own mere existence,
  # which survives a close.
  window_open() {
      if picker_windows | grep -qx "${1:-}"; then echo yes; else echo no; fi
  }

  MENU_FIT=""
  MENU_DX0=0
  MENU_DY0=0
  MENU_KEY=""
  MENU_OFFSET=""
  MENU_SAMPLES=0
  MENU_SCAN_LATTICE=""
  MENU_SCAN_NOTE=""
  MENU_SCAN_TRACE=""
  MENU_WIN=""
  MENU_GEOM=""
  MENU_PICKER_COUNT=0
  MENU_WIN_INFO=""
  MENU_WIN_TYPE=""
  MENU_PICK_W=""
  MENU_PICK_H=""
  MENU_PICK_H_CUSTOM=""
  MENU_TILES=""
  MENU_PICK_BASE=""
  MENU_TILE_ENTRY=""
  MENU_TILE_IDX=""
  # THE TILES THE TWO PICKS PRESS, by TYPE NAME: their indexes are derived from kTileOrder
  # above (picker_tile_idx), so a catalog change moves the press rather than leaving a stale
  # index behind, and the state line then has to name exactly these types.
  MENU_TILE_TYPE="Sine"      # the FIRST pick's tile (kTileOrder index 4)
  MENU_TILE_KB_TYPE="Round"  # the SECOND pick's tile (kTileOrder index 2)
  # The picker own numbers as THIS RUN read them, which is what the outer assertions quote
  # (and what they fail on when a source change outdates them).
  MENU_PICK_W="$PICKER_W"
  MENU_PICK_H="$PICKER_H_NORMAL"
  MENU_PICK_H_CUSTOM="$PICKER_H_CUSTOM"
  MENU_TILES="$PICKER_TILES"
  MENU_PRE_SCAN=""
  MENU_DESELECT=""
  MENU_STATE_BEFORE=""
  MENU_SEL_BEFORE=""
  MENU_SEL_AFTER=""
  MENU_SEL_BEFORE2=""
  MENU_SEL_AFTER2=""
  MENU_BEFORE=""
  MENU_AFTER=""
  MENU_DOCK_BEFORE=""
  MENU_DOCK_AFTER=""
  MENU_ENTRY=""
  MENU_ENTRY_PT=""
  MENU_ENTRY_TILE=""
  MENU_ENTRY_CLOSED=""
  MENU_DEL_LINE=""
  MENU_DEL_OPEN=""
  MENU_UNDO_LINE=""
  MENU_UNDO_OPEN=""
  MENU_ESCAPE_LINE=""
  MENU_ESC_OPEN=""
  MENU_GEOM2=""
  MENU_KB_GEOM=""
  MENU_KB_HL=""
  MENU_KB_HL_BB=""
  MENU_KB_ENTRY=""
  MENU_KB_TILE=""
  MENU_KB_LINE=""
  MENU_KB_CLOSED=""
  MENU_APPKEY_LINE=""
  MENU_EMPTY=""
  MENU_EMPTY_WINS=""
  MENU_NOKEY_LINE=""

  # The panel OWN fit, through the panel OWN action. Its PUBLISHED RECT IS THE PLOT: the
  # overlay publishes model.plot for the fit action (the same rectangle the state sentence
  # carries), because a double-click on the plot performs it. So asking the action where it
  # is IS asking the panel where its plot is - see the header note.
  ae_activate "Fit graph" "the panel own fit action is activated (its published rect is the plot)"
  sleep 1.0
  park
  MENU_FIT=$(ae extents "Fit graph")
  if [ -z "$MENU_FIT" ]; then
      echo "NOFIT the panel published no fit-action rectangle" >> $G
      kill $app 2>/dev/null
      wait $app 2>/dev/null
      exit 0
  fi
  read -r MFX MFY MFW MFH <<< "$MENU_FIT"
  MENU_DX0=$((MFX + MFW / 2))
  MENU_DY0=$((MFY + MFH / 2))
  # A DIAGNOSTIC, not an assertion: whether the panel already had a key selected when the
  # phase began. What makes the scan below decidable is the DESELECT that follows it, which
  # is read back too (MENU_STATE_BEFORE / MENU_SEL_BEFORE).
  MENU_PRE_SCAN=$(selected_half)

  # ---- THE DESELECT THAT MAKES THE NEXT READING DECIDABLE ----
  # WHY IT COMES BEFORE THE SCAN. The reading the right-click has to move is the panel own
  # selected-key half, and it has to start EMPTY for "it moved" to be a statement about the
  # right-click: the right-click that opens the menu SELECTS the key it opens on (the overlay
  # own openKeyMenuAt calls selectKey, because the menu acts on the selection, exactly as the
  # timeline own key menu does). The phase has one honest way to empty it - THE GRAPH OWN
  # DESELECT GESTURE: a Ctrl-press in the plot on no key and no band, which
  # GraphEditor::beginMarquee owns and which applies a rectangle that selects nothing, so it
  # changes no document byte. It is made at the plot own RIGHT-HAND END, which is the plot's
  # own free space on every state this probe reaches (the key is the only one, and the fit
  # puts it left of centre on this build - MEASURED at x=259..262 in a plot whose right edge
  # is 378, and the point below is the one a standalone measurement of this gesture used).
  # The reading is asserted, not trusted: a gesture that failed to deselect is a red
  # assertion rather than a licence for the statement below.
  MENU_DESELECT_X=$((MFX + MFW - 20))
  MENU_DESELECT="$MENU_DESELECT_X $MENU_DY0"
  xdotool keydown --clearmodifiers ctrl
  sleep 0.2
  xdotool mousemove --sync $MENU_DESELECT_X $MENU_DY0
  sleep 0.4
  xdotool click 1
  sleep 0.2
  xdotool keyup --clearmodifiers ctrl
  sleep 1.2
  park
  MENU_STATE_BEFORE=$(panel_line)
  MENU_SEL_BEFORE=$(selected_half)

  # ---- FIND THE KEY BY THE POPUP ITS RIGHT-CLICK PUTS ON SCREEN ----
  # WHAT WAS WRONG BEFORE, MEASURED. This walk used to accept a point when the pixels inside
  # the application window changed like a menu, and it had to be re-tuned twice for a popup
  # that was drawn by ImGui in the widget own GL frame; the phase then accepted "a window that
  # did not exist before". The popover is a top-level window of its own, so a window is still
  # the witness - but NOT the fact that it is new: the picker is one widget the dock header
  # owns and reuses, so its native window outlives a close and only the session's FIRST open
  # is a new child of the root (see the phase comment). The witness is the window PUBLISHING
  # ITSELF as the picker - the width and heights read out of its source, and a viewable map
  # state - and picker_windows is the single rule both this scan and every later opening use.
  # Every picker window is closed before the next sample is read, so a sample is still
  # attributable to its own right-click: the walk asserts that nothing answered BEFORE the
  # click (MENU_PICK_BASE) as well as after it. A right-click that hits no key opens nothing
  # at all (the graph takes no right-button action there, so a miss is inert).
  # AND THE POINT IS FOUND RATHER THAN PREDICTED, because the phase own business is the DRAWN
  # key and not the fit policy. The right-click is answered by
  # ImGuiOverlay::openKeyMenuAt, which takes the nearest key whose centre is within
  # ctrl::graph::kKeyHitRadius = 10 - a 20 px band around the key own centre - so a 6 px
  # lattice cannot straddle it wherever the fit puts the key. The walk is the fit rect own
  # centre ROW first (the value axis is fitted, so a single key is vertically centred), then
  # 8 and 4 px either side of it, which covers a value window that does not centre the key.
  # RIGHT-CLICKS, NOT LEFT PRESSES, because a press the graph does not own is handed to the
  # viewport tools (a PLAIN press in the plot is deliberately left to them), while a
  # right-click that hits no key is inert. The ONE left press this phase makes follows the
  # scan, and the phase says there why it is the only one it can make.
  # EVERY SAMPLE IS WRITTEN TO THE GEOMETRY FILE as point=picker-window, so a scan that finds
  # nothing says what it looked at rather than leaving an empty MENU_KEY to fail elsewhere.
  MENU_SAMPLES=0
  MENU_SCAN_LATTICE="x ${MFX}..$((MFX + MFW - 1)) step 6, rows 0 -8 8 -4 4 around y ${MENU_DY0}"
  MENU_SCAN_TRACE=""
  MENU_SCAN_NOTE=""
  MENU_WIN=""
  MENU_GEOM=""
  MENU_PICK_BASE=$(picker_windows | tr "\n" " ")
  MENU_PICK_BASE=${MENU_PICK_BASE% }
  for mrow in 0 -8 8 -4 4; do
      MY=$((MENU_DY0 + mrow))
      [ "$MY" -lt "$MFY" ] && MY=$MFY
      [ "$MY" -gt $((MFY + MFH)) ] && MY=$((MFY + MFH))
      MX=$MFX
      while [ "$MX" -le $((MFX + MFW - 1)) ]; do
          MENU_SAMPLES=$((MENU_SAMPLES + 1))
          xdotool mousemove --sync $MX $MY
          sleep 0.25
          xdotool click 3
          sleep 1.0
          POP=$(picker_windows | tr "\n" " ")
          POP=${POP% }
          MENU_SCAN_TRACE="${MENU_SCAN_TRACE}${MENU_SCAN_TRACE:+ }${MX},${MY}=${POP:-none}"
          echo "# menu scan $(date +%T): sample $MX,$MY picker=[${POP:-none}]" >> $G
          if [ -n "$POP" ] && [ -z "$MENU_PICK_BASE" ]; then
              MENU_WIN=$(printf "%s" "$POP" | awk "{print \$1}")
              MENU_KEY="$MX $MY"
              MENU_OFFSET="$((MX - MENU_DX0)) $((MY - MENU_DY0))"
              MENU_PICKER_COUNT=$(printf "%s" "$POP" | wc -w)
              MENU_GEOM=$(win_geo "$MENU_WIN")
              MENU_SCAN_NOTE="hit on row $mrow at x $MX after ${MENU_SAMPLES} sample(s): picker window ${MENU_WIN} ${MENU_GEOM:-unreadable}"
              echo "# menu hit $(date +%T): $MX,$MY after ${MENU_SAMPLES} sample(s), picker=${MENU_WIN} geo=${MENU_GEOM:-unreadable}" >> $G
              break 2
          fi
          if [ -n "$POP" ]; then
              # A window that answers the picker own geometry was ALREADY on screen, or the
              # click put one up that the phase cannot attribute to it: say so and clear it,
              # rather than accepting it as this sample's popup.
              MENU_SCAN_NOTE="a window already on screen answers the picker geometry (before [${MENU_PICK_BASE:-none}], now [$POP]) at $MX,$MY"
              echo "# menu scan $(date +%T): POPUP NOT ATTRIBUTABLE $MX,$MY picker=[$POP] base=[${MENU_PICK_BASE:-none}]" >> $G
              xdotool key --clearmodifiers Escape
              sleep 0.5
          fi
          MX=$((MX + 6))
      done
  done

  # ---- WITH THE POPOVER OPEN: THE KEY IT SELECTED, WHAT IT PUBLISHES, AND THE TWO BLOCKED KEYS ----
  # The popover acts on the selection, so the right-click that opened it made the key the panel
  # own selected key: that half of the state line is the panel own answer that the point is
  # on a key the panel DRAWS, and it is asserted outside against the empty reading above.
  MENU_SEL_AFTER=$(selected_half)
  MENU_BEFORE=$(summary_line)
  # The dock own Easing chip read as a DIAGNOSTIC, not an assertion (see the phase comment):
  # while the popover is up the walk over the whole tree also says what - if anything - the
  # accessibility tree publishes about the popup own widgets, which is what the next reader
  # would otherwise have to re-measure.
  MENU_DOCK_BEFORE=$(ae dump 2>/dev/null | grep -i "easing" | tr "\n" ";" | head -c 200)
  MENU_WIN_INFO=""
  MENU_WIN_TYPE=""
  if [ -n "$MENU_WIN" ]; then
      MENU_WIN_INFO=$(win_info "$MENU_WIN")
      MENU_WIN_TYPE=$(win_type "$MENU_WIN")
      park
      import -window root "$SHOTS/graph_probe_14_menu_open.png"
      cp "$SHOTS/graph_probe_14_menu_open.png" "$SHOTS/graph_probe_41_key_scan.png"
      # THE TWO APPLICATION KEY COMMANDS THE POPOVER MUST BLOCK, then Escape (see the header):
      # Delete would remove exactly the key the popover was opened on, ctrl+Z is the popup own
      # session undo, and the popup own existence is read back from the window list each time.
      # The Escape at the end is ALSO the dismissal this phase uses for every pick below, and
      # here it is the claim that dismissing with nothing picked changes nothing: the draft was
      # never edited, so hideEvent commits nothing.
      park
      xdotool key --clearmodifiers Delete
      sleep 1.3
      MENU_DEL_LINE=$(summary_line)
      MENU_DEL_OPEN=$(window_open "$MENU_WIN")
      import -window root "$SHOTS/graph_probe_32_menu_delete.png"
      xdotool key --clearmodifiers ctrl+z
      sleep 1.3
      MENU_UNDO_LINE=$(summary_line)
      MENU_UNDO_OPEN=$(window_open "$MENU_WIN")
      popup_dismiss
      MENU_ESCAPE_LINE=$(summary_line)
      MENU_ESC_OPEN=$(window_open "$MENU_WIN")
      import -window root "$SHOTS/graph_probe_33_menu_escape.png"
      park
  fi

  # ---- AND THE NEGATIVE, ON THE SAME STATE: A RIGHT-CLICK THAT HITS NO KEY OPENS NOTHING ----
  # The point is the key found above, 60 px along the frame axis and inside the plot, so it is
  # on no key and on no drawn band (a one-key map draws no polyline at all). NO viewable picker
  # window may be up afterwards, and the panel own line must not move.
  MENU_EMPTY=""
  MENU_EMPTY_WINS=""
  MENU_NOKEY_LINE=""
  if [ -n "$MENU_KEY" ]; then
      read -r KX KY <<< "$MENU_KEY"
      MENU_EMPTY_X=$((KX + 60))
      if [ "$MENU_EMPTY_X" -gt $((MFX + MFW - 4)) ]; then
          MENU_EMPTY_X=$((KX - 60))
      fi
      MENU_EMPTY="$MENU_EMPTY_X $KY"
      xdotool mousemove --sync $MENU_EMPTY_X $KY
      sleep 0.3
      xdotool click 3
      sleep 1.2
      MENU_EMPTY_WINS=$(picker_windows | tr "\n" " ")
      MENU_EMPTY_WINS=${MENU_EMPTY_WINS% }
      MENU_NOKEY_LINE=$(summary_line)
      park
  fi
  import -window root "$SHOTS/graph_probe_16_menu_nokey.png"

  # THE ONE LEFT PRESS THIS PHASE IS ABOUT, AND ITS DESELECT GESTURE, ARE MADE AT THE END OF
  # PHASE 4b INSTEAD - MEASURED, NOT PREFERRED. The graph own Ctrl-press deselect gesture in
  # the plot leaves the APPLICATION in a state where the dock own Angle spin box stops
  # accepting typed text for the rest of the session, while still reporting AT-SPI "focused
  # editable" (the full measurement is beside the moved block, after 4b). Phase 4b own typed
  # write is the instrument for that finding, so it has to be made BEFORE the gesture: the
  # write does not depend on the selection (it writes the key at the playhead frame through
  # the Current Keys panel), and every deselect/press assertion below is unchanged by the
  # order. The two control captures (graph_probe_12/13_menu_control_*) moved with the block.

  # ---- REOPENING THE POPOVER, AND THE TILE PRESSED WITH THE REAL POINTER ----
  # The scan own opening is closed by the Escape above, and the phase reopens the popover on
  # the SAME point for the picks below. A reopen is a plain right-click again, and the witness
  # is the same rule the scan used, with one extra half: the picker own window must NOT be on
  # screen before the click and must be on screen after it. That is what "the point is still on
  # the drawn key" means here - the point is on the key because the popover opens there - and
  # it is also the assertion that the popover is usable a second time.
  MENU_TRIES=""
  open_key_menu() { # open_key_menu <x> <y> [shot] -> 0 when the popover came up; sets MENU_WIN/MENU_GEOM
      local shot="${3:-"$SHOTS/graph_probe_34_menu_reopen.png"}"
      local before after
      before=$(picker_windows | tr "\n" " "); before=${before% }
      park
      xdotool mousemove --sync "$1" "$2"
      sleep 0.3
      xdotool click 3
      sleep 1.1
      after=$(picker_windows | tr "\n" " "); after=${after% }
      MENU_TRIES="${MENU_TRIES}${MENU_TRIES:+ }$1,$2=[${after:-none}]"
      echo "# menu reopen $(date +%T): at $1,$2 picker=[${after:-none}] (before [${before:-none}])" >> $G
      [ -n "$after" ] || return 1
      [ -z "$before" ] || return 1   # a window that was already up is not this click's
      MENU_WIN=$(printf "%s" "$after" | awk "{print \$1}")
      MENU_GEOM=$(win_geo "$MENU_WIN")
      import -window root "$shot"
      return 0
  }

  # THE ENTRY THE POINTER TAKES is the picker own SINE TILE (kTileOrder index 4), pressed at
  # the point the picker own layout constants put it at (picker_tile_mid, inside the popup own
  # measured window, which is where a user clicks it). Nothing is assumed about the layout that
  # the source did not say, and the entry is named by the TYPE the tile carries rather than by
  # the pixel it was pressed at; the effect is asserted outside against the panel own state
  # line, and the dismissal that COMMITS it is the Escape.
  MENU_GEOM2=""
  MENU_ENTRY=""
  MENU_ENTRY_PT=""
  MENU_ENTRY_TILE=""
  MENU_ENTRY_CLOSED=""
  MENU_AFTER=""
  MENU_TILE_IDX=$(picker_tile_idx "$MENU_TILE_TYPE")
  if [ -n "$MENU_KEY" ] && [ -n "$MENU_TILE_IDX" ] \
     && open_key_menu $MENU_KEY "$SHOTS/graph_probe_34_menu_reopen.png"; then
      MENU_GEOM2="$MENU_GEOM"
      read -r MWW2 MWH2 MWX2 MWY2 <<< "$MENU_GEOM"
      read -r TX TY <<< "$(picker_tile_mid "$MENU_TILE_IDX")"
      MENU_ENTRY_PT="$((MWX2 + TX)) $((MWY2 + TY))"
      press_picker_tile "$MENU_TILE_IDX" "$MWX2" "$MWY2"
      popup_dismiss
      MENU_ENTRY="$MENU_TILE_TYPE"
      MENU_ENTRY_TILE="$MENU_TILE_TYPE tile index ${MENU_TILE_IDX} of 0..$((PICKER_TILES - 1)) in kTileOrder"
      MENU_AFTER=$(summary_line)
      MENU_ENTRY_CLOSED=$(window_open "$MENU_WIN")
      park
  fi
  MENU_DOCK_AFTER=$(ae dump 2>/dev/null | grep -i "easing" | tr "\n" ";" | head -c 200)
  import -window root "$SHOTS/graph_probe_15_menu_after.png"

  # ---- A SECOND PICK, AND THE POPUP OWN PIXELS WHILE IT IS UP ----
  # WHY THIS IS A THIRD OPENING: the scan own opening was closed by Escape and the pick above
  # by its own dismissal, so this opens the popover afresh at the same point - which is also
  # the assertion that it is usable again. What it does is the SECOND PICK, on a DIFFERENT tile
  # of the same catalog (Round, kTileOrder index 2), so the key really changes again and the
  # ctrl+Z below has something of its own to undo.
  # AND THIS IS WHERE THE PRESS IS MEASURED IN PIXELS, whether or not a curve exists yet: the
  # capture taken with the popover up and the pointer PARKED and the capture taken the same way
  # after the press differ by what the press drew inside the popup - the checked tile's accent
  # fill and glyph - and the phase asserts that the changed region lies INSIDE the popup own own
  # rectangle, which is what makes it the popup surface the press landed on rather than a
  # neighbour of it. The picker own motion-preview bar animates between ANY two captures, so
  # this is not a claim about the tile's fill alone (it says so rather than pretending the
  # animation is not there); WHICH entry was pressed is the state line's business.
  MENU_KB_GEOM=""
  MENU_KB_HL=""
  MENU_KB_HL_BB=""
  MENU_KB_ENTRY=""
  MENU_KB_TILE=""
  MENU_KB_LINE=""
  MENU_KB_CLOSED=""
  MENU_TILE_KB_IDX=$(picker_tile_idx "$MENU_TILE_KB_TYPE")
  if [ -n "$MENU_KEY" ] && [ -n "$MENU_TILE_KB_IDX" ] \
     && open_key_menu $MENU_KEY "$SHOTS/graph_probe_35_menu_kb_open.png"; then
      MENU_KB_GEOM="$MENU_GEOM"
      read -r _ _ KWX KWY <<< "${MENU_GEOM:-0 0 0 0}"   # MENU_GEOM is "w h x y"
      park
      import -window root /tmp/probe_menu_kb0.png
      press_picker_tile "$MENU_TILE_KB_IDX" "$KWX" "$KWY"
      import -window root "$SHOTS/graph_probe_36_picker_tile.png"
      MENU_KB_HL=$(magick compare -metric AE /tmp/probe_menu_kb0.png "$SHOTS/graph_probe_36_picker_tile.png" null: 2>&1 | awk "{print \$1+0}")
      magick compare -compose src /tmp/probe_menu_kb0.png "$SHOTS/graph_probe_36_picker_tile.png" /tmp/probe_menu_kb_diff.png 2>/dev/null
      MENU_KB_HL_BB=$(magick identify -format "%@" /tmp/probe_menu_kb_diff.png 2>/dev/null)
      MENU_KB_ENTRY="$MENU_TILE_KB_TYPE"
      MENU_KB_TILE="$MENU_TILE_KB_TYPE tile index ${MENU_TILE_KB_IDX} of 0..$((PICKER_TILES - 1)) in kTileOrder"
      popup_dismiss
      MENU_KB_LINE=$(summary_line)
      MENU_KB_CLOSED=$(window_open "$MENU_WIN")
      park
  fi
  import -window root "$SHOTS/graph_probe_37_picker_after.png"

  # ---- AND THE APPLICATION OWN KEYS, WITH THE POPOVER CLOSED ----
  # The other half of the modality pair (see the header): the last edit at this point is the
  # tile press above - committed by the dismissal that followed it - so ctrl+Z with no popover
  # up has to undo it, and the panel own state line has to move back to what it read before
  # that pick, which is what MENU_AFTER recorded. That is also what leaves the key carrying
  # Sine for phase 4b to re-shape.
  xdotool key --clearmodifiers ctrl+z
  sleep 1.6
  import -window root "$SHOTS/graph_probe_38_appkey_undo.png"
  MENU_APPKEY_LINE=$(summary_line)

  echo "MENU_FIT=\"$MENU_FIT\""                 >> $G
  echo "MENU_DERIVED=\"$MENU_DX0 $MENU_DY0\""   >> $G
  echo "MENU_KEY=\"$MENU_KEY\""                 >> $G
  echo "MENU_OFFSET=\"$MENU_OFFSET\""           >> $G
  echo "MENU_SAMPLES=\"$MENU_SAMPLES\""         >> $G
  echo "MENU_SCAN_LATTICE=\"$MENU_SCAN_LATTICE\"" >> $G
  echo "MENU_SCAN_NOTE=\"$MENU_SCAN_NOTE\""     >> $G
  echo "MENU_SCAN_TRACE=\"$MENU_SCAN_TRACE\""   >> $G
  echo "MENU_WIN=\"$MENU_WIN\""                 >> $G
  echo "MENU_GEOM=\"$MENU_GEOM\""               >> $G
  echo "MENU_PICKER_COUNT=\"$MENU_PICKER_COUNT\"" >> $G
  echo "MENU_WIN_INFO=\"$MENU_WIN_INFO\""       >> $G
  echo "MENU_WIN_TYPE=\"$MENU_WIN_TYPE\""       >> $G
  echo "MENU_PICK_W=\"$MENU_PICK_W\""           >> $G
  echo "MENU_PICK_H=\"$MENU_PICK_H\""           >> $G
  echo "MENU_PICK_H_CUSTOM=\"$MENU_PICK_H_CUSTOM\"" >> $G
  echo "MENU_TILES=\"$MENU_TILES\""             >> $G
  echo "MENU_TILE_TYPE=\"$MENU_TILE_TYPE\""     >> $G
  echo "MENU_TILE_KB_TYPE=\"$MENU_TILE_KB_TYPE\"" >> $G
  echo "MENU_TILE_IDX=\"$MENU_TILE_IDX\""       >> $G
  echo "MENU_TILE_KB_IDX=\"$MENU_TILE_KB_IDX\"" >> $G
  echo "MENU_PICK_BASE=\"$MENU_PICK_BASE\""     >> $G
  echo "MENU_PRE_SCAN=\"$MENU_PRE_SCAN\""       >> $G
  echo "MENU_SEL_BEFORE=\"$MENU_SEL_BEFORE\""   >> $G
  echo "MENU_SEL_AFTER=\"$MENU_SEL_AFTER\""     >> $G
  echo "MENU_BEFORE=\"$MENU_BEFORE\""           >> $G
  echo "MENU_GEOM2=\"$MENU_GEOM2\""             >> $G
  echo "MENU_ENTRY=\"$MENU_ENTRY\""             >> $G
  echo "MENU_ENTRY_PT=\"$MENU_ENTRY_PT\""       >> $G
  echo "MENU_ENTRY_TILE=\"$MENU_ENTRY_TILE\""   >> $G
  echo "MENU_ENTRY_CLOSED=\"$MENU_ENTRY_CLOSED\"" >> $G
  echo "MENU_AFTER=\"$MENU_AFTER\""             >> $G
  echo "MENU_DOCK_BEFORE=\"$MENU_DOCK_BEFORE\"" >> $G
  echo "MENU_DOCK_AFTER=\"$MENU_DOCK_AFTER\""   >> $G
  echo "MENU_EMPTY=\"$MENU_EMPTY\""             >> $G
  echo "MENU_EMPTY_WINS=\"$MENU_EMPTY_WINS\""   >> $G
  echo "MENU_NOKEY_LINE=\"$MENU_NOKEY_LINE\""   >> $G
  echo "MENU_DEL_LINE=\"$MENU_DEL_LINE\""       >> $G
  echo "MENU_DEL_OPEN=\"$MENU_DEL_OPEN\""       >> $G
  echo "MENU_UNDO_LINE=\"$MENU_UNDO_LINE\""     >> $G
  echo "MENU_UNDO_OPEN=\"$MENU_UNDO_OPEN\""     >> $G
  echo "MENU_ESCAPE_LINE=\"$MENU_ESCAPE_LINE\"" >> $G
  echo "MENU_ESC_OPEN=\"$MENU_ESC_OPEN\""       >> $G
  echo "MENU_KB_GEOM=\"$MENU_KB_GEOM\""         >> $G
  echo "MENU_KB_HL=\"$MENU_KB_HL\""             >> $G
  echo "MENU_KB_HL_BB=\"$MENU_KB_HL_BB\""       >> $G
  echo "MENU_KB_ENTRY=\"$MENU_KB_ENTRY\""       >> $G
  echo "MENU_KB_TILE=\"$MENU_KB_TILE\""         >> $G
  echo "MENU_KB_LINE=\"$MENU_KB_LINE\""         >> $G
  echo "MENU_KB_CLOSED=\"$MENU_KB_CLOSED\""     >> $G
  echo "MENU_APPKEY_LINE=\"$MENU_APPKEY_LINE\"" >> $G
  echo "MENU_TRIES=\"$MENU_TRIES\""             >> $G
  # The values are echoed as ONE assignment (not as a free-form sentence): the outer shell
  # eval()s this file, and a bare sentence with brackets and parentheses in it is a syntax
  # error there - which would leave every variable BELOW it unset and turn one broken line
  # into a cascade of unrelated failures.
  echo "MENU_KBDNOTE=\"delete[${MENU_DEL_LINE:-none}] deleteopen[${MENU_DEL_OPEN:-none}] undo[${MENU_UNDO_LINE:-none}] undoopen[${MENU_UNDO_OPEN:-none}] escape[${MENU_ESCAPE_LINE:-none}] escopen[${MENU_ESC_OPEN:-none}] pick2[${MENU_KB_LINE:-none}] pick2closed[${MENU_KB_CLOSED:-none}] appkey[${MENU_APPKEY_LINE:-none}]\"" >> $G

  # ---- PHASE 4b: THE CURVE THE PICKER OWN EASING ACTUALLY SHAPES ----
  # WHY THIS IS NEEDED. Everything above reads the easing back from the panel own state
  # line - and MEASURED, that line is NOT DRAWN while a curve exists: the panel paints
  # its summary only while the model is EMPTY (ImGuiOverlay.cpp:843 draws
  # aModel.emptyState inside the plot; with curves present emptyState is empty and the
  # line lives in the accessibility tree alone). The easing is drawn in exactly ONE
  # place: the polyline of the segment it shapes - and a map of ONE key has no segment
  # (graph::Curve::buildPolyline loops i + 1 < keys.size()), so a one-key panel draws a
  # key square and no curve, and the easing change above moved no pixel at all. So this
  # phase MAKES the segment: a second key at the NEXT frame carrying a DIFFERENT value,
  # after which the segment from the first key is drawn, and re-shaping that segment
  # changes the curve own pixels - which is the only pixel-level proof that a pick in
  # phase 4 reached the panel at all.
  #
  # HOW THE SECOND KEY IS MADE, all of it by driving the app the way a user does:
  #   * the playhead moves one frame right through the app own own key command, Ctrl+Right
  #     ("Move one frame to the right": KeyCommandMap -> MainWindow::onDisplacementTriggered
  #     -> timeLineWidget().setFrame(current + 1), MainWindow.cpp:885). The key event
  #     reaches the main window because nothing else consumes it: the viewport own own
  #     keyPressEvent accepts a key only while an ImGui text field owns the keyboard
  #     (MainDisplayWidget.cpp:672).
  #   * THE MOVE IS ASSERTED BY THE DOCKS OWN OUTPUT, not by a frame number: the Current
  #     Keys panel shows a channel own rows only while a key exists at the current frame and
  #     its add-key KNOB row otherwise (prop::CurrentKeyGroup::setKeyExists), so the knob
  #     row coming back IS the dock saying there is no key here any more.
  #   * pressing that knob adds the key at this frame, seeded from the value playback
  #     holds - the same value, so the segment would be flat and the easing invisible
  #     again. The value is changed through the dock own own Angle field with a REAL edit
  #     (click, select all, type, Return): the field own handler hangs off editingFinished,
  #     which an accessibility write does not fire (prop_Items.cpp:543), and the edit is
  #     read back through the field own Value interface and asserted.
  #   * THE KEY THE MENU ACTS ON HAS NOT MOVED. A second key on a channel already shown
  #     does not change the shown-curve basis, and only a basis change refits the value
  #     window (GraphEditor::syncValueAxis), while the frame window is written only by a
  #     fit or the wheel - so the point the bounded scan found is still on that first key.
  ANGLE_KEY=$(angle_value 2>/dev/null)
  xdotool key --clearmodifiers ctrl+Right
  sleep 1.2; park
  KNOB_2ND=$(ae extents "Rotate" "label")
  ANGLE_2ND_SEEDED=""
  ANGLE_2ND_WRITTEN=""
  ANGLE_2ND_RECT=""
  ANGLE_2ND_TRIES=0
  ANGLE_2ND_TYPED_TEXT=""
  ANGLE_2ND_EDIT_TEXT=""
  DOCK_FOCUS_BEFORE=""
  DOCK_FOCUS_CLICK=""
  DOCK_FOCUS_AFTER=""
  DOCK_TARGET_BEFORE=""
  DOCK_TARGET_AFTER=""
  NEWDEG=""
  if [ -n "$KNOB_2ND" ]; then
      # PRESSED AT THE ROW THE TREE REPORTS, the same route phase 2 uses and the one the
      # recorded runs prove works (the Angle field becomes readable, which is what the
      # assertions below read). The widget OWN action was tried and does NOT add the key here
      # (MEASURED: the channel rows stayed hidden), so the coordinate route is the one this
      # probe drives.
      #
      # AND IT IS PRESSED AGAIN IF IT DID NOT LAND, bounded at three attempts. MEASURED in the
      # run this probe was fixed against: ONE press at this exact rectangle left the channel
      # rows hidden, while the same press at the same rectangle had added the frame-0 key in
      # phase 2 minutes earlier. MEASURED again while fixing it: the SAME press DID land on the
      # next run of the same state (the Angle field answered 0 and the edit below took), so the
      # failure is a press that was lost rather than a knob that cannot be pressed - and a
      # press that does not land cannot be told apart from a dead knob by anything except the
      # dock OWN output, which is what the loop below reads. A retry after a press that DID land can never fire: the Angle field is
      # readable by then, and that readability is also the evidence the assertions below use.
      #
      # AND WHY THIS RETRY IS NOT THE MENU DEFECT CLASS (a consumed event that no repaint
      # processes). The knob is a prop::KeyKnocker, a plain QWidget in the Animation dock: it
      # acts in its own mouseReleaseEvent - a left release inside its own rect - and the row it
      # turns into is a widget-tree change driven by the document notifier (setKeyExists, which
      # calls setVisible), not by anything drawn. No viewport repaint is in that path at all,
      # and the tool that clicks it here never reaches MainDisplayWidget, the only caller of
      # ImGuiOverlay::press - so an ImGui event held back by the overlay cannot be what a lost
      # knob press is. It is a press that did not arrive (or did not land inside the rect), and
      # the bounded retry is the honest instrument for that.
      read -r K2X K2Y K2W K2H <<< "$KNOB_2ND"
      KNOB_2ND_PRESSES=0
      for attempt in 1 2 3; do
          KNOB_2ND_PRESSES=$attempt
          xdotool mousemove --sync $((K2X + K2W / 2)) $((K2Y + K2H / 2))
          sleep 0.4
          xdotool click 1
          sleep 1.6; park
          ANGLE_2ND_SEEDED=$(angle_value 2>/dev/null)
          if [ -n "$ANGLE_2ND_SEEDED" ]; then break; fi
      done
      ANGLE_2ND_RECT=$(angle_rect 2>/dev/null)
      ANGLE_2ND_TRIES=0
      if [ -n "$ANGLE_2ND_RECT" ] && [ -n "$ANGLE_2ND_SEEDED" ]; then
          NEWDEG=$(awk -v a="$ANGLE_2ND_SEEDED" "BEGIN { printf \"%.2f\", a + 30 }")
          read -r A2X A2Y A2W A2H <<< "$ANGLE_2ND_RECT"
          ANGLE_2ND_TRIES=1
          # THREE READINGS, before the click into the field, straight after it, and after the
          # commit key - plus the panel's OWN LINE beside them (the model's statement of what
          # it is drawing: how many channels are shown and which one is focused, which is
          # the target of the write). This read used to be the legend rows, one per channel;
          # the panel publishes no rows any more, and its line carries the same facts.
          # Nothing here is a retry and nothing here changes the write: the one attempt below
          # is the phase own, and these readings say what the field could see.
          DOCK_FOCUS_BEFORE=$(dock_focus 2>/dev/null | tr "\n" "; ")
          DOCK_TARGET_BEFORE=$(panel_line) 2>/dev/null
          xdotool mousemove --sync $((A2X + A2W / 2)) $((A2Y + A2H / 2)) click 1
          sleep 0.5
          DOCK_FOCUS_CLICK=$(dock_focus 2>/dev/null | tr "\n" "; ")
          xdotool key --clearmodifiers ctrl+a
          sleep 0.2
          xdotool type --delay 60 "$NEWDEG"
          sleep 0.3
          # THE FIELD OWN VALUE WHILE THE EDIT IS STILL OPEN, read BEFORE the commit key. This
          # is the reading that tells the two ways a typed write can fail apart, and it changes
          # nothing: the commit key follows it, and the phase own assertion is still about the
          # value AFTER the commit. 30.00 here means the characters ARRIVED and the commit was
          # refused (a lock or a re-entrancy in the write path); 0 means they never reached the
          # field at all (an ImGui keyboard capture still holding the keys, for instance).
          ANGLE_2ND_TYPED_TEXT=$(angle_value 2>/dev/null)
          ANGLE_2ND_EDIT_TEXT=$(angle_text 2>/dev/null)
          xdotool key --clearmodifiers Return
          sleep 1.2; park
          DOCK_FOCUS_AFTER=$(dock_focus 2>/dev/null | tr "\n" "; ")
          DOCK_TARGET_AFTER=$(panel_line) 2>/dev/null
          ANGLE_2ND_WRITTEN=$(angle_value 2>/dev/null)
      fi
  fi
  KNOB_2ND_AFTER=$(ae extents "Rotate" "label")
  # Straight after the typed edit: the angle write goes through the dock own key accessor,
  # which notifies through ctrl::TimeLineUtil::Notifier (prop_KeyAccessor.cpp) and so reaches
  # the viewport repaint on the same signal a timeline key push does. Nothing forced.
  import -window root "$SHOTS/graph_probe_26_curve_sine.png"

  # THE MENU AGAIN, ON THE SAME KEY, CHANGING THE EASING - and this time a segment is
  # drawn, so the change is in the panel own OWN pixels rather than in a line only a screen
  # reader can see. The entry is taken with the picker own TILE, pressed where its own layout
  # constants put it: Linear is kTileOrder's FIRST entry (index 0), so this is the first tile
  # of the catalog. The anchor is the point the scan found - the key has not moved, since
  # adding a key to a channel already shown does not refit the frame window - and it is
  # REOPENED rather than assumed: the picker own window coming back on screen at the same
  # point is what says the anchor is still on the drawn key.
  read -r MKX2 MKY2 <<< "${MENU_KEY:-0 0}"
  PICKER_LINEAR_IDX=$(picker_tile_idx Linear)
  pick_linear() { # pick_linear <capture-to-write> -> 0 when the popover opened and its Linear tile was pressed
      # The picker own catalog: the Linear tile is index 0 of kTileOrder, and the index is
      # DERIVED from the source above rather than assumed. The press is followed by the
      # dismissal that commits the draft (Escape), because the widget writes the model only
      # on close. Nothing is echoed here - the caller reads MENU_GEOM - so a chatty helper
      # cannot contaminate a value.
      local gx gy
      [ -n "$PICKER_LINEAR_IDX" ] || return 1
      open_key_menu $MKX2 $MKY2 "$1" || return 1
      read -r _ _ gx gy <<< "${MENU_GEOM:-0 0 0 0}"
      press_picker_tile "$PICKER_LINEAR_IDX" "$gx" "$gy"
      popup_dismiss
      return 0
  }
  MENU2_GEOM=""
  MENU2_ENTRY=""
  MENU2_LINE=""
  if pick_linear "$SHOTS/graph_probe_27_menu_linear_open.png"; then
      MENU2_GEOM="$MENU_GEOM"
      MENU2_ENTRY="Linear (tile index ${PICKER_LINEAR_IDX} of kTileOrder, pressed with the pointer)"
  fi
  # Nothing forced: the entry is the application own action, and the edit reaches the viewport
  # through the timeline own notifier, exactly as in phase 4.
  import -window root "$SHOTS/graph_probe_28_curve_linear.png"
  MENU2_LINE=$(summary_line)

  # AND THE SAME EASING A SECOND TIME: re-picking what the key already carries must move
  # nothing at all - the control that makes the assertion above an assertion about the
  # EASING rather than about the picker having been used.
  MENU3_GEOM=""
  MENU3_ENTRY=""
  MENU3_LINE=""
  if pick_linear "$SHOTS/graph_probe_29_menu_linear_again_open.png"; then
      MENU3_GEOM="$MENU_GEOM"
      MENU3_ENTRY="Linear (the same tile ${PICKER_LINEAR_IDX}, pressed again)"
  fi
  import -window root "$SHOTS/graph_probe_30_curve_linear_again.png"
  MENU3_LINE=$(summary_line)

  # ---- THE ONE LEFT PRESS THIS PHASE IS ABOUT, AND THE READING IT HAS TO MOVE ----
  # WHY IT IS HERE AND NOT IN PHASE 4, MEASURED RATHER THAN PREFERRED. The press has to be
  # shown to have SELECTED the key, and the reading it moves is taken immediately before it.
  # Phase 4 has already asserted that the RIGHT-CLICK selected the key (MENU_SEL_BEFORE is
  # empty, MENU_SEL_AFTER names it); this pair is the editor own gesture on the same point,
  # made with no modifier - the kind of press the phase can make without perturbing anything
  # else, because ctrl::GraphEditor::ownsCursor claims it (so no viewport tool sees it) and a
  # press only SELECTS: it arms a key move and commits nothing. The selection is cleared again
  # first, by the same graph own Ctrl-press deselect gesture, made at the plot own right-hand
  # end for the same reason as in phase 4, and the empty reading is asserted outside so that
  # "it moved" is about THIS press. The two captures graph_probe_12/13_menu_control_* are
  # taken straight after it (a stable state, nothing forced between them).
  #
  # AND WHY THE GESTURE RUNS LAST, WITH ITS OWN MEASUREMENT BESIDE IT: a Ctrl-press in the
  # plot is not inert for the rest of the session. MEASURED standalone on this build (one app
  # instance, phase 2's state, the key at frame 0):
  #   * `Ctrl+Right`, then press the dock own Rotate knob (a key at the new frame), then click
  #     the dock own unnamed Angle spin box, Ctrl+A, type 21.00, Return: the field reads
  #     [21] WHILE THE EDIT IS OPEN (keyboardTracking commits as you type, so the characters
  #     arrived) and [21] after Return - the write COMMITS.
  #   * do the Ctrl-press deselect in the plot FIRST, then the same write with 22.00: the
  #     field still reads [21] while the edit is open and [21] after Return - the typed
  #     characters never reach it, and they never reach it again in that session (a second
  #     attempt, and attempts after a key-menu pick or a dismissal-closed popover, all fail the
  #     same way. (That last pair was MEASURED against the QMenu this phase used before the
  #     picker; the side effect belongs to the Ctrl-press gesture in the graph editor, not to
  #     the control the pick was made with, and nothing here depends on the wording.)
  #   * the state is NOT a stuck modifier and NOT a focus change: an Xlib observer on a bare
  #     Xvfb shows the X modifier state 0x0 (no Ctrl) after `xdotool keyup --clearmodifiers
  #     ctrl`, `xdotool getwindowfocus` is the application own main window before and after
  #     the failing click, NO extra top-level window exists (the window list is unchanged), and
  #     the spin box still reports AT-SPI "showing enabled focused editable" after the click
  #     in BOTH the working and the broken case. So the application own state is what changes,
  #     which is why this probe does not "fix" it: it is recorded here, and phase 4b own typed
  #     write - the instrument for it - is made BEFORE this gesture so that the write own
  #     assertion is about the menu and not about this interaction.
  MENU_SEL_BEFORE2=""
  MENU_SEL_AFTER2=""
  if [ -n "$MENU_KEY" ]; then
      read -r KX KY <<< "$MENU_KEY"
      park
      # CTRL DOWN BEFORE THE MOVE, not after: the widget re-reads the modifiers on every
      # event it routes, and the move is what writes the pointer position the press is
      # tested against, so the state has to be the deselect press one while the pointer is
      # already over the point.
      xdotool keydown --clearmodifiers ctrl
      sleep 0.2
      xdotool mousemove --sync $MENU_DESELECT_X $MENU_DY0
      sleep 0.4
      xdotool click 1
      sleep 0.2
      xdotool keyup --clearmodifiers ctrl
      sleep 1.2
      park
      MENU_SEL_BEFORE2=$(selected_half)
      xdotool mousemove --sync $KX $KY
      sleep 0.4
      xdotool click 1
      sleep 1.2
      park
      MENU_SEL_AFTER2=$(selected_half)
  fi

  # The control pair, straight off the state the deselect and the press left: nothing changed
  # between them, so nothing is forced and the pair is expected to be 0 px.
  import -window root "$SHOTS/graph_probe_12_menu_control_a.png"
  import -window root "$SHOTS/graph_probe_13_menu_control_b.png"
  # The four readings the moved block produces are written here rather than with phase 4 own
  # echo block (which runs before them): the outer shell evals the whole file, so the order of
  # the assignment lines does not matter.
  echo "MENU_DESELECT=\"$MENU_DESELECT\""       >> $G
  echo "MENU_STATE_BEFORE=\"$MENU_STATE_BEFORE\"" >> $G
  echo "MENU_SEL_BEFORE2=\"$MENU_SEL_BEFORE2\"" >> $G
  echo "MENU_SEL_AFTER2=\"$MENU_SEL_AFTER2\""   >> $G

  echo "phase 4b: the angle of the key was [${ANGLE_KEY:-unreadable}], the knob row at the next frame [${KNOB_2ND:-none}], the seeded value after [${KNOB_2ND_PRESSES:-?}] press(es) [${ANGLE_2ND_SEEDED:-unreadable}], the field own rect [${ANGLE_2ND_RECT:-none}], typed [${NEWDEG:-none}], read back [${ANGLE_2ND_WRITTEN:-unreadable}], the knob row after the press [${KNOB_2ND_AFTER:-none}]; the popover own rect [${MENU2_GEOM:-none}] and the entry taken [${MENU2_ENTRY:-none}], the state line [${MENU2_LINE:-none}]; picking it again: rect [${MENU3_GEOM:-none}], entry [${MENU3_ENTRY:-none}], line [${MENU3_LINE:-none}]"
  echo "ANGLE_KEY=\"$ANGLE_KEY\""             >> $G
  echo "KNOB_2ND=\"$KNOB_2ND\""               >> $G
  echo "KNOB_2ND_PRESSES=\"$KNOB_2ND_PRESSES\"" >> $G
  echo "KNOB_2ND_AFTER=\"$KNOB_2ND_AFTER\""   >> $G
  echo "ANGLE_2ND_SEEDED=\"$ANGLE_2ND_SEEDED\"" >> $G
  echo "ANGLE_2ND_RECT=\"$ANGLE_2ND_RECT\""   >> $G
  echo "ANGLE_2ND_TRIES=\"$ANGLE_2ND_TRIES\"" >> $G
  echo "ANGLE_2ND_TYPED_TEXT=\"$ANGLE_2ND_TYPED_TEXT\"" >> $G
  echo "ANGLE_2ND_EDIT_TEXT=\"$ANGLE_2ND_EDIT_TEXT\"" >> $G
  echo "DOCK_FOCUS_BEFORE=\"$DOCK_FOCUS_BEFORE\"" >> $G
  echo "DOCK_FOCUS_CLICK=\"$DOCK_FOCUS_CLICK\""   >> $G
  echo "DOCK_FOCUS_AFTER=\"$DOCK_FOCUS_AFTER\""   >> $G
  echo "DOCK_TARGET_BEFORE=\"$DOCK_TARGET_BEFORE\"" >> $G
  echo "DOCK_TARGET_AFTER=\"$DOCK_TARGET_AFTER\""   >> $G
  echo "NEWDEG=\"$NEWDEG\""                   >> $G
  echo "ANGLE_2ND_WRITTEN=\"$ANGLE_2ND_WRITTEN\"" >> $G
  echo "MENU2_GEOM=\"$MENU2_GEOM\""           >> $G
  echo "MENU2_ENTRY=\"$MENU2_ENTRY\""         >> $G
  echo "MENU2_LINE=\"$MENU2_LINE\""           >> $G
  echo "MENU3_GEOM=\"$MENU3_GEOM\""           >> $G
  echo "MENU3_ENTRY=\"$MENU3_ENTRY\""         >> $G
  echo "MENU3_LINE=\"$MENU3_LINE\""           >> $G

  # ---- PHASE 3: THE DELETED TARGET, THE GATE THIS PROBE EXISTS FOR ----
  # ctrl::GraphEditor keeps its targets as RAW core::ObjectNode* (GraphEditor.h:409,
  # mTargets), and buildEntries() dereferences every one of them on every draw
  # (GraphEditor.cpp:641). Nothing in that list is told when a node is destroyed, so the
  # only thing standing between a deletion and a use-after-free is an ORDERING property
  # in the tree widget: the removal of the row re-emits itemSelectionChanged before
  # anything repaints, its handler resolves the selection to nullptr, and the lambda in
  # MainWindow then hands setGraphTargets() the (now empty) selection - the dead node
  # leaves the target list before buildEntries() can walk it. That order is asserted
  # nowhere: the panel tests in the suite drive a SYNTHETIC model, and this probe never
  # deleted a target. A regression would not fail an assertion, it would SIGSEGV inside
  # the draw of the panel, and under AT-SPI that surfaces as a walk that times out. So
  # delete the very layer the panel is showing and require: the process still alive, the
  # row gone, and the panel back in one of the model own empty states with the pixels of
  # the curve gone.
  #
  # HOW THE DELETE ENTRY IS PRESSED. Three measured facts decide the route, and each was
  # established by running the app under Xvfb rather than assumed:
  #   * the row reported by AT-SPI is NOT clipped to the tree viewport (MEASURED:
  #     "Folder16" reports 40 221 115 22 while the splitter leaves the tree about 68 px
  #     wide), so the CENTRE of the rect lands in the timeline beside it: a right-click
  #     there opens the menu of the TIMELINE (read off a screenshot: Copy key / Copy key
  #     to clipboard / Paste key / Select spacing between keys / Change key(s) easing to /
  #     Change key(s) range to / Delete key). The click is therefore made at the LEFT edge
  #     of the row rect + 4 px, which is inside the row and inside the tree, and it is the
  #     menu the object tree owns that opens (read off a screenshot: Contract / Rename /
  #     Create layer / Create folder / Paste from clipboard / Duplicate node / Add default
  #     keys / Delete).
  #   * that menu is NOT in the accessibility tree at all, so its entries cannot be
  #     addressed by accessible name: MEASURED with the menu on screen and its window
  #     mapped IsViewable, the tree of the app still published exactly the five menu-bar
  #     popups and no `menu item` named Delete, while a desktop-wide walk found "Delete"
  #     only in OTHER applications on the bus (KeePassXC and friends). The menu is a QMenu
  #     created on the stack inside ObjectTreeWidget::onContextMenuRequested
  #     (ObjectTreeWidget.cpp:699) and the a11y bridge never exposes it, unlike the menus
  #     of the menu bar, which are published even while closed. So there is no name to
  #     use, and this is recorded as a gap rather than worked around.
  #   * what IS usable is the window of the menu itself plus the keyboard. The app opens
  #     it AT THE POINTER, so `xwininfo -root -children` names the window whose origin is
  #     the click point (MEASURED: 0x200032 158x223+44+232, Map State IsViewable). With
  #     no window manager the menu does not take the input focus by itself - MEASURED:
  #     `xdotool getwindowfocus` reports the window of the app while the menu is up, and
  #     keys sent without focusing it do nothing (Up+Return left the row in place) - so
  #     the probe points the focus AT that window and then drives the menu with Up (which
  #     highlights the LAST entry of the menu) and Return. That entry is Delete by
  #     construction (onContextMenuRequested adds mDeleteAction last, after the other
  #     seven) and by measurement (the menu was read off the screenshot listed above: it
  #     offers exactly ONE wording that means remove-this-layer, "Delete"). The Qt menu
  #     type-ahead is NOT a name route here: MEASURED, typing d,e,l,e,t,e then Return
  #     deleted nothing. The probe therefore identifies the entry by menu position and
  #     asserts the EFFECT, which is what makes the gate sound either way: if the entry
  #     pressed were any other one, the row would not be gone and the panel would not
  #     fall back - both of which are asserted below.
  # The pre-deletion control pair, with nothing forced between them: the panel is drawing
  # the state the deletion is about to change, and the pair is expected to be 0 px.
  import -window root "$SHOTS/graph_probe_17_pre_delete_a.png"
  import -window root "$SHOTS/graph_probe_18_pre_delete_b.png"

  # The layer to delete is the one the PANEL names as its own focus - the "<layer>" half of
  # its line ("..., focused: <layer> : <channel>"), which the model builds from the editor's
  # focused channel - addressed as the `table cell` the tree publishes for it. That is what
  # ties the deletion to what the panel is showing; the row the phase-2 loop happened to
  # activate is the fallback, and it is used whenever no channel is focused (the focus is
  # pruned by the editor when the shown set changes, so it may be gone by now).
  LAYER=$(focused_layer)
  LROW=$(ae extents "$LAYER" "table cell")
  if [ -z "$LROW" ] && [ -n "$KEY_ROW" ]; then
      LAYER="$KEY_ROW"
      LROW=$(ae extents "$LAYER" "table cell")
  fi
  POP=""
  POP_INFO=""
  DEL_FOCUS=""
  if [ -n "$LROW" ]; then
      read -r RX RY RW RH <<< "$LROW"
      DX=$((RX + 4)); DY=$((RY + RH / 2))
      xdotool mousemove --sync $DX $DY
      sleep 0.4
      xdotool click 3
      sleep 1.2
      POP=$(xwininfo -root -children 2>/dev/null | grep -E "[0-9]+x[0-9]+\+${DX}\+${DY}" | grep -oE "0x[0-9a-f]+" | head -1)
      if [ -n "$POP" ]; then
          POP_INFO=$(xwininfo -id "$POP" 2>/dev/null | grep -E "Map State|Width:|Height:" | tr -s " " | tr "\n" ";")
          xdotool windowfocus --sync "$POP" 2>/dev/null
          DEL_FOCUS=$(xdotool getwindowfocus 2>/dev/null)
          xdotool key --clearmodifiers Up
          sleep 0.4
          xdotool key --clearmodifiers Return
          sleep 2
      fi
  fi

  # THE ORDER OF THE TWO QUESTIONS MATTERS AND SO DOES THE FIRST ONE: a process that died
  # in this deletion cannot be asked anything, and every later `ae` call would spend its
  # own timeout saying so.
  DEL_ALIVE=$(kill -0 $app 2>/dev/null && echo yes || echo no)
  DEL_ROW_AFTER=$(ae extents "$LAYER" "table cell")
  DEL_EMPTY=$(ae extents "Select a layer to show its curves" "label")
  DEL_NOKEYS=$(ae extents "No keys on the selected layers" "label")
  DEL_ALLHID=$(ae extents "All channels of the selected layers are hidden" "label")
  # The panel's ONE control, asked for under BOTH names it can carry: with the keyed layer
  # gone there is no channel to list, so the model publishes no button at all. (Reading one
  # name alone would pass vacuously for a channel that still existed but were hidden.)
  DEL_LEGEND=$(ae extents "1 channel(s)" "push button")
  DEL_CHAN_HIDDEN=$(ae extents "0 of 1" "push button")
  DEL_CELLS=$(ae dump 2>/dev/null | sed -n "s/^ *table cell *| //p" | tr "\n" ";")
  # No forced paint: deleting the row changes the tree selection, and the tree own
  # selection signal asks the viewport for a frame (DriverHolder::onSelectionChanged ->
  # onVisualUpdated, plus the setGraphTargets call MainWindow makes on the same signal).
  import -window root "$SHOTS/graph_probe_19_after_delete.png"

  echo "DEL_LAYER=\"$LAYER\""             >> $G
  echo "AE_PID=\"$app\""                   >> $G
  echo "DEL_ROW=\"$LROW\""               >> $G
  echo "DEL_POPUP=\"$POP\""              >> $G
  echo "DEL_POPUP_INFO=\"$POP_INFO\""    >> $G
  echo "DEL_FOCUS=\"$DEL_FOCUS\""        >> $G
  echo "DEL_ALIVE=\"$DEL_ALIVE\""        >> $G
  echo "DEL_ROW_AFTER=\"$DEL_ROW_AFTER\"" >> $G
  echo "DEL_EMPTY=\"$DEL_EMPTY\""        >> $G
  echo "DEL_NOKEYS=\"$DEL_NOKEYS\""      >> $G
  echo "DEL_ALLHID=\"$DEL_ALLHID\""      >> $G
  echo "DEL_LEGEND=\"$DEL_LEGEND\""      >> $G
  echo "DEL_CHAN_HIDDEN=\"$DEL_CHAN_HIDDEN\"" >> $G
  echo "DEL_CELLS=\"$DEL_CELLS\""        >> $G

  kill $app 2>/dev/null; wait $app 2>/dev/null
AE_PROBE_BODY_END

# THE BODY'S OWN FIRST WRITE IS ITS WORD ON WHETHER THE BODY RAN AT ALL, and the two cases
# are different failures that used to share one message. An ABSENT file means the body never
# reached its geometry write - it died early, its output is in the body log, and nothing here
# can be said about the panel. A file that is there but empty of FRAME/PROP/ANIM is the panel
# (or the AT-SPI tree) reporting no usable rectangle. Measured 2026-10-14: the absent case
# printed only "the overlay bounds came back non-empty from AT-SPI ()" and sent a whole
# investigation looking for an X server fault.
if [ ! -e /tmp/probe_graph_overlay_geom.txt ]; then
    check 1 "the probe body ran to its first geometry write (nothing wrote /tmp/probe_graph_overlay_geom.txt; the body's own output is in /tmp/probe_graph_overlay_body.log)"
    summary probe_graph_overlay
fi

if grep -q TIMEOUT /tmp/probe_graph_overlay_geom.txt 2>/dev/null; then
    check 1 "the application reached its main window"
    summary probe_graph_overlay
fi
if grep -q NOGEOM /tmp/probe_graph_overlay_geom.txt 2>/dev/null; then
    # NOT a silent pass: with no frame, no Properties dock and no Animation panel there
    # is no way to say WHERE the change happened, and a probe that cannot bound its
    # measurement must fail rather than measure the whole screen and call it good.
    check 1 "the overlay bounds were readable from AT-SPI ($(tr '\n' ' ' < /tmp/probe_graph_overlay_geom.txt 2>/dev/null))"
    summary probe_graph_overlay
fi

eval "$(cat /tmp/probe_graph_overlay_geom.txt 2>/dev/null)"
if [ -z "${FRAME:-}" ] || [ -z "${PROP:-}" ] || [ -z "${ANIM:-}" ]; then
    check 1 "the overlay bounds came back non-empty from AT-SPI ($(tr '\n' ' ' < /tmp/probe_graph_overlay_geom.txt 2>/dev/null))"
    summary probe_graph_overlay
fi
read -r FRAME_X FRAME_Y FRAME_W FRAME_H <<< "${FRAME:-0 0 0 0}"
read -r PROP_X PROP_Y PROP_W PROP_H <<< "${PROP:-0 0 0 0}"
read -r ANIM_X ANIM_Y ANIM_W ANIM_H <<< "${ANIM:-0 0 0 0}"
# Pre-initialized so a rectangle that parses short cannot abort the run under `set -u`
# (the checks below would fail with an unbound variable instead of a readable FAIL).
FRAME_X=${FRAME_X:-0}; FRAME_Y=${FRAME_Y:-0}; FRAME_W=${FRAME_W:-0}; FRAME_H=${FRAME_H:-0}
PROP_X=${PROP_X:-0}; PROP_Y=${PROP_Y:-0}; PROP_W=${PROP_W:-0}; PROP_H=${PROP_H:-0}
ANIM_Y=${ANIM_Y:-0}; ANIM_H=${ANIM_H:-0}
# The crop's top edge is the menu bar's bottom, which is the dock area's top edge: the
# menu bar has no accessible name (see the header), and the docks start exactly under it.
VIEW_TOP=$PROP_Y
CROP_W=$((PROP_X - FRAME_X))
CROP_H=$((ANIM_Y - VIEW_TOP))
CROP_AREA=$((CROP_W * CROP_H))
echo "geometry: frame ($FRAME); Properties ($PROP) -> viewport right x=$PROP_X; Animation ($ANIM) -> viewport bottom y=$ANIM_Y; crop ${CROP_W}x${CROP_H}+${FRAME_X}+${VIEW_TOP}"

# A rectangle that is not a rectangle is not a bound: every edge used below is asserted
# usable before anything is measured against it, and the crop is asserted non-empty.
[ "$FRAME_W" -gt 0 ] && [ "$FRAME_H" -gt 0 ] && [ "$PROP_W" -gt 0 ] && [ "$ANIM_H" -gt 0 ]
check $? "the AT-SPI rectangles are usable (frame $FRAME, Properties $PROP, Animation $ANIM)"
[ "$PROP_X" -gt "$FRAME_X" ] && [ "$ANIM_Y" -gt "$VIEW_TOP" ]
check $? "the viewport crop is a usable rectangle (${CROP_W}x${CROP_H}+${FRAME_X}+${VIEW_TOP} = $CROP_AREA px)"
[ "$VIEW_TOP" -gt "$FRAME_Y" ] && [ "$VIEW_TOP" -lt "$((FRAME_Y + FRAME_H))" ]
check $? "the crop starts below the window's top edge and inside it (menu bar bottom $VIEW_TOP, frame y $FRAME_Y..$((FRAME_Y + FRAME_H)))"

# ---- THE SUBJECT IS PINNED, AND THE PROBE SAYS WHICH PROJECT IT REALLY RAN ON ----
# Only when this probe chose the project itself (arg 1 / AE_PROJECT unset): a caller who
# names a project is choosing the subject on purpose, and the probe then records what it
# got rather than asserting a pin it did not make. See the header for why the fixture
# argument is not what the app opens.
if [ "$PINNED" -eq 1 ]; then
    LOADED=$(sed -n 's/^image path = "\(.*\)"$/\1/p' /tmp/probe_graph_overlay.log 2>/dev/null | head -1)
    [ "$LOADED" = "$ROOT/assets/sample.psd" ]
    check $? "the pinned project really loaded: the app's own log names [$LOADED] (the fixture argument is not opened - the command line opens only .anie, Main.cpp:212 - so the Debug build's startup project assets/sample.psd is the subject, and it is in the repository)"

    [ "${ROW:-}" = "sample" ]
    check $? "and the object tree's first row names that project (row [${ROW:-none}], the PSD's own base name; a different project here means the pin needs re-recording)"
fi

assert_exists "$SHOTS/graph_probe_01_off_empty.png" "the viewport with the graph off and nothing selected"
assert_exists "$SHOTS/graph_probe_02_on_empty.png"  "the viewport with the graph on and nothing selected"
assert_exists "$SHOTS/graph_probe_03_off_empty.png" "the viewport with the graph off again"
assert_exists "$SHOTS/graph_probe_04_off_top.png"   "the viewport with the top row selected and the graph off"
assert_exists "$SHOTS/graph_probe_05_on_top.png"    "the viewport with the top row selected and the graph on"
assert_exists "$SHOTS/graph_probe_06_off_top.png"   "the viewport with the top row selected and the graph off again"
assert_exists "$SHOTS/graph_probe_20_wheel_on_a.png"    "the viewport with the graph on, before the wheel (control a)"
assert_exists "$SHOTS/graph_probe_21_wheel_on_b.png"    "the same state a second time, as the control"
assert_exists "$SHOTS/graph_probe_22_wheel_on_after.png" "the viewport after a wheel over it, with the graph on"
assert_exists "$SHOTS/graph_probe_23_wheel_off_a.png"    "the viewport with the graph off, before the wheel (control a)"
assert_exists "$SHOTS/graph_probe_24_wheel_off_b.png"    "the same state a second time, as the control"
assert_exists "$SHOTS/graph_probe_25_wheel_off_after.png" "the viewport after the same wheel, with the graph off"

# COMPARE NORMALIZED COPIES. `import` writes an image whose PNG type follows its content
# (a bland frame comes out Grayscale, a colourful one sRGB), and `compare` seeds its
# metric from the FIRST image's colorspace - measured on this machine, comparing a
# Grayscale shot against an sRGB one reported 0 differing pixels for images that differ
# by 1200. Forcing both sides to truecolor sRGB removes that trap. The copies live in a
# temp dir so $SHOTS holds only the captures; the diff image stays there for diagnosis.
NORM=$(mktemp -d)
norm() { magick "$1" -colorspace sRGB -type TrueColor PNG24:"$2"; }
norm "$SHOTS/graph_probe_01_off_empty.png" "$NORM/off_empty.png"
norm "$SHOTS/graph_probe_02_on_empty.png"  "$NORM/on_empty.png"
norm "$SHOTS/graph_probe_03_off_empty.png" "$NORM/off_empty2.png"
norm "$SHOTS/graph_probe_04_off_top.png"   "$NORM/off_top.png"
norm "$SHOTS/graph_probe_05_on_top.png"    "$NORM/on_top.png"
norm "$SHOTS/graph_probe_06_off_top.png"   "$NORM/off_top2.png"
norm "$SHOTS/graph_probe_20_wheel_on_a.png"     "$NORM/wheel_on_a.png"
norm "$SHOTS/graph_probe_21_wheel_on_b.png"     "$NORM/wheel_on_b.png"
norm "$SHOTS/graph_probe_22_wheel_on_after.png" "$NORM/wheel_on_after.png"
norm "$SHOTS/graph_probe_23_wheel_off_a.png"    "$NORM/wheel_off_a.png"
norm "$SHOTS/graph_probe_24_wheel_off_b.png"    "$NORM/wheel_off_b.png"
norm "$SHOTS/graph_probe_25_wheel_off_after.png" "$NORM/wheel_off_after.png"
echo "normalized shots and the diffs: $NORM"

# Differing pixels between two shots. No fuzz: both frames are the same renderer in the
# same state, and the OFF-vs-OFF measurements below are 0 differing pixels, so there is no
# antialiasing noise for a tolerance to absorb.
ae_diff() { # ae_diff <a.png> <b.png>
    magick compare -metric AE "$1" "$2" null: 2>&1 | awk '{print $1+0}'
}

# The viewport crop, applied to an already normalized shot. Its rectangle comes from the
# two docks and the frame (above); `crop` never touches the full frame again.
crop() { magick "$1" -crop "${CROP_W}x${CROP_H}+${FRAME_X}+${VIEW_TOP}" +repage -colorspace sRGB -type TrueColor PNG24:"$2"; }

# Fractions, as percentages with two decimals: the assertions below are about how much of
# the crop the panel covers, and an integer percent would hide the difference between the
# states this probe measures (5.72 vs 5.88 vs 7.15).
pct() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.2f", 100*a/b}'; }
ge() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a >= b)}'; }
le() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a <= b)}'; }

# CONTAINMENT, as its own test, because a `case` pattern without a trailing `*` matches the
# WHOLE string: the key-menu phase used `case "$line" in *" Lin")`, which requires the line to
# END with " Lin" - and "1 channel(s) shown, selected key: Rotate Linear" does not - so five
# of its assertions could not pass whatever the application did, and three of them were in the
# 23 FAILs of the run this phase was re-derived from. `has <string> <substring>`.
has() { case "${1:-}" in *"${2:-}"*) return 0 ;; *) return 1 ;; esac; }

# THE BARS FOR THE CROP, both measured on this build with real margin:
#   * FLOOR 2.00%: "something drew". The toggle's own 24x24 highlight is 568 px = 0.91% of
#     the crop, so a floor above that cannot be cleared by the button repainting itself,
#     and the smallest measured state (the empty panel, 2200 px = 3.51%) is nearly 2x it.
#   * CEILING 20.00%: "the panel is not a wash". It draws ticks, frame numbers, value
#     labels, a 1px playhead, the state line and the channel button - measured 3.72% with the top
#     row selected and 7.15% after a key was added. A panel that filled its background
#     would cover the viewport (204x146 = 29784 px = 47.5% of the crop) and fail this by
#     a factor of two; 20% is 2.8x above the largest measurement and far below a wash.
FLOOR_PCT=2
CEIL_PCT=20

# THE WHEEL'S TWO BARS (phase 5), both measured on this build and re-measured with it:
#   * WHEEL_PANEL_PIXELS: "the graph really zoomed". The panel's frame ticks and their
#     labels are laid out from the frames-per-pixel the wheel changed, so a 3-notch zoom
#     moves them; MEASURED (see the header's numbers) this is far above the bar. It must
#     also stay below what a CANVAS move produces, which is the other number: the same
#     3 notches on the artwork move nearly every pixel of the region, so a canvas zoom
#     cannot hide under this bar.
#   * WHEEL_CANVAS_PIXELS: "the canvas really zoomed". MEASURED on this build: with the
#     panel hidden the same 3-notch wheel moves 12842 px of the crop (about 20%), and the
#     controls in that state are exactly 0 - so the bar sits an order of magnitude under the
#     measurement and far above the controls.
WHEEL_PANEL_PIXELS=300
WHEEL_CANVAS_PIXELS=1000

# THE CURVE'S TWO BARS (phase 4b), the pixel half of the key-menu phase:
#   * SEGMENT_PIXELS: "the second key reached the drawn panel". MEASURED on this build:
#     9527 px = 15.21% of the crop, because a new key changes the shown-curve basis and the
#     value window is refitted to it - the change is the segment, the new key's square and
#     that re-layout together. The bar sits 9x under the measurement; the one-key state it is
#     measured against is a 0 px control (the same state twice).
#   * EASE_PIXELS: "the easing really re-shaped the drawn curve". This is the load-bearing
#     bar of the phase: with a curve on screen the panel draws NO summary line, so the
#     changed pixels are the polyline itself. NOT YET MEASURED - the recorded run that drove
#     this phase never reached the pick (its menu clicks were dropped by a detection bug since
#     fixed, and the re-derived Qt-menu phase was only just written), so this is the number to
#     replace from the first run whose state line reads Linear where the key carried Sine; the
#     run that produced it records the measurement beside the assertion. Its control is in
#     place regardless: the same easing picked twice must be EXACTLY 0.
SEGMENT_PIXELS=1000
EASE_PIXELS=100

# ---- (a) THE PANEL ON vs OFF: IT DREW, AND IT IS NOT A WASH ----
# MEASURED: 2332 px = 3.72% of the crop, against 0 for the OFF/OFF identity below. The
# toggle's 24x24 checked highlight is 568 px, all of it inside this crop and all of it
# part of this change; the rest is the panel.
ON_PIXELS=1000
d_on=$(ae_diff "$NORM/off_top.png" "$NORM/on_top.png")
[ "$d_on" -gt "$ON_PIXELS" ]; check $? "toggling the graph ON redraws the viewport ($d_on px changed on the whole screen, bar $ON_PIXELS)"
crop "$NORM/off_top.png" "$NORM/c_off_top.png"
crop "$NORM/on_top.png"  "$NORM/c_on_top.png"
d_on_crop=$(ae_diff "$NORM/c_off_top.png" "$NORM/c_on_top.png")
p_on=$(pct "$d_on_crop" "$CROP_AREA")
ge "$p_on" "$FLOOR_PCT"; check $? "the panel drew inside the crop ($d_on_crop px = ${p_on}% of ${CROP_W}x${CROP_H}, floor ${FLOOR_PCT}%)"
le "$p_on" "$CEIL_PCT";  check $? "the panel is not a wash: it covers only part of the crop (${p_on}% <= ceiling ${CEIL_PCT}%)"

# The bounding box of that change as magick sees it: the compare image is the source with
# the differing pixels marked, so its trim box is the region that moved.
magick compare -compose src "$NORM/off_top.png" "$NORM/on_top.png" "$NORM/diff_on.png" 2>/dev/null
BBOX=$(magick identify -format "%@" "$NORM/diff_on.png" 2>/dev/null)
BBOX_W=0; BBOX_H=0; BBOX_X=0; BBOX_Y=0
read -r BBOX_W BBOX_H BBOX_X BBOX_Y <<< "$(sed -E 's/^([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+)$/\1 \2 \3 \4/; s/\+//g' <<< "${BBOX:-0x0+0+0}")"
echo "changed region: ${BBOX_W}x${BBOX_H}+${BBOX_X}+${BBOX_Y} (${BBOX:-unreadable}); crop is x $FRAME_X..$((FRAME_X + CROP_W - 1)), y $VIEW_TOP..$((VIEW_TOP + CROP_H - 1))"

[ "$BBOX_W" -gt 0 ] && [ "$BBOX_H" -gt 0 ]
check $? "the changed region is not empty (${BBOX_W}x${BBOX_H})"

# MEASURED bbox 370x143+8+39 - x 8..377 and y 39..181. The left edge x=8 is the highlight
# of the toggle itself, which is part of the same change, so this probe asserts the two
# edges that are the VIEWPORT's own bounds: the Properties dock is right-docked, so its
# left edge is where the viewport ends (measured: the panel's rightmost changed column is
# 377 against a dock at x=382), and the Animation panel is docked under the central
# widget, so its top edge is where the viewport ends (measured: the bottom of the change
# is 181 against a panel at y=185). An overlay that escaped the viewport would fail one of
# them - which is the whole claim "it draws over the viewport, not over a dock".
[ $((BBOX_X + BBOX_W)) -le "$PROP_X" ]
check $? "the change ends left of the Properties dock (right $((BBOX_X + BBOX_W)) <= dock left $PROP_X)"

[ $((BBOX_Y + BBOX_H)) -le "$ANIM_Y" ]
check $? "the change ends above the Animation panel (bottom $((BBOX_Y + BBOX_H)) <= panel top $ANIM_Y)"

# ...and inside the crop, which is what makes the fraction above a measurement of the same
# region the bbox describes.
[ "$BBOX_X" -ge "$FRAME_X" ] && [ "$BBOX_Y" -ge "$VIEW_TOP" ]
check $? "the change is inside the crop (bbox starts at ${BBOX_X}+${BBOX_Y}, crop at ${FRAME_X}+${VIEW_TOP})"

# MEASURED: the two OFF shots are identical (0 px), so this bar is not a tolerance on a
# jittery number - it only has to keep the effect of the second toggle from being
# mistaken for the first. A quarter of the ON change is far below it either way.
d_off=$(ae_diff "$NORM/off_top.png" "$NORM/off_top2.png")
[ $((d_off * 4)) -le "$d_on" ]; check $? "toggling it OFF restores the pixels (off-vs-off $d_off px against off-vs-on $d_on px)"

# ---- (f) PHASE 5: THE WHEEL, MEASURED IN THE TWO REGIONS THE MODEL'S OWN RECTS BOUND ----
# THE REGIONS, STATED ONCE AND USED BY EVERY ASSERTION BELOW. The PANEL region is the
# bounding box of the OFF->ON change measured just above - the panel's own ink, and only
# that - dilated by 2 px so the contrast outline's 1 px rings cannot straddle its edge.
# The CANVAS region is the crop WITHOUT that box: the strip of the viewport the panel
# never paints in. Both are derived from this run's own pixels and from the docks, never
# from a constant; the graph-hidden case below is the canvas region's own control, since
# the same pixels must move when the wheel reaches the canvas.
BOX_X=$((BBOX_X - FRAME_X - 2)); BOX_Y=$((BBOX_Y - VIEW_TOP - 2))
BOX_W=$((BBOX_W + 4));          BOX_H=$((BBOX_H + 4))
CANVAS_AREA=$((CROP_W * CROP_H - BOX_W * BOX_H))
echo "wheel regions: the panel's ink box is ${BOX_W}x${BOX_H}+${BOX_X}+${BOX_Y} in the crop (from the OFF/ON change ${BBOX_X}+${BBOX_Y} dilated 2 px); the canvas region is the rest of the ${CROP_W}x${CROP_H} crop = $CANVAS_AREA px"
# A region that is not a region is not a control: an ink box that swallowed the crop
# would make "the canvas is unchanged" true by having nothing left to check.
[ "$BOX_W" -gt 0 ] && [ "$BOX_H" -gt 0 ] && [ "$CANVAS_AREA" -gt 2000 ]
check $? "the two wheel regions are both real (ink box ${BOX_W}x${BOX_H}, canvas $CANVAS_AREA px of the ${CROP_AREA} px crop)"

# Mask the panel's ink box out of a crop, so what remains is exactly the canvas region.
mask_canvas() { # mask_canvas <in> <out>
    magick "$1" -fill '#000000' \
        -draw "rectangle ${BOX_X},${BOX_Y} $((BOX_X + BOX_W - 1)),$((BOX_Y + BOX_H - 1))" \
        -colorspace sRGB -type TrueColor PNG24:"$2"
}

crop "$NORM/wheel_on_a.png"     "$NORM/c_wheel_on_a.png"
crop "$NORM/wheel_on_b.png"     "$NORM/c_wheel_on_b.png"
crop "$NORM/wheel_on_after.png" "$NORM/c_wheel_on_after.png"
crop "$NORM/wheel_off_a.png"    "$NORM/c_wheel_off_a.png"
crop "$NORM/wheel_off_b.png"    "$NORM/c_wheel_off_b.png"
crop "$NORM/wheel_off_after.png" "$NORM/c_wheel_off_after.png"
mask_canvas "$NORM/c_wheel_on_b.png"     "$NORM/m_wheel_on_b.png"
mask_canvas "$NORM/c_wheel_on_after.png" "$NORM/m_wheel_on_after.png"
mask_canvas "$NORM/c_wheel_off_b.png"    "$NORM/m_wheel_off_b.png"
mask_canvas "$NORM/c_wheel_off_after.png" "$NORM/m_wheel_off_after.png"

# THE CONTROLS FIRST, in both visibility states: two captures of the same state, so
# neither change below can be the panel or the canvas repainting on its own.
d_wheel_on_ctl=$(ae_diff "$NORM/wheel_on_a.png" "$NORM/wheel_on_b.png")
[ "$d_wheel_on_ctl" -eq 0 ]
check $? "the panel does not repaint on its own before the wheel (control $d_wheel_on_ctl px, graph visible)"
d_wheel_off_ctl=$(ae_diff "$NORM/wheel_off_a.png" "$NORM/wheel_off_b.png")
[ "$d_wheel_off_ctl" -eq 0 ]
check $? "nor does the canvas (control $d_wheel_off_ctl px, graph hidden)"

# THE GRAPH'S WHEEL: THE PANEL MOVED, THE CANVAS DID NOT. The panel's frame ticks and
# their labels are laid out from the frames-per-pixel the wheel changed, so they move;
# every pixel OUTSIDE the panel's ink box is byte-identical, which is the whole claim
# "the wheel zoomed the graph and left the artwork alone".
d_wheel_panel=$(ae_diff "$NORM/c_wheel_on_b.png" "$NORM/c_wheel_on_after.png")
d_wheel_canvas=$(ae_diff "$NORM/m_wheel_on_b.png" "$NORM/m_wheel_on_after.png")
p_wheel_panel=$(pct "$d_wheel_panel" "$CROP_AREA")
[ "$d_wheel_panel" -gt "$WHEEL_PANEL_PIXELS" ]
check $? "with the graph visible a wheel over the viewport moves the panel alone ($d_wheel_panel px = ${p_wheel_panel}% of the crop changed, bar $WHEEL_PANEL_PIXELS)"
[ "$d_wheel_canvas" -eq 0 ]
check $? "and the canvas is byte-identical: all $CANVAS_AREA px outside the panel's ink box are unchanged ($d_wheel_canvas px changed; the graph owns the wheel while it is visible, Driver.cpp:215)"

# AND THE OTHER DIRECTION, which is also this region's control: with the graph hidden the
# same wheel at the same point reaches the canvas mover, and the artwork scales - over the
# WHOLE CROP, and the reason is measured: the canvas region
# defined above is the crop minus the panel's ink box, i.e. the crop's PERIPHERY (a 16 px band
# at the top and a few columns at the sides) - and the artwork can be letterboxed away from the
# periphery, in which case a canvas zoom does not touch it (MEASURED on this build: 0 px
# there, while the same wheel moved 12842 px inside the box). With the panel hidden nothing is
# painted over the crop at all, so the crop IS the canvas (plus the Tools dock, which a wheel
# over the viewport cannot reach), and that is what is compared.
d_wheel_hidden=$(ae_diff "$NORM/c_wheel_off_b.png" "$NORM/c_wheel_off_after.png")
p_wheel_hidden=$(pct "$d_wheel_hidden" "$CROP_AREA")
magick compare -compose src "$NORM/c_wheel_off_b.png" "$NORM/c_wheel_off_after.png" "$NORM/diff_wheel_off.png" 2>/dev/null
WHEEL_OFF_BBOX=$(magick identify -format "%@" "$NORM/diff_wheel_off.png" 2>/dev/null)
[ "$d_wheel_hidden" -gt "$WHEEL_CANVAS_PIXELS" ]
check $? "with the graph hidden the same wheel scales the artwork: the crop moves ($d_wheel_hidden px = ${p_wheel_hidden}% of it, bar $WHEEL_CANVAS_PIXELS, changed region [${WHEEL_OFF_BBOX:-unreadable}] inside the viewport - and the whole crop is canvas while the panel paints nothing)"
[ -z "${WHEEL_HIDDEN_FIT:-}" ]
check $? "and the panel really is off while it does: it publishes no fit action at all (rect ${WHEEL_HIDDEN_FIT:-none}), so that change is the canvas's and not the panel's"

# ---- (f2) THE RING CAPTURE: A FILE TO LOOK AT, AND NOTHING MORE ----
# The two assertions are the only honest ones for a capture that exists to be SEEN: the file
# is there, and it is not a black frame (which would mean the app died during the zoom). The
# brightness of the artwork under the panel's ink is recorded in the geometry file beside it
# (RING_BRIGHT/RING_INK_BOX) so a reader can tell whether the frame had anything to show; the
# judgement of whether the ring READS at this zoom is the reader's, not this probe's.
echo "ring capture: anchor [${RING_POINT:-none}], ${RING_NOTCHES:-?} notches of canvas zoom in and the same out; the panel's ink box over the zoomed artwork [${RING_INK_BOX:-none}], its mean luminance [${RING_BRIGHT:-?}]"
assert_exists "$SHOTS/graph_probe_31_ring_over_art.png" "the capture of the panel over the zoomed, bright artwork"
assert_rendered "$SHOTS/graph_probe_31_ring_over_art.png" "the same capture (not a black frame: the zoom and the panel both rendered)"

# ---- (c) THE EMPTY STATE: NO SELECTION, SO THE PANEL SAYS WHY AND DRAWS THE TEXT ----
# MEASURED: with nothing selected the panel publishes the model's own sentence
# ("Select a layer to show its curves", GraphPanelModel's emptyState) as an accessible
# label, and NOT the "No keys on the selected layers" one - that is what the top row
# selection produces below. Its change inside the crop is 2200 px = 3.51%, text and axes
# only: the smallest of the states this probe measures. Both are asserted, because the
# sentence IS the state the model is in: the panel could draw the right pixels and say the
# wrong thing.
echo "empty state: line [$EMPTY_LINE], 'no keys' line [$NOKEYS_LINE], top-row line later [$TOP_LINE]"
[ -n "${EMPTY_LINE:-}" ]
check $? "the empty state line names the empty state (the model's own sentence is the accessible label, rect ${EMPTY_LINE:-none})"
[ -z "${NOKEYS_LINE:-}" ]
check $? "and not the no-keys sentence, which belongs to a selected layer (rect ${NOKEYS_LINE:-none})"
[ -n "${TOP_LINE:-}" ]
check $? "the top row selected later publishes that no-keys sentence, i.e. the selection IS a target whose channels have no key (rect ${TOP_LINE:-none})"

crop "$NORM/off_empty.png" "$NORM/c_off_empty.png"
crop "$NORM/on_empty.png"  "$NORM/c_on_empty.png"
d_empty_crop=$(ae_diff "$NORM/c_off_empty.png" "$NORM/c_on_empty.png")
p_empty=$(pct "$d_empty_crop" "$CROP_AREA")
ge "$p_empty" "$FLOOR_PCT"; check $? "the empty panel still drew its state line ($d_empty_crop px = ${p_empty}% of the crop, floor ${FLOOR_PCT}%)"
le "$p_empty" "$CEIL_PCT";  check $? "the empty state is text only, not a wash (${p_empty}% <= ceiling ${CEIL_PCT}%)"
d_off_empty=$(ae_diff "$NORM/off_empty.png" "$NORM/off_empty2.png")
[ "$d_off_empty" -eq 0 ]; check $? "the two OFF shots of the empty state are identical (control $d_off_empty px)"

# ---- (d) PHASE 2, HARDENED: THE KNOB ROW AND THE ANGLE VALUE, BEFORE AND AFTER ----
# WHAT THE PROBE PRESSED, AND WHAT THE DOCK DID ABOUT IT. A key that no knob was ever
# shown for cannot have been added, so the press is attributed BEFORE the pixels are. The
# assertions are the TRANSITION, not the after-state: the knob row exists and the channel
# rows (Angle) do not, then the collapse lifts the knob row into the dock's viewport,
# then the press turns the row into the channel rows and the Angle field becomes readable.
# MEASURED on the default project: knob row 414 394 36 21 (below the fold) -> 414 194 36 21
# after collapsing Constants and Default Keys, Angle absent before and 382 245 82 23 after,
# and its value reads 0. The test then asserts that value is a NUMBER, not the label text.
#
# WHICH ROW GOT SELECTED is deliberately NOT asserted, because the tree cannot answer it
# unambiguously: MEASURED, the object tree publishes its rows as `table cell` nodes named
# after the objects (top `running thru my mind`, then `Folder16`, then `Layer1`), the probe
# activates them one by one until the knob appears, and the PANEL then names the channel that
# received the key - its line carries a "focused:" half once the channel list has been hovered,
# which is the model's own name for it ("Folder16 : Rotate"), and which need not be the cell
# text the probe activated (a cell click that selects nothing leaves the previous selection in
# place, and nothing in the accessibility tree reports which row is current).
# What IS asserted is the panel's own state: the knob row exists, the channel rows do not,
# the collapse lifts the row, and the press turns one into the other.
echo "phase 2: last row activated [${KEY_ROW:-none}], knob row before [${KNOB_BEFORE:-none}], knob row after the collapse [${KNOB:-none}], Angle before [${ANGLE_BEFORE:-none}], Angle after [${ANGLE:-none}] = ${ANGLE_VALUE:-unreadable}"
[ -n "${KNOB_BEFORE:-}" ]
check $? "the Current Keys add-key knob is addressable for the layer the tree had selected (last row activated: ${KEY_ROW:-none}; rect ${KNOB_BEFORE:-none})"
[ -z "${ANGLE_BEFORE:-}" ]
check $? "the channel rows are NOT there before the press (Angle rect ${ANGLE_BEFORE:-none}; prop::KeyKnocker stands in for them while the frame has no key)"
[ -z "${ANGLE_VALUE_BEFORE:-}" ]
check $? "and the Angle value is not readable before the press either (${ANGLE_VALUE_BEFORE:-empty}: the field is hidden with its group)"
KNOB_BEFORE_Y=$(printf '%s' "${KNOB_BEFORE:-0 0 0 0}" | cut -d' ' -f2)
read -r KX KY KW KH <<< "${KNOB:-0 0 0 0}"
[ -n "${KNOB:-}" ] && [ "$KY" -lt "${KNOB_BEFORE_Y:-0}" ]
check $? "collapsing the panels above lifted the knob row into the dock viewport (y ${KNOB_BEFORE_Y:-none} -> ${KY:-none})"
[ -n "${ANGLE:-}" ]
check $? "the press turned the knob row into the channel rows, i.e. the key now exists (Angle row ${ANGLE:-none})"
[ -n "${ANGLE_VALUE:-}" ]
check $? "and the Angle field answers with a value, read from its Value interface (${ANGLE_VALUE:-unreadable})"
case "${ANGLE_VALUE:-}" in
    ''|*[!0-9.eE+-]*) ANGLE_NUM=1 ;;
    *)                ANGLE_NUM=0 ;;
esac
check $ANGLE_NUM "that value is a number and not a label or a placeholder (${ANGLE_VALUE:-unreadable})"

assert_exists "$SHOTS/graph_probe_07_on_control.png" "the viewport with the keyed layer selected, before the press"
assert_exists "$SHOTS/graph_probe_08_on_control.png" "the same state a second time, as the control"
assert_exists "$SHOTS/graph_probe_09_after_key.png"  "the viewport after a key was added to the selected layer"
norm "$SHOTS/graph_probe_07_on_control.png" "$NORM/on7.png"
norm "$SHOTS/graph_probe_08_on_control.png" "$NORM/on8.png"
norm "$SHOTS/graph_probe_09_after_key.png"  "$NORM/on9.png"

# THE CONTROL FIRST. Two captures of the same state: if this drifts there is no
# measurement below, only a tolerance. MEASURED: 0 px.
d_control=$(ae_diff "$NORM/on7.png" "$NORM/on8.png")
[ "$d_control" -eq 0 ]; check $? "the panel does not repaint on its own (control $d_control px between two identical states)"

# THE EFFECT, MEASURED WHERE THE PLOT IS. The press also rewrites the dock row it was
# made on (the knob row becomes the channel rows) and touches the document (the
# Animation panel lane gains the key, the project tab gains the modified marker), so
# MEASURED the whole-screen change is 12807 px - of which the crop holds 2162. A
# whole-screen region therefore CANNOT be confined to the viewport once a real key is
# added, and the plot is measured in the crop itself (which also holds the Tools dock,
# whose pixels the key push leaves identical in both captures: see the x-profile
# measured for the header - 0 px changed at x 32..176).
crop "$NORM/on7.png" "$NORM/c_on7.png"
crop "$NORM/on9.png" "$NORM/c_on9.png"
CURVE_PIXELS=200
d_key=$(ae_diff "$NORM/c_on7.png" "$NORM/c_on9.png")
p_key=$(pct "$d_key" "$CROP_AREA")
[ "$d_key" -gt "$CURVE_PIXELS" ]; check $? "adding a key to the selected layer repaints the plot ($d_key px changed inside the crop = ${p_key}%, bar $CURVE_PIXELS)"
le "$p_key" "$CEIL_PCT"; check $? "and adding a key is still not a wash (${p_key}% <= ceiling ${CEIL_PCT}%)"

magick compare -compose src "$NORM/on7.png" "$NORM/on9.png" "$NORM/diff_key_whole.png" 2>/dev/null
WHOLE_BBOX=$(magick identify -format "%@" "$NORM/diff_key_whole.png" 2>/dev/null)
echo "changed region after the key: whole screen ${WHOLE_BBOX:-unreadable} (the dock and the timeline lane the key was added in), of which $d_key px are inside the crop"

# ---- (b) THE CHANNEL LIST, DRIVEN THROUGH THE PANEL'S OWN CONTROL ----
# The panel used to publish one check box per legend row. It publishes ONE PushButton now,
# and this phase's claims are made on the surface that replaced it: the CONTROL (published
# only while there is a channel to list, and named by the count it changes), the LIST behind
# it (the application's own QMenu - opened through the control's own action, found by its
# window, row found by hovering it), the model's own line for the focus, and the pixels for
# the ink. What each old assertion became is named here rather than left to be guessed:
#   "no legend row before the press"        -> no channel control at all before the press
#   "exactly one legend row after it"       -> the control is published and its own name
#                                              counts exactly one channel ("1 channel(s)")
#   "that row is named by its channel"      -> the panel's line names the channel the hovered
#                                              row focuses ("..., focused: <layer> : Rotate")
#   "addressable with its own rectangle"    -> the control's published rectangle, plus the
#                                              list's own row rectangle measured from pixels
#   "the check box state flips"             -> the control's name moves to "0 of 1" and the
#                                              model's all-hidden sentence appears
# (a per-row CHECK BOX count has no equivalent to keep: there is one button now, and the
#  number of channels it counts is what its name states.)
CHAN_MENU_ID=""; CHAN_MENU_W=0; CHAN_MENU_H=0; CHAN_MENU_X=0; CHAN_MENU_Y=0
read -r CHAN_MENU_ID CHAN_MENU_W CHAN_MENU_H CHAN_MENU_X CHAN_MENU_Y <<< "${CHAN_MENU:-0 0 0 0 0}"
echo "channel list: the control before the key [${CHANNELS_BEFORE:-none}] and after it [${CHANNELS_AFTER:-none}] (names it by its own count); the list window [${CHAN_MENU:-none}] (rows walked ${CHAN_ROWS:-none}); the channel row [${CHAN_ROW:-none}]; after hovering it the panel's line [${CHAN_HOVER_LINE:-none}]; after the activation the control [${CHAN_BTN_HIDDEN:-none}] and the sentence [${ALLHIDDEN:-none}]; after the second opening [${CHAN_MENU_AFTER:-none}], the control [${CHAN_BTN_SHOWN:-none}] and the sentence [${ALLHIDDEN_BACK:-none}]"

[ -z "${CHANNELS_BEFORE:-}" ]
check $? "the panel published NO channel control before any channel had a key (a push button named '1 channel(s)' answers [${CHANNELS_BEFORE:-none}]; the model publishes the control only while there is a channel to list)"
[ -n "${CHANNELS_AFTER:-}" ]
check $? "and the phase-2 key makes the panel publish it, with its own name counting exactly one channel ([${CHANNELS_AFTER:-none}] is the rectangle of a push button NAMED '1 channel(s)' - the name was the query, so the count is the assertion)"
[ -n "${CHAN_MENU_ID:-}" ]
check $? "pressing that control through its own accessibility action opened the application's channel list as a window of its own ([${CHAN_MENU:-none}]; a native QMenu, not accessible items - MEASURED, no row of it is in the tree)"
read -r CBX CBY CBW CBH <<< "${CHANNELS_AFTER:-0 0 0 0}"
[ "${CHAN_MENU_X:-0}" -ge $((CBX - 4)) ] && [ "${CHAN_MENU_X:-0}" -le $((CBX + 4)) ] \
    && [ "${CHAN_MENU_Y:-0}" -ge $((CBY - 40)) ] && [ "${CHAN_MENU_Y:-0}" -le $((CBY + 40)) ]
check $? "and the application put it at the control it belongs to (list origin ${CHAN_MENU_X:-none},${CHAN_MENU_Y:-none} against the button at [${CHANNELS_AFTER:-none}]; showGraphChannelMenu pops it at the box's own top-left)"
[ -n "${CHAN_ROW:-}" ]
check $? "and walking the list with the pointer found its CHANNEL row - the first row that lights a rectangle inside the list's own window is the channel, because the layer heading above it is a disabled action (rows walked: ${CHAN_ROWS:-none}; the channel row is [${CHAN_ROW:-none}] inside [${CHAN_MENU:-none}])"
has "${CHAN_HOVER_LINE:-}" "focused:"
check $? "and HOVERING that row focuses the channel it names, which the panel's own line then says: [${CHAN_HOVER_LINE:-none}]"
case "${CHAN_HOVER_LINE:-}" in
    *": Rotate"*) CHAN_FOCUS_OK=0 ;;
    *)            CHAN_FOCUS_OK=1 ;;
esac
check $CHAN_FOCUS_OK "and it is the channel the probe keyed, named the way the list names it (line [${CHAN_HOVER_LINE:-none}])"
[ -n "${CHAN_BTN_HIDDEN:-}" ]
check $? "activating the row toggles that channel's visibility, and the panel's own control says so: the button now answers to '0 of 1' ([${CHAN_BTN_HIDDEN:-none}] - the hidden count, which is the state the control changes)"
[ -n "${CHAN_BTN_SHOWN:-}" ]
check $? "and activating it again at a fresh opening of the list moves the control back to '1 channel(s)' ([${CHAN_BTN_SHOWN:-none}])"
[ -n "${CHAN_MENU_AFTER:-}" ] && [ "${CHAN_MENU_AFTER:-}" = "${CHAN_MENU:-}" ]
check $? "and the second opening landed at the same place and size as the first ([${CHAN_MENU_AFTER:-none}] against [${CHAN_MENU:-none}]), which is what makes the row rectangle above usable for its activation"

assert_exists "$SHOTS/graph_probe_10_legend_hidden.png" "the viewport with that channel hidden from the list"
assert_exists "$SHOTS/graph_probe_11_legend_shown.png"  "the viewport with it shown again"
norm "$SHOTS/graph_probe_10_legend_hidden.png" "$NORM/on10.png"
norm "$SHOTS/graph_probe_11_legend_shown.png"  "$NORM/on11.png"
crop "$NORM/on10.png" "$NORM/c_on10.png"
crop "$NORM/on11.png" "$NORM/c_on11.png"

# THE CURVE'S PIXELS GO, AND THEY COME BACK EXACTLY. MEASURED: hiding the channel changes
# 2078 px, ALL inside the crop (the whole screen changes the same 2078 - the panel is the
# only thing that reacts), and showing it again returns the crop to its earlier state
# byte-for-byte (0 px against the capture taken before the toggle). The disappearing
# pixels are the curve the model stopped producing: the model drops a hidden channel from
# its curves, and hiding the only keyed channel is also why the panel then says so in
# words (the "All channels ..." sentence asserted next).
d_hidden=$(ae_diff "$NORM/c_on9.png" "$NORM/c_on10.png")
d_hidden_whole=$(ae_diff "$NORM/on9.png" "$NORM/on10.png")
p_hidden=$(pct "$d_hidden" "$CROP_AREA")
[ "$d_hidden" -gt "$CURVE_PIXELS" ]
check $? "hiding that channel from the panel's list removes its curve from the crop ($d_hidden px = ${p_hidden}% gone, bar $CURVE_PIXELS)"
[ "$d_hidden_whole" -eq "$d_hidden" ]
check $? "and nothing changed outside the crop ($d_hidden_whole px whole screen vs $d_hidden inside)"

[ -n "${ALLHIDDEN:-}" ]
check $? "with the only keyed channel hidden the panel says so in the model's own words (rect ${ALLHIDDEN:-none})"

d_back=$(ae_diff "$NORM/c_on9.png" "$NORM/c_on11.png")
[ "$d_back" -eq 0 ]
check $? "and activating the row again restores the pixels exactly ($d_back px against the pre-toggle capture)"
[ -z "${ALLHIDDEN_BACK:-}" ]
check $? "and the all-hidden sentence is gone again (rect ${ALLHIDDEN_BACK:-none})"

# ---- (d) PHASE 4: THE KEY CONTEXT MENU, WHICH IS THE EASING PICKER ----
# WHAT IS ASSERTED, AND WHERE EACH PART COMES FROM. The route first: the panel's own fit
# action gives the plot's rectangle; the phase clears the panel's selection with the graph's
# own deselect gesture; the scan finds the key with right-clicks along the fit rect's centre
# row, and a sample is accepted only when its click PUT THE PICKER'S OWN WINDOW ON SCREEN -
# a viewable top-level that answers the picker's own geometry (its width and one of its two
# pinned heights, read out of its source), which is the identification this control has now
# that its window outlives a close (see the phase comment in the body). The right-click that
# opened it SELECTED the key it landed on, which the panel's own state line says by moving
# its selected-key half from the empty reading; the LEFT press on the same point is the
# editor's own gesture and is asserted the same way; the popup's own rectangle is the X
# server's answer about that window; and the state line then names the easing the tile the
# phase pressed carries, because it is built from the DOCUMENT. The popup is driven through
# its own window and its own layout constants - the tile is pressed with the pointer at the
# point the source puts it, and Escape (Qt's cancel key for a Qt::Popup, and the dismissal
# that COMMITS the pick) closes it - because a tile's QToolButton has no text and therefore
# no accessible name to address it by. What is NOT asserted: that the key is at the fit rect
# centre. The value axis is fitted and the FRAME axis is the fit's own business (the frame
# half of fitAll is what the user asked to change this round), so the key sits where the fit
# puts it, and the old centre test was measuring the probe's own assumption rather than the
# panel. WHAT IS DROPPED with the deleted QMenu, and named here rather than left implicit:
# the menu's row map and its entry count, and the Down-arrow highlight - a popover has no
# rows. The modality pair, the selection behaviour and the state-line readback all remain.
echo "phase 4: the panel's fit rect [${MENU_FIT:-none}], its centre [${MENU_DERIVED:-none}], the point the right-click scan found [${MENU_KEY:-none}] (offset [${MENU_OFFSET:-none}], ${MENU_SAMPLES:-?} sample(s) of the lattice [${MENU_SCAN_LATTICE:-none}], [${MENU_SCAN_NOTE:-no hit}], every sample as point=picker-window [${MENU_SCAN_TRACE:-none}], picker windows on screen before the scan [${MENU_PICK_BASE:-none}]); the selection before the phase [${MENU_PRE_SCAN:-empty}], after the DESELECT at [${MENU_DESELECT:-none}] [${MENU_SEL_BEFORE:-empty}], after the right-click that opened the popover [${MENU_SEL_AFTER:-empty}], after the second deselect [${MENU_SEL_BEFORE2:-empty}] and after the LEFT press [${MENU_SEL_AFTER2:-empty}]; the popover window [${MENU_WIN:-none}] [${MENU_GEOM:-none}] (${MENU_PICKER_COUNT:-?} window(s) on screen answering the picker's own ${MENU_PICK_W:-?}x${MENU_PICK_H:-?} geometry read from its source; the window itself reports [${MENU_WIN_INFO:-none}] and the recorded WM type [${MENU_WIN_TYPE:-none}]; picker windows on screen: ${MENU_PICKER_COUNT:-?}); the picker's catalog is ${MENU_TILES:-?} tiles (the pick presses ${MENU_TILE_TYPE:-?} at index ${MENU_TILE_IDX:-none}, the second pick ${MENU_TILE_KB_TYPE:-?} at index ${MENU_TILE_KB_IDX:-none}); the state line before the popover [${MENU_BEFORE:-none}]; with the popover OPEN: Delete -> [${MENU_DEL_LINE:-none}] (window open [${MENU_DEL_OPEN:-none}]), Undo -> [${MENU_UNDO_LINE:-none}] (window open [${MENU_UNDO_OPEN:-none}]), Escape -> [${MENU_ESCAPE_LINE:-none}] (window open [${MENU_ESC_OPEN:-none}]); the POINTER pick of the ${MENU_ENTRY_TILE:-none} at [${MENU_ENTRY_PT:-none}] -> [${MENU_ENTRY:-none}], the line [${MENU_AFTER:-none}], the popover closed [${MENU_ENTRY_CLOSED:-none}]; the second open [${MENU_GEOM2:-none}]; the opening tries [${MENU_TRIES:-none}]; the dock's easing rows before [${MENU_DOCK_BEFORE:-none}] after [${MENU_DOCK_AFTER:-none}]; a right-click on empty plot space at [${MENU_EMPTY:-none}] left on screen [${MENU_EMPTY_WINS:-none}] and left the line [${MENU_NOKEY_LINE:-none}]"
assert_exists "$SHOTS/graph_probe_12_menu_control_a.png" "the viewport with the keyed layer's curve and the key selected by the press, no menu"
assert_exists "$SHOTS/graph_probe_13_menu_control_b.png" "the same state a second time, as the control"
assert_exists "$SHOTS/graph_probe_14_menu_open.png" "the root window with the application's easing picker open on the key"
assert_exists "$SHOTS/graph_probe_15_menu_after.png" "the viewport after pressing a tile of the picker"
assert_exists "$SHOTS/graph_probe_16_menu_nokey.png" "the viewport after a right-click on empty plot space"
assert_exists "$SHOTS/graph_probe_41_key_scan.png" "the root at the point the right-click scan found the key, with the picker its click opened"
norm "$SHOTS/graph_probe_12_menu_control_a.png" "$NORM/menu_ctl_a.png"
norm "$SHOTS/graph_probe_13_menu_control_b.png" "$NORM/menu_ctl_b.png"
norm "$SHOTS/graph_probe_14_menu_open.png" "$NORM/menu_open.png"
norm "$SHOTS/graph_probe_15_menu_after.png" "$NORM/menu_after.png"
norm "$SHOTS/graph_probe_16_menu_nokey.png" "$NORM/menu_nokey.png"

# A SCAN THAT CANNOT FIND ITS SUBJECT SAYS SO FIRST, and with its own evidence, because every
# assertion below is about a point it produces: with no point, they are all cascades, and a
# cascade hides which one was the first to be false. The trace is every sample as
# point=picker-window - the witness this phase accepts on is the popup's own X window and what
# it publishes about itself, not a pixel count and not "a window that did not exist before" -
# so a red run reads the lattice it tried rather than an empty MENU_KEY. The phase does NOT
# stop here: phases 4b and 3 still run and report their own state.
[ -n "${MENU_KEY:-}" ]
check $? "the right-click scan found a point where the EASING PICKER'S POPOVER came on screen (found [${MENU_KEY:-none}] after ${MENU_SAMPLES:-?} sample(s) of the lattice [${MENU_SCAN_LATTICE:-none}], with no picker window up before the scan [${MENU_PICK_BASE:-none}]; every sample, as point=picker-window: [${MENU_SCAN_TRACE:-no samples taken}])"

read -r MKX MKY <<< "${MENU_KEY:-0 0}"
read -r MFX MFY MFW MFH <<< "${MENU_FIT:-0 0 0 0}"
read -r MWW MWH MWX MWY <<< "${MENU_GEOM:-0 0 0 0}"

# THE PICKER'S OWN NUMBERS ARE THE PROBE'S FIRST CLAIM ABOUT ITSELF: the window the phase
# accepts as the popover has to be exactly as wide and as tall as the widget pins itself to,
# and those numbers are read out of the source rather than remembered. A source change that
# outdates them is a red assertion here rather than a scan that quietly finds nothing.
[ -n "${MENU_PICK_W:-}" ] && [ "${MENU_PICK_W:-0}" -gt 0 ] && [ -n "${MENU_PICK_H:-}" ] && [ "${MENU_PICK_H:-0}" -gt 0 ]
check $? "the picker's own geometry was READ OUT OF ITS SOURCE (width [${MENU_PICK_W:-none}], normal height [${MENU_PICK_H:-none}], custom height [${MENU_PICK_H_CUSTOM:-none}], from src/gui/prop/EasingPickWidget.{h,cpp}) - the window accepted below must have exactly those numbers, so a change to the widget is a red assertion and not a scan that finds nothing"
# THE POPOVER IS THE WINDOW THAT ANSWERS THE PICKER'S OWN GEOMETRY, and the phase asserts
# that exactly ONE window did, that its width and height are the picker's own, and that the
# application put its origin at the click point (popUpAt moves it to the anchor, so the click
# point is the window's top-left; MEASURED on this build within 2 px).
[ -n "${MENU_WIN:-}" ] && [ "${MENU_PICKER_COUNT:-0}" -eq 1 ] && [ "${MWW:-0}" -eq "${MENU_PICK_W:-0}" ] && { [ "${MWH:-0}" -eq "${MENU_PICK_H:-0}" ] || [ "${MWH:-0}" -eq "${MENU_PICK_H_CUSTOM:-0}" ]; }
check $? "the right-click put exactly ONE window on screen that answers the picker's own geometry (window [${MENU_WIN:-none}] [${MENU_GEOM:-none}], ${MENU_PICKER_COUNT:-?} such window(s) - the width and both heights are the widget's own source constants, so a menu-shaped rectangle of another size is NOT accepted here)"
# AND IT IS A Qt::Popup BY WHAT THE X SERVER SAYS, not by assumption: the phase reads the
# window's own report - viewable, and OVERRIDE-REDIRECT, which is how Qt creates a Qt::Popup
# (X11BypassWindowManagerHint) and is not how the application's normal window is created.
win_says() { case "${MENU_WIN_INFO:-}" in *"$1"*) return 0 ;; *) return 1 ;; esac; }
win_says "Map State: IsViewable" && win_says "Override Redirect State: yes"
check $? "and the window itself reports what a Qt::Popup is (the picker's own window answered [${MENU_WIN_INFO:-none}]): on screen, and OVERRIDE REDIRECT - unmanaged, which is the Qt::Popup flag and not the application window's shape"
echo "phase 4: RECORDED, NOT ASSERTED - the popover window's _NET_WM_WINDOW_TYPE is [${MENU_WIN_TYPE:-none}]. Qt's automatic selection covers Tool, ToolTip, Dialog and Splash but not Popup, so a Qt::Popup carries no popup atom here and the type is evidence only of what Qt does put there."
[ "${MWX:-0}" -ge $((MKX - 2)) ] && [ "${MWX:-0}" -le $((MKX + 2)) ] && [ "${MWY:-0}" -ge $((MKY - 2)) ] && [ "${MWY:-0}" -le $((MKY + 2)) ]
check $? "and the application put it AT THE CLICK POINT (window origin ${MWX:-none},${MWY:-none} against the point [${MENU_KEY:-none}])"
# THE SELECTION, BOTH HALVES, each from a reading the phase made EMPTY first with the graph's
# own deselect gesture (a Ctrl-press in the plot on no key and no band, which the graph OWNS,
# so it reaches no viewport tool and changes no document byte). The right-click that opened
# the popover selects the key it lands on - the overlay's openKeyMenuAt calls selectKey before
# it hands the selection on - and the press that follows is the editor's own gesture.
# Asserting the empty reading in each case is what makes "it moved" a statement about the
# gesture rather than about the scan.
[ -n "${MENU_STATE_BEFORE:-}" ] && [ -z "${MENU_SEL_BEFORE:-}" ]
check $? "the selection was cleared before the scan, by the graph's own Ctrl-press gesture in the plot at [${MENU_DESELECT:-none}]: the panel's state line [${MENU_STATE_BEFORE:-none}] carries NO selected-key half (and it IS a state line, so the panel was there to be read)"
[ -n "${MENU_SEL_AFTER:-}" ] && [ "${MENU_SEL_AFTER:-}" != "${MENU_SEL_BEFORE:-}" ]
check $? "and the RIGHT-CLICK that opened the popover SELECTED the key it landed on: the panel's state line's selected-key half MOVED, from [${MENU_SEL_BEFORE:-empty}] to [${MENU_SEL_AFTER:-none}]"
[ -z "${MENU_SEL_BEFORE2:-}" ] && [ -n "${MENU_SEL_AFTER2:-}" ] && [ "${MENU_SEL_AFTER2:-}" != "${MENU_SEL_BEFORE2:-}" ]
check $? "and a LEFT press at the same point SELECTED the key too, from a selection emptied again first ([${MENU_SEL_BEFORE2:-empty}] -> [${MENU_SEL_AFTER2:-none}]; that deselect is at [${MENU_DESELECT:-none}])"
[ -n "${MENU_KEY:-}" ] \
    && [ "$MKX" -ge "$MFX" ] && [ "$MKX" -le $((MFX + MFW)) ] \
    && [ "$MKY" -ge "$MFY" ] && [ "$MKY" -le $((MFY + MFH)) ]
check $? "and the point the scan found is INSIDE the panel's own published fit rect (found [${MENU_KEY:-none}] in ${MENU_FIT:-none}) - and it is NOT asserted to be at that rect's centre: where the key sits inside the plot is the fit policy's business (the frame half of fitAll is what the user asked to change this round), while the phase's business is that the point is on the key the panel DRAWS, which is what the popover opening there and the panel's own selection both answer"
has "${MENU_BEFORE:-}" "Rotate Linear"
check $? "and that key's easing is the param's own default, Linear ([${MENU_BEFORE:-none}])"

# THE EFFECT, and it is the assertion that FAILS IF THE PICK DOES NOTHING: the pointer pressed
# the picker's SINE tile ([${MENU_ENTRY_TILE:-none}]) and the dismissal that followed it
# committed that draft to the DOCUMENT, which is what the panel's own state line names. The
# line is built from the document (the selected target's key), not from the picker's return
# value, and the type named is the one kTileOrder puts on the tile the phase derived the index
# of - so a press that landed on no tile, on another tile, or that never committed, all make
# this red.
has "${MENU_AFTER:-}" "Sine"
check $? "pressing the picker's ${MENU_ENTRY_TILE:-?} tile with the real pointer at [${MENU_ENTRY_PT:-none}] (inside the popup window [${MENU_GEOM:-none}]) and dismissing it changed the selected key's easing to exactly that tile's type: the state line now reads [${MENU_AFTER:-none}], and it read [${MENU_BEFORE:-none}] before"
! has "${MENU_BEFORE:-}" "Sine"
check $? "and the key was not already carrying it, so the line moved for the pick and not for a no-op (before [${MENU_BEFORE:-none}])"
[ "${MENU_ENTRY_CLOSED:-}" = "no" ]
check $? "and the popover closed on that dismissal, by its own window ([${MENU_WIN:-none}] on screen: [${MENU_ENTRY_CLOSED:-none}])"

# THE POPOVER CLOSED AGAIN on the pick: the capture with it up and the capture after it differ
# by the popup's own pixels (the panel underneath was redrawn in the same place). The bar is
# derived from the popup's OWN measured rectangle rather than remembered.
d_menu_gone=$(ae_diff "$NORM/menu_open.png" "$NORM/menu_after.png")
[ "$d_menu_gone" -gt $((MWW * MWH / 2)) ]
check $? "and the popover left the screen with it ($d_menu_gone px changed, bar $((MWW * MWH / 2)) = half the popup's own ${MWW:-?}x${MWH:-?} = $((MWW * MWH)) px rectangle)"

d_menu_ctl=$(ae_diff "$NORM/menu_ctl_a.png" "$NORM/menu_ctl_b.png")
[ "$d_menu_ctl" -eq 0 ]
check $? "the panel does not repaint on its own between the two control captures (control $d_menu_ctl px)"

# THE NEGATIVE, as the TRUE claim: a right-click that hits no key leaves NO picker window on
# screen. The point is 60 px along the frame axis from the key and inside the plot, so it is on
# no key and on no drawn band (a one-key map draws no polyline), and the reading is the WINDOW
# LIST - the same witness the scan accepts on - plus the panel's own line.
[ -z "${MENU_EMPTY_WINS:-}" ]
check $? "a right-click that hits NO key left no picker window on screen (at [${MENU_EMPTY:-none}]: windows answering the picker's geometry [${MENU_EMPTY_WINS:-none}])"
[ "${MENU_NOKEY_LINE:-}" = "${MENU_BEFORE:-}" ] && [ -n "${MENU_BEFORE:-}" ]
check $? "and the panel's own state line is unchanged by it ([${MENU_NOKEY_LINE:-none}], the line the popover was opened from)"

# ---- (d) THE POPOVER'S MODALITY, AND THE APPLICATION'S KEYS BESIDE IT ----
# Two claims, and they are the two halves of the popup's own modality (see the phase's own
# comments): while the popover is OPEN its window has the keyboard, so the application's own
# key commands do not reach the timeline; while it is CLOSED they do. Both are read from the
# DOCUMENT through the panel's own state line, and the popup's side is read from the window
# list - its own viewability - never from a pixel bar. The tile press is measured in pixels
# below, INSIDE the popup's own window, since a popover has no row highlight to measure.
assert_exists "$SHOTS/graph_probe_32_menu_delete.png" "the root with the easing picker open and the Delete key command pressed at it"
assert_exists "$SHOTS/graph_probe_33_menu_escape.png" "the root after Escape was pressed with the easing picker open"
assert_exists "$SHOTS/graph_probe_34_menu_reopen.png" "the easing picker open again at the same key, after Escape closed it"
assert_exists "$SHOTS/graph_probe_35_menu_kb_open.png" "the easing picker open a third time, for the second pick"
assert_exists "$SHOTS/graph_probe_36_picker_tile.png"  "the same popover after the second pick's TILE was pressed"
assert_exists "$SHOTS/graph_probe_37_picker_after.png" "the viewport after that pick was committed by the dismissal"
assert_exists "$SHOTS/graph_probe_38_appkey_undo.png" "the viewport after ctrl+Z with the popover closed"
norm "$SHOTS/graph_probe_32_menu_delete.png"   "$NORM/menu_delete.png"
norm "$SHOTS/graph_probe_33_menu_escape.png"   "$NORM/menu_escape.png"
norm "$SHOTS/graph_probe_34_menu_reopen.png"   "$NORM/menu_reopen.png"
norm "$SHOTS/graph_probe_35_menu_kb_open.png"  "$NORM/menu_kb_open.png"
norm "$SHOTS/graph_probe_36_picker_tile.png"   "$NORM/picker_tile.png"
norm "$SHOTS/graph_probe_37_picker_after.png"  "$NORM/picker_after.png"
norm "$SHOTS/graph_probe_38_appkey_undo.png"   "$NORM/appkey_undo.png"

# THE KEYS THE PICKER ACTS ON ARE THE PICKER'S. Delete is an application command
# (gui::KeyCommandMap: "Delete selected key(s)"), and it would remove exactly the key the
# popover was opened on: a popup that did not consume it would show it in the line below. The
# window list is the second half of the claim: the popover has to be STILL ON SCREEN after both
# keys.
[ "${MENU_DEL_LINE:-}" = "${MENU_BEFORE:-}" ] && [ "${MENU_DEL_OPEN:-}" = "yes" ]
check $? "with the easing picker OPEN the application's own Delete command does NOT reach the timeline, and the popover stays up (the line is unchanged: [${MENU_DEL_LINE:-none}], and its window [${MENU_WIN:-none}] is still on screen [${MENU_DEL_OPEN:-none}])"
[ "${MENU_UNDO_LINE:-}" = "${MENU_BEFORE:-}" ] && [ "${MENU_UNDO_OPEN:-}" = "yes" ]
check $? "and with it OPEN the application's own Undo key command does not reach the timeline either (the line is unchanged: [${MENU_UNDO_LINE:-none}], its window still on screen [${MENU_UNDO_OPEN:-none}]; the picker's own session-undo filter consumes that key, with an empty history here, so nothing commits)"
# ESCAPE CLOSES THE POPOVER AND CHANGES NOTHING. Two halves, because "closed" and "did not
# change the easing" are two different facts: the line must be EXACTLY what it was before the
# popover was opened, and the popover's own window must be GONE (no longer viewable) - which is
# also the failure check for the dismissal every pick below depends on. The identity half is
# the WINDOW LIST (the same witness the scan uses); the pixel half beside it is the
# corroboration, with a bar of half the popup's own measured rectangle.
[ "${MENU_ESCAPE_LINE:-}" = "${MENU_BEFORE:-}" ] && [ -n "${MENU_BEFORE:-}" ]
check $? "Escape closed the easing picker WITHOUT changing anything: the panel's state line still reads [${MENU_ESCAPE_LINE:-none}], exactly the pre-popover [${MENU_BEFORE:-none}] - the draft was never edited, so the close committed nothing"
[ "${MENU_ESC_OPEN:-}" = "no" ]
check $? "and the popover's own window is off the screen ([${MENU_WIN:-none}] on screen: [${MENU_ESC_OPEN:-none}], read from the root window's children)"
d_menu_esc=$(ae_diff "$NORM/menu_delete.png" "$NORM/menu_escape.png")
[ "$d_menu_esc" -ge $((MWW * MWH / 2)) ]
check $? "and its pixels stopped being drawn ($d_menu_esc px changed between the two captures, bar $((MWW * MWH / 2)) = half the popup's own $((MWW * MWH)) px rectangle)"

# AND THE POPOVER IS USABLE AGAIN, twice over: the pointer pick above ran against a fresh
# opening at the same point, and the pick below is a third one. Both report the SAME geometry,
# which is what "the point is still on the drawn key" means for this popover - its origin IS the
# anchor, so a different answer would mean the point had moved (or that the window the phase
# accepted was another one).
[ -n "${MENU_GEOM2:-}" ] && [ "${MENU_GEOM2:-}" = "${MENU_GEOM:-}" ]
check $? "the popover came back after Escape closed it, at the same key and the same place ([${MENU_GEOM2:-none}] against the first [${MENU_GEOM:-none}]) - which is what the pointer pick above was a press INTO"
[ -n "${MENU_KB_GEOM:-}" ] && [ "${MENU_KB_GEOM:-}" = "${MENU_GEOM:-}" ]
check $? "and a third opening, for the second pick, landed there too ([${MENU_KB_GEOM:-none}])"

# THE TILE PRESS DRAWS INSIDE THE POPUP, and the second pick then COMMITS. The pixel half is
# the picker's own: the two captures are taken with the pointer parked and the popover up, so
# what moved is what the press drew on the popup's own surface - the checked tile's accent fill
# and glyph. The changed region is required to lie INSIDE the popup's own rectangle, which is
# what makes this the picker the press landed on rather than a neighbour of it. The picker's
# motion-preview bar animates between ANY two captures, so this is a claim about the surface,
# not about the tile's fill alone; WHICH entry was pressed is the state line's job.
read -r HW HH HX HY <<< "$(printf "%s" "${MENU_KB_HL_BB:-0x0+0+0}" | tr "x+" "  ")"
[ "${MENU_KB_HL:-0}" -gt 200 ] && [ "${HW:-0}" -gt 0 ] && [ "${HH:-0}" -gt 0 ] \
    && [ "${HX:-0}" -ge $((MWX - 2)) ] && [ "${HY:-0}" -ge $((MWY - 2)) ] \
    && [ $((HX + HW)) -le $((MWX + MWW + 2)) ] && [ $((HY + HH)) -le $((MWY + MWH + 2)) ]
check $? "pressing the ${MENU_KB_TILE:-?} changed pixels INSIDE the popover's own window (${MENU_KB_HL:-0} px changed, region [${MENU_KB_HL_BB:-none}], inside [${MENU_GEOM:-none}]) - so the pointer landed on the picker's own surface, where a user clicks a tile"
has "${MENU_KB_LINE:-}" "Round"
check $? "and the pick COMMITTED on the dismissal that followed: the second pick's tile (${MENU_KB_TILE:-?}) put the panel's line on Round ([${MENU_KB_LINE:-none}], which read [${MENU_AFTER:-none}] before it)"
[ "${MENU_KB_CLOSED:-}" = "no" ]
check $? "and the popover closed on that dismissal too ([${MENU_WIN:-none}] on screen: [${MENU_KB_CLOSED:-none}])"

# THE REGRESSION, and the half that matters most: with the popover CLOSED the application's own
# key command works exactly as it did before this control existed. The last edit at this point is
# the second pick above, so ctrl+Z has to undo it, and the panel's own state line has to move
# back to what it read before that pick. (It is also what leaves the key carrying Sine, which is
# what phase 4b below re-shapes.)
[ "${MENU_APPKEY_LINE:-}" = "${MENU_AFTER:-}" ] && [ -n "${MENU_AFTER:-}" ] && [ "${MENU_KB_LINE:-}" != "${MENU_AFTER:-}" ]
check $? "with the easing picker CLOSED the application's own Undo key command DOES reach the timeline: the line went from the second pick's [${MENU_KB_LINE:-none}] back to [${MENU_APPKEY_LINE:-none}], the state before it"

# ---- (d2) PHASE 4b: THE CURVE - THE ONLY PLACE THE EASING IS DRAWN, AND SO THE ONLY
# PIXEL-LEVEL PROOF THAT THE PICKER'S PICK REACHED THE PANEL ----
# WHY THE STATE LINE WAS NOT ENOUGH, MEASURED. The panel draws its summary line only while
# the model is EMPTY (ImGuiOverlay.cpp:843); with curves present the line exists in the
# accessibility tree alone. So every assertion above is invisible on screen, and the easing
# is drawn in exactly one place - the polyline of the segment it shapes - which a map of ONE
# key does not have (graph::Curve::buildPolyline emits nothing for it). MEASURED here on the
# one-key panel: changing the easing changed 0 pixels, because there was no curve to change.
# So phase 4b builds the segment - a second key one frame later carrying a DIFFERENT value -
# and then the assertions below are about the curve's own pixels, with the popover closed in
# every capture they compare:
#   * the segment appears when the second key does (the one-key panel drew no curve at all);
#   * re-shaping it with the picker's Linear tile moves the curve's pixels (the key carried
#     Sine);
#   * and picking the SAME tile a second time moves nothing - which is what makes the line
#     above about the EASING rather than about the picker having been used.
# The route is asserted as well as the effect: the dock's knob row says the playhead moved
# (a channel shows its rows only while a key exists at the current frame), the knob's press
# says the key was added, and the Angle field's own Value interface says the value it was
# given - which is what makes the segment non-flat.
# AND THE PICKER IS DRIVEN THROUGH ITS OWN TILE HERE TOO (Linear is kTileOrder index 0, derived
# from the source, and the tile is pressed with the pointer plus dismissed with Escape), so
# these two picks also assert that a popover opened at phase 4's point keeps landing at that
# same point.
# THE TYPED WRITE IS ALSO THE INSTRUMENT FOR A DEFECT THE IMGUI PHASE RECORDED and which this
# control cannot reproduce: typing into the dock's Angle field used to stop committing once the
# key menu had been used, because the ImGui popup's nav flag was still holding the keyboard.
# This write happens after five popover openings and is asserted to commit.
assert_exists "$SHOTS/graph_probe_26_curve_sine.png"            "the viewport with the segment shaped by the easing the picker's pick chose"
assert_exists "$SHOTS/graph_probe_27_menu_linear_open.png"      "the same panel with the easing picker open again, on the same key"
assert_exists "$SHOTS/graph_probe_28_curve_linear.png"          "the viewport after pressing the picker's Linear tile for that key"
assert_exists "$SHOTS/graph_probe_29_menu_linear_again_open.png" "the easing picker open a third time, for the same tile"
assert_exists "$SHOTS/graph_probe_30_curve_linear_again.png"    "the viewport after pressing the same tile again"
norm "$SHOTS/graph_probe_26_curve_sine.png"             "$NORM/curve_sine.png"
norm "$SHOTS/graph_probe_27_menu_linear_open.png"       "$NORM/menu_linear_open.png"
norm "$SHOTS/graph_probe_28_curve_linear.png"           "$NORM/curve_linear.png"
norm "$SHOTS/graph_probe_29_menu_linear_again_open.png" "$NORM/menu_linear_again_open.png"
norm "$SHOTS/graph_probe_30_curve_linear_again.png"     "$NORM/curve_linear_again.png"
crop "$NORM/curve_sine.png"          "$NORM/c_curve_sine.png"
crop "$NORM/curve_linear.png"        "$NORM/c_curve_linear.png"
crop "$NORM/curve_linear_again.png"  "$NORM/c_curve_linear_again.png"

echo "phase 4b: the second key's angle, seeded [${ANGLE_2ND_SEEDED:-unreadable}] at [${ANGLE_2ND_RECT:-none}], typed [${NEWDEG:-none}] in ${ANGLE_2ND_TRIES:-0} attempt(s), the field value read [${ANGLE_2ND_TYPED_TEXT:-unreadable}] and its line edit [${ANGLE_2ND_EDIT_TEXT:-unreadable}] while the edit was open, and [${ANGLE_2ND_WRITTEN:-unreadable}] after the commit key; the focus holders [${DOCK_FOCUS_BEFORE:-none}] before the click into the field, [${DOCK_FOCUS_CLICK:-none}] after it and [${DOCK_FOCUS_AFTER:-none}] after the commit key; the panel's own line (its shown count and focused channel) [${DOCK_TARGET_BEFORE:-none}] before and [${DOCK_TARGET_AFTER:-none}] after; the knob row at the next frame [${KNOB_2ND:-none}] and after the press [${KNOB_2ND_AFTER:-none}]; the Linear menu [${MENU2_GEOM:-none}] entry [${MENU2_ENTRY:-none}], the state line then [${MENU2_LINE:-none}]; the same pick again: menu [${MENU3_GEOM:-none}] entry [${MENU3_ENTRY:-none}], line [${MENU3_LINE:-none}]"

[ -n "${KNOB_2ND:-}" ]
check $? "the playhead moved one frame right through the app's own key command: the dock's Rotate knob row is back, i.e. this frame carries no key ([${KNOB_2ND:-none}]; Ctrl+Right = MoveRight, MainWindow.cpp:885)"
awk -v a="${ANGLE_2ND_SEEDED:-}" 'BEGIN { exit !(a != "") }'
check $? "and pressing that knob added the second key there: the channel rows are readable (the Angle field answers [${ANGLE_2ND_SEEDED:-unreadable}] at [${ANGLE_2ND_RECT:-none}]), which is what a key at this frame turns on - the knob row's own presence in the tree is NOT the signal, MEASURED: it reports the same rect before and after the press" 
read -r M2W M2H M2X M2Y <<< "${MENU2_GEOM:-0 0 0 0}"
[ "${M2W:-0}" -eq "${MENU_PICK_W:-0}" ] && { [ "${M2H:-0}" -eq "${MENU_PICK_H:-0}" ] || [ "${M2H:-0}" -eq "${MENU_PICK_H_CUSTOM:-0}" ]; }
check $? "the easing picker came back on the same key for the Linear pick, with the picker's own measured geometry ([${MENU2_GEOM:-none}] against the source's ${MENU_PICK_W:-?}x${MENU_PICK_H:-?}/${MENU_PICK_H_CUSTOM:-?}, i.e. the point the scan found is still on the drawn key - the popover opens only within ctrl::graph::kKeyHitRadius of a key)"
[ "$((M2X - MWX))" -ge -2 ] && [ "$((M2X - MWX))" -le 2 ] && [ "$((M2Y - MWY))" -ge -2 ] && [ "$((M2Y - MWY))" -le 2 ]
check $? "and it landed where the first popover did, i.e. the same point is still on the same key ([${MENU2_GEOM:-none}] against phase 4's [${MENU_GEOM:-none}]; popUpAt places it at the anchor, so a different answer would mean the key had moved)"
awk -v a="${ANGLE_2ND_WRITTEN:-}" -v b="${NEWDEG:-}" 'BEGIN { exit !(a != "" && b != "" && (a + 0) == (b + 0)) }'
check $? "the dock's Angle field took the value it was typed, read back through the field's own Value interface (seeded [${ANGLE_2ND_SEEDED:-unreadable}], typed [${NEWDEG:-none}], reads [${ANGLE_2ND_WRITTEN:-unreadable}])"
awk -v a="${ANGLE_2ND_WRITTEN:-}" -v b="${ANGLE_2ND_SEEDED:-}" 'BEGIN { exit !(a != "" && b != "" && (a + 0) != (b + 0)) }'
check $? "and it differs from the value the knob seeded, which is what makes the segment not flat (${ANGLE_2ND_SEEDED:-?} -> ${ANGLE_2ND_WRITTEN:-?} degrees)"
# THE TYPED WRITE IS ALSO THE INSTRUMENT FOR THE DEFECT THE IMGUI PHASE RECORDED: typing into
# this field used to stop committing once the key menu had been used (the ImGui popup's nav
# flag was still holding the keyboard). This write happens AFTER the picker was opened five
# times, so the pair of readings above - what the field held while the edit was open, and what
# it committed - is the answer, and the assertion that the committed value equals what was
# typed is the claim that the defect is gone.
[ -n "${ANGLE_2ND_TYPED_TEXT:-}" ] && [ -n "${ANGLE_2ND_WRITTEN:-}" ]
check $? "and the write still COMMITS after the easing picker has been used (the field read [${ANGLE_2ND_TYPED_TEXT:-unreadable}] while the edit was open and [${ANGLE_2ND_WRITTEN:-unreadable}] after the commit key; the dock's Angle row is the one this phase wrote through five popover openings ago)"

# THE PIXELS. Every one of these captures is taken with the popover CLOSED (pick_linear opens,
# presses the tile, dismisses and waits, and the capture runs after it), so none of them is a
# measurement of the popup's own rectangle: they are the panel the pick changed.
crop "$NORM/menu_after.png" "$NORM/c_menu_after.png"
d_segment=$(ae_diff "$NORM/c_menu_after.png" "$NORM/c_curve_sine.png")
p_segment=$(pct "$d_segment" "$CROP_AREA")
[ "$d_segment" -gt "$SEGMENT_PIXELS" ]
check $? "the second key draws the segment the one-key panel could not ($d_segment px = ${p_segment}% of the crop changed, bar $SEGMENT_PIXELS; the change is the segment's stroke, the new key's square and the 1px playhead moving one frame)"
d_ease=$(ae_diff "$NORM/c_curve_sine.png" "$NORM/c_curve_linear.png")
p_ease=$(pct "$d_ease" "$CROP_AREA")
[ "$d_ease" -gt "$EASE_PIXELS" ]
check $? "and re-shaping that segment moves the CURVE's own pixels: Linear where the key carried Sine ($d_ease px = ${p_ease}% of the crop changed, bar $EASE_PIXELS; the panel draws no summary line while a curve exists, so this change is the polyline and the tick/label layout it lives in)"
d_ease_same=$(ae_diff "$NORM/c_curve_linear.png" "$NORM/c_curve_linear_again.png")
[ "$d_ease_same" -eq 0 ]
check $? "while pressing the SAME tile a second time moves nothing at all ($d_ease_same px against the capture taken before that popover was opened)"
# THE LINE, CONTAINED rather than suffixed: these checks used a case pattern of the form
# `*" Lin"`, which requires the whole line to END with " Lin" and therefore never matched a
# line that goes on - "selected key: Rotate Linear" does - so five assertions in this phase
# could not pass whatever the application did. `has` is the containment test they meant.
has "${MENU2_LINE:-}" "Linear"
check $? "and the panel's own line agrees with the tile the pointer pressed ([${MENU2_LINE:-none}], which read [${MENU_AFTER:-none}] before)"
has "${MENU3_LINE:-}" "Linear"
check $? "and it still reads the same after the second, identical press ([${MENU3_LINE:-none}])"
[ -n "${MENU2_GEOM:-}" ] && [ "${MENU2_GEOM:-}" = "${MENU_GEOM:-}" ] && [ -n "${MENU3_GEOM:-}" ] && [ "${MENU3_GEOM:-}" = "${MENU_GEOM:-}" ]
check $? "and both of those picks put the popover back at the same key and the same place ([${MENU2_GEOM:-none}] and [${MENU3_GEOM:-none}] against phase 4's [${MENU_GEOM:-none}])"

# ---- (e) PHASE 3: THE TARGET THAT GETS DELETED WHILE THE PANEL IS SHOWING IT ----
# WHAT THIS GATE IS FOR. ctrl::GraphEditor holds its targets as RAW core::ObjectNode*
# and walks them on every draw; if a node is destroyed while still in that list, the walk
# is a use-after-free. The application is safe only because the tree's own deletion order
# clears the target list first (see the phase-3 comment in the body), and nothing else in
# the suite or in this probe deletes a target at all. So: the row the panel was showing is
# deleted through the object tree, and the app has to come out of it alive, with the row
# gone, with the panel publishing one of the MODEL's empty-state sentences, and with the
# curve's pixels gone from the crop. The first assertion here is the one that would fire
# if the ordering ever broke: a use-after-free in the panel's draw is a dead process, and
# a dead process is not something AT-SPI can report - every later call would just time out.
echo "phase 3: deleted layer [$DEL_LAYER] as the tree row [$DEL_ROW]; the menu it opened [$DEL_POPUP] [$DEL_POPUP_INFO] with the input focus moved to [$DEL_FOCUS]; process alive [$DEL_ALIVE] (pid [${AE_PID:-?}]); the row after [$DEL_ROW_AFTER], cells now [${DEL_CELLS:-none}]; the panel's sentences: empty [$DEL_EMPTY] no-keys [$DEL_NOKEYS] all-hidden [$DEL_ALLHID]; the panel's channel control after the deletion: '1 channel(s)' [$DEL_LEGEND] and '0 of 1' [${DEL_CHAN_HIDDEN:-none}]"

assert_exists "$SHOTS/graph_probe_17_pre_delete_a.png" "the viewport with the keyed layer's curve, before the deletion"
assert_exists "$SHOTS/graph_probe_18_pre_delete_b.png" "the same state a second time, as the control"
assert_exists "$SHOTS/graph_probe_19_after_delete.png" "the viewport after the layer the panel was showing was deleted"
norm "$SHOTS/graph_probe_17_pre_delete_a.png" "$NORM/pre_del_a.png"
norm "$SHOTS/graph_probe_18_pre_delete_b.png" "$NORM/pre_del_b.png"
norm "$SHOTS/graph_probe_19_after_delete.png" "$NORM/after_del.png"

# THE ROUTE FIRST, because a deletion that never happened cannot have been survived: the
# row was addressable, and the object tree opened its menu AT that row (the popup window's
# origin IS the click point, which is how it is found - see the body; it is not a
# hardcoded coordinate, and the entry it offers was read off a screenshot because the menu
# is not in the accessibility tree at all: see WHAT IS NOT COVERED). The menu has to be
# MAPPED for that reading to be about something on screen, which the window itself answers.
case "${DEL_POPUP_INFO:-}" in
    *IsViewable*) DEL_MAPPED=0 ;;
    *)            DEL_MAPPED=1 ;;
esac
[ -n "${DEL_ROW:-}" ] && [ -n "${DEL_POPUP:-}" ] && [ "$DEL_MAPPED" -eq 0 ]
check $? "the object tree opened its row menu for the layer the panel was showing ([$DEL_LAYER] at [$DEL_ROW], menu window [$DEL_POPUP] [$DEL_POPUP_INFO])"

[ "${DEL_ALIVE:-}" = "yes" ]
check $? "the application survived deleting the layer it was showing (alive [$DEL_ALIVE], pid [${AE_PID:-?}]): a target destroyed while still in ctrl::GraphEditor's raw-pointer list would be walked by buildEntries() and fault here"

[ -z "${DEL_ROW_AFTER:-}" ]
check $? "and the row is gone from the object tree (a cell named [$DEL_LAYER] answers with [$DEL_ROW_AFTER])"

# THE PANEL'S OWN WORDS. The model publishes one of three sentences for an empty target
# list, and which one the deleted layer leaves behind is the model's business; what is
# asserted is that ONE OF THEM IS BACK - the panel must not go on describing a layer that
# no longer exists. The panel's own channel CONTROL is asserted gone beside it: the model's
# list of channels to draw is exactly what buildEntries() produced, so the control (which the
# model publishes only while there is a channel to list) disappearing is the model saying it
# no longer has that curve.
DEL_SAID=""
[ -n "${DEL_EMPTY:-}" ]  && DEL_SAID="Select a layer to show its curves"
[ -n "${DEL_NOKEYS:-}" ] && DEL_SAID="${DEL_SAID:+$DEL_SAID / }No keys on the selected layers"
[ -n "${DEL_ALLHID:-}" ] && DEL_SAID="${DEL_SAID:+$DEL_SAID / }All channels of the selected layers are hidden"
[ -n "$DEL_SAID" ]
check $? "the panel falls back to one of the model's own empty-state sentences after the deletion ([$DEL_SAID]; rects: empty [$DEL_EMPTY], no-keys [$DEL_NOKEYS], all-hidden [$DEL_ALLHID])"
[ -z "${DEL_LEGEND:-}" ] && [ -z "${DEL_CHAN_HIDDEN:-}" ]
check $? "and the model lists no curve for the deleted layer any more: the panel publishes NO channel control at all, under either name its count can take ('1 channel(s)' -> [${DEL_LEGEND:-none}], '0 of 1' -> [${DEL_CHAN_HIDDEN:-none}])"

# THE PIXELS, with the control first: the two captures taken before the deletion, with
# nothing forced between them, are the same state, so the change below can only be the
# deletion.
d_del_control=$(ae_diff "$NORM/pre_del_a.png" "$NORM/pre_del_b.png")
[ "$d_del_control" -eq 0 ]
check $? "the panel does not repaint on its own between the two pre-deletion captures (control $d_del_control px)"

crop "$NORM/pre_del_b.png" "$NORM/c_pre_del.png"
crop "$NORM/after_del.png" "$NORM/c_after_del.png"
d_deleted=$(ae_diff "$NORM/c_pre_del.png" "$NORM/c_after_del.png")
p_deleted=$(pct "$d_deleted" "$CROP_AREA")
[ "$d_deleted" -gt "$CURVE_PIXELS" ]
check $? "the curve the deleted layer was showing left the crop ($d_deleted px = ${p_deleted}% changed, bar $CURVE_PIXELS)"
le "$p_deleted" "$CEIL_PCT"
check $? "and the deletion is still not a wash (${p_deleted}% <= ceiling ${CEIL_PCT}%)"
d_deleted_whole=$(ae_diff "$NORM/pre_del_b.png" "$NORM/after_del.png")
echo "deletion: the whole screen changed $d_deleted_whole px, of which $d_deleted px are inside the crop; the deleted row itself is BELOW the crop (the object tree sits in the Animation dock, y >= $ANIM_Y), so the crop's change is the viewport: the panel's curve leaving and its empty state line arriving, plus whatever the deleted layer contributed to the canvas strip"

# DIAGNOSTIC, NOT AN ASSERTION: the post-deletion crop against the panel's own ON empty
# state from the top of this run (nothing selected, before any key existed). If the deleted
# layer drew nothing in the crop's canvas strip these two are the same picture, which is
# the strongest form of "the panel is back to its no-target state"; the number is recorded
# here rather than asserted, because the canvas strip is not the panel's to control.
d_vs_empty=$(ae_diff "$NORM/c_on_empty.png" "$NORM/c_after_del.png")
echo "deletion: the crop now differs from the panel's ON empty state by $d_vs_empty px (the panel's own empty state is what it fell back to; any remainder is the canvas strip the deleted layer was in)"

assert_rendered "$SHOTS/graph_probe_05_on_top.png" "the viewport with the animation graph overlay"
summary probe_graph_overlay
