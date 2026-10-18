# Translate the interface

The interface currently ships in English. This is separate from the languages
you translate **content** between. Changing an interface catalog never changes
provider settings, preferred/fallback content languages, or saved shortcuts.
OCR language packs are separate too: they let Tesseract read screen text, and do
not translate the application's controls.

The shared Qt interface uses one `Ranslator` translation context. Contributors
edit standard Qt Linguist `.ts` files; they do not need to edit Rust or C++.
Missing and unfinished entries use the original English text. There is no
automatic catalog download or language-pack marketplace.

## Create or update a language

Install Qt **6** Linguist tools (`lupdate`, `lrelease`, and optionally the
`linguist` editor). On Arch these are in `qt6-tools`; Debian/Ubuntu provide
`qt6-l10n-tools` and the Linguist editor in `linguist-qt6`. Homebrew's `qt` includes
the tools. If they are outside `PATH`, set `LUPDATE` and `LRELEASE` to their
absolute executable paths. No Linguist tool is needed to run the application or
build the current English-only sources.

From the repository root:

```sh
python3 scripts/update-translations.py update --language fr
# Open translations/ranslator_fr.ts in Qt Linguist, or a UTF-8 XML editor.
python3 scripts/update-translations.py check --fresh
python3 scripts/update-translations.py compile
```

Use locale filenames such as `ranslator_fr.ts`, `ranslator_pt_BR.ts`, or
`ranslator_zh_Hant.ts`. `translations/ranslator_en.ts` is the generated source
template. `update` merges current marked strings into every catalog and retains
existing translations. Re-run it after changing UI text. Do not translate the
`<source>`, context names, source locations, or disambiguation comments.

Keep `%1`, `%2`, `%n`, `%L1` and literal CLI placeholders such as `{model}` intact;
their order can change. Complete every plural form shown by Linguist. Preserve
intentional line breaks, keyboard hints, and ampersand mnemonics. Do not introduce
HTML into plain labels. Leave an uncertain entry marked **unfinished**: compiled
catalogs omit it, so the app displays English rather than an incomplete label.

## Preview safely

The compile command writes `.qm` files to ignored `build/translations/`.
Close another running Ranslator instance before previewing, or use the existing
isolated UI test workflows. A temporary configuration keeps saved preferences
separate:

```sh
cp config.example.json /tmp/ranslator-language-preview.json
RANSLATOR_CONFIG=/tmp/ranslator-language-preview.json \
RANSLATOR_UI_LANGUAGE=fr \
RANSLATOR_TRANSLATIONS_DIR="$PWD/build/translations" \
cargo run --locked -- --settings
```

Restart after replacing a catalog; live language switching is not implemented.
`RANSLATOR_UI_LANGUAGE=en` forces English. Without an override, the loader follows
the OS UI language preference and falls back from region/script variants to the
base language, then to English. `system` explicitly selects that behavior.

Catalogs are searched in this order:

1. `RANSLATOR_TRANSLATIONS_DIR`, when it is an explicit absolute directory.
2. The user's standard data directory plus `ranslator/translations/` (normally
   `~/.local/share/ranslator/translations/` on Linux;
   `~/Library/Application Support/ranslator/translations/` on macOS).
3. Catalogs embedded in the application.
4. `../share/ranslator/translations/` relative to the executable, and
   `Contents/Resources/translations/` in a macOS app bundle.

Flatpak uses its own app-data directory, normally
`~/.var/app/io.github.ranslator.Ranslator/data/ranslator/translations/`.
An external preview directory needs an explicit Flatpak filesystem grant;
ordinary installed catalogs need no new permission. The loader never searches
the working directory. Only install catalogs from trusted contributors: binary
QM files are parsed by Qt and can change the meaning of security-related labels.
The size/path checks do not make arbitrary downloaded catalogs trustworthy.

## Submit and validate

Submit the `.ts` file and any intentional source-template update; do not commit
generated `.qm` files. State the locale, whether a native speaker reviewed it,
and what remains unfinished. Check Settings, the tray menu, quick translation,
snapshot overlays, error hints, and narrow/resized windows. Attach screenshots
with synthetic text for clipping or right-to-left issues. A text catalog alone
does not prove every layout works in that language.

```sh
python3 -m unittest discover -s tests -p test_translations.py
cmake -S native -B build/i18n-tests -DRANSLATOR_I18N_TESTS=ON
cmake --build build/i18n-tests --target i18n_tests
ctest --test-dir build/i18n-tests --output-on-failure
```

New non-English catalogs are compiled with `lrelease -nounfinished` and embedded
by CMake. This makes native, AppImage, Flatpak and macOS builds use the same
catalogs; their builders must then provide Qt 6 `lrelease`. No runtime Linguist
dependency is added. The synthetic French catalog under `tests/fixtures/i18n/`
is a loader test, not a shipped language pack.

## When adding UI code

Include `i18n.hpp`; use `UiText::tr("Settings")` for visible literals and
`UiText::tr("%n items", nullptr, count)` for counts. Translate complete sentences
before `.arg(...)`, rather than joining translated fragments. Add `//:` translator
comments when a label needs context. The stable context survives widget refactors.

Do not wrap protocol/configuration keys, provider/model identifiers, executable
arguments, language codes, user text, or translated content. Backend/provider and
OS diagnostics can still be English; their structured localization is separate
work. Some toolkit/native dialog text comes from Qt or the operating system.
