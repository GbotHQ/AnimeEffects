# Audio UI redesign — session handoff

Written at the end of a long design session so the next session can continue without archaeology.
Status snapshot date: 2026-10-01. Branch: `feat/audio-ui-redesign` in worktree
`/home/gambot/dev/AnimeEffects-audio`. NEVER touch `/home/gambot/dev/AnimeEffects` (another
agent works there on `feat/export-ui-redesign`).

## Mission and phase gates

Redesign the audio UI and port it to Qt Quick, three phase-gated phases (each needs user
approval before the next):

1. **INVENTORY — DONE, committed `ae1de72`.** `docs/audio-ui-inventory.md`: full catalog with
   file:line traces (controls/flows/states/C++ backing), feature surface + absences, 15
   roughness findings, theme tokens, Q1–Q10 (all answered during the session — answers below).
2. **DESIGN — nearly done.** `tools/audio-ui-mockup/` (HTML + React-CDN mockup + captures).
   Committed through `5f92ddb` (the early grid design); the React/timeline-clip era is
   UNCOMMITTED as of this draft — the handoff commit includes it. Final gate pending: the
   compact-height choice + overall approval (see Open items).
3. **IMPLEMENTATION — not started.** Plan skeleton: `.scratch-orch/phase3-plan-skeleton.md`
   (see Next steps).

## The design as it stands (timeline-clip model)

Sounds are **clips in audio lanes under the animation tracks** of the real "Animation" panel.
Placement = clip position; trim = clip edges (left-edge trim = in-point, video-editor
convention); volume = rubber-band gain line; waveform time-mapped edge-to-edge (peaks from the
file or nothing). One control: **MUTE** (per-track lane-header toggle + per-clip Mute menu item;
muted = out of the mix entirely; muted visuals = the app's layer-visibility language: reduced
opacity of everything in scope). The reference/export distinction (the old playbackEnable ×
exportEnable combos, "ref" marks) was **deliberately dropped "for now"** — too messy; the
guide-track-that-never-ships case is knowingly parked. Contextual UI: chrome appears only when
it has something to say (no mute button when unmuted; selection spawns nothing — exact numbers
open on demand from the clip's context menu; the OVER mark replaces the meter cells in the same
slot when the mix clips). The timeline header (user's product plan) holds the transport column
(moved from the right) + the audio section after the separator: [speaker toggle][meter→OVER
slot]; no "MIX" word anywhere in the UI. The clip's context menu: [☐ Mute] —sep— First/Last
frame + Edit frames… —sep— Reveal file —sep— Remove clip. Lane menu: [☐ Mute] —sep— Add sound
lane / Rename lane… —sep— Delete lane (red). Lane create: '+' in the lane-name column + drop on
an empty lane.

## Binding decisions (chronological where superseding matters)

- Interaction model: **A — sounds live on the timeline** (rejected: settings grid — "shitty for
  new users AND daily users").
- Clip face: minimal; the reference/export distinction dropped 2026-10-01 (supersedes the
  A/B clip-face choice: B was chosen — mute + menu — and is now further simplified to MUTE only).
- Mute: **per track** + per-clip menu override; muted = out of hear AND export (one audio truth,
  user-chosen option B over a metering-only tap). Visual = reduced opacity (match hidden layers:
  `ObjectTreeWidget.cpp:322` baseOpacity 0.66, `AppStyle.cpp` kBranch* 0.4), crossed-speaker
  unmute affordance on muted clips only.
- Trim: in-point semantics left edge (O4); edges clamp; whole-frame snapping only (O1 closed).
- Volume: rubber-band line (Premiere/Resolve/FCP convention); per-clip LED meters removed
  (meters live on the master only).
- Frame numbers: non-primary; shown in the context menu rows; edited only on demand ("Edit
  frames…" / double-click → anchored editor). Selection spawns nothing (no popovers on select).
- Data truth: waveform peaks render only from a readable file (missing = flat body + error
  banner); silence drawn only where the file is silent; no fabricated levels.
- Readout: one smart slot in the header's audio section — meter cells at rest, OVER (red,
  latched, click-to-clear) REPLACES them when the sum clips. No "MIX" label (jargon).
- Header plan (product): transport column moves into the timeline header; audio section after
  the separator (speaker + readout slot); spare space for future items.
- Genre principle: **this is an animation timeline in animation software** — DAW/audio-editor
  chrome is foreign; the app's own conventions govern.
- Contextual UI (user's plain name — don't coin jargon for it).
- Language rules: UI strings report state or name actions (never explain themselves; tooltips
  teach); every element decodable at zero context (words where no learned convention exists);
  app words where they exist ("Enable playback"/"Export" tooltip vocabulary, "First frame"/
  "Last frame", "Edit frames…" ellipsis-action style).
- Layout/implementation rules (the mockup is the implementation template): flow/flex first;
  one spacing scale 4/6/8/12; absolute positioning only where inherently positional; ONE
  styling system (custom classes + token vars for product components; Tailwind for page chrome;
  never mixed); React components map 1:1 to QML modules (Clip, ClipContextMenu, Inspector,
  ReadoutStrip, Lane); icons inline SVG from the app's own assets (no webfont races).
- Preview purity: screenshots show only product pixels — no doc text (clean mode).
- Waveform vertical centering + time mapping: wave x = clip span (in→out); trimming reveals the
  file's slice.

## Process learnings (earned the hard way — respect these)

- **Pixels over prose**: reports are not evidence. Every fix claim names its changed line and
  ships a zoomed crop (verify_*.png); hash-changed ≠ claim-true (the MIX-box lesson: a report
  claimed a border dropped while it was plainly visible at 4x). I restate claims only after
  checking the named crop myself.
- **Composition check v2** per capture: region order, containment, edge safety, CROSS-COLUMN ROW
  ALIGNMENT, SEPARATOR CONTINUITY. "Non-blank" is not verification (a scrambled layout passed
  bbox probes).
- **Zero-context decode test**: a stranger must name every element from the pixels (blank icon
  boxes, "what is this dot", "what even is this" — all caught by the USER, not by reviews).
  Review the pixels, not the intent.
- **User verdicts outrank persona reviews**; persona findings are input, not authority.
- Agent cadence: land small edits and flush early (big composed writes wedge); reminder at 2
  static rounds, interrupt + scripted micro-edits at the stall, stand-down + assembler as
  fallback. Times are wall-clock slow — don't interrupt a turn that is producing edits.
- App reference capture: `.scratch-orch/capture_timeline.sh` (xvfb-run + xcb + generic theme,
  sandboxed HOME, project via CLI so no dialogs can open, poll readiness, kill by PID). Real
  timeline ground truth: `.scratch-orch/capture-home/timeline_ref.png` + `timeline_detail.png`.

## File map

- `docs/audio-ui-inventory.md` — Phase 1 deliverable (committed).
- `tools/audio-ui-mockup/index.html` — the design (React-CDN, self-contained). `render-notes.md`
  — capture commands + surface map. Captures: `preview.png` (contact sheet), `01-hero-timeline`,
  `02-states`, `03-readouts`, `04-export-surfaces`, `05-inspector`, `capture_clipface-a/b`,
  `capture_contextmenu`, `capture_lanemenu`, `verify_*_zoom.png` (claim-evidence crops).
- `.scratch-orch/phase2-designer-brief.md` — the decision record (this session's spine).
- `.scratch-orch/phase3-plan-skeleton.md` — Phase 3 plan + architecture question.
- `.scratch-orch/capture_timeline*.sh`, `capture-home/` — app-reference capture tooling + ground truth.

## Open items at handoff

1. **Compact lane height + track-text font** — user is unsure (84px too tall; vertical space
   precious). Two options were to be rendered side by side (app-rhythm vs slightly taller) for
   the user to choose. THE APP'S REAL METRICS need measuring (TimeLineWidget.cpp / theme/TimeLine.h).
2. **Final Phase 2 gate**: overall design approval after the last batch lands and is verified.
3. Parked knowingly: the guide-track case (audible while animating, never exported) — dropped
   with the reference/export distinction "for now".
4. O6 (from the design's spec notes): gapped-selection audio follows video (per-range concat) —
   binding, flagged.

## Phase 3 preview (from the plan skeleton)

- The port replaces the AudioPlaybackWidget facade + UIState grid entirely; `audioConfig`
  survives as clip data → `AudioMixer::Track::fromSettings` (tests pin this seam).
- Architecture decision to make FIRST: how lanes/clips live with the WIDGET timeline
  (TimeLineWidget/TimeLineEditor are widget code): (a) custom-paint in the widget timeline,
  (b) QQuickWidget strip sharing scroll/zoom/playhead state, (c) QML overlay. Likely (b).
- Engine additions (user-sanctioned, Q8): waveform peak rendering per clip, level taps
  (`QAudioBufferOutput`, Qt 6.8+; verify Qt ≥6.8 at first build), the real-time mix bus
  (one audio truth: preview plays the export sum — this replaces the old split by construction),
  OVER latch from the true sum.
- R8 fix: `.aemus` parsing moves out of GUI code into media/core; schema generalizes to
  clips-per-lane with in-points (sketch in the mockup's spec notes).
- Tests: headless QML tests per `tests/picker_keyboard.cpp`; key routing = in-process
  QKeyEvent injection; never `scripts/gui/run_probes.sh` (other session's gates); locale:
  never hand-edit `assets/locale/*.ts`.
