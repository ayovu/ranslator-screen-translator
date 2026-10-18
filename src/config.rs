use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs, io::Write, path::PathBuf};

/// Remember engine-specific settings without changing the active fields used by providers.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct ProviderProfile {
    pub endpoint: String,
    pub model: String,
    pub api_key: String,
    pub api_key_env: String,
    pub cli_program: String,
    pub cli_args: Vec<String>,
    pub cli_free_only: bool,
    pub timeout_seconds: u64,
}
impl Default for ProviderProfile {
    fn default() -> Self {
        Self {
            endpoint: String::new(),
            model: String::new(),
            api_key: String::new(),
            api_key_env: "RANSLATOR_API_KEY".into(),
            cli_program: String::new(),
            cli_args: vec![],
            cli_free_only: true,
            timeout_seconds: 90,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct Binding {
    pub sequence: String,
    pub taps: u8,
}
impl Default for Binding {
    fn default() -> Self {
        Self {
            sequence: "Ctrl+Alt+R".into(),
            taps: 1,
        }
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub provider_profiles: BTreeMap<String, ProviderProfile>,
    pub preferred_language: String,
    pub fallback_language: String,
    pub provider: String,
    pub endpoint: String,
    pub model: String,
    pub api_key: String,
    pub api_key_env: String,
    pub cli_program: String,
    pub cli_args: Vec<String>,
    pub cli_runtime: String,
    pub cli_free_only: bool,
    pub popup_mode: bool,
    pub snapshot_copy_text: bool,
    pub timeout_seconds: u64,
    pub once: Binding,
    pub continuous: Binding,
    pub snapshot: Binding,
    pub tap_window_ms: u64,
    pub selection_delay_ms: u64,
    pub watch_clipboard: bool,
    pub ocr_languages: String,
    pub tessdata_dir: String,
    pub capture_mode: String,
    pub appearance: String,
    pub reduce_transparency: bool,
    pub reduce_motion: bool,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            provider_profiles: BTreeMap::new(),
            preferred_language: "en".into(),
            fallback_language: "es".into(),
            provider: "auto".into(),
            endpoint: String::new(),
            model: String::new(),
            api_key: String::new(),
            api_key_env: "RANSLATOR_API_KEY".into(),
            cli_program: String::new(),
            cli_args: vec![],
            cli_runtime: "custom".into(),
            cli_free_only: true,
            popup_mode: true,
            snapshot_copy_text: false,
            timeout_seconds: 90,
            once: Binding::default(),
            continuous: Binding {
                sequence: "Ctrl+Alt+Y".into(),
                taps: 1,
            },
            snapshot: Binding {
                sequence: "Ctrl+Alt+O".into(),
                taps: 1,
            },
            tap_window_ms: 450,
            selection_delay_ms: 500,
            watch_clipboard: false,
            ocr_languages: "eng".into(),
            tessdata_dir: String::new(),
            capture_mode: "auto".into(),
            appearance: "system".into(),
            reduce_transparency: false,
            reduce_motion: false,
        }
    }
}
impl Config {
    pub fn path() -> PathBuf {
        if let Some(path) = std::env::var_os("RANSLATOR_CONFIG") {
            return path.into();
        }
        let base = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
                if cfg!(target_os = "macos") {
                    home.join("Library/Application Support")
                } else {
                    home.join(".config")
                }
            });
        base.join("ranslator/config.json")
    }
    pub fn load() -> Result<Self> {
        let path = Self::path();
        if !path.exists() {
            return Ok(Self::default());
        }
        let config: Self = serde_json::from_slice(&fs::read(&path)?)
            .context("Invalid settings JSON; original file was preserved")?;
        config.validate()?;
        Ok(config)
    }
    pub fn validate(&self) -> Result<()> {
        if !["custom", "codex", "claude", "opencode", "antigravity"]
            .contains(&self.cli_runtime.as_str())
        {
            bail!("Choose a supported CLI runtime or Custom command.");
        }
        if !["system", "light", "dark"].contains(&self.appearance.as_str()) {
            bail!("Choose System, Light, or Dark appearance.");
        }
        if !["auto", "desktop", "region", "picker"].contains(&self.capture_mode.as_str()) {
            bail!("Choose a supported screenshot mode.");
        }
        for lang in [&self.preferred_language, &self.fallback_language] {
            if lang.trim().is_empty() || lang.len() > 64 {
                bail!("Enter a language name or ISO language code (up to 64 bytes).");
            }
        }
        if crate::language::normalize(&self.preferred_language)
            == crate::language::normalize(&self.fallback_language)
        {
            bail!("Preferred and fallback languages must be different.");
        }
        if !(5..=300).contains(&self.timeout_seconds) {
            bail!("Timeout must be between 5 and 300 seconds.");
        }
        if !(150..=2000).contains(&self.tap_window_ms)
            || !(100..=3000).contains(&self.selection_delay_ms)
        {
            bail!("Tap interval must be 150–2000 ms; selection delay must be 100–3000 ms.");
        }
        let mut seen = std::collections::HashSet::new();
        for binding in [&self.once, &self.continuous, &self.snapshot] {
            if !(1..=3).contains(&binding.taps) || binding.sequence.trim().is_empty() {
                bail!("Each shortcut needs a key sequence and 1–3 presses.");
            }
            if !seen.insert(
                crate::state::canonical_modifier(&binding.sequence)
                    .unwrap_or_else(|| binding.sequence.to_lowercase().replace(' ', "")),
            ) {
                bail!(
                    "Use a different key sequence for each action; repeated-tap prefixes are ambiguous."
                );
            }
        }
        if ![
            "auto",
            "ollama",
            "compatible",
            "libretranslate",
            "mymemory",
            "cli",
        ]
        .contains(&self.provider.as_str())
        {
            bail!("Unknown translation provider.");
        }
        if self.provider == "cli" && self.cli_program.trim().is_empty() {
            bail!("Choose the CLI executable.");
        }
        if ["compatible", "libretranslate"].contains(&self.provider.as_str())
            && self.endpoint.trim().is_empty()
        {
            bail!("Enter the provider endpoint URL.");
        }
        if !self.endpoint.is_empty() {
            crate::provider::validate_endpoint(&self.endpoint)?;
        }
        if self.provider == "compatible" && self.model.trim().is_empty() {
            bail!("Enter the model identifier.");
        }
        let languages: Vec<_> = self.ocr_languages.split('+').collect();
        if languages.len() > 8
            || languages
                .iter()
                .collect::<std::collections::HashSet<_>>()
                .len()
                != languages.len()
            || languages.iter().any(|language| {
                language.is_empty()
                    || !language
                        .chars()
                        .all(|c| c.is_ascii_alphanumeric() || c == '_')
            })
        {
            bail!("Choose one to eight OCR languages, such as eng or eng+deu.");
        }
        Ok(())
    }
    pub fn save(&self) -> Result<()> {
        self.validate()?;
        let path = Self::path();
        let parent = path.parent().context("Settings path has no parent")?;
        fs::create_dir_all(parent)?;
        let mut file = tempfile::NamedTempFile::new_in(parent)?;
        // NamedTempFile uses 0600 on Unix; the atomic replacement preserves these permissions.
        file.write_all(&serde_json::to_vec_pretty(self)?)?;
        file.as_file().sync_all()?;
        file.persist(path).map_err(|e| e.error)?;
        Ok(())
    }
    pub fn key(&self) -> String {
        std::env::var(&self.api_key_env)
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| self.api_key.clone())
    }
}
