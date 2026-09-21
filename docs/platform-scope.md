# Platform scope: Wayland + Hyprland, on purpose

Omasnap targets one platform: **Wayland, on Hyprland, on Omarchy.** This is
not a starting point we intend to broaden into a general Linux screenshot
tool. It is a deliberate choice that keeps the code small and lets it use
real platform facilities instead of lowest-common-denominator
abstractions.

## What "Hyprland-only" means concretely

- Monitor and window discovery goes through `hyprctl` (`hyprctl monitors
  -j`, `hyprctl clients -j`) — see `src/capture.cpp`. There is no
  generic-compositor fallback for this, because there is no generic,
  reliable way to ask an arbitrary Wayland compositor "what windows exist
  and where."
- Scroll capture's auto-scroll reads `hyprctl getoption input:natural_scroll`
  to pre-compensate injected wheel events (`src/scroll-inject.cpp`) — a
  Hyprland-specific input policy, queried the Hyprland-specific way.
- The keyboard-grab dance in the scroll and area overlays
  (`setKeyboardGrab` in `src/scroll-capture.cpp`) works around a specific,
  observed Hyprland behavior: an exclusive keyboard grab on a layer surface
  pins pointer focus to that layer even over an input-region hole. The
  overlay therefore releases exclusive focus over the live page, retaining
  on-demand keyboard interactivity so Escape can still reach it when focused.
  Pointer events over the chrome restore exclusive focus. This documents a
  Hyprland quirk, not a generic Wayland rule.
- Notifications prefer `omarchy-notification-send` and fall back to
  `OMARCHY_OCR_LANGS`/`OMASNAP_OCR_LANGS` conventions that assume an Omarchy
  install (see `src/capture.cpp`, README's OCR section).

## What's actually generic, and why it can work elsewhere by accident

Underneath the Hyprland-specific discovery, the capture and injection
mechanisms are standard Wayland protocols: `ext-image-copy-capture` for
reading pixels (`src/surface-capture.cpp`), `zwlr_virtual_pointer_v1` for
injected scroll input (`src/scroll-inject.cpp`, protocol vendored in
`protocols/`), and `layer-shell` for every overlay surface. Any wlroots
compositor that implements the same protocols (Sway, river, and others)
will likely run the fullscreen/region/window capture and the editor without
modification, because that code never asks "is this Hyprland" — it asks the
compositor for a capability and uses it if offered.

**This is fine, and it is not a target.** If it works on another
wlroots compositor, that's a side effect of standing on real protocols
instead of a bespoke integration, not a supported configuration. We do not
add fallbacks, feature flags, or compatibility code to make it work
somewhere else. We do not test on anything but Hyprland. A bug that only
reproduces on another compositor is not a bug we chase.

## Contribution policy for other window managers

A pull request that adds support for, or improves behavior on, a different
Wayland compositor (niri, KDE/KWin, GNOME/Mutter, plain Sway, etc.) is
**welcome if and only if it adds zero complexity to the Hyprland path.**
Concretely:

- **Accept:** using an existing generic protocol path that already runs on
  Hyprland, where the PR is really "this also happens to work on niri,
  here's the one guard that was Hyprland-specific and didn't need to be."
- **Accept:** replacing a Hyprland-only call with a protocol-based
  equivalent that is a strict improvement on Hyprland too (fewer external
  processes, more accurate data, etc.) and happens to be portable as a
  bonus.
- **Reject:** anything that branches on compositor identity
  (`if (hyprland) ... else if (kwin) ...`), adds a second discovery
  backend, introduces a compositor-detection abstraction layer, or adds a
  runtime/compile-time flag to select behavior. If it needs an `#ifdef`, a
  new interface, or a "backend" concept, it doesn't belong here — fork it,
  or maintain it downstream.
- **Reject:** dependencies pulled in only to support another compositor
  (e.g., a KWin scripting library, GNOME Shell D-Bus bindings). See
  [dependencies.md](dependencies.md).

If in doubt, the test is: **would a Hyprland user notice any difference —
in binary size, startup time, code paths exercised, or dependencies — if
this PR were reverted?** If the answer is no, it's probably fine. If the
answer is yes, it's probably out of scope.

## X11, macOS, Windows

Not supported, not planned, not accepted. These are different enough
(different capture APIs, different input injection, different window
management entirely) that "zero added complexity" is not achievable, and
there is no version of this tool that is simultaneously fast, small, and
portable to them.
