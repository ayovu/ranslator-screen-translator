use crate::{
    config::Config,
    process::{self, Cancellation},
};
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::{
    path::{Path, PathBuf},
    process::Command,
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
pub fn recognize(config: &Config, image: &Path, cancel: &Cancellation) -> Result<Vec<Word>> {
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
    let words = parse_tsv(&text);
    if words.is_empty() {
        bail!("No readable text found. Try a closer screenshot or change the OCR language.");
    }
    Ok(words)
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
