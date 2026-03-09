use anyhow::{Context, Result};
use gc_core::auth::Auth;
use gc_core::game_trait::*;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

// ── Protocol constants ──────────────────────────────────────────────────────

const PROTOCOL_VERSION: i32 = 765; // 1.20.4

/// Connection state machine phases.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ConnectionState {
    Disconnected,
    Handshaking,
    Login,
    Play,
}

// ── VarInt encoding/decoding ────────────────────────────────────────────────

fn encode_varint(mut value: i32) -> Vec<u8> {
    let mut buf = Vec::with_capacity(5);
    loop {
        let mut byte = (value & 0x7F) as u8;
        value = ((value as u32) >> 7) as i32;
        if value != 0 {
            byte |= 0x80;
        }
        buf.push(byte);
        if value == 0 {
            break;
        }
    }
    buf
}

async fn read_varint(stream: &mut TcpStream) -> Result<i32> {
    let mut result: i32 = 0;
    let mut shift = 0;
    loop {
        let mut byte = [0u8; 1];
        stream.read_exact(&mut byte).await
            .context("reading varint byte")?;
        result |= ((byte[0] & 0x7F) as i32) << shift;
        if byte[0] & 0x80 == 0 {
            break;
        }
        shift += 7;
        if shift >= 35 {
            anyhow::bail!("varint too large");
        }
    }
    Ok(result)
}

fn encode_string(s: &str) -> Vec<u8> {
    let bytes = s.as_bytes();
    let mut buf = encode_varint(bytes.len() as i32);
    buf.extend_from_slice(bytes);
    buf
}

// ── Zlib decompression via flate2 (pure Rust) ──────────────────────────────

fn decompress_packet(data: &[u8], uncompressed_len: usize) -> Result<Vec<u8>> {
    use std::io::Read;

    if uncompressed_len == 0 {
        return Ok(data.to_vec());
    }

    let mut output = Vec::with_capacity(uncompressed_len);
    let mut decoder = flate2::read::ZlibDecoder::new(data);
    decoder.read_to_end(&mut output)?;
    Ok(output)
}

// ── Encryption placeholder (online mode) ────────────────────────────────────

extern "C" {
    /// AES-128 CFB8 encrypt/decrypt. Implemented in C++ for online mode.
    /// Not used in offline mode — placeholder for future online auth.
    fn vf_aes_cfb8_crypt(
        data: *mut u8,
        len: usize,
        key: *const u8,
        iv: *mut u8,
        encrypt: bool,
    );
}

// ── Minecraft connection ────────────────────────────────────────────────────

pub struct MinecraftConnection {
    addr:             String,
    auth:             Auth,
    stream:           Option<TcpStream>,
    state:            ConnectionState,
    compression:      Option<i32>, // compression threshold, None = disabled
    pending_events:   Vec<GameEvent>,
}

impl MinecraftConnection {
    pub fn new(addr: impl Into<String>, auth: Auth) -> Self {
        Self {
            addr:           addr.into(),
            auth,
            stream:         None,
            state:          ConnectionState::Disconnected,
            compression:    None,
            pending_events: Vec::new(),
        }
    }

    /// Build and send a packet (length-prefixed).
    async fn send_packet(&mut self, packet_id: i32, payload: &[u8]) -> Result<()> {
        let stream = self.stream.as_mut()
            .context("not connected")?;

        let id_bytes = encode_varint(packet_id);
        let total_len = id_bytes.len() + payload.len();
        let len_bytes = encode_varint(total_len as i32);

        stream.write_all(&len_bytes).await?;
        stream.write_all(&id_bytes).await?;
        stream.write_all(payload).await?;
        stream.flush().await?;
        Ok(())
    }

    /// Read a single packet. Returns (packet_id, payload).
    async fn read_packet(&mut self) -> Result<(i32, Vec<u8>)> {
        let stream = self.stream.as_mut()
            .context("not connected")?;

        let packet_len = read_varint(stream).await
            .context("reading packet length")?;

        if packet_len < 0 || packet_len > 2_097_152 {
            anyhow::bail!("invalid packet length: {}", packet_len);
        }

        let mut data = vec![0u8; packet_len as usize];
        stream.read_exact(&mut data).await
            .context("reading packet data")?;

        // Handle compression
        if let Some(threshold) = self.compression {
            if threshold >= 0 {
                let (data_length, consumed) = decode_varint_from_slice(&data)?;
                let rest = &data[consumed..];

                if data_length > 0 {
                    let decompressed = decompress_packet(rest, data_length as usize)?;
                    let (packet_id, id_consumed) = decode_varint_from_slice(&decompressed)?;
                    return Ok((packet_id, decompressed[id_consumed..].to_vec()));
                } else {
                    let (packet_id, id_consumed) = decode_varint_from_slice(rest)?;
                    return Ok((packet_id, rest[id_consumed..].to_vec()));
                }
            }
        }

        // No compression
        let (packet_id, id_consumed) = decode_varint_from_slice(&data)?;
        Ok((packet_id, data[id_consumed..].to_vec()))
    }

    /// Send handshake packet.
    async fn send_handshake(&mut self, next_state: i32) -> Result<()> {
        let (host, port) = parse_addr(&self.addr)?;

        let mut payload = Vec::new();
        payload.extend(encode_varint(PROTOCOL_VERSION));
        payload.extend(encode_string(&host));
        payload.extend(&(port as u16).to_be_bytes());
        payload.extend(encode_varint(next_state));

        self.send_packet(0x00, &payload).await?;
        self.state = ConnectionState::Handshaking;
        Ok(())
    }

    /// Send login start packet.
    async fn send_login_start(&mut self) -> Result<()> {
        let username = self.auth.username()
            .context("auth has no username")?
            .to_string();
        let uuid = self.auth.uuid()
            .unwrap_or("00000000-0000-0000-0000-000000000000");

        let mut payload = Vec::new();
        payload.extend(encode_string(&username));
        // Player UUID (as raw 128-bit, no dashes)
        let uuid_bytes = uuid_to_bytes(uuid)?;
        payload.extend(&uuid_bytes);

        self.send_packet(0x00, &payload).await?;
        self.state = ConnectionState::Login;
        Ok(())
    }

    /// Process login phase packets until we reach Play state.
    async fn process_login(&mut self) -> Result<()> {
        loop {
            let (packet_id, payload) = self.read_packet().await
                .context("reading login packet")?;

            match packet_id {
                0x00 => {
                    // Disconnect — read reason and bail
                    let (reason, _) = decode_string_from_slice(&payload)?;
                    anyhow::bail!("disconnected during login: {}", reason);
                }
                0x01 => {
                    // Encryption request — online mode, not supported yet
                    anyhow::bail!(
                        "server requires online-mode authentication (encryption request). \
                         Only offline mode is supported currently."
                    );
                }
                0x02 => {
                    // Login success — transition to Play
                    self.state = ConnectionState::Play;
                    self.pending_events.push(GameEvent::Connected);
                    return Ok(());
                }
                0x03 => {
                    // Set compression
                    let (threshold, _) = decode_varint_from_slice(&payload)?;
                    self.compression = Some(threshold);
                }
                _ => {
                    // Unknown login packet — skip
                }
            }
        }
    }

    /// Parse play-state packets and emit game events.
    fn parse_play_packet(&mut self, packet_id: i32, payload: &[u8]) {
        match packet_id {
            // System chat message (0x64 in 1.20.4)
            0x64 => {
                if let Ok((msg, _)) = decode_string_from_slice(payload) {
                    // Try to extract sender from JSON chat
                    if let Some((sender, text)) = parse_chat_json(&msg) {
                        self.pending_events.push(GameEvent::Chat {
                            sender,
                            message: text,
                        });
                    }
                }
            }
            // Player chat message (0x35 in 1.20.4)
            0x35 => {
                if let Ok((msg, _)) = decode_string_from_slice(payload) {
                    self.pending_events.push(GameEvent::Chat {
                        sender: "unknown".into(),
                        message: msg,
                    });
                }
            }
            // Death combat event (0x3A in 1.20.4)
            0x3A => {
                if let Ok((msg, _)) = decode_string_from_slice(payload) {
                    self.pending_events.push(GameEvent::Death { message: msg });
                }
            }
            // Set time (0x5E in 1.20.4)
            0x5E => {
                if payload.len() >= 16 {
                    let time_of_day = i64::from_be_bytes([
                        payload[8], payload[9], payload[10], payload[11],
                        payload[12], payload[13], payload[14], payload[15],
                    ]);
                    let abs_time = time_of_day.unsigned_abs() % 24000;
                    // Nightfall at 13000, daybreak at 23000 (wraps)
                    if abs_time == 13000 {
                        self.pending_events.push(GameEvent::NightFall);
                    } else if abs_time == 23000 {
                        self.pending_events.push(GameEvent::DayBreak);
                    }
                }
            }
            // Keep alive (0x24 in 1.20.4) — must respond
            0x24 => {
                // Will be handled in poll_events by sending keep alive response
            }
            // Disconnect (play) (0x1B in 1.20.4)
            0x1B => {
                let reason = decode_string_from_slice(payload)
                    .map(|(s, _)| s)
                    .unwrap_or_else(|_| "unknown".into());
                self.pending_events.push(GameEvent::Disconnected { reason });
                self.state = ConnectionState::Disconnected;
            }
            _ => {}
        }
    }
}

// ── GameConnector implementation ────────────────────────────────────────────

#[async_trait::async_trait]
impl GameConnector for MinecraftConnection {
    async fn connect(&mut self) -> Result<()> {
        let stream = TcpStream::connect(&self.addr).await
            .with_context(|| format!("connecting to {}", self.addr))?;
        self.stream = Some(stream);

        self.send_handshake(2).await?;  // next_state=2 (login)
        self.send_login_start().await?;
        self.process_login().await?;

        Ok(())
    }

    async fn poll_events(&mut self) -> Result<Vec<GameEvent>> {
        if self.state != ConnectionState::Play {
            return Ok(Vec::new());
        }

        // Non-blocking read: try to read available packets
        let stream = self.stream.as_mut().context("not connected")?;

        // Check if data is available (non-blocking peek)
        let mut peek_buf = [0u8; 1];
        match stream.try_read(&mut peek_buf) {
            Ok(0) => {
                // Connection closed
                self.pending_events.push(GameEvent::Disconnected {
                    reason: "connection closed".into(),
                });
                self.state = ConnectionState::Disconnected;
            }
            Ok(_) => {
                // Data available — we peeked one byte, need to read full packet
                // For simplicity, read packets in a loop until would-block
                // Note: this is a simplification; real impl would use a proper read buffer
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                // No data available — that's fine
            }
            Err(e) => {
                return Err(e.into());
            }
        }

        Ok(std::mem::take(&mut self.pending_events))
    }

    async fn send_action(&mut self, action: gc_core::game_trait::Action) -> Result<()> {
        match action {
            Action::SendChat(msg) => {
                // Chat message packet (0x05 in 1.20.4)
                let payload = encode_string(&msg);
                // Timestamp (i64) + salt (i64) + signature length (varint 0)
                let mut full_payload = payload;
                full_payload.extend(&0i64.to_be_bytes()); // timestamp
                full_payload.extend(&0i64.to_be_bytes()); // salt
                full_payload.extend(encode_varint(0));     // no signature
                full_payload.extend(encode_varint(0));     // message count
                full_payload.push(0);                      // acknowledged bitset (empty)
                self.send_packet(0x05, &full_payload).await?;
            }
            Action::SelectHotbar(slot) => {
                if slot > 8 {
                    anyhow::bail!("hotbar slot must be 0-8, got {}", slot);
                }
                // Set held item (0x2C in 1.20.4)
                let payload = [slot as u8, 0]; // slot as short (big-endian)
                self.send_packet(0x2C, &(slot as i16).to_be_bytes()).await?;
                let _ = payload;
            }
            Action::Jump => {
                // Player command: start jump (action ID 0 = start sneaking is wrong)
                // Actually done via position packet with on_ground flag changes
                // Simplified: send a player movement packet with Y + 1.25
            }
            Action::Sneak(active) => {
                // Entity action packet (0x1E in 1.20.4)
                // Action: 0 = start sneaking, 1 = stop sneaking
                let action_id = if active { 0i32 } else { 1 };
                let mut payload = encode_varint(0); // entity ID (self = 0, will be set)
                payload.extend(encode_varint(action_id));
                payload.extend(encode_varint(0)); // jump boost
                self.send_packet(0x1E, &payload).await?;
            }
            Action::Sprint(active) => {
                let action_id = if active { 3i32 } else { 4 };
                let mut payload = encode_varint(0);
                payload.extend(encode_varint(action_id));
                payload.extend(encode_varint(0));
                self.send_packet(0x1E, &payload).await?;
            }
            Action::Disconnect => {
                if let Some(stream) = self.stream.take() {
                    drop(stream);
                }
                self.state = ConnectionState::Disconnected;
            }
            _ => {
                // Other actions require more complex packet construction
                // (position tracking, block dig sequences, etc.)
                eprintln!("action not yet implemented: {:?}", action);
            }
        }
        Ok(())
    }

    fn game_type(&self) -> gc_core::game_trait::GameType {
        gc_core::game_trait::GameType::Minecraft
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

fn decode_varint_from_slice(data: &[u8]) -> Result<(i32, usize)> {
    let mut result: i32 = 0;
    let mut shift = 0;
    for (i, &byte) in data.iter().enumerate() {
        result |= ((byte & 0x7F) as i32) << shift;
        if byte & 0x80 == 0 {
            return Ok((result, i + 1));
        }
        shift += 7;
        if shift >= 35 {
            anyhow::bail!("varint too large");
        }
    }
    anyhow::bail!("incomplete varint")
}

fn decode_string_from_slice(data: &[u8]) -> Result<(String, usize)> {
    let (len, consumed) = decode_varint_from_slice(data)?;
    let len = len as usize;
    if data.len() < consumed + len {
        anyhow::bail!("string extends past end of data");
    }
    let s = std::str::from_utf8(&data[consumed..consumed + len])
        .context("invalid utf8 in string")?
        .to_string();
    Ok((s, consumed + len))
}

fn parse_addr(addr: &str) -> Result<(String, u16)> {
    let parts: Vec<&str> = addr.rsplitn(2, ':').collect();
    match parts.as_slice() {
        [port_str, host] => {
            let port: u16 = port_str.parse()
                .with_context(|| format!("invalid port: {}", port_str))?;
            Ok((host.to_string(), port))
        }
        _ => Ok((addr.to_string(), 25565)),
    }
}

fn uuid_to_bytes(uuid_str: &str) -> Result<[u8; 16]> {
    let hex: String = uuid_str.chars().filter(|c| c.is_ascii_hexdigit()).collect();
    if hex.len() != 32 {
        anyhow::bail!("invalid UUID: {}", uuid_str);
    }
    let mut bytes = [0u8; 16];
    for i in 0..16 {
        bytes[i] = u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16)?;
    }
    Ok(bytes)
}

fn parse_chat_json(json_str: &str) -> Option<(String, String)> {
    let v: serde_json::Value = serde_json::from_str(json_str).ok()?;
    // MC chat format: {"text": "...", "extra": [...]}
    // Player messages: {"translate": "chat.type.text", "with": [{"text":"player"}, "message"]}
    if let Some(with) = v.get("with").and_then(|w| w.as_array()) {
        if with.len() >= 2 {
            let sender = with[0].get("text")
                .or_else(|| with[0].as_str().map(serde_json::Value::from).as_ref().map(|_| &with[0]))
                .and_then(|v| v.as_str())
                .unwrap_or("unknown")
                .to_string();
            let message = with[1].as_str()
                .unwrap_or("")
                .to_string();
            return Some((sender, message));
        }
    }
    // Fallback: plain text
    let text = v.get("text").and_then(|t| t.as_str()).unwrap_or("").to_string();
    if text.is_empty() {
        None
    } else {
        Some(("server".into(), text))
    }
}
