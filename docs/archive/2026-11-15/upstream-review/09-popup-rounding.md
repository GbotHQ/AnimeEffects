# Popup rounding on Windows — why build R shows a square shadow

Written 2026-09-18 for `upstream-review/`. Read this with `03-windows-test-plan.md` §"Round 8" and
the "quick verification pass" table, which carry the per-build launch commands.

## The observation

On the laptop (DellikM, Windows 11 Pro, **build 26200** → `microVersion() > 22000`, DWM branch live),
three builds were launched side by side: **U** (`C:\ae\u-dist`, stock `abb4b88`), **F**
(`C:\ae\f-dist`, `abb4b88` + the nine fixes) and **R** (`C:\ae\r-dist`, `abb4b88` + our
`AppStyle.{cpp,h}` + the three MinGW build patches). The owner's report: U and F look correct;
**R's popup has rounded corners but a square/boxy shadow around it** (their words: "rounded but with
a square/boxy shadow around it"). Which window was which was established by closing it and reading
the PID back, not by assumption: the closed window was R's exe.

## What R is, exactly

`R` is the popup handling a merge of our branch into current master would leave in place: upstream's
tree with **our** style. Tree: `.git/dsh-scratch/wt/r`; its whole diff against `abb4b88` (8 files,
149 insertions) is saved as `.git/dsh-scratch/r-vs-upstream.patch`. `src/gui/AppStyle.cpp` there is
byte-identical to `HEAD:src/gui/AppStyle.cpp`.

## Why it happens

Our `AppStyle::polish()` rounds the popup **itself**:

- `setWindowFlags(windowFlags() | Qt::FramelessWindowHint)` — the frame goes away;
- `WA_TranslucentBackground` + `WA_NoSystemBackground` — the popup becomes an alpha window;
- an 8px **1-bit `QBitmap` mask** stamped on `QMenu` from an event filter (on Windows Qt turns a
  widget mask into a window region: `QWindowsWindow::setMask` → `SetWindowRgn`, see
  `qtbase/src/plugins/platforms/windows/qwindowswindow.cpp`);
- and **no** `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, …)` and no
  `Qt::NoDropShadowWindowHint`.

Upstream solved the same problem in `fc5a703` ("Fix DWM not compositing rounded windows correctly,
hacky fallback for Windows 10 or older") — a commit our branch does **not** contain:

- `fc5a703` is an ancestor of `upstream/master` and **not** of our `HEAD`
  (`git merge-base --is-ancestor fc5a703 HEAD` → false; `git branch --contains` shows
  `upstream/master`, `feat/docking-ui`, `upstream-fixes`). Our branch forked before it landed, and
  `4814b5f` later re-invented the region trick independently — its first implementation even used
  `CreateRoundRectRgn`/`SetWindowRgn` for the pre-22H2 case, i.e. our mask's mechanism;
- in current master (`abb4b88`) the region fallback is gone — the `WinResizeFilter` class is
  commented out — and the popup path is: `WA_TranslucentBackground` + `WA_NoSystemBackground`, then
  `#ifdef Q_OS_WIN`: `microVersion() > 22000` → the DWM rounding call, `else` →
  `setWindowFlags(Qt::FramelessWindowHint | Qt::Popup | Qt::NoDropShadowWindowHint)`.

**Mechanism (hypothesis, under test):** rounding the popup's *client area* does not round the
*window frame*. On Windows 11 the frame is what DWM draws the shadow for, so a client-rounded popup
can wear a rectangular shadow. Upstream's DWM call makes DWM round the frame itself, so the corner
radius and the shadow agree. The supporting evidence is circumstantial but consistent: U and F use
the DWM call and are correct, R does not and is not, and nothing else in the popup path differs
materially. **R2/R3 below are the direct test; do not treat the mechanism as settled until they
report.**

**A comment of ours is contradicted by U.** Our `polish()` comment claims "On Windows Qt6
WA_TranslucentBackground only composites when FramelessWindowHint is also set (otherwise the corner
cut is filled black)". U never sets that flag, yet composites the rounded popup over the backdrop
correctly. So either the claim was wrong when written or the failure it describes came from
somewhere else in the combination. It should not be carried into the fix as justification.

**`Qt::NoDropShadowWindowHint` is not the lever here.** It is upstream's pre-22H2 fallback flag, and
it does not appear anywhere in the Windows QPA sources that were fetched and grepped
(`qtbase` 6.9 and 6.11, `src/plugins/platforms/windows/qwindowswindow.cpp` and `qwindowscontext.cpp`;
the only shadow reference is the `Qt::ExpandedClientAreaHint` comment at
`qwindowswindow.cpp:1035`, "Gives us the rounded corners looks and the frame shadow"). Recorded as:
the flag is not consulted by the platform-window code we read, so on this evidence the pre-22H2 path
works by **dropping the frame**, not by the flag suppressing a shadow. (Not proven to be a total
no-op on Windows — only that these code paths ignore it.)

## The two test builds

| build | path / task | what it is | what it tests |
|---|---|---|---|
| **R2** | `C:\ae\r2-dist`, `schtasks /run /tn ae_r2` | R with upstream's block **verbatim**: DWM rounding >22000, `FramelessWindowHint \| Popup \| NoDropShadowWindowHint` below it; mask, event filter and `FramelessWindowHint` OR **removed** | the intended fix — should look like U's popups |
| **R3** | `C:\ae\r3-dist`, `schtasks /run /tn ae_r3` | R's block **kept** (mask + `FramelessWindowHint`) **plus** the DWM rounding call | the MS rule below says this must *still* fail; the run is what makes it evidence |
| **R4** | `C:\ae\r4-dist`, `schtasks /run /tn ae_r4` | mask + frameless removed, **no Win32 call at all**: only Qt's `Qt::ExpandedClientAreaHint` | does Qt's own Windows lever round a popup? (Qt documents the flag for something else, so this is open) |

Generated by `.git/dsh-scratch/mk_variants.py` (exact-match asserts, so source drift fails loudly),
built and deployed by `.git/dsh-scratch/build_variant.sh <variant>`. Both differ from R only in
`src/gui/AppStyle.cpp` (R3 also gains the Windows includes).

**One cross-build trap, hit and recorded:** porting upstream's block verbatim fails to compile on a
case-sensitive host — `<Windows.h>` does not exist in the MinGW sysroot (the first R2 build died at
`AppStyle.cpp:18: fatal error: Windows.h: No such file or directory`). The variants use
`<windows.h>`, which is exactly fix-series patch `0001` (U14). So **the port depends on that patch**;
it is not optional when the merged tree is cross-built here.

Expected outcomes: **R2 correct** → adopt upstream's block on our side, the question is closed.
**R3 also correct** → the mask can stay for pre-22H2 if we want rounded corners there; the DWM call
is what was missing. **Neither correct** → the region mask or `FramelessWindowHint` breaks DWM
rounding outright and both must go; next suspect would be `SetWindowRgn` on a translucent window.

## What Microsoft actually blesses — and why it settles the direction

Source: *Apply rounded corners in desktop apps for Windows 11*
(`learn.microsoft.com/windows/apps/desktop/modernize/ui/apply-rounded-corners`; the raw markdown is
saved as `.git/dsh-scratch/ms-rounded-corners.md`), plus the geometry design page and the
`DWM_WINDOW_CORNER_PREFERENCE` reference. Quoting, because it decides this:

- **The never-roundable list:** "Apps that cannot ever be rounded, even if they call the opt-in API.
  These apps have no frame or borders, and typically have heavily customized UI. If your app does one
  of the following, it cannot be rounded: **Per-pixel alpha layering** / **Window regions**." — with
  the explanation that a custom-shadow app "draw[s] transparent pixels around its main window … which
  makes the window no longer a rectangle and therefore the system cannot round it".
- **Auto-rounded** = apps that "provide enough information to the system so it can properly round
  them, such as setting the `WS_THICKFRAME` and `WS_CAPTION` window styles or providing a 1-pixel
  non-client area border".
- **Roundable with the opt-in API** (category 2) is broken by "lack of frame styles", "empty
  non-client area", "other customizations, such as extra non-child windows used for custom shadows" —
  and the API "is a hint to the system and does not guarantee rounding".
- **Menus specifically:** "By default, menus are pop-up windows, which do not get rounded. If your app
  creates a custom menu and you want it to follow the rounding policy of other standard menus, you can
  call the API" — their own sample uses **`DWMWCP_ROUNDSMALL`** "because menus are auxiliary UI".
- The blessed API is `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, …)`, Windows build
  22000+. Apps are not rounded when maximized, snapped, in a VM, on WVD or under WDAG.

So: **upstream's approach is the Microsoft-documented one** (opt-in corner preference, no region, no
per-pixel layering), and our mask is in the category that can never be rounded — not merely a
redundant belt, an actual disqualifier.

## Why our popup could never have been rounded (Qt source chain)

For a `QMenu` (a `Qt::Popup` window) under our old code:

1. `WindowCreationData::fromWindow`: `if (popup || type == Qt::ToolTip || type == Qt::SplashScreen)
   style = WS_POPUP;` and the `WS_THICKFRAME`/`WS_CAPTION` block is only entered for
   `type == Qt::Window || dialog || tool` — **a Qt popup therefore has no frame styles at all**
   (`hasFrame = (style & (WS_DLGFRAME | WS_THICKFRAME)) && !FramelessWindowHint` → false). A menu is
   exactly MS's "not rounded by default" case.
2. Our `Qt::FramelessWindowHint` made `QWindowsWindow::hasNoNativeFrame()` true; with `hasAlpha`
   (from `WA_TranslucentBackground`) `setWindowLayered()` then returns true, and for a
   non-accelerated window `setWindowOpacity()` calls **`UpdateLayeredWindow(… ULW_ALPHA)`** — per-pixel
   alpha layering.
3. Our `QBitmap` mask: `QWidget::setMask` → `QPlatformWindow::setMask` → `QWindowsWindow::setMask` →
   **`SetWindowRgn`** — a window region.

That is two of MS's three never-roundable conditions, plus the missing frame styles. Dropping the mask
and the frameless hint (R2) leaves `needsLayered == false` and no region: MS category 2, roundable via
the opt-in API.

## Qt has no blessed route of its own

- Qt **never** calls `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE)` — the only
  `DwmSetWindowAttribute` calls in qtbase 6.11's Windows QPA are the dark-mode ones
  (`qwindowswindow.cpp:3913-3914`).
- Qt's only rounded-corner/shadow lever on Windows is `Qt::ExpandedClientAreaHint` →
  `DwmExtendFrameIntoClientArea(hwnd, {-1,-1,-1,-1})`, whose source comment reads "Gives us the
  rounded corners looks and the frame shadow" (`qwindowswindow.cpp:1035`). But Qt's *documentation* of
  that flag (`qnamespace.qdoc`) describes expanding the client area into title-bar controls and
  safe-area margins — the corners are an undocumented side effect. R4 tests whether that side effect
  is usable for a popup.
- `Qt::NoDropShadowWindowHint` is not consulted by the Windows QPA at all, so it is not a lever here;
  Qt popups get their shadow from DWM, not from Qt.
- There is no stylesheet route to round a top-level window, and `QWindows11Style` (6.7+) is about
  widget painting, not window frames.

## The predictions this makes

| build | window region? | per-pixel alpha layering? | frame styles? | DWM opt-in call | prediction |
|---|---|---|---|---|---|
| **R** (tested: bad) | yes (mask) | yes (frameless + alpha → `UpdateLayeredWindow`) | none | no | client-rounded paint inside rectangular system decoration → the reported "square/boxy shadow" |
| **U** (tested: good) | no | no | none | yes | system-rounded, system shadow |
| **R2** | no | no | none | yes | correct, i.e. matches U |
| **R3** | yes | yes | none | yes | **still wrong** — MS: "cannot ever be rounded, even if they call the opt-in API" |
| **R4** | no | no | none | Qt's `ExpandedClientAreaHint` | open |

If R2 comes back correct the fix is settled. If R3 comes back *correct*, MS's never-roundable list
does not hold in our case and this reading has to be revised — which is why R3 is worth a window
rather than a paragraph.

**One design detail for later:** MS's own custom-menu sample passes `DWMWCP_ROUNDSMALL` ("menus are
auxiliary UI"), while upstream passes `DWMWCP_ROUND` and our painted panel uses an 8px radius. These
have to agree: DWM cuts the *window*, so if DWM rounded tighter than our painted panel, the system cut
would clip the panel's own edge. Keep `ROUND` unless the painted radius changes with it.

> **Partly superseded by the measurements below (same day).** R3 — a window region *and* per-pixel
> alpha layering, i.e. two of the three never-roundable conditions — turned out to work, and the
> failure mode of the upstream block was not a shadow at all but **lost transparency**. The
> predictions above were the right questions with two wrong answers; read the next section for what
> the five builds and the Win32 probe actually showed.

## Round 9 (measured): five builds, one probe, and a corrected conclusion

The builds were labelled in their title bars (`03-windows-test-plan.md` §"Telling the builds apart")
so the comparison no longer depended on PIDs. Owner's report, verbatim and paraphrased:

| build | configuration | observed |
|---|---|---|
| **R** | our old block: `FramelessWindowHint` + 8px `QBitmap` mask + translucent, **no DWM call** | rounded corners, but a **square/boxy shadow** around the popup |
| **U** | upstream block: translucent + DWM `DWMWCP_ROUND`, no frameless, no mask | **QMenu transparency broken (black)**, combo drop-down fine |
| **F** | same block as U | same as U |
| **R2** | upstream's block verbatim inside our `AppStyle` | **black background, transparency missing** |
| **R3** | **our block + the DWM call** | **works** |
| **R4** | mask/frameless removed, `Qt::ExpandedClientAreaHint` only | **popup never appears**; the window briefly loses focus |

Then a Win32 probe of the *live* windows (task `ae_probe`, script `.git/dsh-scratch/probe_windows.ps1`,
result copied back to `probe_result.txt`), which is where the explanation came from:

- Every main window has `popup=True thick=True caption=True` — Qt gives top-level windows
  `WS_POPUP | WS_THICKFRAME | WS_CAPTION`, so they *have* frame styles for DWM to round and shadow.
- **Popups have no frame styles at all** (`thick=False`, `caption=False`) in every build.
- **R3's popup windows are `WS_EX_LAYERED`; U's, F's and R2's are not.** That single difference tracks
  the transparency results exactly.
- The region readout was `0` (no region) on every enumerated window, including R3's — but the masked
  `QMenu` windows were evidently not among those still alive at probe time, so this part is
  **inconclusive, not negative**: it neither confirms nor refutes that `setMask` reaches
  `SetWindowRgn`.

### The explanation, from the Qt source

`QWindowsWindow::setWindowLayered()` decides layering like this:

```cpp
const bool needsLayered = (flags & Qt::WindowTransparentForInput)
    || (hasAlpha && hasNoNativeFrame(hwnd, flags)) || opacity < 1.0;
```

with `hasNoNativeFrame() = (style & WS_CHILD) || (flags & Qt::FramelessWindowHint)`, and
`setWindowOpacity()` then does the actual per-pixel work only in that same case:

```cpp
if (hasAlpha && !accelerated && hasNoNativeFrame(hwnd, flags)) {
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}
```

So: **a translucent Qt popup only gets real per-pixel alpha if it has no native frame.** Our block ORs
in `Qt::FramelessWindowHint` → layered → transparency works. Upstream's block does not → not layered →
the alpha channel is simply dropped, and every pixel that our rounded panel leaves unpainted comes out
**black**. That is precisely U/F/R2's symptom, and it means:

- **`fc5a703` is incomplete on Windows.** Upstream's own `PE_PanelMenu` comment assumes "the popup
  window is translucent, so the corner cut shows the backdrop through real alpha" — for a popup they
  never make frameless, that assumption is false. Their Win11 path therefore paints an 8px-rounded
  panel onto an opaque black rectangle. This is a reportable upstream bug, with our R2 as the minimal
  reproduction (their block, our style, same machine).
- **Our mask/frameless approach is not the thing to remove** — it is what makes transparency work at
  all on Windows. The missing piece was the DWM call, i.e. R3.
- **MS's category-3 warning did not hold in this case.** R3 is both layered (per-pixel alpha) and
  region-masked, which the rounded-corners page says "cannot ever be rounded, even if they call the
  opt-in API", yet the DWM call visibly removed the artifact. The honest statement is: for a frameless
  layered popup the corner-preference hint still does something useful here, and the doc's category
  list is not a reliable predictor for this configuration. R6/R7 (below) are re-checking the baseline
  and asking whether the *value* matters.
- **R4 closes the Qt-native question.** `Qt::ExpandedClientAreaHint` on a `QMenu` makes the popup
  never appear (the window briefly takes focus instead). Qt treats that flag as "window with a native
  frame" and `setWindowFlags()` re-creates the platform window, which breaks the popup's show path. So
  Qt has **no usable route** for rounding a popup, and the plugin-level DWM call — what upstream does —
  is the only one. That is worth stating plainly in the upstream discussion rather than being
  discovered twice.

### Two last builds

| build | what it is | question |
|---|---|---|
| **R6** | our current block, unchanged (the old build R, now labelled and deployed) | controlled re-baseline: does R6 really show the square artifact next to R3 in the same session? |
| **R7** | R3 with `DWMWCP_DONOTROUND` | does the *value* do the work (ROUND really rounds), or does merely handing the frame to DWM remove the artifact? |

If R6 shows the artifact and R3 does not, the fix is confirmed by direct A/B: **keep our block, add
the DWM call**. If R7 also looks clean, the value is incidental and the upstream write-up should say
"handing the popup to DWM" rather than "rounding it".

### Probe 2: what the popup windows actually are (measured, 15:00)

`probe_result.txt`, with `corner=` added by `DwmGetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE)`:

| window | layered | frame styles (`thick`/`caption`) | corner preference |
|---|---|---|---|
| U's popups (274x182, 149x123, 230x274) | **False** | absent | **2 = `DWMWCP_ROUND`** |
| F's popup (153x85) | **False** | absent | 0 |
| main windows (all builds) | False | present (`WS_THICKFRAME`/`WS_CAPTION`) | 0 or 2 |
| R3/R6/R7 menus | — | — | not captured; their menus were not open at probe time |

That U row is the whole upstream story in one line: **the corner preference is set, the window has no
frame styles, and the window is not layered.** So DWM has no frame to round — the preference is the
"hint … does not guarantee rounding" the docs warn about — and with no layering the popup's alpha
channel goes nowhere, which is why U's rounded panel sits on black corners.

An attempt to open each build's File menu from the interactive session (`probe_menus.ps1`,
`SetForegroundWindow` + `SendKeys '%f'`) **did not work** — no menu appeared in any build, only U's
already-open General Settings dialog was picked up. That route was abandoned; the menus were measured
by simply probing again after the owner had opened them, below.

### Probe 3: the menu windows themselves (measured, after the owner opened menus)

Re-running the same probe once menus had actually been opened caught the popup windows, and this
settled both open questions:

| build | popup window | layered | region | corner preference |
|---|---|---|---|---|
| **R6** (our block, no DWM call) | 149x123 (the QMenu) | **True** | **3 = `COMPLEXREGION`** | 0 (no call) |
| **R7** (`DWMWCP_DONOTROUND`) | 153x85 (the QMenu) | **True** | **3 = `COMPLEXREGION`** | 1 |
| **R3** (`DWMWCP_ROUND`) | 100x30 popups | **True** | 0 | 2 |
| U (upstream) | 230x274 | **False** | 0 | 2 |
| F | 153x85 | **False** | 0 | 0 |

Two of our own claims are now measured rather than inferred:

- **Our popups are genuinely layered** (`WS_EX_LAYERED`), which is what makes per-pixel alpha real —
  the Qt-source reading above is confirmed on the actual windows. Upstream's are not, which is the
  black-corner bug.
- **`QWidget::setMask` does reach `SetWindowRgn` on Windows**: our masked `QMenu` reports
  `COMPLEXREGION` (a rounded-rect region is a complex region). The earlier "region=0 everywhere"
  readout was inconclusive only because the masked windows were not alive at that moment.
- The corner preference readout also proves the DWM call lands on the window we think it does
  (0 / 1 / 2 per build).

**What this implies for the last open symptom** (owner's report: *"QMenu is correct, but the dropdown
has non-rounded shadow"*): the popup we deliberately do **not** mask — the combo drop-down, see
`7fd0d16` and the `QComboBoxPrivateContainer` note in `AppStyle::eventFilter` — is exactly the popup
that comes out wrong, while the masked `QMenu` is right in every build. So the window region is doing
the shaping work, and the un-masked drop-down depends on the DWM call for its rounded shape. R5
(frameless + translucent + DWM round, **no mask at all**) is the test of whether the mask is needed
at all once DWM rounds the window.

## Outcome (measured): R5, and the fix that was applied

**R5 — frameless + translucent + `DWMWCP_ROUND`, no mask, no event filter — has both popups correct**
(owner's verdict). R3's drop-down was never checked: the owner was closing the older windows and
closed R3 before looking at it, so "R3 works" covers the QMenu only. That no longer matters, because
R5's configuration is strictly simpler and is what was applied:

- **The mask is not needed on Windows.** The corners come from the frameless+translucent popup
  (layered → real per-pixel alpha → the panel's own 8px rounding), and the *shadow* comes from DWM
  once the window is rounded.
- **The value matters**: R7 (`DWMWCP_DONOTROUND`) and R6 (no call at all) both leave the un-masked
  drop-down with a square shadow, while R3/R5 (`DWMWCP_ROUND`) do not. So the upstream write-up should
  say "ask DWM to round the popup", not "hand the popup to DWM".
- **Why the mask had to go on Windows specifically:** a window region is precisely what makes a window
  un-roundable for DWM (Microsoft's own wording), so the region and the corner preference are mutually
  exclusive levers — keeping both means DWM silently does nothing, and the un-masked drop-down keeps
  its square shadow. On non-Windows the region stays, where it still earns its place (compositors that
  don't composite `Qt::Popup` translucency, plus a click-through corner cut).

### What was changed in the tree

`src/gui/AppStyle.{cpp,h}` — `AppStyle::polish()` now:

- keeps `FramelessWindowHint` (OR'd into the existing flags) + `WA_TranslucentBackground` +
  `WA_NoSystemBackground` for menus and the combo pop-up, with the comment explaining that the
  frameless flag is what makes Qt layer the window and therefore what makes the alpha real;
- on Windows 11 22H2+ (`QOperatingSystemVersion::current().microVersion() > 22000`) calls
  `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND)`;
- installs the region-mask event filter **only off Windows** (`#else` branch), so nothing changes for
  Linux/Wayland/X11;
- `AppStyle::eventFilter` itself is unchanged — it is now simply not installed on Windows.

Verified by cross-building the branch's own `AppStyle.cpp` into the R tree (variant `r5b`), which is
the same code path the verified R5 ran, and by the native build + test suite on Linux.

### Still open

1. **Pre-22H2 (Windows 10) is untested and untestable on this laptop.** There is no corner-preference
   API there, so the popup will have no system shadow; the corners still come from the panel paint over
   real alpha. Upstream's old path for that case (`FramelessWindowHint | Popup |
   NoDropShadowWindowHint`) is not needed by us, since we already set the frameless flag.
2. **Our own two popovers still mask themselves** — `EasingPickWidget` (`setMask`, 8px) and
   `AnimationSettingDialog` (QML dialog, `setMask`). By the reasoning above they should show a
   rectangular system shadow on Windows 11 and lose their corners to the region. They cannot be
   cross-built or run here: the MinGW sysroot lacks Qt6 `Qml`/`Quick` (the phase-1 QML pilot) and
   `LinguistTools`, and installing them is a root/network action. Fixing them means giving them the
   same DWM call (a shared helper in `AppStyle`'s neighbourhood would avoid a third copy of it), and
   it needs a real Windows build before it can be called verified.
3. **Upstream report:** `fc5a703` sets the corner preference on a popup that has neither frame styles
   nor layering, so it gets neither rounding nor transparency — their rounded panels sit on black
   corners. R2 (their block, our style) is a minimal reproduction; the measurement table in "Probe 2"
   is the evidence.

> Items 1 and 2 above are now **done** (the modules were installed and the two popovers use the shared
> `gui/PopupWindow.h`); the section below records what that verification turned up.

## Popup activation on Windows: why the QML fields could not take focus

With the shared shaping in place and the contrib slice running on Windows 11 22H2, the owner's verdict
was "works, except the fields in the animation settings popup can't gain focus — left clicking them
should focus and enter text edit mode". Not a regression from the shaping change, and not a mystery
either, once Qt's Windows QPA is read:

```cpp
    // QWindowsWindow::setVisible(), show_sys()
    if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::Tool
        || (flags & Qt::WindowDoesNotAcceptFocus) || testShowWithoutActivating(w))
        sm = SW_SHOWNOACTIVATE;
```

and, a few lines later, the only place a popup is brought forward:

```cpp
    // QTBUG-44928, QTBUG-7386: popups opened from the system tray were not
    // implicitly activated
    if (win->type() == Qt::Popup && !win->parent() && !QGuiApplication::focusWindow())
        SetForegroundWindow(m_data.hwnd);
```

So on Windows a `Qt::Popup` is shown **without activation**, and Qt only activates it when the
application has no focus window at all. Opened from a focused main window — which is exactly how this
popup is opened, from the playback-options button — the popup window is never the active window.

That is invisible for widget-based popups, because `QWidget` tracks focus per window (which is why the
easing picker's `QSpinBox`es are fine), but this dialog renders its form with **Qt Quick**, where
`Item::activeFocus` additionally requires an **active window**, and `ScrubSpinBox` gates its edit mode
on precisely that:

```qml
        onReleased: {
            ...
            } else {
                // Plain click: enter edit mode, selecting the value for typing.
                editInput.text = root.fmt(root.value);
                editInput.forceActiveFocus();
            }
        }
        // edit mode is only visible while focused
        visible: activeFocus
```

On X11/Wayland the compositor activates the popup, which is why the fields work there and only
Windows was affected. The fix is one call in `AnimationSettingDialog::showAt()`, after `show()` and
`raise()`:

```cpp
    activateWindow();
```

`QWindowsWindow::requestActivateWindow()` takes the `SetForegroundWindow(hwnd) + SetFocus(hwnd)` path
when the application is already active, which is our case, so this is enough. Verified by the owner on
the slice build. **Worth knowing for every future frameless popup in this codebase:** on Windows,
"it is a `Qt::Popup`" means "it will not be activated for you", so any popup whose content depends on
window activation (Qt Quick focus being the obvious one) has to ask.

## Consequences for the contribution

1. **On our side the fix is to adopt upstream's block**, i.e. port `fc5a703`'s shape (and delete the
   mask, the event filter, the `FramelessWindowHint` OR, and the header declaration). This is a port
   of upstream's fix, not a revert — it answers README decision 2 in the **opposite** direction from
   the one the review had been carrying: keep upstream's DWM code, drop *our* workaround.
2. **This is a pre-merge conflict hunk retired.** `AppStyle.cpp` is one of the 11 mechanical
   conflicts in `.git/dsh-scratch/premerge-conflicts.txt`; doing this on our side first means the
   merged tree cannot silently keep the square-shadow behaviour, which is exactly the "clean
   cherry-pick, wrong tree" failure mode this review keeps meeting.
3. **`7fd0d16` becomes moot** ("don't stamp a rounded mask on combo drop-downs"): with no mask,
   `QComboBoxPrivateContainer::paintEvent`'s `mask().isEmpty()` condition is satisfied by
   construction, so the special case has nothing left to guard.
4. **Open question that cannot be answered on this laptop:** what pre-22H2 (Windows 10) should look
   like. Upstream's fallback drops the frame and therefore leaves **square** corners there; our mask
   currently rounds them. The owner's laptop is 26200, so neither the current fallback nor a
   reinstated region trick can be tested here. If Win10 rounded corners matter, the region path has
   to be reinstated *deliberately* in the `else` branch and flagged as untested — not carried along
   as a side effect of leaving the mask in place.

## The same client-side mask pattern elsewhere in our code (follow-up, not fixed here)

Two of our own popup windows round themselves exactly the way build R does, so they are candidates
for the same defect on Windows — but the R2 fix does not touch them (they are not `AppStyle`, and R's
tree does not even contain them):

| site | what it masks | why, per its own comment |
|---|---|---|
| `src/gui/prop/EasingPickWidget.cpp:1216-1231` | the easing popover (a `QFrame` window) | "cheap and compositor-independent. On Windows the translucent backing would otherwise show the square corner cut as black" |
| `src/gui/AnimationSettingDialog.cpp:96-105` | the QML animation-settings popup | "clips the window so the (transparent) corner cut is click-through and the compositor knows the shape" |

The mask does three jobs at those sites — rounded corners, a click-through corner cut, and
determinism under Qt's offscreen platform in tests — where in `AppStyle` it only ever did the first.
So "delete the mask", which is right for the menus and drop-downs, is **not** automatically right
here; R3 is the data point on whether a mask and the DWM rounding call can coexist, and its answer
decides how these two are treated.

**Blocker on testing them:** our own branch cannot be cross-built for Windows on this machine — the
MinGW sysroot lacks Qt6 `Qml`/`Quick` (added by the phase-1 QML pilot) and `LinguistTools`, and
installing them is a root/network action needing the owner's approval. So our own popovers' Windows
appearance is currently untested, and fixing them waits on either the sysroot gaining those modules
or a native Windows build on the laptop (its Qt6 kit has not been checked).
