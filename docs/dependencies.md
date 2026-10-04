# Dependencies: learn the current set before adding to it

The dependency list is small on purpose and should stay that way. Before
adding anything — a library, a build tool, an external process — read this
file, then ask whether the thing you need is genuinely absent from it.

## Link-time (build and runtime)

From `CMakeLists.txt`, this is the entire list:

| Dependency | What it's for |
|---|---|
| **Qt6** (Concurrent, Core, Gui, Network, Test, Widgets) 6.8+ | Everything: windowing, painting, the editor UI, the worker-pool threading model ([threading.md](threading.md)), the test harness; Network for uploads |
| **LayerShellQt** | Layer-shell surfaces (the capture overlay and editor) |
| **wayland-client** (pkg-config) | Raw protocol client code (`ext-image-copy-capture`, `zwlr_virtual_pointer_v1`) that LayerShellQt/QtWayland don't expose |
| **libdeflate** (pkg-config) | Fast lossless PNG compression and CRCs for 8-bit screenshots; avoids seconds of Qt/zlib encoding before a 6K preview can appear |
| **wayland-scanner** + protocol XML | Generates the C bindings for the above at build time; not a runtime dependency |

That's it. No JSON library (Qt's `QJsonDocument` handles `hyprctl -j`
output), no HTTP library beyond Qt Network,
no logging framework, no CLI-parsing library beyond `QCommandLineParser`,
no general config-file parser beyond `QSettings` (used for the one optional INI
file — see below). The theme adapter reads a bounded scalar subset of Omarchy's
TOML color files with Qt; it adds no parser library.

`src/png.cpp` writes ordinary 8-bit RGB/RGBA PNGs using libdeflate at level 1.
On a captured 6K desktop, Qt's default PNG encoding took about 3 seconds;
even its fastest compressed setting took about 700 ms. Libdeflate brought
compression to about 150 ms with identical decoded pixels and similar file
size. It earns its dependency by removing that capture-to-preview delay.
The fast encoder also writes Omasnap's small logical-size PNG text field, so
standalone captures reopen at their original display size without resampling.
Qt still reads every image and writes images with profiles/other text/offset metadata,
high-bit-depth formats, and captures exceeding the encoder's 128 MiB filtered
buffer budget. Those use Qt's streaming writer at zlib level 1. PNG DPI metadata
is preserved on both paths. No quality or compression setting is exposed.

Qt Network ships in `qt6-base`, which Omasnap already requires, so uploads add
no package. It is the only practical way to speak the hosts' HTTP APIs (S3's
signed requests, Dropbox's OAuth loopback, multipart forms) without a heavier
library, and it is used only by `src/upload*.cpp`, always from a worker thread
with its own event loop (see [threading.md](threading.md)). Nothing is sent
until `[upload] hosts` names a destination.

## Runtime: external processes, not libraries

Omasnap shells out to a small number of existing Omarchy/Arch tools instead
of linking their libraries in-process. This is intentional: a `QProcess`
call to a well-maintained CLI tool that's already on every Omarchy install
is a dependency Omasnap doesn't have to build, version, or debug — the
alternative (vendoring an OCR engine, a clipboard protocol implementation,
or a compositor IPC client) would be strictly more code and more risk for
no user-visible benefit.

| Process | Used for | Required? |
|---|---|---|
| `hyprctl` | Monitor/window discovery (`-j` JSON), floating pin placement, natural-scroll policy query | Yes — see [platform-scope.md](platform-scope.md) |
| `wl-copy` / `wl-paste` | Writing PNG/text to the Wayland clipboard, and verifying the write | Yes |
| `xdg-open` | Opening a screenshot's containing folder in the default file browser | For Show in folder |
| `xdg-mime` / `busctl` | Resolve the default folder application and ask it to select the screenshot with `FileManager1.ShowItems` | Optional — falls back to `xdg-open` |
| `tesseract` | OCR text recognition | Only if OCR is used; missing tesseract fails just that action |
| `omarchy-notification-send` | Capture-finished notifications | No — falls back silently if absent (checked with `command -v` semantics via failed `QProcess::startDetached`) |
| `curl` | Uploads to SFTP, FTP and FTPS hosts (Qt has no FTP or SFTP); the password goes over stdin | Only for `type = ftp` hosts |
| `secret-tool` | Reading and storing upload secrets in the desktop keyring | No — falls back to a private file |
| `/bin/sh` | Running a host's `KEY_command` (`secret-tool lookup`, `pass show`, …) | Only when configured |

These run through the small `QProcess` helpers in `src/capture.cpp` and
`src/pin.cpp`, from a background worker (see [threading.md](threading.md)).
Calls that wait for a result have a timeout; long-lived applications launch
detached. None run inline on the UI thread.

## Not a dependency: external Qt platform themes

Omarchy exports `QT_QPA_PLATFORMTHEME=gtk3` for the whole session so Qt apps
match GTK apps. Omasnap overrides it to `generic` (Qt's built-in theme) for
its own process in `main()` before `QApplication` is constructed: honouring
the session value loads the `qgtk3` plugin,
which initialises GTK3, GLib/GIO and dconf inside the process — measured at
81–112 ms of `QApplication` construction and ~20–24 MiB of RSS on an Omarchy
laptop — for hand-painted chrome and Qt's built-in file chooser. The only
relevant values the external theme supplied were its general and fixed fonts;
their chrome and application-default replacements
are pinned by `chromeFont()`, `chromeMonoFont()`, and `chromeDefaultFont()`
(`src/overlay-chrome.cpp`) instead. Do not make startup or chrome rendering
depend on an external desktop theme, `QStyle`- or palette-derived chrome, or
icon-theme lookup: each is a startup cost with nothing in this codebase to
spend it on.

Chrome colors do follow Omarchy. `src/chrome-theme.cpp` reads `colors.toml` and
the optional `shell.toml` from `~/.local/state/omarchy/current/theme` on a worker.
It resolves palette references, control fills, tooltip colors, and solid or
gradient borders into explicit paint values. Missing or malformed data has
readable defaults. A filesystem watcher reloads the palette after file or
directory replacement and repaints open windows. This does not use Qt's desktop
palette, alter annotation/export colors, or change the pinned fonts.

## Save As dialog

Save As uses the existing Qt Widgets file chooser asynchronously, with pinned
chrome fonts and the active Omarchy theme, including live theme changes.
The overlay alone receives its layer-shell
role through `LayerShellQt::Window::get`; the inherited global shell override
is cleared so the chooser and overwrite prompts use ordinary xdg-shell windows.
No native platform-theme plugin or new dependency is needed.

## The one config file

`~/.config/omasnap/omasnap.conf` is optional INI, read with `QSettings`.
Its existing overrides cover screenshot destination/filename patterns, preset
colors, editor presentation, and custom backdrop defaults. It is not a
general settings mechanism. See the "minimally configurable" principle in
[AGENTS.md](../AGENTS.md) before adding a new key: the bar is "this is a
real escape hatch for a real divergent need," the same bar the existing
keys cleared, not "this would be nice to expose."

## Evaluating a new dependency

Ask, in order:

1. **Can Qt already do this?** Qt6's modules are broad (networking,
   concurrency, text layout, SVG, image I/O). Check before reaching
   further.
2. **Is this an OS-integration concern better solved by shelling out to an
   existing Omarchy tool**, the way clipboard, OCR, and notifications are?
   A new external-process dependency is far cheaper than a new linked
   library: it doesn't grow the binary, doesn't add a build-time
   dependency, and fails gracefully (a missing/failed process is just an
   error message).
3. **Does it exist only to support a non-Hyprland compositor?** Then it's
   out of scope — see [platform-scope.md](platform-scope.md).
4. **Is it justified anyway?** Then it needs to earn its place in the table
   above, and this file needs to be updated in the same PR. A dependency
   that isn't documented here is a dependency someone will add a second,
   redundant way to do the same thing next to, because they didn't know it
   existed.

## Binary size

The pin icon in `src/icons.cpp` uses [Lucide's pin geometry](https://github.com/lucide-icons/lucide/blob/main/icons/pin.svg),
adapted to the existing `QPainterPath` renderer. Its ISC notice is in
`assets/Lucide-ISC.txt` and installed with the font licenses. No icon library,
SVG renderer, or theme lookup is needed.

Single, statically-linked-where-practical binary, installed to
`~/.local/bin/omasnap` (see the repository's `install-omarchy`). Every
dependency added here is weight every user carries on every install and
every update. If a feature can be built with what's already linked, that's
the implementation to ship.
