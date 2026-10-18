//! Run user-installed commands in their own environment, including from Flatpak.
//! Prompts and arguments remain argv/stdin data; the fixed shell supervisor never
//! evaluates them as shell source.
use crate::process::{self, Cancellation};
use anyhow::{Context, Result, bail};
use std::{
    ffi::OsString,
    path::{Path, PathBuf},
    process::Command,
    time::Duration,
};

pub fn sandboxed() -> bool {
    cfg!(target_os = "linux") && Path::new("/.flatpak-info").exists()
}

// A desktop session's PATH often omits npm, Bun and other per-user installs.
// Never source shell startup files: those may be interactive or execute commands.
const SEARCH_PATH: &str = r#"
for directory in "$HOME/.local/bin" "$HOME/.npm-global/bin" "$HOME/.opencode/bin" \
    "$HOME/.bun/bin" "$HOME/.cargo/bin" "$HOME/.volta/bin" "$HOME/.asdf/shims" \
    "$HOME/.local/share/mise/shims" "$HOME/.nix-profile/bin" \
    "$HOME/.local/state/nix/profile/bin" /opt/homebrew/bin /usr/local/bin \
    /opt/local/bin /run/current-system/sw/bin /nix/var/nix/profiles/default/bin \
    "$HOME"/.nvm/versions/node/*/bin "$HOME"/.fnm/node-versions/*/installation/bin \
    "$HOME"/.local/share/fnm/node-versions/*/installation/bin; do
    if [ -d "$directory" ]; then PATH="${PATH:+$PATH:}$directory"; fi
done
export PATH
"#;

const EXECUTABLE_PATH: &str = r#"
case "$1" in
    /*) PATH="${1%/*}:$PATH"; export PATH ;;
esac
"#;

// flatpak-spawn --watch-bus sends SIGINT, which CLIs and their children can
// ignore. This host process owns a fresh process group; its traps and normal
// completion kill that entire group with SIGKILL. The real command's exit code
// is recorded first, in a private directory visible in both namespaces.
const SUPERVISOR: &str = r#"
status_file=$1
working_directory=$2
shift 2
trap 'trap "" HUP INT TERM; kill -KILL -$$' HUP INT TERM
finish() {
    printf '%s\n' "$1" > "$status_file"
    kill -KILL -$$
}
if [ -n "$working_directory" ]; then
    cd "$working_directory" || finish 125
else
    cd "$HOME" || finish 125
fi
case "$1" in
    /*) PATH="${1%/*}:$PATH"; export PATH ;;
esac
exec 3<&0
"$@" <&3 3<&- &
runtime_pid=$!
exec 3<&-
wait "$runtime_pid"
finish "$?"
"#;

/// Flatpak's private cache has the same absolute path on the host. Its private
/// /tmp does not, so a normal tempfile directory cannot be a host CLI's cwd.
pub(crate) fn workspace() -> Result<tempfile::TempDir> {
    if !sandboxed() {
        return tempfile::tempdir().context("Could not create a runtime workspace");
    }
    let cache = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .context("Flatpak did not provide its shared cache directory")?;
    if !cache.is_absolute() {
        bail!("Flatpak cache directory must be an absolute path");
    }
    let parent = cache.join("ranslator/runtime");
    std::fs::create_dir_all(&parent).context("Could not create a host runtime workspace")?;
    tempfile::Builder::new()
        .prefix("job-")
        .tempdir_in(parent)
        .context("Could not create a private host runtime workspace")
}

fn appimage_directory() -> Option<PathBuf> {
    std::env::var_os("APPDIR").map(PathBuf::from).or_else(|| {
        std::env::current_exe()
            .ok()
            .filter(|path| {
                std::env::var_os("APPIMAGE").is_some() && path.ends_with("usr/bin/ranslator")
            })
            .and_then(|path| path.parent()?.parent()?.parent().map(Path::to_owned))
    })
}

fn native_path() -> OsString {
    let bundle = appimage_directory();
    let paths = std::env::var_os("PATH").unwrap_or_default();
    let paths = std::env::split_paths(&paths).filter(|path| {
        !bundle
            .as_ref()
            .is_some_and(|bundle| path.starts_with(bundle))
    });
    std::env::join_paths(paths).unwrap_or_default()
}

fn prepare_native(command: &mut Command) {
    // Bundled Qt libraries/plugins must not replace a host CLI's libraries.
    // Preserve non-bundle entries in a user's LD_LIBRARY_PATH.
    let bundle = appimage_directory();
    if let Some(bundle) = bundle {
        if let Some(libraries) = std::env::var_os("LD_LIBRARY_PATH") {
            let filtered =
                std::env::split_paths(&libraries).filter(|path| !path.starts_with(&bundle));
            if let Ok(filtered) = std::env::join_paths(filtered) {
                if filtered.is_empty() {
                    command.env_remove("LD_LIBRARY_PATH");
                } else {
                    command.env("LD_LIBRARY_PATH", filtered);
                }
            }
        }
        for name in ["QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"] {
            command.env_remove(name);
        }
    }
}

pub fn run(
    command: &mut Command,
    input: Vec<u8>,
    timeout: Duration,
    cancel: &Cancellation,
) -> Result<Vec<u8>> {
    if !sandboxed() {
        prepare_native(command);
        // Apply the same search-path extension as the Flatpak bridge. Argument
        // vectors are retained verbatim, even for custom executable paths.
        let mut wrapper = Command::new("/bin/sh");
        wrapper
            .arg("-c")
            .arg(format!("{SEARCH_PATH}\n{EXECUTABLE_PATH}\nexec \"$@\""))
            .arg("ranslator-runtime")
            .arg(command.get_program())
            .args(command.get_args())
            .env("PATH", native_path());
        for (name, value) in command.get_envs() {
            if let Some(value) = value {
                wrapper.env(name, value);
            } else {
                wrapper.env_remove(name);
            }
        }
        if let Some(directory) = command.get_current_dir() {
            wrapper.current_dir(directory);
        }
        return process::run(&mut wrapper, input, timeout, cancel);
    }
    run_flatpak(command, input, timeout, cancel)
}

#[cfg(unix)]
fn run_flatpak(
    command: &Command,
    input: Vec<u8>,
    timeout: Duration,
    cancel: &Cancellation,
) -> Result<Vec<u8>> {
    use std::os::unix::process::ExitStatusExt;
    let workspace = workspace()?;
    let status_file = workspace.path().join("exit-status");
    let mut proxy = Command::new("/usr/bin/flatpak-spawn");
    // This helper belongs to the Flatpak runtime, not our Qt bundle. Loading
    // /app/lib's GLib into it can mix incompatible GLib/GObject versions.
    // The host command still receives its original host environment below.
    proxy.env_remove("LD_LIBRARY_PATH");
    proxy.args(["--host", "--watch-bus", "--directory=/"]);
    // Only explicitly set values cross the sandbox boundary. In particular,
    // sandbox HOME, XDG_*, PATH and library paths must not replace host values.
    for (name, value) in command.get_envs() {
        let mut argument = OsString::from(if value.is_some() {
            "--env="
        } else {
            "--unset-env="
        });
        argument.push(name);
        if let Some(value) = value {
            argument.push("=");
            argument.push(value);
        }
        proxy.arg(argument);
    }
    proxy
        .args(["--", "/bin/sh", "-c"])
        .arg(format!("{SEARCH_PATH}\n{SUPERVISOR}"))
        .arg("ranslator-host-runtime")
        .arg(&status_file)
        .arg(command.get_current_dir().unwrap_or(Path::new("")))
        .arg(command.get_program())
        .args(command.get_args());
    let mut output = process::run_capture(&mut proxy, input, timeout, cancel)?;
    let status = std::fs::read_to_string(&status_file)
        .ok()
        .and_then(|value| value.trim().parse::<u8>().ok());
    if let Some(status) = status {
        output.status = std::process::ExitStatus::from_raw(i32::from(status) << 8);
    } else if output.status.success() {
        bail!("The host runtime supervisor did not report an exit status.");
    }
    process::check_output(output)
}

#[cfg(not(unix))]
fn run_flatpak(
    _command: &Command,
    _input: Vec<u8>,
    _timeout: Duration,
    _cancel: &Cancellation,
) -> Result<Vec<u8>> {
    bail!("Flatpak host commands are only available on Linux")
}

pub fn find_programs(names: &[&str]) -> Result<Vec<Option<PathBuf>>> {
    if names.is_empty() {
        return Ok(vec![]);
    }
    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let mut command = Command::new("/bin/sh");
    command
        .args(["-c", "for name do path=$(command -v \"$name\" 2>/dev/null) || path=; printf '%s\\0' \"$path\"; done", "ranslator-discovery"])
        .args(names);
    let output = run(&mut command, vec![], Duration::from_secs(4), &cancel)?;
    let output = output
        .strip_suffix(&[0])
        .context("The runtime lookup returned an invalid result")?;
    let entries: Vec<_> = output.split(|byte| *byte == 0).collect();
    if entries.len() != names.len() {
        bail!("The runtime lookup returned an incomplete result");
    }
    entries
        .into_iter()
        .map(|entry| {
            if entry.is_empty() {
                Ok(None)
            } else {
                let path =
                    String::from_utf8(entry.to_vec()).context("Runtime path must be UTF-8")?;
                let path = PathBuf::from(path);
                Ok(path.is_absolute().then_some(path))
            }
        })
        .collect()
}
