use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

use crate::persona::PersonaId;

const CONFIG_DIR: &str = ".local/share/blackmagic-gen/gaming";
const CONFIG_FILE: &str = "coordinator.json";

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// Minecraft server address (host:port).
    #[serde(default = "default_mc_addr")]
    pub minecraft_server: String,

    /// Factorio RCON address (host:port).
    #[serde(default = "default_factorio_rcon")]
    pub factorio_rcon: String,

    /// Factorio RCON password.
    #[serde(default)]
    pub factorio_rcon_password: String,

    /// Active persona ID.
    #[serde(default = "default_persona")]
    pub persona: PersonaId,

    /// Minecraft bot username (offline mode).
    #[serde(default = "default_username")]
    pub username: String,

    /// Whether TTS is enabled.
    #[serde(default = "default_tts_enabled")]
    pub tts_enabled: bool,

    /// LLM base URL. MUST be 127.0.0.1, not localhost.
    #[serde(default = "default_llm_url")]
    pub llm_base_url: String,

    /// LLM model name.
    #[serde(default = "default_llm_model")]
    pub llm_model: String,

    /// Whether to auto-detect the active game via window title.
    #[serde(default = "default_auto_detect")]
    pub auto_detect_game: bool,

    /// Polling interval for game events in milliseconds.
    #[serde(default = "default_poll_interval")]
    pub poll_interval_ms: u64,
}

fn default_mc_addr()         -> String    { "127.0.0.1:25565".into() }
fn default_factorio_rcon()   -> String    { "127.0.0.1:25575".into() }
fn default_persona()         -> PersonaId { PersonaId::Chihiro }
fn default_username()        -> String    { "VoidBot".into() }
fn default_tts_enabled()     -> bool      { true }
fn default_llm_url()         -> String    { "http://127.0.0.1:9000".into() }
fn default_llm_model()       -> String    { "voidminillama".into() }
fn default_auto_detect()     -> bool      { true }
fn default_poll_interval()   -> u64       { 250 }

impl Default for Config {
    fn default() -> Self {
        Self {
            minecraft_server:      default_mc_addr(),
            factorio_rcon:         default_factorio_rcon(),
            factorio_rcon_password: String::new(),
            persona:               default_persona(),
            username:              default_username(),
            tts_enabled:           default_tts_enabled(),
            llm_base_url:          default_llm_url(),
            llm_model:             default_llm_model(),
            auto_detect_game:      default_auto_detect(),
            poll_interval_ms:      default_poll_interval(),
        }
    }
}

impl Config {
    /// Path to the config file.
    pub fn path() -> Result<PathBuf> {
        let home = std::env::var("HOME").context("HOME not set")?;
        Ok(PathBuf::from(home).join(CONFIG_DIR).join(CONFIG_FILE))
    }

    /// Load config from disk, or return defaults if file doesn't exist.
    pub fn load() -> Result<Self> {
        let path = Self::path()?;
        if !path.exists() {
            return Ok(Self::default());
        }
        let data = std::fs::read_to_string(&path)
            .with_context(|| format!("reading {}", path.display()))?;
        let config: Self = serde_json::from_str(&data)
            .with_context(|| format!("parsing {}", path.display()))?;
        Ok(config)
    }

    /// Save config to disk.
    pub fn save(&self) -> Result<()> {
        let path = Self::path()?;
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let data = serde_json::to_string_pretty(self)?;
        std::fs::write(&path, data)?;
        Ok(())
    }
}
