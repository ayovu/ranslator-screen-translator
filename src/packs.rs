use crate::{config::Config, ocr, process::Cancellation};
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Write},
    path::PathBuf,
    sync::{OnceLock, atomic::Ordering},
    time::Duration,
};

#[derive(Clone, Deserialize, Serialize)]
pub struct Pack {
    pub code: String,
    pub name: String,
    pub bytes: u64,
    pub sha256: String,
}
#[derive(Deserialize)]
pub struct Catalog {
    pub revision: String,
    pub packs: Vec<Pack>,
}
pub fn catalog() -> &'static Catalog {
    static CATALOG: OnceLock<Catalog> = OnceLock::new();
    CATALOG.get_or_init(|| {
        serde_json::from_str(include_str!("../assets/ocr-packs.json")).expect("Bundled OCR catalog")
    })
}
pub fn directory() -> PathBuf {
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
            if cfg!(target_os = "macos") {
                home.join("Library/Application Support")
            } else {
                home.join(".local/share")
            }
        });
    base.join("ranslator/tessdata")
}
pub fn list(config: &Config) -> serde_json::Value {
    serde_json::Value::Array(
        catalog()
            .packs
            .iter()
            .map(|pack| {
                let mut item = serde_json::to_value(pack).expect("Pack serialization");
                item["installed"] =
                    serde_json::json!(ocr::model_path(config, &pack.code).is_some());
                item
            })
            .collect(),
    )
}
pub fn verify(pack: &Pack, bytes: &[u8]) -> Result<()> {
    if bytes.len() as u64 != pack.bytes || format!("{:x}", Sha256::digest(bytes)) != pack.sha256 {
        bail!("OCR download failed its checksum. Nothing was installed; try again.");
    }
    Ok(())
}
pub fn install(codes: &[String], cancel: &Cancellation, progress: impl Fn(String)) -> Result<()> {
    if codes.is_empty() || codes.len() > catalog().packs.len() {
        bail!("Choose one or more languages from the catalog.");
    }
    let packs: Vec<_> = codes
        .iter()
        .map(|code| {
            catalog()
                .packs
                .iter()
                .find(|p| &p.code == code)
                .with_context(|| format!("Unknown OCR language: {code}"))
        })
        .collect::<Result<_>>()?;
    let directory = directory();
    fs::create_dir_all(&directory)?;
    let http: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(120)))
        .timeout_recv_body(Some(Duration::from_secs(5)))
        .max_redirects(0)
        .build()
        .into();
    for pack in packs {
        if cancel.load(Ordering::Relaxed) {
            bail!("Download cancelled.");
        }
        if ocr::model_path(&Config::default(), &pack.code).is_some() {
            continue;
        }
        progress(format!(
            "Downloading {} · {:.1} MB…",
            pack.name,
            pack.bytes as f64 / 1_000_000.0
        ));
        let url = format!(
            "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{}/{}.traineddata",
            catalog().revision,
            pack.code
        );
        let mut response = http.get(url).call().context(
            "Could not download the OCR language pack. Check your connection and try again.",
        )?;
        let mut reader = response.body_mut().as_reader().take(pack.bytes + 1);
        let mut bytes = Vec::with_capacity(pack.bytes as usize);
        let mut buffer = [0; 65536];
        loop {
            if cancel.load(Ordering::Relaxed) {
                bail!("Download cancelled.");
            }
            let count = reader
                .read(&mut buffer)
                .context("OCR download interrupted; try again.")?;
            if count == 0 {
                break;
            }
            bytes.extend_from_slice(&buffer[..count]);
        }
        verify(pack, &bytes)?;
        if cancel.load(Ordering::Relaxed) {
            bail!("Download cancelled.");
        }
        let mut file = tempfile::NamedTempFile::new_in(&directory)?;
        file.write_all(&bytes)?;
        file.as_file().sync_all()?;
        file.persist(directory.join(format!("{}.traineddata", pack.code)))
            .map_err(|e| e.error)?;
    }
    fs::write(
        directory.join("LICENSE"),
        include_str!("../assets/tessdata/LICENSE"),
    )?;
    Ok(())
}
