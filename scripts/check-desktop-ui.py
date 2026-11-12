#!/usr/bin/env python3
"""Check multi-output snapshots and live blur on a private, headless KWin.

Requires KWin, Spectacle, Qt's ui_tests, dbus-run-session and Python Pillow.
All captured content belongs to synthetic fixtures; the user's session is never used.
Run after scripts/check-build.sh. Results are written to build/desktop-ui-checks/.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import statistics
import subprocess
import tempfile
import time

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build/desktop-ui-checks'
OUT.mkdir(exist_ok=True)
for pattern in ('ready-*', 'ack-*'):
    for path in OUT.glob(pattern):
        path.unlink()
for program in ('kwin_wayland', 'spectacle', 'dbus-run-session'):
    if not shutil.which(program, path='/usr/bin:/bin'):
        raise SystemExit(f'Install {program} to run the isolated desktop check.')
if not (ROOT / 'build/native-checks/ui_tests').is_file():
    raise SystemExit('Run scripts/check-build.sh first.')

# In particular, discard the host DISPLAY, WAYLAND_DISPLAY, session bus and AppImage libraries.
env = {key: os.environ[key] for key in ('HOME', 'USER', 'LANG') if key in os.environ}
env.update(PATH='/usr/bin:/bin', QT_FORCE_STDERR_LOGGING='1', XDG_SESSION_TYPE='wayland')
with tempfile.TemporaryDirectory(prefix='ranslator-desktop-ui-') as folder:
    temporary = Path(folder)
    runtime = temporary / 'runtime'
    runtime.mkdir(mode=0o700)
    config = temporary / 'config'
    config.mkdir()
    (config / 'kwinrc').write_text('[Plugins]\nblurEnabled=true\ncontrastEnabled=false\n'
                                  '[Compositing]\nBackend=OpenGL\nEnabled=true\n')
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config),
               XDG_DATA_HOME=str(temporary / 'data'), XDG_CURRENT_DESKTOP='KDE',
               RANSLATOR_MATERIAL_PROBE_DIR=str(OUT), RANSLATOR_TEST_DESKTOP_LAYOUT='1',
               RANSLATOR_TEST_OCR_BINARY=str(ROOT / 'target/release/ranslator'))
    writer = temporary / 'session.py'
    writer.write_text('import os,json\nfrom pathlib import Path\nPath(' +
                      repr(str(OUT / 'session.json')) + ').write_text(json.dumps({k:os.environ[k] '
                      'for k in ["WAYLAND_DISPLAY","DBUS_SESSION_BUS_ADDRESS"]}))\n')
    runner = temporary / 'run-fixtures'
    runner.write_text('#!/bin/sh\nexport QT_QPA_PLATFORM=wayland\n' +
                      shlex.join(['/usr/bin/python3', str(writer)]) + '\nexec ' +
                      shlex.join([str(ROOT / 'build/native-checks/ui_tests'),
                                  'desktop_snapshot_opens_one_view_per_output_and_routes_results',
                                  'material_probe_on_isolated_compositor']) +
                      ' > ' + shlex.quote(str(OUT / 'tests.log')) + ' 2>&1\n')
    runner.chmod(0o700)
    with (OUT / 'kwin.log').open('w') as log:
        process = subprocess.Popen(
            ['dbus-run-session', '--config-file', str(ROOT / 'tests/fixtures/session-bus.conf'),
             '--', 'kwin_wayland', '--virtual', '--socket', 'ranslator-test', '--width', '1000',
             '--height', '800', '--output-count', '2', '--no-lockscreen', '--no-global-shortcuts',
             '--no-kactivities', '--exit-with-session', str(runner)], env=env,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 80
            stage = 0
            while process.poll() is None and time.monotonic() < deadline:
                if stage < 6 and (OUT / f'ready-{stage}').exists():
                    capture_env = env | json.loads((OUT / 'session.json').read_text())
                    capture_env['QT_QPA_PLATFORM'] = 'wayland'
                    subprocess.run(
                        ['spectacle', '--background', '--nonotify', '--fullscreen', '--output',
                         str(OUT / f'composite-{stage}.png')], env=capture_env, check=True,
                        timeout=10, stdout=subprocess.DEVNULL, stderr=log)
                    (OUT / f'ack-{stage}').touch()
                    print(f'Captured synthetic stage {stage}', flush=True)
                    stage += 1
                time.sleep(.1)
            if process.poll() is None:
                raise TimeoutError('Isolated compositor test timed out; see build/desktop-ui-checks/')
            tests = (OUT / 'tests.log').read_text()
            assert process.returncode == 0 and '0 failed' in tests and 'FAIL!' not in tests and stage == 6, tests
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)

samples = []
for stage in range(4):
    with Image.open(OUT / f'surface-{stage}.png') as surface:
        assert 0 < surface.getpixel((300, 220))[3] < 250, 'The live material became opaque'
    with Image.open(OUT / f'composite-{stage}.png').convert('RGB') as composite:
        samples.append(composite.getpixel((500, 400)))
        patch = [composite.getpixel((x, y)) for y in range(370, 430) for x in range(440, 560)]
        assert max(statistics.pstdev(p[channel] for p in patch) for channel in range(3)) < 4, \
            'The stripe pattern behind the window was not blurred'
for first in (0, 2):
    assert samples[first][0] > samples[first + 1][0] + 15, 'Red background did not update'
    assert samples[first + 1][2] > samples[first][2] + 15, 'Blue background did not update'
report = {'snapshot_outputs': 2, 'snapshot_routing': 'passed', 'real_ocr_geometry': 'passed',
          'keep_open_across_outputs_without_repeating_shift': 'passed',
          'live_blur_dark_and_light': 'passed', 'composited_rgb': samples}
(OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
