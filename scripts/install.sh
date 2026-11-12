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
# Replacing an SVG inside an existing directory does not invalidate desktop icon caches.
touch "$prefix/share/icons/hicolor"
if command -v gtk-update-icon-cache >/dev/null; then
    gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" || printf 'Warning: could not refresh the icon cache.\n' >&2
fi
if [[ "${XDG_CURRENT_DESKTOP:-}" == *KDE* && -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    for cache_builder in kbuildsycoca6 kbuildsycoca5; do
        if command -v "$cache_builder" >/dev/null; then
            "$cache_builder" --noincremental || printf 'Warning: could not refresh the KDE application index.\n' >&2
            break
        fi
    done
    if command -v dbus-send >/dev/null; then
        # KIconLoader::emitChange: also invalidate icons held by the running panel and launcher.
        for icon_group in 0 1 2 3 4 5; do
            dbus-send --session --type=signal /KIconLoader org.kde.KIconLoader.iconChanged "int32:$icon_group" \
                || printf 'Warning: could not notify KDE of the icon change.\n' >&2
        done
    fi
fi
printf 'Installed Ranslator. Launch it from your application menu or %s/bin/ranslator\n' "$prefix"
