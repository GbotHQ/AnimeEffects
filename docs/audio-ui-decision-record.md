# Phase 2 designer brief — DRAFT (orchestrator scratch; dispatch only after user approves Phase 1)

Status: inventory `docs/audio-ui-inventory.md` committed (ae1de72) and presented at the Phase 1
gate. Awaiting: (a) user approval of the inventory, (b) answers to Q1–Q10 (answered = binding
input to the design; unanswered = the mockup must show the behavior as an explicit open item,
never silently pick one).

## Task

Produce HTML + Tailwind mockups of the approved audio-UI inventory at
`tools/audio-ui-mockup/` (beside the precedent `tools/dock_mockup/`). One designer agent;
no app code, no app runs, no probes.

## Binding inputs

1. `docs/audio-ui-inventory.md` (the approved inventory) — the mockup covers its catalog:
   per-track row (13 elements: add/remove, file select, playback-enable, export, start/end
   frames, volume icon/slider/percent, duration, headers, separator), config tab (C1/C2),
   transport speaker button + window hosting (Q3), playback clock states, export-dialog
   checkbox + facts row, `.aemus` save/load flows, error/loading states (Q7).
2. USER ANSWERS (2026-09-30 — BINDING DESIGN INPUT):
   - Q1 fidelity: OFFER PREVIEW OF THE ACTUAL EXPORT MIX — design an audition affordance
     (Q8 makes the engine work legitimate).
   - Q2 flag truth table: ALL 4 COMBOS MEANINGFUL — playback and export are independent;
     an export-only track exports without previewing (fixes R1's class).
   - Q3 hosting: DOCK/INLINE IN THE MAIN WINDOW — no detached tool window.
   - Q4 gapped selection: AUDIO FOLLOWS VIDEO — keep cut-together per-range concat
     (Exporter.cpp:588-593). The design principle is that audio never behaves
     differently from video; no span-export variant.
   - Q5 frame-window rules: CLAMP end ≥ start AT THE CONTROL; ONE sentinel convention:
     0 = "to end" everywhere (drop the 500 default).
   - Q6 persistence: .aemus ROUND-TRIPS ALL TRACKS — no silent loss (fixes R2).
   - Q7 failure loudness: INLINE PER-ROW ERROR STATE.
   - Q8 scope: ENGINE FEATURES ALLOWED — the design may call for engine additions
     (meters, audition of the export mix, etc.).
   - Q9 remove semantics: REMOVE THE CLICKED ROW (fixes R9's class).
   - Q10 startFrame/endFrame: TIMELINE PLACEMENT — the file begins at startFrame on the
     timeline (the export/mixer reading is primary); labels and preview must conform
     (resolves R15).
3. Theme tokens: hex values from `src/gui/theme/TailwindPalette.h` VERBATIM (26 rows × 11
   steps) + semantics from `src/gui/theme/Colors.h`: `kWindowBand` is the floating window
   surface token; accent is RESERVED for playhead/selection/marquee — audio chrome must not
   steal it; `kFloaterBody`/`kFloaterEdge` for detached-window chrome; ToolSlider look
   (circular head on thin track) assumed for volume; icons are runtime-tinted — mockups
   specify tint, not baked-in colors.
4. Roughness findings R1–R15 are what the redesign fixes or resolves — the mockup proposes
   the improved UI; where a fix depends on an unanswered Q, show variants.

## Format contract (from tools/dock_mockup/index.html)

- Self-contained `tools/audio-ui-mockup/index.html` (+ optional `preview.png`), no build step.
- "Option prototypes" page style: captioned design options side by side where behavior/look
  is open (matches the unanswered [Q] items); a coherent default where the design is settled.
- Design tokens in `:root` CSS variables sourced from TailwindPalette.h/Colors.h (state the
  source in a comment, like the precedent does); user directive says HTML + **Tailwind** —
  use Tailwind utilities (CDN play is acceptable for a mockup) while keeping palette hexes
  verbatim, not stock Tailwind colors.
- Geist font + Phosphor icons (same as precedent) unless the mockup argues otherwise.

## Process rules

- Design informs implementation, never the reverse. Nothing in UI design is negotiable;
  where the inventory + user answers do not settle a behavior, the mockup SHOWS it as an
  open option set — never adopts toolkit convention silently.
- Isolation: all work in /home/gambot/dev/AnimeEffects-audio; never touch
  /home/gambot/dev/AnimeEffects; no commits (orchestrator commits); never edit
  assets/locale/*.ts.
- No app runs, no run_probes.sh (phases 1–2 need zero app runs).

## APPENDIX — Phase 2 gate decisions (2026-09-30, user; supersede Q1 where noted)

- Q1 REVISED at the gate: the audition-play-affordance is DROPPED (users mostly finish
  audio elsewhere; preview is a focus mute, export is "what ships"). Replacement:
  passive MIX level meter + latching CLIP warning (live during preview playback).
  CONFLICTS 2/3/5 moot via this decision.
- Toggle framing (user's mental model, binding for wording): Preview toggle = "audible
  while animating" (focus mute); Export toggle = "in the exported file" (e.g. voice
  clip for timing, kept out of the file).
- '0 = to end' must NOT appear in the user-facing UI: explicit "to end" toggle on the
  end control; 0 is internal storage only and means "to end" everywhere (kills the
  end=0 EMPTY reading in getValidAudioStreams/syncTracks semantics — R15/Q5 closure).
- CONFLICT 4: F1 inline banner for file-scope failures + KEEP the overwrite-confirm modal.
- Meter styling M1/M2 remains an OPEN look option for the Phase 2 re-gate.

## APPENDIX 2 — Gate review method (binding, from user directive 2026-09-30)

EVERY design/approval gate gets BOTH instruments, in this order:
1. SCREENSHOT + FRESH-EYES REVIEW (primary): render the artifact to an image
   (google-chrome --headless=new --screenshot for HTML; app capture rules from
   AGENTS.md for QML) and give it to a MINIMAL-CONTEXT agent persona'd as the
   end user (animator/artist) — it sees ONLY the image, answers user-scenario
   probes, no project context. The original design's failure was never
   considering the user; spec-conformance review cannot catch user-experience
   failure. Fresh-eyes v2 (2026-09-30) found 6 real issues the spec review missed.
2. Spec-conformance review (secondary): the rubric/trace checking.

Fresh-eyes finding classes to always probe: label decode cost, single-glanceable
state, scattered-evidence, units/magic numbers, jargon in user-visible strings,
destructive-affordance traps, annotation/spec text polluting the visual, density.

## Fresh-eyes v3 probe set (Phase 2 re-gate — re-test v2 failure classes on the REVISED screenshot)

Minimal-context animator persona, image only. Probe: (a) one-glanceable per-row
"will this be in the exported video?" — can it be answered WITHOUT scanning
multiple places? (b) the remove affordance: does anything read as a destructive
action (or as "close" while deleting)? (c) label decode: start/end fields — units
visible? does anything suggest a magic number? (d) can ANY jargon/spec text be seen
in the user-view? (e) row density: where does the eye land first? (f) Scenario A/B
from v2 re-run (voice-guide-not-exported; is-music-in-the-export).

## APPENDIX 3 — Gate verdict 2 (2026-09-30): design-language consistency is review instrument ZERO

User verdict on mockup v2: "too busy", "a regression", "your review has been poor". Root cause:
persona-plain-language novelty ("Start frame/End frame", "Hear while working") beat the app's
OWN design language. Binding corrections:
- Frame ranges = the app's ONE concept: AeFrameRangeRow pattern ([first AeNumberField] [last
  AeNumberField] + per-row trash), labels "First frame"/"Last frame" (ExportViewModel.cpp:89-91),
  unit INSIDE the field, sentinel via AeNumberField's placeholder-word mechanism ("to end"
  replaces the number — like the export form's "Auto"). NO separate "to end" checkbox.
- Toggles = AeCheckBox (box+label click target), app's existing words "Enable playback"/"Export",
  fixed width, off = unchecked (never disabled-look). NO pills.
- DELETE: state badges (duplicate visible info), header counts (visual noise), per-row span bars
  (misaligned noise), long in-row text.
- Remove control = per-row trash (AeFrameRangeRow pattern), DIMMED on non-hovered rows (user).
- Meter = M1 row-end LED segments (user).
REVIEW HIERARCHY (binding): user feedback > persona reviews. Persona findings are input, not
authority. Instrument 0 for every future gate: design-language consistency audit vs the app's
existing controls (Ae* vocabulary, existing labels, frame-range/checkbox patterns) BEFORE any
fresh-eyes pass.

## APPENDIX 4 — Interaction model DECIDED (user, 2026-09-30): Model A — sounds live on the timeline

The disease was the interaction model (a per-track settings grid): unintuitive for new users
(cognitive load) AND daily users (field-editing for frequent ops). DECISION: audio as CLIPS IN
TIMELINE LANES under the animation tracks. Consequences:
- Placement = clip position; trim = clip edges (clamp: edges can't cross, F36 semantics preserved);
  the 0-sentinel dies completely; frame fields exist only in a selection inspector as numbers.
- Zero-config default: a dropped sound is audible AND exported immediately.
- Daily ops are gestures: move=drag, trim=edge drag, volume=vertical drag/handle, mute + in-export
  = one click each (small icon toggles on the clip, app checkbox language, tooltips = the app's
  words 'Enable playback'/'Export'), delete = hover-revealed x / Del (maps the dimmed-trash call).
- Waveform inside the clip = the direct-manipulation feedback for trim (Q8 engine addition; flag).
- Export-dialog surfaces unchanged; .aemus round-trip + inline errors on clips preserved (R2/R7/Q7).
- REVIEW INSTRUMENTS (in order): 0 = usability (novice task obvious from empty state? top-3 daily
  ops <= 2 gestures?), 1 = design-language consistency (Ae*/app vocabulary), 2 = fresh-eyes
  personas LAST, as input not authority.

## Instrument 0 checklist (usability-first — grade BEFORE anything else)

NOVICE TEST:
- Empty state names the one action ('Drop a sound file here') and nothing competes with it.
- Drop -> plays + exports with ZERO further input; no dialog, no confirm, no required field.
- First-run glance: can a non-user say what this lane is FOR without reading captions?
DAILY-OP GESTURE AUDIT (each op <= gestures): move=1 drag; trim=1 edge drag; volume=1 drag;
mute/hear=1 click; in-export=1 click; delete=2 (hover+click or select+Del); add=1 drop/select.
VISUAL LOAD: <= 5 visible elements per clip at rest (waveform, name, 2 toggles, meter).
Everything failing here is a BLOCKING defect; style/consistency findings rank below.

## Instrument 0 amendment (user zero-context test, 2026-09-30)

The user viewed the render WITHOUT reading any text and could not identify the two icon-only
toggle rectangles or the per-clip meter blips. RULE: every visible element must be nameable by a
stranger with ZERO context and ZERO inference. Elements without an existing learned convention
(NLE/DAW standard) MUST carry words. Icon-only is allowed only for universally-conventional
glyphs (play/trash/x). Corollary: persona/author reviews of a design they understand are
contaminated — the zero-context decode test is the gate's first probe.

## Instrument 0 amendment 2 (pixel-truth rule)

The user found: toggle boxes rendered BLANK (markup missed Phosphor's base 'ph' class) and the
'waveform' rendered as uniform rounded pill-blocks (unreadable as sound). I had reviewed the
INTENT; they reviewed the PIXELS. RULES: (1) reviewers describe only what the pixels show —
name each element as a stranger would see it before naming what it is for; (2) icon fonts must
carry their base class or be inline SVG (headless-render determinism); (3) the agent's DONE
report must include a self-render + self-look ('render checked') with residual defects listed.

## Preview purity rule (user, 2026-09-30)

"The preview is a full HTML page contaminated with text." preview.png must contain ONLY pixels
the product would show — zero documentation text (captions/headings/banners/spec). Mockups ship
a clean-view mode (?clean/#clean) rendering bare UI surfaces; previews and all judgment captures
are made in clean mode. Applies to Phase 3 QML captures too: the app screenshot is the preview.

## Capture split + waveform time-mapping (user, 2026-09-30)

- PREVIEWS ARE SPLIT: one composite canvas defeats focused review. Every design artifact ships
  named clean captures per surface (hero / states / readouts / export / inspector) alongside any
  contact sheet. Applies to Phase 3 QML screenshots likewise.
- REPRESENTATIONAL PIXELS MUST BE TIME-TRUE: the clip waveform's x-axis = the clip's time span,
  flush edge-to-edge; trimming reveals/hides the file slice (in-point semantics). Decorative
  padding that implies silence is a semantic lie in a time-mapped control.

## UI voice rule (user, 2026-09-30)

UI strings REPORT STATE or NAME AN ACTION. They never explain meaning/motivation/consequence
inside the element ("OVER — the exported sound would distort here" is explanation in a control's
clothing). Terse controls (an 'OVER' indicator that latches red); teaching lives in tooltips at
most. Plain language is necessary but not sufficient — plain AND appropriate.

## Zoom craft rule (user zoom reviews, 2026-09-30)

Decode-correct is not enough: spacing rhythm and alignment are where quality lives and they are
only visible at ZOOM. Every design review now includes a 2x craft pass: padding consistency,
vertical centering of representational content, deliberate proportions, nothing crowding edges.

## DECISION (user, 2026-09-30): clip face = OPTION B

Mute icon + readable 'ref' mark on the clip face; export flag, exact frames, reveal, remove all
live in the right-click context menu (menu grammar: [✓ Hear while working] [☐ In exported file]
—sep— First/Last frame —sep— Reveal file —sep— red Remove clip). Option A (two icon toggles) is
the documented alternative, not the design.

## Hash-evidence rule (2026-09-30)

A claimed fix is not a fix without CHANGED OUTPUT. Visual-fix verification = before/after hashes
of the affected renders; byte-identical output after a 'fix' means the fix never reached the
surface (duplicate implementations are the usual cause). Applies to my own reviews equally: a
'renders look better' verdict requires the pixels to have actually changed.

## Componentization rule (user, 2026-09-30)

Mockup markup is single-sourced through real components (React via CDN, no build): <Clip>,
<ClipContextMenu>, <Inspector>, <ReadoutStrip>, <ExportSurfaces>, <Lane>. Component boundaries
map 1:1 onto Phase 3 QML modules (Clip.qml etc.) — the mockup models the port's architecture.
Duplicated instance markup is a defect: it is why per-surface fixes half-landed historically.

## Selection spawns nothing (user, 2026-09-30)

Selection is the most frequent action and must never create UI (no popovers/panels on select).
Selected = accent outline only. Exact numbers: displayed in the context menu rows; editable only
on demand (menu 'Edit frames…' / double-click -> anchored editor, closes on commit). Rare +
precise = explicit access; drag remains the fast path.

## PRODUCT PLAN (user, 2026-09-30): transport column -> timeline HEADER

The playback column on the timeline's right moves INTO a timeline header bar (planned app
evolution). The header becomes the home for the MIX/OVER readout and has spare space for future
items. The design shows this planned state. Corollaries: no invented footer; transport buttons =
the real 8 (PlayBackWidget.cpp:48-55 strings) with the app's real icon assets; ruler ticks follow
real frame intervals; 'faders-horizontal' = Animation settings (not a duplicate fader glyph).

## Composition-verification rule (2026-09-30, regression lesson)

'Non-blank' is not verification. Every capture's DONE report states a COMPOSITION check from the
pixels: vertical/horizontal order of major regions and CONTAINMENT (clips inside lanes, ruler
above tracks, controls inside headers, nothing clipped at edges). The header-rebuild regression
(passed bbox probes with ruler at the bottom and clips outside lanes) is the reference failure.

## Composition check v2 (2026-09-30) — extended after three misses

Per-capture statements must now cover: region order, containment, edge safety, CROSS-COLUMN
ROW ALIGNMENT (shared row boundaries across header/content columns), and SEPARATOR CONTINUITY
(dividers complete across region boundaries). Row heights come from one shared variable.
Header grouping: ONE separator style per group boundary — no bordered well + separator stacking.

## Claim-evidence rule (2026-09-30) — hash-change is not claim-true

The MIX 'well border dropped' report passed the hash gate with the border STILL VISIBLE at 4x zoom.
Rules: (1) every fix claim names the exact changed line(s) AND ships a zoomed crop (4x) proving
the claimed visual effect; (2) I restate a claim only after its named zoomed check — echoes of
unverified reports to the user are my failure mode to kill; (3) 'changed pixels' gates are
necessary, never sufficient.

## MUTE MODEL + LANE LIFECYCLE (user, 2026-09-30)

- Mute = PER TRACK (lane header M / 'Hear while working'); clip context menu gains a per-clip
  'Mute' override. Heard = lane heard AND clip not muted.
- Clip face at rest = filename + waveform + 'ref' (exception) ONLY — no buttons. MUTED clip =
  grayed body + crossed speaker top-right which IS the unmute control (click to unmute). The
  icon exists ONLY while muted.
- Lanes: create via '+' list-footer in the lane-name column AND drop-below-last-lane; delete /
  mute / add via LANE CONTEXT MENU ([✓ Hear while working] —sep— Add sound lane —sep— Delete
  lane (red, confirmed, takes the lane's clips)).

## GENRE PRINCIPLE (user, 2026-09-30): animation timeline, not audio editor

This is an animation timeline in animation software. DAW/audio-editor chrome (full-width drop
bars, big add-lanes affordances, mixer flourish) is FOREIGN. Every element speaks the app's
timeline language: compact columns, small controls, existing conventions.
MUTE = THE APP'S LAYER-VISIBILITY LANGUAGE: hidden layers draw text/caret/buttons at reduced
opacity (ObjectTreeWidget.cpp:322 baseOpacity 0.66; AppStyle.cpp kBranch* 0.4). Muted lane or
clip = EVERYTHING in that scope at reduced opacity — the single visual language for 'off'.
Trim handles: 4px visual, ~8-10px hitbox. Inspector padding symmetric. Lane heights compact
(match app row rhythm; two options to the user).

## CONTEXTUAL UI (user, 2026-09-30) — plain name, no coined jargon

Controls appear in context: no button when unmuted; selection spawns nothing; the OVER mark REPLACES the meter cells
in the same slot when the sum clips (latched, click-to-clear -> meter returns). No persistent
state indicators that can sit dead at rest.

## Decisions 2026-09-30 (supersede earlier)

- HEADER: no 'MIX' word. After the separator the audio section = [speaker (audio toggle)]
  [meter->OVER smart slot]. Speaker anchors the audio group.
- REFERENCE/EXPORT DISTINCTION DROPPED 'for now' (too messy): ref mark, 'In exported file',
  4-combo table, reference/export-only states all die. ONE control: MUTE (track + clip), muted =
  out of the mix entirely (one audio truth). Consequence parked knowingly: the
  animate-to-a-guide-that-never-ships case is unsupported for now.
