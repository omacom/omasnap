# Editing model: everything is undoable, nothing is baked in until output

## The operation log is the source of truth

The image on screen while editing is not the working state — the operation
log (`ops_` in `CaptureEditor`, an ordered `QVector<Operation>` with an
`opIndex_` cursor) is. Every action that changes the picture — a crop, a
background choice, adding/patching/deleting an annotation, a cut — appends
one `Operation`. Undo moves `opIndex_` back; redo moves it forward.
`CaptureEditor::replayLog()` rebuilds the entire visible state (selection,
background, annotation list, cuts) by replaying the log from empty up to
`opIndex_`, every time. There is no separate "current annotations" model
that the log merely mirrors — the log **is** the model.

This is why undo is exact and unlimited within a session, why the crash
recovery snapshot (`snapshotWatcher_`) can save just the source image plus
the JSON-serialized log and reconstruct everything on reopen (see
`recent-snaps.hpp`), and why new annotation kinds or edit actions must be
expressed as a new `Operation::Type` (or a field on `Annotation`) rather
than a side channel of editor state — anything that bypasses the log breaks
undo, crash recovery, and the recents shelf simultaneously.

## Rendering is a pure function of the log, until you export

`renderCapture(capture, selection, annotations, backgroundStyle)` in
`src/capture.cpp` takes the pristine source image and the current vector
state and produces a flattened `QImage`. Output workers call it when they
need a complete raster. Interactive painting follows the same layer order
but keeps the unannotated/redacted display base cached and paints vectors
only into Qt's damaged region; pointer motion on a 6K display must not
flatten or repaint the full capture. **Nothing is baked into the working
image as you draw.** Add a rectangle, change your mind, delete it — the
source pixels underneath were never touched.

Completing a fresh capture copies it and shows a timed preview by default. The
preview fades after 10 seconds of idle time; its pin button, Ctrl+P, or moving the
preview keeps it, including a drag that only reorders the stack.
`--editor overlay` or `--editor window` opens annotation before output; `E` or
`A` in the capture picker toggles the same behavior for that capture, including
scrolling captures. A pin's Edit button also opens the editor on demand.
During editing, output happens on **Copy**,
**Save**, or both together (`CaptureEditor::finish()`), plus pinning a
snapshot or returning edits to an existing pin. Each render runs off the UI thread (see
[threading.md](threading.md)), and writes the result. Until one of those
happens, everything remains a log entry you can undo.

Copy/Save can optionally downscale this completed raster to logical size
(`output/logical_size`). Resizing runs after rendering, including redaction,
on the output worker. It never changes the working source, operation log,
OCR input, or pinned image; native-resolution exports remain the default.
Working documents retain the original export scale in the log, so rounding
their logical presentation size does not change output size after reopening.

Every completed capture also keeps a private source, operation log, and rendered
thumbnail in the five-entry recents shelf, including untouched timed previews,
quick output, explicit pins, and editors dismissed with `Esc`. Preview expiry
only removes the runtime preview. The next overlay can reopen its recent card
directly for annotation. A capture identity survives preview editing and editor
handoffs so completing it again updates one entry and retains editable layers.
Shelf writes, thumbnail rendering, and pruning continue on the completion worker
after the preview appears and the overlay closes. Immediately reopening a preview
waits for its pending document on a worker so the original layers remain editable.

Opening a pin for annotation leaves its compositor window in place. A private
copy retains the pristine source and operation log; `Esc` commits any text draft,
dismisses the annotator, and updates the pin's rendered preview. Reopening reads
the source and log, so undo still works. Copying, dragging, or revealing the pin
shares only the rendered preview. The original user file is never overwritten,
and the last pin/editor owner removes the private source, log, and preview together.

Scrolling captures retain the monitor's scale when they become editable
documents. Their native pixels stay intact; only the logical presentation size
changes. Loaded documents also retain their exact pixel dimensions on export,
even when the logical size was rounded for a scaled display.
Flattened pins and previews record the logical size of the entire rendered
image, including its backdrop and expanded canvas, so reopening them preserves
their proportions. The editable source's log still uses its original coordinates.

## The two exceptions, and why they're still safe

Two operations *do* need to touch real pixels before export, and both are
still fully undoable because of how they're kept in the log:

- **Cut** (`Operation::Type::Cut`) removes a band of the image and shifts
  everything after it. The working image (`capture_.source`) after a cut is
  `composeCuts(pristineSource_, cuts_)` — recomputed from the untouched
  original plus the list of cut ops every time the list changes
  (`CaptureEditor::refreshComposedCapture()`). Undo a cut and the composed
  image is rebuilt without it; `pristineSource_` was never modified.
- **Redaction** exists to permanently destroy sensitive content, so it is
  the one place where "non-destructive until export" would be a bug, not a
  feature: `renderCapture` applies redactions to the cropped pixels
  **before** painting ordinary vector annotations, and does so on every
  render, not just the final export. A redaction annotation is still a
  normal log entry — delete it before you export and the pixels underneath
  reappear from the pristine source, exactly like any other undo. But at
  the moment you do export, the destroyed pixels must actually be gone from
  the output. The code comment in `src/capture.cpp` is the rule: redactions
  "must never be approximated by a translucent overlay ... because that
  could leave recoverable source data in exports." Never soften a
  redaction's pixel destruction into a paint-time visual effect to make it
  cheaper or prettier — that reintroduces the leak this feature exists to
  prevent.

## What this means for new features

- New annotation kinds: add a field/kind, extend `renderCapture` and
  `replayLog`; don't add editor-only state that isn't in the log.
- New destructive operations (if ever needed): must recompute from
  `pristineSource_` the way cuts do, unless the entire point is irreversible
  pixel destruction like redaction — and if so, say so explicitly in a
  comment the way redaction's is written, because the next person touching
  that code needs the same warning.
- Anything that renders for output must go through `renderCapture`, not a
  parallel drawing path, or the two will drift.
