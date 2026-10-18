#!/usr/bin/env bash
# Real pointer/keyboard events, restricted to synthetic windows on a private server.
set -euo pipefail
fixture_dir="$(cd "$(dirname "$0")" && pwd)"
test_profile="$(mktemp -d -t ranslator-shortcut-private-XXXXXXXX)"
trap 'rm -rf "$test_profile"' EXIT
mkdir -m 700 "$test_profile"/{home,config,data,cache,runtime,system}
env -u DISPLAY -u WAYLAND_DISPLAY -u SWAYSOCK -u DBUS_SESSION_BUS_ADDRESS \
    -u AT_SPI_BUS_ADDRESS -u QT_QPA_PLATFORMTHEME \
    HOME="$test_profile/home" XDG_CONFIG_HOME="$test_profile/config" \
    XDG_CONFIG_DIRS="$test_profile/system" XDG_DATA_HOME="$test_profile/data" \
    XDG_CACHE_HOME="$test_profile/cache" XDG_RUNTIME_DIR="$test_profile/runtime" \
    RANSLATOR_CONFIG="$test_profile/config/settings.json" \
    QT_QPA_PLATFORM=xcb QT_STYLE_OVERRIDE=Fusion GDK_BACKEND=x11 \
    QT_ACCESSIBILITY=0 NO_AT_BRIDGE=1 XDG_SESSION_TYPE=x11 XDG_CURRENT_DESKTOP=XFCE \
    RANSLATOR_TEST_X11=1 \
    xvfb-run -a -s '-screen 0 1280x960x24' \
    dbus-run-session --config-file "$fixture_dir/session-bus.conf" -- "$@"
