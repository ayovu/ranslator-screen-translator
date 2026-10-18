#!/usr/bin/env bash
# Keep this optional Qt module in a separate image layer from the Qt/KDE toolchain.
set -euo pipefail
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
python3 - "$work/source.tar.gz" <<'PY'
import hashlib, json, pathlib, sys, urllib.request
item = json.loads(pathlib.Path('/tmp/portable/imageformats.json').read_text())
with urllib.request.urlopen(item['url'], timeout=180) as response:
    data = response.read()
assert hashlib.sha256(data).hexdigest() == item['sha256']
pathlib.Path(sys.argv[1]).write_bytes(data)
PY
mkdir -p "$work/src"
tar xzf "$work/source.tar.gz" --strip-components=1 -C "$work/src"
cmake -S "$work/src" -B "$work/build" -GNinja -DCMAKE_INSTALL_PREFIX=/opt/ranslator \
    -DCMAKE_BUILD_TYPE=Release -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DQT_FEATURE_webp=ON
cmake --build "$work/build" --parallel "${RANSLATOR_BUILD_JOBS:-8}"
cmake --install "$work/build"
notices=/opt/ranslator/share/licenses/ranslator-dependencies/qtimageformats
mkdir -p "$notices"
cp -a "$work/src/LICENSES" "$notices/"
cp /tmp/portable/imageformats.json "$notices/"
