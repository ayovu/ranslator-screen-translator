#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
prefix="${PREFIX:-$HOME/.local}"
if [[ "${1:-}" != "--no-build" ]]; then cargo build --release --locked; fi
install -Dm755 target/release/ranslator "$prefix/bin/ranslator"
install -Dm644 LICENSE "$prefix/share/licenses/ranslator/LICENSE"
install -Dm644 assets/tessdata/eng.traineddata "$prefix/share/ranslator/tessdata/eng.traineddata"
install -Dm644 assets/tessdata/LICENSE "$prefix/share/ranslator/tessdata/LICENSE"
install -Dm644 assets/fonts/OFL.txt "$prefix/share/licenses/ranslator/Inter-OFL.txt"
for notice in assets/fonts/noto/*-OFL.txt assets/fonts/noto/sources.json; do
    install -Dm644 "$notice" "$prefix/share/licenses/ranslator/noto/$(basename "$notice")"
done
install -Dm644 assets/io.github.ranslator.Ranslator.svg "$prefix/share/icons/hicolor/scalable/apps/io.github.ranslator.Ranslator.svg"
install -Dm644 assets/io.github.ranslator.Ranslator.metainfo.xml "$prefix/share/metainfo/io.github.ranslator.Ranslator.metainfo.xml"
install -Dm644 assets/io.github.ranslator.Ranslator.desktop "$prefix/share/applications/io.github.ranslator.Ranslator.desktop"
python3 - "$prefix" <<'PY'
from pathlib import Path
import sys
prefix=Path(sys.argv[1]); path=prefix/'share/applications/io.github.ranslator.Ranslator.desktop'
# Desktop Exec quoting is separate from shell quoting.
exe=str(prefix/'bin/ranslator').replace('\\','\\\\').replace('"','\\"').replace('`','\\`').replace('$','\\$')
path.write_text(path.read_text().replace('Exec=ranslator', 'Exec="'+exe+'"'))
PY
if command -v update-desktop-database >/dev/null; then update-desktop-database "$prefix/share/applications"; fi
printf 'Installed Ranslator. Launch it from your application menu or %s/bin/ranslator\n' "$prefix"
