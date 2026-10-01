# Render notes — audio UI mockup (sounds on the timeline)

Mockup: `tools/audio-ui-mockup/index.html` — self-contained, NO-BUILD: React 18 +
Babel-standalone via CDN (product components are single-source JSX), Tailwind CDN
for page chrome only, Geist + Phosphor. Styling rule, spacing scale and layout rule
are stated at the top of the `<style>` block. Open in a browser for the annotated
page (doc text + captions + spec region). The preview/captures show ONLY the product:

- `#clean` — hides every documentation element.
- `#only=<surface-id>` — clean mode + isolates one surface (ancestors and descendants
  of the target stay visible).

`--virtual-time-budget=10000` is REQUIRED in the screenshot commands: the DOM is
built by Babel-compiled React after load.

## Capture commands (run from this directory)

    CH="google-chrome --headless=new --disable-gpu --hide-scrollbars --virtual-time-budget=10000"
    $CH --window-size=1620,1160 --screenshot=preview.png               'file://'$PWD'/index.html#clean'
    $CH --window-size=1620,780  --screenshot=01-hero-timeline.png      'file://'$PWD'/index.html#only=surface-timeline'
    $CH --window-size=1560,330  --screenshot=02-states.png             'file://'$PWD'/index.html#only=surface-states'
    $CH --window-size=980,130   --screenshot=03-readouts.png           'file://'$PWD'/index.html#only=surface-readouts'
    $CH --window-size=500,130   --screenshot=04-export-surfaces.png    'file://'$PWD'/index.html#only=surface-export'
    $CH --window-size=440,320   --screenshot=05-inspector.png          'file://'$PWD'/index.html#only=surface-inspector'
    $CH --window-size=470,130   --screenshot=capture_clipface-a.png    'file://'$PWD'/index.html#only=surface-clipface-a'
    $CH --window-size=640,240   --screenshot=capture_clipface-b.png    'file://'$PWD'/index.html#only=surface-clipface-b'
    $CH --window-size=250,240   --screenshot=capture_contextmenu.png   'file://'$PWD'/index.html#only=surface-contextmenu'

## Component map (React definitions == future QML modules)

- `Lane({children, empty})` → `AudioLane` — sound lane / drop hint.
- `Clip({file, wave, playback, inExport, face, hover, selected, err, ghost, gain, x, handles, style})`
  → `Clip.qml` — one clip; `selected` = accent outline ONLY (selection spawns nothing).
- `ClipContextMenu({muted, inExport, first, last})` + `LaneContextMenu({hear})` → `ClipContextMenu.qml` — state +
  numbers display + "Edit frames…" + Reveal + Remove (app QMenu conventions).
- `Inspector({file, first, last, path})` → `Inspector.qml` — frame editor, ON-DEMAND
  ("Edit frames…" / double-click; closes on commit/Escape).
- `ReadoutStrip({latched})` → `ReadoutStrip.qml` — MIX meter + terse OVER latch.
- `ExportSurfaces()` → existing `ExportDialog.qml` surfaces (unchanged).

Surface map: `surface-timeline` (hero), `surface-states` (hover/drag/numbers/error
band; `surface-selected` = outline-only selection, `surface-inspector` = the editor
open), `surface-readouts` (both strips), `surface-export`, `surface-clipface-a/b`
(the two clip-face options), `surface-contextmenu` (clip menu), `surface-lanemenu` (lane menu scene).
Clip face: CLEAN at rest (name + wave + `ref` mark); muted = grayed + crossed-speaker
unmute control only. Lane headers carry the hearing toggle; `+` footer creates lanes.

## Palette provenance

Every hex is a verbatim cell of `src/gui/theme/TailwindPalette.h`; token roles/columns
follow `src/gui/theme/Colors.h` (dark theme, neutral row for structure, accent rows per
`body[data-accent]`, incl. `--accent-hover` = accent row dark col 6 for menu hover).
Accent is used ONLY for the playhead, the selected clip edge and the menu's checked
indicator (the app's AeCheckBox look); all audio chrome stays neutral. LED meter cells
use the sanctioned meter status rows (green `#22c55e` / amber `#f59e0b` / red `#ef4444`).
kWindowBand appears only as the ruler's playback-window band.

## Waveform time mapping

The wave's x-axis IS the clip's span (in-point→out-point), drawn flush edge-to-edge
with a mirrored amplitude envelope around a center axis; trimming an edge reveals/hides
the file's slice accordingly. Silence is drawn only where the file is actually silent.
The SVG is a replaced element (viewBox = intrinsic ratio), so a plain block wrapper
owns the body box and the SVG fills it — equal breathing room at any clip width.
