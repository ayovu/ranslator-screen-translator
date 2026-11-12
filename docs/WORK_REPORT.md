# Work report — 2026-10-18

## Delivered

- Rust/Qt translation app: selection, snapshot OCR, multiple movable/resizable
  translation cards, expanded reading, editable languages and provider profiles.
  Shortcut fields now record keys, with cancellation and separate repeat counts.
- Ollama, compatible APIs, translation services and host CLI runtimes; clearer
  cancellation, errors and translation instructions. Improved focus and controls;
  corrected content padding inside the rounded window, excluding its shadow area.
- Start-at-login switch in tray and Settings; Linux XDG, Flatpak portal and macOS
  Login Items adapters. Changes apply immediately with system-response feedback,
  including both accessible checkbox actions (Press and Toggle).
- Chinese → English Adventure Time example, blue glass SVG icon and Mac tray mask.
- Qt interface translation catalogs and contributor tooling; English remains the
  source language and fallback for untranslated strings.
- Separate Linux packages/source archive, build/test scripts, short README,
  contributor/agent rules, `.gitignore` and release/store guidance. Git remote points
  to `ayovu/ranslator-screen-translator`; no commits, pushes or store publication.
- GPL v3 application license and package metadata; third-party notices retained.
- Snapshot GIF in the README: actual PDF OCR, local English → Spanish translation,
  multiple selections and movable cards, with cursor highlights and instructions.

## Validation and remaining limits

Local Rust/native suites pass. Isolated checks cover X11, Sway, OCR, providers,
Flatpak boundaries, accessibility and autostart. Package reports record artifact hashes.

- **macOS:** source port only; compilation, permissions, login behavior and Retina/
  multi-monitor interactions need a real Mac. Full-window mode may open at login.
- **Linux:** portal/compositor support varies. Sway capture currently uses the whole
  desktop; GNOME modifier-only shortcuts are unavailable. Physical X11 remains untested.
- **Autostart:** test an actual logout/login. Minimal sessions need an XDG runner;
  moved AppImages need registration again. Flatpak displays last-confirmed state.
- **Rendering:** the reported close/reopen rectangle defect remains unresolved;
  isolated reproduction attempts were clean.
- **Translation/OCR:** LLMs can omit or distort meaning despite prompts. English OCR
  is bundled; other scripts need packs. Automatic mode may use online MyMemory.
  The small local model also returned unchanged English in a Ukrainian-target probe.
- **Demo:** isolated X11 recording with a warm model; it does not establish cold-start
  performance or reproduce every compositor's glass effects.
- Context, word alternatives and translated text painted over the original image
  remain proposals. AUR/Flathub are not published.

See [platform checks](PORTABILITY.md), [translations](TRANSLATING.md) and
[release requirements](RELEASING.md).

## Desktop icon refresh — 2026-10-19

- Confirmed the installed SVG and current tray use the blue glass icon. Refreshed
  the local icon cache and KDE application index, and notified running icon loaders.
- Both Linux installers now refresh caches after installing the icon; the AppImage
  installer remains standalone. No application artwork or translation behavior changed.
- Validation: `scripts/check-build.sh` passed; the KDE-loaded icon matches the source
  pixel for pixel. Installation checks use disposable prefixes.
- Remaining check: visually confirm the reopened Plasma application search and taskbar.
  Desktop automation was unavailable in this session. Previously published installers
  retain their old behavior until updated; existing release binaries were not replaced.
