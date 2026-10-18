#!/usr/bin/env python3
"""Audit every bundled ELF dependency; reject accidental newer-glibc build inputs."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--appdir', type=Path, default=Path('build/AppDir'))
parser.add_argument('--max-glibc', help='Reject an ABI baseline newer than this version, e.g. 2.35')
args = parser.parse_args()
assert (args.appdir / 'usr/bin/ranslator').is_file(), 'Missing packaged executable'
for forbidden in ('libc.so.6', 'ld-linux-x86-64.so.2', 'ld-linux-aarch64.so.1', 'libstdc++.so.6'):
    assert not list(args.appdir.rglob(forbidden)), f'Do not shadow a host ABI/graphics runtime: {forbidden}'
requirements = []
needed_libraries = set()
for path in sorted(args.appdir.rglob('*')):
    if not path.is_file() or path.is_symlink():
        continue
    with path.open('rb') as stream:
        if stream.read(4) != b'\x7fELF':
            continue
    output = subprocess.check_output(['readelf', '--version-info', str(path)], text=True)
    versions = {tuple(map(int, value.split('.'))) for value in re.findall(r'\bGLIBC_(\d+\.\d+(?:\.\d+)?)\b', output)}
    maximum = max(versions, default=(0, 0))
    cxx_versions = {tuple(map(int, value.split('.'))) for value in re.findall(r'\bGLIBCXX_(\d+\.\d+(?:\.\d+)?)\b', output)}
    cxx_maximum = max(cxx_versions, default=(0, 0, 0))
    dynamic = subprocess.check_output(['readelf', '--dynamic', str(path)], text=True)
    for line in dynamic.splitlines():
        if '(NEEDED)' in line or '(RPATH)' in line or '(RUNPATH)' in line:
            value = line.split('[', 1)[1].split(']', 1)[0]
            if '(NEEDED)' in line:
                needed_libraries.add(value)
            assert not any(part.startswith(('/home/', '/opt/', '/src/')) for part in value.split(':')), \
                f'Build-machine path leaked into {path}: {value}'
            if '(NEEDED)' in line and (value.startswith(('libQt6', 'libKF6')) or value == 'libOpenGL.so.0'):
                assert (args.appdir / 'usr/lib' / value).is_file(), \
                    f'Plugin/framework dependency is not bundled: {path} needs {value}'
    requirements.append({'path': str(path.relative_to(args.appdir)),
                         'glibc': '.'.join(map(str, maximum)), 'glibcxx': '.'.join(map(str, cxx_maximum))})
maximum = max(tuple(map(int, item['glibc'].split('.'))) for item in requirements)
cxx_maximum = max(tuple(map(int, item['glibcxx'].split('.'))) for item in requirements)
report = {'minimum_glibc': '.'.join(map(str, maximum)), 'minimum_glibcxx': '.'.join(map(str, cxx_maximum)), 'elf_files': len(requirements),
          'dependencies': requirements,
          'host_libraries': sorted(name for name in needed_libraries
                                   if not (args.appdir / 'usr/lib' / name).is_file())}
dest = args.appdir / 'usr/share/ranslator/abi-requirements.json'
dest.parent.mkdir(parents=True, exist_ok=True)
dest.write_text(json.dumps(report, indent=2) + '\n')
if args.max_glibc:
    limit = tuple(map(int, args.max_glibc.split('.')))
    violations = [item for item in requirements if tuple(map(int, item['glibc'].split('.'))) > limit]
    assert not violations, f'Bundled code requires newer than glibc {args.max_glibc}: {violations}'
print(f"PASS: {len(requirements)} bundled ELF files audited; minimum glibc {report['minimum_glibc']}, GLIBCXX {report['minimum_glibcxx']}.")
