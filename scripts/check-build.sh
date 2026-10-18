#!/usr/bin/env bash
# Local equivalent of the repository's build/format/test checks; no hosted CI required.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify-fonts.py
python3 scripts/update-translations.py check
python3 -m unittest discover -s tests -p test_translations.py
# Catalog editing is optional for an English-only build. Exercise the complete
# extractor/loader checks when Qt 6 Linguist tools are available locally.
i18n_lrelease="$(python3 - <<'PY'
import importlib.util
spec = importlib.util.spec_from_file_location('translations', 'scripts/update-translations.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    module.tool('lupdate')
    print(module.tool('lrelease'))
except (ValueError, OSError):
    pass
PY
)"
if [[ -n "$i18n_lrelease" ]]; then
    python3 scripts/update-translations.py check --fresh
else
    printf 'Skipping source extraction and native catalog tests: install Qt 6 Linguist tools to include them.\n'
fi
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked --all-targets
options=(-DRANSLATOR_UI_TESTS=ON -DRANSLATOR_FONT_TESTS=ON)
if [[ -n "$i18n_lrelease" ]]; then
    options+=(-DRANSLATOR_I18N_TESTS=ON "-DRANSLATOR_LRELEASE=$i18n_lrelease")
else
    options+=(-DRANSLATOR_I18N_TESTS=OFF)
fi
if [[ "$(uname -s)" == Linux ]]; then
    options+=(-DRANSLATOR_PORTAL_TESTS=ON -DRANSLATOR_ACCESSIBILITY_TESTS=ON -DRANSLATOR_X11_TESTS=ON -DRANSLATOR_GNOME_TESTS=ON -DRANSLATOR_AUTOSTART_TESTS=ON)
fi
cmake -S native -B build/native-checks "${options[@]}"
cmake --build build/native-checks --parallel "${RANSLATOR_BUILD_JOBS:-2}"
ctest --test-dir build/native-checks --output-on-failure
cargo build --release --locked
QT_QPA_PLATFORM=offscreen target/release/ranslator --smoke-test build/widget.png
