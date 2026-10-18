#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
repo="$PWD"
arch="$(uname -m)"
dist="${RANSLATOR_DIST_DIR:-$repo/dist/native-linux-$arch}"
[[ ! -L "$dist/Ranslator-$arch.AppImage" ]] || { printf 'Choose another output directory: this path is a verified-release alias.\n' >&2; exit 1; }
appdir="${RANSLATOR_APPDIR:-$repo/build/AppDir}"
target="${CARGO_TARGET_DIR:-$repo/target}"
mkdir -p build/tools "$dist"
cargo build --release --locked
file="$repo/build/tools/linuxdeploy-$arch.AppImage"
python3 - "$file" <<'FETCH_TOOL'
import hashlib, json, pathlib, sys, urllib.request
path = pathlib.Path(sys.argv[1])
item = json.loads(pathlib.Path('packaging/portable/tools.json').read_text())[path.name]
if not path.exists():
    with urllib.request.urlopen(item['url'], timeout=180) as response:
        path.write_bytes(response.read())
if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
    raise SystemExit('linuxdeploy checksum mismatch. Review and update packaging/portable/tools.json before using a newer tool.')
path.chmod(0o755)
FETCH_TOOL
# linuxdeploy keeps files already present; a fresh staging directory prevents stale releases.
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/ranslator/tessdata"
cp assets/tessdata/{eng.traineddata,LICENSE} "$appdir/usr/share/ranslator/tessdata/"
mkdir -p "$appdir/usr/share/ranslator/gnome"
cp -R integrations/gnome/ranslator@ranslator.local "$appdir/usr/share/ranslator/gnome/"
install -m755 scripts/install-gnome-integration.sh "$appdir/usr/bin/install-ranslator-gnome-integration"
cp packaging/appimage/AppRun "$appdir/AppRun"
chmod +x "$appdir/AppRun"
export APPIMAGE_EXTRACT_AND_RUN=1
qmake="${QMAKE:-$(command -v qmake6)}"
plugins="$("$qmake" -query QT_INSTALL_PLUGINS)"
# Bundle only the formats and platforms we use; KDE's optional image plugins can
# depend on codecs not installed on the build machine.
plugin_args=()
for category in platforms wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration xcbglintegrations imageformats iconengines platforminputcontexts; do
    mkdir -p "$appdir/usr/plugins/$category"
    plugin_args+=(--deploy-deps-only "$appdir/usr/plugins/$category")
done
shopt -s nullglob
for plugin in "$plugins"/platforms/libqwayland*.so "$plugins"/platforms/libqxcb.so "$plugins"/platforms/libqoffscreen.so "$plugins"/wayland-decoration-client/*.so "$plugins"/wayland-graphics-integration-client/*.so "$plugins"/wayland-shell-integration/*.so "$plugins"/xcbglintegrations/*.so "$plugins"/imageformats/libq{jpeg,gif,webp,svg}.so "$plugins"/iconengines/libqsvgicon.so "$plugins"/platforminputcontexts/libcomposeplatforminputcontextplugin.so; do
    [[ -f "$plugin" ]] || continue
    category="$(basename "$(dirname "$plugin")")"
    cp "$plugin" "$appdir/usr/plugins/$category/"
done
# Qt directly needs the split GLVND OpenGL wrapper; a Wayland/Mesa desktop
# does not necessarily install it. Keep EGL/GLX/dispatch and GPU drivers on the host.
"$file" --appdir "$appdir" --executable "$target/release/ranslator" --executable "$(command -v tesseract)" --library "$(pkg-config --variable=libdir harfbuzz)/libharfbuzz.so.0" --library "$(ldconfig -p | awk '$1 == "libgpg-error.so.0" { path=$NF } END { print path }')" --library "$(ldconfig -p | awk '$1 == "libOpenGL.so.0" { path=$NF } END { print path }')" --desktop-file "$repo/assets/io.github.ranslator.Ranslator.desktop" --icon-file "$repo/assets/io.github.ranslator.Ranslator.svg" "${plugin_args[@]}"
if [[ -d "$plugins/kf6/kwindowsystem" ]]; then
    mkdir -p "$appdir/usr/plugins/kf6/kwindowsystem"
    cp "$plugins"/kf6/kwindowsystem/*.so "$appdir/usr/plugins/kf6/kwindowsystem/"
    "$file" --appdir "$appdir" --deploy-deps-only "$appdir/usr/plugins/kf6/kwindowsystem"
fi
RANSLATOR_APPDIR="$appdir" python3 scripts/collect-licenses.py
python3 scripts/verify-portability.py --appdir "$appdir" ${RANSLATOR_GLIBC_MAX:+--max-glibc "$RANSLATOR_GLIBC_MAX"}
install -Dm644 assets/io.github.ranslator.Ranslator.metainfo.xml "$appdir/usr/share/metainfo/io.github.ranslator.Ranslator.metainfo.xml"
# Keep our launcher, including the Tesseract data path.
cp packaging/appimage/AppRun "$appdir/AppRun"
export OUTPUT="$dist/Ranslator-$arch.AppImage"
"$file" --appdir "$appdir" --output appimage

# A regular archive is useful on systems whose security policy disables FUSE.
tar -C "$appdir" -czf "$dist/Ranslator-linux-$arch.tar.gz" .
