# Changelog

User-facing changes, newest first. This changelog starts with 1.20.0;
earlier development is recorded in the [commit history](https://github.com/omacom/omasnap/commits/main/).

## Unreleased

### Added

- Save As (`Ctrl+Shift+S`): choose a PNG destination without closing the editor.

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
  Hovering pauses expiry; clicking, dragging, scrolling over a preview, or using
  its controls keeps it until closed. Explicit editor pins also stay until closed.
- Pinned captures are floating compositor windows, with automatic packing
  and drag-to-stack placement, without taking focus when they appear.
- Fold idle pins into a compact deck with a gentle alternating tilt; hover to
  straighten and fan them into exposed cards, keeping the fan open between
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

[Unreleased changes](https://github.com/omacom/omasnap/compare/v1.20.1...main)
· [1.20.1 changes](https://github.com/omacom/omasnap/compare/v1.20.0...v1.20.1)
· [1.20.0 changes](https://github.com/omacom/omasnap/compare/v1.19.1...v1.20.0)
