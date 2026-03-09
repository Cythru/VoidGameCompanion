use anyhow::Result;
use clap::{Parser, Subcommand};

use gc_core::auth::Auth;
use gc_core::config::Config;
use gc_core::coordinator::Coordinator;
use gc_core::persona::{self, PersonaId};

// ── CLI definition ──────────────────────────────────────────────────────────

#[derive(Parser)]
#[command(
    name = "gc",
    about = "VoidForge Game Companion — AI personas that play games with you",
    version
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Start a game companion session.
    Start {
        #[command(subcommand)]
        game: StartGame,
    },

    /// Show current companion status.
    Status,

    /// List available personas.
    Personas,

    /// Show or edit configuration.
    Config {
        /// Print current config as JSON.
        #[arg(long)]
        show: bool,

        /// Set a config key (key=value format).
        #[arg(long)]
        set: Option<String>,
    },
}

#[derive(Subcommand)]
enum StartGame {
    /// Connect to a Minecraft server.
    Minecraft {
        /// Server address (host:port).
        #[arg(long, default_value = "127.0.0.1:25565")]
        server: String,

        /// Persona to use.
        #[arg(long, default_value = "chihiro")]
        persona: String,

        /// Bot username (offline mode).
        #[arg(long, default_value = "VoidBot")]
        username: String,

        /// Disable TTS.
        #[arg(long)]
        no_tts: bool,
    },

    /// Connect to a Factorio server via RCON.
    Factorio {
        /// RCON address (host:port).
        #[arg(long, default_value = "127.0.0.1:25575")]
        rcon: String,

        /// RCON password.
        #[arg(long, env = "FACTORIO_RCON_PASSWORD")]
        password: Option<String>,

        /// Persona to use.
        #[arg(long, default_value = "miu")]
        persona: String,

        /// Disable TTS.
        #[arg(long)]
        no_tts: bool,
    },

    /// Connect to Dyson Sphere Program (requires BepInEx mod).
    Dsp {
        /// Mod bridge address.
        #[arg(long, default_value = "127.0.0.1:9876")]
        addr: String,

        /// Persona to use.
        #[arg(long, default_value = "chihiro")]
        persona: String,

        /// Disable TTS.
        #[arg(long)]
        no_tts: bool,
    },
}

// ── Main ────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    match cli.command {
        Commands::Start { game } => cmd_start(game).await,
        Commands::Status         => cmd_status().await,
        Commands::Personas       => cmd_personas(),
        Commands::Config { show, set } => cmd_config(show, set),
    }
}

async fn cmd_start(game: StartGame) -> Result<()> {
    match game {
        StartGame::Minecraft { server, persona, username, no_tts } => {
            let persona_id: PersonaId = persona.parse()?;

            let mut config = Config::load()?;
            config.persona = persona_id;
            config.minecraft_server = server.clone();
            config.username = username.clone();
            config.tts_enabled = !no_tts;

            let mut coordinator = Coordinator::new(config);

            // Check LLM availability
            let llm = gc_core::llm_bridge::LlmBridge::local();
            if !llm.health_check().await {
                eprintln!(
                    "\x1b[33mWARN: LLM backend at 127.0.0.1:9000 not reachable. \
                     Persona will use quick reactions only.\x1b[0m"
                );
            }

            let auth = Auth::minecraft_offline(&username);
            let connector = gc_minecraft::MinecraftConnection::new(&server, auth);

            eprintln!("Starting Minecraft companion as {} (persona: {})", username, persona_id);
            coordinator.start_game(Box::new(connector)).await?;
            coordinator.run().await?;
        }

        StartGame::Factorio { rcon, password, persona, no_tts } => {
            let persona_id: PersonaId = persona.parse()?;

            let mut config = Config::load()?;
            config.persona = persona_id;
            config.factorio_rcon = rcon.clone();
            config.tts_enabled = !no_tts;

            let password = password
                .or_else(|| {
                    if !config.factorio_rcon_password.is_empty() {
                        Some(config.factorio_rcon_password.clone())
                    } else {
                        None
                    }
                })
                .unwrap_or_default();

            let mut coordinator = Coordinator::new(config);

            let auth = Auth::factorio_rcon(&password);
            let connector = gc_factorio::FactorioConnection::new(&rcon, auth);

            eprintln!("Starting Factorio companion (persona: {})", persona_id);
            coordinator.start_game(Box::new(connector)).await?;
            coordinator.run().await?;
        }

        StartGame::Dsp { addr, persona, no_tts } => {
            let persona_id: PersonaId = persona.parse()?;

            let mut config = Config::load()?;
            config.persona = persona_id;
            config.tts_enabled = !no_tts;

            let mut coordinator = Coordinator::new(config);

            let connector = gc_dsp::DspConnection::new(&addr);

            eprintln!("Starting DSP companion (persona: {})", persona_id);
            coordinator.start_game(Box::new(connector)).await?;
            coordinator.run().await?;
        }
    }

    Ok(())
}

async fn cmd_status() -> Result<()> {
    let config = Config::load()?;
    let persona = persona::get_persona(config.persona);

    println!("VoidForge Game Companion");
    println!("========================");
    println!("Active persona: {} ({})", persona.name, persona.id);
    println!("TTS enabled:    {}", config.tts_enabled);
    println!("LLM backend:    {}", config.llm_base_url);
    println!("MC server:      {}", config.minecraft_server);
    println!("MC username:    {}", config.username);
    println!("Factorio RCON:  {}", config.factorio_rcon);

    // Check LLM health
    let llm = gc_core::llm_bridge::LlmBridge::local();
    let llm_ok = llm.health_check().await;
    println!("LLM status:     {}", if llm_ok { "online" } else { "offline" });

    // Check TTS availability
    let tts_ok = gc_core::tts::TtsEngine::check_available().await;
    println!("TTS status:     {}", if tts_ok { "available" } else { "not found (install edge-tts)" });

    Ok(())
}

fn cmd_personas() -> Result<()> {
    println!("Available personas:");
    println!();

    for p in persona::all_personas() {
        println!(
            "  {}{}{}  — {}",
            p.color.ansi_code(),
            p.name,
            gc_core::persona::TermColor::reset(),
            short_description(&p),
        );
        println!(
            "    Aggression: {:.0}%  Exploration: {:.0}%  Building: {:.0}%  Gathering: {:.0}%",
            p.weights.aggression * 100.0,
            p.weights.exploration * 100.0,
            p.weights.building * 100.0,
            p.weights.resource_gathering * 100.0,
        );
        println!("    Voice: {}", p.tts_voice);
        println!();
    }

    Ok(())
}

fn short_description(p: &gc_core::persona::GamePersona) -> &str {
    match p.id {
        PersonaId::Chihiro => "Methodical, shy, analytical. Optimizes everything.",
        PersonaId::Miu     => "Chaotic, crude, experimental. Swears and builds insane things.",
        PersonaId::Aiko    => "Supportive, warm, careful. Keeps everyone fed and safe.",
        PersonaId::Rim     => "Competitive, aggressive, minimal words. Combat specialist.",
    }
}

fn cmd_config(show: bool, set: Option<String>) -> Result<()> {
    let mut config = Config::load()?;

    if show || set.is_none() {
        let json = serde_json::to_string_pretty(&config)?;
        println!("{}", json);
        return Ok(());
    }

    if let Some(kv) = set {
        let parts: Vec<&str> = kv.splitn(2, '=').collect();
        if parts.len() != 2 {
            anyhow::bail!("expected key=value format, got: {}", kv);
        }
        let (key, value) = (parts[0], parts[1]);

        match key {
            "persona"          => config.persona = value.parse()?,
            "username"         => config.username = value.to_string(),
            "minecraft_server" => config.minecraft_server = value.to_string(),
            "factorio_rcon"    => config.factorio_rcon = value.to_string(),
            "tts_enabled"      => config.tts_enabled = value.parse()?,
            "llm_base_url"     => config.llm_base_url = value.to_string(),
            "llm_model"        => config.llm_model = value.to_string(),
            "auto_detect_game" => config.auto_detect_game = value.parse()?,
            "poll_interval_ms" => config.poll_interval_ms = value.parse()?,
            _                  => anyhow::bail!("unknown config key: {}", key),
        }

        config.save()?;
        eprintln!("Config updated: {} = {}", key, value);
    }

    Ok(())
}
