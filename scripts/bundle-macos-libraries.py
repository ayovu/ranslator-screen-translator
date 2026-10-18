#!/usr/bin/env python3
"""Copy non-system Mach-O dependencies, rewrite load paths, and audit an .app bundle.

Run after macdeployqt. The package must also work without Homebrew on the recipient's Mac.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('app', type=Path)
parser.add_argument('--architecture', required=True, choices=('arm64', 'x86_64'))
args = parser.parse_args()
app = args.app.resolve()
frameworks = app / 'Contents/Frameworks'
frameworks.mkdir(parents=True, exist_ok=True)
origins = {}


def output(*command):
    return subprocess.check_output(command, text=True)


def macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf',
                                 b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
                                 b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca')


def dependencies(path):
    return [line.strip().split(' (compatibility version', 1)[0]
            for line in output('otool', '-L', str(path)).splitlines()[1:] if 'compatibility version' in line]


def rpaths(path):
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', output('otool', '-l', str(path)))


def locate(reference, original):
    if reference.startswith('@loader_path/'):
        candidate = original.parent / reference.removeprefix('@loader_path/')
        if candidate.exists():
            return candidate
    elif reference.startswith('@executable_path/'):
        candidate = app / 'Contents/MacOS' / reference.removeprefix('@executable_path/')
        if candidate.exists():
            return candidate
    elif reference.startswith('@rpath/'):
        suffix = reference.removeprefix('@rpath/')
        for directory in [str(frameworks), *rpaths(original)]:
            directory = directory.replace('@loader_path', str(original.parent))
            directory = directory.replace('@executable_path', str(app / 'Contents/MacOS'))
            candidate = Path(directory) / suffix
            if candidate.exists():
                return candidate
    elif reference.startswith('/') and Path(reference).exists():
        return Path(reference)
    raise RuntimeError(f'Unresolved dependency {reference} in {original}')


for path in app.rglob('*'):
    if path.is_symlink():
        assert path.resolve().is_relative_to(app), f'Bundle symlink escapes the app: {path}'
queue = [path for path in app.rglob('*') if macho(path)]
visited = set()
while queue:
    binary = queue.pop()
    if binary in visited:
        continue
    visited.add(binary)
    original = origins.get(binary, binary)
    for dependency in dependencies(binary):
        if dependency.startswith(('/System/Library/', '/usr/lib/')):
            continue
        source = locate(dependency, original)
        if source.resolve() == binary.resolve():
            # otool includes a dylib's own install name; it is not a dependency.
            continue
        if source.is_relative_to(app):
            destination = source
        else:
            if '.framework/' in str(source):
                raise RuntimeError(f'macdeployqt did not deploy framework: {source}')
            destination = frameworks / source.name
            if destination.exists():
                prior = origins.get(destination, destination)
                if hashlib.sha256(prior.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
                    raise RuntimeError(f'Different libraries have the same bundle filename: {source}')
            else:
                shutil.copy2(source.resolve(), destination)
                destination.chmod(0o755)
                origins[destination] = source
                subprocess.run(['install_name_tool', '-id', '@rpath/' + destination.name, str(destination)], check=True)
                queue.append(destination)
        replacement = '@loader_path/' + os.path.relpath(destination, binary.parent)
        subprocess.run(['install_name_tool', '-change', dependency, replacement, str(binary)], check=True)
    for value in rpaths(binary):
        if value.startswith(('/opt/', '/usr/local/', '/Users/')):
            subprocess.run(['install_name_tool', '-delete_rpath', value, str(binary)], check=True)

minimum = (0, 0)
for binary in visited:
    architectures = output('lipo', '-archs', str(binary)).strip().split()
    assert args.architecture in architectures, f'Wrong architecture in {binary}: {architectures}'
    for dependency in dependencies(binary):
        assert not dependency.startswith(('/opt/', '/usr/local/', '/Users/')), f'External library in {binary}: {dependency}'
        if not dependency.startswith(('/System/Library/', '/usr/lib/')):
            locate(dependency, binary)
    load_commands = output('otool', '-l', str(binary))
    for block in re.split(r'Load command \d+\n', load_commands):
        field = 'minos' if 'cmd LC_BUILD_VERSION\n' in block else 'version' if 'cmd LC_VERSION_MIN_MACOSX\n' in block else None
        if field and (match := re.search(r'\b' + field + r' (\d+(?:\.\d+)+)', block)):
            minimum = max(minimum, tuple(map(int, match.group(1).split('.'))))
plist = app / 'Contents/Info.plist'
info = plistlib.loads(plist.read_bytes())
minimum = max(minimum, tuple(map(int, info['LSMinimumSystemVersion'].split('.'))))
info['LSMinimumSystemVersion'] = '.'.join(map(str, minimum))
plist.write_bytes(plistlib.dumps(info))
provenance = app / 'Contents/Resources/licenses/native-dependencies.json'
provenance.parent.mkdir(parents=True, exist_ok=True)
provenance.write_text(json.dumps({str(binary.relative_to(app)): str(original) for binary, original in origins.items()}, indent=2) + '\n')
print(f'PASS: {len(visited)} Mach-O files contain {args.architecture}; minimum macOS {info["LSMinimumSystemVersion"]}; no Homebrew dependencies remain.')
