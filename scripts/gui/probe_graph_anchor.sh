#!/bin/bash
# THE PANEL'S X IS A CAMERA, NOT A FRAME COUNTER - IN THE TIMELINE DOCK.
#
# The graph panel's horizontal window is the timeline's, pushed as VALUES in FRACTIONAL
# frames, and the panel maps it into its own plot with a fractional rate. Two properties
# follow and are asserted here, on the application's own pixels - the only place where the
# mapping's INPUT (the timeline's exact viewport) and its OUTPUT (a drawn column) are both
# real: a middle-drag pan moves the content by exactly the pixels dragged, and a wheel notch
# holds the frame under the cursor.
#
# THE PANEL MOVED, and this probe moved with it. It used to be a full-viewport Dear ImGui
# overlay toggled from the Tools dock; it is now the timeline dock's own Graph tab
# (gui::TimeLineHeader's tab bar over gui::GraphPanelWidget, drawn on the dock's page with
# the object tree still beside it - see probe_graph_ink.sh, which drove the dock from the
# day it landed). The overlay, and with it the control named "Show animation graph", went
# away, so nothing here presses that control and NOTHING HERE READS A VIEWPORT CROP: the
# panel is not on the viewport any more, and a probe that went on measuring a viewport
# rectangle would be measuring the canvas under it.
#
# THE ROUTE, and where every number comes from (no widget offset is hardcoded):
#   * the measured surface is the rectangle the panel PUBLISHES for its own "Fit graph"
#     action, read by name through AT-SPI: gui::GraphPanelFrame's buildAccessibilityModel
#     sets fitItem.rect to the model's plot (and to the panel when the plot is empty), so
#     the crop below reads the application's own plot, and the notch witness adds the
#     model's own rulerBand to it - neither is this probe's arithmetic;
#   * the header's page tabs are AT-SPI controls named "Timeline" and "Graph", pressed by
#     name, and WHICH ONE IS CURRENT is readable from their rectangles: the current one is
#     inflated to the whole strip (MEASURED on this tab bar by probe_graph_ink.sh, 120px
#     against 60px, while the two keep their own slots at x=8 and x=68);
#   * the dock is the AT-SPI rectangle of the widget named "Animation", read TWICE - once
#     while the timeline is the current page (for the tabs) and again once the Graph page is
#     (for the plot), because the two pages are not the same width (see the PAGE bullet);
#   * the panel's PAGE inside the dock - the dock's right-hand part - starts at the TREE
#     SPLITTER's hairline, measured ON THE GRAPH PAGE ITSELF as the one column inked in every
#     row of the dock's body (the same 1px hairlines probe_timeline_ink.sh measures the
#     timeline's ruler with). It is measured on THAT page and not on the timeline's ruler,
#     because the dock AND that boundary MOVE when the Graph tab becomes current: the header
#     carries the page's OWN controls and the Channels button exists only on the Graph page
#     (MEASURED: dock 848px wide with the boundary at x=242 while the timeline is up, 920px
#     with it at x=263 once this page is up - and the timeline's own tree width is then the
#     edge of a page that is not showing). It is CHECKED against rather than cropped to: the
#     published plot's left edge must sit the scaler's own inset (kMargin 4 + axisLabelWidth,
#     whose reserve is derived from the value format's widest label - 68px in
#     graph_Scaler.h's own measurement, MEASURED here as 70px, so the plot sits 74px into the
#     page - and whose range is [46,96]) past the page's left edge, and the assertion names
#     both numbers.
#   * the crop is that published plot inset by kFrameWindowMargin (8px, the same
#     ctrl::graph::Scaler): the margin the shared frame window is mapped into at both plot
#     edges, so the playhead, which lives inside that mapping, cannot touch the crop's own
#     edge. The finder/pan reading is cut to the rows BELOW the band, which is the
#     difference the RULER BAND makes:
#       - the FINDER (ae_pix.py's "line"/"moved") reads only the rows BELOW the band, so the
#         playhead's PILL - a 14px-wide accent block in the band's number row - is not in its
#         search region at all and the column it returns is the 1px line by construction;
#       - the PAN's before/after reading uses that same below-band range, because the pan
#         moves the playhead LINE there and the finder must not see the pill.
#     THE NOTCH WITNESS HAS ITS OWN REGION (NOTCH_CROP), because the ruler is a SECTION now.
#     ctrl::graph::Scaler::plotRect() starts below the 22px kRulerBandHeight band, the model
#     publishes rulerBand == QRect(plot.left(), plot.top() - 22, plot.width(), 22), and the
#     playhead pill and the frame ticks/numbers live in that band, not in the plot. On the
#     fresh project this probe builds there are no keys, so one notch changes the band's
#     ticks and numbers and almost nothing inside the plot (MEASURED: 975 of the notch's 975
#     changed pixels were in the band's 14 rows, none below them). The witness therefore
#     compares the band PLUS the plot - the panel region from the band's top, plot.top() - 22,
#     to the plot's bottom, over the plot's own width - read from the same GEOM dump the probe
#     already reads (the model's rulerBand and the published plot), so the region is
#     application geometry and not this probe's arithmetic. That is why a stale crop is exactly
#     what made this assertion read 0: the old plot-only region had no notch ink in it at all.
#     Neither comparison chases ink: the band's chrome is neutral, and ae_pix.py's "moved"
#     insists on a CHROMATIC colour that tops exactly ONE column in both shots and moves, so
#     a number's or a tick's column cannot take the playhead's place - and the notch witness is
#     an AE difference on a static raster, so a stale capture (nothing repainted between the
#     two shots) is byte-identical there and counts 0, far under its bar.
#
# THE FEATURE IS THE PLAYHEAD, and no theme constant is baked in: its colour is learned from
# the pan pair below (the colour that tops exactly ONE column in both shots, at a different
# column in each - the panel's flat floor and its chrome top many columns at once, and grey
# is excluded outright), and it is the panel's only feature whose FRAME is known, which is
# what makes "did the camera hold the point under the cursor" a pixel question. ae_pix.py
# does the scan. With the fresh project this probe builds there is nothing else chromatic on
# the panel to learn by mistake: no key means no curve, no marker, and no channels button
# (graph::buildChannels publishes none while the count is zero), so the playhead's column is
# the only full-height chromatic run on the dock's page. The pill is the SECOND piece of
# accent ink (14 rows in the band's number row, MEASURED 17 columns wide, none of which holds
# more than 8 of them inside the crop), and it is exactly why the finder is restricted to the
# rows below the band: the column it returns holds the line's whole run (MEASURED 218 of 218
# rows below the band, one pixel wide), never the pill's block.
#
# THE PROJECT IS BUILT HERE, at the application's own default (600 frames): the pan assertion
# is only decisive where a frame is SEVERAL pixels wide, and that needs room to zoom - a
# short project clamps the axis, and the climbing notches then slide the anchored frame out
# of view instead of zooming (measured on a 160-frame project). The dock's own STARTING rate
# is smaller than a full-viewport surface's (the panel maps the timeline's window into a plot
# narrower than the timeline's own viewport), so the frame-axis notches climb from there at
# the ladder's own 1.1 per notch (ctrl::time::Scaler::kZoomPerNotch) until the rate is back
# where the claim names it, a few pixels per frame; the measured rates are in the log line
# and in the assertions. A project of the caller's (arg 1 or AE_PROJECT) is used instead; the
# numbers in the log are the recording's.
#
# THE TWO THINGS THE DOCK CHANGED ABOUT THE GESTURES, both MEASURED here and worth knowing
# before reading a reading that looks wrong:
#   * A FRAME THE TIMELINE MOVES IS DRAWN BY THIS PAGE, and that is NEW: it used to be redrawn
#     only when the page became visible (TargetWidget::onSurfaceChanged -> GraphPanelWidget::
#     refresh), so ctrl+Right stepped the document's frame and changed 0 px of the screen - and
#     the next gesture that repainted the page flushed every step at once, reading as a pan that
#     much too long. The dock now connects the timeline's own signal (TargetWidget.cpp:
#     mTimeLine->onFrameUpdated -> mGraphPanel->refresh()), so the probe reads the frame-stepped
#     playhead from the page itself and does NOT re-show the page: the two frame-step readings
#     below (FRAME_X and HIGH_PPF) are what says that connection is doing its job.
#   * A WHEEL GOES TO THE WIDGET UNDER THE POINTER. The pointer is parked on the File menu for
#     every capture (the panel has hover states of its own), so a notch sent from there is
#     sent to the menu bar: the probe moves the pointer onto the playhead's column AFTER the
#     capture it compares against.
# Neither is a claim of its own here; both are the route the two claims below need.
#
# WHAT THIS PROBE DOES NOT COVER: the frame ladder itself. tests/time_scaler.cpp pins it
# directly - the x1.1 step at every index, the bounds, the inverse and the tick stride.
#
# FAILING LOUDLY is what the identification is for: a gesture that moves nothing prints none,
# and the run reports it instead of measuring a static chrome column.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

GEOM=/tmp/probe_graph_anchor_geom.txt
SCRATCH=/tmp/probe_graph_anchor
STEPS=/tmp/probe_graph_anchor_steps.log
INK="$ROOT/scripts/gui/ae_ink.py"
APPLOG=/tmp/probe_graph_anchor_app.log
rm -f "$GEOM"
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"

# The finder/pan crop, as an inset of the published plot. 8 is ctrl::graph::Scaler::kFrameWindowMargin,
# the margin the shared window is mapped into at both plot edges: the playhead cannot be
# nearer an edge than that, so the crop keeps the whole of it and drops the panel's own
# edge rules (the value ticks sit AT the plot's left edge, the zero rule spans its width).
CROP_INSET=8
# A crop narrower than this cannot hold the gesture the pan assertion makes: the drag starts
# at the crop's middle and runs PAN_PIXELS, so the crop must be at least middle + drag wide.
CROP_MIN=80
# The ruler band the panel draws immediately ABOVE the plot's top (the scaler reserves its
# rows, so plotRect starts below them): frameRuler::kBandHeight (==
# ctrl::TimeLineEditor::kHeaderHeight), 22px, because the panel republishes the timeline's
# own header height. The playhead's PILL is drawn inside it (frameRuler::kPillTop 2 from the
# band's top, kNumberHeight 14 tall) and the 1px line starts UNDER it
# (playheadLineTop == band top + kLineTop), so the FINDER reads only the rows BELOW this
# many pixels from the plot's top: the pill can then never take the line's column, and the
# column that wins is the line by construction. The finder/pan crop drops the band; the
# NOTCH witness keeps it, because it is what a zoom notch repaints.
RULER_BAND=22
# How many rows one colour must cover for its column to be a candidate at all, as a percent
# of the rows scanned. The playhead line covers all of them; the ruler band's chrome and the
# pill (14 rows) are far below that, so an overlay of text cannot hide it. The rows scanned
# are the FINDER's own, i.e. below RULER_BAND.
MIN_COLOUR_PERCENT=35
# A run this long in the TIMELINE page's ruler hairline is the timeline's, not a tree cell's:
# the edge that page starts at is the first such run.
TREE_EDGE_MIN_RUN=10
# The scaler's own inset of the plot inside the panel: kMargin (4) + axisLabelWidth, whose
# floor is kAxisLabelWidth (46) and whose ceiling is kMaxAxisLabelWidth (96).
PLOT_PAGE_INSET_MIN=50
PLOT_PAGE_INSET_MAX=100
# The pan is ONE pointer move of this many pixels, so the only rounding in the path is the
# timeline's own content-pixel step (a multi-step drag would add one rounding per event).
PAN_PIXELS=40
# Current-frame steps, for reading the panel's pixels-per-frame off the playhead's movement.
FRAME_STEPS=20
# The same at a high zoom, where a frame is several pixels: 20 frames would walk the playhead
# out of the plot, and the rate is readable from a few just as well.
HIGH_FRAME_STEPS=5
# Frame-axis zoom-in notches for the high-zoom phase, chosen so the rate the claim names is
# reached from the dock's own start: the timeline seeds its scaler at index 1 (1.1 px/frame,
# ctrl::time::Scaler), the panel's rate for that window is this plot's share of the timeline's
# viewport (MEASURED on the default project: 1.00 px/frame at the dock's own start, in a
# 484x248 plot of a 788x310 dock), and 1.1^20 = 6.7 takes it back to the few-pixels-per-frame
# the overlay's recording measured (the shared window's exact fit rounds the rung, so the run
# prints 6.2 rather than 6.7 - the same order, and several pixels per frame either way).
HIGH_ZOOM_NOTCHES=20
# The notch has to have done something. Its witness reads the ruler band PLUS the plot
# (NOTCH_CROP): hundreds of columns wide, and one notch redraws every tick, every number and
# the playhead pill in the band, so this bar separates "the notch zoomed" from "the notch did
# nothing" with room to spare. It CANNOT pass on a stale capture: the band's chrome and the
# plot's empty floor are static, so two captures with no repaint between them are identical
# and compare to 0 changed pixels. Only a raster the notch actually changed can reach 500.
NOTCH_CHANGE_MIN=500
export GEOM SCRATCH STEPS INK APPLOG CROP_INSET CROP_MIN RULER_BAND MIN_COLOUR_PERCENT \
       TREE_EDGE_MIN_RUN PLOT_PAGE_INSET_MIN PLOT_PAGE_INSET_MAX PAN_PIXELS FRAME_STEPS \
       HIGH_FRAME_STEPS HIGH_ZOOM_NOTCHES NOTCH_CHANGE_MIN

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  set -u
  cd "$AE_PROBE_DIR"
  "$APP" "${PROJ:-}" >"$APPLOG" 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID
  # EVERY STEP IS RECORDED AS IT HAPPENS (see probe_graph_ink.sh): the body stdout is piped
  # through tail, so a run that hangs or dies prints nothing, and the report below prints
  # this file beside a failure.
  : > "$STEPS"
  say() { echo "t+${SECONDS}s  $*" >> "$STEPS"; }
  alive() { kill -0 "$app" 2>/dev/null; }
  abort() {
    printf "ABORT=\"%s\"\n" "$1" >> "$GEOM"
    say "ABORT: $1"
    kill $app 2>/dev/null
    wait $app 2>/dev/null
    exit 0
  }

  wait_ready 120 || abort "the main window never came up (wait_ready timed out)"
  W=$(xdotool getactivewindow 2>/dev/null || xdotool search --name AnimeEffects | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  sleep 0.6

  # THE PROJECT, at the application own defaults (600 frames), so the timeline can be zoomed
  # past its floor without clamping.
  ae_press "File" "the File menu opens"
  sleep 1
  ae_press "New project" "the New project entry is pressed"
  sleep 2.5
  ae_activate "Create" "a default new project is created"
  sleep 4

  # THE POINTER RESTS ON THE FILE MENU for every capture: the panel has hover states of its
  # own (a marker brightens, a chip appears, tangent handles highlight), so a capture taken
  # with the pointer over the plot would measure one of them.
  FR=$(ae extents "File")
  [ -n "$FR" ] || abort "no File menu: this is not the application window"
  read -r FX FY FW FH <<< "$FR"
  POINTX=$((FX + 3)); POINTY=$((FY + FH / 2))
  park() { xdotool mousemove --sync $POINTX $POINTY; sleep 0.5; }
  shot() { park; import -window root "$1"; }
  raw() { magick "$1" -crop "$2" +repage -depth 8 rgb:"$3"; }
  # The FINDER raw: the crop own columns, but only the rows BELOW the ruler band, so the
  # playhead pill (which lives in the band) is not inside the scanned region at all - see
  # RULER_BAND. The columns are the crop own (the left edge is), so every column number is
  # comparable with the crop the pan is compared in. The notch witness is a separate region
  # (NOTCH_CROP); the playhead columns above are always read from THIS raw, never from it.
  rawline() { raw "$1" "${CW}x${LH}+${CL}+${LT}" "$2"; }
  pix() { python3 "$ROOT/scripts/gui/ae_pix.py" "$@"; }
  inkpx() { python3 "$INK" "$@"; }
  # tab <name>: press the dock header page tab BY NAME, from its own rectangle.
  tab() {
    local r x y w h
    r=$(ae extents "$1")
    [ -n "$r" ] || return 1
    read -r x y w h <<< "$r"
    xdotool mousemove --sync $((x + w / 2)) $((y + h / 2)) click 1
    sleep 1.8
    return 0
  }
  # NO repaint() HELPER: the page draws a frame the timeline steps BY ITSELF now (TargetWidget
  # connects mTimeLine->onFrameUpdated to mGraphPanel->refresh()), so the readings below are
  # taken from the page as it is - and the two frame-step numbers are what checks that.

  # ---- the dock and its header ----
  DOCK=$(ae extents "Animation")
  [ -n "$DOCK" ] || abort "no visible widget named Animation: this is not the docked layout, so there is no dock to measure the panel in"
  read -r DX DY DW DH <<< "$DOCK"
  echo "DOCK=\"$DOCK\"" >> "$GEOM"
  say "dock $DX,$DY ${DW}x${DH}"
  echo "TAB_T_AT_TL=\"$(ae extents "Timeline")\"" >> "$GEOM"
  echo "TAB_G_AT_TL=\"$(ae extents "Graph")\"" >> "$GEOM"

  # ---- the TIMELINE page own edge, off its ruler bottom hairline ----
  # Taken before the switch, because the timeline is the page that is up. The ruler bottom
  # hairline spans the timeline and NOT the tree column, so the first long run of the row
  # modal colour IS where the timeline starts. This is the TIMELINE page own edge and only
  # that page: the dock and this boundary MOVE when the Graph page becomes current, which is
  # why the panel page edge is measured again (below) with that page up.
  shot "$SHOTS/anchor_00_dock_timeline.png"
  TY0=$((DY + 37 + 40))
  raw "$SHOTS/anchor_00_dock_timeline.png" "${DW}x1+${DX}+$((TY0 - 1))" "$SCRATCH/hairline.raw"
  HAIRC=$(inkpx modal "$SCRATCH/hairline.raw" $DW 1 0 0 $DW 1 | cut -d" " -f1-3)
  TREE_W=""; TREE_RUN=0
  case "$HAIRC" in
    ""|none) say "row $((TY0 - 1)) has no modal colour: nothing to locate the page edge with" ;;
    *)
      read -r HC1 HC2 HC3 <<< "$HAIRC"
      inkpx cols "$SCRATCH/hairline.raw" $DW 1 0 0 "$HC1" "$HC2" "$HC3" 2 1 > "$SCRATCH/hair_rows.txt"
      while read -r a b n; do
        case "$a" in ""|none) continue ;; esac
        len=$((b - a + 1))
        [ "$len" -ge "$TREE_EDGE_MIN_RUN" ] && [ -z "$TREE_W" ] && TREE_W=$a
        [ "$len" -gt "$TREE_RUN" ] && TREE_RUN=$len
      done < "$SCRATCH/hair_rows.txt"
      ;;
  esac
  echo "TREE_W=\"${TREE_W:-}\"" >> "$GEOM"
  echo "TREE_RUN=\"$TREE_RUN\"" >> "$GEOM"
  echo "HAIR_COLOUR=\"${HAIRC:-none}\"" >> "$GEOM"
  say "the timeline page starts at $((${TREE_W:-0} + DX)) (tree ${TREE_W:-none}px of the dock, longest hairline run ${TREE_RUN} of ${DW}px)"

  # ---- the route: the dock header own Graph tab, pressed BY NAME ----
  tab "Graph" || abort "the dock header has no pressable tab named Graph: the panel cannot be shown by the dock own route"
  echo "TAB_T_AT_G=\"$(ae extents "Timeline")\"" >> "$GEOM"
  echo "TAB_G_AT_G=\"$(ae extents "Graph")\"" >> "$GEOM"

  # ---- the panel own plot, from the panel own publication ----
  PLOT=$(ae extents "Fit graph")
  [ -n "$PLOT" ] || abort "the docked panel published no widget named Fit graph while its page is up: that action box IS the model own plot, and without it this probe has no measured surface to crop"
  read -r PLX PLY PLW PLH <<< "$PLOT"
  CL=$((PLX + CROP_INSET)); CT=$((PLY + CROP_INSET))
  CW=$((PLW - 2 * CROP_INSET)); CH=$((PLH - 2 * CROP_INSET))
  echo "PLOT=\"$PLOT\"" >> "$GEOM"
  echo "CROP=\"$CL $CT $CW $CH\"" >> "$GEOM"
  # THE RULER BAND, from the model own publication: GraphPanelModel publishes rulerBand as
  # QRect(plot.left(), plot.top() - kRulerBandHeight, plot.width(), kRulerBandHeight), with
  # the band bottom (plot.top() - 1) adjacent to the plot, and the published Fit graph box
  # above IS model.plot. kRulerBandHeight is 22 (Scaler::kRulerBandHeight, static_asserted
  # against frameRuler::kBandHeight), the same RULER_BAND the finder rows are cut with.
  BL=$PLX
  BT=$((PLY - RULER_BAND))
  BW=$PLW
  BH=$RULER_BAND
  echo "rulerBand=\"$BL $BT $BW $BH\"" >> "$GEOM"
  # THE NOTCH WITNESS REGION: the ruler band PLUS the plot - the panel page from the band top
  # to the plot bottom, over the plot own width. Named apart from CROP because the finder
  # must keep reading only below the band (the playhead pill would otherwise win the column),
  # while the notch witness must include the band: on this keyless project that is where one
  # notch actually paints (its ticks, its numbers and the pill).
  NC_L=$PLX
  NC_T=$BT
  NC_W=$PLW
  NC_H=$((PLH + RULER_BAND))
  echo "NOTCH_CROP=\"$NC_L $NC_T $NC_W $NC_H\"" >> "$GEOM"
  if [ "$CW" -lt "$CROP_MIN" ] || [ "$CH" -lt "$CROP_MIN" ]; then
    abort "the panel own plot [${PLOT:-none}] leaves a ${CW}x${CH} crop, and a pan of ${PAN_PIXELS}px from its middle needs ${CROP_MIN}px each way"
  fi
  MIDY=$((CT + CH / 2))
  MINC=$((CH * MIN_COLOUR_PERCENT / 100))
  # The FINDER rows: the crop below the ruler band, which the playhead pill lives in. LH
  # cannot run out before the crop does (CH >= CROP_MIN with 16 of CROP_INSET inside it, so
  # PLH >= 96 and LH == PLH - RULER_BAND - CROP_INSET >= 66).
  LT=$((PLY + RULER_BAND))
  LH=$((PLY + PLH - CROP_INSET - LT))
  MINC_LINE=$((LH * MIN_COLOUR_PERCENT / 100))
  echo "FINDER_ROWS=\"$LT $LH\"" >> "$GEOM"
  say "plot $PLOT -> crop $CL,$CT ${CW}x${CH} (a column needs ${MINC} rows of one colour); the finder reads rows ${LT}..$((LT + LH - 1)) below the ${RULER_BAND}px band (${LH} rows, ${MINC_LINE} of one colour)"
  say "ruler band $BL,$BT ${BW}x${BH} (plot.top() - ${RULER_BAND}); the notch witness is $NC_L,$NC_T ${NC_W}x${NC_H} (band top to plot bottom)"
  shot "$SHOTS/anchor_01_graph_page.png"

  # ---- the dock and the page edge AS THE GRAPH PAGE SHOWS THEM ----
  # THE DOCK MOVES WHEN THE GRAPH PAGE BECOMES CURRENT, and the tree splitter with it. The
  # header above the body carries the page OWN controls: TimeLineHeader shows the Channels
  # button only while the Graph page is current (TimeLineHeader.cpp, the onTabChanged branch
  # that sets mChannelsButton visible for Surface::Graph), so switching pages changes what the
  # dock has to be wide enough for, and the dock takes that width (MEASURED on this recording:
  # 848px wide with the boundary at x=242 while the timeline was up, 920px with it at x=263
  # now). The rectangle read before the switch is therefore the TIMELINE page own dock, and
  # using it as the panel page is how a correct plot came out "outside" it. Both bounds are
  # read HERE, with the Graph page up:
  #   * the dock own AT-SPI rectangle again - the page own right and bottom bounds;
  #   * the page own LEFT edge from the splitter hairline, the one column inked in EVERY row
  #     of the dock body, so a column scan needing all ${CH} rows returns it first (the
  #     playhead line and the dock own right-hand edge are the only other such columns, both
  #     to its right). The page first pixel is the column after the run.
  DOCK_G=$(ae extents "Animation")
  echo "DOCK_G=\"${DOCK_G:-}\"" >> "$GEOM"
  PAGE_LEFT=""; PBG=""
  read -r DGX DGY DGW DGH <<< "${DOCK_G:-0 0 0 0}"
  if [ "$DGW" -gt 0 ]; then
    PW=$((DGX + DGW))
    raw "$SHOTS/anchor_01_graph_page.png" "${PW}x${CH}+${DGX}+${CT}" "$SCRATCH/pageband.raw"
    PBG=$(inkpx modal "$SCRATCH/pageband.raw" "$PW" "$CH" 0 0 "$PW" "$CH" | cut -d" " -f1-3)
    case "$PBG" in
      ""|none) say "the dock body has no modal colour: the page edge cannot be measured" ;;
      *)
        read -r PBR PBGG PBB <<< "$PBG"
        inkpx inkcols "$SCRATCH/pageband.raw" "$PW" "$CH" 0 $((CH - 1)) "$PBR" "$PBGG" "$PBB" 12 "$CH" > "$SCRATCH/page_cols.txt"
        while read -r c0 c1 cn cr cg cb; do
          case "$c0" in ""|none) continue ;; esac
          PAGE_LEFT=$((DGX + c0 + 1))
          break
        done < "$SCRATCH/page_cols.txt"
        ;;
    esac
  fi
  echo "PAGE_LEFT=\"${PAGE_LEFT:-}\"" >> "$GEOM"
  echo "PAGE_BG=\"${PBG:-none}\"" >> "$GEOM"
  say "the graph page starts at ${PAGE_LEFT:-unmeasured} (dock now ${DGW}px wide, body background ${PBG:-none})"

  # The playhead column of a capture, given its learned colour; 99999 fails the assertion
  # loudly when the feature is not in view. The raw it reads is a rawline (the rows below the
  # ruler band), so its height is LH and the bar is MINC_LINE: the column that wins holds the
  # line whole run there, and the pan own reading prints that count against LH.
  col() {
    local c
    c=$(pix line "$1" $CW $LH "$2" "$3" "$4" $MINC_LINE | cut -d" " -f1)
    case "$c" in ""|*[!0-9]*) echo 99999 ;; *) echo "$c" ;; esac
  }

  # (1) THE PAN, which also names the playhead: a middle-drag of PAN_PIXELS must move the
  # content by exactly that many pixels. A whole-frame carrier snaps it to the nearest frame
  # instead. The drag runs AWAY FROM THE WINDOW START: a new project opens with its first
  # frame at the window own left edge, so a drag the other way would take the playhead out of
  # the crop the columns are read from.
  shot "$SHOTS/anchor_02_pan_before.png"
  rawline "$SHOTS/anchor_02_pan_before.png" "$SCRATCH/pa.raw"
  xdotool mousemove --sync $((CL + CW / 2)) $MIDY
  sleep 0.3
  xdotool mousedown 2
  sleep 0.2
  xdotool mousemove --sync $((CL + CW / 2 + PAN_PIXELS)) $MIDY
  sleep 0.3
  xdotool mouseup 2
  sleep 1.0
  shot "$SHOTS/anchor_03_pan_after.png"
  rawline "$SHOTS/anchor_03_pan_after.png" "$SCRATCH/pb.raw"
  MOVED=$(pix moved "$SCRATCH/pa.raw" "$SCRATCH/pb.raw" $CW $LH $MINC_LINE)
  echo "MOVED=\"$MOVED\"" >> "$GEOM"
  if [ "$MOVED" = "none" ]; then
    say "the pan moved nothing in the crop ${CL},${CT} ${CW}x${CH} below the band"
    kill $app 2>/dev/null
    wait $app 2>/dev/null
    exit 0
  fi
  read -r PX0 PX1 PLR PLG PLB <<< "$MOVED"
  echo "PAN_X=\"$PX0 $PX1\"" >> "$GEOM"
  echo "COLOUR=\"$PLR $PLG $PLB\"" >> "$GEOM"
  # WHAT THE COLUMN IS, printed with the colour it is found by: the finder reads only the rows
  # below the 22px ruler band, so the pill is not in its region - and this count is the proof,
  # because a pill column could hold at most its own 14 rows while the line holds every row of
  # the region.
  LINE_READ=$(pix line "$SCRATCH/pa.raw" $CW $LH "$PLR" "$PLG" "$PLB" $MINC_LINE)
  LINE_X=${LINE_READ%% *}
  LINE_N=${LINE_READ##* }
  case "$LINE_N" in ""|*[!0-9]*) LINE_N=0 ;; esac
  echo "LINE_RUN=\"column ${LINE_X:-none} holds $LINE_N of $LH rows\"" >> "$GEOM"
  say "the playhead is rgb $PLR,$PLG,$PLB and the pan moved it $PX0 -> $PX1 (its column holds $LINE_N of ${LH} rows of the colour below the band: a full-height 1px line, not the pill)"

  # The panel own pixels per frame, from the playhead own frame: 20 current-frame steps. The
  # steps move the frame the TIMELINE is on and the page draws it as it goes (the dock connects
  # onFrameUpdated to the panel refresh - see THE TWO THINGS above), so this reading is also
  # what says that connection is live: if it were not, the playhead would still be drawn where
  # the pan had left it and FRAME_X would equal PAN_X.
  xdotool key --clearmodifiers --repeat $FRAME_STEPS --repeat-delay 60 ctrl+Right
  sleep 1.0
  shot "$SHOTS/anchor_04_frameN.png"
  rawline "$SHOTS/anchor_04_frameN.png" "$SCRATCH/fn.raw"
  FX1=$(col "$SCRATCH/fn.raw" "$PLR" "$PLG" "$PLB")
  echo "FRAME_X=\"$PX1 $FX1\"" >> "$GEOM"
  say "$FRAME_STEPS frame steps moved the playhead $PX1 -> $FX1"

  # (2) THE ZOOM ANCHOR: notches with the pointer ON the playhead column, so the frame under
  # the cursor is the playhead own frame and an anchored zoom cannot move it. The first is a
  # plain notch (both axes), then the frame axis is climbed until a frame is several pixels
  # wide - which is the zoom the pan below has to survive.
  #
  # THE POINTER IS MOVED ONTO THE COLUMN AFTER THE CAPTURE IT IS COMPARED AGAINST. A wheel
  # goes to the widget UNDER THE POINTER, and shot() parks the pointer on the File menu; a
  # notch sent from there is a notch the panel never sees (MEASURED: the first version of this
  # file sent it while the pointer was still parked, and the notch changed 0 px of the panel
  # and moved the anchored frame 0 px - i.e. it did not happen at all).
  shot "$SHOTS/anchor_05_zoom_before.png"
  magick "$SHOTS/anchor_05_zoom_before.png" -crop "${NC_W}x${NC_H}+${NC_L}+${NC_T}" +repage "$SCRATCH/za.png"
  xdotool mousemove --sync $((CL + FX1)) $MIDY
  sleep 0.4
  xdotool click 5
  sleep 1.0
  shot "$SHOTS/anchor_06_zoom_after.png"
  rawline "$SHOTS/anchor_06_zoom_after.png" "$SCRATCH/zb.raw"
  magick "$SHOTS/anchor_06_zoom_after.png" -crop "${NC_W}x${NC_H}+${NC_L}+${NC_T}" +repage "$SCRATCH/zb.png"
  ZX1=$(col "$SCRATCH/zb.raw" "$PLR" "$PLG" "$PLB")
  echo "ZOOM_X=\"$FX1 $ZX1\"" >> "$GEOM"
  # THE WITNESS COUNTS THE BAND PLUS THE PLOT, i.e. the region a notch really repaints now
  # that the ruler is its own section. A stale capture (both shots identical because the
  # notch did not reach the panel) compares to 0 and cannot reach the bar.
  ZCH=$(magick compare -metric AE "$SCRATCH/za.png" "$SCRATCH/zb.png" null: 2>&1)
  echo "ZOOM_PIXELS=\"$ZCH\"" >> "$GEOM"
  say "one notch on the playhead column: $FX1 -> $ZX1; the band+plot witness $NC_L,$NC_T ${NC_W}x${NC_H} (band $BL,$BT ${BW}x${BH} + plot $PLOT) changed ${ZCH} px"

  xdotool mousemove --sync $((CL + ZX1)) $MIDY
  sleep 0.4
  xdotool click --repeat $HIGH_ZOOM_NOTCHES --delay 90 5
  sleep 1.2
  shot "$SHOTS/anchor_07_high_before.png"
  rawline "$SHOTS/anchor_07_high_before.png" "$SCRATCH/ha.raw"
  HA=$(col "$SCRATCH/ha.raw" "$PLR" "$PLG" "$PLB")
  echo "HIGH_ANCHOR=\"$ZX1 $HA\"" >> "$GEOM"
  say "$HIGH_ZOOM_NOTCHES notches on the playhead column: $ZX1 -> $HA"

  # ...and the playhead MOVES correctly at that zoom, which is the state the pan below is
  # measured in. It steps TOWARD the middle of the plot: a step the other way walks it off the
  # edge at a high zoom, and a playhead outside the plot is clipped and cannot be measured at
  # all.
  STEPKEY=$( [ "$HA" -gt $((CW / 2)) ] && echo ctrl+Left || echo ctrl+Right )
  xdotool key --clearmodifiers --repeat $HIGH_FRAME_STEPS --repeat-delay 60 $STEPKEY
  sleep 1.0
  shot "$SHOTS/anchor_08_high_frameN.png"
  rawline "$SHOTS/anchor_08_high_frameN.png" "$SCRATCH/hn.raw"
  HB=$(col "$SCRATCH/hn.raw" "$PLR" "$PLG" "$PLB")
  echo "HIGH_PPF=\"$HA $HB\"" >> "$GEOM"
  say "$HIGH_FRAME_STEPS frame steps at the high zoom moved the playhead $HA -> $HB"

  # (3) THE PAN AT THAT ZOOM: the same drag, with a frame now several pixels wide - a
  # whole-frame carrier misses by a frame, which at this zoom is a visible distance. Toward
  # the middle again, so the playhead stays inside the measured crop.
  #
  # THIS PAN IS MEASURED AS THE APPLICATION BEHAVES, INCLUDING A DEFECT IT EXPOSES: the
  # previous pan left a remainder in TimeLineWidget::mPendingScrollFrames, which is counted
  # in FRAMES, and the 21 notches between the two pans re-price it (see the assertion at the
  # end of this file). The probe does NOT flush or evade it - the claim is that a pan is
  # exact, and a gesture sequence a user can perform is what has to satisfy it.
  PAN2_DX=$PAN_PIXELS
  [ "$HB" -gt $((CW / 2)) ] && PAN2_DX=$((-PAN_PIXELS))
  echo "PAN2_DIR=\"$PAN2_DX\"" >> "$GEOM"
  xdotool mousemove --sync $((CL + HB)) $MIDY
  sleep 0.3
  xdotool mousedown 2
  sleep 0.2
  xdotool mousemove --sync $((CL + HB + PAN2_DX)) $MIDY
  sleep 0.3
  xdotool mouseup 2
  sleep 1.0
  shot "$SHOTS/anchor_09_pan_zoomed_after.png"
  rawline "$SHOTS/anchor_09_pan_zoomed_after.png" "$SCRATCH/pc.raw"
  echo "PAN2_X=\"$HB $(col "$SCRATCH/pc.raw" "$PLR" "$PLG" "$PLB")\"" >> "$GEOM"
  say "the pan at the high zoom ($PAN2_DX px) was measured"

  if alive; then
    echo "ALIVE=\"yes\"" >> "$GEOM"
  else
    echo "ALIVE=\"no\"" >> "$GEOM"
    echo "APP_TAIL=\"$(tail -2 "$APPLOG" | tr "\n" "|")\"" >> "$GEOM"
    # The abort is the application log own, not this probe own log: a host that dies drawing
    # a frame says why there.
    say "the application DIED during the gestures"
  fi
  kill $app 2>/dev/null
  wait $app 2>/dev/null
  say "body done"
' 2>&1 | tail -4

. "$GEOM" 2>/dev/null
if [ -n "${ABORT:-}" ]; then
    echo "steps: $(tr "\n" ";" < "$STEPS" 2>/dev/null)"
    check 1 "the probe reached the docked panel through the dock own route (it stopped: ${ABORT})"
    summary probe_graph_anchor
fi
if [ -z "${MOVED:-}" ] || [ "$MOVED" = "none" ]; then
    echo "steps: $(tr "\n" ";" < "$STEPS" 2>/dev/null)"
    check 1 "the playhead is identified by a pan that moves it (the pan moved nothing in the ${CROP:-?} crop of the plot [${PLOT:-?}], or the playhead is out of view)"
    summary probe_graph_anchor
fi

read -r DX DY DW DH <<< "${DOCK:-0 0 0 0}"
# The dock AS THE GRAPH PAGE SHOWS IT. Its width is not the timeline page's (that page's own
# controls are narrower), so this is the rectangle the published plot has to be inside.
read -r DGX DGY DGW DGH <<< "${DOCK_G:-0 0 0 0}"
read -r t1x t1y t1w t1h <<< "${TAB_T_AT_TL:-0 0 0 0}"
read -r g1x g1y g1w g1h <<< "${TAB_G_AT_TL:-0 0 0 0}"
read -r t2x t2y t2w t2h <<< "${TAB_T_AT_G:-0 0 0 0}"
read -r g2x g2y g2w g2h <<< "${TAB_G_AT_G:-0 0 0 0}"
read -r PLX PLY PLW PLH <<< "${PLOT:-0 0 0 0}"
read -r FLT FLH <<< "${FINDER_ROWS:-0 0}"
read -r PX0 PX1 <<< "${PAN_X:-0 0}"
read -r FX0 FX1 <<< "${FRAME_X:-0 0}"
read -r ZX0 ZX1 <<< "${ZOOM_X:-0 0}"
read -r HX0 HX1 <<< "${HIGH_ANCHOR:-0 0}"
read -r HP0 HP1 <<< "${HIGH_PPF:-0 0}"
read -r P20 P21 <<< "${PAN2_X:-0 0}"

# ---- 1. THE ROUTE: THE DOCK OWN TAB, AND THE PANEL OWN PUBLISHED PLOT ----
# Both are asserted BEFORE the claims, because a claim measured in the wrong rectangle is a
# claim about another widget - which is exactly what the deleted overlay route would now be.
tabs_ok=1
[ "$t1x" -lt "$g1x" ] || tabs_ok=0
[ "$t1x" -ge "$DX" ] && [ "$t1y" -ge "$DY" ] && [ $((t1x + t1w)) -le $((DX + DW)) ] \
    && [ $((t1y + t1h)) -le $((DY + DH)) ] || tabs_ok=0
[ "$g1x" -ge "$DX" ] && [ "$g1y" -ge "$DY" ] && [ $((g1x + g1w)) -le $((DX + DW)) ] \
    && [ $((g1y + g1h)) -le $((DY + DH)) ] || tabs_ok=0
check "$((1 - tabs_ok))" "the dock header publishes two page tabs, Timeline in the left slot and Graph in the right ([${TAB_T_AT_TL:-none}] and [${TAB_G_AT_TL:-none}]), both inside the dock's own AT-SPI rectangle [${DOCK:-none}]: the probe presses the control it names"
gswitch_ok=1
# THE CURRENT TAB IS THE WIDE ONE, and the strip it is inflated to starts at the LEFT slot:
# with Graph current its rectangle is the union of both slots (x 8, 120px), which is why the
# two x values are EQUAL here and only the widths say which page the dock is showing.
[ "$g2w" -gt "$t2w" ] || gswitch_ok=0
check "$((1 - gswitch_ok))" "and pressing the tab BY NAME made the Graph tab the current one: [${TAB_G_AT_G:-none}] against Timeline [${TAB_T_AT_G:-none}] - the tab bar itself says which page the dock is showing"

inside_ok=1
[ "$PLW" -gt 0 ] && [ "$PLH" -gt 0 ] || inside_ok=0
[ "$DGW" -gt 0 ] || inside_ok=0
[ "$PLX" -ge "$DGX" ] && [ "$PLY" -ge "$DGY" ] && [ $((PLX + PLW)) -le $((DGX + DGW)) ] \
    && [ $((PLY + PLH)) -le $((DGY + DGH)) ] || inside_ok=0
check "$((1 - inside_ok))" "and the panel publishes its own plot as the box of its Fit graph action, inside the dock's page rectangle measured with the Graph page current: [${PLOT:-none}] inside [${DOCK_G:-none}] (the dock's rectangle while the timeline was up was [${DOCK:-none}], $((DGW - DW))px narrower - the page is the one that is showing). Its own left edge is measured separately, below. The crop is the application's own geometry, not this probe's"

# THE PAGE'S LEFT EDGE, measured ON THE GRAPH PAGE (see the body): it is where the tree
# splitter's hairline is, and it is NOT the timeline page's edge - the dock and that boundary
# both move when this page becomes current.
INSET=-1
[ -n "${PAGE_LEFT:-}" ] && INSET=$((PLX - PAGE_LEFT))
echo "geometry: dock with the timeline up [${DOCK:-none}] (tree ${TREE_W:-none}px, longest hairline run ${TREE_RUN:-0} of ${DW}px, hairline colour ${HAIR_COLOUR:-none}), dock with the Graph page up [${DOCK_G:-none}], graph page left ${PAGE_LEFT:-unmeasured} (body background ${PAGE_BG:-none}), plot [${PLOT:-none}] -> inset ${INSET}px, ruler band [${rulerBand:-none}] (the model rulerBand: plot.left(), plot.top()-22, plot.width(), 22), crop [${CROP:-none}], notch witness [${NOTCH_CROP:-none}] (band + plot), finder rows [${FLT:-?}..$((FLT + FLH - 1))] (${FLH:-0} rows)"
inset_ok=0
[ -n "${PAGE_LEFT:-}" ] && [ "$INSET" -ge "$PLOT_PAGE_INSET_MIN" ] && [ "$INSET" -le "$PLOT_PAGE_INSET_MAX" ] && inset_ok=1
check "$((1 - inset_ok))" "the published plot's left edge sits the scaler's own inset past the Graph page's left edge, measured on that page's own splitter hairline: plot x ${PLX:-?} minus page x ${PAGE_LEFT:-unmeasured} = ${INSET}px, inside [${PLOT_PAGE_INSET_MIN},${PLOT_PAGE_INSET_MAX}] (kMargin 4 + axisLabelWidth, 46..96 behind it)"
assert_rendered "$SHOTS/anchor_01_graph_page.png" "the docked graph panel's page"

# ---- 2. THE CLAIMS ----
pan=$((PX1 - PX0)); [ "$pan" -lt 0 ] && pan=$((-pan))
zoom=$((ZX1 - ZX0)); [ "$zoom" -lt 0 ] && zoom=$((-zoom))
high_anchor=$((HX1 - HX0)); [ "$high_anchor" -lt 0 ] && high_anchor=$((-high_anchor))
pan2=$((P20 - P21)); [ "$pan2" -lt 0 ] && pan2=$((-pan2))
ppf=$(awk -v a="$FX1" -v b="$PX1" -v n="$FRAME_STEPS" 'BEGIN { printf "%.4f", (a - b) / n }')
# The frame steps run toward the middle of the plot, so the sign is the run's business: the
# rate is the movement over the step, in either direction.
high_ppf=$(awk -v a="$HP1" -v b="$HP0" -v n="$HIGH_FRAME_STEPS" 'BEGIN { printf "%.4f", (a > b ? a - b : b - a) / n }')
changed=${ZOOM_PIXELS%% *}
echo "measured: playhead rgb ${COLOUR:-?} at ${PX0:-?}px into the ${CROP:-?} crop, ${ppf} px/frame, pan ${pan}px of the ${PAN_PIXELS}px dragged, one notch moved it ${zoom}px (${changed:-0} px changed in the band+plot witness [${NOTCH_CROP:-?}], bar ${NOTCH_CHANGE_MIN}), ${HIGH_ZOOM_NOTCHES} notches held it within ${high_anchor}px at ${high_ppf} px/frame, pan at that zoom ${pan2}px"
echo "steps: $(tr "\n" ";" < "$STEPS" 2>/dev/null)"
[ "${ALIVE:-no}" = "yes" ]
alive_rc=$?
alive_msg="the application survived every gesture on the docked panel (ALIVE=${ALIVE:-?})"
[ "$alive_rc" -eq 0 ] || alive_msg="the application was NOT running when the gestures ended (ALIVE=${ALIVE:-?}, its log ended [${APP_TAIL:-none}])"
check "$alive_rc" "$alive_msg"

check 0 "the playhead is found and moved by the pan (column ${PX0:-?} -> ${PX1:-?}, colour ${COLOUR:-?}, found as a full-height 1px run below the ${RULER_BAND}px ruler band: ${LINE_RUN:-unmeasured} - the pill's block is not in the scanned rows at all)"
check "$([ "$pan" -gt $((PAN_PIXELS - 3)) ] && [ "$pan" -lt $((PAN_PIXELS + 3)) ] && echo 0 || echo 1)" \
    "a ${PAN_PIXELS}px middle-drag moves the content ${PAN_PIXELS}px, not a frame's worth (measured ${pan}px of the ${PAN_PIXELS}px dragged, ${PX0:-?} -> ${PX1:-?}, at ${ppf} px/frame)"
check "$([ "$zoom" -le 1 ] && echo 0 || echo 1)" \
    "a wheel notch with the pointer ON a frame holds that frame still (measured ${zoom}px of ${ZX0:-?} -> ${ZX1:-?})"
check "$([ "${changed:-0}" -gt "$NOTCH_CHANGE_MIN" ] && echo 0 || echo 1)" \
    "and the same notch really zoomed (${changed:-0} px changed in the band+plot witness [${NOTCH_CROP:-none}]: ruler band [${rulerBand:-none}] + plot [${PLOT:-none}], bar ${NOTCH_CHANGE_MIN} - the band's static chrome and the empty plot compare to 0 on a stale capture)"
# THE CLIMB IS NOT THE ONE NOTCH ABOVE, AND THE DIFFERENCE IS MEASURED, NOT ASSUMED. Each notch
# re-anchors on the TIMELINE's INTEGER content axis - TimeLineWidget::zoomFramesAbout rounds the
# anchor's content pixel and TimeLineEditor::anchorPixelAfterScale rounds the scaled one - which
# is up to ~0.8 of this panel's pixels per notch and does not cancel. MEASURED over this climb:
# 2px at 6.8 px/frame (0.3 of a frame) on the overlay recording, and 6px at 7.0 px/frame (0.86
# of a frame) on the docked page, against the ONE notch's exact 0px above both times - the
# handler is exact, the repeated step accumulates. The bar is what "anchored" means where a
# frame is SEVERAL pixels - the anchored frame stays under the pointer to better than HALF a
# frame at the zoom reached - and it is the design's invariant, so it is NOT relaxed to admit
# the drift: the run prints its own px and px/frame either way.
check "$(awk -v a="$high_anchor" -v b="$high_ppf" 'BEGIN { print (b > 0 && a * 2 < b) ? 0 : 1 }')" \
    "$HIGH_ZOOM_NOTCHES notches on the playhead leave the anchored frame under the pointer to better than half a frame at the zoom they reach (measured ${high_anchor}px of ${high_ppf} px/frame, ${HX0:-?} -> ${HX1:-?}, while the one notch above held exactly)"
check "$(awk -v a="$ppf" -v b="$high_ppf" 'BEGIN { print (a > 0 && b > a * 3) ? 0 : 1 }')" \
    "and they climbed the zoom (${ppf} -> ${high_ppf} px/frame)"
# ---- THE PAN AFTER THE ZOOM IS A DEFECT, NOT THE CAMERA CLAMP ----
#
# The claim is kept exact, so this FAILs until TimeLineWidget is fixed. The failure is
# reported as the defect it is, with the arithmetic that rules the clamp out and names the
# cause, rather than by widening the tolerance to the measured 37px.
#
# THE CLAMP IS EXCLUDED. updateCamera (src/gui/TimeLineWidget.cpp) bounds the view to an
# OVERLAP: mHorizontalOffset in [-contentSpan, viewport], where
# contentSpan = frameEndPixel + kTimeLineMargin and viewport = mInner->width(). The two bounds
# are "the content's start at the view's right edge" and "the content's end at the view's left
# edge". This run's numbers: the timeline page is DW - PAGE_LEFT = 933 - 268 = 665px (the
# viewport), the panel maps that same window into the plot inset CW = 587 - 16 = 571px, and
# the measured 7.00 px/frame is therefore 7.00 * 665/571 = 8.14 px/frame on the timeline's
# content axis (the timeline's seeded rung, index 1, is 1.1 = 0.945 * 665/571 - the same
# ratio). The 600-frame content is 24 + 600*8.14 + 24 = 4932px long; the playhead is on frame
# ~25-26 (FRAME_STEPS + HIGH_FRAME_STEPS steps from the new project's first frame) and its
# column is 114/7.00 = 16.3 frames into the window, so the view starts at ~9 frames, offset
# ~ -100px. Room is ~665 - (-100) = ~765px toward the content's start and
# ~-100 + 4932 = ~4830px toward its end, against the 40 * 665/571 = 46.6 content px the drag
# asks for. The content end is NOT reachable within 40px at this zoom - it is thousands of px
# away - so no clamp is the cause.
#
# THE CAUSE, READ OFF THE CODE: TimeLineWidget::scrollFramesBy carries mPendingScrollFrames in
# FRAMES (delta = lround(pending * rate); pending -= delta/rate), and zoomFramesAbout never
# touches it. A remainder of R frames is R * rate pixels, so the carry is sub-pixel only while
# the rate is constant - and a notch multiplies the rate by 1.1. The earlier pan asked for
# 46.58 content px, lround applied 47, leaving +0.377 frames; at the old 1.1 px/frame that was
# 0.42px (exactly what the carry exists for); the 1 + HIGH_ZOOM_NOTCHES notches multiply the
# rate by 1.1^21 = 7.400, so the same 0.377 frames is 3.07px at 8.14 px/frame. This pan then
# computes lround(0.377*8.14 - 46.58) = lround(3.07 - 46.58) = -44 content px - 44 * 571/665 =
# 37.8px of exact panel shift, which the per-column rounding draws as the measured 37 (114 ->
# 151) - where 47 content px would have drawn 40.4px. The carry is not scoped to a drag
# either: GraphEditor::endPan clears only the pointer origin, and nothing in TimeLineWidget
# clears mPendingScrollFrames, so it survives any gesture between two pans.
#
# THE PENDING ZOOM REMAINDER CANNOT DO THIS: mPendingZoomPixels is a PIXEL quantity (the
# zoom's view translation is the content axis, 1:1 with screen pixels, so a scale change does
# not re-price it) and it is read only by zoomFramesAbout through
# ctrl::TimeLineEditor::zoomAnchorDelta - never by scrollFramesBy. Nothing to do in the probe.
#
# THE FIX (not a probe edit): keep the pan remainder in CONTENT PIXELS (pending +=
# aFrameDelta * rate; delta = lround(pending); pending -= delta), which is invariant under a
# scale change; the minimal alternative is to drop mPendingScrollFrames in zoomFramesAbout
# once updateWheel has moved the scale.
pan2_requested=unknown
if [ -n "${PAGE_LEFT:-}" ] && [ "${CW:-0}" -gt 0 ]; then
    pan2_requested=$(awk -v p="$PAN_PIXELS" -v i="$((DW - PAGE_LEFT))" -v c="$CW" \
        'BEGIN { printf "%.2f", p * i / c }')
fi
zoom_gain=$(awk -v n="$((HIGH_ZOOM_NOTCHES + 1))" 'BEGIN { printf "%.3f", 1.1 ^ n }')
check "$([ "$pan2" -gt $((PAN_PIXELS - 2)) ] && [ "$pan2" -lt $((PAN_PIXELS + 2)) ] && echo 0 || echo 1)" \
    "DEFECT, not the camera clamp: a ${PAN_PIXELS}px middle-drag after the zoom must still move the content exactly ${PAN_PIXELS}px and moves ${pan2}px (${P20:-?} -> ${P21:-?}, dragged ${PAN2_DIR:-?}px; the camera's overlap bounds are hundreds of px away, while the drag asks for ${pan2_requested} content px). TimeLineWidget::scrollFramesBy carries mPendingScrollFrames in FRAMES across the zoom: the earlier pan left ~0.38 frame, and the ${zoom_gain}x rate change re-prices it from a sub-pixel 0.4px to ~3.1px, so this pan applies ~44 content px where ${pan2_requested} was asked - 37.8px of panel shift, drawn as the measured ${pan2}px. Fix: drop mPendingScrollFrames when the scale changes (or carry it in pixels). The exact claim is NOT relaxed"
summary probe_graph_anchor
