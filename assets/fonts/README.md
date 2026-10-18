# Inter

Inter Variable v4.1, from [rsms/inter](https://github.com/rsms/inter/releases/tag/v4.1).

- Font: `https://raw.githubusercontent.com/rsms/inter/v4.1/docs/font-files/InterVariable.ttf`
- SHA-256: `4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf`
- License: SIL Open Font License 1.1; see `OFL.txt`.

Embedded using Qt resources so the UI does not depend on installed fonts.

## Translation script fallback

`noto/` contains pinned, unmodified SIL OFL fonts. Every font and license is listed
with its immutable upstream URL and SHA-256 in `noto/sources.json`.

- Noto Sans CJK Regular is a shared collection containing Japanese, Korean,
  simplified Chinese, traditional Chinese, and Hong Kong regional families.
- Noto Sans Arabic supports Arabic and Persian.
- Noto Sans Devanagari, Hebrew, Bengali, and Thai cover the remaining non-Latin
  scripts offered by the language picker.
- Inter provides Latin (including Vietnamese), Greek, and Cyrillic text.

These fonts are embedded through `native/resources.qrc`, registered at startup,
and require no downloads. CJK translation surfaces choose the regional family
from the target language. Other scripts entered manually can use fonts installed
on the system; universal Unicode or emoji coverage is not claimed.

Run `python3 scripts/verify-fonts.py` to check source integrity and license
notices. The native `font_probe` target shapes eleven multilingual samples,
rejects missing glyphs, and renders a PNG. The portable distribution checks hide
all system fonts and also verify that a control run without Noto fails, so a
passing result cannot accidentally depend on the build machine's font packages.
