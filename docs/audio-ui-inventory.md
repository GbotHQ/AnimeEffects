# Audio UI inventory — Phase 1 of the audio UI redesign

The current audio UI, catalogued from code with every claim traced to `file:line` (paths relative
to the worktree root). This is input for the design phase: a designer should not need to re-read
the code. Style follows `docs/export-dialog-behavior.md`: **[D]** a design the code deliberately
specifies (its comments state the decision), **[R]** a finding with its rationale/evidence, **[Q]**
a question only the user can settle. No app was run; everything here is **[read]** from source.

Sources: `src/gui/AudioPlaybackWidget.{h,cpp}`, `src/media/AudioMixer.{h,cpp}`,
`src/media/VideoEncoder.{h,cpp}`, `src/media/FFmpegHelpers.{h,cpp}`, `src/gui/PlayBackWidget.{h,cpp}`,
`src/gui/TimeLineWidget.cpp`, `src/gui/TargetWidget.cpp`, `src/gui/MainWindow.cpp`,
`src/core/Project.{h,cpp}`, `src/ctrl/{Exporter,ExportTarget,ExportParams}.{h,cpp}`,
`src/gui/qml/{ExportDialog.qml,AeFactRow.qml,ExportViewModel.{h,cpp},QmlTheme.{h,cpp}}`,
`src/gui/ExportFacts.{h,cpp}`, `tests/{media_audio_mixer,media_video_encoder,media_exporter_e2e,media_export_target,export_form}.cpp`,
`src/gui/theme/{Colors.h,TailwindPalette.h,Icons.cpp}`, `tools/dock_mockup/index.html`.

## Overview

Audio lives in **two disconnected halves**:

1. **Live playback while animating** — a floating "Audio Player" window of per-track rows, driven
   by the animation transport and the playback clock. Backend: one `QMediaPlayer` +
   `QAudioOutput` per track (Qt Multimedia), kept in `src/gui/AudioPlaybackWidget.*`.
2. **Export mixing** — an offline, in-process libav mixer that sums enabled tracks into one float
   buffer and muxes it as a single audio stream. Backend: `media::AudioMixer` + `media::VideoEncoder`.

They share only the `audioConfig` data (a vector of track settings on `core::Project`,
`src/core/Project.h:110-114`) and disagree in one important reading of that data (see R1).
There is no transport-level audio UI beyond one speaker button, no meters, no waveform, no
timeline audio lanes, and no audio preferences anywhere in the settings dialogs (searched
`audio|sound|volume|mute|mixer|playback|sample|media` across `src/`, `tests/`, `tools/`).

Surfaces, at a glance:

| Surface | Where | What |
| --- | --- | --- |
| Audio Player window | `src/gui/AudioPlaybackWidget.{h,cpp}` | per-track rows (file, frame window, volume, playback/export switches) + Save/Load config tab |
| Transport speaker button | `src/gui/PlayBackWidget.cpp:55` | opens/closes the Audio Player window |
| Animation transport | `src/gui/PlayBackWidget.cpp:47-55` | play/pause/step/loop — the clock that drives audio (not itself audio UI) |
| Export dialog "Export with audio" checkbox | `src/gui/qml/ExportDialog.qml:490-497` | one capability checkbox under Advanced |
| Export facts "Audio:" row | `src/gui/qml/ExportViewModel.cpp:373-386`, `src/gui/ExportFacts.cpp:47-67` | stated yes/no + the reason detail |
| Project sidecar `.aemus` | `src/core/Project.cpp:56-69` (load), `src/gui/AudioPlaybackWidget.cpp:71-95` (save) | JSON persistence of the track list |

## Catalog

### A. Audio Player window — controls

Window shell (`AudioPlaybackWidget::setupUi`, `src/gui/AudioPlaybackWidget.h:116-177`): a plain
(non-`QObject`) builder class (`src/gui/AudioPlaybackWidget.h:71`) that assembles a `QWidget`
window titled "Audio Player" (`:166`) with a two-tab `QTabWidget`: "Audio player" (`:167`) and
"Save/Load audio config" (`:170`).

Per-track row — one `UIState` (`src/gui/AudioPlaybackWidget.h:48-69`) per track, four grid rows
built in `addUIState` (`src/gui/AudioPlaybackWidget.cpp:273-497`; row math `:277-282`; column
comment `:286-289`):

| # | Control | Created | Label / range | Reads / writes (backing) |
| --- | --- | --- | --- | --- |
| 1 | `addNewTrack` QToolButton (dual-mode) | `src/gui/AudioPlaybackWidget.h:49`, placed `src/gui/AudioPlaybackWidget.cpp:309` | last row "Add new audio track" (`:361`), earlier rows "Remove next audio track" (`:418`) | appends a default `audioConfig` + player (`:448-455`) or erases the NEXT config + full UI rebuild (`:456-471`); mode flag `UIState::addTrack` (`src/gui/AudioPlaybackWidget.h:68`), invariant stated at `src/gui/AudioPlaybackWidget.cpp:399-415` |
| 2 | `selectMusButton` QToolButton | `src/gui/AudioPlaybackWidget.h:50`, placed `:346-348` | file name truncated to 17 chars + full name as tooltip (`src/gui/AudioPlaybackWidget.cpp:374-381`); unconfigured: "Select audio file..." (`:383`) | opens file dialog (`.mp3 .mp4 .wav .ogg .flac`, `:425-431`); writes `audioConfig.audioPath`/`audioName` (`:433-434`) and re-binds the player (`modifyTrack`, `:435`) |
| 3 | `playAudio` QCheckBox | `src/gui/AudioPlaybackWidget.h:51`, placed `:284-285` | "Enable playback" (`:350`) | `audioConfig.playbackEnable` (`:489-491`; field at `src/gui/AudioPlaybackWidget.h:36`) |
| 4 | `exportAudio` QCheckBox | `src/gui/AudioPlaybackWidget.h:55`, placed `:290-291` | "Export" (`:354`) + tooltip explaining reference-only tracks (`:355-360`) | `audioConfig.exportEnable` (`:483-488`; field at `src/gui/AudioPlaybackWidget.h:37`, separation rationale `:32-35`) |
| 5 | `startSpinBox` QSpinBox | `src/gui/AudioPlaybackWidget.h:57`, placed `:297-301` | 0..INT32_MAX (`:298-300`), column header "Playback start frame" (`:362`) | `audioConfig.startFrame` (`:473-474`) + recomputes duration label (`:475-476`) |
| 6 | `endSpinBox` QSpinBox | `src/gui/AudioPlaybackWidget.h:56`, placed `:292-296` | 0..INT32_MAX (`:293-295`), header "Playback end frame" (`:363`) | `audioConfig.endFrame` (`:478-479`) + duration label (`:480-481`) |
| 7 | `volumeIcon` QLabel (speaker glyph) | `src/gui/AudioPlaybackWidget.h:62`, placed `:320-325` | `speaker-high.svg` at 16 px, DPR-scaled (`src/gui/AudioPlaybackWidget.cpp:18-27`) | display only; re-tinted on theme change (`:265-271`) |
| 8 | `volumeSlider` (`gui::tool::ToolSlider`) | `src/gui/AudioPlaybackWidget.h:66` (rationale `:63-65`), placed `:326-332` | horizontal, 0..100 (`:327-329`) | `audioConfig.volume` (`:492-493`), live-applies to the player (`:494`, `modifyTrack` `:519-533`), updates the percentage label (`:495`) |
| 9 | `volumeLabel` QLabel | `src/gui/AudioPlaybackWidget.h:61`, placed `:333-336` | "N%" (`:366`) | display only |
| 10 | `musDurationLabel` QLabel | `src/gui/AudioPlaybackWidget.h:60`, placed `:342-344` | "Duration (in frames): end−start" (`:364-365`) | display only; refreshed by both spinboxes (`:475-476`, `:480-481`) |
| 11 | `startLabel` / `endLabel` QLabels | `src/gui/AudioPlaybackWidget.h:58-59`, placed `:310-319` | column headers (`:362-363`) | display only |
| 12 | `line` QFrame separator | `src/gui/AudioPlaybackWidget.h:67`, placed `:337-341` | HLine spanning all 5 columns | display only |

Config tab — the 2 config-tab buttons (`src/gui/AudioPlaybackWidget.h:145-163`); the full surface is **13 per-track elements + 2 config-tab buttons** (the per-track table's 12 rows hold 13 elements — row 11 is two labels):

| # | Control | Label | Backing |
| --- | --- | --- | --- |
| C1 | `saveConfigButton` QPushButton | "Save current audio configuration" (`src/gui/AudioPlaybackWidget.h:168`) | `QFileDialog::getSaveFileName`, `.aemus` extension (`src/gui/AudioPlaybackWidget.cpp:162-172`) → `serialize` (`:71-95`) |
| C2 | `loadConfigButton` QPushButton | "Load audio configuration from file" (`:169`) | `QFileDialog::getOpenFileName` (`:174-186`) → JSON parse (`:186-194`) → overwrite confirm (`:195-207`) → teardown + `deserialize` + rebuild (`:208-235`) |

Modal dialogs raised from these flows (all `QMessageBox`/`QFileDialog`, parented to the window):
save/load file dialogs (`src/gui/AudioPlaybackWidget.cpp:163-167`, `:175-179`), a file-open error
(`:182-184`), a JSON-parse warning (`:187-194`), an overwrite confirmation defaulting to OK
(`:196-206`), a deserialize-failure warning (`:222-224`), and a success notice (`:233`). Every
Qt connection is wrapped in `checkConnection`, which shows a "Qt connection error" box on failure
(`:10-14`).

### B. Flows (each with its trigger and code path)

1. **Open/close the Audio Player window.** Transport button 8, `speaker-high`, tooltip "Audio
   track" (`src/gui/PlayBackWidget.cpp:55`); the window is a top-level `Qt::Window` owned by the
   transport (`src/gui/PlayBackWidget.h:48`); handler `src/gui/PlayBackWidget.cpp:99-122`: lazy
   re-create if destroyed (`:100-104`), adopt a freshly loaded project's config (`:106-110`),
   refuse re-open if already visible (`:111-113`), **stop every player on open** (`:114`), rebuild
   rows after project load (`:116-119`), `show()` (`:121`).
2. **Select a track file.** Control 2 → dialog → `audioPath`/`audioName` write + `modifyTrack`
   (`src/gui/AudioPlaybackWidget.cpp:425-444`); `modifyTrack` stops a playing track, re-sets the
   source and volume (`:519-533`).
3. **Add a track.** Last-row button click → append default `audioConfig` (`audioName` =
   "Placeholder", `endFrame` 500; `src/gui/AudioPlaybackWidget.h:29-41`), create a player pair,
   append a UI row (`src/gui/AudioPlaybackWidget.cpp:448-455`); the clicked button flips to
   "Remove next audio track" (`:453-454`).
4. **Remove a track.** Non-last-row button click erases the **NEXT** config entry and rebuilds the
   whole UI (`src/gui/AudioPlaybackWidget.cpp:456-471`; the add/remove invariant is documented at
   `:399-415`). A size-mismatch guard first forces a full rebuild (`:446`).
5. **Edit the frame window.** Spinboxes write `startFrame`/`endFrame` and refresh the duration
   label (`src/gui/AudioPlaybackWidget.cpp:473-482`). No cross-clamping (see R11).
6. **Edit volume.** Slider writes `volume`, calls `modifyTrack` (live `QAudioOutput::setVolume`,
   `src/gui/AudioPlaybackWidget.cpp:530-532`), updates the % label (`:492-496`). Gain conversion
   is linear `volume/100` (`src/gui/AudioPlaybackWidget.h:98`).
7. **Play.** Transport play (`src/gui/PlayBackWidget.cpp:75-81`, `PlayPause` `:136-153`) →
   `TargetWidget::onPlayBackButtonPushed` (`src/gui/TargetWidget.cpp:80-82`) →
   `TimeLineWidget::setPlayBackActivity(true, …)` (`src/gui/TimeLineWidget.cpp:199-211`) →
   `AudioPlaybackWidget::aPlayer(…, play=true, …)` (`:209`; impl
   `src/gui/AudioPlaybackWidget.cpp:124-159`): lazily allocates one `QMediaPlayer`+`QAudioOutput`
   per config (`:128-133`), binds output/source/volume (`:138-142`), seeks (`:143`), then plays a
   track iff its window contains the playhead and playback is enabled (`:153-156`).
8. **Pause / stop.** `setPlayBackActivity(false, …)` → `aPlayer(…, play=false, …)` stops every
   player and clears `mediaState::playing` (`src/gui/TimeLineWidget.cpp:212-221`); the transport's
   `pushPauseButton` is the programmatic variant (`src/gui/PlayBackWidget.cpp:173-180`), also used
   on project switch (`src/gui/TargetWidget.cpp:48-54`).
9. **Per-frame sync (the playback clock).** `TimeLineWidget::onFrameUpdated` → `syncAudioTracks`
   (wiring `src/gui/TimeLineWidget.cpp:184-185`, impl `:548-552`) → `AudioPlaybackWidget::syncTracks`
   (`src/gui/AudioPlaybackWidget.cpp:37-69`): while `mediaState::playing` (`:38`), for each track
   with a backing player (`:41-44`): a jump of more than one frame re-seeks (`:48-50`), normal
   stepping lets the track run on its own clock; a playing track stops when the animator is
   suspended or the playhead leaves `[startFrame, endFrame)` or playback is disabled (`:51-53`); a
   stopped track starts inside the window unless the media has already ended (`:55-65`, the
   no-restart guard `:57-64`). Seek itself tolerates sub-frame drift below one frame to avoid
   audible stutter (`correctTrackPos`, `:535-544`; target position `trackPositionMs`, `:30-33`).
10. **Save config.** `serialize` writes JSON `{Tracks: N, "0".."N-1": {Name, Path, Playback,
    Export, Volume, StartFrame, EndFrame}}` (`src/gui/AudioPlaybackWidget.cpp:71-95`) after
    filtering through `getValidAudioStreams` (`:73`; filter at
    `src/gui/AudioPlaybackWidget.h:99-109`) — see R2.
11. **Load config.** `deserialize` (`src/gui/AudioPlaybackWidget.cpp:97-122`) tolerates old files
    with no Export key (defaults true, `:106-109`) and swallows all exceptions with a `TODO`
    (`:119-120`); the caller destroys all players (`:209-212`), restores a backup on failure
    (`:214-225`) or rebuilds from the loaded config (`:226-234`), then `rectifyUI` (`:235`).
12. **Project open.** `core::Project` auto-loads a `.aemus` sidecar next to the `.anie`
    (`src/core/Project.cpp:56-69`; path built by string surgery on the project path `:57`) and sets
    the `mediaRefresh`/`uiRefresh` handoff flags (`:61-62`, declared
    `src/core/Project.h:113-114`).
13. **Project close / tab switch.** A closed ACTIVE project hands its `mediaState`/`pConf` copies to
    the next tab (`src/gui/MainWindow.cpp:750-772`); the `Project` destructor stops any playing
    players (`src/core/Project.cpp:88-97`).
14. **Export.** The export dialog's audio checkbox (see §C) becomes `GeneralParams::exportWithAudio`
    (`src/ctrl/ExportParams.h:84`; read back at `src/gui/MainWindow.cpp:2066`,
    `src/gui/qml/ExportViewModel.cpp:1267`). At export time the track list is filtered through
    `getValidAudioStreams` (`src/gui/MainWindow.cpp:1750-1751`, `:1847-1848`, `:2116-2117`),
    counted per `exportEnable` (`:1372-1376`) and translated to `media::AudioMixer::Track`s
    (`toAudioTracks`, `:1221-1233`). `translateVideoExport` empties the track list unless
    `exportWithAudio` (`:1255`). The plan derives audio presence (never a request —
    `src/ctrl/ExportTarget.h:282-300`) and MainWindow copies plan audio params (`:1596-1600`) and
    calls `exporter.setAudioTracks` (`:1602`; `src/ctrl/Exporter.cpp:88-94`). At finish the
    exporter mixes **per selected range** and concatenates, so a gapped selection's audio is the
    selected frames' sound back to back (`src/ctrl/Exporter.cpp:585-638`, rationale `:588-593`),
    then `VideoEncoder::setAudio` muxes it on `close()` (`src/media/VideoEncoder.h:96-101`).
15. **Theme change.** `GUIResources::onThemeChanged` → `AudioPlaybackWidget::onThemeUpdated`
    re-fetches the runtime-tinted speaker glyph (`src/gui/PlayBackWidget.cpp:66-68`,
    `src/gui/AudioPlaybackWidget.cpp:265-271`).

### C. States

- **Track unconfigured** — `audioName == "Placeholder"` (default, `src/gui/AudioPlaybackWidget.h:30`):
  the selector reads "Select audio file..." (`src/gui/AudioPlaybackWidget.cpp:382-384`). The same
  sentinel doubles as "config is empty, no overwrite warning needed" (`:195`) — a name is being
  used as a state flag.
- **Track configured** — truncated name + tooltip (`:374-381`).
- **Row mode** — last row = add, earlier rows = remove-next (`:416-421`); the flag lives in two
  copies of `UIState` (the lambda's captured copy mutates at `:454`, the vector's copy at `:470`).
- **Playback** — `mediaState::playing` (`src/gui/AudioPlaybackWidget.h:43`) plus per-player
  `QMediaPlayer::playbackState()` (`src/gui/AudioPlaybackWidget.cpp:45`, `:148`, `:526`).
- **Loading/error** — there is no loading state. Errors are modal boxes (see §A) at load/save time
  only; at playback a missing file simply produces silence (`QMediaPlayer` errors are not
  connected to anything — no `errorOccurred` connection exists); at export a bad track is skipped
  and only counted in `AudioMixer::error()` (`src/media/AudioMixer.cpp:72-75`, `:87-92`).
- **Validity** — the only filter is `getValidAudioStreams` (`src/gui/AudioPlaybackWidget.h:99-109`):
  swaps a reversed window (`:102`), requires `playbackEnable` (`:103`), a non-empty window (`:103`),
  and an existing readable file (`:104`).
- **Export-dialog audio checkbox** — visible/enabled iff the artifact can carry audio (not GIF, not
  a sequence, `containerCarriesAudio`); value stashed across artifacts and restored
  (`src/gui/qml/ExportViewModel.cpp:571-580`, stash `:554-558`); frozen to the plan's answer under
  a destination (`:985`); re-enabled in manual mode (`:1069`).

### D. Signal/slot connections (complete list)

All UI connections, each wrapped in `checkConnection` (`src/gui/AudioPlaybackWidget.cpp:10-14`):

| Signal | Slot (lambda) | Effect | Where |
| --- | --- | --- | --- |
| `selectMusButton::clicked` | file dialog → config write | controls 2 | `src/gui/AudioPlaybackWidget.cpp:425-444` |
| `addNewTrack::clicked` | add or remove-next | control 1 | `:445-472` |
| `startSpinBox::valueChanged` | `startFrame` + label | control 5 | `:473-477` |
| `endSpinBox::valueChanged` | `endFrame` + label | control 6 | `:478-482` |
| `exportAudio::checkStateChanged` | `exportEnable` | control 4 | `:483-488` |
| `playAudio::checkStateChanged` | `playbackEnable` | control 3 | `:489-491` |
| `volumeSlider::valueChanged` | `volume` + `modifyTrack` + label | control 8 | `:492-496` |
| `saveConfigButton::clicked` | save dialog + `serialize` | control 13 | `:162-173` |
| `loadConfigButton::clicked` | load dialog + `deserialize` + rebuild | control 14 | `:174-237` |
| `GUIResources::onThemeChanged` | `AudioPlaybackWidget::onThemeUpdated` | speaker glyph | `src/gui/PlayBackWidget.cpp:68` |
| `QMetaObject::connectSlotsByName(audioWidget)` | (no matching slots exist) | setup boilerplate | `src/gui/AudioPlaybackWidget.h:172` |

Playback driving is **not** signal/slot: the transport's `PushDelegate` (`src/gui/PlayBackWidget.cpp:71-123`)
calls into `TargetWidget::onPlayBackButtonPushed` (`src/gui/TargetWidget.cpp:77-105`), and the
timeline frame updater calls `syncAudioTracks` via `util::Signaler`
(`src/gui/TimeLineWidget.cpp:184-185`).

### E. Export-dialog audio surfaces (QML)

- **Checkbox** "Export with audio": `src/gui/qml/ExportDialog.qml:490-497`, inside the Advanced
  disclosure (`:459-472`), third capability after transparency and loop (`:474-489`). Backing:
  `ExportCapabilityRow mAudio` (`src/gui/qml/ExportViewModel.h:588`, exposed `:216`, accessor
  `:360`), label set in `retranslateUi` (`src/gui/qml/ExportViewModel.cpp:1237`). Its
  visible/enabled/value rules are in §C.
- **Facts row** "Audio:": built at `src/gui/qml/ExportViewModel.cpp:373-386` (manual mode) and by
  `exportFacts`/`audioReason` (`src/gui/ExportFacts.cpp:47-67`) for destination mode; the detail is
  one of "not accepted by this destination" / "none in this project" / "off in this dialog" /
  empty (`src/gui/ExportFacts.cpp:57-67`). Rendered by `AeFactRow` — muted 104 px label column +
  wrapping value; detail joins with " · " (`src/gui/qml/AeFactRow.qml:58`); severity colors are
  `theme.danger`/`theme.warning`/`theme.text` (`src/gui/qml/AeFactRow.qml:58-61`).
- **Request wiring**: checkbox → `buildRequest().mute = !mAudio.value()`
  (`src/gui/qml/ExportViewModel.cpp:1090-1091`) → `planExport` derives `audioEnabled`
  (`src/ctrl/ExportTarget.cpp:1029-1032`) → read-back `exportWithAudio`
  (`src/gui/qml/ExportViewModel.cpp:1267`) → `src/gui/MainWindow.cpp:2066`, `:1377`. The
  remembered preference is `export_allow_audio` in QSettings (`src/gui/MainWindow.cpp:1963`,
  `:1998`).

### F. Out of scope / not audio UI (rejected explicitly)

- **Animation transport proper** (skip-back/step/play/step/skip/loop/faders buttons,
  `src/gui/PlayBackWidget.cpp:47-54`) — animation playback UI. Included above only where it hosts
  or drives audio (the speaker button; the playback clock). The loop button is the animation loop
  (`src/gui/PlayBackWidget.cpp:87-93`).
- **Timeline temporary playback window / ruler** (`src/ctrl/TimeLineEditor.h:55-124`) — frame-range
  playback, not audio.
- **Destination audio rules text** (`src/ctrl/ExportTargetStrings.h`, e.g. "without sound" `:55`)
  — data strings behind the export facts; noted only as the export-side context.
- **`AudioSpec`/plan/encoder internals** — listed in §G (features), not catalogued as UI.
- False positives rejected: `QMutex` matches ("mute"), `src/deps/zip_file.h:5128` ("volume", zip
  archives), `AnimationSettingDialog.qml` `textMuted` (theming only).
- `tools/verify_blur/verify_blur.cpp:114-116` and several `tests/` stub
  `AudioPlaybackWidget::deserialize` to link `core::Project` without the GUI — test plumbing, kept
  as evidence for R8.

## G. Audio features available to expose

### G1. Offline mixing — `media::AudioMixer` (export half)

- `Track` (`src/media/AudioMixer.h:18-43`): `path` :19 (any libav-demuxable file), `startFrame` :20 (negatives clamp to 0), `endFrame` :21 (0 = to end of export range), `volume` :22 (linear 0..1 gain; "the UI owns its own percentage curve" :32), `enabled` :23. `Track::fromSettings(path, volume, start, end, exportEnabled)` :33-42 is the sanctioned editor→mixer mapping; its comment :25-32 records the past defect where the export switch was hardcoded true (reference-only tracks leaked into exports).
- `Config` :45-50: `fps` :46 (timebase), `totalFrames` :47 (tracks clamp to this), `sampleRate` :48 (default 48000), `channels` :49 (default 2).
- `mix(tracks, config)` :57-60: one-shot; returns false + `error()` on invalid fields or a fatal decode; a track that fails to decode is skipped best-effort (:58-59).
- Buffer queries :62-67: `samples()` interleaved float (ownership stays with the mixer), `sampleCount()` per channel, `channels()`, `sampleRate()`, `isSilent()`.
- `frameToSamples(frame, fps, sampleRate)` :71-72 — project-frame ↔ output-sample mapping.
- Decode internals (`src/media/AudioMixer.cpp:9-14`): avformat + avcodec + swresample; every track resampled to a common rate/channel layout (s16→float per `tests/media_audio_mixer.cpp:3-8`).
- Post-mix HARD per-sample clamp of the summed buffer to [-1.0, +1.0] (`src/media/AudioMixer.cpp:93-98`, comment :93-94) — overlapping loud tracks clip.
- GL-free and QCoreApplication-free (`AudioMixer.h:13`) — headless-testable (`tests/media_audio_mixer.cpp`).

### G2. Live preview — per-track `QMediaPlayer` + `QAudioOutput` (playback half)

- One `QMediaPlayer`+`QAudioOutput` pair per track, allocated lazily in `aPlayer` (`src/gui/AudioPlaybackWidget.cpp:128-133`; second allocation site `:500-501`), owned by `mediaState` (`src/gui/AudioPlaybackWidget.h:42-46`).
- `aPlayer(pConf, play, state, fps, curFrame)` (impl `:124-159`): binds output/source/volume (`:138-142`), seeks (`:143`), then plays only tracks whose `[startFrame, endFrame)` window contains the playhead and whose playback is enabled (`:153-156`).
- Volume: linear percent/100 (`getVol`, `AudioPlaybackWidget.h:98`) → live `QAudioOutput::setVolume` (`:530-532`).
- Clock-following: `syncTracks` (impl `:37-69`) — >1-frame jumps re-seek (`:48-50`); stop when playhead leaves the window (`:51-53`); restart-inside-window with an already-ended guard (`:55-65`); sub-frame drift tolerance to avoid audible stutter (`correctTrackPos` `:535-544`, `trackPositionMs` `:30-33`).
- Qt Multimedia offers position/duration/state/error signals; the current UI consumes almost none of it (duration label only). That unused capacity is available to the redesign without engine changes.

### G3. Muxing — `media::VideoEncoder` + `media::FFmpegHelpers` (export container)

- Optional audio stream muxed alongside video in one pass (`src/media/VideoEncoder.h:78-83`): `audioCodec` ("aac" for mp4/mov/mkv, "libopus" for webm), `audioChannels`, `audioSampleRate`, `audioBitrate` (default 128000).
- `setAudio` accepts the pre-mixed interleaved float buffer (from AudioMixer) and muxes it as ONE stream on `close()` (`VideoEncoder.h:96-101`); `openAudio`/`writeAudio` :130-131.
- `containerCarriesAudio(container)` (`src/media/FFmpegHelpers.h:109-113`) — which containers can carry audio at all; comment :109-112 records the past defect where "export with audio" was a checkbox that silently did nothing for an unsupported container.
- Plan-side: audio presence is DERIVED, never requested (`src/ctrl/ExportTarget.h:282-300`); the exporter mixes per selected range and concatenates (`src/ctrl/Exporter.cpp:585-638`, rationale :588-593).

### G4. Explicit ABSENCES — the design must not assume these

- No level meters / peak / loudness readout anywhere (nothing reads the sample buffer for display).
- No streaming or incremental mixing — `mix()` is one-shot into RAM; no progress, no partial results.
- No pan, no fades/ducking, no EQ (Track fields, `AudioMixer.h:18-23`).
- No waveform or file-overview rendering.
- No within-track scrubbing control (only frame spinboxes for the window).
- No limiter/normalization/headroom handling — summed tracks hard-clip at ±1.0 (`src/media/AudioMixer.cpp:93-98`).
- No sample-accurate preview of the EXPORT mix — preview is N independent QMediaPlayers (G2); export is one summed buffer (G1). See Q1.
- No audio preferences in any settings dialog; no timeline audio lanes (§Overview).

## Roughness findings

- **R1 [R] — The two halves disagree on enable-flag semantics.** `audioConfig` separates `playbackEnable`/`exportEnable` on purpose (reference tracks; `src/gui/AudioPlaybackWidget.h:32-37`), but the shared validity filter `getValidAudioStreams` requires `playbackEnable` (`src/gui/AudioPlaybackWidget.h:99-109`, condition :103) and is what the EXPORT path filters through (`src/gui/MainWindow.cpp:1750-1751`, `:1847-1848`, `:2116-2117`) — while counting by `exportEnable` (`:1372-1376`) and mapping the export flag into the mixer (`toAudioTracks` `:1221-1233`, `fromSettings` `AudioMixer.h:33-42`). Net effect: an export-only track (playback off, export on) is silently dropped from the export. The past hardcoded-true variant of this class of bug is recorded at `AudioMixer.h:25-32`.
- **R2 [R] — Save is lossy.** `serialize` filters through `getValidAudioStreams` before writing (`src/gui/AudioPlaybackWidget.cpp:73`; JSON shape `:71-95`), so playback-disabled, unconfigured, or file-missing tracks are silently absent from the `.aemus` and gone after reload. Load cannot round-trip what save dropped.
- **R3 [R] — Copy-paste objectName bug.** `addNewTrack->setObjectName("startLabel")` (`src/gui/AudioPlaybackWidget.cpp:303`), colliding with the real `startLabel` (:311). Object names are the addressing scheme for accessibility/probes; two widgets answer to one name.
- **R4 [R] — `rectifyUI` is a full teardown/rebuild.** It deletes every widget and the whole grid, calls `QCoreApplication::processEvents()` mid-teardown, then rebuilds all rows (`src/gui/AudioPlaybackWidget.cpp:242-261`): any list change re-creates the entire UI (focus/scroll loss; re-entrancy risk from `processEvents`).
- **R5 [R] — Fixed-grid layout pain is self-documented.** "This is a great time to tell you that we accept pull requests that fix this kind of nonsense" (addUIState, `src/gui/AudioPlaybackWidget.cpp:272`) and the `exportAudio` placement comment (:286-290: the current cell exists because row 0 col 2 landed on the "Playback end frame" label). "Every cell in the four columns is taken."
- **R6 [R] — Theme coupling through a scratch-dir icon.** `onThemeUpdated` re-fetches the runtime-tinted speaker pixmap from the regenerated scratch dir on every theme change (`src/gui/AudioPlaybackWidget.cpp:265-271`; wired `src/gui/PlayBackWidget.cpp:66-68`).
- **R7 [R] — "Placeholder" name-as-state-flag.** `audioName == "Placeholder"` means "unconfigured" (`src/gui/AudioPlaybackWidget.h:30`; selector text `:382-384`) and doubles as "empty config, skip the overwrite warning" (:195). A string is the state.
- **R8 [R] — Layering: core depends on GUI code.** `core::Project` calls `AudioPlaybackWidget::deserialize` from the `.aemus` loader (`src/core/Project.cpp:56-61`); `tools/verify_blur/verify_blur.cpp:114-116` and several `tests/` stub `AudioPlaybackWidget::deserialize` to link `core::Project` without the GUI (evidence from §F).
- **R9 [R] — Remove-next semantics: button words, documented invariant, and code disagree.** The button is labelled "Remove next audio track" (`src/gui/AudioPlaybackWidget.cpp:418`) and the invariant comment claims "every earlier row removes the track after it. That is exactly what the click handler maintains" (`:399-415`). But the handler (`:456-470`) computes `bool deleteNext = config->size() > 2; int delIndex = index; if (deleteNext && index + 1 <= (int)config->size() - 1) delIndex = index + 1;` before erasing. With EXACTLY 2 tracks, row 0's button erases `delIndex = 0` — its OWN track — not the next one; only with 3+ tracks does it erase `index + 1`. The same comment says the old `size()==2` special case "is gone", yet `deleteNext = config->size() > 2` IS a surviving size==2 special case. So the2-track behavior contradicts both the label and the documented invariant (see Q9).
- **R10 [R] — Audio-as-inverted-mute naming.** The export request maps the checkbox to `buildRequest().mute = !mAudio.value()` (`src/gui/qml/ExportViewModel.cpp:1090-1091`) — the domain field is "mute", the UI speaks "export with audio"; every hop must invert correctly (read-back :1267).
- **R11 [R] — No cross-clamping of the frame window.** The spinboxes accept 0..INT32_MAX independently (`src/gui/AudioPlaybackWidget.cpp:293-300`, writes `:473-482`); an inverted range is silently SWAPPED by the validity filter instead (`src/gui/AudioPlaybackWidget.h:102`). Contrast the export form's clamp-against-each-other rule (`docs/export-dialog-behavior.md` F36). And two sentinel conventions: a new track defaults `endFrame=500` (`AudioPlaybackWidget.h:29-41`) while the mixer reads `endFrame=0` as "to end of export range" (`AudioMixer.h:21`).
- **R12 [R] — Per-range audio concat.** The exporter mixes per selected range and concatenates (`src/ctrl/Exporter.cpp:585-638`, rationale :588-593): a gapped selection's audio is its selected frames' sound back to back.
- **R13 [R] — Silent failure at playback.** `QMediaPlayer` errors are connected to nothing (no errorOccurred hook, §C): a missing/corrupt file just plays silence. At export a bad track is skipped and surfaces only in `AudioMixer::error()` (`src/media/AudioMixer.cpp:72-75`, `:87-92`). Errors are modal boxes at config load/save only (§A).
- **R14 [R] — `deserialize` swallows all exceptions with a `TODO`** (`src/gui/AudioPlaybackWidget.cpp:119-120`); old files without the Export key default it true (:106-109) — silent data-shape drift.
- **R15 [R] — `startFrame`/`endFrame` are read three different ways (a reading divergence).** (1) Export/mixer: the file's beginning is placed at TIMELINE frame `startFrame` — `frameToSamples(track.startFrame, …)` (`src/media/AudioMixer.cpp:49`), `dst = mSamples.data() + offset * channels` writes file sample `s` at output `offset + s` (`:81`), `endFrame` clamps the contribution length (`:55-60`); the field comment says "offset, in project frames" (`AudioMixer.h:20`). (2) Preview: `trackPositionMs` (`src/gui/AudioPlaybackWidget.cpp:30-33`) computes the media position as `(config.startFrame + curFrame)/fps` ms from the ABSOLUTE timeline frame — under the export reading it should be `(curFrame − startFrame)/fps`; at the window's opening (`curFrame == startFrame`) the preview plays `2*startFrame/fps` into the file. (3) The UI labels "Playback start frame"/"Playback end frame" (`:362-363`) read like a trim of the FILE. So preview and export can hear different alignment for any track with `startFrame ≠ 0` — the sharpest concrete case of the Q1 preview≠export gap (see Q10).

## Theme tokens for the mockups

Source of truth: `src/gui/theme/TailwindPalette.h` — GENERATED by `scripts/generate_tailwind_palette.py` (26 rows × 11 steps; `kTailwindRowCount`/`kTailwindStepCount` :17-18). Mockups must use these hex values verbatim (existing mockup precedent: `tools/dock_mockup/index.html`). Rows a mockup will likely need (full table :23-48):

- Neutrals: slate :40 (`#f8fafc #f1f5f9 #e2e8f0 #cbd5e1 #94a3b8 #64748b #475569 #334155 #1e293b #0f172a #020617`), zinc :42 (`#fafafa #f4f4f5 #e4e4e7 #d4d4d8 #a1a1aa #71717a #52525b #3f3f46 #27272a #18181b #09090b`), gray :41, plus custom rows mist :47, taupe :48, mauve :45, olive :46.
- Status/accent rows (transport/meter states): red :23 (`#fef2f2 … #ef4444 … #450a0a`), orange :24, amber :25, yellow :26, green :28 (`#f0fdf4 … #22c55e … #052e16`), emerald :29.

Semantic tokens from `src/gui/theme/Colors.h` that the audio UI already touches:

- `kWindowBand` (:76) — "temporary playback window: translucent band across the [window]" — the token literally reserved for the floating playback/audio window surface.
- Accent reservation (:79): the accent is reserved for the playhead/selection/marquee — audio chrome must not steal it.
- `kAccentBrightText` (:83) + `accentBright` (:273) — text on accentBright fills (playhead pill) / brand bright edge (focus ring, marquee).
- Floaters: `kFloaterBody`/`kFloaterEdge` (:69) — floater surfaces and edges (popups, detached-window edges).
- Accent enum `kAccentRed/Orange/Yellow/Green` (:30) with Tailwind row mapping (:36-44).
- The volume slider is custom-painted `gui::tool::ToolSlider` because QSS cannot draw a clean circular head on a thin track (`src/gui/AudioPlaybackWidget.h:63-65`) — mockups may assume that look.
- Icons are runtime-tinted (speaker glyph re-fetch per theme change, `AudioPlaybackWidget.cpp:265-271`) — mockups should specify icon tint, not baked-in colors.

## Design questions for the user

- **Q1 — Preview vs export fidelity.** Playback is N independent QMediaPlayer tracks; export is one offline libav sum (§G1/G2). Keep this split ("preview is approximate"), or should the redesign offer to hear the actual export mix?
- **Q2 — Flag truth table.** The comments sanction (playback on, export off) = reference track (`AudioPlaybackWidget.h:32-37`). Is (playback off, export on) = "export-only" also intended? Today it is silently dropped (R1). The design needs the intended truth table of (playback, export) × (audible, exported).
- **Q3 — Window hosting.** The Audio Player is a detached `Qt::Window` opened from the transport's speaker button (`src/gui/PlayBackWidget.h:48`, `:55`, handler `:99-122` — which even stops every player on open, :114). Keep the detached tool window, or dock/inline the audio UI in the main window?
- **Q4 — Gapped-selection audio.** Keep per-range concat (R12), or export the selection's timeline span with silence in the gaps?
- **Q5 — Frame-window rules.** Clamp end ≥ start at the control (the export form does, F36) instead of silently swapping at the filter (R11)? And pick one sentinel convention: new-track `endFrame=500` vs AudioMixer's `endFrame=0` = "to end" (R11)?
- **Q6 — Save/Load fidelity.** Should `.aemus` persist every track (true round-trip) or only valid ones (current, R2)?
- **Q7 — Failure loudness.** Silent-at-playback / modal-at-config-load / skipped-at-export (R13): what should the redesigned UI do — inline per-row error state, a status area, toasts?
- **Q8 — Scope ceiling.** Meters, waveforms, pan/fades do not exist in the engine (§G4). Is the redesign limited to exposing existing capability, or may Phase 3 add engine features?
- **Q9 — Remove semantics (grounded in a real divergence).** The button's words, the documented invariant, and the code disagree (R9): the button says "Remove next audio track", the comment at `:399-415` says every earlier row removes the track after it, and the handler removes the clicked row's OWN track when exactly 2 tracks exist. Which interaction should the redesign have — remove the row you clicked, or remove the next one? Either way the 2-track divergence is a defect, not a behavior to preserve.
- **Q10 — What do `startFrame`/`endFrame` mean?** Three readings coexist (R15): timeline placement of the file (the export/mixer reading), a trim of the file (the UI labels' reading), or something else. Which is intended — and which does the redesigned UI make primary?
