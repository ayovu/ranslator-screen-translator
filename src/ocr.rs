use crate::{
    config::Config,
    process::{self, Cancellation},
};
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::{
    path::{Path, PathBuf},
    process::Command,
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Word {
    pub text: String,
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub line: String,
}
pub fn parse_tsv(tsv: &str) -> Vec<Word> {
    tsv.lines()
        .skip(1)
        .filter_map(|row| {
            let fields: Vec<_> = row.splitn(12, '\t').collect();
            if fields.len() != 12 || fields[0] != "5" || fields[11].trim().is_empty() {
                return None;
            }
            let confidence: f32 = fields[10].parse().ok()?;
            if confidence < 20.0 {
                return None;
            }
            let word = Word {
                text: fields[11].trim().into(),
                x: fields[6].parse().ok()?,
                y: fields[7].parse().ok()?,
                width: fields[8].parse().ok()?,
                height: fields[9].parse().ok()?,
                line: fields[1..5].join(":"),
            };
            (word.width > 0 && word.height > 0).then_some(word)
        })
        .collect()
}
pub fn data_dir(config: &Config) -> PathBuf {
    if !config.tessdata_dir.is_empty() {
        return config.tessdata_dir.clone().into();
    }
    bundled_data_dir()
}
fn bundled_data_dir() -> PathBuf {
    if let Some(path) = std::env::var_os("RANSLATOR_TESSDATA") {
        return path.into();
    }
    let exe = std::env::current_exe().unwrap_or_default();
    let prefix = exe
        .parent()
        .and_then(Path::parent)
        .unwrap_or(Path::new("/usr"));
    for candidate in [
        prefix.join("Resources/tessdata"),
        prefix.join("share/ranslator/tessdata"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("assets/tessdata"),
        PathBuf::from("/usr/share/tessdata"),
    ] {
        if candidate.join("eng.traineddata").exists() {
            return candidate;
        }
    }
    prefix.join("share/ranslator/tessdata")
}

pub fn executable() -> PathBuf {
    let bundled = std::env::current_exe().ok().and_then(|exe| {
        exe.parent()?
            .parent()
            .map(|contents| contents.join("Helpers/tesseract"))
    });
    bundled
        .filter(|path| path.is_file())
        .unwrap_or_else(|| PathBuf::from("tesseract"))
}
/// Internal option: `ranslator --vision-ocr IMAGE LANGUAGES` prints Apple Vision results as TSV.
pub const VISION_OPTION: &str = "--vision-ocr";
/// With [`VISION_OPTION`], prepares Vision's recognition models without an image.
pub const WARM_UP_OPTION: &str = "--warm-up";

/// The engine Snapshot uses. Apple Vision exists only in macOS builds with the native layer;
/// other platforms keep Tesseract whatever a copied configuration requests.
pub fn engine(config: &Config) -> &'static str {
    if cfg!(all(target_os = "macos", not(core_only))) && config.ocr_engine != "tesseract" {
        "vision"
    } else {
        "tesseract"
    }
}

pub fn recognize(config: &Config, image: &Path, cancel: &Cancellation) -> Result<Vec<Word>> {
    let words = if engine(config) == "vision" {
        recognize_vision(config, image, cancel).map_err(|error| {
            if cancel.load(Ordering::Relaxed) {
                error
            } else {
                error.context(
                    "Apple Vision could not read this image. To use the bundled engine instead, \
                     choose Tesseract in Settings → Screen text.",
                )
            }
        })?
    } else {
        recognize_tesseract(config, image, cancel)?
    };
    if words.is_empty() {
        bail!("No readable text found. Try a closer screenshot or change the OCR language.");
    }
    Ok(words)
}

// A child process keeps the shared timeout and cancellation: Vision itself cannot be interrupted.
fn recognize_vision(config: &Config, image: &Path, cancel: &Cancellation) -> Result<Vec<Word>> {
    let mut command = Command::new(std::env::current_exe()?);
    command
        .arg(VISION_OPTION)
        .arg(image)
        .arg(&config.ocr_languages);
    let output = process::run(
        &mut command,
        vec![],
        Duration::from_secs(config.timeout_seconds),
        cancel,
    )?;
    Ok(parse_tsv(
        &String::from_utf8(output).context("OCR returned invalid UTF-8")?,
    ))
}

/// Vision compiles its accurate recognizer on first use for each app and macOS build, which
/// takes about a minute. Doing that in the background keeps the first Snapshot responsive.
/// Later warm-ups find the cache and finish in a fraction of a second.
pub fn warm_up(config: &Config) {
    static RUNNING: AtomicBool = AtomicBool::new(false);
    if engine(config) != "vision" || RUNNING.swap(true, Ordering::AcqRel) {
        return;
    }
    let languages = config.ocr_languages.clone();
    std::thread::spawn(move || {
        if let Ok(exe) = std::env::current_exe() {
            let mut command = Command::new(exe);
            command.args([VISION_OPTION, WARM_UP_OPTION]).arg(languages);
            let _ = process::run(
                &mut command,
                vec![],
                Duration::from_secs(300),
                &Default::default(),
            );
        }
        RUNNING.store(false, Ordering::Release);
    });
}

fn recognize_tesseract(config: &Config, image: &Path, cancel: &Cancellation) -> Result<Vec<Word>> {
    // A temporary view lets bundled, downloaded and system models work together.
    let data = tempfile::tempdir()?;
    for language in config.ocr_languages.split('+') {
        let source = model_path(config, language).with_context(|| {
            format!(
                "OCR model '{language}' is missing. Open Settings → Screen text → Language packs."
            )
        })?;
        let target = data.path().join(format!("{language}.traineddata"));
        #[cfg(unix)]
        std::os::unix::fs::symlink(std::fs::canonicalize(source)?, target)?;
        #[cfg(not(unix))]
        std::fs::copy(source, target)?;
    }
    let mut command = Command::new(executable());
    command
        .arg(image)
        .arg("stdout")
        .arg("--tessdata-dir")
        .arg(data.path())
        .arg("-l")
        .arg(&config.ocr_languages)
        .args(["--psm", "11", "-c", "tessedit_create_tsv=1"])
        .env("OMP_THREAD_LIMIT", "2");
    let output = process::run(
        &mut command,
        vec![],
        Duration::from_secs(config.timeout_seconds),
        cancel,
    )?;
    let text = String::from_utf8(output).context("OCR returned invalid UTF-8")?;
    Ok(parse_tsv(&text))
}

pub fn model_path(config: &Config, language: &str) -> Option<PathBuf> {
    if language.is_empty()
        || !language
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_')
    {
        return None;
    }
    let name = format!("{language}.traineddata");
    let mut directories = vec![];
    if !config.tessdata_dir.is_empty() {
        directories.push(PathBuf::from(&config.tessdata_dir));
    }
    directories.extend([
        crate::packs::directory(),
        bundled_data_dir(),
        PathBuf::from("/usr/share/tessdata"),
    ]);
    directories
        .into_iter()
        .map(|dir| dir.join(&name))
        .find(|path| path.is_file())
}
