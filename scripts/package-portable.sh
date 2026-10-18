#!/usr/bin/env bash
# Build against Ubuntu 22.04's glibc 2.35, independent of the host distribution.
set -euo pipefail
cd "$(dirname "$0")/.."
repo="$PWD"
engine="${RANSLATOR_CONTAINER_ENGINE:-podman}"
command -v "$engine" >/dev/null || { printf 'Install podman or set RANSLATOR_CONTAINER_ENGINE=docker.\n' >&2; exit 1; }
arch="$(uname -m)"
image="ranslator-builder:ubuntu22.04-$arch"
mkdir -p build/portable dist/portable
python3 scripts/build-source-manifest.py build/portable/source-manifest.sha256
"$engine" build -f packaging/portable/Containerfile -t "$image" .
# Separate Cargo/Qt build trees prevent host paths or ABI-specific cached objects leaking in.
# Preserve caller ownership with both rootless Podman and Docker.
user_args=(--user "$(id -u):$(id -g)")
if [[ "$(basename "$engine")" == podman ]]; then
    user_args=(--userns=keep-id --user "$(id -u):$(id -g)")
fi
"$engine" run --rm "${user_args[@]}" \
    -v "$repo:/src:rw" -e CARGO_HOME=/src/build/portable/cargo \
    -e CARGO_TARGET_DIR=/src/build/portable/target \
    -e RANSLATOR_APPDIR=/src/build/portable/AppDir \
    -e RANSLATOR_DIST_DIR=/src/dist/portable \
    -e RANSLATOR_REQUIRE_LINUX_INTEGRATIONS=1 \
    -e RANSLATOR_GLIBC_MAX=2.35 \
    -e QMAKE=/opt/ranslator/bin/qmake \
    "$image" bash -ec '
        python3 scripts/verify-fonts.py
        python3 scripts/update-translations.py check
        python3 -m unittest discover -s tests -p test_translations.py
        cargo test --locked --all-targets
        cmake -S native -B build/portable/native-tests \
            -DRANSLATOR_FONT_TESTS=ON -DRANSLATOR_UI_TESTS=ON -DRANSLATOR_AUTOSTART_TESTS=ON
        cmake --build build/portable/native-tests --target font_probe ui_tests autostart_tests --parallel 2
        ctest --test-dir build/portable/native-tests --output-on-failure
        scripts/package-appimage.sh
    '
python3 scripts/verify-portability.py --appdir build/portable/AppDir --max-glibc 2.35
python3 scripts/build-source-manifest.py build/portable/source-manifest.after.sha256
cmp build/portable/source-manifest.sha256 build/portable/source-manifest.after.sha256
sha256sum --check --status build/portable/source-manifest.sha256
cp build/portable/source-manifest.sha256 dist/portable/build-sources.sha256
printf 'Portable package: dist/portable/Ranslator-%s.AppImage\n' "$arch"
