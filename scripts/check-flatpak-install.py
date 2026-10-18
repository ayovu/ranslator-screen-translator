#!/usr/bin/env python3
"""Install and run an exported Flatpak with its real permissions and a disposable profile.

Requires Flatpak, bubblewrap, D-Bus, and the package's runtime installed system-wide.
The installation is isolated with FLATPAK_USER_DIR. A private mount namespace
hides the real home directory without changing HOME or the user's files.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('bundle', type=Path)
parser.add_argument('--report', type=Path, default=Path('build/flatpak-installed-report.json'))
args = parser.parse_args()
bundle = args.bundle.resolve()
report_path = args.report.resolve()
assert bundle.is_file(), f'Missing bundle: {bundle}'
for program in ('flatpak', 'bwrap', 'dbus-run-session'):
    assert shutil.which(program), f'Install {program} before running this check.'
appid = 'io.github.ranslator.Ranslator'


def metadata(text):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read_string(text)
    return {section: dict(parser[section]) for section in parser.sections()}


def items(value):
    return set(filter(None, value.split(';')))


with tempfile.TemporaryDirectory(prefix='ranslator-installed-flatpak-') as temporary:
    temporary = Path(temporary)
    installation = temporary / 'installation'
    private_home = temporary / 'private-home'
    profile = temporary / 'profile'
    private_home.mkdir()
    profile.mkdir()
    package = temporary / 'Ranslator.flatpak'
    shutil.copy2(bundle, package)
    environment = os.environ | {'FLATPAK_USER_DIR': str(installation),
                                'XDG_CONFIG_HOME': str(private_home / '.config'),
                                'XDG_CACHE_HOME': str(private_home / '.cache'),
                                'XDG_DATA_HOME': str(private_home / '.local/share')}
    # Flatpak otherwise stores app caches under the real ~/.var even with a custom
    # installation. Only this child namespace sees the disposable home mount.
    bus_config = temporary / 'session-bus.conf'
    bus_config.write_text('''<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>
<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
    prefix = ['dbus-run-session', '--config-file', str(bus_config), '--', 'bwrap', '--bind', '/', '/', '--dev-bind', '/dev', '/dev', '--bind', str(private_home), str(Path.home()),
              '--chdir', '/', '--die-with-parent', '--', 'flatpak']

    def run(*arguments):
        result = subprocess.run(prefix + list(arguments), env=environment, cwd='/',
                                text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f'Flatpak command failed: {arguments}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    run('install', '--user', '--bundle', '--no-deps', '--no-related', '--no-pull',
        '--noninteractive', str(package))
    installed = metadata(run('info', '--user', '--show-metadata', appid))
    runtime = installed['Application']['runtime']
    runtime_commit = run('info', '--system', '--show-commit', runtime).strip()
    location = Path(run('info', '--user', '--show-location', appid).strip())
    assert location.is_relative_to(installation), 'Installed outside the isolated user directory'
    assert installed['Application']['command'] == 'ranslator-launch'
    assert run('list', '--user', '--app', '--columns=application').splitlines() == [appid]
    # No sockets, devices, bus names, networking or broad filesystem permissions
    # are added. The only grant is this test's disposable profile/output folder.
    options = ['run', '--user', f'--filesystem={profile}',
               '--env=QT_QPA_PLATFORM=offscreen',
               f'--env=RANSLATOR_CONFIG={profile / "config.json"}',
               f'--env=XDG_CONFIG_HOME={profile / "config"}',
               f'--env=XDG_DATA_HOME={profile / "data"}',
               f'--env=XDG_CACHE_HOME={profile / "cache"}']
    effective = metadata(run(*options, '--command=cat', appid, '/.flatpak-info'))
    expected_context = installed.get('Context', {})
    actual_context = effective.get('Context', {})
    for key in ('shared', 'sockets', 'devices', 'filesystems'):
        expected = items(expected_context.get(key, ''))
        if key == 'filesystems':
            expected.add(str(profile))
        assert items(actual_context.get(key, '')) == expected, (key, expected, actual_context)
    for section in ('Session Bus Policy', 'System Bus Policy'):
        assert effective.get(section, {}) == installed.get(section, {}), (section, effective.get(section))
    doctor = json.loads(run(*options, appid, '--doctor'))
    assert doctor['english_ocr'] and doctor['tesseract'], doctor
    assert doctor['config'] == str(profile / 'config.json'), doctor
    widget = profile / 'widget.png'
    rendered = json.loads(run(*options, appid, '--smoke-test', str(widget)))
    assert rendered['platform'] == 'offscreen', rendered
    words = json.loads(run(*options, appid, '--ocr', str(widget)))
    assert len(words) > 20, words
    report_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(widget, report_path.with_suffix('.png'))
    report = {
        'passed': True,
        'bundle_sha256': hashlib.sha256(bundle.read_bytes()).hexdigest(),
        'installed_executable_sha256': hashlib.sha256((location / 'files/bin/ranslator').read_bytes()).hexdigest(),
        'flatpak_version': run('--version').strip(),
        'runtime': runtime,
        'system_runtime_commit': runtime_commit,
        'installation': 'Disposable FLATPAK_USER_DIR; installed exported bundle; actual flatpak run',
        'profile': 'Disposable private home mount and synthetic config/data/cache; real HOME environment unchanged',
        'session_bus': 'Private D-Bus session without desktop service activation; no compositor or document-portal behavior is exercised',
        'extra_permissions': ['Filesystem access to the disposable profile/output folder only'],
        'metadata': installed,
        'effective_permissions_match_metadata_plus_temporary_profile': True,
        'checks': ['Dependency doctor', 'Qt offscreen rendering', 'Bundled Tesseract OCR'],
        'ocr_words': len(words),
    }
    report_path.write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS: installed Flatpak respects exported permissions; Qt and bundled OCR ({len(words)} words).')
print(report_path)
