# Handoff brief — export dialogs: redesign COMPLETE through the post-R1 plan and rulings

Paste this into the new session. It is self-contained; deeper detail lives in the files it points to.

## Outcome (read first)

The 16 review defects are fixed, the dialogs live on Qt Quick Controls where the design allows,
the behavior contract is fully decided (no open `[Q]` lines), the post-R1 plan's seven increments
plus the 2026-10-01 and 2026-10-03 batch rulings are all landed, and the closing 13-probe sweep
passed **13/13, `sweep_rc=0 portal=0`**. What remains is small, listed below — nothing blocks.

The three 2026-10-03 rulings, in the user's words: **AV1 at every non-Lossless archive bucket with
transparency OFF** (settled the Balanced-mapping question); **"show only the control that governs the
file"** — hide the Quality bucket wherever an image is written, so the Quality row follows the
artifact, not the mode; and **"Tag hvc1 whenever HEVC lands in an mp4/mov"**. A fourth decision
deferred the Wayland real-input-path evidence by name ("Defer it"). All four are recorded as dated
amendments in `docs/export-dialog-behavior.md`, **all three code changes are committed as `b686a83`**
(one batch: they overlap in `ExportTargets.txt`, the generated header and `tests/media_export_target.cpp`),
and that commit passed the full always-on gate plus three targeted probes.

## Mission and standing directives (user's, still binding)

- The design is the specification; **"Nothing in UI design is negotiable — design informs the
  implementation, not implementation informing the design."** Where the contract doesn't settle a
  behavior, ask the user; record the answer in `docs/export-dialog-behavior.md` (`[Q]` → `[D]`
  with a date; precedents: items 5/10/17/22/25/28/34/35/37/39/42/43/44 and the 2026-10-01 batch).
- **Agents do the actual implementation; you orchestrate, plan, review, own architecture.**
  Dispatch to `subagent_fork` (it inherits the session). Agents do not commit and do not run
  `run_probes.sh` sweeps; the orchestrator reviews diffs + evidence, runs gates, commits.
- **Verification rule**: a line counts as verified only with a captured observation or an
  automated test that fails on regression. Never argue what you didn't measure.
- **Gate policy (user-approved 2026-09-29)**: always-on = zero-warning builds Debug+Release,
  `scripts/run_suites.sh` both configs (now **21 suites** + the 2 i18n checks inside that count),
  `grep -c unfinished assets/locale/translation_*.ts` = 0, pixel/geom evidence for visual changes.
  Targeted probes only (grep `scripts/gui/probe_*.sh` for the touched control names, 2–4 probes).
  Full `run_probes.sh` sweep ONLY for a11y-factory / shell-dialog-wiring / harness-platform-theme
  changes and rung milestones — announce first (R7), canary first, compositor watcher on.

## State at handoff

- **HEAD: `ab4ca9c`**, branch `feat/export-ui-redesign` — **tree DIRTY: six files, three agents'
  in-flight defect work** (see "The five defects" below). This arc's trail (newest first):
  `ab4ca9c` a childless top-level destination is no longer indented as a child (defect 2, 6 files,
  362+/3−) · `c14ac2c` the keep-aspect-ratio lock recursed 261 291 frames until the stack died
  (defect 5, 4 files, 141+/1−) · `ff37603` E28 resolved, overhang declined (item 6 closed, 2 doc files) ·
  `c30c6e5` the popup wall is the compositor BACKEND, not the host (item 6 measurement, 4 files)
  · `8e6ed15` i18n catalog extraction debt (item 4 below, 2026-10-03) · `b686a83` the
  2026-10-03 batch (AV1 everywhere + artifact-scoped Quality row +
  conditional `hvc1`) · `89da2ed` archive rule + Advanced codec row (2026-10-01 rulings) · `af7b536`
  contract batch rulings · `54ecc23` I43 size cap + the two sweep-caught regressions · `c28c73b`
  I44 codec schema · `9075bbc` persistence C17/A5 · `63f597e` C21 ghost fill · `892b492` overflow
  D25/Q9 · `9abcea2` tooltips E29/E27/E28 · `7a58bc7` canary self-contamination lesson · `e91bab6`
  Escape B10/F34 · `31cfb2d` host decision (keep QQuickWidget) · `596dc19` scrub F31/F33 ·
  `386eeec` Q1–Q9 decisions. Earlier: `f7ef22e` … `f5aa28d` the R1 control conversions.
- Closing sweep (2026-10-01 18:3x–19:13): 13/13, portal=0. Suites 21/21 both configs. Catalogs 0.
- **2026-10-03 arc CLOSED, committed as `b686a83`** (13 files, +566/−52). The three changes:
  (a) AV1 everywhere for `archive.master` (agent ec1590d4) — the deliberate break fails exactly 4
  named asserts pre-change, `ALL PASS 0` after; (b) the Quality-row `qualityVisible` fix (agent
  c424be8b) — the call-form binding threw 24 TypeErrors/run (×2 configs) and drew the row in both
  modes, now `PASS 60 FAIL 0` with a TypeError counter and both-direction assertions; (c) the
  conditional `hvc1` tag (agent e2e9f0bf) — `codec="hevc"` qualifier on `<codecTag>`, honoured only
  when the plan RESOLVED to HEVC. **Gate run 2026-10-03 11:04–11:20 on the frozen tree, all green**:
  builds Debug+Release exit 0 / warnings 0 (both NO-OP-fresh: the 2 automoc lines then restat prune);
  `run_suites.sh` 21/21 ×2 (both i18n script checks PASS ×2); catalogs 0 unfinished ×6; probes
  `probe_format` 25/25, `probe_quality` 20/20, `probe_axes` 15/15, rc=0 each, `portal=0` (both
  journal hits are this harness's own `[systemd-run]` echoes), no stray Xvfb/app processes. Logs:
  `/tmp/gate_build_{debug,release}.log`, `/tmp/gate_suites_{debug,release}.log`,
  `/tmp/gate_probe_{format,quality,axes}.log`. A full `run_probes.sh` sweep was deliberately NOT run
  (not a rung milestone; the touched surface is planner/tests/QML + three probe-visible rows, and
  those probes were run individually).
- **i18n extraction debt CLOSED, committed as `8e6ed15`** (2026-10-03, item 4 below): the two
  strings the `b686a83` batch added (`Size cap (MB)`, the rewritten AV1 archive note) now reach a
  reader in all five catalogs; 7 files, +903/−774. `translation_en.ts` byte-identical (md5
  `584dc4a6b75d63f61403c6e808675a2f`). Graded by `grep -c unfinished`=0 ×6,
  `check_tr_contexts.py`, `gen_export_targets_i18n.py --check`, `run_suites.sh` (23 suites, both
  configs) and a post-build `QTranslator` witness over the built `.qm` (with a negative control),
  because none of the script gates can see `.ts` → `.qm` → lookup. Extraction traps are now in
  AGENTS.md under "Translations".

## What remains (priority order — all small, all flagged to the user)

> **The live work is not in this list.** After the user ran the app on his own desktop on
> 2026-10-03 he reported defects, and kept reporting them as he used the delivered build; they are
> tracked in **"The defects the user reported 2026-10-03"** near the end of this file. Two are
> fixed and committed, one is a confirmed design change awaiting implementation, three are in
> flight. Items 1–5 below are the older, mostly-closed backlog, kept for the reasoning.

1. ~~**Balanced-mapping confirmation**~~ **CLOSED 2026-10-01 by the user's own ruling**: the ladder
   was the wrong question — *"Why would the ladder pick H.265? AV1 all the way makes more sense."*
   `archive.master` now takes AV1 at EVERY non-Lossless bucket with transparency OFF; H.265 is only
   the fallback where no AV1 encoder is usable. The `fastFirst` family demotion is deleted from the
   Archive block (it still orders hardware-vs-software WITHIN a family, as everywhere else).
   Contract: behavior item 45 `[R7]`. The ladder rule landed with `89da2ed`; its AV1-everywhere
   amendment landed with `b686a83`, tests amended (pre-change: exactly 4 named asserts fail).
2. ~~**hevc-in-mp4 `hvc1` tag nuance**~~ **CLOSED 2026-10-03 by ruling (3), committed `b686a83`**:
   *"Tag hvc1 whenever HEVC lands in an mp4/mov"* — a `codec=` qualifier on `<codecTag>`, honored
   only when the planner actually resolved HEVC (`formatKeyForEncoder(plan.config.codec)` — the one
   shared normalizer); the six untagged HEVC-capable mp4 rows gain it, `safari.transparent` stays
   unconditional. Contract: behavior item 44 `[R15]` (2026-10-03 paragraph). Measured en route: the
   mp4 muxer *rejects* an alien `codec_tag` outright (`Tag hvc1 incompatible with output codec id
   '27' (avc1)`, 13 such, container open fails), which is why the tag cannot be unconditional; and
   the H.265 leg is forced by pinning `libx265` in the suite (this build resolves AV1 by default).
3. ~~**Quality label's latent binding trap**~~ **FIXED 2026-10-01, committed 2026-10-03 `b686a83`**
   (agent c424be8b): the call form `exportModel.manualMode()` at `ExportDialog.qml:511`
   and `:516` threw `TypeError` at every dialog construction and left the row drawn in BOTH modes.
   Replaced by a real `Q_PROPERTY` — `qualityVisible` = `!imageQualityVisible`, `NOTIFY
   constraintsChanged` — because the predicate the row actually needed is the ARTIFACT, not the mode
   (the widget era's `!isSequence`): a still or a numbered-sequence folder writes no video stream, so
   the Quality bucket provably governs nothing and the row must be hidden. Contract: behavior item
   46. `tests/form_layout.cpp` now asserts BOTH rows in each case plus a `TypeError` counter on
   stderr, and `probe_format.sh` gained the `assert_hidden "Quality"` (and three `assert_showing`
   halves) that would have caught it — `probe_format` 25/25 green.
4. **i18n extraction — TAKEN OVER by the orchestrator 2026-10-03 (was "for the user"; that was
   wrong)**. The "54-string debt / 2151 lines per catalog / EN skeleton fills up" figures in the I43
   notes were **re-measured today and are stale**: the intervening i18n commits (`79695a5`,
   `5ccf05e`, `efdae31`) plus the `src/gui/CMakeLists.txt` source-list fix that made the 48
   `ExportTarget.cpp` strings visible have paid almost all of it. Measured now, running the real
   extraction target `ninja -C build 'src/gui/update_translations:Debug'`:
   - per catalog: `Found 893 source text(s) (2 new and 891 already existing)`, `Kept 1 obsolete
     entries` — i.e. **exactly 2 untranslated strings**, not 54;
   - churn: 5 files, **+840/−765 (321 lines/catalog)**, of which 159+/152− per file are bare
     `<location line=>` renumbering — not 2151;
   - **`translation_en.ts` is NOT written in practice**: lupdate *prints* "Updating
     translation_en.ts … 893 new", yet the file comes back as the same 4-line, 0-entry stub
     (md5 `584dc4a6b75d63f61403c6e808675a2f`, unchanged) — en is not in
     `I18N_TRANSLATED_LANGUAGES` (root `CMakeLists.txt:29`). The 0-unfinished gate therefore never
     sees it; the earlier fear was not reproducible.
   The 2 strings: the new label `Size cap (MB)`, and the archive note rewritten by the 2026-10-03
   AV1 ruling (whose predecessor is the run's single new `vanished` entry — read the vanished line
   of the same catalog as the vocabulary base). Both are translated in all five catalogs and the
   result is committed; nothing about this needs the user.
5. ~~Optional evidence gap: Wayland **input-path** proof~~ **DEFERRED by the user, 2026-10-03**
   ("Defer it"): the live desktop is not to be driven. Placement is Wayland-native measured; input
   was X11-only (`ydotool` absolute-move accuracy). Every observation in this arc is X11/Xvfb
   synthetic (`xdotool`/AT-SPI/`QKeyEvent`) or in-process; libinput → compositor → client
   focus/grab semantics are **unmeasured**. This is a platform-confidence gap, not an unverified
   contract line — the contract lines are all measured on the X11 path. Recorded in
   `docs/export-dialog-implementation-notes.md` ("Open evidence gap, deferred by the user").
   Reopens only on a Wayland-only input complaint; the measurement then is: activate the window,
   drive one dialog with `ydotool`, screenshot with `ffmpeg -f x11grab -draw_mouse 1`.
6. ~~**Host question / tooltips**~~ **CLOSED 2026-10-03 (see the tail of this item): the user ruled
   the in-scene flip/shift is what he wants, so no code changed.** Kept for the measurement record.
   The old text said only wanting tooltips that OVERHANG
   the dialog would flip the host, and left it hanging. Measured (weston 15.0.1 / Qt 6.11.2, private
   runtime dir + socket, spike `host-spike/popup-spike.cpp`): the wall is the compositor **backend**,
   not the host and not "Wayland" — a window-backed `Popup.Window` never maps from a `QQuickWidget`
   on weston's `--backend=headless` (0/9, at 300/1200/3000/5000 ms settle, under both the desktop
   and kiosk shells) but does on `--backend=x11` (9/9), and a `QQuickView` maps it on both; on xcb
   the two hosts are byte-identical. Not the client GL/EGL path: the two `WAYLAND_DEBUG=1` wire logs
   show the same mesa bootstrap, the difference is that the headless client never even sends
   `get_xdg_popup`. No production QML uses `popupType` at all, so nothing changed in code — the
   in-scene tooltip meets E28 by flipping/shifting. Corrected in the notes (new section
   "The Wayland limitation is a backend limitation") and in the three QML headers that had stated
   the old claim as fact (`AeComboBox.qml`, `AeToolTip.qml`, `DestinationPicker.qml`).
   **CLOSED 2026-10-03 — the user ruled: keep the in-scene flip/shift, the overhang look is NOT
   wanted.** That was the single thing that could have re-opened the host ruling, so `31cfb2d`
   stands and no code changed. Recorded in the contract (item 28 `[R10]` "RESOLVED 2026-10-03") and
   in the notes as `ff37603`. The same ruling exposed two stale "still clipped today" statements
   that predated `9abcea2` (the flip+clamp commit); both are corrected to name the historical state.
   Measurement landed as `c30c6e5` (4 files: the notes + the three headers), ruling as `ff37603`
   (2 files: contract + notes; documentation only). Gates on the frozen tree: build
   Debug+Release rc=0 warnings 0, `run_suites.sh` 21 pass/0 fail BOTH configs, catalogs 0 unfinished
   x6, `probe_format` 25/25, `probe_quality` 20/20, portal=0, no stray processes. Logs
   `/tmp/item6_{build,suites}_{Debug,Release}.log`, `/tmp/item6_probe_{format,quality}.log`.
   The measured overhang evidence is *leftward*: on xcb the window-backed popup's rect sits 200 px
   left of the host (`OVERGEO ... left=200 top=-300 bottom=-70`) - a capture with a positive RIGHT
   spill (`right > 0`) was never produced and is not needed for the claim as written.

## The defects the user reported 2026-10-03 (first real GUI session)

He ran the app on his own desktop and reported these. Defects 1–5 came from the first session;
**defect 6 arrived after the pristine `ab4ca9c` build was delivered and he was using it. Two are
fixed, gated and committed; four are in flight or dispatched in the main worktree as of this
handoff.** Every one is a UI-design question, so the standing rule applies: design decides
implementation, and where the contract is silent the user is asked and the answer is recorded as a
dated `[D]` line.

1. **"Why is transparency under Advanced? It should be on the regular options and default off —
   when it is on it pushes the codec choice to a suboptimal codec."** IN FLIGHT (agent `655042c4`).
   The reason is measured and real: with alpha on, `libsvtav1` silently drops it, `libx265` refuses,
   `libvpx-vp9` and `ffv1` are correct — the toggle trades codec quality for alpha, which is a
   user-visible trade-off and therefore belongs on the regular surface. QML move + model wiring are
   done; outstanding are the default flip (`ExportViewModel.h:285` and `MainWindow.cpp:1990`, both
   `true` → `false`; a persisted user value still wins), the docs (a dated `[D]` superseding item
   42's *placement* clause **for transparency only** — item 42's statement/provisioning principle
   stands), a `tests/form_layout.cpp` assertion, and a re-measurement of the 630/558 geometry figures.
2. **"Why is a top-level destination with no children indented like a child?"** **FIXED, committed
   `ab4ca9c`** (6 files, +362/−3). `DestinationPicker.qml` had no notion of depth and used
   `model.isSection` as a lane table, so every destination took a child lane. New `LevelRole`
   (`0` = listed directly — a section or a childless top-level destination; `1` = nested under a
   section; explicitly *not* "is a section") drives `indentPx: model.level * 14`. Before/after
   geometry and both new suites are in the implementation notes.
3. **"Why does an export input field stay focused after I click elsewhere?"** IN FLIGHT (agent
   `6796d790`). `ExportDialog.qml:108-115` has a background `MouseArea` meant to release focus, but
   (a) it guards on `fi.objectName === "scrubEdit"` while no exported `Ae*` control sets an
   `objectName`, so the guard can never match, and (b) it is a **sibling declared after** the
   `Flickable`, so a press inside the scrolled content never reaches it — and mouse events never
   travel to siblings. Orchestrator ruling on the intent: **a press anywhere that is not on an
   interactive control releases keyboard focus; a press on an interactive control keeps focusing
   it.** The mirrored guard in `AnimationSettingDialog.qml:38-43` is the agent's call to make;
   `objectName: "scrubEdit"` must not be renamed or removed (`tests/scrub_escape.cpp` depends on it).
4. **"Why do so many targets default to less than the project resolution when they impose no
   resolution limit? And why doesn't Reset to native actually reset to native? The button often does
   nothing."** AUDITED (agent `8a4b32ac`); **user ruled option A** — *"A：默认=项目原生，框只作说明，
   不设上限（推荐）"*. Not yet dispatched. The facts that make him right: 32 of the 66 rows are `max`;
   `applySizeRule`'s `Max` branch **never upscales** (`std::min(1.0, …)`); of the 64 annotated
   non-`free` rows only 16 mention no pixel/resolution number at all and **9 of those 16 are
   1280×720/1080p-class `max` rows**; and **no `max` row's note claims an upload *rejection*** —
   every documented hard cap is bytes or duration (Discord 20 MB + Nitro tiers, X 8 GB/20 min,
   TikTok 4 GB, Reels 300 MB + faststart; the only explicit rejection is Reels' moov/faststart
   requirement). 1280×720 is a fit/resample target, so the `ExportViewModel.cpp:994-995` "CEILING"
   comment and any wording derived from it must be retracted as a block quote. `exact` /
   `maxExactSide` / `maxMinSide` are genuine requirements and stay. Also note the table's own
   `free` documentation (`assets/export/ExportTargets.txt:74-93`): "the export dialog's own
   width/height apply (they start at the project size)".
5. **"The keep aspect ratio button crashes the app."** **FIXED, committed `c14ac2c`** (4 files,
   +141/−1). Unbounded mutual recursion `ExportViewModel::setWidth` ↔ `setHeight`: **261 291
   frames** deep, SIGSEGV, core dumped, caught in gdb. The rounding has no fixed point
   (`720→680→418→680`), and the derivation sits *after* the `if (mWidth != aValue)` guard, so it
   re-fires forever. Fixed with a one-way `mWritingSize` guard. Before RC=139 core dumped / after
   RC=0, 1232 PASS 0 FAIL. One edge case is left open and recorded: **clamp-vs-derive ORDER is
   unverified** on a clamped instance; option A removes the `Max` clamp but `exact` /
   `maxExactSide` / `maxMinSide` still clamp. Contract item 36 `[D]` records the lock semantics:
   enabling the lock preserves the ratio, and Reset re-baselines the ratio the lock then maintains.
6. **"I noticed that the Details text doesnt wrap and instead introduces horizontal scroll."**
   DISPATCHED 2026-10-03 as a second workstream of agent `655042c4` (it already holds both files
   this touches). Reported while he was using the delivered pristine `ab4ca9c` build, so it is
   present in that binary too. The location is the notes block behind the `Details` disclosure,
   `src/gui/qml/ExportDialog.qml:289-316`. **The naive reading is wrong**: the delegate already has
   `Layout.fillWidth: true`, `wrapMode: Text.WordWrap`, `visible: factId === "note"` and its
   `Accessible.role`/`name`. It is a width-propagation defect. Mechanism, and the loop that makes it
   self-reinforcing: `:138 contentWidth: Math.max(width, page.implicitWidth)` feeds `:183 width:
   scroller.contentWidth`; if the note's **unwrapped** natural width reaches the page's
   `implicitWidth`, the page becomes that wide, the layout then hands the `Text` that full width, so
   it never needs to wrap — and the `Flickable` has a horizontal axis. That is exactly "doesn't wrap
   AND introduces horizontal scroll". Not yet confirmed by measurement; the agent must print the
   numbers before editing anything. **Census that makes this a class-of-one, measured 2026-10-03:**
   across all of `src/gui/qml/` there are only **two** wrapping `Text`s in this form —
   `AeFactRow.qml:63` and `ExportDialog.qml:308`. `AeFactRow` is the one that behaves, and it says
   why in its own comments: `:27 implicitWidth: 340` (a real, pinned implicit width) and `:28-30`
   *"contentHeight, not implicitHeight: the value WRAPS, so its height depends on the width the
   layout gives it."* The notes delegate has neither discipline. **Design intent is already on
   record, so no new ruling is needed**: the axis exists only because the form's own minimum size
   does not fit (`ExportDialog.qml:125-127`), and D25/Q9 forbid the form overflowing without a
   scroll — a long note wrapping and the form growing *taller* is the designed remedy, not a new
   choice. Only a vertical `ScrollBar` is declared (`:775-797`, `policy: ScrollBar.AsNeeded`, D23),
   so what the user saw is content wider than the viewport (pannable), not a drawn horizontal bar.
7. **"CRF and size cap have greyed out auto, but when I click in them, they have a value, but a gray
   auto still shows behind the value. … Auto should be a special value. Similarly the Quality
   dropdown just gets disabled and still shows the last set quality … The convention here is to show
   a special value there as well as Custom."** NOT DISPATCHED — waiting on one design choice (see
   below). Mechanism pinned 2026-10-03, no agent needed to find it: `AeNumberField.qml:35-38`
   defines `placeholder` (a word) + `placeholderValue` (a number) and
   `atPlaceholder = placeholder !== "" && Math.round(value) === Math.round(placeholderValue)`. When
   it is true, `:108 showValue: !root.atPlaceholder` hides the number inside `ScrubSpinBox` and
   `:128-137` draws a muted `Text` with the word **as a later sibling, i.e. on top of the whole
   field including its editor** (`ScrubSpinBox.qml:192 visible: activeFocus`). So clicking the field
   puts the editor on screen and the grey word *also* stays drawn — two texts in one place, which is
   exactly the reported "still shows behind the value". And "auto always shows" is literal: the word
   is a *consequence of the number equalling the default*, not a state. The sentinels are in-range
   values (`ExportViewModel.cpp:416 sizeCapAutoValue() == 0.0` with `:413 sizeCapMinimum() == 0.0`;
   `:513-516 qualityOverrideAutoValue()` with `.h:390 qualityOverrideMinimum() == qualityOverrideAutoValue()`),
   so no set of numbers can distinguish "Auto" from "the user chose that number". The user's ruling:
   **Auto is a special value, and the Quality combo needs an explicit "Custom" entry** so the presets
   stay reachable after a manual override (`setQualityOverrideToAuto()` `:857`,
   `setSizeCapToAuto()` `:923`, `setImageQualityToAuto()` `:908` already exist as the way *back*).
8. **"How does the export FPS work? if the project FPS is 60 and the export FPS is 24, does a
   keyframe at frame 60 in the project land on frame 60, or does it land on frame 24 and sub-frame
   keyframes get interpolated correctly?"** ANSWERED 2026-10-03, code read end to end (no new
   commit needed — nothing changes). **It lands on output frame 24.** `Exporter.cpp:383`
   `const double current = (mIndex * mOriginTimeInfo.fps) / (double)mCommonParam.fps;` maps an output
   index into *project-frame space*, and the numerator is genuinely the **project's** fps:
   `mOriginTimeInfo = mProject.currentTimeInfo()` at `:113`/`:138`/`:172` (`Exporter.h:209`), which
   is `Project.cpp:104-111` (`time.fps = mAttribute.fps()`). Note `current` is an index into the
   **flattened selection** (`:384 base = (int)current`, `:385 count = mSelectedFrames.size()`,
   `:389`'s guard is in those units), so output frame *k* samples selection index *2.5k*; for the
   ordinary case — a contiguous export whose first selected frame is project frame 0 — that is
   project frame *2.5k*, so project frame 60 → output frame 24 and the duration is unchanged
   (general rule: project frame *p* → output frame *p·exportFps/projectFps*). With a gapped
   selection the sampled position is interpolated between the two neighbouring **selected** frames
   (`:395 next = base + 1`, `:396-397`'s comment says why: "with several ranges the following output
   frame is not frame + 1"). Sub-frame positions are interpolated,
   not snapped: `:400-403` `frame = mSelectedFrames[base] + frac * (next - base)` then
   `Frame::fromDecimal` (`src/core/Frame.h:12-16`, thousandths at `:44-47`), and the fraction
   *survives into keyframe evaluation* — `TimeKeyGatherer.cpp:38` skips its exact-frame fast path
   whenever `frame.hasFraction()`, `:88/:105/:133/:149` evaluate at `frame - baseFrame.getDecimal()`,
   and `SRTExpans.cpp:15` keys its spline cache on the decimal. Two honest caveats: the mappings
   between two output frames are *not* frame-blended — each property curve is re-evaluated at the
   fractional time; and the run ends on fractional overshoot (`:389` halts at the first output index
   whose sampled position passes the last selected frame), so a selection of N frames emits
   `floor((N−1)·exportFps/projectFps) + 1` — **not** a rounded `N·E/P`, so the tail cannot be
   reasoned about from the ratio alone.

**The launch he asked for ("给我拿一个能跑起来的构建") is done and reproducible:**
`.git/dsh-scratch/build_head_ab4ca9c.sh` builds a **pristine `ab4ca9c`** in a throwaway worktree
`.git/dsh-scratch/wt/head-ab4ca9c` → `.git/dsh-scratch/wt/head-ab4ca9c-build` (needs
`-DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62`, else configure dies at
`src/media/CMakeLists.txt:57`). Measured 2026-10-03 22:47: **343/343 steps, 0 warnings, 0 errors**.
`.git/dsh-scratch/launch_live_ab4ca9c.sh` starts it on the user's REAL desk — deliberately the
inverse of the headless recipe: `WAYLAND_DISPLAY=wayland-1`, `DISPLAY=:0`,
`QT_QPA_PLATFORMTHEME=cosmic`, `QT_QPA_PLATFORM='wayland;xcb'`, `QT_FORCE_STDERR_LOGGING=1`, values
read from the session's own processes. It loaded `assets/sample.psd` (720×1172, 13 layers) and
`libqwayland.so` is the only QPA plugin mapped — i.e. it is genuinely a Wayland client on the live
compositor, not on Xvfb. Because it is a pristine HEAD build it **contains defects 2 and 5 fixed and
not 1, 3, 4** — that is the point: it is a named-commit binary free of the three agents' in-flight
edits.

## Files that are the project's memory

- `docs/export-dialog-behavior.md` — the CONTRACT. Every `[D]`/`[R]` line with its ruling dates;
  G-codes = section+item (G37 = item 37). Never change a decided line without the user.
- `docs/export-dialog-implementation-notes.md` — measured record per increment ("…, landed:"
  sections) + the control inventory + the host-question section. Append one per increment.
- `.git/dsh-scratch/checkbox-capture/STATE.md` — the running log: plan, run logs, evidence tables,
  the flagged-choices history. Read it first when context matters.
- `AGENTS.md` — binding measured recipes. This arc added: the canary `[systemd-run]`
  self-contamination fix, the portal-check attribution gap, two QML geometry-walk traps, and the
  method-CALL-binding trap.
- Scratch (`.git/dsh-scratch/`): `sweep_runner.sh` (canary → R5 → sweep under a 35%×3
  auto-aborting watcher → after-checks; reuses `key-spike/canary_dialog.sh`), `scrub-spike/`,
  `tooltip-spike/`, `overflow-spike/`, `hover-spike/`, `persist-spike/`, `cap-spike/`,
  `host-spike/` (incl. the zero-input Wayland selftest), `adjudication/` (`alpha_probe` = the
  AV1-alpha measurement), `escape-spike/`, `combo-spike/`, `picker-capture/`, `key-spike/geom.cpp`.

## Verification recipes (short form)

- Builds: `cmake --build build --config Debug` / `Release` — zero warnings each. Ninja Multi-Config
  runs 2 automoc edges then restat-prunes: a 2-line "build" output is a NO-OP up-to-date build, not
  a failure (a `-n` dry-run misleadingly shows pending edges).
- Suites: `scripts/run_suites.sh Debug` / `Release` — exit code is the gate (**23 suites** as of
  2026-10-03, auto-discovered from `add_executable` in `tests/CMakeLists.txt`; the script needs no
  edit when a target is added). It **does not build** — it runs the binaries already on disk, so a
  stale binary silently gates the wrong tree; build first. It also opens with two script-level i18n
  checks (`check_tr_contexts.py`, `gen_export_targets_i18n.py --check`).
- Pixels: `magick compare -metric AE a.png b.png null:` at `-alpha off -depth 8 -colorspace sRGB
  -type TrueColor` — never file hashes (timestamps), and mind `#RRGGBBAA` vs Qt's `#AARRGGBB`.
  `grabToImage` composites over OPAQUE BLACK — it is blind to alpha-fade defects; sample the
  property instead (see `hover_parity`).
- Probes: targeted = `bash scripts/gui/probe_<name>.sh` inside their own harness (they wrap
  `xvfb-run` and the guards themselves). Per-probe budget lives in `run_probes.sh`
  (`PROBE_TIMEOUT`; `probe_capped` = 1800 s — its honest fit cases measure ~1120 s).
- Cursor/tooltip claims: photograph the real pointer path (`ffmpeg -f x11grab -draw_mouse 1`),
  crop + 400% nearest before judging; in-window two-step mouse approach (a teleport skips hover).
- Compositor health: `sweep_runner.sh`'s `sample_pct` on cosmic-comp's main thread; baseline 0–7%,
  abort >35% × 3 samples.

## Safety rules (measured the hard way — do not rediscover)

- `unset WAYLAND_DISPLAY; QT_QPA_PLATFORM=xcb` + `QT_QPA_PLATFORMTHEME=generic` for anything that
  runs the app under test. Raw `xdotool` only after `ae_guard_display` (exit 97 unless Xvfb serves
  `$DISPLAY`) — anything else drives the user's LIVE desktop. Live-session driving needs the
  user's explicit OK and `ydotool`.
- Portal canary before any run that opens an OS file dialog. The journal check excludes
  `[systemd-run]` echoes (a grep for "file_chooser" contaminates its own window) AND cannot
  attribute lines to a process — on an active desktop, read the journal neighborhood before
  calling `portal=1` a leak (AGENTS.md has both lessons with measurements).
- `assets/locale/*.ts`: extraction comes **only** from lupdate — the build target
  `ninja -C build 'src/gui/update_translations:Debug'` (`:Release` likewise; the bare target name
  without the config suffix does not exist). Writing translations into the entries it extracts is
  ordinary translator work and is allowed; **fabricating** a `<message>`/`<source>`/`<location>` by
  hand is not, because only lupdate knows the real source file and line. Never run `lrelease` by
  hand — the build's `release_translations` step does it and embeds the `.qm` in the binary.
- Kill by PID, never `pkill -f "<pattern>"` (it matches your own shell).
- Keep `/tmp` and the repo clean; scratch goes in `.git/dsh-scratch/`.
- Announce runs > ~10 min before starting them (R7).

## Parallel track (separate session, do NOT touch)

An audio-UI redesign runs in a **separate worktree** `/home/gambot/dev/AnimeEffects-audio`
(branch `feat/audio-ui-redesign`) — never modify/build/commit in the main tree from that side,
and vice versa. Integration with `feat/export-ui-redesign` is a deliberate later merge.
