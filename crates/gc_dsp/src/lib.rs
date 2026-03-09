use anyhow::{Context, Result};
use gc_core::game_trait::*;

// ── Dyson Sphere Program mod bridge ─────────────────────────────────────────
//
// DSP doesn't have a native server protocol. Integration requires a BepInEx mod
// that exposes a local HTTP or WebSocket API. This is a placeholder for that
// bridge.
//
// Planned architecture:
//   1. BepInEx mod (C#) running inside DSP process
//   2. Mod opens a local WebSocket server on 127.0.0.1:9876
//   3. This connector connects to it and exchanges JSON messages
//   4. Events: resource alerts, research complete, Dyson shell progress, etc.
//   5. Actions: queue research, set logistics routes, pause/resume

/// DSP game connector (stub).
pub struct DspConnection {
    addr:            String,
    pending_events:  Vec<GameEvent>,
}

impl DspConnection {
    pub fn new(addr: impl Into<String>) -> Self {
        Self {
            addr: addr.into(),
            pending_events: Vec::new(),
        }
    }
}

#[async_trait::async_trait]
impl GameConnector for DspConnection {
    async fn connect(&mut self) -> Result<()> {
        // TODO: Connect to BepInEx mod WebSocket
        anyhow::bail!(
            "DSP connector not yet implemented. \
             Requires BepInEx mod with WebSocket API at {}",
            self.addr
        )
    }

    async fn poll_events(&mut self) -> Result<Vec<GameEvent>> {
        Ok(std::mem::take(&mut self.pending_events))
    }

    async fn send_action(&mut self, action: Action) -> Result<()> {
        eprintln!("DSP action not implemented: {:?}", action);
        Ok(())
    }

    fn game_type(&self) -> GameType {
        GameType::DysonSphereProgram
    }
}
