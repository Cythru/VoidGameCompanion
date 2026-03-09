use anyhow::{Context, Result};
use tokio::sync::Mutex;
use std::sync::Arc;

/// TTS engine using edge-tts subprocess + ffplay for audio playback.
/// One-at-a-time playback enforced via tokio Mutex.
#[derive(Clone)]
pub struct TtsEngine {
    enabled:       bool,
    playback_lock: Arc<Mutex<()>>,
}

impl TtsEngine {
    pub fn new(enabled: bool) -> Self {
        Self {
            enabled,
            playback_lock: Arc::new(Mutex::new(())),
        }
    }

    /// Speak text using the given edge-tts voice.
    /// Blocks until playback finishes (or returns immediately if TTS disabled).
    pub async fn speak(&self, text: &str, voice: &str, rate: &str, pitch: &str) -> Result<()> {
        if !self.enabled || text.trim().is_empty() {
            return Ok(());
        }

        // Acquire lock — only one utterance at a time
        let _guard = self.playback_lock.lock().await;

        // Create temp file for the audio
        let tmp_dir = std::env::temp_dir();
        let audio_path = tmp_dir.join(format!("gc_tts_{}.mp3", std::process::id()));
        let audio_str = audio_path.to_string_lossy().to_string();

        // Run edge-tts to generate audio file
        let edge_tts = tokio::process::Command::new("edge-tts")
            .arg("--voice")
            .arg(voice)
            .arg("--rate")
            .arg(rate)
            .arg("--pitch")
            .arg(pitch)
            .arg("--text")
            .arg(text)
            .arg("--write-media")
            .arg(&audio_str)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .await
            .context("failed to run edge-tts — is it installed? (pip install edge-tts)")?;

        if !edge_tts.success() {
            anyhow::bail!("edge-tts exited with status {}", edge_tts);
        }

        // Play audio via ffplay (no video, quiet, auto-exit)
        let ffplay = tokio::process::Command::new("ffplay")
            .arg("-nodisp")
            .arg("-autoexit")
            .arg("-loglevel")
            .arg("quiet")
            .arg(&audio_str)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .await
            .context("failed to run ffplay — is ffmpeg installed?")?;

        if !ffplay.success() {
            anyhow::bail!("ffplay exited with status {}", ffplay);
        }

        // Cleanup temp file (best effort)
        let _ = tokio::fs::remove_file(&audio_path).await;

        Ok(())
    }

    /// Convenience: speak with default rate/pitch.
    pub async fn speak_default(&self, text: &str, voice: &str) -> Result<()> {
        self.speak(text, voice, "+0%", "+0Hz").await
    }

    /// Check if edge-tts is available on the system.
    pub async fn check_available() -> bool {
        tokio::process::Command::new("edge-tts")
            .arg("--list-voices")
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .await
            .map(|s| s.success())
            .unwrap_or(false)
    }
}
