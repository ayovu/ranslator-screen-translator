#!/usr/bin/env python3
"""Check packaged localhost providers with synthetic HTTP data and a private Xvfb.

No AI services/accounts are contacted. Translation uses each release executable's
CLI; compatible model discovery uses its actual Settings window on a private
display because model listing has no CLI command. Requires Xvfb and xdotool.
"""
import argparse
import contextlib
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import signal
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
TEXT = 'Synthetic selection: "hello" — こんにちは\nSecond line.'
TRANSLATION = "Bonjour — traduction synthétique ✓"
PROXY_VARIABLES = ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")


class Provider(http.server.BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def handle_request(self):
        size = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(size)) if size else None
        with self.lock:
            self.requests.append((self.command, self.path, self.headers.get("Authorization"), body))
        if self.path.startswith("/denied/"):
            status, value = 401, {"error": "fixture-private-server-diagnostic"}
        elif self.path.endswith("/api/tags"):
            status, value = 200, {"models": [{"name": "fixture-local:small"}, {"name": ""},
                                             {"name": "fixture-local:small"}]}
        elif self.path.endswith("/models"):
            status, value = 200, {"data": [{"id": "fixture-api/model"}]}
        elif self.path.endswith("/api/chat"):
            status, value = 200, {"message": {"content": TRANSLATION}}
        elif self.path.endswith("/chat/completions"):
            status, value = 200, {"choices": [{"message": {"content": TRANSLATION}}]}
        else:
            status, value = 404, {"error": "unexpected fixture route"}
        output = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(output)))
        self.end_headers()
        self.wfile.write(output)

    do_GET = handle_request
    do_POST = handle_request


def stop(process):
    # AppImage extraction can keep an intermediate launcher process. Every
    # fixture owns a private process group, so cleanup includes its descendants.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    if process.poll() is None:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


@contextlib.contextmanager
def private_desktop(environment):
    read_fd, write_fd = os.pipe()
    display = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1200x900x24",
                                "-nolisten", "tcp"], pass_fds=(write_fd,), stderr=subprocess.DEVNULL,
                               start_new_session=True)
    os.close(write_fd)
    bus = None
    try:
        with os.fdopen(read_fd) as pipe:
            number = pipe.readline().strip()
        assert number, "Private Xvfb did not start"
        environment["DISPLAY"] = ":" + number
        bus = subprocess.Popen(["/usr/bin/dbus-daemon", "--nofork", "--print-address",
                                "--config-file=" + str(ROOT / "tests/fixtures/session-bus.conf")],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                               start_new_session=True)
        environment["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
        assert environment["DBUS_SESSION_BUS_ADDRESS"].startswith("unix:"), "Private bus did not start"
        yield
    finally:
        if bus:
            stop(bus)
        stop(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appdir", type=Path, default=ROOT / "build/portable/AppDir")
    parser.add_argument("--appimage", type=Path, default=ROOT / "dist/portable/Ranslator-x86_64.AppImage")
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=ROOT / "build/bundled-providers-report.json")
    args = parser.parse_args()
    appdir, appimage, stage = args.appdir.resolve(), args.appimage.resolve(), args.stage.resolve()
    for path in (appdir / "AppRun", appimage, stage / "files/bin/ranslator"):
        assert path.is_file(), f"Missing packaged artifact: {path}"
    report = {"passed": False,
              "scope": "Synthetic host-loopback HTTP; actual packaged CLI and private-Xvfb Settings",
              "artifacts": {"appdir_binary": hashlib.sha256((appdir / "usr/bin/ranslator").read_bytes()).hexdigest(),
                            "appimage": hashlib.sha256(appimage.read_bytes()).hexdigest(),
                            "flatpak_binary": hashlib.sha256((stage / "files/bin/ranslator").read_bytes()).hexdigest()},
              "checks": []}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        with tempfile.TemporaryDirectory(prefix="providers-", dir=ROOT / "build") as temporary:
            tmp = Path(temporary)
            env = os.environ.copy()
            for name in (*PROXY_VARIABLES, "WAYLAND_DISPLAY", "AT_SPI_BUS_ADDRESS", "RANSLATOR_API_KEY",
                         "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
                env.pop(name, None)
            for name in ("config", "data", "cache", "runtime", "tmp"):
                (tmp / name).mkdir(mode=0o700)
            env.update({"RANSLATOR_CONFIG": str(tmp / "config.json"),
                        "XDG_CONFIG_HOME": str(tmp / "config"), "XDG_DATA_HOME": str(tmp / "data"),
                        "XDG_CACHE_HOME": str(tmp / "cache"), "XDG_RUNTIME_DIR": str(tmp / "runtime"),
                        "TMPDIR": str(tmp / "tmp"), "QT_QPA_PLATFORM": "xcb", "QT_STYLE_OVERRIDE": "Fusion",
                        "QT_ACCESSIBILITY": "0", "NO_AT_BRIDGE": "1", "XDG_SESSION_TYPE": "x11",
                        "XDG_CURRENT_DESKTOP": "", "USER": "ranslator-provider-fixture",
                        "RANSLATOR_FIXTURE_KEY": "fixture-environment-key"})
            config = json.loads((ROOT / "config.example.json").read_text())
            config.update(provider_profiles={}, timeout_seconds=5, api_key="", api_key_env="",
                          preferred_language="ru", fallback_language="en", popup_mode=True)

            def write_config(**changes):
                candidate = dict(config, **changes)
                Path(env["RANSLATOR_CONFIG"]).write_text(json.dumps(candidate))

            def command(kind, *arguments, network=True):
                if kind == "appdir":
                    return [str(appdir / "AppRun"), *arguments]
                if kind == "appimage":
                    return [str(appimage), "--appimage-extract-and-run", *arguments]
                flags = ["flatpak", "build", "--with-appdir", "--runtime", f"--filesystem={tmp}:rw"]
                for name in ("RANSLATOR_CONFIG", "RANSLATOR_FIXTURE_KEY", "DISPLAY", "QT_QPA_PLATFORM",
                             "QT_STYLE_OVERRIDE", "QT_ACCESSIBILITY", "NO_AT_BRIDGE", "XDG_CURRENT_DESKTOP"):
                    if name in env:
                        flags.append(f"--env={name}={env[name]}")
                flags += [f"--unset-env={name}" for name in PROXY_VARIABLES]
                flags += ["--unset-env=WAYLAND_DISPLAY"]
                if not network:
                    flags.append("--unshare=network")
                return [*flags, str(stage), "/app/bin/ranslator-launch", *arguments]

            def run_cli(kind, success=True, network=True, detail=""):
                result = subprocess.run(command(kind, "--translate", "fr", network=network), env=env,
                                        input=TEXT, capture_output=True, text=True, timeout=20)
                if success:
                    assert result.returncode == 0, f"{kind} {detail}: {result.stderr}"
                    assert result.stdout.strip() == TRANSLATION, f"{kind}: wrong translation"
                else:
                    assert result.returncode != 0, f"{kind}: failure incorrectly succeeded"
                    assert "Check the endpoint" in result.stderr, f"{kind}: error is not actionable"
                    for secret in ("fixture-inline-key", "fixture-environment-key", "fixture-private-server-diagnostic"):
                        assert secret not in result.stderr, f"{kind}: sensitive diagnostics leaked"
                return result

            def calls():
                with Provider.lock:
                    return list(Provider.requests)

            with private_desktop(env):
                for kind in ("appdir", "appimage", "flatpak"):
                    before = len(calls())
                    write_config(provider="ollama", endpoint=base + "/custom/", model="")
                    run_cli(kind, detail="Ollama discovery and translation")
                    observed = calls()[before:]
                    assert [(r[0], r[1]) for r in observed] == [
                        ("GET", "/custom/api/tags"), ("POST", "/custom/api/chat")], observed
                    assert all(r[2] is None for r in observed), "Unexpected Ollama credentials"
                    assert observed[-1][3]["model"] == "fixture-local:small"
                    report["checks"].append({"bundle": kind, "check": "Ollama custom URL, model discovery, translation", "passed": True})

                    for source in ("inline", "environment"):
                        before = len(calls())
                        write_config(provider="compatible", endpoint=base + "/v1///", model="provider/manual-v2",
                                     api_key="fixture-inline-key",
                                     api_key_env="RANSLATOR_FIXTURE_KEY" if source == "environment" else "")
                        run_cli(kind, detail="Compatible translation")
                        observed = calls()[before:]
                        assert len(observed) == 1 and observed[0][:2] == ("POST", "/v1/chat/completions")
                        assert observed[0][2] == "Bearer fixture-" + ("environment-key" if source == "environment" else "inline-key")
                        body = observed[0][3]
                        assert body["model"] == "provider/manual-v2" and body["stream"] is False
                        assert body["messages"][1] == {"role": "user", "content": TEXT}
                        assert "into fr." in body["messages"][0]["content"]
                        report["checks"].append({"bundle": kind, "check": f"Compatible URL/model/UTF-8/{source} credential/target", "passed": True})

                    # There is no --models CLI. Open only this private Settings
                    # window and choose Translation; no real desktop is visible.
                    before = len(calls())
                    write_config(provider="compatible", endpoint=base + "/catalog/v1/", model="fixture-api/model",
                                 api_key="fixture-inline-key")
                    with (tmp / f"{kind}-settings.log").open("w") as log:
                        app = subprocess.Popen(command(kind, "--settings"), env=env, stdout=log, stderr=log,
                                               start_new_session=True)
                        try:
                            deadline = time.monotonic() + 15
                            window = None
                            while time.monotonic() < deadline and app.poll() is None:
                                found = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Ranslator Settings$"],
                                                       env=env, capture_output=True, text=True)
                                if found.returncode == 0:
                                    window = found.stdout.splitlines()[-1]
                                    break
                                time.sleep(.05)
                            assert window, f"{kind}: private Settings did not open: {(tmp / f'{kind}-settings.log').read_text()}"
                            subprocess.run(["xdotool", "mousemove", "--window", window, "120", "165", "click", "1"], env=env, check=True)
                            deadline = time.monotonic() + 8
                            while len(calls()) == before and time.monotonic() < deadline:
                                time.sleep(.05)
                            observed = calls()[before:]
                            assert len(observed) == 1 and observed[0][:3] == (
                                "GET", "/catalog/v1/models", "Bearer fixture-inline-key"), observed
                            report["checks"].append({"bundle": kind, "check": "Settings compatible model discovery reaches host loopback with saved credentials", "passed": True})
                        finally:
                            stop(app)

                    write_config(provider="compatible", endpoint=base + "/denied/v1", model="fixture",
                                 api_key="fixture-inline-key")
                    run_cli(kind, success=False, detail="401")
                    # Keep the unused port reserved, without listening, to avoid
                    # accidentally reaching a concurrent real local service.
                    with socket.socket() as unavailable:
                        unavailable.bind(("127.0.0.1", 0))
                        write_config(provider="compatible", endpoint=f"http://127.0.0.1:{unavailable.getsockname()[1]}/v1", model="fixture")
                        run_cli(kind, success=False, detail="connection refused")
                    report["checks"].append({"bundle": kind, "check": "Unauthorized/unavailable endpoint produces sanitized actionable error", "passed": True})
                    print(f"PASS: {kind}: Ollama discovery/chat, compatible models/chat, configuration and errors", flush=True)

                before = len(calls())
                write_config(provider="compatible", endpoint=base + "/v1", model="fixture")
                run_cli("flatpak", success=False, network=False)
                assert len(calls()) == before, "Flatpak without network permission reached host loopback"
                report["checks"].append({"bundle": "flatpak", "check": "Revoked network permission prevents host loopback access", "passed": True})
                report["passed"] = True
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: final bundle localhost provider checks; no external AI service contacted.")


if __name__ == "__main__":
    main()
