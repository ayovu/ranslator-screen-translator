#!/usr/bin/env bash
# Install a built AppImage without FUSE or system Qt/KDE packages.
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { printf 'Use the native .app package on macOS.\n' >&2; exit 1; }
[[ $# -eq 1 ]] || { printf 'Usage: %s /path/to/Ranslator-ARCH.AppImage\n' "$0" >&2; exit 2; }
package="$(realpath "$1")"
[[ -f "$package" && -x "$package" ]] || { printf 'AppImage is missing or not executable: %s\n' "$package" >&2; exit 1; }
prefix="${PREFIX:-$HOME/.local}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
if [[ "$prefix" == *$'\n'* || "$prefix" == *$'\t'* || "$prefix" == *=* ]]; then
    printf 'PREFIX must not contain a newline, tab or equals sign (Desktop Entry path restriction).\n' >&2
    exit 1
fi
mkdir -p "$prefix/opt" "$prefix/bin"
stage="$(mktemp -d "$prefix/opt/.ranslator-stage.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
(cd "$stage" && "$package" --appimage-extract >/dev/null)
app="$stage/squashfs-root"
desktop="$app/usr/share/applications/io.github.ranslator.Ranslator.desktop"
[[ -x "$app/AppRun" && -x "$app/usr/bin/ranslator" && -f "$desktop" ]] || {
    printf 'This package does not contain a Ranslator application bundle.\n' >&2; exit 1;
}
previous="$prefix/opt/.ranslator-previous"
if [[ -e "$prefix/opt/ranslator" ]]; then
    rm -rf -- "$previous"
    mv "$prefix/opt/ranslator" "$previous"
fi
if ! mv "$app" "$prefix/opt/ranslator"; then
    [[ ! -e "$previous" ]] || mv "$previous" "$prefix/opt/ranslator"
    exit 1
fi
ln -sfn "$prefix/opt/ranslator/AppRun" "$prefix/bin/ranslator"
install -Dm644 "$prefix/opt/ranslator/usr/share/icons/hicolor/scalable/apps/io.github.ranslator.Ranslator.svg" \
    "$prefix/share/icons/hicolor/scalable/apps/io.github.ranslator.Ranslator.svg"
install -Dm644 "$prefix/opt/ranslator/usr/share/applications/io.github.ranslator.Ranslator.desktop" \
    "$prefix/share/applications/io.github.ranslator.Ranslator.desktop"
# Desktop Entry Exec quoting differs from shell quoting; preserve literal special characters.
executable="$prefix/bin/ranslator"
executable="${executable//\\/\\\\}"
executable="${executable//\"/\\\"}"
executable="${executable//\`/\\\`}"
executable="${executable//\$/\\\$}"
# Desktop string escaping is decoded before command-line quoting.
executable="${executable//\\/\\\\}"
executable="${executable//%/%%}"
desktop="$prefix/share/applications/io.github.ranslator.Ranslator.desktop"
while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" == Exec=ranslator* ]]; then
        printf 'Exec="%s"%s\n' "$executable" "${line#Exec=ranslator}"
    else
        printf '%s\n' "$line"
    fi
done < "$desktop" > "$desktop.new"
mv "$desktop.new" "$desktop"
if command -v update-desktop-database >/dev/null; then
    update-desktop-database "$prefix/share/applications"
fi
# Keep this installer standalone: release downloads may contain only this script and the AppImage.
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
printf 'Installed %s. Launch Ranslator from the application menu; restart any previous running version.\n' "$prefix/bin/ranslator"
