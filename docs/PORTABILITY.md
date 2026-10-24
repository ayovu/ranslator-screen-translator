# Platform validation

Support is a combination of CPU architecture, userspace libraries, display
protocol, compositor, application accessibility, and permissions. One passing
Linux build cannot certify all of those combinations.

## Checks available in this repository

| Check | What it establishes |
| --- | --- |
| `scripts/check-build.sh` | Rust formatting/lints/tests and native interaction suites. Linux additionally exercises private D-Bus portals, accessibility fixtures and X11/Xvfb. |
| Native `autostart_registration` / UI tests | XDG state and launcher paths, write failures, tray/settings synchronization, Background portal approval/denial/timeout and late responses in isolated profiles. They do not log out of a real desktop. |
| `scripts/check-multi-snapshot.py` | Three real mouse selections, crop OCR, out-of-order translations, card dragging and Escape against a mock provider on a private display. |
| `scripts/check-host-runtimes.py` | Synthetic host commands inside a real Flatpak, including cancellation, timeout, errors and permission denial. No AI accounts are used. |
| `scripts/check-bundled-providers.py` | Actual AppDir/AppImage/Flatpak executables against synthetic host-loopback Ollama and compatible API servers: discovery, translation, credentials, errors and denied network access. No remote service or model quality is tested. |
| `scripts/check-flatpak-install.py` | Installs the exported bundle in a disposable installation and uses real `flatpak run` to verify effective permissions, dependency detection, rendering and OCR. A private session bus excludes desktop/document portal validation. |
| `scripts/check-flatpak-accessibility.py` | Synthetic AT-SPI service through the actual Flatpak boundary, plus denied socket access. The native suite separately tests real Qt/GTK editor fixtures. |
| `scripts/check-bundled-accessibility.py` | Screen-reader UI labels and editable language fields in the actual AppImage and an isolated installed Flatpak, on private display and accessibility buses. This checks outgoing Qt accessibility separately from selected-text capture. |
| `scripts/check-portable-distros.py` | AppImage dependency closure, Qt rendering, X11/Xvfb, multilingual glyphs and bundled OCR in isolated distribution userspaces. |
| `scripts/check-wlroots-integration.py` | Actual portable archive on private headless Sway: PRIMARY lifecycle, clipboard filtering, Qt accessibility, screenshot portal and OCR. See [scope and container commands](WLROOTS_TESTING.md); current-monitor capture parity is not established. |
| `scripts/check-monitors.py` | Explicit live KDE test with synthetic content across physical monitors. Temporarily covers the user's screens; run only in an idle test session. |
| `scripts/package-macos.sh` | Native Mac compilation, library closure, architecture/minimum-OS audit, signature, render and OCR checks. Requires a real Mac. |

Machine-readable results and generated logs belong under `build/` or `dist/` and
are excluded from source control. Attach the relevant reports to a release.

## Boundaries that need real desktops

KDE Wayland has been exercised on the original Arch machine, including its two
monitors. X11 tests use an isolated display. Distro containers validate libraries
and fonts; they do not emulate KDE, GNOME, graphics drivers, permissions, display
hotplug or a physical multi-monitor session.

GNOME's optional Shell companion is tested in a nested GNOME session, with its
version and results documented alongside the integration. It needs explicit
installation/enabling and may require logging in again. Modifier-only taps in a
foreign native Wayland application are unavailable through the supported Shell
API; the app must report that limitation rather than silently dropping gestures.

AT-SPI and macOS AX can read selected text only when the source application
publishes it. Password/protected fields are excluded. Snapshot OCR provides the
alternative for images and inaccessible content. Portal permission prompts and
OS privacy decisions cannot be bypassed for an out-of-box promise.

## Mac handoff

Linux cannot execute AppKit, ScreenCaptureKit or macOS privacy services. A Rust
target check, if available, validates only Rust code. It does not validate the
Objective-C++ adapters, Qt Cocoa behavior or the `.app` bundle.

On the friend's Mac, build from the source archive using the Mac instructions.
Then verify ordinary and repeated shortcuts; permission denial/grant/revocation;
selection in Safari and a native editor; screenshot orientation and OCR with both
the Apple Vision and Tesseract engines, including the first Snapshot after installing; current
and combined monitors at different Retina scales; region cancellation; three
overlapping translation cards; outside-click dismissal without a Space switch;
light/dark blur; and CLI/Ollama switching with cancellation. Use synthetic text.
Record OS/CPU/Qt versions and attach the first build error or a redacted log.

Also test **Start at login** with the packaged `.app`: registration, system
approval, cancellation of a pending approval, external revocation and a real
logout/login. Check both compact and full-window preferences; a macOS login launch
with compact popup mode disabled may display the full window. Confirm the menu-bar
icon remains visible in both appearances.

Until those checks run, macOS remains an experimental source port. Linux aarch64
also requires an architecture-native build and test before an aarch64 artifact
can be advertised.

The portable AppImage assumes a conventional glibc/FHS userspace. NixOS may require `appimage-run` or `nix-ld`; musl-only systems such as Alpine need a compatible runtime. Use Flatpak where available. Those systems are not covered by the six-distribution AppImage matrix.
