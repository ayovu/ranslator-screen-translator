#!/usr/bin/env python3
"""Check Flatpak's accessibility adapter using only an isolated synthetic bus."""
from pathlib import Path
import argparse
import os
import select
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stage", type=Path)
parser.add_argument("--test-binary", type=Path, default=root / "build/accessibility-tests/accessibility_tests")
args = parser.parse_args()
stage = args.stage or max(root.glob("build/flatpak-stage.*"), key=lambda path: path.stat().st_mtime)
binary = args.test_binary.resolve()
probe_binary = binary.parent / "accessibility_probe"
runtime = Path(os.environ["XDG_RUNTIME_DIR"]) / "at-spi"
runtime.mkdir(mode=0o700, exist_ok=True)
processes = []

def launch(command, env):
    process = subprocess.Popen(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    processes.append(process)
    return process

def line(process):
    if not select.select([process.stdout], [], [], 8)[0]:
        raise RuntimeError("Fixture did not become ready")
    value = process.stdout.readline().strip()
    if not value and process.poll() is not None:
        raise RuntimeError(process.stderr.read())
    return value

try:
    with tempfile.TemporaryDirectory(prefix="ranslator-test-", dir=runtime) as directory:
        address = f"unix:path={directory}/bus"
        env = os.environ | {"DBUS_SESSION_BUS_ADDRESS": address}
        daemon = launch(["dbus-daemon", f"--config-file={root}/tests/fixtures/session-bus.conf",
                         f"--address={address}", "--nofork", "--print-address"], env)
        assert line(daemon).startswith(address)
        fixture = launch([str(binary), "--fake"], env)
        assert line(fixture) == "READY"
        command = ["flatpak", "build", "--with-appdir", "--runtime", "--talk-name=org.a11y.Bus",
                   f"--filesystem={binary.parent}:ro", "--env=LD_LIBRARY_PATH=/app/lib",
                   "--env=QT_PLUGIN_PATH=/app/plugins",
                   # A normal Flatpak sets this to its restricted export proxy.
                   # The selection adapter must explicitly query the host address.
                   "--env=AT_SPI_BUS_ADDRESS=unix:path=/unreachable-export-proxy"]
        probe = launch(command + ["--filesystem=xdg-run/at-spi", str(stage), str(probe_binary)], env)
        assert line(probe) == "READY"
        subprocess.run(["gdbus", "call", "--address", address, "--dest", "org.a11y.atspi.Registry",
                        "--object-path", "/org/a11y/atspi/test", "--method", "org.ranslator.Test.Emit"],
                       check=True, stdout=subprocess.DEVNULL)
        output, errors = probe.communicate(timeout=8)
        assert probe.returncode == 0, errors
        assert "synthetic host selection" in output, output
        denied = subprocess.run(command + ["--nofilesystem=xdg-run/at-spi", str(stage), str(probe_binary)],
                                env=env, capture_output=True, text=True, timeout=15)
        assert denied.returncode == 2, denied.stderr
        assert "accessibility" in denied.stderr and "Allow" in denied.stderr, denied.stderr
        print("PASS: Flatpak reads the synthetic host selection; revoked accessibility access reports an actionable error.")
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
