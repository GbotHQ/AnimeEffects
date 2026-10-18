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
#      layers", "N channel(s) shown", ...) and whose children are its ONE control (the channels
#      button, while there is one), that summary as a label and "Fit graph"; the fit button
#      appears EXACTLY ONCE in the whole tree, which is what rules a second publication out (the
#      docked host takes the panel's ONE accessibility installation - gui::a11y::install is called
#      for one widget and kept - and two hosts publishing one panel is exactly the bug that would
#      hide here). With the Timeline tab current the SAME node is still in the tree,
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
#      those types with), the panel's ONE control is present and names the count it changes, and
#      a key DRAG draws its delta readout at the pointer - and nowhere else, before or after the
#      gesture. The keys come from the application's own routes: "Add default keys" in the
#      object tree's row menu puts ten channels at frame 0, and the Properties dock's Current
#      Keys knobs put a key of TWO key TYPES at the frame the playhead has been moved to (a
#      frame-0 marker is culled at the plot's left edge, so the knobs are what makes the plot
#      show keys at all).
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
# THE READOUT'S BOX. The panel draws the readout AT THE POINTER, offset by gui::kReadoutGapPx
# (12) and clamped inside the panel, so the box starts 8px out - clear of the marker under the
# pointer, whose ring reaches 6.35px - and is 210x24, wider and taller than the drawn text.
RO_GAP=8
RO_W=210
RO_H=24
# The readout is THE PANEL'S TEXT INK: neutral (a chromatic curve crossing its box is not text)
# and far from the floor. The panel's own labels are theme::Graph::axisText == Colors::text;
# on this palette that is 208 away from the floor rgb 38,38,38, so 100 is half of it, and a
# channel spread of 24 admits every antialiased glyph pixel while refusing every key hue.
RO_INK_DIST=100
RO_INK_CHROMA=24
# ...and a drawn readout is a sentence: dozens of glyph pixels. The bar is one short word.
RO_INK_MIN=60
export GEOM SCRATCH INK APPLOG DOCK_CHANGE_MIN DOCK_CHANGE_SHARE HUE_TOL HUE_CURVE_MIN \
       HUE_COLS_MIN HUE_SEP HUE_MIN_PX RO_GAP RO_W RO_H RO_INK_DIST RO_INK_CHROMA RO_INK_MIN

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
    CBTN=$(sed -n "s/^ *button *| \([0-9]\{1,\} channel(s)\)$/\1/p" "$SCRATCH/dump_graph_keys.txt" | head -1)
    say "candidate [$cand]: the panel publishes [${KEYSUM:-nothing}] channel(s) shown, its control reads [${CBTN:-none}]"
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

      # ---- the drag readout, on a key the PANEL ITSELF makes ----
      # THE INSERT IS THE PANEL OWN GESTURE: a Ctrl-press ON A CURVE inserts a key at that frame
      # (ctrl::GraphEditor::insertKeyAt, seeded from the curve so the shape does not move) and
      # SELECTS it, so its position is simply the point that was pressed - and the tree then
      # publishes it as the "Key menu" button on its own centre, which CONFIRMS the press landed
      # on a key rather than on empty plot. Nothing here has to guess where a marker is drawn.
      # The point is read off the panel own ink: a channel hue widest column run is its curve, and
      # a third of the way into that run is a point on the curve, clear of both ends.
      # THE POINT HAS TO BE INSIDE THE CURVE OWN BAND. The insert refuses a frame outside
      # [firstKey, lastKey] (ctrl::GraphEditor::curveBandItem), and the keys the knobs made are at
      # frame 0 and at the frame the playhead was moved to, so the band is the FIRST STRETCH of the
      # plot: a point a third of the way along the drawn stroke can already be past the last key,
      # where the stroke is the loop rule and no key can be inserted. The candidates are
      # therefore taken near the LEFT END of each channel hue stroke, and the press is repeated
      # over them until the tree publishes the key it wrote - which is also the confirmation that
      # the point was on the curve.
      : > "$SCRATCH/insert.txt"
      while read -r r g b n ncols left; do
        [ -n "${left:-}" ] || continue
        [ "$left" -ge 0 ] || continue
        # the point: the hue centre of mass over the FIRST columns of its leftmost run, which is on
        # the stroke as near its left end as the ink allows - inside the band the insert needs
        C=$(inkpx centroid "$SCRATCH/panel.raw" $PW2 $PH2 "$left" 0 17 $PH2 "$r" "$g" "$b" $HUE_TOL)
        read -r ccx ccy ccn <<< "$C"
        [ "${ccx:-none}" != "none" ] || continue
        [ "${ccn:-0}" -ge "$HUE_MIN_PX" ] || continue
        printf "%s %s %s %s %s %s\n" "$((PX2 + ccx))" "$((PY2 + ccy))" "$r" "$g" "$b" >> "$SCRATCH/insert.txt"
      done < "$SCRATCH/hues_cand.txt"
      INSX=0; INSY=0; KM=""
      while read -r cx cy cr cg cb; do
        [ -n "${cx:-}" ] || continue
        say "trying the panel own Ctrl-insert on the curve rgb $cr,$cg,$cb at $cx,$cy"
        xdotool mousemove --sync $cx $cy; sleep 0.4
        xdotool keydown ctrl; sleep 0.15
        xdotool click 1; sleep 0.15
        xdotool keyup ctrl; sleep 1.5
        park
        KM=$(ae extents "Key menu")
        if [ -n "$KM" ]; then INSX=$cx; INSY=$cy; break; fi
      done < "$SCRATCH/insert.txt"
      echo "INSERT_AT=\"$INSX $INSY\"" >> "$GEOM"
      if [ "$INSX" -gt 0 ]; then
        echo "KEYMENU=\"${KM:-}\"" >> "$GEOM"
        if [ -n "$KM" ]; then
          read -r MX4 MY4 MW4 MH4 <<< "$KM"
          DOTX=$((MX4 + MW4 / 2)); DOTY=$((MY4 + MH4 / 2))
          echo "DOT=\"$DOTX $DOTY\"" >> "$GEOM"
          # the box the readout will own, at the gesture own end position. THE DRAG GOES UP, not
          # down: with the key near the plot bottom edge a downward drag would carry the pointer
          # past the dock (MEASURED: the dot at y=480 with the dock ending at y=495, so the box
          # landed at y=512..536, outside the page), and the readout is CLAMPED inside the panel -
          # so the box would be measured where the readout is not.
          RGX=$((DOTX + 60)); RGY=$((DOTY - 40))
          echo "RO_BOX=\"$((RGX + RO_GAP)) $((RGY + RO_GAP)) $RO_W $RO_H\"" >> "$GEOM"
          readout_ink() { # readout_ink <png>
            raw "$1" "${RO_W}x${RO_H}+$((RGX + RO_GAP))+$((RGY + RO_GAP))" "$SCRATCH/ro.raw"
            inkpx neutral "$SCRATCH/ro.raw" $RO_W $RO_H 0 0 $RO_W $RO_H "$F1" "$F2" "$F3" \
                "$RO_INK_DIST" "$RO_INK_CHROMA" | cut -d" " -f1
          }
          RO_BEFORE_V=$(readout_ink "$SHOTS/gink_30_dock_graph_keys.png")
          echo "RO_BEFORE=\"$RO_BEFORE_V\"" >> "$GEOM"
          # THE DRAG: press on the key and move in STEPS with the button held, so the gesture has an
          # ACCEPTED delta to report (a press that never crossed a pixel has none, and the model
          # draws nothing then: that is the readout own "no move in flight" signal).
          xdotool mousemove --sync $DOTX $DOTY; sleep 0.4
          xdotool mousedown 1; sleep 0.2
          i=1
          while [ "$i" -le 6 ]; do
            xdotool mousemove --sync $((DOTX + 10 * i)) $((DOTY - 6 * i))
            sleep 0.15
            i=$((i + 1))
          done
          sleep 0.6
          import -window root "$SHOTS/gink_31_key_drag.png"
          RO_DRAG_V=$(readout_ink "$SHOTS/gink_31_key_drag.png")
          echo "RO_DRAG=\"$RO_DRAG_V\"" >> "$GEOM"
          xdotool mouseup 1; sleep 1.0
          import -window root "$SHOTS/gink_32_key_after.png"
          RO_AFTER_V=$(readout_ink "$SHOTS/gink_32_key_after.png")
          echo "RO_AFTER=\"$RO_AFTER_V\"" >> "$GEOM"
          say "readout ink at [${RGX} ${RGY}]+${RO_GAP}: before [${RO_BEFORE_V:-?}] during [${RO_DRAG_V:-?}] after [${RO_AFTER_V:-?}]"
        else
          say "no Key menu after the Ctrl-insert at [$INSX $INSY]"
        fi
      else
        say "no curve point was found to insert a key at"
      fi
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
# The keys, the two key TYPES and the drag are all made by the application's own controls, so the
# routes are asserted beside the ink: which row was keyed, which two knobs were pressed, and where
# the marker was found. The crash's own log tail is printed with any failure below, because a host
# that dies drawing the keyed frame is the defect this phase exists to catch.
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
# THE PANEL'S ONE CONTROL, under the count it changes, once, inside the dock.
[ -n "${CHBTN:-}" ] && [ "$(grep -c "^ *button *| ${CHBTN}$" "$KEYSLOG" 2>/dev/null)" -eq 1 ]
check $? "and the panel publishes its ONE control under the count it changes, exactly once ([${CHBTN:-none}])"
read -r CX2 CY2 CW2 CH2 <<< "${CHBTN_RECT:-0 0 0 0}"
awk -v x="$CX2" -v y="$CY2" -v w="$CW2" -v h="$CH2" -v X="$DX" -v Y="$DY" -v W="$DW" -v H="$DH" \
    'BEGIN{ exit !(x >= X && y >= Y && x + w <= X + W && y + h <= Y + H) }'
check $? "and that control is a real one inside the dock's own rectangle ([${CHBTN_RECT:-none}] in [${DOCK:-none}])"
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
# bone, pose, mesh and FFD are NOT asserted here: they are structural key types and are not
# graphable at all (ctrl::graph::Channel::componentCount returns 0), so no project can put them in
# this panel.
# ---- THE DRAG READOUT, AT THE POINTER ----
# The key is made by the panel's own insert gesture (Ctrl-press on a curve), which SELECTS it, and
# the tree publishes a selected key as the "Key menu" button on its own centre: so the dot the drag
# starts from is read back from the application rather than guessed from ink.
echo "readout: the Ctrl-insert was made at [${INSERT_AT:-none}] on a drawn curve, the tree published the key it wrote as [${KEYMENU:-none}] so the dot is [${DOT:-none}], the box [${RO_BOX:-none}]: neutral ink in it before [${RO_BEFORE:-?}], during [${RO_DRAG:-?}], after [${RO_AFTER:-?}] (bar ${RO_INK_MIN} during, <= 2 before and after)"
# THE PRECONDITION FIRST, and it is its own assertion: if no stroke point is found there is nothing
# to insert into, and the three claims below cannot be made - which is this probe's finding, not the
# application's. MEASURED: the panel drew N hues on its floor and the probe built N candidate points.
INS_N=$(grep -c . "$SCRATCH/insert.txt" 2>/dev/null) || INS_N=0
[ "${INS_N:-0}" -ge 1 ]
check $? "the probe FOUND a stroke point on the panel's own floor to insert a key at - the precondition of the three readout claims below (chromatic hues found: [${PANEL_HUES_ALL:-none}]; points built: ${INS_N:-0})"
[ -n "${KEYMENU:-}" ] && [ -n "${DOT:-}" ]
check $? "and the panel's OWN Ctrl-insert on that stroke wrote a key, with the tree publishing it at its own centre ([${KEYMENU:-none}], pressed at [${INSERT_AT:-none}])"
[ "${RO_BEFORE:-1}" -le 2 ]
check $? "and NO delta readout is drawn at the pointer before the drag: ${RO_BEFORE:-?} pixels of the panel's neutral text ink in the ${RO_W}x${RO_H} box the readout will own"
[ "${RO_DRAG:-0}" -ge "$RO_INK_MIN" ]
check $? "while the move is in flight the readout IS drawn there: ${RO_DRAG:-0} pixels of the panel's text ink at the pointer (bar ${RO_INK_MIN})"
[ "${RO_AFTER:-1}" -le 2 ]
check $? "and the release takes it away again: ${RO_AFTER:-?} pixels in the same box"
awk -v a="${RO_DRAG:-0}" -v b="${RO_BEFORE:-0}" -v c="${RO_AFTER:-0}" -v m="$RO_INK_MIN" 'BEGIN{ exit !(a > b + c + m) }'
check $? "so the readout is ink that arrives and leaves with the gesture (${RO_BEFORE:-?} -> ${RO_DRAG:-?} -> ${RO_AFTER:-?})"

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
# own floor, and a key drag's readout following the pointer.

summary probe_graph_ink
