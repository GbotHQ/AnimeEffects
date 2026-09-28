# The export dialogs: behaviour contract

This is the specification the export form and the destination picker are graded against. It exists
because the port was accepted on the wrong evidence: screenshots that cannot show a cursor, an
accessibility assertion that a control *exists*, and unit tests on the view model. None of those can
observe focus, keys, hover, drag, tooltip timing or overflow — which is why fourteen defects reached
the user with every gate green.

**The design is the specification. The implementation is chosen to meet it, not the other way
round.** Where no stock control meets a line below, the line still stands and the behaviour is
written and tested; a toolkit's limits are never a reason to weaken this document.

## Sources

- **[R]** the review of the ported dialogs (sixteen items, numbered in the traceability table).
- **[W]** the house standard: the behaviour of the equivalent QWidget control in this application.
  "Consistent with the rest of the app" is only meaningful because these exist; each [W] line is
  confirmed by driving that widget before the new control is accepted.
- **[D]** the design intent of the two dialogs themselves.

## How each line is verified

Driving the real application, never reading QML:

| Instrument | Shows |
| --- | --- |
| typed key sequences (type, arrows, tab, escape, enter, ctrl+…) | focus, routing, caret, navigation |
| click on another control / empty space | focus loss, commit, click-outside |
| `ffmpeg -f x11grab -draw_mouse 1` capture | pointer shape per region (X11 renders the cursor; `import` cannot) |
| timestamped hover frames (0 ms → wake-up delay → timeout) | tooltip delay, position, flipping, hiding |
| drag with motion, then reading the scroll position | scrollbar drag, proportion, page and wheel |
| resize to the minimum size and to maximised | overflow, clipping, layout at the extremes |
| a second capture at 1.5×/2× scaling | icon and text rendering, aliasing |

A line is only "verified" when it has a captured observation or an automated test that fails when
the behaviour regresses. "It looks right in a screenshot" is not a verification of anything below.

## A. Dialog level — focus and keys

1. [W] Tab and Shift+Tab move through the controls in visual order; the focused control shows its
   focus ring, and the ring never appears from a mouse click alone.
2. [W] Enter activates the default action (Export), Escape cancels, unless a focused text field
   consumes the key.
3. [W] Clicking a non-focusable area does not steal focus from a text field mid-edit; the field
   keeps its text and caret.
4. [D] The dialog is modal over the main window, is transient for it, and restores its last size,
   never below the minimum size, at every DPI.

## B. Search field (picker)

5. [R1] The field takes focus on open, with a visible caret; typing filters the list as you type.
6. [R1] While the field has focus, Down/Up move the LIST's highlighted row; they never move the
   caret and never alter the text.
7. [D] Enter activates the highlighted row, identically to clicking it.
8. [D] Escape clears the field when it has text, and only closes the dialog when the field is
   empty.
9. [R3] Clicking anywhere outside the field — list, buttons, empty space — removes its focus and
   keeps its text.
10. [R4] The pointer is an I-beam over the text area, the arrow over the clear button, and the
    arrow everywhere else in that field's row.
11. [R12] The selection highlight uses the app's text-selection tokens, not a platform or style
    default.
12. [W] Select-all on focus, double-click word select, Ctrl+A/C/V, and IME composition behave as
    the app's widget line edits do.

## C. Destination list (picker)

13. [R2] A click on a group header expands or collapses it — including while the search field has
    focus — and never selects the row.
14. [R8] Group expansion state is explicit and stable: see the open design questions.
15. [R6] A destination with no children is not indented as though it had a parent.
16. [D] Search matches across every group; groups with no match are not listed; matches are
    reachable with the keyboard, not only with the pointer.
17. [W] The keyboard and pointer agree: the highlighted row is the row Enter activates and the row
    a click selects, and hovering never changes the selection.
18. [R7] No row paints a wrong intermediate colour: the hover state on first entry is the same as
    on the fifth, with no black or unstyled frame.

## D. Scrolling and overflow

19. [R5] The list's scrollbar reflects the visible/total ratio, drags the content 1:1, pages on
    track click, responds to the wheel, and appears only when the content overflows.
20. [R5] Its thumb, track, hover and pressed states use the app's tokens — the same ones the main
    window's scrollbars use.
21. [R11] The export form never exceeds the window without a scroll: when the content does not fit
    (minimum size, small screen, or the Advanced section open), the content scrolls by wheel, keys
    and drag, and the Cancel/Export row stays reachable.

## E. Tooltips

22. [R9] A tooltip appears after the platform's wake-up delay — never instantly — and hides after
    the platform's timeout or when the pointer leaves the control.
23. [R9] It is positioned relative to its control in the conventional place (below, or above when
    there is no room), not at the pointer.
24. [R10] It is never clipped by the dialog's edge; it flips or shifts to stay inside the screen.
25. [R9] It does not appear for a control whose text already states the same thing; the set of
    controls that keep a tooltip is explicit (open design question).
26. [W] It does not intercept the pointer, does not take focus, and hides immediately on click.

## F. Numeric fields (frames, resolution, frame rate, quality)

27. [R12] In text-entry mode the pointer over the text is an I-beam and the selection uses the
    app's selection tokens.
28. [D] In scrub mode (the pointer over the field but not in text entry) a horizontal drag changes
    the value by the field's step, clamped to the field's range, with a pointer that shows the
    scrub affordance.
29. [W] Click selects the whole value; typing replaces it; Enter commits; Escape restores the value
    the field had when it was focused; Up/Down step by one.
30. [D] The Auto/Reset affordance is a separate, keyboard-reachable control; using it restores the
    automatic value and the row reads "Auto".
31. [D] Width and height keep their aspect lock; the frame-range fields clamp against each other so
    a range can never invert.

## G. Combo boxes (format, codec, quality intent)

32. [W] Click, Down, type-ahead and Enter choose an item; the closed field shows the current item;
    the popup closes on selection, on Escape, and on a click outside.
33. [R10] The popup is never clipped by the dialog's edge, and it is positioned under its field
    (flipping above when there is no room below).
34. [D] The list is keyboard-navigable while open, with the highlighted item following the keys.

## H. Facts panel, layout and information architecture

35. [R13] The facts read as muted label / value pairs, one fact per row, with no row repeating its
    own value as its explanation.
36. [R12] The frame range lives under the Custom/Advanced section; the default surface stays
    uncluttered for the common case.
37. [D] Controls the destination dictates are stated, not offered; controls it merely defaults stay
    reachable under Advanced.

## I. Destination data

38. [R14] The Discord 20 MB cap is opt-in and settable: the guidance (20 MB free, 50 MB Nitro,
    500 MB boosted) is stated as a fact the user may apply, never enforced silently.
39. [R15] A destination row declares the codecs it accepts and which one is the default, so the
    codec row can offer AV1/H.265/H.264 where the platform accepts them, instead of a fixed
    encoder. (Schema change: a list, not a mode.)

## J. Rendering

40. [R7] Icons render without aliasing at 1×, 1.5× and 2×: vector sources are rasterised at the
    device pixel ratio, and nothing is scaled up from a 1× bitmap.

## Open design questions (design decisions, not implementation limits)

- **D1.** Picker: default expansion state of the groups — all collapsed, all expanded, or the last
  state remembered per session?
- **D2.** Picker: does typing in the search field auto-expand the groups that have matches?
- **D3.** Tooltips: which controls keep one? (Items that restate their own label do not.)
- **D4.** Picker: is the group header the only expansion affordance, or does a click anywhere in
  the group's header row toggle it (the current complaint is that rows were selectable at all)?

## Traceability: the review items → this document

| Review item | Contract |
| --- | --- |
| search: no keyboard selection/navigation | B6, B7, C17 |
| arrows expand/collapse while typing | B6, C13 |
| search keeps focus on outside click | B9 |
| search: wrong pointer | B10 |
| scrollbar inconsistent, drag inert | D19, D20 |
| childless top-level items indented | C15 |
| icons aliased | J40 |
| groups expanded by default | C14, D1 |
| tooltips instant, redundant, at the item | E22, E23, E25, D3 |
| tooltips clipped by the window | E24 |
| Export parameters overflows | D21 |
| frame range belongs under Custom | H36 |
| scrubber: wrong pointer, wrong highlight | F27, F28 |
| Add range hover flashes black | C18 |
| Discord 20 MB enforced | I38 |
| Discord codec fixed | I39 |
