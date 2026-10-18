use anyhow::{Context, Result, bail};
use std::{
    io::{Read, Write},
    process::{Command, Output, Stdio},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

pub const OUTPUT_LIMIT: u64 = 4 * 1024 * 1024;
pub type Cancellation = Arc<AtomicBool>;
pub fn run(
    command: &mut Command,
    input: Vec<u8>,
    timeout: Duration,
    cancel: &Cancellation,
) -> Result<Vec<u8>> {
    check_output(run_capture(command, input, timeout, cancel)?)
}

/// Capture output separately from interpreting the exit status. The host adapter
/// obtains the real command's status from its supervisor, not flatpak-spawn.
pub(crate) fn run_capture(
    command: &mut Command,
    input: Vec<u8>,
    timeout: Duration,
    cancel: &Cancellation,
) -> Result<Output> {
    if cancel.load(Ordering::Relaxed) || timeout.is_zero() {
        bail!("Operation cancelled or timed out.");
    }
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context("Could not start executable; check its installation and path")?;
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let writer = thread::spawn(move || stdin.write_all(&input));
    let output_exceeded = Arc::new(AtomicBool::new(false));
    let reader_exceeded = output_exceeded.clone();
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.take(OUTPUT_LIMIT + 1).read_to_end(&mut bytes)?;
        reader_exceeded.store(bytes.len() as u64 > OUTPUT_LIMIT, Ordering::Relaxed);
        Ok::<_, std::io::Error>(bytes)
    });
    let errors = thread::spawn(move || {
        let mut bytes = Vec::new();
        // Drain diagnostics even after the display limit. Closing a full stderr
        // pipe can kill an otherwise healthy runtime with SIGPIPE.
        let mut stderr = stderr;
        let mut buffer = [0; 8192];
        loop {
            let size = stderr.read(&mut buffer)?;
            if size == 0 {
                break;
            }
            let keep = size.min(16_384_usize.saturating_sub(bytes.len()));
            bytes.extend_from_slice(&buffer[..keep]);
        }
        Ok::<_, std::io::Error>(bytes)
    });
    let start = Instant::now();
    let mut interrupted = false;
    let status = loop {
        if let Some(status) = child.try_wait()? {
            break status;
        }
        if cancel.load(Ordering::Relaxed)
            || start.elapsed() >= timeout
            || output_exceeded.load(Ordering::Relaxed)
        {
            interrupted = true;
            #[cfg(unix)]
            unsafe {
                libc::kill(-(child.id() as i32), libc::SIGKILL);
            }
            let _ = child.kill();
            break child.wait()?;
        }
        thread::sleep(Duration::from_millis(20));
    };
    // Reap descendants that inherited pipes; never leave a local model running after cancellation.
    #[cfg(unix)]
    unsafe {
        libc::kill(-(child.id() as i32), libc::SIGKILL);
    }
    let _ = writer.join();
    let bytes = reader
        .join()
        .map_err(|_| anyhow::anyhow!("Output reader stopped"))??;
    let diagnostic = errors.join().ok().and_then(Result::ok).unwrap_or_default();
    if bytes.len() as u64 > OUTPUT_LIMIT {
        bail!("Process output exceeded the 4 MiB limit.");
    }
    if interrupted {
        bail!("Operation cancelled or timed out.");
    }
    Ok(Output {
        status,
        stdout: bytes,
        stderr: diagnostic,
    })
}

pub(crate) fn check_output(output: Output) -> Result<Vec<u8>> {
    if !output.status.success() {
        let message = format!(
            "{} {}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        )
        .to_lowercase();
        if message.contains("authenticate")
            || message.contains("oauth")
            || message.contains("not logged in")
            || message.contains("authentication required")
        {
            bail!(
                "The CLI runtime needs login. Open it in a terminal, sign in again, then retry translation."
            );
        }
        if message.contains("org.freedesktop.flatpak")
            && (message.contains("denied")
                || message.contains("not provided")
                || message.contains("not allowed")
                || message.contains("allowed to talk"))
        {
            bail!(
                "Host commands are unavailable. Allow Ranslator to access org.freedesktop.Flatpak in your Flatpak permissions, then retry."
            );
        }
        bail!(
            "Process exited with {}. Check the executable, model, and OCR language data.",
            output.status
        );
    }
    Ok(output.stdout)
}
