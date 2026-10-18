#!/usr/bin/env python3
"""Exercise identical CLI adapters natively and in a real Flatpak runtime."""
from pathlib import Path
import argparse
import json
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stage", type=Path, help="Existing Flatpak build directory")
parser.add_argument("--test-binary", type=Path, help="Prebuilt host_runtime integration test")
args = parser.parse_args()
if args.test_binary:
    binary = args.test_binary.resolve()
else:
    result = subprocess.run(
        ["cargo", "test", "--locked", "--test", "host_runtime", "--no-run", "--message-format=json"],
        cwd=root, check=True, text=True, stdout=subprocess.PIPE,
    )
    artifacts = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    binary = Path(next(item["executable"] for item in artifacts
                       if item.get("executable") and item.get("target", {}).get("name") == "host_runtime"))
stage = args.stage or max(root.glob("build/flatpak-stage.*"), key=lambda path: path.stat().st_mtime)
subprocess.run([str(binary), "--nocapture"], check=True)
flatpak = ["flatpak", "build", "--with-appdir", "--runtime", f"--filesystem={binary.parent}:ro"]
subprocess.run(flatpak + ["--talk-name=org.freedesktop.Flatpak", str(stage), str(binary), "--nocapture"], check=True)
subprocess.run(flatpak + ["--no-talk-name=org.freedesktop.Flatpak", str(stage), str(binary),
                         "--ignored", "--exact", "denied_host_permission_has_actionable_error", "--nocapture"], check=True)
print("PASS: native and Flatpak host runtimes, process cleanup, argument safety, and permission denial.")
