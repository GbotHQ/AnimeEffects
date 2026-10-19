#!/bin/bash
# THE GRAPH PANEL IN THE TIMELINE DOCK, ON A LIVE APPLICATION.
#
# THE PANEL MOVED. It used to be a full-viewport Dear ImGui overlay toggled from the Tools
# dock; it is now the dock's own Graph tab (gui::TimeLineHeader's tab bar, drawn by
# gui::GraphPanelWidget at the dock's size on the dock's opaque floor, with the object tree
# still beside it). The overlay goes away in the commit after this one, so every claim here is
# made through the DOCK's own route: the tab by name, the dock's AT-SPI rectangle, and the
# dock's own pixels. Nothing here touches the old toggle and nothing here reads a viewport
# crop: the panel is not on the viewport any more.
#
# WHAT THIS PROBE PROVES, AND WHY A PROBE AT ALL. tests/graph_panel_pixels.cpp renders the
# panel from a synthetic model and reads the framebuffer back, and tests/graph_panel_model.cpp
# pins the geometry the drawing layer walks; both pass whether or not the panel is ever put on
# a real dock, for the layers the object tree actually has selected. What only a running
# application can show is that composition, and that is what is asserted here:
#
#   1. THE DOCK'S TAB IS THE ROUTE, AND THE TAB BAR ITSELF SAYS SO. The header publishes two
#      AT-SPI page tabs, "Timeline" and "Graph"; pressing one by name makes it the current one,
#      which the tabs' own rectangles report (MEASURED: the two keep their own slots - Timeline
#      at x=8, Graph at x=68 - and the CURRENT one's rectangle is inflated to the whole strip,
#      120px against the other's 60px). The probe asserts the geometry AND the model's own state,
#      so a restyled tab bar still has to be a working switch.
#
#   2. THE PANEL PUBLISHES ITSELF, BY NAME, ONLY WHILE IT DRAWS. With the Graph tab current the
#      panel is a named Pane whose name IS the model's summary ("No keys on the selected
#      layers", "N channel(s) shown", ...) and whose children are that summary as a label and
#      "Fit graph" - the panel's ONE control, the channels button, is NOT among them: it lives
#      in the dock HEADER (TimeLineHeader's own Qt button), so the panel publishes no channel
#      control of its own; the fit button appears EXACTLY ONCE in the whole tree, which is what
#      rules a second publication out (the docked host takes the panel's ONE accessibility
#      installation - gui::a11y::install is called for one widget and kept - and two hosts
#      publishing one panel is exactly the bug that would hide here). With the Timeline tab
#      current the SAME node is still in the tree,
#      unnamed and childless: the hidden page publishes the empty model instead of leaving the
#      last drawn frame in the tree.
#
#   3. THE PIXELS ARE THE DOCK'S OWN. Switching the tab changes a bounded number of pixels and
#      ALL of them are inside the Animation dock's AT-SPI rectangle: the panel is drawn in the
#      dock's page area, not over the screen. This is the docked form of the claim the old
#      overlay probe made with a viewport crop, and it is stronger where it matters: the docked
#      panel paints its own OPAQUE floor (theme::Colors::base; GraphPanelWidget::paintGL), so
#      what changes inside that rectangle is the panel's own ink on its own surface, with no
#      artwork composited under it.
#
#   4. THE KEYED FRAME DRAWS, AND ITS INK IS THE MODEL'S. Two channels of different key TYPES
#      are coloured differently (theme::Graph::keyColor, the same token the timeline paints
#      those types with), the DOCK HEADER's channels control is present and names the count it
#      changes (the panel no longer draws one, and no longer paints any gesture numbers either -
#      the drag readout it used to print at the pointer is gone). The keys come from the
#      application's own routes: "Add default keys" in the object tree's row menu puts ten
#      channels at frame 0, and the Properties dock's Current Keys knobs put a key of TWO key
#      TYPES at the frame the playhead has been moved to (a frame-0 marker is culled at the
#      plot's left edge, so the knobs are what makes the plot show keys at all).
#
# WHY THAT FRAME USED TO KILL THE APPLICATION, AND WHAT NOW PINS IT (measured 2026-10-18). The
# docked host used to abort on the first frame it had to STROKE: gui::GraphPanelWidget::paintGL
# -> ImGui_ImplOpenGL3_RenderDrawData -> gui::stroke's draw callback ->
# gl::EasyShaderProgram::makeSureVBO -> gl::BufferObject::resetRawData -> GL_CHECK_ERROR ->
# Q_ASSERT(0), "OpenGL Error (code:1282)". The mechanism was that gui::stroke's pass (its VAO
# above all) was a process-global created by whichever host ran a frame first - the viewport's
# overlay - and a vertex-array object is not shared between OpenGL contexts, while
# glBindVertexArray on a name the CURRENT context never generated is GL_INVALID_OPERATION. The
# empty panel drew no stroke, which is why every claim above it measured fine. The fix (the
# overlay host deleted, one context owning the panel's GL objects, and the frame driver
# rebuilding them if another context ever turns up) is what this phase now exercises: if the
# process dies on the keyed frame again, the crash's own log tail is printed beside the failure.
#
# WHAT THIS PROBE DELIBERATELY DOES NOT ASSERT, AND WHERE THOSE CLAIMS ARE GATED INSTEAD. The
# tick ALPHAS (0.85 / 0.60 / 0.20), the selection EMPHASIS (full strength for a channel that owns
# a selected key, 0.45 for every other curve, everything dimmed with an empty selection) and the
# marker's three INKS (selected / sibling / hovered) are NOT measurable on a live screen with
# this harness, and that is a property of the screen rather than of the panel:
#   * a frame tick and the frame NUMBER that belongs to it share one column, and on this build
#     their two bands merge into ONE run in a difference, so "which band is the ticks" would be
#     a heuristic;
#   * the marker's rings are 1px strokes at 0.95 and 0.35 over its own fill, which is the same
#     pixel count at the widths involved - a 1px arc is not separable by counting ink;
#   * the panel's text ink is neutral and thin, over a floor the drawing layer may re-tint
#     between frames, which a hue count cannot difference.
# Offscreen, where nothing depends on a live window, ALL of them ARE gated:
#   * tests/graph_panel_pixels.cpp - a model whose frame ticks carry all three levels, read off
#     the rendered image (the alphas); the key-marker pair plus the stroke group's
#     dimmed/full-strength split (the emphasis, in both states); and the three marker inks, read
#     through a two-row probe because ImGui strokes a circle as a POLYLINE;
#   * tests/graph_panel_model.cpp - the published tick levels, the per-channel classification and
#     the per-key selection flags the drawing layer reads.
# See "WHAT IS NOT ASSERTED HERE" at the foot of this file, beside the assertions dropped for
# that reason.
#
# WHERE EVERY NUMBER COMES FROM. The dock's rectangle is the AT-SPI rectangle of the widget named
# "Animation" (the dock itself); the tabs are addressed by name; and the panel's PAGE AREA - used
# by the keyed phase only - is the dock's right-hand part, whose left edge is the timeline's,
# measured off the ruler's bottom hairline exactly as probe_timeline_ink.sh measures it (the
# hairline spans the timeline and not the tree column, and nothing publishes the tree's width
# (MEASURED: 189px for one project's row names, 226px for another's, which is why it is measured
# rather than assumed). Widgets are addressed by name (ae_ui.py); the row menu that
# adds the keys is driven the way these probes have always driven a QMenu: a top-level popup at the
# click point, focused by id, its entries reached with Up, and a menu too short to be the child row
# menu REFUSED rather than pressed blind (the short one is the top node's, and pressing it opens
# Create layer).
#
# WHICH PROJECT THIS NEEDS. Any .anie with at least two object rows, one of them a layer: the keys
# are created by the probe itself. A project whose rows have no timeLine leaves the panel with no
# channels, and the keyed phase then fails loudly naming that instead of skipping - the crash
# above is the only thing this probe skips on.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

GEOM=/tmp/probe_graph_ink.txt
SCRATCH=/tmp/probe_graph_ink
INK="$ROOT/scripts/gui/ae_ink.py"
APPLOG=/tmp/probe_graph_ink_app.log
rm -f "$GEOM"
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"

# THE TAB SWITCH IS A REPAINT AND NOT A TRANSIENT: the panel re-reads the document when it
# becomes visible (TargetWidget::onSurfaceChanged -> GraphPanelWidget::refresh), and the diff
# between the two pages is taken from settled captures. MEASURED on this layout: 8880px changed
# inside the dock's 788x310 rectangle (3.6%) with nothing selected and 21362px (8.7%) with a layer
# selected - the panel page draws its labels and the timeline page its lanes, so the number moves
# with the project; the bar is a fifth of the smaller one.
DOCK_CHANGE_MIN=2000
# ...and it is BOUNDED: a change that swallowed the whole dock would make "the panel is in there"
# unfalsifiable, and it would mean the measurement caught a repaint of everything (MEASURED 3.6%
# and 8.7% in the two states above).
DOCK_CHANGE_SHARE=0.50
# A channel hue on the panel's floor. 20 admits the antialiased edge of a stroke (the token the
# timeline's own ink probe uses for the same colours).
HUE_TOL=20
# The floor of the chromatic-ink scan: any colour this far from the floor with this much spread is
# ink the panel drew, and every one of them is measured. A hue needing a curve's share of the plate
# (HUE_CURVE_MIN below) is what the two-colour claim uses; the insert uses ANY of them, because a
# point on any stroke will do.
HUE_MIN_PX=6
# A CURVE'S share of the panel: a channel's curve is a stroke across most of the plot and measures
# MEASURED 475..886 px at 0.45 strength (two curves of one hue when the channel has two
# components), while a hue that is only an antialiasing edge of one measures single digits. 100 is
# five times above that noise and a fifth of the smallest curve.
HUE_CURVE_MIN=100
# ...and its width: a curve spans hundreds of COLUMNS, while the playhead is ONE 1px column. A hue
# that only ever owns a handful of columns is not a channel's curve, which is how the playhead
# stays out of the pair below without hardcoding the accent.
HUE_COLS_MIN=5
# Two key TYPES are two entries of theme::Graph::keyColor, and the palette's nearest pair is
# further apart than this. The timeline's own ink probe uses the same bar for the same token.
HUE_SEP=40
# THE HOVER'S OWN BOX, AND ITS BAR. A hovered marker brightens its FILL (gui::drawPanel:
# mixedToward(curve.color, axisText, kKeyHoverFillMix), 0.4 of the way to the text ink) and that
# fill is a disk of radius ctrl::graph::kKeyDotRadius - 3.75px, so about 44px of ink - inside the
# edge stroke the pointer never touches. The box below is the 13x13 square that holds that disk,
# and the bar is under a fifth of it: a state change this phase can clear, and a bar a same-state
# control pair in the same page clears with 0px cannot fake. The reading is asked of the marker's
# OWN box and not of a share of the page, because a page-wide bar would pass on any repaint that
# had nothing to do with the marker.
HOVER_BOX_PX=13
HOVER_BOX_MIN=8
# THE HEADER VIEW BUTTON'S OWN BAR, on the panel's page. One zoom-in notch is one rung of the
# frame ladder (x1.1, ctrl::time::Scaler::kZoomPerNotch): the shared frame window shrinks about
# the playhead's frame, so every tick, number, curve sample and key marker away from the anchor
# lands on a new pixel and the plate is re-stroked - hundreds of the page's pixels. 200 is the
# floor below which the re-map would have had to do almost nothing, and the same page's
# same-state control pair (section 4) measures 0.
ZOOM_PAGE_MIN=200
# The readout's constants (RO_*) went with the readout itself: the panel draws no gesture numbers
# at the pointer any more (the drag deltas are still computed for the edit - only the printed
# readout is gone), so there is no box to measure and no bar to compare against.
export GEOM SCRATCH INK APPLOG DOCK_CHANGE_MIN DOCK_CHANGE_SHARE HUE_TOL HUE_CURVE_MIN \
       HUE_COLS_MIN HUE_SEP HUE_MIN_PX HOVER_BOX_PX HOVER_BOX_MIN ZOOM_PAGE_MIN

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  set -u
  cd "$AE_PROBE_DIR"
  "$APP" "${PROJ:-}" >"$APPLOG" 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID

  abort() { printf "ABORT=\"%s\"\n" "$1" >> "$GEOM"; kill $app 2>/dev/null; wait $app 2>/dev/null; exit 0; }
  # EVERY STEP IS RECORDED AS IT HAPPENS: the body stdout is piped through tail, so a hang prints
  # nothing at all and a run that died at step 7 looks like one that never started.
  STEPS=/tmp/probe_graph_ink_steps.log
  : > "$STEPS"
  say() { echo "t+${SECONDS}s  $*" >> "$STEPS"; }
  alive() { kill -0 "$app" 2>/dev/null; }

  wait_ready 120 || abort "the main window never came up (wait_ready timed out)"
  W=$(xdotool getactivewindow 2>/dev/null || xdotool search --name AnimeEffects | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  sleep 0.6

  raw() { magick "$1" -crop "$2" +repage -depth 8 rgb:"$3"; }
  inkpx() { python3 "$INK" "$@"; }
  # The pointer rests on the File menu for every capture the probe takes: the panel has hover
  # states of its own (a marker brightens, a chip appears, tangent handles highlight), so a
  # capture taken with the pointer over the plot would measure one of them.
  FR=$(ae extents "File")
  [ -n "$FR" ] || abort "no File menu: this is not the application window"
  read -r FX FY FW FH <<< "$FR"
  POINTX=$((FX + 3)); POINTY=$((FY + FH / 2))
  park() { xdotool mousemove --sync $POINTX $POINTY; sleep 0.5; }
  shot() { park; import -window root "$1"; }
  # settle_shot <png>: capture AT THE POINTER THE CALLER PUT IT (shot() parks first, this one
  # does not, because a hover capture must be taken with the pointer still on its target) and
  # repeat until two captures in a row agree, so a frame caught mid-repaint is never the reading
  # a phase is decided on. Bounded, and false when the page never settles - a page that keeps
  # repainting under a stationary pointer is itself worth failing on rather than measuring.
  settle_shot() {
    local i=0 d
    while [ "$i" -lt 10 ]; do
      import -window root "$1"
      if [ "$i" -gt 0 ]; then
        d=$(magick compare -metric AE "$SCRATCH/settle_prev.png" "$1" null: 2>&1); d=${d%% *}
        if [ "${d:-x}" = "0" ]; then cp "$1" "$SCRATCH/settle_prev.png"; return 0; fi
      fi
      cp "$1" "$SCRATCH/settle_prev.png"
      i=$((i + 1)); sleep 0.4
    done
    return 1
  }
  # sel <row name>: AT THE ROW LEFT EDGE, never its reported centre - the cell rectangles the tree
  # publishes are WIDER THAN THE TREE, so a click at the centre lands beside the row.
  sel() {
    local r x y w h
    r=$(ae extents "$1" "table cell")
    if [ -n "$r" ]; then
      read -r x y w h <<< "$r"
      xdotool mousemove --sync $((x + 4)) $((y + h / 2)) click 1
      return 0
    fi
    ae click "$1" "table cell" >/dev/null 2>&1
  }
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

  # ---- the dock, and the two tabs its header publishes ----
  DOCK=$(ae extents "Animation")
  [ -n "$DOCK" ] || abort "no visible widget named Animation: this is not the docked layout, so there is no dock to measure the panel in"
  read -r DX DY DW DH <<< "$DOCK"
  echo "DOCK=\"$DOCK\"" >> "$GEOM"
  say "dock $DX,$DY ${DW}x${DH}"

  echo "TAB_T_AT_TL=\"$(ae extents "Timeline")\"" >> "$GEOM"
  echo "TAB_G_AT_TL=\"$(ae extents "Graph")\"" >> "$GEOM"
  shot "$SHOTS/gink_10_dock_timeline.png"
  ae dump > "$SCRATCH/dump_timeline.txt" 2>/dev/null
  say "timeline state captured"

  tab "Graph" || abort "the dock header has no pressable tab named Graph: the panel cannot be shown by the dock own route"
  echo "TAB_T_AT_G=\"$(ae extents "Timeline")\"" >> "$GEOM"
  echo "TAB_G_AT_G=\"$(ae extents "Graph")\"" >> "$GEOM"
  shot "$SHOTS/gink_20_dock_graph.png"
  ae dump > "$SCRATCH/dump_graph.txt" 2>/dev/null
  say "graph state captured"

  # ...and back to the timeline, for the pair the pixel claim is built from, plus a CONTROL
  # capture of the same state twice: the probe must not read a repaint as the switch.
  tab "Timeline" || abort "the dock header stopped answering its Timeline tab"
  shot "$SHOTS/gink_11_dock_timeline_b.png"
  shot "$SHOTS/gink_12_dock_timeline_c.png"
  say "timeline state re-captured (the control pair)"

  # ---- the tree edge, which is where the panel page starts ----
  # The ruler bottom hairline spans the timeline and NOT the tree column, so the first long run of
  # the panel modal colour in row TY0-1 IS the timeline x range (probe_timeline_ink.sh measures it
  # this way; the tree is MEASURED as 189px of this dock while older probes assume 72).
  TY0=$((DY + 37 + 40))
  raw "$SHOTS/gink_10_dock_timeline.png" "${DW}x1+${DX}+$((TY0 - 1))" "$SCRATCH/hairline.raw"
  HAIRC=$(inkpx modal "$SCRATCH/hairline.raw" $DW 1 0 0 $DW 1 | cut -d" " -f1-3)
  case "$HAIRC" in ""|none) abort "row $((TY0 - 1)) has no modal colour: nothing to locate the tree edge with" ;; esac
  read -r HC1 HC2 HC3 <<< "$HAIRC"
  inkpx cols "$SCRATCH/hairline.raw" $DW 1 0 0 $HC1 $HC2 $HC3 2 1 > "$SCRATCH/hair_rows.txt"
  TREE_W=0; TLL=0
  while read -r a b n; do
    case "$a" in ""|none) continue ;; esac
    if [ $((b - a + 1)) -ge 10 ] && [ "$TREE_W" -eq 0 ]; then TREE_W=$a; fi
    if [ $((b - a + 1)) -gt "$TLL" ]; then TLL=$((b - a + 1)); fi
  done < "$SCRATCH/hair_rows.txt"
  [ "$TREE_W" -gt 0 ] && [ "$TLL" -ge $((DW / 4)) ] || abort "row $((TY0 - 1)) holds no run of the panel modal colour rgb ${HAIRC} that could be the ruler bottom hairline (first long run at ${TREE_W}, longest ${TLL} of ${DW}px)"
  echo "TREE_W=\"$TREE_W\"" >> "$GEOM"
  say "the object tree is ${TREE_W}px of the dock; the panel page starts there"

  # ---- keys, through the object tree own row menu ----
  # THE FIRST TREE ROW IS THE TOP NODE AND ITS LAYERS STAY INVISIBLE UNTIL IT IS EXPANDED; the only
  # expander is the branch indicator at (rowX - 12, rowY + rowH/2) on that row own rectangle, and
  # it is a TOGGLE. It is clicked ONCE and the row count is asserted to grow.
  cells() { ae dump 2>/dev/null | sed -n "s/^ *table cell *| //p"; }
  ROWS=$(cells | sed -n 1,8p)
  NROWS=$(printf "%s\n" "$ROWS" | grep -c .)
  say "tree rows: $NROWS"
  if [ "$NROWS" -lt 3 ]; then
    TOP=$(printf "%s\n" "$ROWS" | sed -n 1p)
    R=$(ae extents "$TOP" "table cell")
    [ -n "$R" ] || abort "the first tree row ($TOP) has no rectangle to expand"
    read -r EX EY EW EH <<< "$R"
    xdotool mousemove --sync $((EX - 12)) $((EY + EH / 2)) click 1
    sleep 1.5
    ROWS=$(cells | sed -n 1,8p)
    NROWS=$(printf "%s\n" "$ROWS" | grep -c .)
    say "tree rows after the expansion: $NROWS"
  fi
  echo "ROWS=\"$(printf "%s" "$ROWS" | tr "\n" ";")\"" >> "$GEOM"
  [ "$NROWS" -ge 2 ] || abort "the tree publishes ${NROWS} row(s) and expanding its top node did not grow it: the row menu offers Add default keys only for a node WITH a parent, so no row can be keyed"
  # EVERY CHILD ROW IS TRIED IN TURN, because Add default keys does nothing at all on a FOLDER
  # (obj::Item::cast refuses it, the menu closes and the layer stays keyless): the loop stops at
  # the first row whose keys reach the panel, and switches back to the timeline for the next one.
  # The top node is skipped: it answers the row menu with no Add default keys at all, so pressing
  # the entries by position on it would open Create layer instead.
  CAND=$(printf "%s\n" "$ROWS" | sed -n 2,8p)
  say "candidate child rows: $(printf "%s" "$CAND" | tr "\n" ";")"
  ROW_A=""; KEYSUM=""; CBTN=""; CRASH=""; CRASHROW=""
  while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    [ -n "$ROW_A" ] && break
    R=$(ae extents "$cand" "table cell")
    [ -n "$R" ] || continue
    read -r AX AY AW AH <<< "$R"
    sel "$cand"; sleep 0.8
    dx=$((AX + 6)); dy=$((AY + AH / 2))
    xdotool mousemove --sync $dx $dy; sleep 0.4
    xdotool click 3; sleep 1.2
    POP=$(xwininfo -root -children 2>/dev/null | grep -E "[0-9]+x[0-9]+\+${dx}\+${dy}" | grep -oE "0x[0-9a-f]+" | head -1)
    # a menu that is not there is a REFUSAL, not a failure of the run: a probe that presses entries
    # blind on a menu it cannot see is how Create layer gets opened.
    if [ -z "$POP" ]; then
      say "candidate [$cand]: no row menu opened at $dx,$dy"
      continue
    fi
    PH=$(xwininfo -id "$POP" 2>/dev/null | sed -n "s/^ *Height: *//p" | tr -d " ")
    # the child row menu MEASURED 223px tall; a menu without the parent-gated entries is far
    # shorter, and pressing ITS entries blind is how a probe opens Create layer.
    if [ -z "$PH" ] || [ "$PH" -lt 180 ]; then
      xdotool key --clearmodifiers Escape
      say "candidate [$cand]: the row menu is ${PH:-?}px tall, not the child row menu: refusing it"
      continue
    fi
    echo "POP=\"$POP ${PH}px\"" >> "$GEOM"
    xdotool windowfocus --sync "$POP" 2>/dev/null
    xdotool key --clearmodifiers Up;     sleep 0.3
    xdotool key --clearmodifiers Up;     sleep 0.3
    xdotool key --clearmodifiers Return; sleep 2
    xdotool windowactivate --sync $W 2>/dev/null
    say "Add default keys pressed on row [$cand]"
    # ---- the keyed frame: the repaint the docked host has to survive ----
    # THE CRASH LANDS IN A PAINT, NOT IN THE CLICK, so the state is given time to settle and the
    # application is checked on both sides of the capture and of the dump: on this tree the docked
    # host aborts a second or two after the switch (see THE BLOCKED CLAIMS), and a probe that read
    # the tree in between would report "no channel count" for a frame that was never drawn.
    tab "Graph" || abort "the dock header stopped answering its Graph tab after the keys were added"
    sleep 3
    if ! alive; then CRASH="yes"; CRASHROW="$cand"; ROW_A="$cand"; break; fi
    shot "$SHOTS/gink_30_dock_graph_keys.png"
    if ! alive; then CRASH="yes"; CRASHROW="$cand"; ROW_A="$cand"; break; fi
    ae dump > "$SCRATCH/dump_graph_keys.txt" 2>/dev/null
    if ! alive; then CRASH="yes"; CRASHROW="$cand"; ROW_A="$cand"; break; fi
    KEYSUM=$(sed -n "s/^ *label *| \([0-9]\{1,\}\) channel(s) shown.*$/\1/p" "$SCRATCH/dump_graph_keys.txt" | head -1)
    # The "N channel(s)" PUSH BUTTON of the whole tree: the dock header own control (the panel
    # publishes none - see the channels claim in the reporting half). Read here so the geometry
    # below is that control own rectangle, not a guess.
    CBTN=$(sed -n "s/^ *button *| \([0-9]\{1,\} channel(s)\)$/\1/p" "$SCRATCH/dump_graph_keys.txt" | head -1)
    say "candidate [$cand]: the model publishes [${KEYSUM:-nothing}] channel(s) shown, the dock header control reads [${CBTN:-none}]"
    if [ -n "${KEYSUM:-}" ]; then
      ROW_A="$cand"
      echo "PANEL_COUNT=\"${KEYSUM}\"" >> "$GEOM"
      echo "CHBTN=\"${CBTN:-}\"" >> "$GEOM"
      [ -n "${CBTN:-}" ] && echo "CHBTN_RECT=\"$(ae extents "${CBTN}")\"" >> "$GEOM"
      break
    fi
    tab "Timeline" || abort "the dock header stopped answering its Timeline tab between candidates"
  done <<< "$CAND"
  echo "ROW_A=\"$ROW_A\"" >> "$GEOM"
  if [ -n "$CRASH" ]; then
    echo "CRASH=\"yes\"" >> "$GEOM"
    echo "CRASH_ROW=\"$CRASHROW\"" >> "$GEOM"
    echo "CRASH_TAIL=\"$(tail -2 "$APPLOG" | tr "\n" "|")\"" >> "$GEOM"
    say "the application DIED on row [$CRASHROW]"
  fi
  if [ -n "${KEYSUM:-}" ] && [ -z "$CRASH" ]; then
      # ---- A SECOND KEY SET, AT A FRAME THE PLOT SHOWS ----
      # EVERY key the row menu adds is at frame 0, and the plot maps the timeline window, which
      # starts at frame 1: a frame-0 marker lands at the plot LEFT EDGE and is culled, so the key
      # set alone publishes no marker to drag. The dock own Current Keys knobs add a key for ONE
      # channel at the CURRENT frame: the playhead is moved into the plot (a click on the ruler at
      # TL+150) and the Move and Rotate knobs are pressed, which is TWO DIFFERENT KEY TYPES at a
      # frame the plot shows. The knobs sit below the fold of the Properties dock, so the two
      # groups above them are collapsed first, through their own title labels.
      tab "Timeline" || abort "the dock header stopped answering its Timeline tab before the knobs"
      RULER_X=$((DX + TREE_W + 150)); RULER_Y=$((DY + 37 + 40 - 11))
      xdotool mousemove --sync $RULER_X $RULER_Y click 1; sleep 1.2
      park
      say "the playhead was set from the ruler at $RULER_X"
      for title in "Constants" "Default Keys"; do
        TR=$(ae extents "$title" "label")
        [ -n "$TR" ] || continue
        read -r TX3 TY3 TW3 TH3 <<< "$TR"
        xdotool mousemove --sync $((TX3 + TW3 / 2)) $((TY3 + TH3 / 2)) click 1
        sleep 1.2
        park
      done
      KNOBS=""
      for kn in Move Rotate; do
        KL=$(ae extents "$kn" "label")
        if [ -z "$KL" ]; then KNOBS="$KNOBS ${kn}:missing"; continue; fi
        read -r KKX KKY KKW KKH <<< "$KL"
        xdotool mousemove --sync $((KKX + KKW / 2)) $((KKY + KKH / 2)) click 1
        sleep 1.6
        park
        KNOBS="$KNOBS ${kn}:${KL}"
        say "the $kn knob was pressed at $KL"
      done
      echo "KNOBS=\"$KNOBS\"" >> "$GEOM"

      tab "Graph" || abort "the dock header stopped answering its Graph tab after the knobs"
      sleep 1.5
      shot "$SHOTS/gink_30_dock_graph_keys.png"
      if ! alive; then
        echo "CRASH=\"yes\"" >> "$GEOM"
        echo "CRASH_TAIL=\"$(tail -2 "$APPLOG" | tr "\n" "|")\"" >> "$GEOM"
        say "the application DIED on the keyed frame"
      fi

      # ---- the panel PAGE, its floor, and the channel hues on it ----
      PX2=$((DX + TREE_W)); PY2=$((DY + 40))
      PW2=$((DX + DW - PX2)); PH2=$((DY + DH - PY2))
      echo "PANELRECT=\"$PX2 $PY2 $PW2 $PH2\"" >> "$GEOM"
      raw "$SHOTS/gink_30_dock_graph_keys.png" "${PW2}x${PH2}+${PX2}+${PY2}" "$SCRATCH/panel.raw"
      FLOOR=$(inkpx modal "$SCRATCH/panel.raw" $PW2 $PH2 0 0 $PW2 $PH2 | cut -d" " -f1-3)
      echo "PANEL_FLOOR=\"$FLOOR\"" >> "$GEOM"
      read -r F1 F2 F3 F4 <<< "$FLOOR"
      # A CHANNEL HUE IS CHROMATIC INK ON THE PANEL OWN FLOOR: the floor is one opaque colour, so
      # the only chromatic ink on it is a channel hue (theme::Graph::keyColor) and the accent the
      # playhead and the selection read as. The tree column is outside this crop by construction,
      # so its accent-tinted row band cannot be read as a channel colour. The distance bar is 60
      # and not the 150 a full-strength marker would need: with nothing selected every curve is
      # drawn at 0.45, i.e. HALF its token distance from the floor.
      # EVERY chromatic ink on the floor is measured once: its pixel count, the columns it owns and
      # the LEFTMOST of them. Which HUE a channel is drawn in is the application business (it is
      # theme::Colors::channelColor(type, component) - an axis hue where the channel has one - and
      # this probe never states it), so the probe reads whatever it finds: the two-colour claim
      # below takes the hues with a curve share of the plate, and the insert below takes any of
      # them, at its leftmost column.
      inkpx inkhues "$SCRATCH/panel.raw" $PW2 $PH2 0 0 $PW2 $PH2 "$F1" "$F2" "$F3" 60 40 "$HUE_MIN_PX" > "$SCRATCH/hues_raw.txt"
      : > "$SCRATCH/hues.txt"
      : > "$SCRATCH/hues_all.txt"
      while read -r r g b n; do
        case "$r" in ""|none) continue ;; esac
        inkpx cols "$SCRATCH/panel.raw" $PW2 $PH2 0 $((PH2 - 1)) "$r" "$g" "$b" $HUE_TOL 1 > "$SCRATCH/hue_runs.txt"
        ncols=0; left=-1
        while read -r a bb nn; do
          case "$a" in ""|none) continue ;; esac
          ncols=$((ncols + (bb - a + 1)))
          if [ "$left" -lt 0 ] || [ "$a" -lt "$left" ]; then left=$a; fi
        done < "$SCRATCH/hue_runs.txt"
        # the playhead is ONE column wide, and the tree column is not in this crop: a hue that owns
        # two or more columns is ink the panel drew inside its plot
        [ "$ncols" -ge 2 ] || continue
        printf "%s %s %s %s %s %s\n" "$r" "$g" "$b" "$n" "$ncols" "$left" >> "$SCRATCH/hues_all.txt"
        if [ "$n" -ge "$HUE_CURVE_MIN" ] && [ "$ncols" -ge "$HUE_COLS_MIN" ]; then
          printf "%s %s %s %s %s\n" "$r" "$g" "$b" "$n" "$ncols" >> "$SCRATCH/hues.txt"
        fi
      done < "$SCRATCH/hues_raw.txt"
      head -6 "$SCRATCH/hues.txt" > "$SCRATCH/hues_u.txt"
      head -8 "$SCRATCH/hues_all.txt" > "$SCRATCH/hues_cand.txt"
      echo "PANEL_HUES=\"$(tr "\n" ";" < "$SCRATCH/hues_u.txt")\"" >> "$GEOM"
      echo "PANEL_HUES_ALL=\"$(tr "\n" ";" < "$SCRATCH/hues_cand.txt")\"" >> "$GEOM"
      say "chromatic hues on the panel: curves [$(tr "\n" ";" < "$SCRATCH/hues_u.txt")], all [$(tr "\n" ";" < "$SCRATCH/hues_cand.txt")]"

      # ---- PHASE 4: A HOVER REPAINTS THE KEY MARKER IT LIGHTS ----
      # WHAT A REGRESSION LOOKS LIKE: the pointer sits on a key dot and NOTHING is clicked, the
      # marker never lights on the page, and the box this phase reads stays at the control pair
      # 0px - which is exactly what GraphPanelWidget::updateHoverRepaint not asking for a repaint
      # when the hovered target identity changes leaves behind.
      # THE MARKER IS FOUND IN THE PANEL OWN INK, with the helpers the hue scan above rests on: a
      # key marker is the one CHROMATIC mark on the plate that is a DISK. The ae_ink.py inkcolmap
      # gives every inked column with its row span, its pixel count and its modal colour, and the
      # disk is read off that: a column of 5..12 ink pixels whose colour is chromatic (the hue
      # family the curves are drawn in - theme::Graph::keyColor), with 2..10 such columns within
      # 12px of it whose centres agree within 4 rows. The two halves of that test are the two
      # things a disk has: a disk narrows from its centre to the curve own 2px stroke
      # (gui::stroke::kWidthPx) over a compact run of neighbouring columns, and it is BOUNDED -
      # while a near-vertical curve segment crosses those rows in ONE column whose neighbours
      # centre rows away, and two flat curves of one hue running together are a chromatic band
      # that spans the whole plate rather than 2..10 columns. The shapes are told apart by the
      # ink, not by a guess. The playhead accent column spans the plate (far above 12 rows), the
      # ruler ticks and numbers are neutral (theme::Graph::axisText), and with an empty selection
      # no chip is drawn (a chip is for a curve IN PLAY), so a chromatic disk on the plate is a
      # key marker.
      PLOT=$(ae extents "Fit graph")
      [ -n "$PLOT" ] || abort "the docked panel published no Fit graph box while its Graph page is up: there is no measured plate to find a key marker in"
      read -r PLX PLY PLW PLH <<< "$PLOT"
      echo "PLOT=\"$PLOT\"" >> "$GEOM"
      shot "$SHOTS/gink_40_hover_base.png"
      shot "$SHOTS/gink_40b_hover_control.png"
      raw "$SHOTS/gink_40_hover_base.png" "${PLW}x${PLH}+${PLX}+${PLY}" "$SCRATCH/plate.raw"
      inkpx inkcolmap "$SCRATCH/plate.raw" $PLW $PLH "$F1" "$F2" "$F3" 60 > "$SCRATCH/plate_cols.txt"
      MCX=(); MCY=(); MCN=(); MCK=0
      while read -r cxx cy0 cy1 cnn cr cg cb; do
        case "$cxx" in ""|none) continue ;; esac
        cnn=${cnn:-0}
        cmx=$cr; [ "$cg" -gt "$cmx" ] && cmx=$cg; [ "$cb" -gt "$cmx" ] && cmx=$cb
        cmn=$cr; [ "$cg" -lt "$cmn" ] && cmn=$cg; [ "$cb" -lt "$cmn" ] && cmn=$cb
        [ $((cmx - cmn)) -ge 40 ] || continue
        { [ "$cnn" -ge 5 ] && [ "$cnn" -le 12 ]; } || continue
        MCX[$MCK]=$cxx; MCY[$MCK]=$(( (cy0 + cy1) / 2 )); MCN[$MCK]=$cnn; MCK=$((MCK + 1))
      done < "$SCRATCH/plate_cols.txt"
      DOTX=""; DOTY=""; DOTN=0
      mi=0
      while [ "$mi" -lt "$MCK" ]; do
        mnear=0; mj=0
        while [ "$mj" -lt "$MCK" ]; do
          if [ "$mj" -ne "$mi" ]; then
            mdx=$((MCX[mi] - MCX[mj])); [ "$mdx" -lt 0 ] && mdx=$((-mdx))
            mdy=$((MCY[mi] - MCY[mj])); [ "$mdy" -lt 0 ] && mdy=$((-mdy))
            if [ "$mdx" -le 12 ] && [ "$mdy" -le 4 ]; then mnear=$((mnear + 1)); fi
          fi
          mj=$((mj + 1))
        done
        if [ "$mnear" -ge 2 ] && [ "$mnear" -le 10 ] && [ "${MCN[mi]}" -gt "$DOTN" ]; then
          DOTN=${MCN[mi]}; DOTX=${MCX[mi]}; DOTY=${MCY[mi]}
        fi
        mi=$((mi + 1))
      done
      [ -n "$DOTX" ] || abort "no key marker ink was found on the panel plate [${PLOT:-none}]: a chromatic disk 5..12 rows deep over a run of neighbouring columns is what a key marker is, and this frame draws none (the keys the knobs made may lie outside the plate window, or no marker was inked)"
      HKX=$((PLX + DOTX)); HKY=$((PLY + DOTY))
      echo "HOVER_AT=\"$HKX $HKY\"" >> "$GEOM"
      echo "HOVER_BOX=\"$((HKX - HOVER_BOX_PX / 2)) $((HKY - HOVER_BOX_PX / 2)) $HOVER_BOX_PX $HOVER_BOX_PX\"" >> "$GEOM"
      say "key marker at $HKX,$HKY (its column is ${DOTN}px of chromatic ink): the pointer moves onto it with NO click"
      xdotool mousemove --sync $HKX $HKY
      settle_shot "$SHOTS/gink_41_hover.png" || abort "the docked panel never settled with the pointer on the key marker at $HKX,$HKY"
      park
      settle_shot "$SHOTS/gink_42_hover_off.png" || abort "the docked panel never settled with the pointer parked off every key"
      say "the hover was captured, and so was the page with the pointer off every key"

      # ---- PHASE 5: THE DOCK HEADER VIEW BUTTON REPAINTS THE PAGE IT DRIVES ----
      # WHAT A REGRESSION LOOKS LIKE: the header zoom-in moves the timeline own view (the button
      # works, the ruler and the lanes move) while the docked panel keeps the frame window it had,
      # so the panel own page changes by 0px - which is what MainDisplayWidget::
      # syncGraphPanelSharedState no longer refreshing the Graph page on a view-range change
      # leaves behind. The button is pressed BY NAME, through the header own accessibility
      # publication and its own rectangle.
      ZR=$(ae extents "Zoom in")
      [ -n "$ZR" ] || abort "the dock header publishes no button named Zoom in (TimeLineHeader own view button, tooltip Zoom in): the header view action cannot be pressed by name"
      echo "ZOOM_BTN=\"$ZR\"" >> "$GEOM"
      read -r ZBX ZBY ZBW ZBH <<< "$ZR"
      xdotool mousemove --sync $((ZBX + ZBW / 2)) $((ZBY + ZBH / 2)) click 1
      park
      settle_shot "$SHOTS/gink_50_zoom_after.png" || abort "the docked panel never settled after the header zoom-in was pressed"
      say "the header zoom-in was pressed at [$ZR] and the page captured again"
  fi

  say "body done"
  kill $app 2>/dev/null
  wait $app 2>/dev/null
' 2>&1 | tail -6

. "$GEOM" 2>/dev/null
if [ -n "${ABORT:-}" ]; then
    check 1 "$ABORT"
    summary probe_graph_ink
fi
if [ -z "${DOCK:-}" ] || [ -z "${TAB_T_AT_TL:-}" ] || [ -z "${TREE_W:-}" ]; then
    check 1 "the probe body produced no geometry (see $GEOM and $APPLOG)"
    summary probe_graph_ink
fi

read -r DX DY DW DH <<< "${DOCK:-0 0 0 0}"
DOCK_AREA=$((DW * DH))
TABLOGS="$SCRATCH/dump_timeline.txt"
TABGLOG="$SCRATCH/dump_graph.txt"
KEYSLOG="$SCRATCH/dump_graph_keys.txt"

# The panel Pane, out of a dump, by the ONE thing that identifies it: the "Fit graph" button it
# owns. Prints "<indent>|<name>" for the pane whose button that was, so the name can be read
# beside the indent that says WHICH node it is.
panelnode() { # panelnode <dump>
    awk '
      /^ *panel *\|/ { pind = match($0, /[^ ]/) - 1; pname = $0; sub(/^ *panel *\| */, "", pname) }
      /^ *button *\| Fit graph$/ { printf "%d|%s\n", pind, pname; exit }
    ' "$1"
}
# The panels inside the dock: the dump lines of the Animation subtree, which is where both the
# panel Pane and the tabs live. NAMED panes are counted beside the unnamed ones, because the
# hidden page publishes an EMPTY model - the node stays, its name and children do not.
anim_panes() { # anim_panes <dump> -> "<indent> <name-or-empty>" per line
    awk '
      !inb { if ($0 ~ /^ *panel +\| Animation$/) { inb = 1; base = match($0, /[^ ]/) - 1 } ; next }
      /^ *$/ { next }
      { ind = match($0, /[^ ]/) - 1; if (ind <= base) exit }
      /^ *panel *\|/ { nm = $0; sub(/^ *panel *\| */, "", nm); printf "%d %s\n", ind, nm }
    ' "$1"
}
aediff() { local d; d=$(magick compare -metric AE "$1" "$2" null: 2>&1); d=${d%% *}; [ -n "$d" ] || d=0; echo "$d"; }
dist() { awk -v a1="$1" -v a2="$2" -v a3="$3" -v b1="$4" -v b2="$5" -v b3="$6" \
        'BEGIN{d1=a1-b1; d2=a2-b2; d3=a3-b3; printf "%.1f", sqrt(d1*d1+d2*d2+d3*d3)}'; }

# ---- 1. THE DOCK'S OWN ROUTE ----
# WHICH TAB IS CURRENT IS READABLE FROM THE RECTANGLES THE TAB BAR PUBLISHES, and that is what
# makes the switch measurable rather than assumed. MEASURED on this tab bar: the two tabs keep
# their own slots (Timeline at x=8, Graph at x=68) whatever is current, and the CURRENT tab's
# rectangle is inflated to the whole strip - 120px, the union of both slots - while the other
# keeps its own 60px. So "the current tab is the wide one" reads the index straight off the tree,
# in both directions.
read -r t1x t1y t1w t1h <<< "${TAB_T_AT_TL:-0 0 0 0}"
read -r g1x g1y g1w g1h <<< "${TAB_G_AT_TL:-0 0 0 0}"
read -r t2x t2y t2w t2h <<< "${TAB_T_AT_G:-0 0 0 0}"
read -r g2x g2y g2w g2h <<< "${TAB_G_AT_G:-0 0 0 0}"
awk -v x="$t2x" -v X="$g1x" 'BEGIN{ exit !(x < X) }'
check $? "the dock header's two page tabs keep their own slots whatever is current - Timeline in the left one (x ${t2x:-?} while Graph is current) and Graph in the right (x ${g1x:-?} while Timeline is): the probe presses the control it names"
awk -v w="$t1w" -v W="$g1w" 'BEGIN{ exit !(w > W) }'
check $? "with the Timeline tab current, the Timeline tab is the one whose rectangle spans the strip: [${TAB_T_AT_TL:-none}] against Graph [${TAB_G_AT_TL:-none}]"
awk -v w="$g2w" -v W="$t2w" 'BEGIN{ exit !(w > W) }'
check $? "and pressing the tab BY NAME made the Graph tab the current one: [${TAB_G_AT_G:-none}] against Timeline [${TAB_T_AT_G:-none}] - the tab bar itself says which page the dock is showing"
awk -v x="$t1x" -v y="$t1y" -v w="$t1w" -v h="$t1h" -v X="$DX" -v Y="$DY" -v W="$DW" -v H="$DH" \
    'BEGIN{ exit !(x >= X && y >= Y && x + w <= X + W && y + h <= Y + H) }'
check $? "and both tabs live inside the dock's own AT-SPI rectangle [${DOCK:-none}] - the header whose page the probe then measures"

# THE PANEL, NAMED, AND PUBLISHED ONCE. "Fit graph" appearing twice in the tree would mean two
# hosts publishing one panel, which is exactly what the docked host takes the ONE installation to
# prevent.
PN=$(panelnode "$TABGLOG")
PANEL_IND=${PN%%|*}
PANEL_NAME=${PN#*|}
FIT_G_ALL=$(grep -c "^ *button *| Fit graph$" "$TABGLOG" 2>/dev/null) || FIT_G_ALL=0
FIT_T_ALL=$(grep -c "^ *button *| Fit graph$" "$TABLOGS" 2>/dev/null) || FIT_T_ALL=0
G_NAMED=$(anim_panes "$TABGLOG" | grep -cE "^[0-9]+ .")
T_NAMED=$(anim_panes "$TABLOGS" | grep -cE "^[0-9]+ .")
T_IND=$(anim_panes "$TABLOGS" | sed -n "s/^\([0-9]*\) *$/\1/p" | head -1)
echo "tree: with the Graph tab up the dock holds ${G_NAMED} named panel pane(s), with the Timeline tab ${T_NAMED}; the panel node is [${PANEL_NAME:-none}] at indent ${PANEL_IND:-?}, and the unnamed pane the timeline state holds is at indent ${T_IND:-?}"
[ "${FIT_G_ALL:-0}" -eq 1 ]
check $? "the panel publishes \"Fit graph\" EXACTLY ONCE in the whole accessibility tree while its tab is up (${FIT_G_ALL:-0} occurrence(s)): one panel, one publication"
[ -n "${PANEL_NAME:-}" ] && [ "${PANEL_NAME:-}" != "unknown" ]
check $? "and the Pane that owns it is NAMED with the model's own summary rather than left anonymous: [${PANEL_NAME:-none}]"
case "${PANEL_NAME:-}" in
    *"channel(s) shown"*|"No keys on the selected layers"|"Select a layer to show its curves"|"All channels of the selected layers are hidden"|"No room for the graph in this viewport")
        ok=0 ;;
    *) ok=1 ;;
esac
check "$ok" "and that name is one of the model's own sentences, not a leftover label ([${PANEL_NAME:-none}])"
if [ "${FIT_T_ALL:-0}" -eq 0 ]; then ok=0; else ok=1; fi
check "$ok" "with the TIMELINE tab current the same panel publishes NOTHING of its own: \"Fit graph\" appears ${FIT_T_ALL:-0} time(s) in that dump, because the hidden page publishes the EMPTY model"
if [ "${T_NAMED:-1}" -eq 0 ] && [ "${G_NAMED:-0}" -eq 1 ] && [ "${T_IND:-x}" = "${PANEL_IND:-y}" ]; then ok=0; else ok=1; fi
check "$ok" "and the panel's Pane is still in the tree, UNNAMED and at the SAME position, while its page is hidden (${T_NAMED:-?} named pane(s) then against ${G_NAMED:-?} now; indent ${T_IND:-?} against ${PANEL_IND:-?})"
if grep -q "gui::a11y: the viewport was queried for accessibility before install" "$APPLOG" 2>/dev/null; then ok=1; else ok=0; fi
check "$ok" "and the application never warned that its viewport was queried for accessibility before the factory was installed - the ordering the panel's ONE publication depends on (gui::a11y::adoptViewportInterface)"

# ---- 2. THE DOCK'S OWN PIXELS ----
# INSIDE, on the two tab states; OUTSIDE, on the same two captures with the dock's rectangle
# blanked in both, so the number is a measurement rather than a subtraction; and the CONTROL pair,
# which is what rules a repaint out.
magick "${SHOTS}/gink_10_dock_timeline.png" "${SHOTS}/gink_20_dock_graph.png" \
    -crop "${DW}x${DH}+${DX}+${DY}" +repage "${SCRATCH}/dock_%d.png" 2>/dev/null
magick "${SHOTS}/gink_10_dock_timeline.png" -fill black -draw "rectangle ${DX},${DY} $((DX + DW)),$((DY + DH))" "${SCRATCH}/tl_masked.png"
magick "${SHOTS}/gink_20_dock_graph.png" -fill black -draw "rectangle ${DX},${DY} $((DX + DW)),$((DY + DH))" "${SCRATCH}/gr_masked.png"
DOCK_DIFF=$(aediff "$SCRATCH/dock_0.png" "$SCRATCH/dock_1.png")
OUTSIDE_DIFF=$(aediff "$SCRATCH/tl_masked.png" "$SCRATCH/gr_masked.png")
CONTROL_DIFF=$(aediff "${SHOTS}/gink_11_dock_timeline_b.png" "${SHOTS}/gink_12_dock_timeline_c.png")
SHARE=$(awk -v a="$DOCK_DIFF" -v b="$DOCK_AREA" 'BEGIN{ printf "%.3f", (b > 0) ? a / b : 0 }')
echo "pixels: the Graph tab against the Timeline tab changes ${DOCK_DIFF}px inside the dock (${SHARE} of its ${DOCK_AREA}px) and ${OUTSIDE_DIFF}px outside it; the same state captured twice differs by ${CONTROL_DIFF}px (the tree is ${TREE_W:-?}px of the dock, so the panel's page is the other $((DW - ${TREE_W:-0}))px)"
[ "${CONTROL_DIFF:-1}" -eq 0 ]
check $? "the control pair is the SAME state twice: ${CONTROL_DIFF:-?}px differ between two captures taken with nothing forced between them, so the change below is the tab switch and not a repaint"
[ "${DOCK_DIFF:-0}" -ge "$DOCK_CHANGE_MIN" ]
check $? "switching the dock's tab to Graph repaints a substantial part of the dock's own rectangle: ${DOCK_DIFF:-0}px changed inside [${DOCK:-none}] (bar ${DOCK_CHANGE_MIN})"
[ "${OUTSIDE_DIFF:-1}" -eq 0 ]
check $? "and NOTHING changes outside that rectangle: ${OUTSIDE_DIFF:-?}px differ elsewhere on the screen, so the panel is the dock's PAGE and not an overlay over the window"
awk -v s="$SHARE" -v m="$DOCK_CHANGE_SHARE" 'BEGIN{ exit !(s <= m) }'
check $? "and the change is BOUNDED - ink on the dock's page, not a repaint of the whole dock: share ${SHARE} <= ${DOCK_CHANGE_SHARE}"
assert_rendered "${SHOTS}/gink_20_dock_graph.png" "the docked panel's page with the Graph tab up"

# ---- 3. THE KEYED CLAIMS ----
# The keys and the two key TYPES are all made by the application's own controls, so the
# routes are asserted beside the ink: which row was keyed and which two knobs were pressed. The
# crash's own log tail is printed with any failure below, because a host that dies drawing the
# keyed frame is the defect this phase exists to catch.
KNOB_MOVE=""; KNOB_ROT=""
for pair in ${KNOBS:-}; do
    case "$pair" in
        Move:*) KNOB_MOVE=${pair#Move:} ;;
        Rotate:*) KNOB_ROT=${pair#Rotate:} ;;
    esac
done
if [ -n "${CRASH:-}" ]; then
    echo "crash: the application died on the keyed frame; its log ended [${CRASH_TAIL:-none}]"
fi
[ -z "${CRASH:-}" ]
check $? "the docked host drew a keyed frame WITHOUT DYING${CRASH:+ (its log ended [${CRASH_TAIL:-none}]; this is the stroke-pass defect the one-context fix removed)}"
[ -n "${ROW_A:-}" ] && [ -n "${PANEL_COUNT:-}" ]
check $? "the keys the object tree's own row menu added to [${ROW_A:-none}] reached the panel's model: it publishes [${PANEL_COUNT:-nothing}] channel(s) shown"
[ -n "${KNOB_MOVE:-}" ] && [ -n "${KNOB_ROT:-}" ]
check $? "and the Properties dock's CURRENT KEYS knobs put a key of TWO different types at the frame the playhead was moved to: Move [${KNOB_MOVE:-missing}] and Rotate [${KNOB_ROT:-missing}]"
# THE PANEL'S ONE CONTROL IS NOT THE PANEL'S ANY MORE. The design moved it into the dock HEADER
# (TimeLineHeader's own Qt button), and the panel's accessibility publication carries no channel
# control at all: the ONE "N channel(s)" push button in the whole tree is therefore the header's
# own. The dump cannot tell the two apart by name - both read "N channel(s)" - so the count is
# what separates them: the panel publishing one as well would make this 2, and the rectangle check
# below places the survivor in the dock's header row rather than in the panel's page.
CHBTN_LINES=$(grep -c "^ *button *| [0-9]\{1,\} channel(s)$" "$KEYSLOG" 2>/dev/null) || CHBTN_LINES=0
[ "${CHBTN_LINES:-0}" -eq 1 ]
check $? "the panel publishes NO channels control of its own: the tree holds exactly ONE \"N channel(s)\" button (${CHBTN_LINES:-0}), and it is the DOCK HEADER's ([${CHBTN:-none}])"
read -r CX2 CY2 CW2 CH2 <<< "${CHBTN_RECT:-0 0 0 0}"
awk -v x="$CX2" -v y="$CY2" -v w="$CW2" -v h="$CH2" -v X="$DX" -v Y="$DY" -v W="$DW" -v H="$DH" \
    'BEGIN{ exit !(x >= X && y >= Y && x + w <= X + W && y + h <= Y + H) }'
check $? "and that header control is a real one inside the dock's own rectangle ([${CHBTN_RECT:-none}] in [${DOCK:-none}])"
echo "panel: page area [${PANELRECT:-none}], floor [${PANEL_FLOOR:-none}]; the channel hues on it (rgb px columns): [${PANEL_HUES:-none}]"
# ---- TWO CHANNELS OF DIFFERENT KEY TYPES ARE TWO COLOURS ----
# The pair is chosen from the hues the panel is DRAWN with, not from a list of tokens: a hue that
# covers a curve's share of the panel (HUE_CURVE_MIN px) over hundreds of columns is a channel's
# own colour (theme::Graph::keyColor), the playhead's single column keeps the accent out, and the
# two are required HUE_SEP apart. Measured in both directions below.
HA=""; HB=""; HD=""
while read -r r g b n cols; do
    [ -n "${r:-}" ] || continue
    if [ -z "$HA" ]; then HA="$r $g $b $n $cols"; continue; fi
    [ -n "$HB" ] && continue
    D=$(dist "$r" "$g" "$b" $(printf "%s" "$HA" | cut -d" " -f1-3))
    awk -v a="$D" -v m="$HUE_SEP" 'BEGIN{ exit !(a >= m) }' || continue
    HB="$r $g $b $n $cols"; HD="$D"
done < "$SCRATCH/hues_u.txt"
NH=$(grep -c . "$SCRATCH/hues_u.txt" 2>/dev/null) || NH=0
[ "${NH:-0}" -ge 2 ]
check $? "the keyed layer shows at least two CHANNELS at the frame the knobs keyed, each drawn as a stroke on the panel's own floor: ${NH:-0} qualifying hue(s) ([${PANEL_HUES:-none}])"
read -r A1 A2 A3 A4 A5 <<< "${HA:-0 0 0 0 0}"
read -r B1 B2 B3 B4 B5 <<< "${HB:-0 0 0 0 0}"
[ -n "${B1:-}" ] && [ "${B4:-0}" -ge "$HUE_CURVE_MIN" ] && [ "${A4:-0}" -ge "$HUE_CURVE_MIN" ]
check $? "and the two of them are different colours, each with a curve's share of ink (rgb ${A1:-?},${A2:-?},${A3:-?} at ${A4:-0}px over ${A5:-0} columns; rgb ${B1:-?},${B2:-?},${B3:-?} at ${B4:-0}px over ${B5:-0} columns)"
awk -v a="${HD:-0}" -v m="$HUE_SEP" 'BEGIN{ exit !(a >= m) }'
check $? "and two different CHANNELS carry different colours: the two are ${HD:-?} apart (bar ${HUE_SEP}), so the panel does not paint every channel of this layer the same. WHICH hue a channel gets is the application business - theme::Colors::channelColor(type, component), an axis hue where the channel has one and its type hue otherwise - and this probe never states it: it measures the hue the panel actually drew"

# ---- 4. A HOVER REPAINTS THE KEY MARKER IT LIGHTS ----
# A REGRESSION LOOKS LIKE this: the pointer sits on a key's dot with NO click and the marker never
# lights, because GraphPanelWidget::updateHoverRepaint stopped asking for a repaint when the
# hovered target's identity changes. The reading is then the control pair's 0px. The bar is set
# from the ink the marker itself owns - its fill is a disk of radius ctrl::graph::kKeyDotRadius
# (3.75px, about 44px of ink) taken 0.4 of the way to the text ink by kKeyHoverFillMix - and it is
# asked of the marker's OWN box rather than of a share of the page, so a page-wide repaint that
# left the marker alone cannot clear it. A missing capture reads 0 through magick compare and
# fails the box check, and the capture's existence and non-blackness are asserted beside it.
HOVER_BASE="${SHOTS}/gink_40_hover_base.png"
HOVER_CTRL="${SHOTS}/gink_40b_hover_control.png"
HOVER_ON="${SHOTS}/gink_41_hover.png"
HOVER_OFF="${SHOTS}/gink_42_hover_off.png"
read -r HBX HBY HBW HBH <<< "${HOVER_BOX:-0 0 0 0}"
assert_exists "$HOVER_BASE" "the panel's page captured before the hover"
assert_rendered "$HOVER_BASE" "the panel's page captured before the hover"
assert_exists "$HOVER_CTRL" "the panel's page captured again, for the same-state pair"
assert_exists "$HOVER_ON" "the panel's page with the pointer ON a key's dot"
assert_rendered "$HOVER_ON" "the panel's page with the pointer ON a key's dot"
assert_exists "$HOVER_OFF" "the panel's page with the pointer back off every key"
assert_rendered "$HOVER_OFF" "the panel's page with the pointer back off every key"
[ -n "${HOVER_BOX:-}" ] && [ -n "${HOVER_AT:-}" ]
check $? "the probe hovers a marker it FOUND in the panel's own ink rather than a guessed point: a chromatic disk read off the plate at [${HOVER_AT:-none}], measured as the box [${HOVER_BOX:-none}] around it"
awk -v x="$HBX" -v y="$HBY" -v w="$HBW" -v h="$HBH" -v X="$DX" -v Y="$DY" -v W="$DW" -v H="$DH" \
    'BEGIN{ exit !(x >= X && y >= Y && x + w <= X + W && y + h <= Y + H) }'
check $? "and that box lies inside the dock's own rectangle [${DOCK:-none}], so the ink it measures is the docked panel's own"
magick "$HOVER_BASE" -crop "${HBW}x${HBH}+${HBX}+${HBY}" +repage "$SCRATCH/hov_base.png" 2>/dev/null
magick "$HOVER_ON" -crop "${HBW}x${HBH}+${HBX}+${HBY}" +repage "$SCRATCH/hov_on.png" 2>/dev/null
HOVER_BOX_DIFF=$(aediff "$SCRATCH/hov_base.png" "$SCRATCH/hov_on.png")
HOVER_PAGE_DIFF=$(aediff "$HOVER_BASE" "$HOVER_ON")
HOVER_CTRL_DIFF=$(aediff "$HOVER_BASE" "$HOVER_CTRL")
HOVER_RET_DIFF=$(aediff "$HOVER_BASE" "$HOVER_OFF")
echo "hover: the pointer on the marker changes ${HOVER_BOX_DIFF}px of its own ${HBW}x${HBH} box at [${HOVER_BOX:-none}] (${HOVER_PAGE_DIFF}px of the whole page); the same state twice differs by ${HOVER_CTRL_DIFF}px, and the page with the pointer back off every key differs from the pre-hover one by ${HOVER_RET_DIFF}px"
[ "${HOVER_CTRL_DIFF:-1}" -eq 0 ]
check $? "the keyed Graph page is a STATE and not a repaint: two captures of it taken with nothing forced between them differ by ${HOVER_CTRL_DIFF:-?}px, so what the next reading sees is the pointer"
[ "${HOVER_BOX_DIFF:-0}" -ge "$HOVER_BOX_MIN" ]
check $? "moving the pointer ONTO a key's dot, with no click, repaints the docked panel: ${HOVER_BOX_DIFF:-0}px changed inside the marker's own box (bar ${HOVER_BOX_MIN}) - a stale or missing capture reads 0 here and fails"
[ "${HOVER_RET_DIFF:-1}" -eq 0 ]
check $? "and taking the pointer OFF every key puts the page back to the pre-hover pixel: ${HOVER_RET_DIFF:-?}px between the capture before the hover and the one after the pointer was parked (an exact zero, the rule the selection probe applies to Escape) - a marker left lit, or a hover that repainted something else and left it changed, fails here"

# ---- 5. THE DOCK HEADER'S VIEW BUTTON REPAINTS THE PAGE IT DRIVES ----
# A REGRESSION LOOKS LIKE this: the zoom-in button works (the timeline's own ruler and lanes move)
# while the docked panel keeps the frame window it had, so the panel's own page changes by 0px -
# which is what MainDisplayWidget::syncGraphPanelSharedState not refreshing the Graph page on a
# view-range change leaves behind. The button is addressed BY NAME through the header's own
# accessibility publication, and the reading is the dock's right-hand page ([${PANELRECT:-none}]),
# so neither the header's own ink nor the object tree beside it can move the number. A missing
# capture reads 0 through magick compare and fails the bar.
ZOOM_ON="${SHOTS}/gink_50_zoom_after.png"
read -r ZPX ZPY ZPW ZPH <<< "${PANELRECT:-0 0 0 0}"
assert_exists "$ZOOM_ON" "the panel's page captured after the header zoom-in"
assert_rendered "$ZOOM_ON" "the panel's page captured after the header zoom-in"
magick "$HOVER_OFF" -crop "${ZPW}x${ZPH}+${ZPX}+${ZPY}" +repage "$SCRATCH/zoom_before.png" 2>/dev/null
magick "$ZOOM_ON" -crop "${ZPW}x${ZPH}+${ZPX}+${ZPY}" +repage "$SCRATCH/zoom_after.png" 2>/dev/null
ZOOM_DIFF=$(aediff "$SCRATCH/zoom_before.png" "$SCRATCH/zoom_after.png")
echo "zoom: the dock header's zoom-in [${ZOOM_BTN:-none}] changed ${ZOOM_DIFF}px of the panel's page [${PANELRECT:-none}]"
[ -n "${ZOOM_BTN:-}" ]
check $? "the probe pressed the dock header's view button BY NAME: [${ZOOM_BTN:-none}] (TimeLineHeader's own zoom-in - the same ladder the timeline's wheel takes), not a coordinate"
[ "${ZOOM_DIFF:-0}" -ge "$ZOOM_PAGE_MIN" ]
check $? "and pressing it repaints the docked panel's own page: ${ZOOM_DIFF:-0}px of [${PANELRECT:-none}] changed (bar ${ZOOM_PAGE_MIN}) - one notch is one rung (x1.1) of the frame ladder, so the window the panel maps shrinks about the playhead and every tick, number, curve sample and key marker away from the anchor lands on a new pixel. This is the panel's page and not the header's: the before capture is the same settled page section 4 left behind"

# bone, pose, mesh and FFD are NOT asserted here: they are structural key types and are not
# graphable at all (ctrl::graph::Channel::componentCount returns 0), so no project can put them in
# this panel.
# ---- THE DRAG READOUT IS GONE, WITH ITS INK ----
# The panel used to draw the key move's live deltas AT the pointer, and this probe used the panel's
# own Ctrl-insert (Ctrl-press on a curve) to make the key it dragged and measured the text ink in
# the box the readout would own. The design decision that removed the readout removed the phase
# with it: there is no number drawn at the pointer any more, so there is nothing to measure and no
# bar to compare against. The gestalt it leaves behind is still covered above and below: the key
# the insert writes (and its SELECTION) is what the marker/hue phases read, and the drag's own
# arithmetic stays gated offscreen (tests/graph_drag.cpp drives the move through the editor's
# cursor path). Nothing is left to assert about ink the panel no longer paints.

# ---- WHAT IS NOT ASSERTED HERE, AND WHERE IT IS GATED ----
# The panel's tick ALPHAS and its selection EMPHASIS are deliberately NOT asserted above, and
# neither is a statement about the product here:
#   * a frame tick and the frame NUMBER that belongs to it share one column, and their two bands
#     merge into ONE run in a panel-on/off difference on this build, so "which band is the ticks"
#     would be a heuristic;
#   * the marker's rings are 1px strokes at 0.95 and 0.35 over the marker's own fill, and the
#     panel's text ink is neutral and thin: at those widths a pixel count cannot separate them.
# They ARE gated, offscreen, where nothing depends on a live screen:
#   * tests/graph_panel_pixels.cpp - a model whose frame ticks carry all three levels, read off the
#     rendered image (the alphas); the key-marker pair plus the stroke group's dimmed/full-strength
#     split (the emphasis, in both states); and the marker's three inks, read through a two-row
#     probe because ImGui strokes a circle as a POLYLINE;
#   * tests/graph_panel_model.cpp - the published tick levels, the per-channel classification and
#     the per-key selection flags the drawing layer reads.
# What THIS probe adds to those is what only a running application can show: that the dock's own
# tab puts the panel on the dock's own page, that the panel publishes itself exactly once and
# stops publishing its items when its page is hidden, that its pixels land inside the dock's
# rectangle, and that a keyed frame draws at all - two key types as two colours on the panel's
# own floor.

summary probe_graph_ink
