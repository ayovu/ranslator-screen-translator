#!/usr/bin/env python3
"""Collect application/data/crate notices and binary package provenance for local bundles."""
import json
import os
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
appdir = pathlib.Path(os.environ.get('RANSLATOR_APPDIR', root / 'build/AppDir'))
dest = pathlib.Path(os.environ.get('RANSLATOR_LICENSE_DIR', appdir / 'usr/share/licenses/ranslator'))
dest.mkdir(parents=True, exist_ok=True)
shutil.copy2(root / 'LICENSE', dest / 'LICENSE')
shutil.copy2(root / 'assets/tessdata/LICENSE', dest / 'tessdata-LICENSE')
shutil.copy2(root / 'assets/fonts/OFL.txt', dest / 'Inter-OFL.txt')
for notice in (root / 'assets/fonts/noto').glob('*-OFL.txt'):
    shutil.copy2(notice, dest / notice.name)
shutil.copy2(root / 'assets/fonts/noto/sources.json', dest / 'Noto-sources.json')
shutil.copy2(root / 'assets/protocols/keystate.xml', dest / 'KDE-keystate-protocol.xml')
metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--locked', '--format-version=1'], cwd=root))
packages = []
for package in metadata['packages']:
    if package['source'] is None:
        continue
    directory = pathlib.Path(package['manifest_path']).parent
    target = dest / 'rust' / (package['name'] + '-' + package['version'])
    for item in directory.iterdir():
        if item.is_file() and item.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target / item.name)
    packages.append({key: package.get(key) for key in ('name', 'version', 'license', 'repository', 'source')})
(dest / 'rust-dependencies.json').write_text(json.dumps(packages, indent=2) + '\n')
if shutil.which('pacman'):
    owners = set()
    for library in (appdir / 'usr/lib').glob('*.so*'):
        original = pathlib.Path('/usr/lib') / library.name
        result = subprocess.run(['pacman', '-Qqo', str(original)], capture_output=True, text=True)
        if result.returncode == 0:
            owners.update(result.stdout.splitlines())
    owners.update(['tesseract', 'qt6-base', 'qt6-wayland'])
    records = []
    for owner in sorted(owners):
        info = subprocess.run(['pacman', '-Qi', owner], capture_output=True, text=True)
        records.append(info.stdout)
        source = pathlib.Path('/usr/share/licenses') / owner
        if source.is_dir():
            shutil.copytree(source, dest / 'system' / owner, dirs_exist_ok=True)
    common = pathlib.Path('/usr/share/licenses/common')
    if common.is_dir():
        shutil.copytree(common, dest / 'system/common', dirs_exist_ok=True)
    (dest / 'system-packages.txt').write_text('\n'.join(records))
elif shutil.which('dpkg-query'):
    # Debian/Ubuntu builders must carry notices too; the Arch-only path is not enough.
    owners = set()
    for library in (appdir / 'usr/lib').glob('*.so*'):
        result = subprocess.run(['dpkg-query', '-S', '*/' + library.name],
                                capture_output=True, text=True)
        if result.returncode == 0:
            owners.update(line.rsplit(': ', 1)[0] for line in result.stdout.splitlines())
    owners.add('tesseract-ocr')
    records = []
    for owner in sorted(owners):
        result = subprocess.run(['dpkg-query', '-W', '-f', '${Package}\t${Version}\t${Source}\n', owner],
                                capture_output=True, text=True)
        records.append(result.stdout)
        source = pathlib.Path('/usr/share/doc') / owner.split(':')[0] / 'copyright'
        if source.exists():
            target = dest / 'system' / owner
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    common = pathlib.Path('/usr/share/common-licenses')
    if common.is_dir():
        shutil.copytree(common, dest / 'system/common', dirs_exist_ok=True)
    (dest / 'system-packages.txt').write_text(''.join(records))
source_notices = pathlib.Path('/opt/ranslator/share/licenses/ranslator-dependencies')
if source_notices.is_dir():
    shutil.copytree(source_notices, dest / 'source-dependencies', dirs_exist_ok=True)
print('Collected dependency notices in', dest)
