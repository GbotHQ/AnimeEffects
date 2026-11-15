#!/bin/bash
S=/home/gambot/dev/AnimeEffects-audio/.scratch-orch/capture-home
mkdir -p "$S/.config" "$S/.local/share" "$S/.cache"
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=generic QT_FORCE_STDERR_LOGGING=1
export HOME="$S" XDG_CONFIG_HOME="$S/.config" XDG_DATA_HOME="$S/.local/share" XDG_CACHE_HOME="$S/.cache"
/home/gambot/dev/AnimeEffects/build/src/gui/Debug/AnimeEffects \
  /home/gambot/dev/AnimeEffects-audio/assets/sample.psd > "$S/app2.log" 2>&1 &
app=$!
W=""
for i in $(seq 1 60); do
  W=$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1)
  [ -n "$W" ] && break
  sleep 1
done
xdotool windowsize "$W" 1280 860 2>/dev/null
prev=""
for i in $(seq 1 30); do
  g=$(xdotool getwindowgeometry --shell "$W" 2>/dev/null | tr '\n' ' ')
  [ -n "$g" ] && [ "$g" = "$prev" ] && break
  prev="$g"
  sleep 1
done
sleep 1
import -window root "$S/timeline_ref_large.png"
# tighter detail crops for the rebuild
convert "$S/timeline_ref_large.png" -crop 1280x240+0+600 +repage "$S/timeline_detail.png" 2>/dev/null
kill "$app" 2>/dev/null; sleep 1; kill -9 "$app" 2>/dev/null
echo "window=$W geom=$prev"
