#!/usr/bin/env bash
set -euo pipefail
prefix="${PREFIX:-$HOME/.local}"
rm -f "$prefix/bin/ranslator" "$prefix/share/applications/io.github.ranslator.Ranslator.desktop" "$prefix/share/metainfo/io.github.ranslator.Ranslator.metainfo.xml" "$prefix/share/icons/hicolor/scalable/apps/io.github.ranslator.Ranslator.svg" "$prefix/share/ranslator/tessdata/eng.traineddata" "$prefix/share/ranslator/tessdata/LICENSE" "$prefix/share/licenses/ranslator/Inter-OFL.txt"
printf 'Removed the application. Your settings were preserved.\n'
