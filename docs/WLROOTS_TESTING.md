# Isolated wlroots integration check

This check runs the portable archive on a real Sway compositor with two headless
outputs. It uses a private D-Bus session, synthetic GTK text, a loopback translation
server, synthetic PRIMARY changes and a persistent virtual pointer. It never connects
to the host display, clipboard,
session bus or graphics devices. Only the repository is mounted into the container.

From the repository root, after building the portable archive:

```sh
podman build -f tests/fixtures/wlroots-lab.Containerfile -t ranslator-wlroots-lab .
podman run --rm --init --userns=keep-id:uid=0,gid=0 \
  -v "$PWD:/work" -e RANSLATOR_WLROOTS_LAB=1 \
  ranslator-wlroots-lab python3 scripts/check-wlroots-integration.py
```

Set `RANSLATOR_TEST_BUNDLE=/work/path/to/archive.tar.gz` to test a different archive.
Logs, screenshots and the artifact SHA-256 report are written to `build/wlroots/`.
The container deliberately omits the system `libopengl0` package: the archive must
supply its vendor-neutral loader while retaining the host's EGL/GLX implementation.
Sway's `--unsupported-gpu` flag bypasses host-kernel vendor detection; this session
uses the headless backend and pixman, with no GPU access.

The check exercises a real pointer click on Listen in the full translator,
initial-selection suppression, repeated PRIMARY changes,
ordinary clipboard exclusion, disarm/rearm, cancellation when a selection clears,
the real screenshot portal, bundled OCR, pointer-selected text translation and the
app's AT-SPI accessibility bridge.
The provider receives only fixed synthetic text. Screenshot permission dialogs, if
shown, are approved only on the disposable session bus.

Ubuntu 24.04 supplies Sway 1.9 and xdg-desktop-portal-wlr 0.7.1 here. This proves the
`zwlr_data_control_manager_v1` path, not the newer `ext_data_control_manager_v1`
path. This portal does not expose GlobalShortcuts; compositor bindings can invoke
`ranslator --select`, `--listen` and `--snapshot`. Modifier-only gesture parity is
not established by this test.

The noninteractive screenshot is the full two-output desktop, fitted into a
single-output overlay. The test records that limitation; it does not establish
active-monitor capture parity. The portal supplies no output identity or pixel-to-
logical-coordinate mapping, and generic Wayland clients cannot obtain a reliable
global pointer position outside their surfaces. A compositor-specific invocation
context and verified image mapping would be needed before cropping safely,
especially with mixed scales, rotated outputs or negative output coordinates.
Physical hotplug, fractional scaling, hardware rendering and other wlroots
compositors remain separate validation work.
