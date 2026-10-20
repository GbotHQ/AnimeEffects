#!/bin/bash
# THE TIMELINE TAB AND THE GRAPH TAB ARE ONE CHROME - measured on the application's
# own pixels, at ONE window, in ONE state.
#
# The user's complaint is not "the panel is a bit off": it is that the graph's ruler,
# playhead pill and scroll bar do not look like the timeline's, and that no engine
# limitation explains it. So this probe does what the eye does when it switches tabs:
# it builds ONE state (a new 600-frame project, the playhead set in the ruler, the
# frame axis zoomed in until the visible window is much narrower than the range - the
# state where both views have travel and both bars must be drawn), captures the dock
# with the TIMELINE tab current, captures it again with the GRAPH tab current WITHOUT
# touching the state, and asserts the same numbers from both captures:
#
#   * the ruler strip's rows: the playhead pill's own row run and the hairline row
#     under the band, in absolute screen rows - the two strips are the same rows of
#     the same page, as the earlier capture measured them to be;
#   * the pill's box: its row run and its column run READ FROM ITS PAGE'S OWN LEFT
#     EDGE (the two pages are not the same width - the dock takes the width of its
#     header's controls - so x is compared page-relative; rows are absolute);
#   * the pill's inks: the FILL colour and the ring of the NUMBER inside it, compared
#     between the two pages. This is the assertion that fails for the shipped defect:
#     the timeline's number is theme::Colors::accentBrightText (dark on the violet
#     fill) and the panel's was theme::Colors::text (near-white - two lit pixels);
#   * both lanes: the horizontal scroll bar's pill is 4 rows thick and sits in the
#     page's last 12 rows in BOTH - i.e. the lane's top row is page.bottom() - 11 and
#     its pill spans page.bottom() - 7 .. page.bottom() - 4, flush with the page's
#     bottom edge, with the trough invisible.
#
# HOW EACH NUMBER IS FOUND, and no theme constant is baked in: the accent fill and the
# scroll bar's pill are LEARNED from each capture. In the page's top rows the only
# chromatic ink on a keyless project is the playhead (ColorAccent), so `inkhues`
# names it; the pill's run is the widest column run of that colour (the 1px line is
# inside it), and its row run is the rows of a 2px strip inside the pill that are off
# the band's base colour. The lane's pill is the farthest NEUTRAL ink in the page's
# last 12 rows (`neutral`), and its rows are the rows of a 1px strip at its own centre
# that are off the lane's base. The number's ink is the pixel farthest from the fill
# inside the pill box (`far`).
#
# WHY THE PAGE'S LEFT EDGE IS MEASURED, NOT ASSUMED: the timeline's page and the
# panel's page have different widths (the dock sizes itself to the header's controls,
# and the Graph page has a Channels button the Timeline page does not), so a column is
# only comparable WITHIN a page. It is found per capture with the same instrument
# probe_graph_anchor.sh uses for the same boundary: the tree splitter's hairline is
# the one column inked in every row of the dock's body, so it is the first column run
# `inkcols` returns; the page starts on the column after it.
#
# THE STATE. A new project (the application's own 600 frames) so there is room to zoom;
# a left press in the timeline's ruler band sets the playhead under the pointer (the
# ruler's own seek, the same gesture both views' bands drive), and six wheel notches
# with the pointer ON that playhead column zoom in about it (1.1^6 = 1.77x from the
# timeline's seeded 1.1 px/frame, i.e. a visible window around 320 frames of the 600 -
# a third of the range, so both the timeline and the panel have real travel). The
# wheel is sent with the pointer on the column because a notch goes to the widget
# under the pointer and every capture parks the pointer on the File menu (the panel
# has hover states of its own).
#
# FAILING LOUDLY: a pill that is not found, a lane whose pill is not 4 rows thick or
# is not flush with the page's bottom, or two inks that differ all report FAIL with
# both measured numbers - the run never compares two unknowns and calls them equal.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

GEOM=/tmp/probe_graph_parity_geom.txt
SCRATCH=/tmp/probe_graph_parity
STEPS=/tmp/probe_graph_parity_steps.log
INK="$ROOT/scripts/gui/ae_ink.py"
APPLOG=/tmp/probe_graph_parity_app.log
rm -f "$GEOM"
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"

# The page's top rows the playhead pill is searched in: the ruler band (22) plus the
# margin and the plot's first rows. The pill is inside the band, so 60 rows is slack.
BAND_ROWS=60
# The ruler band's height and the pill's row inside it, from the model's own constants
# (gui::frameRuler::kBandHeight == Scaler::kRulerBandHeight == 22, and kPillTop ==
# kNumberTop + 1 == 2): the hairline row is derived from the pill's row with these.
BAND_HEIGHT=22
PILL_TOP=2
# The wheel notches that give the frame axis travel (see THE STATE above).
ZOOM_NOTCHES=6
# The page's bottom band the horizontal scroll bar lives in: Scaler::kScrollBarThickness,
# which is AppStyle's own lane (src/gui/AppStyle.cpp, PM_ScrollBarExtent).
LANE_ROWS=12
# AppStyle's pill thickness (kPillThickness): the bar's 4px slider.
PILL_ROWS=4
export GEOM SCRATCH STEPS INK APPLOG BAND_ROWS BAND_HEIGHT PILL_TOP ZOOM_NOTCHES LANE_ROWS PILL_ROWS

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  set -u
  cd "$AE_PROBE_DIR"
  "$APP" "${PROJ:-}" >"$APPLOG" 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID
  : > "$STEPS"
  say() { echo "t+${SECONDS}s  $*" >> "$STEPS"; }
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

  inkpx() { python3 "$INK" "$@"; }
  raw() { magick "$1" -crop "$2" +repage -depth 8 rgb:"$3"; }
  # EVERY CAPTURE PARKS THE POINTER ON THE FILE MENU: the panel has hover states of its
  # own (a marker brightens, a chip appears), and a capture taken with the pointer over
  # the plot would measure one of them.
  FR=$(ae extents "File")
  [ -n "$FR" ] || abort "no File menu: this is not the application window"
  read -r FX FY FW FH <<< "$FR"
  POINTX=$((FX + 3)); POINTY=$((FY + FH / 2))
  park() { xdotool mousemove --sync $POINTX $POINTY; sleep 0.5; }
  shot() { park; import -window root "$1"; }
  tab() {
    local r x y w h
    r=$(ae extents "$1")
    [ -n "$r" ] || return 1
    read -r x y w h <<< "$r"
    xdotool mousemove --sync $((x + w / 2)) $((y + h / 2)) click 1
    sleep 1.8
    return 0
  }

  # THE PROJECT, at the application own defaults (600 frames), so the frame axis can be
  # zoomed past the ladder floor without clamping and the content is wider than the
  # window at the zoom this probe uses.
  ae_press "File" "the File menu opens"
  sleep 1
  ae_press "New project" "the New project entry is pressed"
  sleep 2.5
  ae_activate "Create" "a default new project is created"
  sleep 4

  # ---- the dock, and the page start (the splitter hairline) ----
  DOCK=$(ae extents "Animation")
  [ -n "$DOCK" ] || abort "no visible widget named Animation: this is not the docked layout"
  read -r DX DY DW DH <<< "$DOCK"
  echo "DOCK=\"$DOCK\"" >> "$GEOM"
  # The body starts below the dock header (the page tabs and their controls). Its
  # height is what the ruler hairline and the lane are measured inside; the exact
  # offset does not matter, because every row below is relative to BODY_TOP or to the
  # dock own bottom row.
  BODY_TOP=$((DY + 37))
  BODY_H=$((DY + DH - BODY_TOP))
  PAGE_BOTTOM=$((DY + DH - 1))
  echo "BODY_TOP=\"$BODY_TOP\"" >> "$GEOM"
  echo "PAGE_BOTTOM=\"$PAGE_BOTTOM\"" >> "$GEOM"
  say "dock $DOCK; body top $BODY_TOP, page bottom row $PAGE_BOTTOM"

  # page_left <shot> -> the first column of the dock body AFTER the tree splitter, in
  # absolute x. The splitter is the one column inked in EVERY row of the body, so
  # `inkcols` with min = the body height returns it first; the page starts after it.
  page_left() {
    local s=$1
    raw "$s" "${DW}x${BODY_H}+${DX}+${BODY_TOP}" "$SCRATCH/pl.raw"
    local bg
    bg=$(inkpx modal "$SCRATCH/pl.raw" "$DW" "$BODY_H" 0 0 "$DW" "$BODY_H" | cut -d" " -f1-3)
    case "$bg" in ""|none) echo ""; return ;; esac
    read -r br bgg bb <<< "$bg"
    inkpx inkcols "$SCRATCH/pl.raw" "$DW" "$BODY_H" 0 $((BODY_H - 1)) "$br" "$bgg" "$bb" 12 "$BODY_H" \
      > "$SCRATCH/pl_cols.txt"
    local c0 c1 cn cr cg cb
    while read -r c0 c1 cn cr cg cb; do
      case "$c0" in ""|none) continue ;; esac
      echo $((DX + c0 + 1))
      return
    done < "$SCRATCH/pl_cols.txt"
    echo ""
  }

  # measure <shot> <tag> <page_left> : every parity number for one page, into $SCRATCH/<tag>.*
  # and into the GEOM file as <TAG>_FILL / <TAG>_PILL / <TAG>_INK / <TAG>_HAIR / <TAG>_LANE.
  measure() {
    local s=$1 tag=$2 pl=$3
    local right=$((DX + DW))
    local w=$((right - pl))
    if [ "$w" -le 0 ]; then abort "the page [${s}] has no columns after the splitter (left $pl, dock right $right)"; fi

    # ---- the playhead pill, in the page own top rows ----
    raw "$s" "${w}x${BAND_ROWS}+${pl}+${BODY_TOP}" "$SCRATCH/$tag.band.raw"
    local base hue
    base=$(inkpx modal "$SCRATCH/$tag.band.raw" "$w" "$BAND_ROWS" 0 0 "$w" "$BAND_ROWS" | cut -d" " -f1-3)
    case "$base" in ""|none) abort "the $tag page has no modal band colour"; ;; esac
    read -r br bgg bb <<< "$base"
    hue=$(inkpx inkhues "$SCRATCH/$tag.band.raw" "$w" "$BAND_ROWS" 0 0 "$w" "$BAND_ROWS" \
                        "$br" "$bgg" "$bb" 10 40 40 | head -1)
    case "$hue" in ""|none) abort "the $tag page shows no chromatic ink in its ruler band: the playhead pill was not found"; ;; esac
    read -r hr hg hb hcount <<< "$hue"
    # The pill COLUMN run: the widest run of the fill colour (the 1px playhead line is
    # inside it, so the run is the pill own width).
    inkpx cols "$SCRATCH/$tag.band.raw" "$w" "$BAND_ROWS" 0 $((BAND_ROWS - 1)) "$hr" "$hg" "$hb" 2 1 \
      > "$SCRATCH/$tag.pill_cols.txt"
    local px0="" px1="" pw=0 c0 c1 cn
    while read -r c0 c1 cn; do
      case "$c0" in ""|none) continue ;; esac
      local len=$((c1 - c0 + 1))
      if [ "$len" -gt "$pw" ]; then pw=$len; px0=$c0; px1=$c1; fi
    done < "$SCRATCH/$tag.pill_cols.txt"
    [ -n "$px0" ] || abort "the $tag page has no pill column run of its fill colour"
    # The pill ROW run: a 2px strip just inside the pill left edge (past its 3px
    # corner radius, clear of the 1px line at its centre) keeps the rows whose BOTH
    # pixels are off the band base - the pill own rows, and the line alone cannot
    # reach the bar.
    local sx=$((px0 + 4))
    raw "$s" "2x${BAND_ROWS}+$((pl + sx))+${BODY_TOP}" "$SCRATCH/$tag.pill.raw"
    inkpx rows "$SCRATCH/$tag.pill.raw" 2 "$BAND_ROWS" "$br" "$bgg" "$bb" 10 1.0 \
      > "$SCRATCH/$tag.pill_rows.txt"
    local py0="" py1="" ph=0 r0 r1
    while read -r r0 r1; do
      case "$r0" in ""|none) continue ;; esac
      local len=$((r1 - r0 + 1))
      if [ "$len" -gt "$ph" ]; then ph=$len; py0=$r0; py1=$r1; fi
    done < "$SCRATCH/$tag.pill_rows.txt"
    [ -n "$py0" ] || abort "the $tag page published no pill row run inside its own fill column"
    local absx0=$((pl + px0))
    local absy0=$((BODY_TOP + py0))
    echo "${tag}_FILL=\"$hr $hg $hb $hcount\"" >> "$GEOM"
    echo "${tag}_PILL=\"$((px0)) $pw $absx0 $absy0 $ph\"" >> "$GEOM"
    say "$tag: pill page-relative x $px0..$((px0 + pw - 1)) (absolute $absx0..$((absx0 + pw - 1))), rows $absy0..$((absy0 + ph - 1)), fill $hr,$hg,$hb ($hcount px)"

    # ---- the number ink inside the pill ----
    # `far` inside the pill box against the FILL: the pixel farthest from the fill is the
    # glyph core, and it is the token the two pages must agree about.
    local inkline
    inkline=$(inkpx far "$SCRATCH/$tag.band.raw" "$w" "$BAND_ROWS" "$px0" "$py0" "$pw" "$ph" \
                    "$hr" "$hg" "$hb" 30)
    echo "${tag}_INK=\"$inkline\"" >> "$GEOM"
    say "$tag: the number inside the pill: $inkline (far from the fill, so the glyph own ink)"

    # ---- the hairline under the band ----
    # THE ROW IS THE BAND OWN ARITHMETIC. The pill rides the band number row at
    # frameRuler::kPillTop (2 rows down) and is kNumberHeight (14) tall, while the band
    # is kBandHeight (22) rows, so the band top is absy0 - 2 and the hairline - the row
    # UNDER it - is absy0 + 22 - 2 = absy0 + 20. (Not absy0 + the pill height: the 6
    # rows between the number row and the band bottom are band surface.) The column is
    # clear of the pill, of every number and of the playhead line (they all live at
    # frame columns; 100px into the page is the band quiet part on this zoomed, keyless
    # project).
    local hair_y=$((absy0 + BAND_HEIGHT - PILL_TOP))
    local hair_x=$((pl + 100))
    [ "$hair_x" -lt "$right" ] || hair_x=$((pl + 20))
    raw "$s" "1x1+${hair_x}+${hair_y}" "$SCRATCH/$tag.hair.raw"
    local hair
    hair=$(inkpx px "$SCRATCH/$tag.hair.raw" 1 1 0 0)
    echo "${tag}_HAIR=\"$hair_x $hair_y $hair\"" >> "$GEOM"
    say "$tag: hairline row $hair_y at x ${hair_x}: $hair"

    # ---- the horizontal LANE, in the page own last 12 rows ----
    # The trough is invisible (AppStyle draws none), so the lane is measured by its own
    # arithmetic and its PILL: the slider is the farthest neutral ink of the band, and
    # its rows are the 1px strip at its centre that is off the band base.
    local lane_top=$((PAGE_BOTTOM - LANE_ROWS + 1))
    raw "$s" "${w}x${LANE_ROWS}+${pl}+${lane_top}" "$SCRATCH/$tag.lane.raw"
    local lbase pill
    lbase=$(inkpx modal "$SCRATCH/$tag.lane.raw" "$w" "$LANE_ROWS" 0 0 "$w" "$LANE_ROWS" | cut -d" " -f1-3)
    case "$lbase" in ""|none) abort "the $tag page own lane band has no modal colour"; ;; esac
    read -r lr lg lbb <<< "$lbase"
    pill=$(inkpx neutral "$SCRATCH/$tag.lane.raw" "$w" "$LANE_ROWS" 0 0 "$w" "$LANE_ROWS" \
                    "$lr" "$lg" "$lbb" 6 8 | cut -d" " -f4-6)
    case "$pill" in ""|none|*none*) abort "the $tag page draws no neutral pill in its last ${LANE_ROWS} rows: the scroll bar is absent or invisible"; ;; esac
    read -r sr sg sb <<< "$pill"
    # ...its columns, so the strip below is taken from inside the pill.
    inkpx cols "$SCRATCH/$tag.lane.raw" "$w" "$LANE_ROWS" 0 $((LANE_ROWS - 1)) "$sr" "$sg" "$sb" 6 1 \
      > "$SCRATCH/$tag.lane_cols.txt"
    local lx0="" lx1="" lw=0
    while read -r c0 c1 cn; do
      case "$c0" in ""|none) continue ;; esac
      local len=$((c1 - c0 + 1))
      if [ "$len" -gt "$lw" ]; then lw=$len; lx0=$c0; lx1=$c1; fi
    done < "$SCRATCH/$tag.lane_cols.txt"
    [ -n "$lx0" ] || abort "the $tag page own lane pill has no column run"
    local lsx=$((lx0 + lw / 2))
    raw "$s" "1x${LANE_ROWS}+$((pl + lsx))+${lane_top}" "$SCRATCH/$tag.lane_pill.raw"
    inkpx rows "$SCRATCH/$tag.lane_pill.raw" 1 "$LANE_ROWS" "$lr" "$lg" "$lbb" 6 1.0 \
      > "$SCRATCH/$tag.lane_rows.txt"
    local ly0="" ly1="" lh=0
    while read -r r0 r1; do
      case "$r0" in ""|none) continue ;; esac
      local len=$((r1 - r0 + 1))
      if [ "$len" -gt "$lh" ]; then lh=$len; ly0=$r0; ly1=$r1; fi
    done < "$SCRATCH/$tag.lane_rows.txt"
    [ -n "$ly0" ] || abort "the $tag page own lane pill has no row run in its own column"
    echo "${tag}_LANE=\"$((lane_top + ly0)) $lh $((pl + lx0)) $lw $sr $sg $sb\"" >> "$GEOM"
    say "$tag: lane rows $lane_top..$PAGE_BOTTOM, pill rows $((lane_top + ly0))..$((lane_top + ly1)) (${lh} thick), x $((pl + lx0))..$((pl + lx0 + lw - 1)), ink $sr,$sg,$sb"
  }

  # ---- the TIMELINE page: one state, then the capture ----
  tab "Timeline" || abort "the dock header has no pressable tab named Timeline"
  sleep 0.6
  shot "$SHOTS/parity_00_timeline_probe.png"
  PL_T=$(page_left "$SHOTS/parity_00_timeline_probe.png")
  [ -n "$PL_T" ] || abort "the timeline page own splitter hairline was not found"
  echo "PL_T=\"$PL_T\"" >> "$GEOM"
  # THE PLAYHEAD, set with the ruler own seek: a left press in the timeline ruler
  # band (its strip is the page own first rows) puts the frame under the pointer. Its y
  # is inside the band, below the page top and above the body second row group.
  xdotool mousemove --sync $((PL_T + 300)) $((BODY_TOP + 20)) click 1
  sleep 1.2
  # ...and the ZOOM, with the pointer on the playhead column AFTER the capture it is
  # compared against (a wheel goes to the widget under the pointer, and the pointer has
  # just been parked by shot()): the frame under the pointer stays there, so the pill
  # does not walk out of the page.
  shot "$SHOTS/parity_01_timeline_seek.png"
  measure "$SHOTS/parity_01_timeline_seek.png" T "$PL_T"
  . "$GEOM"
  read -r _tp _tw _tax _tay _th <<< "${T_PILL:-0 0 0 0 0}"
  xdotool mousemove --sync $((_tax + _tw / 2)) $((_tay + _th / 2))
  sleep 0.4
  xdotool click --repeat $ZOOM_NOTCHES --delay 120 5
  sleep 1.2
  shot "$SHOTS/parity_02_timeline.png"
  measure "$SHOTS/parity_02_timeline.png" T "$PL_T"
  . "$GEOM"

  # ---- the GRAPH page, the SAME state ----
  tab "Graph" || abort "the dock header has no pressable tab named Graph"
  sleep 0.8
  shot "$SHOTS/parity_03_graph.png"
  PL_G=$(page_left "$SHOTS/parity_03_graph.png")
  [ -n "$PL_G" ] || abort "the graph page own splitter hairline was not found"
  echo "PL_G=\"$PL_G\"" >> "$GEOM"
  # The panel own published rectangle (its root Pane, found by the Fit graph action it
  # owns - the same route probe_graph_anchor.sh uses): the graph page own exact canvas, so
  # the lane bottom row is checked against the widget that draws it and not only
  # against the dock.
  PLOT=$(ae extents "Fit graph")
  ae dump > "$SCRATCH/dump_graph.txt" 2>/dev/null
  PNAME=$(awk "
    /^ *panel *\\|/ { nm = \$0; sub(/^ *panel *\\| */, \"\", nm) }
    /^ *button *\\| Fit graph\$/ { print nm; exit }
  " "$SCRATCH/dump_graph.txt")
  PANEL=""
  if [ -n "$PNAME" ]; then PANEL=$(ae extents "$PNAME" "panel"); fi
  echo "PANEL=\"${PANEL:-}\"" >> "$GEOM"
  echo "PLOT=\"${PLOT:-}\"" >> "$GEOM"
  measure "$SHOTS/parity_03_graph.png" G "$PL_G"
  . "$GEOM"

  kill $app 2>/dev/null
  wait $app 2>/dev/null
  say "body done"
' 2>&1 | tail -6

. "$GEOM" 2>/dev/null
if [ -n "${ABORT:-}" ]; then
    echo "steps: $(tr "\n" ";" < "$STEPS" 2>/dev/null)"
    check 1 "the parity probe reached both pages and measured them (it stopped: ${ABORT})"
    summary probe_graph_parity
fi

read -r TFR TFG TFB TFN <<< "${T_FILL:-0 0 0 0}"
read -r T_px T_pw T_ax T_ay T_ph <<< "${T_PILL:-0 0 0 0 0}"
read -r T_in T_ix T_iy T_ir T_ig T_ib T_id <<< "${T_INK:-0}"
read -r T_hx T_hy T_hr T_hg T_hb <<< "${T_HAIR:-0 0 0 0 0}"
read -r T_ly T_lh T_lx T_lw T_lr T_lg T_lb <<< "${T_LANE:-0 0 0 0 0 0 0}"
read -r GFR GFG GFB GFN <<< "${G_FILL:-0 0 0 0}"
read -r G_px G_pw G_ax G_ay G_ph <<< "${G_PILL:-0 0 0 0 0}"
read -r G_in G_ix G_iy G_ir G_ig G_ib G_id <<< "${G_INK:-0}"
read -r G_hx G_hy G_hr G_hg G_hb <<< "${G_HAIR:-0 0 0 0 0}"
read -r G_ly G_lh G_lx G_lw G_lr G_lg G_lb <<< "${G_LANE:-0 0 0 0 0 0 0}"
read -r DX DY DW DH <<< "${DOCK:-0 0 0 0}"
PB=${PAGE_BOTTOM:-0}

assert_rendered "$SHOTS/parity_02_timeline.png" "the timeline page"
assert_rendered "$SHOTS/parity_03_graph.png" "the graph page"

echo "measured: timeline page left ${PL_T:-?}: pill box page-relative x ${T_px:-?}+${T_pw:-?}, rows ${T_ay:-?}..$((T_ay + T_ph - 1)), fill ${TFR:-?},${TFG:-?},${TFB:-?}, number ink ${T_ir:-?},${T_ig:-?},${T_ib:-?} (${T_in:-0} px far from the fill), hairline row ${T_hy:-?} rgb ${T_hr:-?},${T_hg:-?},${T_hb:-?}, lane pill rows ${T_ly:-?}..$((T_ly + T_lh - 1)) (${T_lh:-?} thick) at x ${T_lx:-?}+${T_lw:-?}"
echo "measured: graph page left ${PL_G:-?}: pill box page-relative x ${G_px:-?}+${G_pw:-?}, rows ${G_ay:-?}..$((G_ay + G_ph - 1)), fill ${GFR:-?},${GFG:-?},${GFB:-?}, number ink ${G_ir:-?},${G_ig:-?},${G_ib:-?} (${G_in:-0} px far from the fill), hairline row ${G_hy:-?} rgb ${G_hr:-?},${G_hg:-?},${G_hb:-?}, lane pill rows ${G_ly:-?}..$((G_ly + G_lh - 1)) (${G_lh:-?} thick) at x ${G_lx:-?}+${G_lw:-?}"
echo "measured: page bottom row ${PB:-?} (both pages), panel [${PANEL:-unpublished}], plot [${PLOT:-unpublished}]"

maxdiff() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; print d }'; }

# ---- 1. BOTH PILLS WERE FOUND, and in the same rows ----
found_ok=1
[ "${T_pw:-0}" -gt 0 ] && [ "${T_ph:-0}" -gt 0 ] || found_ok=0
[ "${G_pw:-0}" -gt 0 ] && [ "${G_ph:-0}" -gt 0 ] || found_ok=0
[ "${T_hx:-0}" -gt 0 ] && [ "${G_hx:-0}" -gt 0 ] || found_ok=0
[ "$TFR" -gt 0 ] && [ "$GFR" -gt 0 ] || found_ok=0
check "$((1 - found_ok))" "both pages draw the playhead pill in their ruler band and it was found by its fill colour (timeline ${TFR:-?},${TFG:-?},${TFB:-?} ${TFN:-0} px, graph ${GFR:-?},${GFG:-?},${GFB:-?} ${GFN:-0} px): a page with no pill has nothing to compare"

# ---- 2. THE PILL'S BOX: the same rows, the same size, the same page-relative column ----
rows_ok=0
[ "$(maxdiff "${T_ay:-0}" "${G_ay:-0}")" -le 1 ] && [ "$(maxdiff "${T_ph:-0}" "${G_ph:-0}")" -le 1 ] && rows_ok=1
check "$((1 - rows_ok))" "the pill's BOX is the same box on both pages: rows ${T_ay:-?}..$((T_ay + T_ph - 1)) (${T_ph:-?} tall) against ${G_ay:-?}..$((G_ay + G_ph - 1)) (${G_ph:-?} tall) - the two rulers' strips are the same rows of the same page"
cols_ok=0
[ "$(maxdiff "${T_px:-0}" "${G_px:-0}")" -le 2 ] && [ "$(maxdiff "${T_pw:-0}" "${G_pw:-0}")" -le 2 ] && cols_ok=1
check "$((1 - cols_ok))" "and its column is the same column OF ITS PAGE: ${T_px:-?}+${T_pw:-?} from the timeline page's splitter against ${G_px:-?}+${G_pw:-?} from the graph page's (the pages are different widths, so x is compared page-relative; the pill is as wide on both)"

# ---- 3. THE PILL'S INKS: the fill and the number on it ----
fill_ok=0
[ "$(maxdiff "${TFR:-0}" "${GFR:-0}")" -le 6 ] && [ "$(maxdiff "${TFG:-0}" "${GFG:-0}")" -le 6 ] \
    && [ "$(maxdiff "${TFB:-0}" "${GFB:-0}")" -le 6 ] && fill_ok=1
check "$((1 - fill_ok))" "the pill's FILL is one token on both pages: ${TFR:-?},${TFG:-?},${TFB:-?} against ${GFR:-?},${GFG:-?},${GFB:-?} (theme::Colors::accentBright through theme::Graph::selected() and TimeCursor's badge)"
ink_ok=0
[ "${T_in:-0}" -ge 10 ] && [ "${G_in:-0}" -ge 10 ] \
    && [ "$(maxdiff "${T_ir:-0}" "${G_ir:-0}")" -le 24 ] \
    && [ "$(maxdiff "${T_ig:-0}" "${G_ig:-0}")" -le 24 ] \
    && [ "$(maxdiff "${T_ib:-0}" "${G_ib:-0}")" -le 24 ] && ink_ok=1
check "$((1 - ink_ok))" "THE NUMBER INSIDE THE PILL IS THE SAME INK ON BOTH PAGES: timeline ${T_ir:-?},${T_ig:-?},${T_ib:-?} (${T_in:-0} px far from the fill) against the graph's ${G_ir:-?},${G_ig:-?},${G_ib:-?} (${G_in:-0} px). This is the shipped defect's assertion: the timeline writes theme::Colors::accentBrightText on the accent fill and the panel wrote the ruler's near-white text token, which left two lit pixels"
# ...and the ink is OFF the fill by enough to read (not a near-copy of it).
inkread_ok=0
[ "${T_id:-0}" -ge 30 ] && [ "${G_id:-0}" -ge 30 ] && inkread_ok=1
check "$((1 - inkread_ok))" "and that ink is far enough from the fill to read at all: ${T_id:-0} / ${G_id:-0} px away (the bar is 30; the defect measured 2 lit pixels of a near-fill colour)"

# ---- 4. THE STRIP'S ROWS: the hairline under the band ----
hair_ok=0
[ "$(maxdiff "${T_hy:-0}" "${G_hy:-0}")" -le 1 ] && [ "$(maxdiff "${T_hr:-0}" "${G_hr:-0}")" -le 6 ] \
    && [ "$(maxdiff "${T_hg:-0}" "${G_hg:-0}")" -le 6 ] && [ "$(maxdiff "${T_hb:-0}" "${G_hb:-0}")" -le 6 ] \
    && hair_ok=1
check "$((1 - hair_ok))" "the hairline under the ruler band is the same ROW and the same INK on both pages: row ${T_hy:-?} rgb ${T_hr:-?},${T_hg:-?},${T_hb:-?} against row ${G_hy:-?} rgb ${G_hr:-?},${G_hg:-?},${G_hb:-?} - one half-covered rule (the timeline's 1px pen centred on the strip's boundary), not a full-strength separator in one view"

# ---- 5. BOTH LANES: 12 rows, flush with the page's bottom, a 4px pill in them ----
lane_ok=0
[ "$T_lh" -eq "$PILL_ROWS" ] && [ "$G_lh" -eq "$PILL_ROWS" ] \
    && [ "$((T_ly + T_lh - 1))" -eq "$((PB - 4))" ] && [ "$((G_ly + G_lh - 1))" -eq "$((PB - 4))" ] \
    && [ "$T_ly" -eq "$((PB - LANE_ROWS + 1 + (LANE_ROWS - PILL_ROWS) / 2))" ] \
    && [ "$G_ly" -eq "$((PB - LANE_ROWS + 1 + (LANE_ROWS - PILL_ROWS) / 2))" ] && lane_ok=1
check "$((1 - lane_ok))" "the horizontal scroll bar is the SAME LANE on both pages: its ${PILL_ROWS}px pill sits at rows ${T_ly:-?}..$((T_ly + T_lh - 1)) on the timeline and ${G_ly:-?}..$((G_ly + G_lh - 1)) on the graph, and the lane's own slot is the page's last ${LANE_ROWS} rows (${PB:-?} is the page's bottom row, so the pill spans page.bottom()-7..page.bottom()-4) - flush with the bottom edge, no trough, the app's CC_ScrollBar geometry"
lane_ink_ok=0
[ "$(maxdiff "${T_lr:-0}" "${G_lr:-0}")" -le 6 ] && [ "$(maxdiff "${T_lg:-0}" "${G_lg:-0}")" -le 6 ] \
    && [ "$(maxdiff "${T_lb:-0}" "${G_lb:-0}")" -le 6 ] && lane_ink_ok=1
check "$((1 - lane_ink_ok))" "and its pill is one ink on both: ${T_lr:-?},${T_lg:-?},${T_lb:-?} against ${G_lr:-?},${G_lg:-?},${G_lb:-?} (theme::Colors::outline, or hover while hot)"
# THE GRAPH PAGE HAS TRAVEL, which is the state this probe is built in: a lane pill
# shorter than the lane is a window narrower than the content.
travel_ok=0
[ "${G_lw:-0}" -gt 0 ] && [ "${G_lw:-0}" -lt "$((DX + DW - PL_G))" ] && travel_ok=1
check "$((1 - travel_ok))" "and the graph page's horizontal bar exists WITH TRAVEL in this state: its pill is ${G_lw:-?}px of the ${DW:-?}px dock it is drawn in (the frame window is a fraction of the 600-frame range at the ${ZOOM_NOTCHES} notches this probe zooms in, so a thumb shorter than the lane is the proof that the bar is not suppressed)"

summary probe_graph_parity
