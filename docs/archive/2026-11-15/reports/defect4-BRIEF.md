# Defect 4 — "why do so many targets open below the project size, and why does Reset to native not reset?"

Read-only audit. Tree: `/home/gambot/dev/AnimeEffects`, working-tree HEAD **`ab4ca9c`** (HANDOFF quotes `ff37603`; every line number below was re-read verbatim in this tree, not copied from notes).
Native project used for every measurement: **`assets/sample.psd` → 720 × 1172 @ 24 fps** (app log: `BG COLOR size = 720 , 1172`).
All measurements: fresh config sandbox, `QT_QPA_PLATFORM=xcb`, Xvfb 1440x900x24, `RECORDED_AGAINST=2`, `AE_UI_PID` pinned, real app, AT-SPI reads via `scripts/gui/ae_ui.py`. Raw dumps in `/tmp/defect4/raw/`, CSVs `/tmp/defect4/census.csv` + `/tmp/defect4/reset.csv`.

---

## 0. Three corrections to earlier claims (read this first)

**(0.1) The Part-4 counts I was asked to verify are wrong in both forms.**
- Broad pixel language (`NxN`|`px`|`pixel`|`resolution`|`wide`|`width`|`height`|`dimension`) absent from note+source: **`max` 15 of 32**, total **32 of 66**.
- Narrow (literal `NxN` only): **`max` 22 of 32**, total **50 of 66**.
- The single row that separates 15 from 16 is **`post.bluesky.animation`**, excluded only by the word "dimension" ("No dimension, frame or frame-rate rule is published"). Both numbers are defensible; neither is "16 non-pixel notes / 9 `max` rows".
- **"No comment claims a resolution-based rejection" is false.** Three `max` rows do: `post.x.gif`, `post.instagram.reels`, `post.weibo.gif` (§4).

**(0.2) The dominant cause of (A) is a mechanical ratchet, not the target boxes.** `ExportViewModel::writeSize` ends with `mUserSize = QSize(mWidth, mHeight);` — *after* clamping — at `src/gui/qml/ExportViewModel.cpp:660-661`. Every plan write therefore records the destination-clamped result as "the user's own size", and `planExport` bounds the next `Max` rule by the current field values (`src/ctrl/ExportTarget.cpp:1011-1015`). Within one dialog session the size can only shrink, and the amount depends on the **order** in which destinations were visited. Measured, reproduced to the pixel for all 10 rounds (§3.1).

**(0.3) Round 0 was not "persistence" and not "manual".** Round 0's destination was `chat.generic.video` ("A video for a chat app", the persisted `export_destination`), and 442 × 720 is pure `Max` arithmetic on 720 × 1172 (`0.614334`); the 442 there has nothing to do with `export_inputs`. The 442 in the sandbox config was written later (§2.3). I withdraw the earlier "disk proof of (A)(2)" framing — the disk proof stands, the round-0 attribution does not.

---

## 1. Cause table (A) — every way a target opens below native

### A1. `Max` boxes open below native whenever the box is smaller than the project on any axis — by design, silently
`src/ctrl/ExportTarget.cpp:650-658`:
```
case SizeMode::Max: {
    // Capped at 1.0: a MAXIMUM does not enlarge. ... and every one of the fifteen max-mode rows
    // did it. `exact` and `maxExactSide` are the modes that mean "make it this big" ...
    const double scale = std::min(1.0, std::min(aRule.w / sw, aRule.h / sh));
    return QSize(evenFloor(sw * scale), evenFloor(sh * scale));
}
```
`evenFloor` floors to even, minimum 2 (`:76-83`). The `//` block at `:651-655` is a comment — it is **never appended to `aNotes`**, and `exportFacts.cpp:211` is the only consumer of `plan.notes`. So the clamp produces **no fact row, no tooltip, no dialog**: it is completely silent. (The comment's own count, "fifteen", is stale: the file has 32 `max` rows.)

On a 720 × 1172 project a `max` row opens below native iff `w < 720 || h < 1172`: **21 of the 32 `max` rows** (19 bind on the height axis only, 2 on both). The 19 one-axis rows are exactly the 1280×720 chat/Discord rows and the 1920×1080 post rows; the 2 both-axis rows are `discord.emoji` (128×128) and `post.tumblr.gif` (540×540). The other 11 `max` rows open **at** native (scale saturates at 1.0).

Of those 21 rows, a *resolution* ceiling is actually documented for **3** (`post.x.gif` 1280×1080, `post.instagram.reels` 1080×1920, `post.weibo.gif` 1080 wide). The rest are byte/duration-capped or uncited: the pixel box is this application's own choice (§4).

### A2. The ratchet: the previous destination's output becomes the next destination's ceiling
- `ExportViewModel::writeSize` `:655-662` — `mUserSize = QSize(mWidth, mHeight)` at `:660-661`, after clamping.
- `buildRequest` `:1150-1153` — `request.requestedSize = QSize(mWidth, mHeight)` (the fields, not the project).
- `planExport` `:1006-1015` — `hasRequested` → for `SizeMode::Max`, `SizeBound` takes `std::min` of each non-zero rule bound with `requestedSize`, then `applySizeRule(bounded, aRequest.sourceSize)`.
- `applyDestinationDefaults` `:957-973` — when the previous target dictated the size, `writeSize(mUserSizeKnown ? mUserSize : mNativeSize)` hands the *recorded* value back to the fields before the plan reads them.

Net effect: `native → box ∩ previous output → re-fit to preserve ratio → always ≤ previous output`, so each switch shrinks. Reproduced exactly: 720×1172 → 442×720 → 442×718 → 78×128 → 78×126 → 76×126 → 78×126 → … (§3.1). Note the `Max` path also **ignores the aspect ratio the user set** in effect: the re-fit uses the *native* ratio, so the pair never returns to native once clamped.

### A3. Cross-session persistence of the shrunk value
- Write: `MainWindow::rememberExportInputs` `:1655-1673` writes `export_inputs/<name>/width` = `aModel.width()` (`:1670`) and `/height` (`:1671`) on dialog close.
- Read: `MainWindow::openExportDialog` `:1964-1969` — `if (w > 0 && h > 0) { init.width = w; init.height = h; }`.
- Effect: `ExportViewModel` ctor `:249-252` starts the fields at those values instead of `mNativeSize` (which `Init.width == 0` would have selected).

So the value that reaches the next session is whatever the boxes held when the dialog closed — including a value the destination clamped. On disk (`/tmp/defect4/sandbox-probe/config/AnimeEffectsProject/AnimeEffects.conf`):
```
[export_inputs]
untitled\fps=24
untitled\height=718
untitled\ranges="[[1,600]]"
untitled\width=442
[General]
export_destination=chat.discord.file
export_aspect_ratio=1
```
and the next open measured `dst_label=Discord, width=442, height=718, reset_enabled=1` — i.e. **the fields came back at the shrunk pair, not at 720×1172**. (One 2 px detail is unresolved: a full re-fit of 442×718 would give 440×718; see §6.)

### A4. `free` rows: no box, but not immune
`ExportTarget.cpp:632-633` returns the requested size unchanged, and `planExport:1008-1010` for `Free && hasRequested` returns `evenFloor(requested)` with no clamp. So a `free` row opens at native **only if nothing was carried in**. Measured: `chat.telegram.video` (`free`) opened at **720×1172** in a clean state (B4) and at **78×126** after six clamped switches (round 6). A `free` row is the cleanest demonstration that the shrink is *not* the destination's doing.

### A5. Rows that *are* small because the destination requires it (10 rows — legitimate)
- `exact` (8): `applySizeRule` `:635-648` returns the rule size and **does** append a note (`"Size forced to %1x%2 (required by this destination); the art is %3."`, padded/cropped). Measured: `discord.sticker` 320×320, `whatsapp.sticker` 512×512. Rows: `telegram.emoji` 100×100, `twitch.emote` 112×112, `wechat.sticker` 240×240, `qq.sticker` 300×300, `discord.sticker` 320×320, `kakaotalk.sticker` 360×360, `whatsapp.sticker` 512×512, `signal.sticker` 512×512.
- `maxExactSide` (1): `:660-663`, `scale = aRule.side / max(sw, sh)` — no cap, may enlarge; **appends no note**. Measured `telegram.sticker` → 314×512.
- `maxMinSide` (1): `:665-675`; **appends no note**. Measured `line.sticker` → 164×270.
These are the 10 rows where "small" is a real requirement; only the `exact` ones tell the user so.

### A6. `mSizeEditable` in this table
`applyPlan` `:1009-1012`: `sizeIsUsers = Free || boundsOnly` (`boundsOnly` = `Max`), then `mSizeEditable = sizeIsUsers`, and `mWidthMaximum`/`mHeightMaximum` are set to the box for `Max` (else `INT32_MAX`). Measured exactly: `max` and `free` rows → `width_enabled=1, reset_enabled=1`; `exact`/`maxExactSide`/`maxMinSide` → all `0` (r7–r10, B5, B6).

**Summary of scope on this project:** 21 `max` rows below native by silent clamp + 10 rows below native by requirement = **31 of 66 rows open below 720×1172**; the remaining 35 (24 `free` + 11 non-binding `max`) open at native only when nothing was carried in, and at a ratcheted value otherwise.

---

## 2. Cause table (B) — `Reset to native`

### 2.1 The button and the write path
- `ExportDialog.qml:420-424`: `AeIconButton { iconName: "arrow-clockwise"; label: qsTr("Reset to native"); enabled: exportModel.sizeEditable; onClicked: exportModel.setSizeToNative() }` — the size reset is the one immediately **before** `Keep aspect ratio` (the fps row has its own, identical `resetLabel`).
- `ExportViewModel::setSizeToNative` `:723-725` → `writeSize(mNativeSize)`.
- `writeSize` `:655-662`: holds `mWritingSize = true` across both field writes, so the aspect derivation at `:682-687` / `:704-709` **never** runs — the two values written are the raw native pair, each **individually clamped** at `:667-668` / `:697-698` to `mWidthMaximum` / `mHeightMaximum`.
- Consequence: the button can only ever be a *partial* reset when either maximum is below native, and it can restore neither the native size nor the native ratio in that case.

### 2.2 What happens in each of the five modes
| mode | maxima | native restored? | measured | note shown? |
|---|---|---|---|---|
| `free` (24 rows) | `INT32_MAX` | **yes** | B4 `chat.telegram.video`: 99999×162776 → **720×1172** ✔ | n/a |
| `max`, box ≥ native both axes | box | **yes** | B3 `post.instagram.reel`: 1080×1758 → **720×1172** ✔ | no |
| `max`, box < native on an axis | box | **no** | B1 `chat.discord.file`: **720×720**; B2 `post.x.video`: **720×1080** ✘ (native 720×1172) | **no** |
| `exact` | `INT32_MAX` | button dead | B6 `discord.sticker`: `reset_enabled=0`, value unchanged | **no** |
| `maxExactSide` / `maxMinSide` | `INT32_MAX` | button dead | B5 `telegram.sticker`: `reset_enabled=0`, value unchanged | **no** |

Measured `reset.csv` (columns `id, open w,h, capped w,h, reset w,h, reset_enabled, width_enabled, rc`):
```
b1,chat.discord.file,314,512,1280,720,720,720,1,1,0
b2,post.x.video,442,720,1920,1080,720,1080,1,1,0
b3,post.instagram.reel,662,1080,1080,1758,720,1172,1,1,0
b4,chat.telegram.video,720,1172,99999,162776,720,1172,1,1,0
b5,telegram.sticker,314,512,99999,162776,99999,162776,0,0,0
b6,discord.sticker,320,320,99999,162776,99999,162776,0,0,0
```
B1/B2 are the user's complaint, exactly: the button fires, the field changes, and the result is **not** the project's size — while a fact panel and a tooltip claim "Reset to native".

### 2.3 Every reason the button can be a no-op
1. **Disabled** — `enabled: exportModel.sizeEditable` (`ExportDialog.qml:423`), and `mSizeEditable = sizeIsUsers` is false for `exact`/`maxExactSide`/`maxMinSide` (`:1012`). Measured: `reset_enabled=0`; pressing it changes nothing; **no explanation reaches the user** (no tooltip, note, or error). `ae_activate` still returned rc=0 because AT-SPI found the node — that rc is the harness's, not product feedback.
2. **Clamped back** — `setWidth`/`setHeight` `:667-668`/`:697-698` silently truncate to the box (B1, B2). This is a *partial* no-op, which reads as "the button did nothing useful".
3. **The fields already hold the value** — a fresh project with a `free` target (nothing to change, no visible effect).
4. **Aspect-ratio write-back** — `writeSize` suppresses derivation, so a reset under lock writes the native **pair** but not the native **ratio** when a maximum interferes (B1 ends at 720×720, i.e. the lock the user has visibly *on* is violated). Editing a field afterwards derives the paired axis from native, i.e. the next edit re-derives a different pair than the reset produced.
5. **The reset restores against the wrong baseline in manual mode** — `clearPlan` `:1125-1132` writes `mUserSizeKnown ? mUserSize : mNativeSize`, and `mUserSize` is what `writeSize` last recorded (a clamped value, §A2), so in a loaded session even the un-targeted state cannot re-establish the project's size.

### 2.4 Why the user sees "nothing happened"
Nothing in the UI ever shows the destination's box. `exportFacts.cpp` prints only the resolved plan (`:105` `Output:` = `"<w> x <h>, <fps> fps"`), never the rule; the `Limits:` row `:202` carries byte/time caps only; `plan.notes` (the only note channel, `:211`) is **never** fed by the `Max` branch. So a clamped reset produces a smaller number with no stated cause — indistinguishable from a broken button.

---

## 3. Measurements

### 3.1 Rounds 0–10 (one dialog session; aspect lock **On**; native 720×1172) — `census.csv`
Every value below is reproduced by `writeSize(plan.size)` with the previous round's output as the current `requestedSize` (`planExport:1011-1015`); the arithmetic is shown so the mechanism is auditable, not asserted.

| # | destination | mode / box | measured | arithmetic |
|---|---|---|---|---|
| 0 | `chat.generic.video` (label "A video for a chat app") | max 1280×720 | **442×720** | min(1,1280/720,720/1172)=0.614334 → 720·s=442.32→442, 1172·s=720→720 |
| 1 | `chat.discord.file` (Discord) | max 1280×720 | **442×718** | bounded=min(1280,442),min(720,720) → s=min(442/720,720/1172)=0.613889 → 442, 1172·s=719.5→718 |
| 2 | `discord.emoji` | max 128×128 | **78×128** | bounded=128,128 → s=128/1172 → 720·s=78.6→78, 128 |
| 3 | `post.x.video` (X (Twitter)) | max 1920×1080 | **78×126** | bounded=78,128 → s=78/720 → 78, 1172·s=126.97→126 |
| 4 | `post.instagram.reel` | max 1080×1920 | **76×126** | bounded=78,126 → s=126/1172 → 77.4→76, 126 |
| 5 | *wrong row selected* — see §6 | — | 78×126 | not a Pinterest measurement |
| 6 | `chat.telegram.video` (Telegram) | **free** | **78×126** | requested pair passed through unchanged — a free target carrying a ratcheted value |
| 7 | `telegram.sticker` | maxExactSide 512 | **314×512** | s=512/1172 → 314.5→314, 512; fields+lock disabled |
| 8 | `discord.sticker` | exact 320×320 | **320×320** | exact × 2; disabled |
| 9 | `line.sticker` | maxMinSide 320×270 | **164×270** | {evenFloor(720·270/1172)=164, 270}; guard `max(out) < 270` false |
| 10 | `whatsapp.sticker` | exact 512×512 | **512×512** | disabled |

Raw field dumps (verbatim, 9 lines each: `dst_label`, `width`, `height`, `width_enabled`, `height_enabled`, `reset_enabled`, `keepar_enabled`, `keepar`, `fps`):
```
r0_manual.txt        dst_label=A video for a chat app  width=442 height=720
r1_chat.discord.file dst_label=Discord                 width=442 height=718
r2_discord.emoji     dst_label=Discord emoji           width=78  height=128
r3_post.x.video      dst_label=X (Twitter)             width=78  height=126
r7_telegram.sticker  dst_label=Telegram sticker        width=314 height=512  w_en=0 h_en=0 reset_en=0 keepar_en=0
r9_line.sticker      dst_label=LINE sticker            width=164 height=270  all disabled
r10_whatsapp.sticker dst_label=WhatsApp sticker        width=512 height=512  all disabled
```

### 3.2 Reset blocks
See the table in §2.2 and the raw `b*_open/capped/reset.txt` files. Notable raw facts:
- B1: `set Width 99999` → **1280×720** (width clamped to 1280, then the derived height 2084 clamped to 720 — the lock is *broken* by the clamp, and the pair is 16:9, not the art's ratio). Then reset → **720×720**.
- B2: → **1920×1080**, then reset → **720×1080**.
- B3: → **1080×1758**, then reset → **720×1172** (works — both box axes exceed native).
- B4 (`free`): → **99999×162776** (no clamp at all; ratio preserved to 5 decimals), then reset → **720×1172**.
- B5/B6: fields and button disabled; `ae set` still moved the values (the AT-SPI `set` bypasses `enabled:`; a real user cannot type there) and the button did nothing. **The `99999×162776` values in B5/B6 are a harness artifact, not a user-reachable state.**
- Persistence: after a `Cancel`, `export_inputs/untitled/{width=442,height=718}` on disk; reopening measured `dst_label=Discord width=442 height=718 reset_enabled=1`.

### 3.3 What was *not* measured
No screenshot was produced (`/tmp/defect4/shots/` is empty; the `import -window root` calls in the probe never ran), so there is **no pixel/geometry evidence** for any claim here — every number is an AT-SPI field read. `tests/export_form.cpp` pins *editability per destination* on a 1000×400 project, so no test pins the numeric defaults on a tall project like 720×1172.

---

## 4. Part 4 — what the data file actually claims (from `assets/export/ExportTargets.txt` only)

**No `<caps>` attribute expresses a resolution limit at all.** Attribute census: `maxBytes=` 49, `maxDurationSec=` 20, `maxFps=` 6, `maxFrames=` 5, `minDurationSec=` 1, `minFrames=` 1, `minScalePct=` 1. Every `max` row's `w`/`h` therefore comes from the mode field, i.e. from this application's model of the destination — it is never a documented rejection threshold.

`max` rows whose **note or source states a pixel/resolution ceiling** (3):
- `post.x.gif` — "resolution up to 1280x1080, up to 350 frames, at most 300 million pixels total (w × h × frames)".
- `post.instagram.reels` — "up to 1920 px wide in 9:16".
- `post.weibo.gif` — "原始图片需不超过1080宽，超宽会导致动图不动" (source width must not exceed 1080; wider stops the animation) — an explicit, documented pixel ceiling.

`max` rows whose note mentions pixel numbers **descriptively only** (the *target's* own maximum, not an upload rejection): `post.bilibili.video` ("up to 4096x4096, 120 fps"), `post.niconico.video` ("up to 4096x2160…"), `post.dailymotion.video` ("up to 3840x2160…"), `post.mastodon.video` (twice the upstream limit), `post.bluesky.animation` ("the circulated 720p/30 fps/5000 kbps numbers come from user comments, not documentation"), `post.tumblr.gif` ("540 px wide is Tumblr's own recommendation").

**Byte/duration-only rows with no resolution claim** include the ones the user is asking about:
- `chat.discord.file` (line 343, verbatim): "Sending 20 MB, 50 MB with Nitro Basic, 500 MB with Nitro; boosted servers raise everyone to 50 MB (level 2) or 100 MB (level 3). Discord's older help page still says 10 MB. On mobile the file size is now measured before compression, so a video that used to fit may no longer." — **bytes only; no resolution statement anywhere.**
- `chat.generic.video` (cap 20 MB; "720p or less, under 20 MB" is stated as what *every* chat app accepts, i.e. advice), `chat.telegram.gif` (2 GB), `chat.signal.video` ("the client re-encodes, ~2 Mbps, 720p" — downscaling, not rejection), `chat.messenger.video`, `chat.kakaotalk.video`, `chat.wechat.video`, `chat.qq.video` (unverified caps), `post.x.video` (8 GB / 20 min; 16 GB / 125 min Premium; the "140 s / 512 MB" figure is X's DM limit).
- Explicit **downscale** (not reject) wording: `:254`/`:258` (Twitch 112×112 and "Twitch will resize"), `:290` (WhatsApp downsampling). Explicit reject wording that is about bytes/time/animation, not resolution: `:66`, `:535`, `:671` (animated GIF), `:685` (oversized stickers come back with cropped edges), `:802` (Reels 3–90 s), `:897` (Vimeo, no alpha), `:968` (pixiv APNG), `:1098` (hvc1 requirement), `:1152` (AV1 silently dropped, H.265 rejected).

**Answer to "is the Discord 1280×720 a real upload cap?"** No. Nothing in the file, and no `<caps>` attribute anywhere in the file, states a Discord resolution limit; Discord's own note is a byte budget. The 1280×720 box is this application's own editorial choice, presented to the user as if it were the destination's requirement. The same is true of every `max` box except the three listed above.

---

## 5. Part 5 — design options for the orchestrator to put to the user

Facts the options must respect: (i) 10 rows have genuine, documented size requirements (`exact`/`maxExactSide`/`maxMinSide`); (ii) 24 `free` rows have no size requirement at all; (iii) of the 32 `max` rows, 3 have documented pixel ceilings, the rest are byte/time-limited or uncited; (iv) `Max` currently means "cap" in `applySizeRule` but "the fields are also capped, and the previous result becomes the next cap" in practice.

**Option (a) — the `Max` box becomes advisory; the fields default to native and are never clamped by it.**
- `Max` fields keep `INT32_MAX` maxima (like `free`), so the user can type anything and `Reset to native` always works.
- `planExport` keeps bounding by the box (`:1011-1015`), so the *export* is still within the destination's limit, but the mismatch is now visible and explained instead of silent: append a real note in the `Max` branch of `applySizeRule` when `scale < 1` (the text is already written in the comment at `:651-655`), and show the box in the fact panel ("Destination limit: 1280 × 720").
- Fix the ratchet at the source: `writeSize` `:660-661` must not record a *destination-dictated* size as the user's (`if (sizeIsUsers) …`), so the next destination does not inherit the clamped value, and `export_inputs` persists the user's number, not the clamp's.
- `exact`/`maxExactSide`/`maxMinSide` unchanged: still dictated, still disabled, still padded/cropped — and they gain nothing to explain because their note (`exact`) or their requirement is already stated; if desired, give `maxExactSide`/`maxMinSide` the same note.
- Cost: `Max` rows can now open at native even when the box is smaller, so the exported file is smaller than the box — which is what the user asked for in the complaint. Largest patch, and the one that matches the code's own stated intent (`ExportViewModel.cpp:997-1002`: "Max是上限… the user can export smaller").

**Option (b) — `Max` boxes stay a hard cap, but the UI explains it.**
- Fields keep being clamped and open clamped to the box (current numeric defaults: `min(box, native)`), so today's opening sizes are unchanged; the ratchet is still fixed by the `writeSize` change above (otherwise the same order-dependence remains).
- Add the fact-panel row ("Destination limit: …") and the clamp note, so "Reset to native" that lands on 720×720 is explained: the project is 720×1172 and this destination accepts at most 720 high.
- Smallest behavioural change, but keeps "the boxes do not show the project's size" — the user's first complaint stands as a design decision.

**Option (c) — leave the box semantics alone and only repair `Reset to native`.**
- Then a reset must beat the clamp: either temporarily ignore `mWidthMaximum` for the write (and let the *plan* clamp at export time), or disable/relabel the button when the maximum is below native ("Reset to 720 × 720 — this destination's limit") so it stops lying.
- Cheapest, but the (A) ratchet and the silent opening sizes remain; it fixes the symptom the user named and nothing else.

**Recommendation: (a).** Strongest argument: the file itself contains no resolution ceiling for any of these destinations except `post.x.gif`, `post.instagram.reels` and `post.weibo.gif`, so the "cap" is an invention; the app's own comment says a `Max` is a cap the user may go under; and the only two mechanisms that make the user's sizes wrong (`writeSize`'s ownership leak at `:660-661` and the silent clamp) are both removed by (a) without touching the ten genuine requirements. If the orchestrator wants the smallest safe step first, do the `writeSize` ownership fix **plus** the fact-panel row and the clamp note (that combination is compatible with (a) and (b) both), then revisit whether `Max` fields should be unclamped.

**Answers the options need to state explicitly** (they are the questions inside the complaint):
- `exact` / `maxExactSide` / `maxMinSide`: unchanged — real requirements, still dictated and disabled.
- The fact panel must show the destination's own box (new row) and any clamp actually applied (note), because today nothing shows either.
- `mSizeEditable`: should stay false only for the three genuinely-fixed modes; under (a) `Max` keeps it true.
- Aspect lock (`c14ac2c`): the pair write in `writeSize` deliberately suppresses derivation to avoid the 680→418→680 oscillation. Under (a) the reset no longer gets clamped, so the suppression stops producing a ratio-violating pair; the documented `[D]` item (behavior doc #36: reset re-establishes the ratio from the size it just reset to) then holds as written.
- Do not touch the one-way write-back guard itself: it is the fix for the real crash (`docs/export-dialog-implementation-notes.md:1596-1655`).

---

## 6. Unverified / open

1. **B1's *open* reading (314×512)** does not follow from the state I can reconstruct after round 10 (512×512 → predicted 442×720). Every other reading in `census.csv`/`reset.csv` is reproduced to the pixel by the arithmetic above, and B1's own `capped` (1280×720) and `reset` (720×720) readings are self-consistent with `chat.discord.file` being active. Treat B1's open value as an unexplained outlier.
2. **The reopen pair 442×718 vs a predicted 440×718** (a full re-fit of 442×718 against native 720×1172 gives `evenFloor(720·0.612628)=440`). I could not isolate whether the reopened plan path ran at all, whether `planOk` was false (which would leave the ctor/persisted pair untouched — `ExportPlan::ok()` is only `error.isEmpty()`), or whether the measurement read a pre-plan state. The *persistence itself* is proven by the config file; the exact arithmetic of the reopened field values is not.
3. **No screenshots / no pixel evidence.** `shots/` is empty. All geometry/visibility claims (tooltip absence, fact-row absence) rest on the code paths (`exportFacts.cpp:105/:202/:211`) and the a11y tree dump, not on an image.
4. **Tooltips not inspected.** "`Reset to native` is enabled but inert" is established from `reset_enabled=0` and the value not changing; whether the disabled button surfaces any tooltip text was not read.
5. **Round 5 is invalid** — the search text selected the default "My platform isn't listed (I'll choose the format)" row instead of `post.pinterest.image` (search matches the row *label*, not the id). No Pinterest/`still.*` row was measured; the `still.*` family and `post.tumblr.gif`'s 540×540 are covered by arithmetic only.
6. **`maxExactSide`/`maxMinSide` note text** was read from code, not observed in the fact panel; and `line.sticker`'s `@side` value (270) was taken from the data file and matches the measured 164×270, but the second stage of `maxMinSide` (the "at least @side" floor, `:671-674`) never fired on this project, so it is unexercised.
7. **Only one project size was measured** (720×1172, tall). Whether the same ratchet reproduces on a wide/short project is inferred from the arithmetic, not measured. Also, a project with no `imageSize` (new project default is invalid/0×0) is unmeasured.
8. **`export_inputs` effect measured indirectly**: the conf file proves the write, the reopen shows the value coming back, but a clean-sandbox run with a *pre-seeded* non-native width/height was not performed (the GUI-probe budget of one run is spent and no further application launch is permitted).
9. **The `ae set` readings inside disabled fields (B5/B6)** are a harness capability, not a user-reachable state; the "frozen 99999×162776" numbers must not be read as product behaviour.
