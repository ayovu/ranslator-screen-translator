#!/usr/bin/env bash
# Run natively on the target Mac architecture; produces a relocatable .app and ZIP.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { printf 'Build this package on macOS with Xcode and Homebrew installed.\n' >&2; exit 1; }
for program in cargo cmake python3 brew codesign xcrun; do
    command -v "$program" >/dev/null || { printf 'Missing %s. Install Xcode tools, then: brew install rust cmake qt tesseract pkg-config python\n' "$program" >&2; exit 1; }
done
arch="$(uname -m)"
[[ "$arch" == arm64 || "$arch" == x86_64 ]] || { printf 'Unsupported architecture: %s\n' "$arch" >&2; exit 1; }
qt="${RANSLATOR_QT_PREFIX:-$(brew --prefix qt)}"
tesseract="$(brew --prefix tesseract)/bin/tesseract"
export CMAKE_PREFIX_PATH="$qt${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.4}"
export CARGO_TARGET_DIR="$PWD/build/macos-$arch/target"
dist="${RANSLATOR_DIST_DIR:-$PWD/dist/macos-$arch}"
app="$dist/Ranslator.app"
[[ -x "$qt/bin/macdeployqt" && -x "$tesseract" ]] || { printf 'Install native Qt and Tesseract: brew install qt tesseract\n' >&2; exit 1; }
cargo test --locked --all-targets
cargo build --release --locked
# Only this architecture's staging bundle is replaced; other platform packages are preserved.
rm -rf -- "$app"
mkdir -p "$app/Contents/"{MacOS,Helpers,Resources/tessdata,Frameworks}
cp "$CARGO_TARGET_DIR/release/ranslator" "$app/Contents/MacOS/ranslator"
cp "$tesseract" "$app/Contents/Helpers/tesseract"
cp assets/tessdata/{eng.traineddata,LICENSE} "$app/Contents/Resources/tessdata/"
cp packaging/macos/Info.plist "$app/Contents/Info.plist"
python3 - "$app/Contents/Info.plist" <<'PY'
import os, plistlib, pathlib, sys, tomllib
path = pathlib.Path(sys.argv[1]); info = plistlib.loads(path.read_bytes())
info['CFBundleShortVersionString'] = tomllib.loads(pathlib.Path('Cargo.toml').read_text())['package']['version']
info['LSMinimumSystemVersion'] = os.environ['MACOSX_DEPLOYMENT_TARGET']
path.write_bytes(plistlib.dumps(info))
PY
# rsvg-convert is optional. Without it, macOS's own sips rasterizes the SVG; it renders at the
# document's declared size, so each size gets a copy that declares it.
render_icon() {
    if command -v rsvg-convert >/dev/null; then
        rsvg-convert -w "$1" -h "$1" assets/io.github.ranslator.Ranslator.svg -o "$2"
    else
        local source="$PWD/build/macos-$arch/icon-$1.svg"
        sed "s/width=\"512\" height=\"512\"/width=\"$1\" height=\"$1\"/" \
            assets/io.github.ranslator.Ranslator.svg > "$source"
        sips -s format png "$source" --out "$2" >/dev/null
        rm -f -- "$source"
    fi
}
iconset="$PWD/build/macos-$arch/Ranslator.iconset"
rm -rf -- "$iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
    render_icon "$size" "$iconset/icon_${size}x${size}.png"
    render_icon "$((size * 2))" "$iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/Ranslator.icns"
/usr/libexec/PlistBuddy -c 'Add CFBundleIconFile string Ranslator.icns' "$app/Contents/Info.plist"
"$qt/bin/macdeployqt" "$app" "-executable=$app/Contents/Helpers/tesseract" -no-codesign
# macdeployqt deploys only the cocoa platform; the headless smoke test below needs offscreen.
cp "$qt/plugins/platforms/libqoffscreen.dylib" "$app/Contents/PlugIns/platforms/"
python3 scripts/bundle-macos-libraries.py "$app" --architecture "$arch"
mkdir -p "$app/Contents/Resources/licenses"
RANSLATOR_LICENSE_DIR="$app/Contents/Resources/licenses" python3 scripts/collect-licenses.py
# Collect notices from the installed dependency closure, including Qt subformulae.
python3 - "$app/Contents/Resources/licenses" "$qt" <<'NOTICES'
import pathlib, shutil, subprocess, sys
root = pathlib.Path(sys.argv[1])
formulae = {'qt', 'tesseract'}
formulae.update(subprocess.check_output(['brew', 'deps', '--installed', '--union', 'qt', 'tesseract'], text=True).splitlines())
for formula in sorted(formulae):
    prefix = pathlib.Path(subprocess.check_output(['brew', '--prefix', formula], text=True).strip()).resolve()
    target = root / 'homebrew' / formula.replace('/', '_')
    for source in prefix.rglob('*'):
        if len(source.relative_to(prefix).parts) > 3 or not source.is_file():
            continue
        if source.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE', 'COPYRIGHT')):
            destination = target / source.relative_to(prefix)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    receipt = prefix / 'INSTALL_RECEIPT.json'
    if receipt.exists():
        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(receipt, target)
# A custom Qt prefix can provide its source-license directory independently of Homebrew.
qt = pathlib.Path(sys.argv[2])
for name in ('LICENSES', 'licenses'):
    if (qt / name).is_dir():
        shutil.copytree(qt / name, root / 'qt-source-licenses', dirs_exist_ok=True)
provenance = subprocess.check_output(['brew', 'info', '--json=v2', *sorted(formulae)], text=True)
(root / 'homebrew-provenance.json').write_text(provenance)
NOTICES
identity="${RANSLATOR_SIGN_IDENTITY:--}"
python3 - "$app" "$identity" <<'PY'
import pathlib, subprocess, sys
app = pathlib.Path(sys.argv[1]); identity = sys.argv[2]
options = ['--options', 'runtime', '--timestamp'] if identity != '-' else []
targets = []
for path in app.rglob('*'):
    if path.is_file() and not path.is_symlink():
        if 'Mach-O' in subprocess.check_output(['file', '-b', str(path)], text=True):
            targets.append(path)
targets += list(app.rglob('*.framework'))
for path in sorted(targets, key=lambda path: len(path.parts), reverse=True) + [app]:
    subprocess.run(['codesign', '--force', '--sign', identity, *options, str(path)], check=True)
PY
codesign --verify --deep --strict "$app"
profile="$(mktemp -d)"
trap 'rm -rf -- "$profile"' EXIT
export RANSLATOR_CONFIG="$profile/config.json" XDG_DATA_HOME="$profile/data"
QT_QPA_PLATFORM=offscreen "$app/Contents/MacOS/ranslator" --smoke-test "$dist/widget.png"
"$app/Contents/MacOS/ranslator" --ocr "$dist/widget.png" > "$dist/ocr-smoke.json"
python3 - "$dist/ocr-smoke.json" <<'PY'
import json, pathlib, sys
assert len(json.loads(pathlib.Path(sys.argv[1]).read_text())) > 20, 'Bundled OCR did not recognize the widget'
PY
archive="$dist/Ranslator-macos-$arch.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
if [[ -n "${RANSLATOR_NOTARY_PROFILE:-}" ]]; then
    [[ "$identity" != - ]] || { printf 'Notarization requires RANSLATOR_SIGN_IDENTITY.\n' >&2; exit 1; }
    xcrun notarytool submit "$archive" --keychain-profile "$RANSLATOR_NOTARY_PROFILE" --wait
    xcrun stapler staple "$app"
    ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
fi
printf 'Built %s and %s\n' "$app" "$archive"
