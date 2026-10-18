#!/usr/bin/env python3
"""Run packaged Qt rendering and OCR in isolated distribution userspaces, without host libraries.

This checks ABI and packaged dependencies. It does not certify real compositor integration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--appdir', type=Path, default=Path('build/portable/AppDir'))
parser.add_argument('--font-probe', type=Path, default=Path('build/portable/native-tests/font_probe'))
parser.add_argument('--results', type=Path, default=Path('build/portable/distro-results'))
parser.add_argument('--engine', default=os.environ.get('RANSLATOR_CONTAINER_ENGINE', 'podman'))
parser.add_argument('--only', nargs='+', help='Subset: ubuntu22 ubuntu24 debian12 debian13 fedora arch')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
appdir = args.appdir.resolve()
font_probe = args.font_probe.resolve()
assert font_probe.is_file(), 'Build the portable font_probe target first (scripts/package-portable.sh).'
results = args.results.resolve()
results.mkdir(parents=True, exist_ok=True)
apt = 'apt-get update -qq && apt-get install --no-install-recommends -y libgl1 libegl1 libdbus-1-3 libfontconfig1 libxkbcommon0 libfreetype6 libglib2.0-0 ca-certificates xvfb xauth >/dev/null'
matrix = {
    'ubuntu22': ('docker.io/library/ubuntu:22.04', apt),
    'ubuntu24': ('docker.io/library/ubuntu:24.04', apt),
    'debian12': ('docker.io/library/debian:12-slim', apt),
    'debian13': ('docker.io/library/debian:13-slim', apt),
    'fedora': ('registry.fedoraproject.org/fedora:44', 'dnf -y install mesa-libGL mesa-libEGL dbus-libs fontconfig freetype glib2 libxkbcommon ca-certificates xorg-x11-server-Xvfb xorg-x11-xauth >/dev/null'),
    'arch': ('docker.io/library/archlinux:base', 'pacman -Sy --noconfirm mesa libglvnd dbus fontconfig freetype2 glib2 libxkbcommon ca-certificates xorg-server-xvfb xorg-xauth >/dev/null'),
}
report = []
provenance = {
    'appdir_executable_sha256': hashlib.sha256((appdir / 'usr/bin/ranslator').read_bytes()).hexdigest(),
    'qt_gui_sha256': hashlib.sha256((appdir / 'usr/lib/libQt6Gui.so.6').read_bytes()).hexdigest(),
    'abi_manifest_sha256': hashlib.sha256((appdir / 'usr/share/ranslator/abi-requirements.json').read_bytes()).hexdigest(),
    'font_probe_sha256': hashlib.sha256(font_probe.read_bytes()).hexdigest(),
    'font_sources_sha256': hashlib.sha256((root / 'assets/fonts/noto/sources.json').read_bytes()).hexdigest(),
}
for name, (image, dependencies) in matrix.items():
    if args.only and name not in args.only:
        continue
    # Cache only the distribution's base graphics/test packages, never application binaries.
    # Tests then run without network access or any host library mounts.
    context = results / (name + '-image')
    context.mkdir(exist_ok=True)
    (context / 'Containerfile').write_text(f'FROM {image}\nENV DEBIAN_FRONTEND=noninteractive\nRUN {dependencies}\n')
    test_image = f'ranslator-portability-test:{name}'
    built = subprocess.run([args.engine, 'build', '-f', str(context / 'Containerfile'),
                            '-t', test_image, str(context)], capture_output=True, text=True)
    (results / f'{name}-image.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        print(f'FAIL: {name}: test image setup; see {results / (name + "-image.log")}', flush=True)
        report.append({'distribution': name, 'image': image, 'passed': False, 'phase': 'test image setup'})
        continue
    test_image_id = subprocess.check_output(
        [args.engine, 'image', 'inspect', '--format', '{{.Id}}', test_image], text=True).strip()
    command = f'''set -eu
getconf GNU_LIBC_VERSION
ldconfig -p | awk '$1 == "libOpenGL.so.0" {{ print $NF }}' > /results/{name}-system-opengl.txt
QT_QPA_PLATFORM=offscreen /bundle/AppRun --smoke-test /results/{name}.png
QT_QPA_PLATFORM=xcb xvfb-run -a /bundle/AppRun --smoke-test /results/{name}-x11.png
/bundle/AppRun --ocr /results/{name}.png > /results/{name}-ocr.json
export LD_LIBRARY_PATH=/bundle/usr/lib QT_PLUGIN_PATH=/bundle/usr/plugins QT_QPA_PLATFORM=offscreen
export FONTCONFIG_FILE=/font-fixtures/bundled-fonts-only.conf
/font-probe /results/{name}-fonts.png > /results/{name}-fonts.json 2>&1
set +e
/font-probe --without-fallback > /results/{name}-fonts-negative.json 2>&1
negative_status=$?
set -e
test "$negative_status" -eq 1
'''
    result = subprocess.run([args.engine, 'run', '--rm', '--network=none',
                             '-v', f'{appdir}:/bundle:ro', '-v', f'{results}:/results:rw',
                             '-v', f'{font_probe}:/font-probe:ro',
                             '-v', f'{root / "tests/fixtures"}:/font-fixtures:ro',
                             test_image, '/bin/sh', '-c', command], capture_output=True, text=True)
    (results / f'{name}.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(f'FAIL: {name}; see {results / (name + ".log")}', flush=True)
        report.append({'distribution': name, 'image': image, 'passed': False})
        continue
    words = json.loads((results / f'{name}-ocr.json').read_text())
    passed = len(words) > 20
    assert hashlib.sha256((appdir / 'usr/bin/ranslator').read_bytes()).hexdigest() == provenance['appdir_executable_sha256'], 'Application changed during matrix validation'
    assert hashlib.sha256((appdir / 'usr/lib/libQt6Gui.so.6').read_bytes()).hexdigest() == provenance['qt_gui_sha256'], 'Qt changed during matrix validation'
    report.append({'distribution': name, 'image': image, 'passed': passed, **provenance,
                   'test_image_id': test_image_id,
                   'runtime': result.stdout.splitlines()[0],
                   'ocr_words': len(words),
                   'system_libopengl_present': bool((results / f'{name}-system-opengl.txt').read_text().strip()), 'checks': ['Qt offscreen rendering', 'Qt X11 rendering in Xvfb', 'bundled Tesseract OCR', '11 mixed-script shaping samples with OS fonts hidden', 'missing-font negative control']})
    print(f'{"PASS" if passed else "FAIL"}: {name}: native Qt offscreen + X11 rendering, {len(words)} OCR words.', flush=True)
(results / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
raise SystemExit(0 if report and all(item['passed'] for item in report) else 1)
