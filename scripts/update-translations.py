#!/usr/bin/env python3
"""Extract, validate and compile the shared Qt UI catalogs (no network access)."""
import argparse
from collections import Counter
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
CATALOGS = ROOT / "translations"
LOCALE = re.compile(r"[a-z]{2,3}(?:_[A-Z][a-z]{3})?(?:_(?:[A-Z]{2}|[0-9]{3}))?\Z")
TOKENS = re.compile(r"%(?:L?[1-9][0-9]?|L?n)|\{(?:target|source|model)\}")


def tool(name):
    override = os.environ.get(name.upper())
    candidates = [override] if override else [name + "6", name + "-qt6", name]
    for candidate in candidates:
        if candidate and (found := shutil.which(candidate)):
            version = subprocess.run([found, "-version"], capture_output=True, text=True, check=True)
            if re.search(r"\b6\.[0-9]", version.stdout + version.stderr):
                return found
    raise ValueError(f"Install Qt 6 Linguist tools or set {name.upper()} to its executable path.")


def catalogs():
    return sorted(CATALOGS.glob("ranslator_*.ts"))


def sources():
    return sorted(str(path.relative_to(ROOT)) for path in (ROOT / "native").iterdir()
                  if path.suffix in {".cpp", ".hpp", ".mm"})


def extract(paths):
    subprocess.run([tool("lupdate"), *sources(), "-I", "native", "-locations", "relative",
                    "-source-language", "en", "-ts", *(str(path) for path in paths)],
                   cwd=ROOT, check=True)


def messages(path):
    root = ET.parse(path).getroot()
    expected = path.stem.removeprefix("ranslator_")
    if not LOCALE.fullmatch(expected) or root.get("language") != expected:
        raise ValueError(f"{path.name}: language must match its locale filename")
    if root.get("sourcelanguage") != "en":
        raise ValueError(f"{path.name}: sourcelanguage must remain en")
    result = {}
    for context in root.findall("context"):
        context_name = context.findtext("name")
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is not None and translation.get("type") in {"vanished", "obsolete"}:
                continue
            source = message.findtext("source", "")
            key = (context_name, source, message.findtext("comment", ""), message.get("numerus", "no"))
            if key in result:
                raise ValueError(f"{path.name}: duplicate message {key!r}")
            result[key] = message
            if translation is None or translation.get("type") == "unfinished":
                continue  # lrelease -nounfinished keeps these in source English.
            forms = translation.findall("numerusform") if message.get("numerus") == "yes" else [translation]
            if not forms:
                raise ValueError(f"{path.name}: missing plural forms for {source!r}")
            for form in forms:
                text = "".join(form.itertext())
                if not text.strip():
                    raise ValueError(f"{path.name}: empty finished translation for {source!r}")
                if Counter(TOKENS.findall(source)) != Counter(TOKENS.findall(text)):
                    raise ValueError(f"{path.name}: preserve placeholders in {source!r}")
    return result


def validate(paths, fresh=False):
    if not paths:
        raise ValueError("No translations/ranslator_*.ts catalogs found; run update first.")
    template = messages(CATALOGS / "ranslator_en.ts")
    for path in paths:
        found = messages(path)
        missing, extra = template.keys() - found.keys(), found.keys() - template.keys()
        if missing or extra:
            raise ValueError(f"{path.name}: catalog differs from English template; run update "
                             f"({len(missing)} missing, {len(extra)} extra)")
    if fresh:
        with tempfile.TemporaryDirectory(prefix="ranslator-lupdate-") as directory:
            copy = Path(directory) / "ranslator_en.ts"
            shutil.copyfile(CATALOGS / copy.name, copy)
            extract([copy])
            if messages(copy).keys() != template.keys():
                raise ValueError("Marked UI strings changed; run scripts/update-translations.py update.")
    print(f"Validated {len(paths)} catalog(s), {len(template)} active messages each.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    update = sub.add_parser("update", help="Merge marked UI strings, preserving existing translations")
    update.add_argument("--language", help="Create a locale catalog, for example fr, pt_BR or zh_Hant")
    check = sub.add_parser("check", help="Check locale names, message coverage and placeholders")
    check.add_argument("--fresh", action="store_true", help="Also compare with current sources (requires lupdate)")
    compile_parser = sub.add_parser("compile", help="Build optional QM catalogs; English needs no QM")
    compile_parser.add_argument("--output", type=Path, default=ROOT / "build/translations")
    args = parser.parse_args()
    CATALOGS.mkdir(exist_ok=True)
    if args.command == "update":
        language = args.language
        if language and not LOCALE.fullmatch(language):
            raise ValueError("Use a locale such as fr, pt_BR or zh_Hant; paths are not accepted.")
        for locale in ("en", language):
            if not locale:
                continue
            path = CATALOGS / f"ranslator_{locale}.ts"
            if not path.exists():
                path.write_text('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'
                                f'<TS version="2.1" language="{locale}" sourcelanguage="en"/>\n')
        extract(catalogs())
        validate(catalogs())
    elif args.command == "check":
        validate(catalogs(), args.fresh)
    else:
        validate(catalogs())
        paths = [path for path in catalogs() if path.stem != "ranslator_en"]
        if not paths:
            print("English is built in; there are no additional catalogs to compile.")
            return
        compiler = tool("lrelease")
        args.output.mkdir(parents=True, exist_ok=True)
        for path in paths:
            subprocess.run([compiler, "-nounfinished", str(path), "-qm",
                            str(args.output / (path.stem + ".qm"))], check=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, ET.ParseError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
