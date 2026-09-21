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
| `finishWatcher_` | Renders the export, encodes PNG, does the clipboard round trip, moves the file |
| `snapshotWatcher_` | Writes the crash-recovery working snapshot + operation log |
| `pinWatcher_` | Renders the image for a pinned compositor window |
| `recentsWatcher_` | Lists and decodes thumbnails for the recents shelf |
| `backdropWatcher_` | Decodes an optional user-supplied backdrop image |
| `highlighterProbeWatcher_` | Detects a nearby screenshot text row for highlighter Snap mode |

`src/scroll-capture.cpp` follows the same rule with a plain `QFuture<void>`:
the capture loop (grab → crop → classify → accumulate) runs on a worker
thread so the overlay keeps painting the live page and the mode pills while
frames come in, however slow the compositor's damage-driven capture is.

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

## A known violation, not yet fixed

`spawnScrollInjector()` (`src/scroll-inject.cpp`) is called synchronously
from the UI thread when auto-scroll starts or Continues
(`ScrollCapturePanel::startCapture`/`continueCapture` in
`src/scroll-capture.cpp`), and it deliberately probes the injection
backends before returning — including `hyprctl getoption
input:natural_scroll`, a subprocess spawn with up to a 2-second
`waitForFinished`. That's a real, if brief and infrequent (once per
auto-scroll start, not per frame), block on the UI thread. It hasn't been
moved to a worker because the auto-scroll injector is the most delicate,
most recently hardened part of the codebase and depends on live
Hyprland/Wayland state that the offline smoke suite cannot exercise —
changing its threading needs a live re-verification pass, not just a
green `make check`. Fix it with the same worker/watcher shape above
(`QtConcurrent::run` wrapping the whole call, a small watcher applying the
result) when you can test it live.

## Delayed capture

`--delay` runs a precise timer in the application's event loop before any
window is created or pixels are read. The existing signal notifier and
single-instance lock stay active throughout the wait. A second invocation
cancels the pending capture. No countdown surface is mapped, so the delay
cannot steal focus from menus or contribute pixels to the screenshot.

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
Clipboard actions, saved copies for path sharing, editor launches, and drag
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

Returning from annotation renders and saves the pin preview and operation log
on a worker. A filesystem watch on the completed log starts a worker to decode
the new preview and prepare its drag payload. The GUI updates the image inside
the existing window; its compositor position and stack membership are unchanged.
A nonblocking local socket routes compositor close requests from pins underneath
the overlay to its Escape handler. It is present only while the overlay is shown;
pin expiry and explicit pin actions do not use it. Returning the document still
runs on the existing worker, and repeated close requests cannot interrupt it.

Normal previews use a one-shot ten-second timer and a short paint-opacity fade.
Only explicit pin actions disable that timer. Hover, shared stack activity,
drags, and pending actions pause the remaining time without changing pin state.
Unpinning restarts its countdown, paused until ongoing
interaction finishes. Expiry compacts the stack on the placement worker and never issues
a focus transfer. No extra compositor polling or process is needed for the fade.

## Pen smoothing budget

Release-time smoothing bounds the iterative RDP pass to 32,768 point-to-segment
comparisons over at most 2,048 samples. When that budget runs out, unexamined
spans retain their samples; no quadratic scan continues on the input thread.
At most three Chaikin passes then produce 16,384 points. The initial arc-length
resampling remains linear in the raw stroke length.
