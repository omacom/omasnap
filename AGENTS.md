# Omasnap — Agent Guide

Omasnap is a super fast, native Wayland screenshot and annotation overlay,
built for [Omarchy](https://omarchy.org) on Hyprland. It captures region,
window, or full monitor (plus a scrolling-region mode that stitches a taller
page into one image), then copies it and opens a floating compositor preview.
The preview fades after 7 seconds of idle time unless kept with its pin button,
Ctrl+P, or by dragging it, including within the stack. Hovering and in-progress
actions pause its countdown.
It opens an annotation editor on demand, with vector layers
(arrows, lines, freehand, highlighter, rectangles, ellipses, numbered
markers, text, OCR). Finished captures go to clipboard,
`~/Pictures/Screenshots`, or a floating capture pinned across workspaces.

## Project principles

Each of these has a longer writeup under `docs/` — read it before making a
change that touches the principle, not just this summary.

- **A specialized tool, not a general app.** Omasnap does one job — capture,
  annotate, output — and does it fast. It is not a drawing program, not a
  file manager, not a general Wayland utility. A feature that isn't in
  service of "screenshot, mark it up, send it somewhere" doesn't belong
  here, however useful it might be on its own.
- **The main thread never blocks.** Capture, paint, and input handling are
  the UI thread's whole job. Disk I/O on a full-resolution image, spawning
  a process, PNG encoding — all of it runs on a worker via
  `QtConcurrent`/`QFutureWatcher`, never inline. See
  [docs/threading.md](docs/threading.md).
- **Every operation is undoable.** The operation log is the source of
  truth; the visible image is rebuilt from it. Rendering for editing is a
  pure, repeatable function of that log — nothing is baked into the working
  image as you draw. Fresh captures copy and show a timed preview by default;
  during editing, output is applied only on **Copy**, **Save**, or both
  (or an explicit pin or a return to its preview),
  which is the one moment a flattened image is produced. Redaction is the
  deliberate, documented exception: it must actually destroy pixels at
  render time so nothing recoverable leaks into an export, while remaining
  a normal, undoable log entry until you export. See
  [docs/editing-model.md](docs/editing-model.md).
- **Minimally configurable — pre-configured to be right, like Omarchy.**
  No settings UI, no wizards, no onboarding. The defaults are the product;
  a config key is a narrow escape hatch for a real divergent need (where to
  save, what to name it, preset colors), never a general mechanism. Adding
  a new key needs the same justification the existing ones had, not "this
  would be nice to expose."
- **Speed first.** Instant capture, annotate, copy. No startup bloat.
- **Wayland only, Hyprland only.** Monitor/window discovery, input
  injection quirks, and notification conventions are all Hyprland-specific
  on purpose. Code that happens to also run on another wlroots compositor,
  because it stands on a real Wayland protocol rather than a Hyprland
  shortcut, is a fine accident — it is not a target, not tested, and not a
  bug magnet we chase. A PR that supports another compositor (niri, KDE,
  etc.) is welcome **only if it adds zero complexity to the Hyprland
  path** — no compositor branching, no backend abstraction, no new
  dependency pulled in just for it. Otherwise it's rejected; fork it
  instead. No X11, no macOS/Windows. See
  [docs/platform-scope.md](docs/platform-scope.md).
- **Lean, learned dependencies.** The dependency set is Qt6 + LayerShellQt +
  wayland-client + libdeflate, plus shelling out to a few existing Omarchy tools
  (`hyprctl`, `wl-copy`/`wl-paste`, `tesseract`, `omarchy-notification-send`)
  instead of linking their equivalents in-process. Know this list before
  proposing an addition to it. See [docs/dependencies.md](docs/dependencies.md).
- **Single small binary.** Everything (capture, editor, pin mode, scroll
  capture) runs from the one `omasnap` executable. Every new dependency or
  vendored asset is weight every install carries.
- **No backwards compatibility.** Break keybindings, CLI flags, file
  formats, or internals whenever it keeps the code simpler or the tool
  faster. Do not add compatibility shims, deprecation aliases, or migration
  code.
- **Omarchy aesthetics.** `omarchy-notification-send` when available,
  `OMASNAP_OCR_LANGS`/`OMARCHY_OCR_LANGS` fallback for OCR languages,
  minimal vector-drawn icons (no icon-theme dependency), the bundled Neucha
  font. Chrome text uses `chromeFont()`/`chromeMonoFont()`
  (`src/overlay-chrome.cpp`), pinned in code, and `main()` installs
  `chromeDefaultFont()` as the application font; external desktop platform
  themes are deliberately bypassed at startup in favour of Qt's built-in
  `generic` theme (see [docs/dependencies.md](docs/dependencies.md)). Chrome
  must use the pinned fonts and explicit colours; do not derive chrome from
  `QFontDatabase::systemFont`, `QStyle`, or `palette()`.

## Repository layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI parsing, single-instance lock, mode dispatch |
| `src/instance-lock.cpp/.hpp` | Single-instance handover: cancel a running overlay, or stop it and take over for `--file` |
| `src/capture.cpp/.hpp` | Capture, render pipeline, output (clipboard/save/notify), source+JSON operation-log persistence, config loading glue |
| `src/editor.cpp/.hpp` | Annotation editor: tools, vector layers, operation-log undo/redo, the select↔edit phase machine, export |
| `src/overlay-chrome.cpp/.hpp` | Shared chrome every overlay wears: the capture-kind tab strip, hotkey legend, status pill |
| `src/scroll-capture.cpp/.hpp` | The scroll-capture panel: region-live page, manual/auto mode, grips, stitched result |
| `src/scroll-inject.cpp/.hpp` | Auto-scroll wheel injection (uinput / `zwlr_virtual_pointer_v1`) |
| `src/auto-capture.cpp/.hpp`, `src/stitch.cpp/.hpp` | Pure, offline-testable frame classification and stitching |
| `src/stitch-replay.cpp` | Standalone tool: replay a dumped frame directory through the stitcher with no compositor |
| `src/surface-capture.cpp` | In-process output/window capture via `ext-image-copy-capture` |
| `src/cut.cpp/.hpp` | Cut-band tool: remove a strip and collapse the gap |
| `src/recent-snaps.cpp/.hpp` | The recents shelf: shelving/reopening working documents |
| `src/output-config.cpp/.hpp`, `src/palette-config.cpp/.hpp` | The optional `omasnap.conf` INI: output destination/filename, color presets |
| `src/pin.cpp/.hpp`, `src/pin-file.cpp/.hpp`, `src/pin-layout.cpp/.hpp` | Floating pinned captures, their files, and compositor placement |
| `src/pin-expiry.cpp/.hpp` | Preview countdown, interaction pauses, and fade |
| `src/icons.cpp/.hpp` | Vector icon renderer for toolbar and pin controls |
| `src/cli-path.cpp/.hpp` | Command-line image target resolution |
| `src/eyedropper.cpp/.hpp` | Display-to-source color sampling |
| `tests/*-smoke.cpp/.hpp` | `omasnap_tests`, headless Qt Test coverage: offscreen region clicks, async capture, single-instance handover, stitching fixtures |
| `docs/` | Longer writeups of the principles above — read before changing behavior they cover |
| `bin/build`, `bin/test`, `bin/install` | Build, test through ctest, and install as a package |
| `pkgbuild/PKGBUILD` | The dependency list, and the package `bin/install` builds from this checkout |
| `CMakeLists.txt` | Build definition; **the version lives here** (`project(omasnap VERSION ...)`) |

## Build and verify

```bash
bin/test
```

`bin/test` builds everything (`bin/build`) and runs it through ctest offscreen: `omasnap_tests`, the headless smoke suite (simulated region clicks, asynchronous capture, single-instance handover, stitching fixtures), and `omasnap_lint`, clang-tidy with the checks in `.clang-tidy`, which fails on any finding and runs whenever `run-clang-tidy` is installed. `bin/test -R omasnap_tests` runs only the smoke suite. `bin/install` builds `pkgbuild/PKGBUILD` from this checkout with `makepkg -fsi`, so the package replaces the installed one.

Smoke artifacts belong in `build/smoke-output/` (or `smoke-output/` beside a
custom build's executable). Direct smoke runs use that default too; use
`--output-dir <directory>` for an explicit alternative. Keep generated test
screenshots and other debugging artifacts out of the repository root.

Always run `bin/test` after behavioral changes. CI (`.github/workflows/test.yml`) runs the same `bin/build` and `bin/test` on every push and PR.

Dependencies (Arch) are the `depends`, `makedepends` and `checkdepends` in `pkgbuild/PKGBUILD`, which CI installs from. See [docs/dependencies.md](docs/dependencies.md) before adding to that list.

## Release process

The release tag is the version; nothing in the tree records it, so a release
needs no commit. CMake reads it from `git describe` in a checkout, or from
the `omasnap-<version>` directory of a GitHub release tarball.

1. Build and run the smoke test (above) on `main`.
2. `gh release create v<version> --target main --generate-notes`, then edit
   the notes into a short list of user-facing changes. The GitHub workflow
   attaches the build artifact to the release.
3. omarchy-pkgs picks up the new GitHub release on its own: the hourly
   Sync Upstream run watches `omacom/omasnap` releases, bumps
   `pkgbuilds/omasnap/` (`pkgver` and checksum), and auto-merges once the
   build passes. No manual omarchy-pkgs PR is needed.

See `README.md` for user-facing features, keybindings, and install
instructions — keep it in sync when behavior changes.
