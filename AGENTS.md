# Working notes for coding agents

The conventions, traps and verified commands that cost real time on this repository. **Git history
holds what changed and why**; this file holds only what you need to work without rediscovering it.
Every line was measured by running it, unless it says otherwise.

## Building

The tree is **Ninja Multi-Config** and needs **Qt 6.12** (Canvas Painter is a hard requirement):

```bash
rm -rf build && cmake -S . -B build -G "Ninja Multi-Config" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64" -DAE_FFMPEG_GPL_AVCODEC=/usr/lib64/ffmpeg/libavcodec.so.62
cmake --build build --config Debug -j"$(nproc)"
```

- **Build ONE configuration at a time.** Ninja Multi-Config writes one `.ninja_log`/`.ninja_deps` per
  tree; two concurrent `cmake --build` runs (Release and Debug) corrupt the autogen and produce, among
  other things, `qtopenglwidgetsexports.h: source file is not valid UTF-8` on a fine Qt header — with
  the target silently keeping its old binary.
- **A single-config build FAILS at the link**: the binary would land at `src/gui/AnimeEffects`, which
  is the QML module's output directory (`.../AnimeEffects/Dialogs/...qmltypes`).
- Reaching another configuration: `cmake --build build --config Release`, or
  `ninja -C build -f build-Release.ninja all`. `ninja -C build all:Release` does not exist.
- **`ninja`'s own state can corrupt** (`stl_vector.h: operator[]: Assertion '__n < this->size()'`):
  `rm -f build/.ninja_deps build/.ninja_log`. After any build whose effect you cannot see, check the
  binary's mtime or `strings <bin> | grep <new string>` **before** instrumenting anything.
- **`cp -a` / `rsync -a` INTO a tree that has a build dir keeps the sources' OLD mtimes, and ninja
  trusts mtime**: a header change that arrives older than the objects is never compiled, and the
  resulting binary mixes two layouts of the same struct. Measured: `MainDisplayWidget::requestFrame`
  segfaulted in `QTimer::isActive` after four strokes because one object still had the pre-smoothing
  `ViewportInput`; a clean rebuild of the same sources survived 12 rounds. `touch` the copied files
  or build `--clean-first`.
- **A `-W` warning is only absent from the run that COMPILED that TU.** An incremental build can miss
  warnings in files it did not rebuild; `touch` the files and rebuild to ask.
- Do not `dnf remove 'qt6-*'` to "remove the old Qt": 80 packages depend on it, including the KDE
  stack and the desktop's platform theme.

## Verifying

- `scripts/run_suites.sh Debug` — the whole headless suite. It discovers targets from
  `tests/CMakeLists.txt` via `add_executable(`, so registering a target is all it takes to gate it. A
  target the build system knows but did not produce is a **failure**; one the build system does not
  know is a skip.
- **While iterating, run the gate for the unit you touched**, not the suite; the suite is for the end
  of a coherent change. Focused gates are named after their source in `tests/`.
- GL/Canvas gates need a display: `xvfb-run -a ./build/tests/Debug/<target>`.
- `AE_FRAMEBUFFER_DUMP=<prefix>` makes the viewport write its own **scene FBO** to `<prefix>-N.png`
while that FBO is bound and just rendered — the reliable way to see what the application drew on a
session this harness cannot screenshot. A timer cannot do it (no current context → black) and
`grabFramebuffer()` comes back sheared on a fractional DPR.

`scripts/gui/run_probes.sh` runs every `scripts/gui/probe_*.sh` (globbed — no registration). Run a
  single probe directly while iterating.

## Seeing what the application prints

Qt does **not** send `qDebug`/`qInfo`/`qWarning` or QML `console.*` to stderr when stderr is a file or
a pipe — its default handler writes to the **systemd journal**:

```bash
QT_FORCE_STDERR_LOGGING=1 ./build/src/gui/Debug/AnimeEffects project.anie > /tmp/app.log 2>&1
QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES="qml.debug=true" ./...   # console.log is `qml`/debug
journalctl --user --since "5 min ago" | grep -i <what you are looking for>
```

Measured, same program: no env → 0 lines; with `QT_FORCE_STDERR_LOGGING=1` → 1 (the `console.info`);
with it plus `qml.debug=true` → `console.log` too. Do not set `*.debug=true` routinely: Qt's
font-database and pointer chatter buries your output.

## Driving the GUI headlessly

**First choice: `scripts/gui/gpu_headless.sh -- <command>`** (cage + wlroots headless) — an Xwayland
whose GL is the real GPU. **Xvfb is the fallback** and its GLX is llvmpipe by construction, so any
pixel or timing taken there is software. Cage is a **kiosk**: the command must BE the presented client,
there is no WM, and no keyboard focus. `grim` captures it.

For Xvfb, the app needs xcb and Wayland must be off:

```bash
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=generic
```

Both lines matter. Declaring `DISPLAY=:77` alone does **not** help: Qt prefers Wayland whenever
`WAYLAND_DISPLAY` is set, so the app quietly opens on the real desktop. And the platform theme must be
pinned: a session that sets `cosmic` makes a file dialog open through `xdg-desktop-portal` **on the
live session** while the probe waits inside Xvfb for a dialog that is not there. Only a theme with no
portal strings works (`generic`). Neither `QT_NO_XDG_DESKTOP_PORTAL=1` nor unsetting the theme helps.

**`scripts/gui/_lib.sh`** provides the sandbox: `wait_ready`, `ae_guard_display` (exits 97 unless an
Xvfb serves the current display), `ae_memory_cap`, `ae_size_window`, `ae_press`, `ae_activate`,
`ae_report`, `assert_showing`/`assert_hidden`, the settings sandbox, and it **unsets `DISPLAY` and
`WAYLAND_DISPLAY` in the OUTER shell** so a leaked command cannot reach the desktop. `scripts/gui/`
also holds `ae_ui.py` (the AT-SPI client: `click`, `set`, `value`, `text`, `report`, `wait`,
`extents`, `focus`, `dump`, `AE_UI_ANY=1` to include items reported hidden) and `ae_pix.py` (colour
scans: `line`, `moved`, `inkcols`, `rowruns`).

Traps that have each cost a run:

- **Keyboard input needs a window manager.** Without one the window never activates and every key is
  dropped; run `openbox` inside the display and target the window with `xdotool getactivewindow`.
  Coordinate clicks and AT-SPI actions work with no WM.
- **Never guess coordinates.** Use the widget's own geometry (`xdotool getwindowgeometry --shell`); a
  click outside the window takes focus away and looks exactly like "keys do not work".
- **A fixed `sleep` is not readiness.** The app is ready in ~4 s idle, >25 s under load, and a probe
  that slept started driving a 200x71 splash. `wait_ready` polls; and note that `wait_ready` does not
  mean *your* project has loaded — the GUI opens `assets/sample.psd` first.
- **One instance at a time, with a memory ceiling.** `nene.anie` is a 15000x15000 project (~112 image
  layers): one instance reaches ~10 GB resident in ~80 s and stays there. `wait_ready` starts
  `ae_memory_cap` (12 GB; `AE_MAX_RSS_KB` to raise). `_lib.sh` defaults to the **smallest** `.anie` it
  can find; `AE_PROJECT` / argv choose another. Never run a suite and a probe at the same time.
- **Nothing sizes the window under Xvfb** (no WM, fresh `XDG_CONFIG_HOME`): it maps at ~795x337.
  `ae_size_window` (called by `wait_ready`; `AE_WINDOW_SIZE`/`AE_NO_RESIZE`) fixes it by geometry from
  `xwininfo -root -tree` — `xdotool search --name AnimeEffects` returns the selection owner.
- **On a scaled session every AT-SPI-driven click misses**: accessible rects are **logical** pixels,
  `xdotool` moves **device** pixels. Multiply by the scale factor.
- **A dialog that opens mid-session** is "the window that was not there before":
  diff `xdotool search --onlyvisible --name .` before/after.
- **A killed app leaves a recovery copy**, and the next launch opens a modal "Recover unsaved
  project?" that swallows every click. Give probes a fresh `XDG_CACHE_HOME`.
- **A project that moved on disk makes the app start empty, silently** — assert the project is open.
- A Wayland-native window is invisible to X11 capture; a black frame means the wrong compositor.
- `pkill -f "<pattern>"` matches the shell running it: kill by PID (`pkill -a Xvfb`, `kill <pid>`).
- `import -window root` never contains the pointer; `ffmpeg -f x11grab -draw_mouse 1` does. To time
  something, record and compare frames, not one snapshot.

## Authoring probes

- **An apostrophe in a comment inside a `bash -c '...'` body ends the body there.** Everything after
  runs in the *outer* shell (with the developer's real `DISPLAY`); `bash -n` does **not** catch it.
  Grep the body for `'` after editing. When such a probe dies silently, read the app log for
  `The X11 connection broke` and then **count the argv words** (`tr '\0' '\n' < /proc/<pid>/cmdline`).
- **`ae set` writes through the AT-SPI Value interface**, which a `QLineEdit` does not have; it is
  refused and the probe carries on. Prefer a `QML` field's own marker, or a control that really is a
  value.
- **A panel readback is not the document.** A model can keep its own cached value and report the new
  number while the document kept the old one. Discriminators: an undo, or save + reopen.
- **Two runs of the SAME mode agreeing proves nothing.** The control must differ in exactly the thing
  under test.
- **A stale binary** (see Building) is the most common "it does not work".
- **`run_probes.sh` needs `PROBE_TIMEOUT` overrides** for a probe that legitimately takes longer.

## Where things live

| path | what |
|---|---|
| `src/core/` | document: `Project`, `ObjectTree`/`LayerNode`/`FolderNode`, `TimeLine`, `TimeKey*`, `CameraInfo`, `AbstractCursor`, formats |
| `src/cmnd/` | the undo stack; `cmnd::Gesture` is "one open edit"; every command is value-replayable |
| `src/ctrl/` | editors and tools: `Driver`, `TimeLineEditor`, `GraphEditor`, `ffd/`, `srt/`, brushes, `MaskEditor`, `FilterFrame` passes |
| `src/gui/` | widgets, hosts, docks; `qml/` holds the Qt Quick scenes and their view-models; `theme/` the tokens |
| `src/gl/`, `src/img/`, `src/util/` | GL wrappers, image/PSD I/O, helpers |
| `tests/` | headless gates, one file each, registered in `tests/CMakeLists.txt` |
| `scripts/gui/` | `_lib.sh`, `ae_ui.py`, `ae_pix.py`, `run_probes.sh`, `probe_*.sh`, `gpu_headless.sh` |

## Document and edit model

Every state the document holds must be reproducible by replaying the undo stack:

- `Base::undo/redo` rebuilds from the values the command carries — never from the editor that made it.
- An iterative solver that must not mutate the document works on scratch and commits one command
  (`ctrl::pose::BoneDynamics` is the reference).
- **One gesture = one command**, rewritten in place while `Stack::isModifiable` holds. `cmnd::Gesture`
  is that shape; `tests/cmnd_gesture.cpp` pins it.
- One undo step **per gesture**, not per sample: a drag rewrites its command.
- Format versions gate structures: never patch a header to claim a newer version on an older file —
  other structures are version-gated too (`tests/roundtrip_main.cpp` writes fixtures).
- **Proportional keys** store their weights (never re-derive them after each edit: frame quantisation
  drifts). A key on a *flat* value segment inherits the time fraction — otherwise the value axis is
  inert forever. Format 0.16 carries ids and bounds.
- Value edits go through `ctrl::proportional::ValueReflow` (capture before, apply/modify after,
  restore on undo), so a proportional companion follows a value change in the same undo step.
- Keys carry **one easing param per graphable component** (`src/core/KeyEasing.h`); a channel reads its
  own. Structural keys (pose/FFD) keep one shared easing.

## Viewport pointer input

`gui::ViewportInput` owns every pointer decision; the three mouse handlers build a `PointerSample` and
hand it over. Its one question is `Sampling`:

| gesture | sampling | consequence |
|---|---|---|
| move/rotate/scale, canvas pan, brush size/hardness dial | `Latest` | the sample is **held** and fed once per painted frame; the release lands the final position first |
| mask brush, bone/pose influence, FFD/mesh brush, **any pen** | `Every` | applied on arrival; a pen is always `Every` and is never held |

- **Qt COMBINES MOUSE MOTION BY DEFAULT and that is documented, not a bug**: on X11 and Wayland
  `Qt::AA_CompressHighFrequencyEvents` defaults to **true**, and `QEvent::MouseMove` is "combined
  whenever they occur more frequently than the application handles them". **Set the attribute to
  `false` AFTER `QApplication` is constructed** — the platform plugin sets it during construction, so a
  value set before is silently overwritten (`testAttribute` reads 1). Measured on one 40-position drag:
  **6 samples** with the default, **41** with it off. `Main.cpp` sets it and warns if it did not take.
- **A frame is the unit of expensive work.** The frame's duties, in order: feed a held sample
  (`ViewportInput::frameTick`), drain the editor's queued brush work (`Driver::frameTick` →
  `IEditor::flushPendingWork`), announce the mask stroke's damage, then blend once
  (`Driver::flushPendingBlend`). Input handlers never blend, render or force a paint.
- **The editor's own damage must not end its own stroke**: `MaskEditor::announcePendingPaint` sets
  `mAnnouncing`, and `updateEvent` ignores what the editor raised itself.
- A move with **no** gesture still feeds the tool (the brush ring follows the pointer).
- The drag's buttons come from the **cursor**, not from the event's `buttons()`.
- Gate: `tests/viewport_input.cpp` (fake host + a real `AbstractCursor`).
- **The brush size dial is `sqrt` - chosen by hand on 2026-10-31, not by the numbers alone.** The
  two laws were built and run side by side: "sqrt definitely works a lot better" was the verdict. One drag step
  is 8 screen px (`ctrl::BrushDrag`). sqrt moves `sqrt(radius)` by 0.25 per step, so the same hand
  travel buys the same EFFORT across the range: measured **+11.5 % at radius 20 (22.30)** and
  **+2.5 % at 400 (410.06)**. linear adds a flat 2 px: **+10 % at 20 (22)** and **+0.5 % at 400
  (402)** - it crawls exactly where the sqrt law is right. The old multiplicative law was x1.1 at
  both ends (x2.36 across the same drag at either size), which is the report that started this.
  The keyboard notch always keeps the ratio (`adjustSize`); `tests/tool_view_model.cpp` pins all
  four numbers.
- **The brush path is SMOOTHED by arc length, in screen pixels** (`gui::PointerSmoothing`, the SMA
  measured in `~/dev/input-smoothing-lab`): the mean of the last **D = 12 px** of the pointer's
  path, which cancels a 4-px staircase outright (12 px is three periods). `AE_POINTER_SMOOTH=<px>`
  sets the window, `0` disables it. It trails by **D/2 = 6 px of path** - ~60 ms at 100 px/s, ~300 ms
  at 20 px/s: the latency of a distance parameterisation is the speed's, not a constant. Rules, each
  measured: the window is in SCREEN px (a sample-count window changes strength with the event rate,
  a document-px one with the zoom); only a DRAWING PATH is smoothed (a pan's end state is not a
  path, and a hover feeds the ring unsmoothed); a PEN is never smoothed; and the RAW release sample
  lands after the last smoothed one, because the mean ends 6 px short of the nib and a stroke must
  end under it. `tests/viewport_input.cpp` pins the trail, the start anchor, the release and the
  hover.
- **There is no scissored "band" repaint.** One was built and deleted: with it engaged (5 scissored
  frames) a 40-position drag still delivered 6 samples of stroke, so it never addressed delivery, and
  its only effect was a mode that could leave parts of the surface unpainted.

## Rendering and hosts

- The timeline surface, the ruler and the graph page are **Canvas Painter** hosts (`QCanvasPainterWidget`
  through `PanelHostBase`); Qt ≥ 6.12 is required. The scene models live in `ctrl/` (they read
  `ctrl::TimeLineRow`), the ink decisions in `gui/`.
- **One ImGui context, one backend, one atlas.** Two hosts would need VAOs per context:
  `GraphPanelFrame` records the owning context and rebuilds its objects on a different one.
- `gui::Ink` is the drawing vocabulary (`InkImGui`, `InkQPainter`, `InkCanvas`); `composePanelFrame`
  builds a frame with no rasteriser in it, so two hosts cannot disagree about content.
- The ruler's band height, number rows, tick length and pill metrics are `static_assert`ed against the
  timeline's constants: change one, change both.
- **ImGui text**: `PixelSnapH` on every atlas source (a fractional advance blurs every glyph after the
  first). Text runs are placed by the engine's own top baseline, not a hand-derived ink offset.
- The timeline's **lanes never draw curves** (user ruling). Timeline key/lane/chip hues come from
  `theme::channelColor(type, component)` — one table shared by the timeline and the panel.
- 1px chrome (playhead, ticks, zero rule) is deliberately **not** anti-aliased and sits on integer
  columns; the coverage pass is for curves, handles, the marquee and the guides.
- The viewport overlay engine (`ctrl::GLCorePaintEngine`) has **no `drawPath`, no `setClipRect`**, and
  drops text unless a brush is set. Draw with points/lines/polylines/rects/ellipses/polygons/text.

## Masks and filters

- `core::FilterMask` is one 8-bit field in the node's content space, capped at **1024** texels on the
  long side; the neutral value is **per channel** (amount starts white, time offset starts black) and a
  neutral mask must cost **nothing** (`isPainted`, `filterValueIsActive`).
- A layer mask's overlay is a flat rubylith **over** the composite (`Kind_MaskOverlay`), not a tint; the
  thumbnail and tree chip show the matte itself in grayscale.
- The brush is one model (`ctrl/Brush.h`): `BrushStroke` walks spacing between samples and carries the
  remainder; `brushFalloff` is flat core + smoothstep, and its GLSL copy is injected into the shaders.
- `FilterActivity.h` answers "does this filter do anything" from the **blended value**, not from the
  presence of keys.

## Theme, fonts, text

- Colours come from `theme::Colors`; QSS substitutes tokens **by name** (`%1`-style placeholders collide
  with literal percentages).
- A rule drawn **on a floater** uses `floaterEdge`, not `hairline`: in the dark palette `hairline` equals
  the floater body (1.00:1). `hairline` is correct on `base` (dock sashes, tree guides, lane seams).
- QML uses the dock font explicitly (`uiFont` from `LocaleParam`); `Qt.application.font` is 9pt and is
  **not** the dock's 8pt. Do not set `renderType` to chase the widget inspector's look.
- Counts use Qt numerus (`%n key(s)` → "1 key", "2 keys"); the source string never prints `(s)`.
- A disabled/degenerate control must be asserted in **both** directions, and a binding that throws
  writes no value — keep the stderr check that catches `TypeError`.

## Translations (`assets/locale/*.ts`)

```bash
ninja -C build 'src/gui/update_translations:Debug'   # the :Debug / :Release suffix is REQUIRED
/usr/lib64/qt6/bin/lrelease assets/locale/translation_es.ts   # binaries are not on PATH
```

- lupdate renumbers every `<location>`: never hand-edit a catalog.
- `translation_en.ts` is the source-language stub: it holds only `numerus="yes"` messages. Its
  `unfinished` count is not debt.
- lupdate sees only files in `AE_I18N_SOURCES`; a `tr()` elsewhere is invisible **with no error**.
- A lookup in the wrong **context** returns the English source without error. The panel's context is
  frozen at `gui::ImGuiOverlay` although that class is gone: the context is a catalog key.
- `scripts/run_suites.sh` gates `unfinished` = 0, `check_tr_contexts.py` and
  `gen_export_targets_i18n.py --check`.

## Qt Quick scenes

- **A `Repeater`'s delegates are not in the root's QObject tree** — walk `childItems()` too.
- **`parentItem` is a C++ getter**: in QML it is `undefined`; walk the item tree with `.parent`.
- **QML bindings cannot track `mapToItem`**; and there is no `mapToScene` — use `mapToItem(null, ...)`.
- **A called `Q_INVOKABLE` registers no binding dependency** (use the property form).
- **A zero-height item still paints its children** — use `visible: false` to remove it from the paint.
- **A delegate's `required property int index`** is shadowed by a same-named component property.
- **`QQuickWidget::keyPressEvent` never calls the base** unless you do; with no active focus item it
  ends the key walk and kills every rebindable command.
- **A child drag inside a `ScrollView` loses the grab** — set `preventStealing: true`.
- **A scroll bar's thickness comes from the bar's `padding`**, not `contentItem.implicitWidth`.
- **A `modal: true` popup swallows the press on its own field.** `modal: false`.
- **`AeButton` has a 72px implicit-width floor**; three in a row overflow a 220px dock.
- **A tab-scoped header control must keep its box** (`retainSizeWhenHidden`), or the dock and then the
  window change size when tabs switch.
- **`onClosed`-time measurements**: a binding over `mapToItem`/`Window.height` evaluates once — measure
  in the open handler into plain properties.
- A locale/format default that hides a row must be asserted in both modes; a `TypeError` in a binding is
  silent.

## Object tree and lanes

- The tree's first `table cell` row is the top node; child layers appear only when the folder is
  expanded, and only a CHILD row gives the Properties board a target.
- The expander is the branch indicator at `(rowX - 12, rowY + rowH/2)` from the row's own rect; it
  toggles, and a double-click on the row collapses too — use single indicator clicks.
- A collapsed folder with keyed descendants gets a **summary lane**: its own ink, one rounded triangle
  per frame, fill-only, `TimeLineRow::summaryRect()`; the tree's row height follows the same rule.
- Row seams are **solid** (`trackEdgeColor` 70 %); key-type separators inside a row are **dashed
  4-on/4-off** (`trackSeperatorColor` 40 %). Do not make them look alike.
- A key is drawn at `kKeyDotRadius` but picked at `kKeyHitRadius`; a proportional key is a **ring**.

## Known-good conventions worth copying

- A dropped gesture is not an undo: keep the pushed command, drop only the transient state.
- Clearing state and repainting are two duties; a transient view that a drag repaints must ask for the
  repaint when it clears.
- A gate that excuses a region must still assert the region is **occupied on both sides**, or the excuse
  becomes the hole.
- Measured claims name their number; a claim you did not measure is marked as inference.
