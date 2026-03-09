use anyhow::{Context, Result};
use gc_core::auth::Auth;
use gc_core::game_trait::*;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

// ── RCON protocol constants ─────────────────────────────────────────────────

const RCON_AUTH: i32 = 3;
const RCON_AUTH_RESPONSE: i32 = 2;
const RCON_EXEC: i32 = 2;
const RCON_RESPONSE: i32 = 0;

// ── RCON client ─────────────────────────────────────────────────────────────

/// Factorio game connector via RCON protocol.
/// RCON allows sending console commands and reading responses.
pub struct FactorioConnection {
    addr:     String,
    password: String,
    stream:   Option<TcpStream>,
    request_id: i32,
    pending_events: Vec<GameEvent>,
}

impl FactorioConnection {
    pub fn new(addr: impl Into<String>, auth: Auth) -> Self {
        let password = match &auth {
            Auth::FactorioRcon { password } => password.clone(),
            _ => String::new(),
        };
        Self {
            addr: addr.into(),
            password,
            stream: None,
            request_id: 0,
            pending_events: Vec::new(),
        }
    }

    /// Send an RCON packet.
    async fn send_rcon(&mut self, packet_type: i32, payload: &str) -> Result<i32> {
        let stream = self.stream.as_mut().context("not connected")?;
        self.request_id += 1;
        let id = self.request_id;

        let body = payload.as_bytes();
        // RCON packet: length (i32) + request_id (i32) + type (i32) + body + \0\0
        let length = 4 + 4 + body.len() + 2;

        stream.write_all(&(length as i32).to_le_bytes()).await?;
        stream.write_all(&id.to_le_bytes()).await?;
        stream.write_all(&packet_type.to_le_bytes()).await?;
        stream.write_all(body).await?;
        stream.write_all(&[0, 0]).await?; // two null terminators
        stream.flush().await?;

        Ok(id)
    }

    /// Read an RCON response.
    async fn read_rcon(&mut self) -> Result<(i32, i32, String)> {
        let stream = self.stream.as_mut().context("not connected")?;

        let mut len_buf = [0u8; 4];
        stream.read_exact(&mut len_buf).await?;
        let length = i32::from_le_bytes(len_buf) as usize;

        let mut buf = vec![0u8; length];
        stream.read_exact(&mut buf).await?;

        let request_id = i32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
        let response_type = i32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
        let body = std::str::from_utf8(&buf[8..buf.len().saturating_sub(2)])
            .unwrap_or("")
            .to_string();

        Ok((request_id, response_type, body))
    }

    /// Execute an RCON command and return the response.
    pub async fn execute(&mut self, command: &str) -> Result<String> {
        let _id = self.send_rcon(RCON_EXEC, command).await?;
        let (_rid, _rtype, body) = self.read_rcon().await?;
        Ok(body)
    }
}

// ── GameConnector implementation ────────────────────────────────────────────

#[async_trait::async_trait]
impl GameConnector for FactorioConnection {
    async fn connect(&mut self) -> Result<()> {
        let stream = TcpStream::connect(&self.addr).await
            .with_context(|| format!("connecting to Factorio RCON at {}", self.addr))?;
        self.stream = Some(stream);

        // Authenticate
        let password = self.password.clone();
        let _id = self.send_rcon(RCON_AUTH, &password).await?;
        let (rid, _rtype, _body) = self.read_rcon().await?;

        if rid == -1 {
            anyhow::bail!("RCON authentication failed — wrong password?");
        }

        self.pending_events.push(GameEvent::Connected);
        Ok(())
    }

    async fn poll_events(&mut self) -> Result<Vec<GameEvent>> {
        // Factorio RCON is command-response only — no push events.
        // To get events, we'd periodically run commands like /players or read the console log.
        // For now, return any pending events from connect/commands.
        Ok(std::mem::take(&mut self.pending_events))
    }

    async fn send_action(&mut self, action: Action) -> Result<()> {
        match action {
            Action::SendChat(msg) => {
                // Factorio console chat
                self.execute(&msg).await?;
            }
            Action::Disconnect => {
                self.stream = None;
            }
            _ => {
                // Most actions aren't directly supported via RCON
                eprintln!("action not supported via Factorio RCON: {:?}", action);
            }
        }
        Ok(())
    }

    fn game_type(&self) -> GameType {
        GameType::Factorio
    }
}
