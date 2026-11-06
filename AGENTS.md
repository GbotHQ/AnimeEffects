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
- GL/Canvas gates need a display: `xvfb-run -a ./build/tests/Debug/<target>` — but the display does
  not decide the renderer (MEASURED 2026-11-03: Xvfb's client GL is radeonsi here). A gate that
  measures pixels should run under `scripts/gui/gpu_headless.sh`, and a scripted comparison should
  assert the renderer instead of trusting the display.
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

**Default: `scripts/gui/gpu_headless.sh -- <command>`** (cage + wlroots headless) — an Xwayland
whose GL is the real GPU, and the choice for anything long or interactive. **Xvfb is the last resort
for pixels or timing**: MEASURED 2026-11-03, its client GL is radeonsi here too, so software GL is a
possibility, not a guarantee — the display does not decide the renderer. Use Xvfb ONLY when the
answer cannot depend on filtering, pixels or timing, and never assume the renderer: assert it with
`render_probe --require-gpu` in any scripted comparison. Cage is a **kiosk**: the command must BE the
presented client, there is no WM, and no keyboard focus. `grim` captures it.

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

### Parallel runs stall the desktop

A 12-way parallel pool of `render_probe` jobs hung the desktop: each allocates 16x-supersampled
internal buffers, so twelve at once saturate the driver and the compositor. Cap the pool at 2-4,
`nice` it, and prefer `gpu_headless` for anything long.

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
| `tools/render_probe/` | headless deterministic renderer: `render_probe --project x.anie --out dir --size WxH --frames a..b [--zoom s] [--offset px] [--metrics] [--require-gpu]`. Drives `ctrl::Driver::renderGL`, the viewport's own path, with no window and no event loop. **Default for any rendering, pixel or timing question: `scripts/gui/gpu_headless.sh -- <cmd>`**; `QT_QPA_PLATFORM=offscreen` is acceptable for pure logic/CLI runs but keep concurrent instances bounded. MEASURED 2026-11-03: offscreen, Xvfb and `gpu_headless` all report the same radeonsi renderer here, so the display does not decide the renderer. It prints `GL renderer: <renderer> (vendor, version)` and `--require-gpu` exits 4 on any software renderer (llvmpipe/softpipe/swrast/software): use it in every scripted comparison, never assume the renderer. MEASURED: for a uniformly scaled layer, GPU and software are PIXEL-IDENTICAL (same frame, same size, `cmp` clean); prefer `gpu_headless` for anisotropic footprints or timing. Use it instead of live playback whenever a rendering question needs frames: a software-GL run drops animation frames, so the motion disappears |
| `tools/mip_compare/` | the mip-chain study harness: builds every level five ways and measures each against the exact premultiplied area average |

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
- **The brush size dial moves `sqrt(radius)` by 0.25 per step.** One drag step is 8 screen px
  (`ctrl::BrushDrag`), so the same hand travel buys the same EFFORT across the range: measured
  **+11.5 % at radius 20 (22.30)** and **+2.5 % at 400 (410.06)**. Two alternatives were built and
  run beside it on 2026-10-31 - a flat +2 px per step (it crawls at the top: +0.5 % at 400) and the
  old x1.1 multiplicative law (too coarse at the top, too fine at the bottom, the report that
  started this) - and the hand picked this one. No switch remains: the selector used for that
  comparison was removed with it. The keyboard notch always keeps the ratio (`adjustSize`);
  `tests/tool_view_model.cpp` pins both ends of the range.
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

- **The layer sampler is explained, with its figure and its numbers, in
  `docs/design/renderer/layer-sampling.md`**: the two-tap, one-level reconstruction (the level from the
  footprint's minor axis, both taps splitting its major axis, eight fetches, no level blend) against the
  hardware's trilinear read, what each does, the measured cost, and why neither is anisotropic (the mip
  chain is isotropic by construction and the read uses `textureLod`, which bypasses the sampler's
  anisotropy). The footprint is the draw's own Jacobian from screen derivatives, so it follows a
  mesh/FFD/bone deformation the transform cannot see.

- **The texture-residency plan lives in `docs/design/renderer/texture-residency.md`** - measured
  numbers, the density metric and its anisotropy gap, the pyramid-as-authoring-data decision, the
  page-pool design, the staged path, moving FFD into the pass, and what the clipping pass can do
  with the same GPU-side measurements. Read it before touching textures, mips or residency. **Stage 3
  (paged layer textures) is built** (`gl::PagePool`, `core::VirtualTexture`, `core::ImagePyramid`,
  the `VIRTUAL_TEXTURE` shader variation): `AE_VIRTUAL_TEXTURE=0` is the full-resolution control,
  `AE_VT_STATS=1` prints the pool each frame, and the V1/P1/P2 suites in `verify_mipmap` are the
  gates. Clipper layers and **masks are paged too** (Stage 4): a mask has its own R8
  coarse chain, table and atlases (units 10..15), its own demand pass (`uUvFromScreen` computes the
  uv from the screen->content matrix), and its own `MASK_VIRTUAL_TEXTURE` shader variation, only
  ever set with `VIRTUAL_TEXTURE`. A pageable mask's levels live in `core::MaskPyramid` (captured
  once from the GPU chain, then patched by a stroke's box); `MaskKey::Data::virtualTexture()` is
  the pool resource, and the rubylith overlay pages with it (`MaskOverlayFrag.glsl`). The pyramid
  stores only levels 1 and deeper; level 0 IS the field (MX4). **A folder's matte pages too**: its
  pass is fullscreen, so its demand is a quad through the demand map rather than a mesh walk
  (`VirtualTexture::markScreenMaskDemand`), and it runs in the exporter's demand-only pass as
  well. Filter-lane masks (`core::FilterMask`) are the known residual. All of these share the
  sampler in `assets/shaders/LayerSampling.glsl` (`#include`d by the layer, clipper, overlay and
  folder-matte shaders; `gl::ExtendShader` expands includes before the variation pass);
  CL1/MA1/MX3/FM1/DR1 in `verify_mipmap` are the clipper, mask and demand-replay gates. See the
  doc's open list.
  **A repeated demand signature replays instead of drawing**: `markDemand` (and both matte forms)
  take a `DemandSignature` whose `positions` field is `LayerNode::mMeshRevision` - the skinning
  pass's own stamp, because the position buffer's id is stable while its values change. A still
  view's repaint (a pointer move, a matte stroke, a panel redraw) therefore walks no triangles and
  re-stamps the pages it last marked, which is what keeps them inside `kKeepFrames`; a camera pan
  or a playback frame still marks (the view matrix or the revision moved). `AE_VT_STATS` prints
  `marks=`/`replays=`, and `AE_VT_DEMAND=nodraw` measures the pass's setup against its geometry
  (measured: the geometry).
  Two sampling rules, each found as a whole-frame residual first: **the always-resident coarse tier
  takes the same two-tap reconstruction as the full path** (one bilinear tap there cost 816 px over
  8/255 on nene), and **the `lod <= 0` branch reads `textureLod(..., 0)` on BOTH paths, never
  `texture()`** - the branch is entered on the biased level, the unbiased footprint can still be
  minifying, and the hardware's own LOD would then blend mips (and filter anisotropically) that a
  page stack cannot reproduce (measured on test.anie as 6873 px, mean 0.076, max 43, until both
  paths read level 0 explicitly).

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

- `core::FilterMask` / `core::MaskKey` are one 8-bit field in the node's content space at the
  **content's own resolution** - one texel per content pixel, a quarter of the layer's RGBA image.
  The 1024-texel cap they used to carry was an unforced optimisation (a 15000 px canvas became a
  15 px grid); no release shipped masks, so nothing is kept compatible, and `kMaxStoredTexels`
  (32768) is only a sanity bound against a corrupt file. The neutral value is **per channel**
  (amount starts white, time offset starts black) and a neutral mask must cost **nothing**
  (`isPainted`, `filterValueIsActive`).
- **A mask is NOT "low frequency"**: hard edges and painted texture are legitimate, so a mask needs
  the same sampling treatment as an image (mips) and its pyramid must be updated INCREMENTALLY from
  the stroke's dirty box, level by level, not regenerated whole. Any note that called masks soft
  was wrong. The sampled read is now the same two-tap reconstruction as the layer
  (`sampleMask`): the old `texture()` was the hardware's implicit LOD on BOTH paths, and a page
  stack cannot reproduce it - so the full path changed too, by design.
- **A mask's chain is `CLAMP_TO_EDGE`, not border colour**: `MaskMipChain::create` sets it, and a
  paged mask must mirror it exactly - `MaskPyramid::fillPage` repeats the level's edge texel in
  the gutter and `sampleMask` clamps each tap's texel position. A zero gutter would darken the
  field's outermost texels against the full chain's clamp. The mask copies its field's *content*
  pixels, so its levels and the layer image's are separate pyramids (`MaskPyramid`, span-1 CPU
  boxes) even when their sizes match. The pyramid does not copy level 0 - that is the field the
  mask's Data owns, and the page fill reads it in place (a 4096² mask's levels are 5.33 MiB, the
  full copy it used to keep was 21.33).
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
