# Changelog

User-facing changes, newest first. This changelog starts with 1.20.0;
earlier development is recorded in the [commit history](https://github.com/omacom/omasnap/commits/main/).

## Unreleased

### Added

- `--delay SECONDS` captures after a cancellable wait without taking focus,
  allowing menus and tooltips to be arranged before the screen is sampled.
- Press `E` (edit) or `A` (annotate) in the capture picker to toggle keeping
  the annotator open after capture, including scrolling captures.
- Press `T` ("tack") while hovering a preview to pin or unpin it, alongside
  the pin button and `Ctrl+P`. The editor keeps `T` for Text.

### Changed

- Keep a preview pinned once it is dragged, whether reordering the stack or
  moving it elsewhere on screen, including Super+left-drag.
- Use the active window-border color for the pin icon while a preview is pinned.
- Match pins, editor controls, tooltips, window-selection highlights, and crop
  outlines to Omarchy's theme files, with live updates when the theme changes.
- Hide the cursor and drag handles while placing arrow heads, tails, bends, and
  line endpoints; restore them on release or cancellation.

### Fixed

- Delayed captures observe cancellation received as the countdown event loop
  exits, before synchronous fullscreen output can begin.
- Show capture crosshair guides and the correct hovered window on pointer entry,
  without waiting for the first mouse movement.
- Keep dashed selection outlines stable during partial repaints, including
  narrow rounded rectangles and scaled displays.
- Match the editor's background padding, image corners, and scaled shadows to
  saved captures, and fit the complete background frame in the preview.
- Keep previews and pins frameless at the compositor level across theme changes,
  preventing a second outline around the frame Omasnap draws.
- Suppress the armed tool while moving or resizing an annotation, including
  the cut-band preview, and keep that tool ready for the next canvas gesture.
- Keep screenshot content anchored while dragging crop handles, then re-center
  the image on release.
- Preserve the editor's keyboard focus when late pointer events arrive from a
  dismissed scrolling panel.
- Keep the scrolling overlay eligible for keyboard focus after releasing its
  exclusive grab, so Escape remains available when the overlay has focus.

## 1.21.0

### Added

- Standard, Pointy, Curved, and Double arrow styles, with editable bends.
- Text wrapping at the canvas edge or a dragged width, preserved when
  reopening a capture.
- Annotations can start outside the image and grow the canvas.
- Adjustable pen smoothing with undoable levels from 0 to 6.
- A normal editor window alongside the fullscreen overlay; `W` switches
  presentations while keeping the working document and undo history.
- Hovered-pin shortcuts: X / Super+W to close, A / E to annotate,
  C to copy the image, and L / F to copy its file path.

### Changed

- One `Esc` dismisses the annotator. Editing a pin keeps its window in place;
  returning updates that same pin and preserves editable layers and undo history,
  including across window/overlay switches.
- Fresh captures copy immediately and open a floating preview, with annotation
  available from its Edit button. Use `--editor overlay` or `--editor window`
  to annotate before output; `--copy`/`--save` bypass the pin.
- Fade normal capture previews after 10 seconds of idle time. The pin button
  or `Ctrl+P` keeps a shot on screen with a pin icon; new shots stay in front.
  Hovering, dragging, and in-progress actions pause expiry without pinning the
  preview. Only an explicit pin action keeps it until closed.
- Pinned captures are floating compositor windows, with automatic packing
  and drag-to-stack placement, without taking focus when they appear.
- Fold idle pins into a compact deck with the recents shelf's alternating tilt.
  Hover to straighten and fan them into exposed cards, keeping the fan open between
  cards and throughout a drag.
- Default capture selection accepts a region drag, a window click, or a
  click on open space for the focused monitor.
- Reduce pointer repaint work on large displays and detect highlighter text
  rows on a worker to keep the overlay responsive.
- Reduce startup time and memory use by bypassing the GTK platform theme;
  overlay fonts and colors are supplied by Omasnap.
- Preserve the selected rectangle when switching between Region and
  Scrolling Region capture.

### Fixed

- Route compositor close requests from pins to the open editor overlay, so
  `Super+W` returns to the pin like `Esc`, preserving edits and undo history.
- Preserve monitor scaling when opening scrolling captures for annotation
  or from a floating preview, so 2× captures do not appear twice as large.
  Retain the original pixel dimensions when the logical size rounds.
- Keep copied file paths usable after closing or expiring previews by saving
  the PNG before copying its path.
- Focus the next pin when closing one, so repeated X presses work without
  another mouse movement.
- Give Super+left-drag the same pin stacking and drop recovery as an image drag.
- Bring off-screen pin drops back inside the monitor where the drag started,
  using the stack's 14-pixel gap and allowing for bars on any edge.
- Match padding across pin controls while keeping the drag-out handle narrow.
- Show a pointing-hand cursor when hovering over pin buttons.
- Center labeled Edit and Copy buttons over previews, without tooltips, and
  place the Lucide pin icon beside Close. Other controls use compact, dark
  tooltips with the chrome font.
- Show Omasnap in the application launcher.
- Pass notification click actions as separate arguments.
- Keep the editor viewport and text draft aligned when its window resizes.
- Retry failed pin and editor placement, and require an ownership token
  before removing a consumed editor handoff.

## 1.20.1

### Added

- Canvas growth around annotations, with Framed, Overflow, and Image
  boundary modes (`G` / `Shift+G`).
- Custom backdrop images and an optional default backdrop style.
- Highlighter snapping to screenshot text, with a freehand mode available.
- JetBrains Mono and Inter Display annotation fonts, alongside Neucha
  (`Shift+T`), and an outlined text style.

### Fixed

- Preserve normal framing as the canvas grows and keep custom backdrops
  consistent with canvas boundary modes.

## 1.20.0

### Changed

- Group the annotation toolbar into History, Style, Tools, and Actions.
- Present Region, Window, Scrolling Region, and Fullscreen capture tabs.
- Keep the pointer still during automatic scrolling capture.
- Allow zooming out to 10% and offset numbered markers from the pointer.

### Fixed

- Keep the toolbar, capture tabs, hotkey legend, and canvas geometry aligned.
- Remove the pixels actually covered by a cut-band drag.

[Unreleased changes](https://github.com/omacom/omasnap/compare/v1.21.0...main)
· [1.21.0 changes](https://github.com/omacom/omasnap/compare/v1.20.1...v1.21.0)
· [1.20.1 changes](https://github.com/omacom/omasnap/compare/v1.20.0...v1.20.1)
· [1.20.0 changes](https://github.com/omacom/omasnap/compare/v1.19.1...v1.20.0)
