#!/usr/bin/env python3
"""Verify packaged Qt UI exposure over AT-SPI on a private display and buses.

Requires Python PyGObject (Gio), Xvfb, at-spi2-core, Flatpak, bubblewrap, and
the Flatpak runtime installed system-wide. Installs only into a disposable
FLATPAK_USER_DIR; a private home mount protects the user's Flatpak profile.
This checks the outgoing screen-reader bridge, separately from selected-text
capture. No desktop text, real configuration, or AI services are used.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time

try:
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib
except ImportError as error:
    raise SystemExit("Install python-gobject/python3-gi and use the system Python.") from error

ROOT = Path(__file__).resolve().parent.parent
APPID = "io.github.ranslator.Ranslator"
ACCESSIBLE = "org.a11y.atspi.Accessible"
TEXT = "org.a11y.atspi.Text"
EDITABLE = "org.a11y.atspi.EditableText"
REGISTRY = "org.a11y.atspi.Registry"
ROOT_PATH = "/org/a11y/atspi/accessible/root"


def executable(name):
    found = shutil.which(name)
    for directory in ("/usr/libexec", "/usr/lib/at-spi2-core", "/usr/lib",
                      "/usr/local/libexec", "/usr/local/lib/at-spi2-core"):
        candidate = Path(directory) / name
        if not found and candidate.is_file() and os.access(candidate, os.X_OK):
            found = str(candidate)
    assert found, f"Install {name} (at-spi2-core for accessibility services)."
    return found


def stop(process):
    # AppImage and Flatpak use intermediary launchers; own the entire group.
    for signum in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, signum)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass


def connection(address):
    return Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)


def call(bus, destination, path, interface, method, signature=None, values=()):
    arguments = GLib.Variant(signature, values) if signature else None
    return bus.call_sync(destination, path, interface, method, arguments, None,
                         Gio.DBusCallFlags.NONE, 1500, None).unpack()


def wait_for(callback, message, timeout=15):
    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        try:
            result = callback()
            if result:
                return result
        except GLib.Error as error:
            last_error = str(error)
        time.sleep(.1)
    raise AssertionError(f"{message}{': ' + last_error if last_error else ''}")


def tree(bus):
    result, visited = [], set()
    pending = list(call(bus, REGISTRY, ROOT_PATH, ACCESSIBLE, "GetChildren")[0])
    while pending:
        owner, path = pending.pop()
        if (owner, path) in visited or path.endswith("/null"):
            continue
        visited.add((owner, path))
        assert len(visited) < 1500, "Unexpectedly large private accessibility tree"
        name = call(bus, owner, path, "org.freedesktop.DBus.Properties", "Get",
                    "(ss)", (ACCESSIBLE, "Name"))[0]
        interfaces = call(bus, owner, path, ACCESSIBLE, "GetInterfaces")[0]
        children = call(bus, owner, path, ACCESSIBLE, "GetChildren")[0]
        role = call(bus, owner, path, ACCESSIBLE, "GetRoleName")[0]
        result.append({"owner": owner, "path": path, "name": name,
                       "role": role, "interfaces": interfaces, "children": children})
        pending.extend((child[0] or owner, child[1]) for child in reversed(children))
    return result


def inspect_window(bus, mode, process):
    expected = "Ranslator Settings" if mode == "settings" else "Text to translate"
    observed = []

    def available():
        assert process.poll() is None, f"{mode}: packaged UI exited before accessibility inspection"
        nodes = tree(bus)
        observed[:] = [node["name"] for node in nodes]
        return nodes if any(node["name"] == expected for node in nodes) else None

    try:
        nodes = wait_for(available, f"{mode}: Qt UI never appeared on the private AT-SPI bus")
    except AssertionError as error:
        raise AssertionError(f"{error}; observed names: {observed[:40]}") from error
    names = {node["name"] for node in nodes}
    if mode == "settings":
        for name in ("Languages", "Translate into", "When text is already in that language",
                     "Close settings"):
            assert name in names, f"Settings accessibility label missing: {name}"
        expected_values = {"English", "Español"}
    else:
        for name in ("Swap languages", "Text to translate", "Translated text"):
            assert name in names, f"Translator accessibility control missing: {name}; names: {sorted(names)}"
        expected_values = {"Detect language", "Auto · EN / ES"}
    # Qt's Unix QAccessibleComboBox intentionally exposes its current value as
    # Name, with labels represented by relations rather than accessibleName.
    controls = [node for node in nodes if node["name"] in expected_values and node["role"] == "combo box"]
    assert {node["name"] for node in controls} == expected_values, "Language combo values are not exposed"
    assert len(controls) == 2, "Expected two accessible language controls"
    by_reference = {(node["owner"], node["path"]): node for node in nodes}
    editable_controls = []
    for control in controls:
        candidates = [control] + [by_reference.get((owner or control["owner"], path), {})
                                  for owner, path in control["children"]]
        editor = next((node for node in candidates if EDITABLE in node.get("interfaces", ())), None)
        assert editor and TEXT in editor["interfaces"], "Language control is not accessible as editable text"
        before = call(bus, editor["owner"], editor["path"], TEXT, "GetText", "(ii)", (0, -1))[0]
        assert before, "Accessible language value is empty"
        changed = call(bus, editor["owner"], editor["path"], EDITABLE, "SetTextContents", "(s)", ("fr",))[0]
        assert changed, "AT-SPI could not edit the language control"
        actual = call(bus, editor["owner"], editor["path"], TEXT, "GetText", "(ii)", (0, -1))[0]
        assert actual == "fr", f"AT-SPI language edit was not applied: {actual!r}"
        editable_controls.append({"name": control["name"], "initial_text": before,
                                  "editable_text_roundtrip": actual})
    # Settings changes remain unsaved; main-window edits only change the
    # disposable profile. No translation action is invoked.
    return {"window": mode, "accessible_nodes": len(nodes),
            "labels": sorted(name for name in names if name in {
                "Languages", "Translate into", "When text is already in that language",
                "Text to translate", "Translated text", "Ranslator Settings"}),
            "editable_language_controls": editable_controls}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appimage", type=Path, default=ROOT / "dist/portable/Ranslator-x86_64.AppImage")
    parser.add_argument("--flatpak", type=Path, default=ROOT / "dist/portable/Ranslator.flatpak")
    parser.add_argument("--native-control", type=Path, help="Optional native Qt executable to validate the test fixture first")
    parser.add_argument("--report", type=Path, default=ROOT / "build/bundled-accessibility-report.json")
    args = parser.parse_args()
    artifacts = {"appimage": args.appimage.resolve(), "flatpak": args.flatpak.resolve()}
    if args.native_control:
        artifacts["native-control"] = args.native_control.resolve()
    for path in artifacts.values():
        assert path.is_file(), f"Missing release artifact: {path}"
    for name in ("Xvfb", "bwrap", "flatpak", "dbus-daemon", "at-spi-bus-launcher", "at-spi2-registryd"):
        executable(name)
    report = {"passed": False, "artifacts": {kind: hashlib.sha256(path.read_bytes()).hexdigest()
                                             for kind, path in artifacts.items()}, "checks": [],
              "scope": "Outgoing Qt screen-reader bridge on private Xvfb, session bus and AT-SPI bus; actual AppImage and installed Flatpak",
              "isolation": "Private TMPDIR and unique USER prevent desktop singleton IPC; temporary config/home and buses contain synthetic data only",
              "limitations": "This verifies accessible names, tree and editable text; it does not assess speech output, navigation ergonomics or a real Wayland compositor."}
    processes = []
    try:
        with tempfile.TemporaryDirectory(prefix="ranslator-bundle-a11y-") as temporary:
            temporary = Path(temporary)
            env = os.environ.copy()
            for name in ("WAYLAND_DISPLAY", "AT_SPI_BUS_ADDRESS", "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH",
                         "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_ACCESSIBILITY",
                         "QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "NO_AT_BRIDGE", "RANSLATOR_API_KEY"):
                env.pop(name, None)
            for name in ("home", "config", "cache", "data", "runtime", "profile", "profile/tmp"):
                (temporary / name).mkdir(mode=0o700)
            env.update(HOME=str(temporary / "home"), XDG_CONFIG_HOME=str(temporary / "config"),
                       XDG_CACHE_HOME=str(temporary / "cache"), XDG_DATA_HOME=str(temporary / "data"),
                       XDG_RUNTIME_DIR=str(temporary / "runtime"), XDG_SESSION_TYPE="x11",
                       XDG_CURRENT_DESKTOP="", QT_QPA_PLATFORM="xcb", QT_STYLE_OVERRIDE="Fusion",
                       QT_QPA_PLATFORMTHEME="", GSETTINGS_BACKEND="memory",
                       TMPDIR=str(temporary / "profile/tmp"), USER=temporary.name,
                       FLATPAK_USER_DIR=str(temporary / "installation"),
                       RANSLATOR_CONFIG=str(temporary / "profile/config.json"))
            fixture = json.loads((ROOT / "config.example.json").read_text())
            fixture.update(provider="cli", cli_program="/bin/false", cli_args=[], api_key="", api_key_env="",
                           preferred_language="en", fallback_language="es", popup_mode=True, reduce_motion=True)
            Path(env["RANSLATOR_CONFIG"]).write_text(json.dumps(fixture))

            def launch(command, label, environment=env, **kwargs):
                log = temporary / f"{label}.log"
                with log.open("w") as errors:
                    process = subprocess.Popen(command, env=environment, cwd="/", stderr=errors,
                                               stdout=kwargs.pop("stdout", errors), start_new_session=True, **kwargs)
                processes.append(process)
                return process

            read_fd, write_fd = os.pipe()
            display = launch(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1200x900x24",
                              "-nolisten", "tcp"], "xvfb", pass_fds=(write_fd,))
            os.close(write_fd)
            with os.fdopen(read_fd) as pipe:
                assert select.select([pipe], [], [], 8)[0], "Private Xvfb failed to start"
                env["DISPLAY"] = ":" + pipe.readline().strip()
            bus_config = temporary / "session-bus.conf"
            shutil.copyfile(ROOT / "tests/fixtures/session-bus.conf", bus_config)
            daemon = launch([executable("dbus-daemon"), "--nofork", "--print-address", f"--config-file={bus_config}"],
                            "session-bus", stdout=subprocess.PIPE, text=True)
            assert select.select([daemon.stdout], [], [], 8)[0], "Private session bus failed to start"
            env["DBUS_SESSION_BUS_ADDRESS"] = daemon.stdout.readline().strip()
            session_bus = connection(env["DBUS_SESSION_BUS_ADDRESS"])
            launch([executable("at-spi-bus-launcher"), "--launch-immediately"], "atspi")
            address = wait_for(lambda: call(session_bus, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress")[0],
                               "Private accessibility bus failed to start")
            status = call(session_bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
                          "GetAll", "(s)", ("org.a11y.Status",))[0]
            for property_name in ("IsEnabled", "ScreenReaderEnabled"):
                if property_name in status:
                    call(session_bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
                         "Set", "(ssv)", ("org.a11y.Status", property_name, GLib.Variant("b", True)))
            assert call(session_bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties",
                        "Get", "(ss)", ("org.a11y.Status", "IsEnabled"))[0], "Private accessibility status did not enable"
            a11y_bus = connection(address)
            # Launch explicitly: distros using dbus-broker otherwise attempt
            # systemd activation, which is unavailable on this isolated bus.
            launch([executable("at-spi2-registryd")], "registry", env | {"AT_SPI_BUS_ADDRESS": address})
            wait_for(lambda: call(a11y_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "NameHasOwner", "(s)", (REGISTRY,))[0],
                     "Private accessibility registry did not register")
            wait_for(lambda: call(a11y_bus, REGISTRY, ROOT_PATH, ACCESSIBLE, "GetChildren"),
                     "Private accessibility registry failed to start")
            assert not call(a11y_bus, REGISTRY, ROOT_PATH, ACCESSIBLE, "GetChildren")[0], "Private bus contains an unexpected application"
            env["AT_SPI_BUS_ADDRESS"] = address
            # Preserve HOME's real value only for the Flatpak launcher while a
            # private mount hides that directory. The app uses a disposable home.
            flatpak_env = env | {"HOME": str(Path.home())}
            flatpak_prefix = ["bwrap", "--bind", "/", "/", "--dev-bind", "/dev", "/dev",
                              "--bind", str(temporary / "home"), str(Path.home()),
                              "--chdir", "/", "--die-with-parent", "--", "flatpak"]
            package = temporary / "Ranslator.flatpak"
            shutil.copyfile(artifacts["flatpak"], package)

            def flatpak(*arguments):
                result = subprocess.run(flatpak_prefix + list(arguments), env=flatpak_env, cwd="/",
                                        capture_output=True, text=True, timeout=120)
                assert result.returncode == 0, f"Flatpak command failed: {result.stderr}"
                return result.stdout

            flatpak("install", "--user", "--bundle", "--no-deps", "--no-related", "--no-pull", "--noninteractive", str(package))
            metadata = configparser.ConfigParser(interpolation=None)
            metadata.read_string(flatpak("info", "--user", "--show-metadata", APPID))
            report["flatpak_runtime"] = metadata["Application"]["runtime"]
            report["flatpak_extra_permissions"] = ["Disposable profile folder only; original socket and D-Bus policies preserved"]
            options = ["run", "--user", f"--filesystem={temporary / 'profile'}",
                       f"--env=RANSLATOR_CONFIG={env['RANSLATOR_CONFIG']}", "--env=QT_QPA_PLATFORM=xcb",
                       "--env=QT_STYLE_OVERRIDE=Fusion", "--env=QT_QPA_PLATFORMTHEME=", "--unset-env=WAYLAND_DISPLAY"]
            proxy_address = flatpak(*options, "--command=printenv", APPID, "AT_SPI_BUS_ADDRESS").strip()
            assert proxy_address.startswith("unix:") and proxy_address != address, "Flatpak did not provide its accessibility proxy"
            report["flatpak_accessibility_transport"] = "Sandbox-provided AT-SPI proxy; host inspector reads application objects through it"
            kinds = (["native-control"] if args.native_control else []) + ["appimage", "flatpak"]
            for kind in kinds:
                for mode in ("window", "settings"):
                    Path(env["RANSLATOR_CONFIG"]).write_text(json.dumps(fixture))
                    label = f"{kind}-{mode}"
                    phase_tmp = temporary / "profile" / f"tmp-{label}"
                    phase_tmp.mkdir(mode=0o700)
                    phase_user = f"{temporary.name}-{label}"
                    # Each launch gets a fresh singleton namespace: Flatpak
                    # reuses namespace PIDs and SIGKILL cannot remove lock files.
                    phase_env = (flatpak_env if kind == "flatpak" else env) | {
                        "TMPDIR": str(phase_tmp), "USER": phase_user}
                    if kind == "native-control":
                        command = [str(artifacts[kind]), f"--{mode}"]
                    elif kind == "appimage":
                        command = [str(artifacts[kind]), "--appimage-extract-and-run", f"--{mode}"]
                    else:
                        instance_file = temporary / f"{label}.instance"
                        # Open a non-reserved fd inside the outer mount namespace.
                        # The shell program is fixed; paths/arguments are argv
                        # values and are never interpolated into shell source.
                        command = flatpak_prefix[:-1] + ["/bin/sh", "-c",
                            'exec 3>"$1"; shift; exec flatpak "$@"', "flatpak-instance", str(instance_file),
                            *options, f"--env=TMPDIR={phase_tmp}", f"--env=USER={phase_user}",
                            "--instance-id-fd=3", APPID, f"--{mode}"]
                    process = launch(command, label, phase_env)
                    instance = None
                    try:
                        if kind == "flatpak":
                            instance = wait_for(lambda: instance_file.read_text().strip() if instance_file.exists() else None,
                                                "Flatpak did not report its private instance", timeout=10)
                            assert instance.isdecimal(), f"Invalid Flatpak instance identifier: {instance!r}"
                        check = inspect_window(a11y_bus, mode, process)
                        report["checks"].append({"bundle": kind, "passed": True, **check})
                        print(f"PASS: {kind} {mode}: accessible labels and editable language controls", flush=True)
                    except Exception as error:
                        details = (temporary / f"{label}.log").read_text()
                        report["checks"].append({"bundle": kind, "window": mode, "passed": False,
                                                 "error": str(error), "process_log": details[-6000:]})
                        raise AssertionError(f"{label}: {error}\n{details}") from error
                    finally:
                        # flatpak run detaches the sandbox into its own session.
                        # Stop exactly the instance launched above, never an app
                        # selected by its shared application ID.
                        if instance and instance.isdecimal():
                            flatpak("kill", instance)
                        stop(process)
                    wait_for(lambda: not call(a11y_bus, REGISTRY, ROOT_PATH, ACCESSIBLE, "GetChildren")[0],
                             "Previous fixture did not leave the private accessibility registry")
            report["passed"] = True
    finally:
        for process in reversed(processes):
            stop(process)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(args.report)


if __name__ == "__main__":
    main()
