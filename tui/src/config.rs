use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub downloader: PathBuf,
    pub download_dir: PathBuf,
    pub threads: u16,
    pub platform: String,
    pub language: String,
    pub include: String,
    pub exclude: String,
    pub archive: ArchiveConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ArchiveConfig {
    pub enabled: bool,
    pub output_dir: PathBuf,
    pub move_to_vault: bool,
    pub vault_dir: PathBuf,
    pub delete_source: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            downloader: PathBuf::from("lgogdownloader"),
            download_dir: dirs::download_dir()
                .unwrap_or_else(|| PathBuf::from("."))
                .join("gog-downloads"),
            threads: 6,
            platform: "w+l".into(),
            language: "en".into(),
            include: String::new(),
            exclude: String::new(),
            archive: ArchiveConfig::default(),
        }
    }
}

impl Default for ArchiveConfig {
    fn default() -> Self {
        let base = dirs::download_dir().unwrap_or_else(|| PathBuf::from("."));
        Self {
            enabled: false,
            output_dir: base.join("gog-archives"),
            move_to_vault: false,
            vault_dir: base.join("gamevault"),
            delete_source: false,
        }
    }
}

impl Config {
    pub fn path() -> PathBuf {
        dirs::config_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("lgogdownloader-tui")
            .join("config.toml")
    }

    pub fn load() -> Result<Self> {
        let path = Self::path();
        if !path.exists() {
            return Ok(Self::default());
        }
        let contents = fs::read_to_string(&path)
            .with_context(|| format!("failed to read {}", path.display()))?;
        toml::from_str(&contents).with_context(|| format!("failed to parse {}", path.display()))
    }

    pub fn save(&self) -> Result<()> {
        let path = Self::path();
        ensure_parent(&path)?;
        let contents = toml::to_string_pretty(self)?;
        fs::write(&path, contents).with_context(|| format!("failed to write {}", path.display()))
    }
}

fn ensure_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    Ok(())
}
