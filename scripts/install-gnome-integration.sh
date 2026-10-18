#!/bin/sh
set -eu

# Explicitly invoked by the user; never called by application startup or package installation.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
extension_source="$script_dir/../integrations/gnome/ranslator@ranslator.local"
if [ ! -f "$extension_source/extension.js" ]; then
    extension_source="$script_dir/../share/ranslator/gnome/ranslator@ranslator.local"
fi
if [ ! -f "$extension_source/extension.js" ]; then
    printf '%s\n' 'Could not find the bundled GNOME integration.' >&2
    exit 1
fi
if ! command -v gnome-extensions >/dev/null 2>&1; then
    printf '%s\n' 'Install GNOME Shell and its gnome-extensions command first.' >&2
    exit 1
fi
shell_major=$(gnome-shell --version | sed -nE 's/^GNOME Shell ([0-9]+).*/\1/p')
if [ -z "$shell_major" ] || ! grep -q "\"$shell_major\"" "$extension_source/metadata.json"; then
    printf '%s\n' 'This GNOME Shell version has not been validated for the companion yet.' \
        'Ranslator can still use desktop portals and application accessibility.' >&2
    exit 1
fi
staging=$(mktemp -d)
trap 'rm -rf -- "$staging"' EXIT HUP INT TERM
gnome-extensions pack --force --out-dir "$staging" "$extension_source"
gnome-extensions install --force "$staging/ranslator@ranslator.local.shell-extension.zip"
if gnome-extensions enable ranslator@ranslator.local && \
    gdbus call --session --timeout 2 --dest io.github.ranslator.Ranslator.Desktop \
        --object-path /io/github/ranslator/Ranslator/Desktop \
        --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1; then
    printf '%s\n' 'GNOME integration enabled. Ranslator will detect it automatically.'
else
    printf '%s\n' 'Integration installed. Log out and back in, then run:' \
        '  gnome-extensions enable ranslator@ranslator.local'
fi
