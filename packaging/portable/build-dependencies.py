#!/usr/bin/env python3
"""Build pinned Qt/KDE sources against the container's glibc, with verified archives."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tarfile
import urllib.request

prefix = Path('/opt/ranslator')
work = Path('/tmp/ranslator-dependencies')
work.mkdir(exist_ok=True)
jobs = os.environ.get('RANSLATOR_BUILD_JOBS', '8')
sources = json.loads(Path(__file__).with_name('sources.json').read_text())


def run(*command):
    subprocess.run(command, check=True)


def build(name, *options):
    if len(sys.argv) > 1 and name not in sys.argv[1:]:
        return
    item = next(source for source in sources if source['name'] == name)
    archive = work / f'{name}.tar.gz'
    with urllib.request.urlopen(item['url'], timeout=180) as response:
        archive.write_bytes(response.read())
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == item['sha256'], name
    source = work / name
    source.mkdir()
    with tarfile.open(archive) as tar:
        # Commit archives have one top-level directory; strip only that directory.
        for member in tar.getmembers():
            parts = Path(member.name).parts
            if len(parts) < 2:
                continue
            member.name = str(Path(*parts[1:]))
            if member.name.startswith('/') or '..' in Path(member.name).parts:
                raise ValueError('Unsafe archive path')
            tar.extract(member, source)
    target = work / f'{name}-build'
    if name == 'wayland-protocols':
        run('meson', 'setup', str(target), str(source), f'--prefix={prefix}', '-Dtests=false')
        run('meson', 'install', '-C', str(target))
    else:
        run('cmake', '-S', str(source), '-B', str(target), '-GNinja',
            f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DBUILD_TESTING=OFF',
            '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF', *options)
        run('cmake', '--build', str(target), '--parallel', jobs)
        run('cmake', '--install', str(target))
    notices = prefix / 'share/licenses/ranslator-dependencies' / name
    notices.mkdir(parents=True, exist_ok=True)
    for candidate in source.iterdir():
        if candidate.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
            if candidate.is_dir():
                shutil.copytree(candidate, notices / candidate.name, dirs_exist_ok=True)
            else:
                shutil.copy2(candidate, notices)
    shutil.rmtree(source)
    shutil.rmtree(target)
    archive.unlink()


build('qtbase', '-DQT_FEATURE_openssl_linked=ON', '-DQT_FEATURE_system_sqlite=OFF',
      '-DQT_FEATURE_printsupport=OFF', '-DQT_FEATURE_cups=OFF', '-DQT_FEATURE_sql=OFF',
      '-DQT_FEATURE_icu=ON', '-DQT_FEATURE_glib=OFF', '-DQT_FEATURE_dbus=ON',
      '-DQT_FEATURE_accessibility=ON', '-DQT_FEATURE_accessibility_atspi_bridge=ON',
      '-DQT_FEATURE_xcb=ON', '-DQT_FEATURE_eglfs=OFF', '-DQT_FEATURE_linuxfb=OFF')
build('wayland-protocols')
build('qtwayland', '-DQT_FEATURE_wayland_server=OFF', '-DQT_FEATURE_wayland_client=ON')
build('qtsvg')
build('extra-cmake-modules', '-DBUILD_HTML_DOCS=OFF', '-DBUILD_MAN_DOCS=OFF')
build('plasma-wayland-protocols')
build('kwindowsystem', '-DKWINDOWSYSTEM_QML=OFF', '-DBUILD_QCH=OFF', '-DKF_SKIP_PO_PROCESSING=ON')
build('kglobalaccel', '-DBUILD_QCH=OFF', '-DKF_SKIP_PO_PROCESSING=ON')
shutil.copy2(Path(__file__).with_name('sources.json'), prefix / 'share/licenses/ranslator-dependencies')
shutil.rmtree(work)
