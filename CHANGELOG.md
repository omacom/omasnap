# Changelog

User-facing changes, newest first. This changelog starts with 1.20.0;
earlier development is recorded in the [commit history](https://github.com/omacom/omasnap/commits/main/).

## Unreleased

### Added

- Standard, Pointy, Curved, and Double arrow styles, with editable bends.
- Text wrapping at the canvas edge or a dragged width, preserved when
  reopening a capture.
- Annotations can start outside the image and grow the canvas.
- Adjustable pen smoothing with undoable levels from 0 to 6.
- A normal editor window alongside the fullscreen overlay; `W` switches
  presentations while keeping the working document and undo history.
- Floating pins close themselves after `[pin] dismiss_after_seconds`
  idle seconds, with a shrinking bottom-edge bar and a countdown pill in
  the last 10 seconds; any hover, click, or key keeps the pin until closed.

### Changed

- Fresh captures copy immediately and open a floating pin, with annotation
  available from its Edit button. Use `--editor overlay` or `--editor window`
  to annotate before output; `--copy`/`--save` bypass the pin.
- Pinned captures are floating compositor windows, with automatic packing
  and drag-to-stack placement, without taking focus when they appear.
- Default capture selection accepts a region drag, a window click, or a
  click on open space for the focused monitor.
- Reduce pointer repaint work on large displays and detect highlighter text
  rows on a worker to keep the overlay responsive.
- Reduce startup time and memory use by bypassing the GTK platform theme;
  overlay fonts and colors are supplied by Omasnap.
- Preserve the selected rectangle when switching between Region and
  Scrolling Region capture.

### Fixed

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
