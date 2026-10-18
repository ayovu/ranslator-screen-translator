#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
appdir="${RANSLATOR_APPDIR:-$PWD/build/AppDir}"
dist="${RANSLATOR_DIST_DIR:-$PWD/dist/native-linux-$(uname -m)}"
[[ ! -L "$dist/Ranslator.flatpak" ]] || { printf 'Choose another output directory: this path is a verified-release alias.\n' >&2; exit 1; }
# AppDir contains the Rust executable, native libraries, Tesseract and OCR data.
# Build it first with scripts/package-appimage.sh. No SDK is needed to wrap it.
[[ -x "$appdir/usr/bin/ranslator" ]] || { printf 'Run scripts/package-appimage.sh first.\n' >&2; exit 1; }
branch="${RANSLATOR_FLATPAK_RUNTIME:-26.08}"
if ! flatpak info "org.freedesktop.Platform//$branch" >/dev/null 2>&1; then
    printf 'Install the runtime first: flatpak install flathub org.freedesktop.Platform//%s\n' "$branch" >&2
    exit 1
fi
mkdir -p build "$dist"
stage="$(mktemp -d "$PWD/build/flatpak-stage.XXXXXX")"
printf 'Flatpak staging directory: %s\n' "$stage"
# build-init requires an installed SDK name even when copying a prebuilt bundle.
# Use the runtime for staging; the exported metadata names the real SDK.
flatpak build-init "$stage" io.github.ranslator.Ranslator org.freedesktop.Platform org.freedesktop.Platform "$branch"
cp -a "$appdir/usr/." "$stage/files/"
install -Dm755 packaging/flatpak/ranslator-launch "$stage/files/bin/ranslator-launch"
install -Dm644 assets/io.github.ranslator.Ranslator.metainfo.xml "$stage/files/share/metainfo/io.github.ranslator.Ranslator.metainfo.xml"
sed -i 's/Exec=ranslator/Exec=ranslator-launch/g' "$stage/files/share/applications/io.github.ranslator.Ranslator.desktop"
flatpak build-finish "$stage" --command=ranslator-launch --sdk="org.freedesktop.Sdk/$(uname -m)/$branch" --share=network --share=ipc --socket=wayland --socket=fallback-x11 --device=dri --talk-name=org.freedesktop.Flatpak --talk-name=org.a11y.Bus --filesystem=xdg-run/at-spi:ro --talk-name=org.kde.kglobalaccel --talk-name=org.kde.KWin --talk-name=org.kde.StatusNotifierWatcher --talk-name=io.github.ranslator.Ranslator.Desktop --own-name=io.github.ranslator.Ranslator.Integration
# The runtime must provide the host command bridge used by local CLI runtimes.
flatpak build --with-appdir --runtime "$stage" sh -c 'command -v flatpak-spawn >/dev/null'
# Verify the packaged runtime, loader, OCR executable and model before export.
flatpak build --with-appdir --runtime \
    --env=RANSLATOR_CONFIG=/tmp/ranslator-package-check/config.json \
    --env=XDG_DATA_HOME=/tmp/ranslator-package-check/data \
    "$stage" /app/bin/ranslator-launch --doctor
flatpak build-export build/flatpak-repo "$stage" stable
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo build/flatpak-repo "$dist/Ranslator.flatpak" io.github.ranslator.Ranslator stable
printf 'Created %s. Install with: flatpak install --user %s\n' "$dist/Ranslator.flatpak" "$dist/Ranslator.flatpak"
