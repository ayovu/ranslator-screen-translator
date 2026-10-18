# GNOME desktop integration

This optional Shell extension gives Ranslator direct current-monitor snapshots,
accurate pointer/monitor geometry, ordinary global shortcuts, and PRIMARY selection
notifications on GNOME Wayland. The application still works through desktop portals
and AT-SPI when the extension is absent.

Run explicitly from a source checkout:

```sh
./scripts/install-gnome-integration.sh
```

GNOME may require a logout/login before a newly installed extension becomes visible.
If the installer says so, log back in and run
`gnome-extensions enable ranslator@ranslator.local`. Ranslator detects enabling,
disabling, and Shell restarts without restarting the app. Disable with
`gnome-extensions disable ranslator@ranslator.local`, or remove with
`gnome-extensions uninstall ranslator@ranslator.local`.

## Capabilities and limits

- GNOME 46 has been exercised in a real nested Wayland compositor with a native GTK4
  client and two synthetic monitors. Metadata currently enables version 46 only;
  later Shell versions need the same runtime validation before being added.
- Use chords containing a normal key, such as `Ctrl+Alt+R`. Arbitrary modifier-only
  gestures such as double Shift are unavailable: Mutter routes native Wayland client
  events before Shell extension event listeners. The extension reports these settings
  as unsupported instead of registering unreliable shortcuts. Ranslator's native X11
  and KWin adapters have different capabilities.
- Active-monitor and full-desktop captures are one frame, transferred in memory.
  Region selection happens in Ranslator over the frozen image. This does not start a
  background screen-recording session or write screen contents to a shared file.
- Only the Ranslator client owning its integration D-Bus name can invoke the methods.
  Selection is read only while explicitly armed; disarming, client exit, and extension
  disable stop it. Lock-screen/greeter capture and selection access are rejected.
- The extension does not claim Orca's identity, modify keyboard settings, or require
  GNOME Shell unsafe mode. Extensions execute inside Shell and must be installed only
  from trusted sources.

## Isolated validation

`scripts/check-gnome-integration.py` runs the real Shell, a GTK client and synthetic
keyboard/clipboard events inside a disposable container. It requires
`RANSLATOR_GNOME_LAB=1`, a container-local system D-Bus, GNOME Shell 46, Xvfb, xdotool,
x11-utils, dbus-x11, Python GObject/GTK4, and software Mesa. No host display, Wayland,
session-bus or runtime-directory sockets should be mounted into the container.
The test creates a private HOME/session bus and two nested 800×600 outputs. It verifies
shortcut delivery while the GTK client is focused, ordinary typing, selection,
unauthorized-client rejection, second-monitor targeting, and PNG output dimensions.

For example, from the repository root with rootless Podman:

```sh
podman build -t ranslator-gnome-lab -f tests/fixtures/gnome-lab.Containerfile tests/fixtures
podman run -d --name ranslator-gnome-check --userns=keep-id:uid=0,gid=0 \
  -v "$PWD:/work" ranslator-gnome-lab
podman exec -e RANSLATOR_GNOME_LAB=1 ranslator-gnome-check \
  python3 scripts/check-gnome-integration.py
podman rm -f ranslator-gnome-check
```

The native client protocol tests run with a separate fake service:

```sh
cmake -S native -B build/gnome-tests -DRANSLATOR_GNOME_TESTS=ON
cmake --build build/gnome-tests --target gnome_tests
ctest --test-dir build/gnome-tests --output-on-failure
```
