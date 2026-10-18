//! Run this same binary natively and inside the packaged Flatpak runtime.
#![cfg(unix)]
use ranslator::{config::Config, host, provider, runtimes};
use std::{
    os::unix::fs::PermissionsExt,
    path::Path,
    process::Command,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

fn cancel() -> Arc<AtomicBool> {
    Arc::new(AtomicBool::new(false))
}

fn workspace() -> tempfile::TempDir {
    if host::sandboxed() {
        tempfile::tempdir_in(std::env::var_os("XDG_CACHE_HOME").unwrap()).unwrap()
    } else {
        tempfile::tempdir().unwrap()
    }
}

fn script(path: &Path, body: &str) {
    std::fs::write(path, format!("#!/bin/sh\n{body}\n")).unwrap();
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).unwrap();
}

#[test]
fn discovery_checks_the_execution_environment_without_starting_runtimes() {
    let programs =
        host::find_programs(&["sh", "cat", "ranslator-definitely-not-installed"]).unwrap();
    assert!(programs[0].as_ref().unwrap().is_absolute());
    assert!(programs[1].as_ref().unwrap().is_absolute());
    assert!(programs[2].is_none());
    let result = runtimes::discover();
    assert_eq!(result["host_access"], true, "{result}");
    assert_eq!(result["sandboxed"], host::sandboxed());
    assert_eq!(result["runtimes"].as_array().unwrap().len(), 4);
}

#[test]
fn custom_command_preserves_unicode_stdin_arguments_and_environment() {
    let directory = workspace();
    let executable = directory.path().join("runtime with spaces");
    script(
        &executable,
        "printf '%s\\n' \"$1\" \"$RANSLATOR_SOURCE\" \"$RANSLATOR_TARGET\"; cat",
    );
    let injection = format!(
        "$(touch '{}') `echo hello`; $HOME",
        directory.path().join("unexpected").display()
    );
    let config = Config {
        provider: "cli".into(),
        cli_program: executable.to_str().unwrap().into(),
        cli_args: vec![injection.clone()],
        ..Config::default()
    };
    let text = "Доброго дня! こんにちは\nNo shell evaluation: $HOME $(id) `id`";
    let result = provider::translate(&config, text, Some("en"), Some("fr"), &cancel()).unwrap();
    assert_eq!(result.text, format!("{injection}\nen\nfr\n{text}"));
    assert!(!directory.path().join("unexpected").exists());
}

#[test]
fn absolute_runtime_can_resolve_tools_installed_beside_it() {
    let directory = workspace();
    let executable = directory.path().join("runtime in private install");
    let helper = directory.path().join("ranslator-fixture-companion");
    script(&helper, "printf 'companion tool found'");
    script(&executable, "exec ranslator-fixture-companion");
    let result = host::run(
        &mut Command::new(executable),
        vec![],
        Duration::from_secs(3),
        &cancel(),
    )
    .unwrap();
    assert_eq!(result, b"companion tool found");
}

#[test]
fn runtime_models_and_translation_use_the_same_host_adapter() {
    let directory = workspace();
    let executable = directory.path().join("opencode fixture");
    script(
        &executable,
        r#"
if [ "$1" = models ]; then
    printf 'opencode/fixture-free\nprovider/paid-model\n'
else
    [ "$1" = run ] || exit 31
    [ "$OPENCODE_CONFIG_CONTENT" = '{"permission":{"*":"deny"},"share":"disabled"}' ] || exit 32
    case "$PWD" in *ranslator*|*/.tmp*) ;; *) exit 33 ;; esac
    cat >/dev/null
    printf '%s\n' '{"type":"text","part":{"text":"Bonjour"}}'
fi
"#,
    );
    let config = Config {
        provider: "cli".into(),
        cli_program: executable.to_str().unwrap().into(),
        cli_runtime: "opencode".into(),
        cli_free_only: true,
        cli_args: runtimes::default_args("opencode"),
        model: "opencode/fixture-free".into(),
        ..Config::default()
    };
    assert_eq!(
        runtimes::models(&config).unwrap(),
        ["opencode/fixture-free"]
    );
    assert_eq!(
        provider::translate(&config, "hello", Some("en"), Some("fr"), &cancel())
            .unwrap()
            .text,
        "Bonjour"
    );
}

#[test]
fn host_home_and_config_are_not_replaced_with_sandbox_values() {
    let mut command = Command::new("/bin/sh");
    command.args([
        "-c",
        "printf '%s\\n' \"$HOME\" \"$XDG_CONFIG_HOME\"; test ! -e /.flatpak-info",
    ]);
    let result = host::run(&mut command, vec![], Duration::from_secs(3), &cancel()).unwrap();
    let result = String::from_utf8(result).unwrap();
    let mut lines = result.lines();
    assert_eq!(lines.next().unwrap(), std::env::var("HOME").unwrap());
    if host::sandboxed() {
        assert_ne!(
            lines.next().unwrap_or_default(),
            std::env::var("XDG_CONFIG_HOME").unwrap()
        );
    }
}

#[test]
fn timeout_stops_children_that_ignore_interrupt_and_termination() {
    let directory = workspace();
    let marker = directory.path().join("should-not-be-created");
    let executable = directory.path().join("ignores-interrupt");
    script(
        &executable,
        "trap '' HUP INT TERM; (sleep 0.6; printf leaked > \"$1\") & wait",
    );
    let now = Instant::now();
    let result = host::run(
        Command::new(&executable).arg(&marker),
        vec![],
        Duration::from_millis(150),
        &cancel(),
    );
    assert!(result.unwrap_err().to_string().contains("timed out"));
    assert!(now.elapsed() < Duration::from_secs(2));
    std::thread::sleep(Duration::from_millis(650));
    assert!(!marker.exists(), "A descendant survived cancellation");
}

#[test]
fn cancellation_stops_a_host_command_before_its_timeout() {
    let cancel = cancel();
    let trigger = cancel.clone();
    let worker = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(150));
        trigger.store(true, Ordering::Relaxed);
    });
    let now = Instant::now();
    assert!(
        host::run(
            Command::new("/bin/sh").args(["-c", "trap '' INT TERM; sleep 30 & wait"]),
            vec![],
            Duration::from_secs(10),
            &cancel
        )
        .is_err()
    );
    assert!(now.elapsed() < Duration::from_secs(2));
    worker.join().unwrap();
}

#[test]
fn successful_exit_reaps_descendants_that_kept_output_pipes_open() {
    let now = Instant::now();
    let result = host::run(
        Command::new("/bin/sh").args(["-c", "sleep 30 & printf done"]),
        vec![],
        Duration::from_secs(3),
        &cancel(),
    )
    .unwrap();
    assert_eq!(result, b"done");
    assert!(now.elapsed() < Duration::from_secs(2));
}

#[test]
fn runtime_exit_code_and_authentication_error_survive_the_bridge() {
    let error = host::run(
        Command::new("/bin/sh").args(["-c", "exit 42"]),
        vec![],
        Duration::from_secs(3),
        &cancel(),
    )
    .unwrap_err();
    assert!(error.to_string().contains("42"), "{error}");
    let error = host::run(
        Command::new("/bin/sh").args([
            "-c",
            "printf 'Authentication required: private token' >&2; exit 7",
        ]),
        vec![],
        Duration::from_secs(3),
        &cancel(),
    )
    .unwrap_err();
    assert!(error.to_string().contains("needs login"), "{error}");
    assert!(!error.to_string().contains("private token"));
}

#[test]
fn large_diagnostics_do_not_kill_a_successful_runtime() {
    let result = host::run(
        Command::new("/bin/sh").args(["-c", "head -c 65536 /dev/zero >&2; printf done"]),
        vec![],
        Duration::from_secs(3),
        &cancel(),
    )
    .unwrap();
    assert_eq!(result, b"done");
}

#[test]
fn output_limit_stops_a_runtime_that_ignores_a_closed_pipe() {
    let now = Instant::now();
    let error = host::run(
        Command::new("/bin/sh").args(["-c", "trap '' PIPE; head -c 5000000 /dev/zero; sleep 30"]),
        vec![],
        Duration::from_secs(10),
        &cancel(),
    )
    .unwrap_err();
    assert!(error.to_string().contains("4 MiB"), "{error}");
    assert!(now.elapsed() < Duration::from_secs(2));
}

#[test]
fn already_cancelled_request_does_not_start_a_runtime() {
    let directory = workspace();
    let marker = directory.path().join("must-not-start");
    let cancel = Arc::new(AtomicBool::new(true));
    assert!(
        host::run(
            Command::new("/usr/bin/touch").arg(&marker),
            vec![],
            Duration::from_secs(3),
            &cancel
        )
        .is_err()
    );
    assert!(!marker.exists());
}

#[test]
#[ignore = "Run separately inside Flatpak with host-command permission removed"]
fn denied_host_permission_has_actionable_error() {
    assert!(host::sandboxed());
    let result = runtimes::discover();
    assert_eq!(result["host_access"], false, "{result}");
    let error = result["host_error"].as_str().unwrap();
    assert!(error.contains("Flatpak permissions"), "{error}");
}
