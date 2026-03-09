use anyhow::{Context, Result};
use std::time::Duration;

use crate::config::Config;
use crate::game_trait::{GameConnector, GameEvent};
use crate::llm_bridge::LlmBridge;
use crate::persona::{self, GamePersona, PersonaId};
use crate::tts::TtsEngine;

// ── Game state ──────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GameState {
    Idle,
    Minecraft,
    Factorio,
    DysonSphereProgram,
}

impl std::fmt::Display for GameState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Idle               => write!(f, "Idle"),
            Self::Minecraft          => write!(f, "Minecraft"),
            Self::Factorio           => write!(f, "Factorio"),
            Self::DysonSphereProgram => write!(f, "Dyson Sphere Program"),
        }
    }
}

// ── Window detection ────────────────────────────────────────────────────────

/// Detect which game is currently focused by reading the active window title.
/// Tries hyprctl first (Hyprland), falls back to xdotool.
pub async fn detect_active_game() -> GameState {
    // Try hyprctl (Hyprland compositor)
    if let Ok(output) = tokio::process::Command::new("hyprctl")
        .args(["activewindow", "-j"])
        .output()
        .await
    {
        if output.status.success() {
            let text = String::from_utf8_lossy(&output.stdout).to_lowercase();
            return classify_window_title(&text);
        }
    }

    // Fallback: xdotool
    if let Ok(output) = tokio::process::Command::new("xdotool")
        .args(["getactivewindow", "getwindowname"])
        .output()
        .await
    {
        if output.status.success() {
            let text = String::from_utf8_lossy(&output.stdout).to_lowercase();
            return classify_window_title(&text);
        }
    }

    GameState::Idle
}

fn classify_window_title(title: &str) -> GameState {
    if title.contains("minecraft") {
        GameState::Minecraft
    } else if title.contains("factorio") {
        GameState::Factorio
    } else if title.contains("dyson sphere") || title.contains("dyson_sphere") || title.contains("dsp") {
        GameState::DysonSphereProgram
    } else {
        GameState::Idle
    }
}

// ── Coordinator ─────────────────────────────────────────────────────────────

/// Master orchestrator. Manages game connectors, routes events through the
/// persona system, generates LLM responses, and triggers TTS.
pub struct Coordinator {
    config:    Config,
    persona:   GamePersona,
    llm:       LlmBridge,
    tts:       TtsEngine,
    state:     GameState,
    connector: Option<Box<dyn GameConnector>>,
}

impl Coordinator {
    pub fn new(config: Config) -> Self {
        let persona = persona::get_persona(config.persona);
        let llm = LlmBridge::new(
            &config.llm_base_url,
            &config.llm_model,
            120,
        );
        let tts = TtsEngine::new(config.tts_enabled);

        Self {
            config,
            persona,
            llm,
            tts,
            state: GameState::Idle,
            connector: None,
        }
    }

    pub fn state(&self) -> GameState {
        self.state
    }

    pub fn persona(&self) -> &GamePersona {
        &self.persona
    }

    /// Switch to a different persona.
    pub fn set_persona(&mut self, id: PersonaId) {
        self.persona = persona::get_persona(id);
        self.config.persona = id;
        eprintln!("Switched to persona: {}", self.persona.name);
    }

    /// Attach a game connector and transition to active state.
    pub async fn start_game(&mut self, mut connector: Box<dyn GameConnector>) -> Result<()> {
        let game_type = connector.game_type();
        eprintln!(
            "{}",
            self.persona.format_msg(&format!("Connecting to {}...", game_type))
        );

        connector.connect().await
            .with_context(|| format!("connecting to {}", game_type))?;

        self.state = match game_type {
            crate::game_trait::GameType::Minecraft          => GameState::Minecraft,
            crate::game_trait::GameType::Factorio           => GameState::Factorio,
            crate::game_trait::GameType::DysonSphereProgram => GameState::DysonSphereProgram,
        };

        self.connector = Some(connector);

        // Announce connection
        if let Some(line) = self.persona.quick_reaction("connected") {
            eprintln!("{}", self.persona.format_msg(line));
            let voice = self.persona.tts_voice.clone();
            let _ = self.tts.speak_default(line, &voice).await;
        }

        Ok(())
    }

    /// Stop the current game connector.
    pub async fn stop_game(&mut self) -> Result<()> {
        if let Some(mut conn) = self.connector.take() {
            conn.send_action(crate::game_trait::Action::Disconnect).await?;
        }
        self.state = GameState::Idle;
        eprintln!("{}", self.persona.format_msg("Disconnected."));
        Ok(())
    }

    /// Main event loop. Polls the connector, routes events, generates responses.
    pub async fn run(&mut self) -> Result<()> {
        let poll_ms = self.config.poll_interval_ms;

        loop {
            if let Some(conn) = self.connector.as_mut() {
                match conn.poll_events().await {
                    Ok(events) => {
                        for event in events {
                            self.handle_event(event).await;
                        }
                    }
                    Err(e) => {
                        eprintln!(
                            "{}",
                            self.persona.format_msg(&format!("Connection error: {e}"))
                        );
                        self.state = GameState::Idle;
                        self.connector = None;
                        break;
                    }
                }
            } else {
                break;
            }

            tokio::time::sleep(Duration::from_millis(poll_ms)).await;
        }

        Ok(())
    }

    /// Handle a single game event: try quick reaction first, fall back to LLM.
    async fn handle_event(&self, event: GameEvent) {
        let tag = event.tag();

        // Try a quick reaction first (no LLM call needed)
        if let Some(line) = self.persona.quick_reaction(tag) {
            eprintln!("{}", self.persona.format_msg(line));
            let voice = self.persona.tts_voice.clone();
            let line = line.to_string();
            let tts = self.tts.clone();
            tokio::spawn(async move {
                let _ = tts.speak_default(&line, &voice).await;
            });
            return;
        }

        // For chat events, always use LLM for a real response
        if let GameEvent::Chat { ref sender, ref message } = event {
            let context = format!("{} said in game chat: {}", sender, message);
            match self.llm.react_to_event(&self.persona, &context).await {
                Ok(response) => {
                    eprintln!("{}", self.persona.format_msg(&response));
                    let voice = self.persona.tts_voice.clone();
                    let tts = self.tts.clone();
                    let tts_text = response.clone();
                    tokio::spawn(async move {
                        let _ = tts.speak_default(&tts_text, &voice).await;
                    });
                    // Note: sending reply into game chat would require the connector
                    // to be behind Arc<Mutex<>> for concurrent access. Left for future.
                    let _ = &response;
                }
                Err(e) => {
                    eprintln!("LLM error: {e}");
                }
            }
        }
    }
}
