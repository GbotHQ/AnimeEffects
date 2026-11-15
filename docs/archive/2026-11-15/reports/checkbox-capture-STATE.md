# Where the AeCheckBox R1 conversion stands (paused for a reboot)

Saved 2026-09-28. Everything durable is in this directory or in the repo; `/tmp` is
wiped by the reboot and has been copied here first.

## Working tree (NOT committed)

- `src/gui/qml/AeCheckBox.qml` — converted from a hand-rolled `Item` to Qt Quick
  Controls' `CheckBox` with our own `indicator` (box + tick) and `contentItem` (label).
  Build is clean (Debug, zero warnings). Deliberately left uncommitted: the look is
  verified offscreen but the end-to-end pixel run has not been repeated since the
  label fix.
- Everything else committed at `f5aa28d` (AeButton) — tree was clean before this file.

## Evidence, all captured rather than argued

| claim | evidence |
| --- | --- |
| rest / hover look unchanged | `cb_grabs/rest_{old,new}.png` etc: `magick compare -metric AE` = **0** for rest, hover_box, hover_label |
| the label had moved 3px after the first attempt | `cb_before/` vs `cb_after/` row crops: AE=1205 at dy=0, 19 at dy=-2 — cause measured in `geom`: Qt writes the delegate's HEIGHT from padding (17 → 12) and `verticalCenter` then re-centred the shrunken box. Fixed by anchoring the label's TOP to its own line box; geom now prints label x=24 y=3 for both versions |
| the focus ring (contract A1) exists now, and did not before | `cb_before/tabs.txt`: 30 Tabs, box-changed=0 every time. `cb_after/tabs.txt`: box-changed=72 on Tab #9. Offscreen: focus_old vs focus_rest = 0, focus_new vs focus_rest = 72 |
| Space toggles on the real dialog | `cb_after/focus_state.txt`: checked yes → no → yes |
| hover lights the whole row, as the old MouseArea did | `cb_before/`: hover at box, label and row-end each change the same 72 px; `cb_after/` identical |
| geometry unchanged | `cb_before/geometry.txt` and `cb_after/geometry.txt`: both `rect=436,619 568x24` |

Known measurement trap: ImageMagick stamps a `date:timestamp` into every PNG it writes,
so **compare pixels with `magick compare -metric AE`, never with a file hash** — two
identical captures hash differently (this cost an hour).

## Tools built for this (in the parent directory)

- `geom.cpp` — old (from git) and new AeCheckBox side by side, offscreen: geometry plus
  a rendered grab per state (rest / hover over box / hover over label / focus). Needs no
  Xvfb, no openbox, no application: ~2 seconds and no compositor load.
  `QT_QPA_PLATFORM=offscreen ./build/geom <outdir>`
  Its `achieved=` line is the precondition check — a state that was not actually entered
  is reported, not assumed (the first run's "rest" was silently a HOVER grab, because
  the offscreen pointer sits at 0,0 on top of a control drawn there).
- `capture_checkbox.sh` + `capture_body.sh` + `cb.py` + `compare_checkbox.sh` — the
  end-to-end run: manual export form → Advanced → three hover points → Tab-until-the-box-
  changes → Space twice, with a pixel diff of before vs after.
- `cb.py` exists because `ae extents` mis-addressed controls: the application owns two
  windows and both have a control named "Loop animation" (the main window's transport
  button won every name match), and QML items inside the dialog report rectangles in
  QQuickWidget coordinates, so a coordinate click for "Advanced" landed in the MAIN
  window and reported PASS. Every query in `cb.py` is scoped to the dialog subtree and
  its rectangle is corrected against the dialog's own screen rect, failing loudly if the
  result is still outside.

## Next steps, in order

1. Rebuild Debug (the QML changed after the last build? `cmake --build build --config
   Debug` anyway) and run `geom` to re-confirm AE=0 for rest/hover after any edit.
2. When the machine is cool, one end-to-end capture run and
   `compare_checkbox.sh <before> <after>` — expect rest/hover 0, focus non-zero with a
   ring. **Read PROBE-SAFETY.md first**: measure gnome-shell before/after (R5), abort if
   hot (R6), one stack at a time (R4), ask before the 16-probe sweep (R7).
3. `run_suites.sh Debug` then Release, catalogs (`grep -c unfinished` = 0), commit with
   the measurement note appended to `docs/export-dialog-implementation-notes.md`.
4. Remaining R1 increments: `AeDisclosure`, `AeComboBox`, picker rows → `ItemDelegate`
   (each also fixes its false-constraint header comment; see the pending list).

## Corrections recorded along the way

- `Qt.StrongFocus` is **0xB** (Tab | Click | 0x8), not 3 — an earlier note in this
  session's summary was wrong, and the explicit `focusPolicy: Qt.StrongFocus` in
  `AeCheckBox.qml` therefore does take effect.
- `QQuickItem::grabToImage` returns `QSharedPointer<QQuickItemGrabResult>` whose method
  is `saveToFile()` (not `save()`), and `QHoverEvent`'s Qt 6 constructor takes
  scenePos, globalPos, oldPos.

---

# Paused again — for the COSMIC session switch

User: "I'm gonna try cosmic, stop your work for now. I'll tell you when to resume."

Nothing was left running (no Xvfb, no openbox, no application, no probes), and no
background jobs are live.

## AeDisclosure: converted, verified offscreen, NOT committed

- `src/gui/qml/AeDisclosure.qml` — now a `ToolButton` with our own caret/label
  (`contentItem`) and hover row (`background`). **`checkable` stays off** on purpose:
  the old header's real point is that `QToolButton:checked` fills with the `@focus@`
  accent, so `open` stays the model's word. Debug builds zero-warning.
- `docs/export-dialog-implementation-notes.md` — the section for it is written (both
  measurement tables, the ring's provenance, the `TabFocusReason` detail).
- Verified with `geom` (both files, one engine, four states):
  rest **0**, hover **0** (both light the whole 568×30 row identically), focus
  **1208** = a 1 px ring sampled at `#a78bfa` = `theme.focus`; `focus vs rest` is 0
  for the old control, i.e. the A1 defect, still visible in the evidence.
  Grabs: `.git/dsh-scratch/disclosure-capture/`.
- Measured first: `toggled()` exists **and can be emitted from QML**, so
  `onClicked: root.toggled()` keeps `onToggled` working without `checkable`; Space
  fires `clicked` on release (so `Keys.onSpacePressed` dropped); Return/Enter are not
  consumed (so the design's `Keys.onReturnPressed` stays — this control, unlike the
  checkbox, has always handled Return); widget `QToolButton` is **TabFocus**, not
  StrongFocus, which is what the hand-rolled control did too.
- Done: Debug units **16/16**. Not done: Release build, Release units, the targeted
  capture, the 13-probe sweep. Stopped because gnome-shell was at 52% (R6) and the
  user asked to stop.

## Resume here

1. R5 first: `gnome-shell main-thread %` (baseline 9–16, abort above 35 for 3 samples).
2. Release build → `scripts/run_suites.sh Release`.
3. One capture run + `compare_checkbox.sh before after2 rep` (the checkbox must still
   pass after the disclosure change — it exercises the same Advanced row).
4. 13-probe sweep (R7: ~30 min, ask first).
5. Commit `AeDisclosure.qml` + the docs section.
6. Then the remaining R1 item: picker rows → `ItemDelegate`; then `AeComboBox`
   (the riskiest: five delegates, a popup outside the widget bounds, type-ahead Q6).

## Note for the COSMIC switch

Their session becomes Wayland: the probes are unaffected (they run under Xvfb with
`QT_QPA_PLATFORM=xcb`, and `ae_guard_display` still exits 97 unless an Xvfb serves
`$DISPLAY`), but driving the live session by hand needs `ydotool`, as AGENTS.md says.

---

# Committed: disclosure + the portal fix

- `52890bc` — `AeDisclosure` is a `ToolButton` (rest 0, hover 0, focus = a 1208 px ring
  at theme.focus; old control's focus vs rest = 0). Units 16/16 Debug and Release,
  probes **13/13**, both builds zero-warning, catalogs 0 unfinished.
- `d001e30` — the harness pin (`QT_QPA_PLATFORMTHEME=generic`) + the AGENTS.md entry.

## The portal incident (user-visible, fixed)

A sweep put save dialogs on the user's real desktop: COSMIC sets
`QT_QPA_PLATFORMTHEME=cosmic` → `libcutecosmictheme.so` →
`org.freedesktop.portal.FileChooser` → the portal draws the window in the live session.
Journal: `xdg-desktop-portal-cosmic ... no file chooser dialog with ID Id(642)`.
`QT_NO_XDG_DESKTOP_PORTAL` does not stop it (the plugin never reads it) and unsetting
the variable does not either (Qt's list is `("cosmic","generic")`). Canary first, then
the sweep under an auto-aborting watcher: 13/13, **0 portal lines**.

## Next: picker rows -> ItemDelegate (measured, not yet converted)

`picker-spike` answers the three risks:

- `toggled`/`clicked`/`TapHandler` **coexist**: with an ItemDelegate root, one tap gives
  `clicked=1 AND tapped=1`, a double tap adds `double=1`. So wiring `onClicked` for
  select/toggle and keeping the TapHandler only for `onDoubleTapped` reproduces today's
  behaviour exactly (a double tap on a section still nets two toggles, as it does now).
  Wiring both for single taps would toggle a section TWICE.
- `hoverEnabled` defaults **true**; `focusPolicy` defaults **NoFocus** - rows stay out
  of the Tab order, so contract A1's tab sequence is untouched.
- Instrument trap: a `TapHandler` sees NOTHING from events posted to the item (tapped=0
  for every synthetic tap) - pointer handlers are driven by the *window's* delivery
  path. Post them to the window, as `geom` does for hover.

Mapping: Rectangle's visuals become `background`, `rowHover.hovered` becomes the
control's `hovered`, the caret/badge/label children stay anchored to the delegate root
(parent is still the control, so positions do not move), HoverHandler stays only for
`cursorShape` and the tooltip wiring. Verify with `geom` (old vs new row, four states)
plus `picker_keyboard` (32 checks on the key contract).

## Standing working mode (user directive)

"For future work, you should use agents for the actual implementation while you
orchestrate, plan, reviews, and handle architecture at a high level."

So: each increment's code + its immediate verification goes to a subagent (fork, so it
inherits this context). The orchestrator keeps: design decisions, the gate suite (R5
compositor measure, dialog canary, 13-probe sweep, R7 announcement), diff review,
`docs/` notes, and commits. Subagents do not commit and do not run `run_probes.sh`.

---

# Picker rows -> ItemDelegate: landed (this session, agents did the implementation)

- First implementation agent was **stopped by the user mid-verification** (it was
  interfering with their desktop use); the second finished it. Lesson recorded in the
  working-mode note above: builds/sweeps are the interference, not windows (those are
  offscreen on Xvfb).
- **The drag delta (31544 px) was real and is fixed**: `hoverEnabled: true` latches
  `hovered` because a press-drag delivers MouseMove and NO HoverLeave to crossed rows;
  the row's HoverHandler also goes blind during the gesture. Fix: `hoverEnabled: false`,
  colour reads `rowHover.hovered` as before. Residual accepted by the user: during a
  held-button drag hover freezes until release (first buttonless move = exact parity).
- Gates (orchestrator-run): suites 16/16 Debug + Release, catalogs 0, geom 10/10
  achieved=yes, AE 0/0/0/0/0, probe_dest 95s PASS, full sweep **13/13** in ~32 min with
  canary clean, 0 portal lines, cosmic-comp 0-7% (189 samples).
- `sweep_runner.sh` (this dir) now does canary -> R5 baseline -> sweep under a
  35%x3 auto-aborting watcher -> after-checks, and it works; reuse it for future sweeps.

---

# Combo landed: f7ef22e. Gate policy CHANGED (user: "Is this probe sweep really needed every time?")

- `AeComboBox` → `Controls.ComboBox` (in-scene popup by G37, all keys ruled from the contract,
  Q6 type-ahead decided YES and recorded in behavior.md item 39, Tab+list-open decided and
  recorded in item 37). Targeted probes format/quality/axes/dialog 4/4 PASS in 446s.
- **New standing gate policy (agreed 2026-09-29):** full 13-probe sweep ONLY for changes that
  touch what only sweeps see — a11y factory, shell/dialog wiring, harness/platform-theme — and
  at rung milestones (next full sweep: R1 milestone). Everything else: always-on gates
  (builds ×2, suites ×2 incl. media_exporter_e2e, catalogs, geom/AE) + the 2–4 probes that
  drive what the change touches (grep the probes for the control names; skip the canary when
  none opens an OS file dialog).
- R1 remaining: `ScrubSpinBox.qml:4-6` false header + F31/F33, `AeSectionHeader.qml:15-16`
  false header (both comment-truth fixes, cheap). Then Q1/Q2/Q5/Q7/Q8/Q9 + Discord I43/I44
  + the QQuickWidget-vs-QQuickWindow host question (blocked on Wayland popup measurement).

---

# Plan after R1 (drafted 2026-09-30, orchestrator; Q1–Q9 decided and committed 386eeec)

Each increment: subagent implements + immediate verification → orchestrator reviews diff/evidence,
runs always-on gates, targeted probes (grep the probes for the touched controls), commits. Every
behavioral line needs a captured observation or a regression-failing test (`tests/` offscreen
pattern, e.g. `picker_keyboard.cpp`).

1. **Escape semantics (B10 + F34) — DONE 2026-09-30, commit `e91bab6`.** Picker: Escape clears the
   search field first, closes the picker only when it is already empty. Numeric: Escape restores the
   value from when THIS edit began (not dialog-open). Measure first:
   `ScrubSpinBox`'s `cancelEdit` already keeps `root.value`, which is usually the edit-start value —
   verify against a live `changed` mid-edit and the AeNumberField sentinel; argue nothing.
   Files: `DestinationPicker.qml`, `ScrubSpinBox.qml`/`AeNumberField.qml`. Tests: extend
   `tests/picker_keyboard.cpp` + a numeric-escape offscreen test. Probes: dest, inputs.
2. ~~**Tooltip set (E29 + item 35 + E27/E28 placement, one increment).**~~ **DONE 2026-09-30** —
   see the notes section "the tooltip set is two kinds…". Set = 5 Reset affordances + truncated
   picker-row labels only (showTip gate per call site); below-first + flip + clamp in AeToolTip;
   E26 pinned by asserts; suites now **18**. Judgment flag for the user: the manual button's
   explanatory tooltip was DROPPED per the decided set (restore = one instance + the flag).
   Routed to increment 3: the picker list won't shrink below content width (labels cut, not
   elided) — check the same on the form side. Original plan text follows.
   Exactly two kinds survive:
   picker-row tooltips that carry the full text of a truncated label (rows incl. the manual row only
   if its text can actually truncate — measure what `manualTooltip` says before deciding), and the
   icon-only Auto/Reset buttons, whose tooltip is "Reset to default" (wording along those lines,
   already `qsTr`-able). Everything else is dropped (e.g. any numeric-hint tooltip). Reset buttons
   must be icon-only (verify `AeNumberField:105-108`). Placement work the host measurement added
   (2026-09-30): E27 below-first (stock `Controls.ToolTip` places ABOVE always — delegate needed;
   `popupType=0`, in-scene) and E28 flip/shift-INSIDE clamp (real `AeToolTip` has none — measured
   114 px clip at the window edge; user ruled either resolution allowed, host verdict picks
   clamp-inside). E26 timing (b8aa66a) is untouched. Probes: dest + the reset driver.
3. ~~**Overflow layout (D25/Q9).**~~ **DONE 2026-09-30, commit `892b492`** — 2-D Flickable + pinned
   footer (`form_layout` suite = #19); three flagged choices for the user's final review: themed
   scrollbar (D23/24 by analogy), root-level key paging, screen-clamped window floor. Two QML traps
   recorded in AGENTS.md (`.parent` not `parentItem`; bindings can't track `mapToItem`). The routed
   labels-cut finding does NOT reproduce on the form. Original plan text follows.
3b. **C21 Add-range hover flash** (inserted 2026-09-30 when the run resumed — this plan predates
   the goal's 7-increment numbering; goal items (4)-(7) = plan items 4-6 below). **DONE 2026-09-30,
   commit `63f597e`** — reproduced (deterministic per-frame + 30fps real-pointer), root-caused
   (ColorAnimation lerps straight RGBA; `"transparent"` = `rgba(0,0,0,0)` carries black), fixed
   (ghost rest fill = hover RGB at alpha 0), gated (`hover_parity` suite #20; the `grabToImage`
   instrument is blind to this class — recorded in AGENTS-grade notes).
4. **Persistence (C17 + A5) — IMPLEMENTED 2026-09-30, pending the full sweep + commit.**
   Headline finding: section rows carried NO id (remembered state would have been row numbers!);
   fixed `section.id = g.id`, proven across a reversed table. C17 = `picker/expansion` keyed by
   platform id (absent = collapsed; preselection invariant preserved — FLAGGED). A5 =
   `exportdialog/size` (export dialog ONLY — FLAGGED; picker size = 5-line copy if wanted).
   `persist_state` suite #21 (27 checks; deliberate breaks fail 2/1/3 exactly). "Not settled 3"
   CLOSED (behavior table in the notes). Suites 21/21 ×2, probes dest/dialog + canary clean;
   full sweep running (shell-wiring trigger). Original plan text follows. Groups start collapsed;
   expansion state persisted across sessions (settings mechanism exists: `ctrl/System.cpp`,
   `gui/System.cpp` QSettings usage — reuse, don't invent). Dialog size persisted; first open uses
   the designed default. Probes: dest (expansion), dialog (size). Unit-test the state mapping.
5. ~~**I44 codec schema**~~ **DONE 2026-09-30, commit `c28c73b`** — `codecs=` + `default=` replace the
   fixed|ladder mode; row offers accepted ∩ build-capable per format; `request.codec` reaches
   `planExport` (MainWindow's 4 lines). Zero string changes (151 up to date). Three fact-constrained
   deltas + placement + hvc1-tag nuance flagged for the user batch (notes have the table).
6. ~~**I43 Discord cap**~~ **IMPLEMENTED 2026-09-30 — the cap asks first** (commit at the final
   sweep). Opt-in "Size cap (MB)" field (Auto sentinel idiom + icon-only "Reset to auto", Advanced
   per item 42), fits under the USER's number only; guidance stays a stated FACT; at-limit refusal
   kept verbatim (flagged). Findings: the audit generalizes to Telegram/WeChat caps (flagged);
   `AeNumberField` a11y/scrub `Math.round` flattened 0.25 to 0 (fixed + `scrub_escape` check); the
   old probe's fit cases were VACUOUS (20-frame GIF = 142 KB under every cap — fixed with heavier
   ranges + a silent-enforcement detector case; `probe_capped` edits justified vs contract 43).
   ONE new source string ("Size cap (MB)"); catalogs deliberately NOT lupdate'd (a full-tree run
   churns 2151 lines/catalog + surfaces a 54-string pre-existing extraction debt — its own
   maintenance finding for the user).

~~Blocked (needs the user): host question QQuickWidget vs real QML window~~ — **DECIDED 2026-09-30:
keep the `QQuickWidget` host** (see the run log below and the notes' "host question" section).
Reopen condition: wanting tooltips that overhang the dialog (window-backed) — the only measured
behavior needing a real-window host on Wayland.

R1-milestone gate before any of the above: full 13-probe sweep via `sweep_runner.sh` (announced;
canary → R5 → sweep under 35%×3 watcher → after-checks).

---

# Run log 2026-09-30 (session resumed from HANDOFF.md)

- **`6739d17c` was abandoned, not landed** (tree clean at `f7ef22e`, reflog silent, dangling commits
  unrelated). Its `scrub-spike/` harness and the `before/` capture set survived and were reused;
  `run.sh` gained jiggle + leave/re-enter states. Redone by a fresh implementation agent.
- **Committed `596dc19`**: F31 (I-beam in edit mode — MouseArea mode-binding + editInput
  HoverHandler; SizeHor kept for scrub), F33 (`theme.selection`/`theme.selectionText` — the only
  pair `QmlTheme` exposes; `Colors::textSelection` exists but is unexposed), both false headers
  rewritten after measurement (Label-vs-Text probe; SpinBox inventory citation).
  - Orchestrator review caught nothing missing: independent frame-level check (my own crops from
    the full captures at scene.qml-derived coordinates) = edit I-beam + purple selection (new) vs
    SizeHor + navy (old), rest SizeHor (new) — matching the agent's table exactly.
  - Gates re-run by the orchestrator: builds 0 warnings ×2 (artifacts postdate the edit; note the
    Ninja Multi-Config restat quirk — `cmake --build` runs 2 automoc edges then re-plans, `-n`
    dry-runs misleadingly show 37 pending), suites **16/16 ×2** exit 0, catalogs 0.
  - Traps now in the notes: ImageMagick `#RRGGBBAA` vs Qt `#AARRGGBB`, 16-bit AE inflation (compare
    at `-depth 8 -type TrueColor`), subpixel-AA glyph pixels defeat selectionText sampling.
- **Q1–Q9 all decided by the user** (answers verbatim in contract commit `386eeec`); the post-R1
  plan above was written from them.
- **R1 code complete** → milestone sweep `sweep_runner.sh` launched (R7 announced), job `bash-196`;
  result lands in `sweep-run/result.txt` (sweep_rc=0, portal=0 is the pass condition).
- **R1 MILESTONE SWEEP PASSED (2026-09-30 12:30–13:04, ~34 min)**: **13/13 probes PASS**
  (`pass=13 fail=0 stale=0`), canary PASS (dialog on Xvfb, journal clean), 0 portal lines,
  cosmic-comp main thread 0–7% over 190 during-samples (abort threshold never approached),
  after-samples 1/1/2%. `sweep-run/result.txt`: `sweep_rc=0 portal=0`. R1 is DONE — the next work
  is the post-R1 plan above.
- **POST-R1 PLAN COMPLETE (2026-09-30) — all seven increments landed.** Commits: `e91bab6` (B10/F34
  Escape), `9abcea2` (E29/E27/E28 tooltips), `892b492` (D25/Q9 overflow), `63f597e` (C21 ghost
  fill), `9075bbc` (C17/A5 persistence), `c28c73b` (I44 codec schema), + I43 landing at the final
  sweep. Suites 16 → **21**. Two maintenance findings for the user: the 54-string .ts extraction
  debt (lupdate deliberately not run) and the stale `NumberFieldAccessibility` comment.
- **Flagged choices for the user's adjudication (all reversible, all in the notes)**: (1) E27
  placement = below-first (item 30 read as z-order); (2) manual-button tooltip dropped (E29 set);
  (3) form scrollbar (D23/24 by analogy — not contract-required); (4) root-level key paging; (5)
  screen-clamped window floor (D25 "small screen"); (6) A5 covers the export dialog only; (7)
  first-open: a remembered destination's platform opens (preselection invariant beats "all
  collapsed" literally); (8) I43 generalizes: Telegram/WeChat caps also stop auto-enforcing; (9)
  cap unit = MB (0.25 = 256 KiB expressible); (10) I44: codec row stays in Encoding (item 42's
  "under Advanced" strict reading = one QML move); (11) I44: three fact-constrained data deltas (X,
  YouTube@Lossless, archive@non-lossless); (12) hevc-into-mp4 rows lack the `hvc1` tag (facts
  silent; one attribute per row to fix).
- **FULL SWEEP at increment 5 (shell-wiring trigger), 22:37–23:12**: **13/13 PASS**
  (`pass=13 fail=0 stale=0`), canary clean, `portal=0`, cosmic-comp 2–3% over 190 samples,
  after 2/2/1%. `probe_reexport` (the within-run-reopen candidate) passed — persistence is
  invisible to the probes as flagged. `sweep-run/result.txt`: `sweep_rc=0 portal=0`.
- Flagged choices accumulating for the user's final review: form scrollbar (D23/24 analogy),
  root-level key paging, screen-clamped window floor (all inc 3); manual-button tooltip drop (inc
  2); E27-placement reading (inc 2); A5 export-dialog-only scope + preselection-opens-platform
  first-open semantics (inc 5).
  tests (`picker_keyboard` extended — driven through a real window; `scrub_escape` new — suite
  count is now **17**, `run_suites.sh` auto-discovers from `add_executable`). Deliberate breaks
  caught by exactly 7 and 4 assertions. Gates: builds 0×2, suites 17/17 ×2, catalogs 0, probes
  dest/inputs/axes/quality **4/4 ×2** (agent + orchestrator). Routing facts measured (escape-spike):
  direct-send never bubbles, window delivery does; specific Keys handlers auto-accept.
  - **Harness lesson, committed `7a58bc7`**: the portal canary false-FAILs on the harness's own
    `[systemd-run]` command echoes (journal logs command TEXT — a grep for "file_chooser"
    contaminates its own window). Fixed canary_dialog.sh + sweep_runner.sh + AGENTS.md recipe with
    `grep -v '\[systemd-run\]'`. Also owned: the agent's job script continued to probes after a
    canary FAIL instead of stopping — harmless (all runs stayed in Xvfb), fixed behavior going on.
- Post-R1 increment 2 (tooltips) dispatched next.
- **HOST QUESTION MEASURED AND DECIDED (2026-09-30, user-authorized live Wayland run)**: in-scene
  placement is identical in both hosts AND both platforms (Wayland-native selftest logs: combo
  popup flips above the bottom edge fully in-bounds = G38 ✓; `AeToolTip` edge tip overhangs 114 px
  and clips — no clamp anywhere; stock `Controls.ToolTip` = in-scene `popupType=0`, ABOVE its owner
  always). E28 wording ruled by the user ("the design here allows for either. This is nuanced.") →
  either resolution satisfies the line, nothing requires window-backed tooltips → **keep
  QQuickWidget** (full reasoning in `docs/export-dialog-implementation-notes.md`, "The host
  question, decided"). Remaining X11-only evidence: Wayland input-path semantics (option B: rerun
  with pointer acceleration disabled — not needed for the decision). Disclosures: one driver bug
  injected ~18 synthetic events near the screen's left edge early in the run (targets computed from
  a polluted capture; fixed with an input guard), and one ~5 s spike window leaked onto the
  XWayland desktop during a mechanics check (zero input). Evidence: `host-spike/` + `out/`.
- **FINAL MILESTONE SWEEP FAILED (2026-10-01, 10:5x-11:40) — and earned its keep**: 11/13 PASS;
  `sweep_rc=1 portal=1`. Triage: (1) **REAL REGRESSION** — `probe_still`'s numbered-PNG SEQUENCE
  writes 0 of 20 files (single-still JPEG passes = image writer fine; the multi-file path is dead).
  Suspects: `c28c73b`'s "Image artifacts skip codec resolution" or I43's fit-gating path switch.
  (2) **harness budget** — `probe_capped`'s asserts ALL pass but `run_probes.sh`'s `timeout 900`
  kills it mid-default-case (the honest 72/100-frame fit cases outran the budget the vacuous
  20-frame ones fit in). (3) **portal=1 = AMBIENT user activity**, not a leak: the journal
  neighborhood shows `vicinae` clipboard + Wayland portal traffic at 11:23 (the Id(1064) signature
  also appeared at Id(872) with no agent work running); canary green at sweep start; our runs are
  Xvfb + pinned theme. Recipe note added to AGENTS.md (the check cannot attribute lines to a
  process). Fix agent dispatched (sequence regression + per-probe timeout override); I43 stays
  UNCOMMITTED until a re-sweep passes 13/13.
- **RE-SWEEP PASSED (2026-10-01, 12:5x-13:37) after the fixes: 13/13 PASS, `sweep_rc=0 portal=0`**
  (probe_capped finished in its new 1800s budget, probe_still sequence green, compositor <=11%
  over 268 samples, no ambient portal traffic this time). **I43 + both regression fixes committed
  as `54ecc23`.** THE POST-R1 PLAN IS FULLY LANDED: `e91bab6` `7a58bc7` `9abcea2` `892b492`
  `63f597e` `9075bbc` `c28c73b` `54ecc23` (+ contract `386eeec`, host decision `31cfb2d`).
  Suites 16 -> 21. Open items = the user-batch adjudication + the 54-string extraction debt.
- **BATCH RULINGS IMPLEMENTED (2026-10-01, commit at the closing sweep)**: archive rule =
  ffv1/av1/hevc/vp9 offered, toggle+bucket default (Lossless=FFV1; ON=VP9; OFF=AV1 Best / H.265
  Balanced+Smaller — Balanced-mapping FLAGGED to the user, one branch swap); **AV1 alpha MEASURED
  silent-fallback in this build** (svtav1 drops alpha silently; vp9/ffv1 carry it; x265 refuses) —
  the measurement prevented shipping a rule that silently destroyed transparency. Codec row =
  placement-only SPLIT (manual's row stays beside Format — probe_format/axes pin it; the offered
  destination-mode row moved to Advanced under the transparency toggle). New AGENTS.md trap: method
  CALL in a QML binding = no dependency (frozen binding); latent Quality-label instance left for a
  later one-word fix. Alpha probe: `adjudication/alpha_probe` (corrected once: libvpx decoder
  mis-call had falsely cleared VP9 — suite methodology now matches). i18n: 1 source string replaced,
  regenerated .h = 151 up to date, no lupdate (the 54-string debt stands).
- **CLOSING SWEEP PASSED (2026-10-01, 18:3x-19:13): 13/13, `sweep_rc=0 portal=0`, portal clean,
  compositor calm.** Batch rulings committed. THE RUN IS COMPLETE: plan + rulings + two sweep-caught
  regressions fixed + two harness lessons + one QML trap. Branch `feat/export-ui-redesign` at the
  adjudication commit on top of `af7b536` (contract) `54ecc23` (I43+fixes) `c28c73b` `9075bbc`
  `63f597e` `892b492` `9abcea2` `7a58bc7` `e91bab6` `386eeec` `31cfb2d`. Leftover for the user:
  the 54-string .ts extraction debt, the Quality-label latent binding trap (one word), the
  Balanced-mapping confirmation, and the hvc1-tag nuance.
- **2026-10-03 BATCH CLOSED — all three leftover rulings implemented and committed as `b686a83`**
  (13 files, +566/−52, tree clean). (1) **AV1 at every non-Lossless archive bucket with transparency
  OFF** (user: *"Why would the ladder pick H.265? AV1 all the way makes more sense."*) — the
  `fastFirst` family demotion is deleted from the Archive block, H.265 survives only where no AV1
  encoder is usable. (2) **Quality row follows the ARTIFACT, not the mode** (*"show only the control
  that governs the file"*) — and in fixing the predicate the latent 2026-10-01 trap was fixed for
  real: `exportModel.manualMode()` at `ExportDialog.qml:511`/`:516` was a method CALL in a binding,
  which registers no dependency AND threw `TypeError` (24 per run) so `visible` never got written and
  the row drew in BOTH modes; replaced by `Q_PROPERTY qualityVisible = !imageQualityVisible`.
  (3) **`hvc1` whenever HEVC lands in mp4/mov** — optional `codec=` qualifier on `<codecTag>`,
  honoured only when the plan RESOLVED to that family. Measured en route: the mp4 muxer *rejects* an
  alien `codec_tag` (`Tag hvc1 incompatible with output codec id '27' (avc1)`, 13×, container open
  fails), so an unconditional tag is not an option; the H.265 leg must be forced by pinning
  `libx265` (this build resolves AV1 by default). Wayland input-path evidence stays DEFERRED by name.
  **Re-run of the gate on the committed tree, 2026-10-03 11:39–11:49**: a deliberate `touch` of the
  seven changed sources + `cmake --build --config {Debug,Release}` = **85 edges each, 0 warnings,
  0 errors, both link** (the earlier gate's "0 warnings" was a NO-OP build and was recorded as
  vacuous — now measured); suites **23 PASS / 0 FAIL rc=0 ×2** on the freshly linked binaries;
  `probe_format` **25/25 rc=0 STALE=0**; catalogs 0 unfinished ×6; `portal=0`, no stray
  Xvfb/AnimeEffects. Logs: `/tmp/recompile_{Debug,Release}.log`,
  `/tmp/reverify_suites_{Debug,Release}.log`, `/tmp/reverify_probe_format.log`.
  **Left for the user: only the 54-string .ts extraction debt (item 4) and the host reopen condition
  (item 6).**
