use anyhow::Result;
use serde::{Deserialize, Serialize};

// ── Game types ───────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum GameType {
    Minecraft,
    Factorio,
    DysonSphereProgram,
}

impl std::fmt::Display for GameType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Minecraft          => write!(f, "Minecraft"),
            Self::Factorio           => write!(f, "Factorio"),
            Self::DysonSphereProgram => write!(f, "Dyson Sphere Program"),
        }
    }
}

// ── Events emitted by game connectors ────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum GameEvent {
    Chat {
        sender:  String,
        message: String,
    },
    PlayerNearby {
        name:     String,
        distance: f64,
    },
    EntitySpawn {
        entity_type: String,
        x: f64,
        y: f64,
        z: f64,
    },
    BlockBreak {
        x: i32,
        y: i32,
        z: i32,
        block: String,
    },
    BlockPlace {
        x: i32,
        y: i32,
        z: i32,
        block: String,
    },
    Death {
        message: String,
    },
    NightFall,
    DayBreak,
    HealthLow {
        health: f32,
    },
    HungerLow {
        food_level: f32,
    },
    MobAlert {
        entity_type: String,
        distance:    f64,
    },
    Connected,
    Disconnected {
        reason: String,
    },
}

impl GameEvent {
    /// Short tag for quick-reaction lookups.
    pub fn tag(&self) -> &'static str {
        match self {
            Self::Chat { .. }          => "chat",
            Self::PlayerNearby { .. }  => "player_nearby",
            Self::EntitySpawn { .. }   => "entity_spawn",
            Self::BlockBreak { .. }    => "block_break",
            Self::BlockPlace { .. }    => "block_place",
            Self::Death { .. }         => "death",
            Self::NightFall            => "nightfall",
            Self::DayBreak             => "daybreak",
            Self::HealthLow { .. }     => "health_low",
            Self::HungerLow { .. }     => "hunger_low",
            Self::MobAlert { .. }      => "mob_alert",
            Self::Connected            => "connected",
            Self::Disconnected { .. }  => "disconnected",
        }
    }
}

// ── Actions sent to game connectors ──────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum Action {
    SendChat(String),
    MoveToPosition { x: f64, y: f64, z: f64 },
    BreakBlock { x: i32, y: i32, z: i32 },
    PlaceBlock { x: i32, y: i32, z: i32, block_id: u32 },
    AttackEntity { entity_id: u32 },
    UseItem,
    SelectHotbar(u8),
    Jump,
    Sneak(bool),
    Sprint(bool),
    Disconnect,
}

// ── Connector trait ──────────────────────────────────────────────────────────

#[async_trait::async_trait]
pub trait GameConnector: Send + Sync {
    /// Establish connection to the game server.
    async fn connect(&mut self) -> Result<()>;

    /// Poll for new game events. Returns empty vec if nothing new.
    async fn poll_events(&mut self) -> Result<Vec<GameEvent>>;

    /// Send an action into the game world.
    async fn send_action(&mut self, action: Action) -> Result<()>;

    /// Send a chat message (convenience wrapper).
    async fn chat(&mut self, msg: &str) -> Result<()> {
        self.send_action(Action::SendChat(msg.to_string())).await
    }

    /// Which game this connector handles.
    fn game_type(&self) -> GameType;
}

// async_trait re-export so downstream crates don't need to depend on it directly
pub use async_trait::async_trait;
