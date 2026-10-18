#!/usr/bin/env python3
"""Offline packaging smoke checks. Requires completed AppImage and Flatpak builds."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dist', type=Path, default=root / ('dist/native-linux-' + os.uname().machine))
parser.add_argument('--release', type=Path, default=root / 'target/release/ranslator')
parser.add_argument('--stage', type=Path)
parser.add_argument('--kde-blur', action='store_true')
args = parser.parse_args()
dist = args.dist.resolve()
release = args.release.resolve()
stage = args.stage.resolve() if args.stage else max(root.glob('build/flatpak-stage.*'), key=lambda path: path.stat().st_mtime)
appimage = dist / ('Ranslator-' + os.uname().machine + '.AppImage')
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    subprocess.run([str(appimage), '--appimage-extract', 'usr/bin/ranslator'], cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    repository = tmp / 'flatpak-repo'
    subprocess.run(['ostree', f'--repo={repository}', 'init', '--mode=archive-z2'], check=True)
    subprocess.run(['flatpak', 'build-import-bundle', str(repository), str(dist / 'Ranslator.flatpak')], check=True, stdout=subprocess.DEVNULL)
    ref = subprocess.check_output(['ostree', f'--repo={repository}', 'refs'], text=True).strip()
    assert ref.startswith('app/io.github.ranslator.Ranslator/')
    subprocess.run(['ostree', f'--repo={repository}', 'checkout', '--user-mode', ref, str(tmp / 'flatpak')], check=True)
    hashes = []
    for index, binary in enumerate([release, tmp / 'squashfs-root/usr/bin/ranslator', tmp / 'flatpak/files/bin/ranslator', stage / 'files/bin/ranslator']):
        section = Path(tmp) / str(index)
        subprocess.run(['objcopy', f'--dump-section=.text={section}', str(binary), str(Path(tmp) / f'copy{index}')], check=True)
        hashes.append(hashlib.sha256(section.read_bytes()).hexdigest())
    assert len(set(hashes)) == 1, 'A package contains a different executable'
print('PASS: AppImage and Flatpak bundle machine code matches the tested release.')

with tempfile.TemporaryDirectory(prefix='ranslator-bundle-profile-') as profile:
    env = os.environ | {'QT_QPA_PLATFORM': 'offscreen', 'APPIMAGE_EXTRACT_AND_RUN': '1',
                        'RANSLATOR_CONFIG': str(Path(profile) / 'config.json'),
                        'XDG_DATA_HOME': str(Path(profile) / 'data')}
    subprocess.run([str(appimage), '--smoke-test', str(dist / 'appimage-widget.png')], check=True, env=env)
    result = subprocess.run([str(appimage), '--ocr', str(dist / 'appimage-widget.png')], check=True, capture_output=True, text=True, env=env)
    assert len(json.loads(result.stdout)) > 20
    print('PASS: AppImage Qt rendering and bundled OCR.')

    command = ['flatpak', 'build', '--with-appdir', '--runtime', f'--filesystem={dist}', '--env=QT_QPA_PLATFORM=offscreen',
               '--env=RANSLATOR_CONFIG=/tmp/ranslator-verify/config.json',
               '--env=XDG_DATA_HOME=/tmp/ranslator-verify/data', str(stage), '/app/bin/ranslator-launch']
    subprocess.run(command + ['--smoke-test', str(dist / 'flatpak-widget.png')], check=True)
    result = subprocess.run(command + ['--ocr', str(dist / 'flatpak-widget.png')], check=True, capture_output=True, text=True)
    assert len(json.loads(result.stdout)) > 20
    print('PASS: Flatpak Qt rendering and bundled OCR inside the target runtime.')

    if args.kde_blur:
        # flatpak build clears WAYLAND_DISPLAY even when its socket is exposed.
        # Pass it explicitly; flatpak run supplies it during a normal desktop launch.
        wayland = os.environ['WAYLAND_DISPLAY']
        commands = [
            [str(appimage), '--smoke-test', str(dist / 'appimage-material.png')],
            ['flatpak', 'build', '--with-appdir', '--runtime', '--socket=wayland',
             f'--filesystem={dist}', f'--env=WAYLAND_DISPLAY={wayland}',
             '--env=QT_QPA_PLATFORM=wayland', str(stage), '/app/bin/ranslator-launch',
             '--smoke-test', str(dist / 'flatpak-material.png')],
        ]
        for command in commands:
            result = subprocess.run(command, check=True, capture_output=True, text=True,
                                    env=env | {'QT_QPA_PLATFORM': 'wayland'}, timeout=30)
            report = json.loads(result.stdout)
            assert report['desktop_blur_available'] and report['translucent_window'], report
        print('PASS: native KDE background blur is available in both Wayland packages.')
