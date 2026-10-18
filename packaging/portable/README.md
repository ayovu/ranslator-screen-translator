# Portable Linux packages

Run `scripts/package-portable.sh` on an x86_64 or aarch64 Linux machine with
rootless Podman (or `RANSLATOR_CONTAINER_ENGINE=docker`). The architecture of the
builder determines the output architecture. No host Qt, KDE, Tesseract or C++
libraries are copied into this package.

The container uses Ubuntu 22.04 / glibc 2.35 and builds Qt 6.12 and KDE Frameworks
6.31 from the commit IDs and SHA-256 archives in `sources.json` and `imageformats.json`. Rust is pinned
in `Containerfile`; linuxdeploy is verified against `tools.json`. Distribution
security updates are resolved when rebuilding the image, so builds are isolated
and repeatable but are not claimed to be byte-for-byte reproducible.

Outputs and Cargo intermediates are separated from normal developer builds:

- `dist/portable/Ranslator-<architecture>.AppImage`
- `dist/portable/Ranslator-linux-<architecture>.tar.gz`
- `build/portable/AppDir/`
- `build/portable/target/`

The builder requires Qt accessibility with its AT-SPI bridge, both KDE libraries,
and the generic X11 integration. Missing dependencies fail the release build
instead of silently dropping those features.
`scripts/verify-portability.py` examines every bundled ELF file and rejects a
glibc requirement above 2.35 or accidental build-machine loader paths. The ABI
report also records the required GLIBCXX version. The host C++ runtime is used
to avoid breaking newer host graphics drivers with an older bundled copy.
The separate `libOpenGL.so.0` API wrapper is included because Ubuntu Wayland
desktops can omit that package. EGL/GLX/GLdispatch and Mesa/NVIDIA drivers stay
with the host graphics stack.

Build a Flatpak from the same tested binaries:

```sh
RANSLATOR_APPDIR="$PWD/build/portable/AppDir" \
RANSLATOR_DIST_DIR="$PWD/dist/portable" scripts/package-flatpak.sh
python3 scripts/verify-bundles.py --dist dist/portable \
  --release build/portable/target/release/ranslator
python3 scripts/check-portable-distros.py
python3 scripts/check-flatpak-install.py dist/portable/Ranslator.flatpak
```

The distribution checks launch Qt and real bundled OCR in separate Ubuntu,
Debian, Fedora and Arch userspaces. Their JSON report explicitly lists what was
tested; an offscreen container test does **not** prove real Wayland compositor,
screen-recording permission, global-shortcut or multi-monitor behavior.

The installed-Flatpak check uses the exported bundle, a disposable per-user
installation, and the system runtime. Bubblewrap and a private D-Bus session
keep the real home directory and desktop services untouched. It checks effective
permissions against package metadata, with only its temporary output/profile
folder added, then runs the dependency doctor, Qt rendering and bundled OCR.

To update dependencies, review upstream releases, replace the pinned versions,
commit IDs and checksums, rebuild the container, then rerun ABI, package and
desktop tests. Keep the previous verified package until those checks pass.

Qt and other shared libraries remain replaceable: extract the AppImage with
`--appimage-extract`, replace compatible libraries under `usr/lib`, then run
`squashfs-root/AppRun`. Bundled notices and source provenance are under
`usr/share/licenses/ranslator/`.

## Interface translation catalogs

The current English-only build does not need Qt Linguist. The builder does not
currently include `lrelease`. Before releasing a contributed non-English `.ts`
catalog, add a Qt 6 Linguist compiler to the builder and enable the catalog tests.
Ubuntu 22.04 provides the build tool in
[`qt6-l10n-tools`](https://packages.ubuntu.com/jammy/qt6-l10n-tools); its compiler
is normally `/usr/lib/qt6/bin/lrelease`. Verify `lrelease -version` reports Qt 6
and set `LRELEASE` to its absolute path when it is outside `PATH`.

CMake embeds generated `.qm` files, so all package formats receive the same
catalogs without a runtime Linguist dependency. Source archives and build-input
hashes include `translations/*.ts`; generated `.qm` files are excluded. Follow
the [contributor workflow](../../docs/TRANSLATING.md) and test the embedded-catalog
branch before publishing a translated release. This prerequisite has not yet
been added to the English-only portable builder.

## Installation without FUSE

An AppImage needs the host FUSE device and `fusermount` for its normal mounted
launch. On machines where these are unavailable, install it once with:

```sh
scripts/install-bundle.sh /path/to/Ranslator-x86_64.AppImage
```

This extracts the bundle under `~/.local/opt/ranslator`, creates the application
menu entry, and uses bundled Qt/KDE/OCR libraries. It does not start or stop a
running app. `PREFIX` selects another installation directory. A previous
extracted install is retained at `~/.local/opt/.ranslator-previous` until the next
upgrade. For a temporary launch, use `--appimage-extract-and-run`; the `.tar.gz`
contains the same `AppRun` and `usr/` tree and can also run after extraction.

These packages target glibc-based FHS distributions. NixOS requires its
`appimage-run`/`nix-ld` compatibility environment or a configured Flatpak setup;
a musl-only userspace needs Flatpak or another glibc environment.
