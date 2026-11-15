# Phase 3 plan skeleton — orchestrator scratch (design-neutral; fill after Phase 2 gate)

Principle: design informs implementation, never the reverse. This skeleton records only
process/architecture facts that hold regardless of the mockup's look. The component mapping
and behavior spec come from the APPROVED mockup at the Phase 2 gate.

## Architecture invariants (settled now)

- Qt Quick port; the export-dialog project's patterns are the code-level reference
  (src/gui/qml/*.qml controls, ExportViewModel context-property style).
- The port replaces the AudioPlaybackWidget FACADE (it is not a QWidget; src/gui/AudioPlaybackWidget.h:71)
  and its UIState rows — not the engine seam. media::AudioMixer stays the export mixer;
  QMediaPlayer/QAudioOutput stay the preview players unless the approved design's Q1
  audition affordance requires engine additions (Q8 allows them).
- Model shape to re-shape toward QML: audioConfig / mediaState / UIState
  (src/gui/AudioPlaybackWidget.h:29-69) — the redesign's Q2/Q5/Q9/Q10 answers change
  these semantics (independent enable flags, 0="to end", remove-own-row, timeline placement).
- core::Project must stop depending on GUI code (R8): move .aemus deserialize out of
  AudioPlaybackWidget into media/core so tests stop stubbing GUI symbols.

## Process rules (from AGENTS.md + brief)

- Subagents implement; I plan/review/own architecture. Design questions that arise during
  implementation go back to the user (ask_user_question), never resolved by toolkit convention.
- Locale: never hand-edit assets/locale/*.ts — lupdate regenerates, on our branch only.
- Tests: headless QML tests in tests/ (QT_QPA_PLATFORM=offscreen), following tests/picker_keyboard.cpp
  (loads real .qml, asserts on model; assert preconditions inside the test). Key-routing
  questions = in-process QKeyEvent injection, not xdotool.
- App runs (only when unavoidable): unset WAYLAND_DISPLAY; QT_QPA_PLATFORM=xcb;
  QT_QPA_PLATFORMTHEME=generic (or xvfb-run); portal canary before any file dialog;
  ae_guard_display before raw xdotool; NEVER scripts/gui/run_probes.sh (other session's gates).
- Isolation: all work in /home/gambot/dev/AnimeEffects-audio on feat/audio-ui-redesign.

## Work breakdown (to be scoped against the approved mockup)

1. Data model + core seam (audioConfig semantics per Q2/Q5/Q10; R8 layering fix; R2 round-trip).
2. QML surface (docked panel per Q3; track rows; states incl. per-row error Q7; audition Q1).
3. Transport/MainWindow integration (replace speaker-button window per Q3).
4. Export-dialog surface alignment (checkbox/facts semantics unchanged behaviorally — verify).
5. Tests: model/mixer mapping (tests/media_audio_mixer.cpp pattern), QML interaction tests,
   key routing if the design specifies custom keys.
6. Review gates: I review each PR-sized chunk against the mockup + answers; probes deliberately
   NOT run (forbidden); verification via headless tests + targeted reads.

## Meter engine path (verified against Qt docs 2026-09-30)

- Per-track levels + MIX sum: QAudioBufferOutput (Qt 6.8+) per track QMediaPlayer —
  audioBufferReceived(QAudioBuffer) gives decoded buffers; peak/RMS from F32/S16 data.
- MIX meter = sum over EXPORT-ENABLED tracks' taps (the binding design's semantics);
  latching CLIP = that sum crossing ±1.0 (same condition as AudioMixer.cpp:93-98 clamp).
- App builds against Qt 6.11-era (export-dialog notes cite 6.11 sources) — 6.8+ API OK.

## Model A implications (decision 2026-09-30: sounds = timeline clips)

- The audio UI surface is now: timeline audio lanes + clip interactions (drag/trim/volume/
  2 icon toggles/hover x) + selection inspector popover + passive MIX/CLIP readout + the
  existing export-dialog surfaces. The AudioPlaybackWidget facade + UIState grid die entirely;
  audioConfig survives as the clip's data (path, start, end, volume, playbackEnable,
  exportEnable) mapped 1:1 onto AudioMixer::Track::fromSettings (the seam tests pin).
- HARD ARCHITECTURE QUESTION (mine to settle at Phase 3 planning): the timeline is widget code
  (TimeLineWidget/TimeLineEditor, custom paint + mouse). Candidate strategies for lanes/clips:
  (a) render lanes inside the widget timeline (custom paint + event handling — matches existing
  timeline interaction code; no Quick), (b) hybrid: QQuickWidget strip for the lanes region
  sharing the timeline's scroll/zoom/playhead state, (c) lanes as QML overlaid on the widget
  timeline. The 'port to Qt Quick' directive covers the AUDIO UI; (a) may conflict with it,
  (b) is the likely middle. Decide from the approved mockup's interaction needs, then record.
- Preview/export seam: clip position/trim replaces the frame fields; aPlayer/syncTracks logic
  becomes clip-driven (window = clip extent). The R15 semantics fix (timeline placement) is the
  clip model by construction — preview and export both derive from the same clip extents.
- Engine additions sanctioned (Q8): waveform rendering per clip, per-track level taps
  (QAudioBufferOutput path documented above), MIX sum + latching CLIP.
