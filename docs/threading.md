# Threading: the main thread never blocks

Omasnap is a layer-shell overlay. The instant it stops painting — even for
one dropped frame — it looks broken, because there is nothing else on
screen to explain the freeze. So the rule is absolute: **the UI thread does
capture, paint, and input handling, and nothing else.** Anything that can
take more than a frame (disk I/O for a full-resolution image, spawning a
process, PNG encoding, network of any kind) runs off it.

## The pattern

Every background operation in the codebase follows the same shape:

1. Copy the small amount of state the worker needs by value into a lambda
   (`CaptureData`, an image, a path). Qt's implicit sharing makes this
   cheap; it also means the worker never touches `this` while the UI thread
   might be mutating it.
2. `QtConcurrent::run(...)` the lambda on Qt's global thread pool.
3. A `QFutureWatcher` connected on the UI thread picks up the result via a
   queued `finished` signal and applies it — never the other way around.
4. A `busy_`-style flag (or a more specific one) blocks reentrancy while the
   watcher is in flight, and the status pill says what is happening.

`src/editor.hpp` keeps dedicated watchers, each the entry point for
reading its corresponding worker:

| Watcher | Worker does |
|---|---|
| `captureWatcher_` | Reads window/monitor pixels via `captureMonitorPixels` |
| `ocrWatcher_` | Renders the OCR crop and runs `tesseract` |
| `finishWatcher_` | Renders/encodes the export, copies/saves/launches the preview, then records the recent document |
| `snapshotWatcher_` | Writes the crash-recovery working snapshot + operation log |
| `pinWatcher_` | Renders and launches a pinned compositor window, then records the capture |
| `recentsWatcher_` | Lists and decodes thumbnails for the recents shelf |
| `backdropWatcher_` | Decodes an optional user-supplied backdrop image |
| `highlighterProbeWatcher_` | Detects a nearby screenshot text row for highlighter Snap mode |

Editor dismissal also uses a worker, tracked by `dismissFuture_`, to return edits
to an originating pin and retain the recent document. Output workers publish
their result through `QPromise` as soon as output is ready: `resultReadyAt` closes
the overlay before full-monitor history compression. The worker continues saving
the pristine source/log/thumbnail, and editor destruction drains it after the
window has closed. Teardown waits on futures without dispatching GUI events.
On a failed recent write after successful output, the worker drains autosave
and attempts to persist the final pristine source and log at the existing
working paths, retaining them instead of deleting the recovery document.
`main()` releases the instance lock first, so a rapid second
capture cannot terminate the pending save or be mistaken for cancelling an overlay.

A per-capture reservation is acquired before launching/updating the preview.
Immediate Edit and shelf-loading workers wait for it to finish, preserving the
original layers even while history is being encoded. Different captures encode
independently; only thumbnail publication and pruning use the shared shelf lock.
The thumbnail is published last, after its source and log are complete. No
full-resolution file is moved or PNG-encoded in the completion signal handler.

`src/scroll-capture.cpp` follows the same rule with a plain `QFuture<void>`:
the capture loop (grab → crop → classify → accumulate) runs on a worker
thread so the overlay keeps painting the live page and the mode pills while
frames come in, however slow the compositor's damage-driven capture is.

`ChromeThemeWatcher` in `src/chrome-theme.cpp` reads and parses Omarchy's theme
files on a worker too. The GUI applies a completed palette and repaints open
windows; paint handlers only read in-memory values. Debounced filesystem
notifications trigger reloads, including when the whole theme directory is
replaced, so idle windows do not poll the filesystem.

## What this buys, concretely

- **OCR**: whole-image or drag-region text recognition spawns `tesseract`
  and renders a full-resolution crop, both off the UI thread, with a
  scanning animation over the region so the wait reads as progress rather
  than a hang.
- **Export**: a stitched scroll capture can be 25,000 pixels tall. PNG
  encoding that image, plus the `wl-copy`/`wl-paste` verification round
  trip, is seconds of work — all in `finishWatcher_`'s worker. See
  `CaptureEditor::finish()`.
- **Scroll capture**: reading the screen back after every wheel tick, at
  whatever cadence the page's animation settles at, never stalls painting
  the overlay's own chrome.

## Pointer motion on large monitors

Input and `QWidget` painting necessarily share Qt's GUI thread, but pointer
motion must not turn into a full-surface render. This matters on a 6K display:
a single full ARGB frame is over 80 MB before compositor copies.

`CaptureEditor` therefore treats pointer chrome as damaged regions. Crosshair
lines, measurement badges, toolbar hover, annotation previews, and drag shapes
invalidate only their old/new pixels; Qt's backing store preserves the rest.
High-rate mouse samples are coalesced to at most one repaint per 16 ms. The
text-aware highlighter is stricter still: scanning screenshot pixels happens
through `highlighterProbeWatcher_`, and mouse-down uses the latest completed
probe rather than scanning in the input handler. Reintroducing a bare
`update()` in `mouseMoveEvent`, or image analysis from `updatePointerCursor()`,
turns that bounded path back into a full-display stall.

The other half of that bargain is that damage must cover every pixel
`paintEdit()` would draw differently, or the backing store keeps a stale patch
beside fresh ones: a torn outline, a block of backdrop behind a carried layer.
So damage is derived from what paints rather than modelled beside it.
`pointerMotionRegion()` reads the same `liveLayers()` that `paintEdit()` draws,
takes ink extents from `annotationPaintedBounds()` (the bounds that grow the
canvas), adds the selection chrome around a carried layer, and
`liveCanvasDamage()` repaints the mat when carrying a layer grows or shrinks
the canvas being previewed (only the strips that differ for a flat mat, all of
it for a gradient laid out afresh), or the whole canvas when a spotlight's
dimming covers it or switches on: that happens on a pointer move, with the
first pixel of a lens being dragged out, not on the press.
`QWidget::grab()` repaints everything and so can never see a missed pixel; the
smoke suite compares it against the backing store in the middle of a drag.

Bounded damage only helps if painting is bounded too. A spotlight magnifies
the composed canvas (mat, shadow, redacted image), and composing all of that at
display resolution cost tens of milliseconds on every paint however small the
damage. `paintEdit()` composes just the patch `spotlightSampleBounds()` says
the lenses read, which is a fraction of their own area.

## The one documented exception

Before any window exists — single-instance handover in `src/instance-lock.cpp`,
and the instant `--fullscreen --copy`-style quick output path in `main()`
(`quickOutput()`, called before `QGuiApplication::exec()` even runs) — there
is no live, painted surface to keep responsive, so a bounded synchronous
wait is fine. The rule is about not freezing something the user is looking
at; a CLI-style path that exits before showing anything doesn't have that
problem. Don't extend this exception to anything that runs after a window
is visible.

## Self-violations found and fixed

Two places broke this rule despite being written after the pattern was
established, which is worth remembering: the pattern has to be followed on
purpose every time, since nothing enforces it automatically.

- **`CaptureEditor::reopenRecent()`** loaded a shelved capture's full-resolution
  source with a synchronous `QImage::load()` directly in the shelf's click
  handler — for a stitched scroll capture, tens of megapixels, on the UI
  thread. Fixed to decode on the worker pool (`reopenWatcher_`), the same
  shape as every other watcher above.
- **`CaptureEditor::pinSnapshot()`** rendered the pin image on a worker but
  then PNG-encoded and wrote it to disk in the `pinWatcher_::finished` slot —
  back on the UI thread, after the watcher had already proven the async
  shape was easy to reach. Fixed to do the encode+write inside the same
  worker lambda as the render and process launch, so the slot only closes
  the editor or displays an error. Automatic copy-and-preview output uses the
  same worker helper, including clipboard verification.

## Auto-scroll startup

Auto-scroll Start and Continue run `spawnScrollInjector()` on a setup worker,
including the natural-scroll subprocess query and Wayland/uinput probes. A
watcher starts capture or reports setup failure on the GUI thread. Each attempt
owns copied inputs and its own shared stop token; Back, Cancel and destruction
cancel it without waiting for setup. A stale completion cannot start capture in
a later session. The injection thread checks cancellation during its initial
settle, before parking or nudging the pointer.

Backend setup and injection still need live Hyprland verification when changed:
offscreen tests cover responsiveness, cancellation, stale completions and panel
lifetime, but cannot prove that compositor input reaches the underlying page.

## Adding new work

If you're adding an operation that touches disk, spawns a process, or does
anything non-trivial with an image, it does not go on the UI thread. Follow
the existing watchers as templates — the shape (copy in, run, watch,
apply) is the same every time, on purpose, so new code doesn't invent a
seventh way to do it. If a signal needs to fire when the worker is truly
finished, remember `QFutureWatcher::finished` is a queued connection: it
does not race obtaining a "the watcher is running" check made moments
earlier in the same call.

See also [editing-model.md](editing-model.md) for what state a background
render is allowed to read, and [dependencies.md](dependencies.md) for the
processes (`tesseract`, `wl-copy`/`wl-paste`, `hyprctl`) these workers spawn.

## Floating pins

Pin placement, compositor polling, and move dispatches run on a single worker
per pin process. The GUI applies completed geometry snapshots through a watcher;
it never waits for `hyprctl` during a drag. A runtime lock serializes placement
across pin processes, with short-lived target reservations covering compositor
animation latency. The initial monitor query uses the same worker pool; a fallback frame maps
immediately and adopts the display-shaped size when the query finishes.
Clipboard actions, saved copies for path sharing or revealing in a file browser,
default-browser discovery and launch, editor launches, and drag
payload preparation also run on workers. Final drag placement reports completion
and retries a failed move;
after repeated failures, the remaining stack closes the insertion gap.
For compositor-initiated Super drags, the same geometry watch is armed while
Super is held over a pin. Motion during that drag is not mistaken for release;
the mouse release or releasing Super completes placement. There is no permanent
polling timer on idle pins and no global shortcut registration.

Hovering a deck starts a single fan watch, owned by the last pin entered. Its
worker samples the pointer while the fan is open, including the gaps between
windows, and stops when the deck folds. The runtime placement transaction shares
hover ownership and drag state so other pin processes cannot fold a live drag.
It also records freely placed pins, which must stay free even when aligned with
the screen edge. Fan moves use compositor animations and preserve the native
windows; folding restores their stacking order without a focus dispatch.
Closing an active pin compacts the deck and focuses the next pin on the same
monitor in that worker transaction. Opening annotation leaves the pin alive,
so there is no closing-pin focus transfer to compete with the editor.
The same transaction publishes each card's tilt. A filesystem watcher triggers
a small worker read when that state changes; it adds no idle polling. The UI
animates the painted card and its input region inside the existing window bounds.
Pin frames are drawn with their images so the outline can rotate too; the runtime
pin rule disables the compositor's rectangular border, shadow and background blur.
The initial placement worker also sets these as per-window properties, preserving
the frameless surface across compositor and theme reloads without polling.

Returning from annotation renders and saves the pin preview and operation log
on a worker. A filesystem watch on the completed log starts a worker to decode
the new preview and prepare its drag payload. The GUI updates the image inside
the existing window; its compositor position and stack membership are unchanged.
A nonblocking local socket routes compositor close requests from pins underneath
the overlay to its Escape handler. It is present only while the overlay is shown;
pin expiry and explicit pin actions do not use it. Returning the document still
runs on the existing worker, and repeated close requests cannot interrupt it.

Normal previews use a one-shot ten-second timer and a short paint-opacity fade.
Explicit pin actions and moving a preview disable that timer. The existing drag
watch detects movement, including a quick drag seen only in its final snapshot.
Hover, shared stack activity, file sharing drags, and pending actions pause the
remaining time without changing pin state.
Unpinning restarts its countdown, paused until ongoing
interaction finishes. Expiry compacts the stack on the placement worker and never issues
a focus transfer. No extra compositor polling or process is needed for the fade.

## Pen smoothing budget

Release-time smoothing bounds the iterative RDP pass to 32,768 point-to-segment
comparisons over at most 2,048 samples. When that budget runs out, unexamined
spans retain their samples; no quadratic scan continues on the input thread.
At most three Chaikin passes then produce 16,384 points. The initial arc-length
resampling remains linear in the raw stroke length.
