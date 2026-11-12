#pragma once
#include <QStringList>

// Shortcut action IDs in settings order. Every shortcut backend and the settings dialog use these
// IDs, matching the Rust `Config::bindings`. An empty sequence leaves an action unbound.
inline const QStringList shortcutActions{"translate_selection", "once", "continuous", "snapshot"};
