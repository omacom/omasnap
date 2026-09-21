# Omasnap

See the [changelog](CHANGELOG.md) for release highlights and unreleased changes.

A native Wayland screenshot and annotation overlay designed for Omarchy and Hyprland.
It captures the focused monitor before mapping an exclusive layer-shell surface, so the
editor never appears in its own screenshot. The editor retains annotations as movable,
resizable vector layers and preserves the monitor's native pixels on scaled displays.

Select a capture and it copies straight to the clipboard, with a floating preview for
copying again, dragging into another app, or opening the editor on demand.
The preview fades after 10 seconds of idle time. Hovering and unfinished actions
pause its countdown. The pin button, `Ctrl+P`, `T` while hovered, or dragging the
preview keeps it on screen.

[![Looping Omasnap demonstration](assets/omasnap.gif)](assets/omasnap.mp4)

## Features

- Smart selection by default: drag a freeform region and release to capture.
  With a fixed aspect ratio, adjust the region and press Enter to capture.
  Click a window to crop it, or click open monitor space for the full monitor.
  Explicit region, window, fullscreen, and scrolling-region modes remain
  available.
- Fresh captures copy immediately and show a floating preview for 10 seconds
  without taking keyboard focus. Hover for Pin, Edit, Copy, file drag, and Close
  controls. Use the pin button, `Ctrl+P`, `T` while hovered, or drag the preview to
  keep it on screen.
- A pointer-side readout that turns any drag into a ruler: the pointer position
  while the crosshair is idle, then the frame size in native export pixels while a
  region, a hovered window, or a crop handle is being sized. Crosshair guides
  appear without requiring an initial mouse movement.
- Window capture is a crop of the focused-monitor frame. Overlapping windows stay
  visible; there is no second clean-window recapture.
- Select/move/resize layers, mouse-wheel scaling, and eight external recropping handles.
- Start arrows, shapes, strokes, markers, spotlights, or text in the unused
  fullscreen workspace around a screenshot, or resize and carry an existing
  layer past its edge, to grow the canvas. Source-based tools (redact, cut,
  OCR, and eyedropper) stay on the screenshot.
  Framed growth is the default, with 15 px of mat kept beyond any layer that
  outgrows the normal frame; `G` cycles to tight Overflow growth (only the
  sides needed by annotations, with no frame), then Image (the original canvas
  size, clipping every outside annotation). `Shift+G` cycles backward without
  changing layer geometry. New framed strips start in window gray with the
  original screenshot's card shadow, and follow a layer live while it is drawn
  or carried past the edge, or back inside it; a label being typed out there
  counts from the moment its caret is placed. `B` cycles through the colorful backdrops,
  shadowed and flat window gray, and Off so a background can always be removed.
  Overflow with no backdrop leaves its added pixels transparent. `Shift+B`
  toggles the current shadow directly, and undo/delete can contract grown strips.
- Standard, pointy, curved, and double-headed arrows; straight lines; smoothed
  freehand strokes; and translucent highlighter
  strokes that automatically match and stay straight across screenshot text (with
  freehand fallback), plus hollow or filled rectangles (optionally rounded) and
  ellipses, numbered markers, editable text in Neucha, JetBrains Mono, or Inter
  Display (plain, outlined, or on a readability pill), and secure redaction with
  opaque or randomized non-spatial mosaic output.
- Per-layer preset or custom colors (including highlighter ink), undo/redo history,
  one-click whole-image or drag-region OCR (the recognized text is shown beside
  the image and copied to the clipboard),
  mesh-gradient backdrops, and rendered drop shadows on standard backdrop cards.
  The editor previews the same background padding, rounding, and shadow as the
  saved image, including when zoomed or fitted to a smaller window.
- Dragging an arrow head, tail, or bend, or a line endpoint, hides the cursor
  and drag handles for precise placement. They return on release or cancellation.
- A subtle dotted image boundary stays visible with every tool. Select mode
  brightens it and shows crop handles when no annotation is selected.
- Cut tool: drag across a band of the image to remove it and collapse the gap, with a
  live preview and dashed seam marker while dragging; annotations shift to follow.
  Moving or resizing an existing layer suspends the armed tool's action until
  release, then leaves the tool ready for the next canvas gesture.
- Pin a finished capture as a bottom-right floating compositor window, launched
  from the same `omasnap` executable and visible on every workspace.
  Pins form a compact deck with the recents shelf's alternating tilt while
  idle; hover to straighten and fan them out.
  Dropping a pin partly off-screen or underneath a bar brings it fully back
  inside the monitor where the drag started, with the same 14-pixel gap as
  the stack from any screen edge or reserved bar area.
- Crash-resistant working documents under `/run/user/<UID>/omasnap/` (falling back to
  a private `/tmp/omasnap-<UID>/`): the original source image plus a sidecar JSON
  operation log. Undo still works after a crash or `--file` reopen. Saving and
  copying write a normal flattened PNG to the clipboard or `~/Pictures/Screenshots`.
- Verified PNG clipboard output through `wl-copy`/`wl-paste`, plus timestamped files
  under `~/Pictures/Screenshots` by default.
- Open an image already on the clipboard directly in the annotation editor.
- A recents shelf: the select overlay stacks small cards of the last five captures
  along the right edge; hover to fan them out, click one to reopen it in the editor
  with its layers still editable instead of taking a new screenshot.
- Correct native-pixel export on fractional or integer-scaled monitors.
- Pins, editor controls, tooltips, and selection outlines follow the current
  Omarchy theme, including live theme changes. Screenshot pixels, annotation
  colors, and exported backdrops keep their chosen colors.

## Platform scope

The supported target is **Wayland + Hyprland**, with Omarchy as the primary integration.
The renderer, layer surface, clipboard, and monitor capture use Wayland protocols;
monitor/window discovery currently calls `hyprctl`. The focused output is captured
in-process through `ext-image-copy-capture` before the layer maps. Selection displays
that captured frame, while the annotation editor uses
a translucent layer scrim over the live desktop and draws only the selected capture.
Another Wayland compositor could support the application after supplying equivalent
monitor and window discovery; generic Wayland support is not claimed by 1.0.

Runtime commands used by the application:

- `hyprctl`
- `wl-copy` and `wl-paste`
- `tesseract`
- `omarchy-notification-send` when available; saved captures include a thumbnail and
  reopen in Omasnap when clicked. Notification failure does not invalidate output.

## Install on Omarchy

Clone the repository and run the Omarchy installer:

```bash
git clone https://github.com/tobi/omasnap.git
cd omasnap
./install-omarchy
```

The installer uses Omarchy's package helper for missing dependencies, builds in
`~/.cache/omasnap`, and installs under `~/.local`. It does not modify
Hyprland configuration.

Pinned-window placement uses the Lua dispatcher on Omarchy’s Hyprland.

### Hyprland binding

Paste this into a Lua config loaded after `require("default.hypr.omarchy")`:

```lua
hl.unbind("PRINT")
hl.unbind("F12")
hl.unbind("ALT + SHIFT + 4")

o.bind("PRINT", "Screenshot", "omasnap")
o.bind("F12", "Screenshot", "omasnap")
o.bind("ALT + SHIFT + 4", "Screenshot", "omasnap")

hl.layer_rule({
  match = { namespace = "^omasnap$" },
  no_anim = true,
  animation = "none",
  no_screen_share = true,
})
```

Each of these keys toggles: the first press opens the overlay, the next press dismisses it.

Apply and verify:

```bash
hyprctl reload
hyprctl configerrors
hyprctl binds -j | jq -c \
  '[.[] | select(.description == "Screenshot") | {modmask,key,description}]'
```

`omarchy plugin add` is intentionally not used. Omarchy plugins are Quickshell QML
extensions; they do not install native executables or system packages.

Set `OMASNAP_PREFIX` before running `install-omarchy` to use a prefix other than
`~/.local`.

### Manual Arch Linux build

Install the complete build/runtime dependency set:

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf qt6-base layer-shell-qt \
  wayland wayland-protocols hyprland wl-clipboard xdg-utils \
  tesseract tesseract-data-eng
```

Build and install:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

The install step places:

- `~/.local/bin/omasnap`
- `~/.local/share/applications/omasnap.desktop`
- `~/.local/share/licenses/omasnap/Neucha-OFL.txt`
- `~/.local/share/licenses/omasnap/JetBrainsMono-OFL.txt`
- `~/.local/share/licenses/omasnap/Inter-OFL.txt`
- `~/.local/share/licenses/omasnap/Lucide-ISC.txt`

Launch Omasnap from the application launcher by searching for its name, or use
the screenshot keybindings above.

Ensure `~/.local/bin` is on `PATH`, then verify the installed CLI:

```bash
omasnap --version
omasnap --help
```

## CLI capture modes

Running without arguments opens smart selection. Drag for a freeform region,
click a window to capture it, or click outside every window to capture the
focused monitor:

```bash
omasnap
```

Explicit starting modes:

```bash
omasnap --capture-region
omasnap --capture-window
omasnap --capture-fullscreen
```

Scroll capture stitches a region that is taller (or wider) than the screen:

```bash
omasnap --scroll
```

Drag a region, then pick a direction: **Scroll ↓ / →** scrolls the page
yourself while omasnap captures each step, and **Auto ↓ / →** scrolls it for
you, one acknowledged notch at a time, stopping when the page stops moving.
The frames are aligned and stitched into one image, copied, and shown in a timed
preview. Keep it with the pin button, `Ctrl+P`, or `T` while hovered, or open its
editor to annotate it; `Ctrl`+wheel zooms and the wheel scrolls it.

Positional capture modes are also accepted:

```bash
omasnap region
omasnap windows
omasnap fullscreen
omasnap smart
```

These options choose what is initially selected. Completing a selection copies it
and shows a preview that fades after 10 seconds of idle time unless kept. Existing
previews and pins are ordinary compositor windows and remain visible in later
screen captures; close or move them aside when they cover the next capture area.

For annotation before any output, add `--editor overlay` or `--editor window`.
You can also press `E` (edit) or `A` (annotate) in the capture picker to keep
the annotator open after the capture. Press either key again to turn it off.
The editor then controls whether the result is copied, saved, or both.

Quick output skips the preview as well as the annotation editor. Add `--copy` to copy
only, `--save` to save only, or both flags to copy and save. Region captures
output on release in Free mode or after Enter with a fixed aspect ratio;
window captures output on selection;
fullscreen captures output immediately. Quick output cannot be combined with `--file`,
`--clipboard`, or `--pin`.

### One instance, toggled by the same hotkey

Only one capture overlay runs at a time, guarded by a lock file in the runtime snapshot
directory. Starting omasnap while an overlay is open sends the running instance `SIGTERM`,
which it handles with a clean Qt shutdown; the new process then exits without capturing.
Pressing `PRINT` therefore opens the overlay and pressing it again dismisses it.

Every capture invocation dismisses this way, quick output included: `--copy`/`--save`
while an overlay is open closes the overlay and outputs nothing, rather than screenshotting
the overlay that is still on screen.

Editing an existing image is never cancelled this way: `--file`, `--clipboard`, or an
image path stops the running instance, waits up to two seconds for the lock, and opens the
editor on that image. That is how a pin's Edit button and a notification click always land
in the editor.

A lock left behind by a crashed instance is removed and reclaimed. A lock file that cannot
be read or written at all is reported on stderr instead of being mistaken for a running
instance.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | Success, including dismissing a running overlay |
| `1` | Capture, image, or single-instance lock failure |
| `2` | Usage error |

### Edit an existing or clipboard image

Point omasnap at any readable image and it opens straight into the annotation editor
with the whole image selected, skipping the screen-capture step:

```bash
omasnap ~/Pictures/Screenshots/screenshot-2026-08-11_10-00-00.png
# or
omasnap --file /path/to/capture.png
```

To open the image currently on the Wayland clipboard:

```bash
omasnap --clipboard
```

The clipboard must offer readable image data. Text-only clipboard contents return an
error instead of opening an empty editor.

File URLs are accepted too. A saved capture notification's "Click to edit" action launches
`omasnap` on the finished screenshot, so it can be reopened and re-annotated. The action is
handed to `omarchy-notification-send` as `--exec <omasnap> <file:// URL>`, separate argv
words after a trailing `--exec`, which the shell runs directly without shell parsing.

### Recent captures

Every completed capture keeps its working document, source plus operation log,
on a shelf of the five most recent under
`~/.local/state/omasnap/recent/` (`OMASNAP_RECENT_DIR` overrides). The select
overlay shows them as a small stack of cards on the right; hovering fans them out
and clicking one reopens that capture in the editor, undo history intact, in place
of a new screenshot. No annotation, Copy, Save, or pin action is required: the
shot remains available after its floating preview expires or closes. Dismissing
the editor with `Esc` also remembers its current edits. Editing the same shot
updates its existing entry; cancelling before selecting a capture adds nothing.

### Theme

Omasnap reads the current Omarchy palette from
`~/.local/state/omarchy/current/theme/colors.toml` and uses the surface, control,
tooltip, and border colors in `shell.toml` when present. Theme changes update
open windows automatically; missing or invalid values use readable defaults.
Chrome fonts stay pinned, and loading colors does not load a desktop Qt theme
plugin or add a startup dependency.

### Configuration (optional)

Omasnap has no settings UI and runs fine with no config at all. If you want to
change where screenshots land or what they are called, create
`~/.config/omasnap/omasnap.conf` (INI format); every key is optional:

```ini
[editor]
# overlay (default): the editor fills the screen as a fullscreen overlay.
# window: the editor opens as a normal compositor window, tiled or floated
# by the compositor, so a capture can be annotated next to another window.
# W switches a live editor between the two either way, and --editor
# window|overlay overrides this per invocation.
# This chooses the on-demand editor's presentation; fresh captures still
# copy and show a timed preview unless --editor is explicitly passed.
mode = overlay
# floating (default): a windowed editor asks the compositor to float it at
# the capture's natural size. tiled: it joins the tiling layout instead.
window = floating
# opaque (default): a windowed editor paints a solid backdrop.
# translucent: it keeps the overlay's see-through dim.
backdrop = opaque

[output]
# Where saved screenshots go. Default: ~/Pictures/Screenshots
directory = ~/Pictures/Captures
# Filename pattern, without extension (.png is appended).
# Default: screenshot-{date}_{time}-{app}
filename = screenshot-{date}_{time}-{app}

[colors]
# Up to eight preset colors for the palette, and the initial custom color.
palette = #ff375f, #ff9f0a, #ffd60a, #30d158, #0a84ff, #bf5af2, #000000, #ffffff
custom = #ff375f

[background]
# An image used as a "Custom" backdrop. B (or the toolbar button) cycles
# through the four gradients, this image when readable, shadowed/flat window
# gray, and Off.
image = ~/Pictures/backdrops/desk.jpg
# Style a fresh capture starts with: none, off, slate, aurora, sunset, lagoon,
# violet, or custom. `none` still allows Framed canvas growth to add its
# automatic window-gray mat; `off` stays transparent. `custom` only takes
# effect once `image` above loads successfully.
default = custom
```

Filename tokens:

| Token | Expands to |
|---|---|
| `{date}` | `2026-08-23` (yyyy-MM-dd) |
| `{time}` | `14-05-09` (HH-mm-ss) |
| `{app}` | Slug of the app under the selection, e.g. `firefox`, `alacritty`, `nautilus` (from the Hyprland window class). Empty for fullscreen captures, file edits, and when nothing is known — the separator before or after it is dropped too, so the default pattern gives `screenshot-2026-08-23_14-05-09.png`. |

The default keeps the date first so the folder always sorts chronologically:
`screenshot-2026-08-23_14-05-09-firefox.png`. Anything else in the pattern is
literal text (`screenshot-` is just a string). A name that already exists
gets `-2`, `-3`, … appended.

Environment overrides (`OMASNAP_SCREENSHOT_DIR` takes precedence over the config):

```bash
OMASNAP_SCREENSHOT_DIR="$HOME/Pictures/Captures" omasnap
OMASNAP_OCR_LANGS="eng+deu" omasnap
# Thai plus English:
OMASNAP_OCR_LANGS="tha+eng" omasnap
```

Install the corresponding Tesseract language data before adding a language to
`OMASNAP_OCR_LANGS`. When unset, omasnap falls back to Omarchy's
`OMARCHY_OCR_LANGS` (which commonly includes the user's script, e.g.
`tha+eng`), then to `eng`.

## Controls

The capture picker and fullscreen annotator show a readable shortcuts card in
the lower left. Press `?` or click its **Shortcuts** header to collapse or expand
it. The card scrolls on shorter screens; `?` still types normally in a text
annotation. The windowed editor keeps its guide above the toolbar.

### Capture selection

The default smart picker infers the capture kind from the gesture: drag for a
region, click a window for that window, or click open space for the full
focused monitor. In **Free** mode, releasing the mouse captures immediately.
With a **fixed aspect ratio**, releasing leaves an adjustable box: drag inside
it to move it or drag a corner handle to resize it, then press **Enter** to capture.
**Esc** cancels an in-progress adjustment or clears a completed selection;
clicking outside the box starts a new region. Window and monitor clicks capture
immediately. Whatever is lit is what will be captured.

Press `S` before drawing to select a scrolling region; after release in Free
mode or **Enter** with a fixed ratio, the page inside it goes live and the scroll
controls appear in place. A small **Scroll capture** button under an image
already open in the editor turns that region into a scrolling capture. Explicit `region`, `windows`, `fullscreen`, and
`scroll` command-line targets remain available for scripts and keybindings.
While scrolling, use the on-screen Done and Cancel buttons, or `Enter` and
`Esc` when the overlay has keyboard focus. Move the pointer back over the
controls to return focus from the live page.

| Input | Action |
|---|---|
| Click | In smart mode, capture the window under the pointer, or the full monitor outside any window |
| Drag | Free: release to capture. Fixed ratio: release to adjust, drag inside to move or a corner to resize, then Enter to capture |
| `F` / `Shift+F` | Cycle aspect ratio forward/backward before, during, or after a region drag: Free, 1:1, 16:9, 16:10, 4:3, 3:2, 9:16, 10:16, 3:4, 2:3 |
| `Ctrl+F` | Reset aspect ratio to Free, including during a drag |
| `S` | Toggle scrolling-region mode |
| `E` / `A` | Toggle annotation after capture; the capture guide shows on/off, and the choice also applies to scrolling captures |
| `R` | Restore the last drawn region, including from a previous Omasnap launch in this login session (same monitor and overlay size); adjust it and press Enter |
| `SUPER + Arrow` | Move among windows in window mode |
| `Enter` | Capture the adjusted region or highlighted window |
| `Ctrl+A` | Select the full focused monitor |
| Hover the right-edge stack | Fan out the five most recent captures; click one to reopen it |
| `Esc` | Undo an in-progress adjustment, clear a drawn region, or dismiss the picker |

Aspect-ratio drawing keeps the starting corner fixed and fits inside the mouse
drag using both axes. The real pointer can move outside the constrained box.
The guide shows the active ratio, and each launch starts Free. For scrolling
captures, the ratio constrains only the initial region.

Region memory is stored in Omasnap's private runtime directory. It survives
closing and reopening Omasnap and lasts until those runtime files are removed
(normally when the login session ends). A region from another monitor, a
different overlay size, or outside the current screen is ignored; draw a new
region after changing the display layout.

### Annotation editor

| Input | Action |
|---|---|
| `V` | Select/move/resize layers; carrying one past the source grows the canvas; drag empty canvas for a marquee; multi-select outlines each layer without treating the canvas as one layer; wheel scales the selected layer |
| `A` | Arrow; press again to cycle Standard, Pointy, Curved, and Double styles |
| `S` | Spotlight/loupe; press again to cycle ellipse, rectangle, rounded |
| `L` | Straight line |
| `F` | Freehand pen; medium `3/6` smoothing by default, while `0/6` preserves the raw pointer path |
| `H` | Highlighter; Snap mode uses a mouse-following I-beam at the nearby text height, then locks the drag straight to that row. Press `H` again (or click the active toolbar button) for Normal freehand mode, where wheel or `Alt`+wheel changes thickness; Snap keeps detected-row height automatic and wheel sets only its off-text fallback |
| `I` | Eyedropper in the color popover · sample the image as the custom color |
| `C` | Numbered marker |
| `R` | Rectangle; hover the shape button for rectangle, ellipse, and fill controls; `Alt`+wheel rounds corners |
| `E` | Ellipse; shares the shape submenu and filled/hollow toggle |
| `D` | Redact; press again to toggle randomized pixelation or solid redaction |
| `X` | Cut out a band; drag to preview the crossed-out strip, then release to remove and collapse it |
| `T` | Text on a cream readability pill, with Neucha as the default. Click for a one-line label, or drag a box to give it room for several lines: Enter moves to the next line while there is room and commits on the last one; `Shift+Enter` always adds a line; `Esc` commits the text and dismisses the annotator; long text wraps at the current canvas edge by default, while moving it or dragging its width handle beyond that edge expands the canvas; clicking away keeps the text; press T again to toggle the pill |
| `Shift+T` | Cycle the next or selected text through Neucha, JetBrains Mono, and Inter Display |
| `O` | Recognize and copy all text in the current image |
| `B` | Cycle shadowed colors, window gray (shadowed and flat), and Off |
| `Shift+B` | Toggle the screenshot card's drop shadow; on by default |
| `G` / `Shift+G` | Cycle canvas boundaries forward/backward: Framed, Overflow, Image. Framed auto-grows with the normal frame; Overflow grows only the sides needed by annotations with no frame; Image clips at the original screenshot edge |
| `W` | Re-present the editor as a normal compositor window, or back as the fullscreen overlay; selection, layers, and undo history carry over |
| `1`–`8` | Set annotation color; `7` is black and `8` is white |
| Wheel | Scale selected layer, magnify the spotlight under the cursor, or change active tool size (`Alt`+wheel: selected pen smoothing from 0–6, the next pen's smoothing when none is selected, rectangle corner radius, or spotlight border); while just viewing a zoomed capture, scroll it like a document |
| `Shift`+wheel | Scroll a zoomed capture sideways (a wide stitch); never changes the zoom |
| `Ctrl`+wheel · middle-drag | Zoom about the cursor · pan by dragging |
| `+` / `-` / `0` (also with `Ctrl`) | Zoom in / out / fit |
| Hold `Shift` while dragging | Make rectangles, ellipses, and spotlights 1:1; snap line and arrow endpoints to 45°; keep curved-arrow bends centered; while dragging a selected layer's handle, keep a rectangle, redaction or spotlight's aspect ratio |
| Hold `Alt` while dragging | Center rectangles, ellipses, and spotlights on the press point; add `Shift` for a centered square/circle |
| `←` `↑` `→` `↓` | Nudge the selected layer 1 px; hold `Shift` for 10 px (a held key is one undo step). With nothing selected, pan a zoomed capture |
| Double-click text · `Enter` on a selected text | Reopen text editing |
| `Delete` | Delete selected layer |
| `Alt+D` | Duplicate selected layer (offset down-left, or away from a nearby edge); the copy becomes the selection |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z`, `Ctrl+Y` | Redo |
| `Ctrl+C` | Copy PNG only |
| `Ctrl+S` | Save PNG only |
| `Enter` | Copy and save (with a text layer selected: edit it) |
| `Ctrl+P` / `P` | Keep the capture pinned on screen and close the editor |
| `Esc` / `Super+W` | Dismiss the annotator; keep an originating pin in place with its edits and undo history |
| Right-click | Return to Select; cancel active drawing |

### Capture previews and pins

Normal captures show a preview that fades after 10 seconds of idle time, replacing
the completion notification. Hovering the stack and in-progress
actions pause the countdown; it resumes when the preview is idle again. Clicking,
scrolling, copying, or editing does not pin the preview. The pin button,
`Ctrl+P` on a focused preview, or `T` ("tack") while hovered keeps it until closed.
Dragging the preview also pins it, even if the drag only reorders the stack.
`T` remains the Text shortcut in the editor. Kept shots show a
pin icon in the active window-border color, even when the other controls are
hidden. Unpinning starts a fresh
10-second countdown.
New captures always go in front of the existing stack, including kept shots.
Opening Edit leaves the preview's expiry policy unchanged; pin it first to keep
the preview available throughout annotation.

In the editor, `Ctrl+P` or `P` renders a capture that stays pinned. It writes a
`pin-<pid>-<n>-<random>.png` under the runtime snapshot directory, and launches
the same `omasnap` executable in
detached pin mode. Hyprland floats and pins each window on every workspace.
Idle pins overlap in a compact deck at the focused monitor's bottom-right
corner, newest in front. The front card stays straight; the cards behind it
alternate the same growing lean as the recents shelf: −3°, +6°, −9°, +12°.
Omasnap paints the rounded frames with the images so their edges tilt together,
with transparent corners that take no input. Theme changes keep this single frame
without adding a second compositor outline.
Hover to straighten and fan them upward into fully exposed cards, wrapping into
further columns when needed. The front card stays anchored; moving between cards
keeps the fan open, and leaving folds it after a short delay. Placement accounts for
monitor origins, scaling and rotation, respecting bars on any edge and leaving
a 14-pixel gap inside the usable area. It reserves each new target while the
compositor animates it. If no on-screen slot fits, automatic packing leaves the window
where the compositor placed it.

The preview is 200 logical pixels wide with the display's aspect ratio (height
clamped to 50–400 pixels). It fills that frame with a top-anchored cover crop;
copy, edit and drag-out still use the complete full-resolution image. Drag the
image background, or use `Super`+left-drag, to move a pin. Dragging over the stack
opens an insertion gap, and releasing snaps it into that gap. The fan stays open
during a drag. Moving or closing a stacked pin closes the gap; pins dragged entirely clear of the stack
stay freely placed and are left alone when the stack folds or opens.
A drop partly outside the usable area returns fully inside the monitor where the
drag started, keeping the same 14-pixel edge gap.

Pinning neither touches the clipboard nor writes to the screenshot directory; it is a
fourth output alongside copy, save, and copy-and-save. `Ctrl+P` / `P` closes the editor and releases
the single-instance lock immediately. Pins from separate captures accumulate as independent
processes.

Hover the pin to reveal its controls and use its keyboard shortcuts; the cursor
becomes a pointing hand over each button. **Edit** and **Copy** sit in the center
of the image, with text labels and no tooltips. The pin button sits beside **×**
at the top-right; the drag handle, file-path button, and folder button sit at
the top-left.
Icon buttons use compact, dark tooltips for their actions and shortcuts.
Pins follow normal mouse focus while hovered and keep focus with the current
app when first created.
Closing an active or hovered pin focuses the next pin on that monitor, starting
with the front of the remaining stack, so repeated `X` presses dismiss them
without needing another mouse movement. Opening a pin for annotation keeps
the pin in place and gives focus to the editor. One `Esc` dismisses the editor
and updates that same pin, including any text being typed. Reopening it restores
the editable layers and undo history. `P` / `Ctrl+P` in that editor returns to
the existing pin too.
While the overlay is open, a compositor close aimed at a pin (including stock
`Super+W`) dismisses the overlay as `Esc` would, leaving the pins in place.
Automatic expiry compacts the stack without transferring keyboard focus.

| Input on a pin | Action |
|---|---|
| Pin button, `Ctrl+P` while focused, `T` while hovered | Keep on screen; press again to unpin and restart the countdown |
| Drag the image background, `Super`+left-drag | Move the preview and keep it on screen, including when reordering the stack |
| Edit button, `A` / `E` while hovered | Annotate the capture while keeping the same pin |
| Link button, `L` / `F` while hovered | Save the capture if needed and copy its file path |
| Folder button, `R` while hovered | Save the capture if needed and show it in the default file browser |
| Copy button, `C` while hovered, `Ctrl+C` | Copy the full-resolution PNG |
| Top-left six-dot drag handle | Drag the PNG into a file-capable drop target |
| Wheel | Keep the fixed preview size |
| Close button, `X` / `Super+W` while focused, `Esc`, middle-click | Close and focus the next pin |

Image and path copying use `wl-copy` rather than `QClipboard`, so clipboard data remains
available after the pin is closed. Copying a temporary capture's path or showing
it in its folder first saves its PNG in the configured screenshots directory.
Both actions reuse that file;
closing or expiring the preview leaves the saved copy available. A pin opened from
an existing file copies that file's original path.
The folder button follows the default `inode/directory` application. It asks
that application to select the screenshot through `FileManager1.ShowItems` when
supported, otherwise opens the containing folder with `xdg-open`.

Hyprland placement uses runtime dispatches and
requires no user window rules. The controls use the annotation toolbar’s vector
icons, including Lucide’s pin drawn directly by the renderer.

Canvas boundary changes affect only preview and export clipping. The complete vector
geometry stays in the operation log, so switching back to Grow restores every off-canvas
part of a layer.

Creation tools return to Select after one placement without selecting the new layer. In
Select mode, lines and straight arrows show two endpoint handles; curved and double arrows
add an on-curve handle for bending the arc (hold `Shift` to keep that bend centered). Other
layers show a selection boundary. The eight handles outside the image recrop
its corners or edges. The image stays in place while dragging a crop handle, then
re-centers when released. After the canvas grows, those crop handles remain on the
original source frame.

## Development and verification

```bash
make check
```

The smoke executable exercises smart/region/window/fullscreen startup modes, capture selection,
working-document persistence (source plus op-log JSON), annotation tools, undo/redo
replay, vector movement and scaling, text editing, OCR, native-DPI output,
endpoint-only line selection, annotation-driven canvas growth and clipping policies,
external crop handles,
and the native-pixel
measurement readout on a scaled monitor.

For live launch profiling, the binary has an opt-in millisecond trace from `main()`
through the first completed overlay paint:

```bash
OMASNAP_PROFILE_STARTUP=1 ./build/omasnap 2>startup.log
```

The trace also breaks native capture into Wayland registry, buffer allocation, frame wait,
and pixel handoff stages, and marks output readiness separately from subsequent
recent-history persistence. It is completely silent by default.

`.github/workflows/build-linux.yml` runs the same `make check` build, interaction smoke,
and available static-analysis checks in an Arch Linux container, stages the CMake installation, and uploads a versioned Linux
artifact. A `v*` tag also attaches that artifact to the corresponding GitHub release.

## Acknowledgements

The capture and annotation workflow is inspired by three excellent screenshot tools:

- [Shottr](https://shottr.cc/) — fast region/window capture, OCR, and polished backdrops.
- [Satty](https://github.com/Satty-org/Satty) — a focused, Wayland-native annotation workflow.
- [Flameshot](https://github.com/flameshot-org/flameshot) — selection-first capture and an
  approachable annotation toolbar.

Thanks to their authors and contributors for establishing the interaction patterns that made
this project possible. Omasnap is an independent implementation and is not
affiliated with those projects.

## Project history

This standalone repository was extracted with `git filter-repo` from the original Omarchy
system-customization repository. The former `omasnap/` directory was promoted to
the repository root while retaining its relevant commit history.

The bundled Neucha, JetBrains Mono, and Inter Display fonts are distributed
under the SIL Open Font License. Their provenance and hashes are recorded in
`assets/FONTS.md`; their licenses are installed with the application.
