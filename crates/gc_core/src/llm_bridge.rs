use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::persona::GamePersona;

// IPv4 only — localhost resolves to ::1 (IPv6) on CachyOS, llama.cpp binds 0.0.0.0
const DEFAULT_BASE_URL: &str = "http://127.0.0.1:9000";
const DEFAULT_MODEL:    &str = "voidminillama";
const DEFAULT_TIMEOUT:  u64  = 120;

// ── Request / Response ───────────────────────────────────────────────────────

#[derive(Serialize)]
struct ChatRequest<'a> {
    model:       &'a str,
    max_tokens:  u32,
    temperature: f32,
    messages:    Vec<ChatMessage<'a>>,
}

#[derive(Serialize)]
struct ChatMessage<'a> {
    role:    &'a str,
    content: &'a str,
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Vec<Choice>,
}

#[derive(Deserialize)]
struct Choice {
    message: ResponseMessage,
}

#[derive(Deserialize)]
struct ResponseMessage {
    content: Option<String>,
}

// ── LLM Bridge ──────────────────────────────────────────────────────────────

#[derive(Clone)]
pub struct LlmBridge {
    base_url:     String,
    model:        String,
    timeout_secs: u64,
    client:       reqwest::Client,
}

impl LlmBridge {
    pub fn new(base_url: impl Into<String>, model: impl Into<String>, timeout_secs: u64) -> Self {
        Self {
            base_url:     base_url.into(),
            model:        model.into(),
            timeout_secs,
            client:       reqwest::Client::new(),
        }
    }

    /// Create a bridge pointing at the local llama.cpp instance.
    /// MUST use 127.0.0.1 — not localhost — due to IPv6 resolution bug on CachyOS.
    pub fn local() -> Self {
        Self::new(DEFAULT_BASE_URL, DEFAULT_MODEL, DEFAULT_TIMEOUT)
    }

    /// Raw chat completion — system prompt + user message.
    pub async fn chat(&self, system: &str, user: &str, max_tokens: u32) -> Result<String> {
        let body = ChatRequest {
            model:       &self.model,
            max_tokens,
            temperature: 0.8,
            messages:    vec![
                ChatMessage { role: "system", content: system },
                ChatMessage { role: "user",   content: user },
            ],
        };

        let resp = self.client
            .post(format!("{}/v1/chat/completions", self.base_url))
            .timeout(std::time::Duration::from_secs(self.timeout_secs))
            .json(&body)
            .send()
            .await?
            .json::<ChatResponse>()
            .await?;

        let text = resp.choices
            .into_iter()
            .next()
            .and_then(|c| c.message.content)
            .unwrap_or_default();

        Ok(text.trim().to_string())
    }

    /// Chat with persona system prompt injected.
    /// The persona's personality string becomes the system prompt, and the
    /// game context + user message are combined into the user message.
    pub async fn persona_chat(
        &self,
        persona: &GamePersona,
        context: &str,
        user_msg: &str,
        max_tokens: u32,
    ) -> Result<String> {
        let user = if context.is_empty() {
            user_msg.to_string()
        } else {
            format!("[Game context: {}]\n\n{}", context, user_msg)
        };

        self.chat(&persona.personality, &user, max_tokens).await
    }

    /// Generate a persona reaction to a game event.
    pub async fn react_to_event(
        &self,
        persona: &GamePersona,
        event_description: &str,
    ) -> Result<String> {
        let user = format!(
            "React in character to this game event (1-2 sentences max): {}",
            event_description,
        );
        self.chat(&persona.personality, &user, 80).await
    }

    /// Check if the LLM backend is reachable.
    pub async fn health_check(&self) -> bool {
        let url = format!("{}/v1/models", self.base_url);
        self.client
            .get(&url)
            .timeout(std::time::Duration::from_secs(5))
            .send()
            .await
            .map(|r| r.status().is_success())
            .unwrap_or(false)
    }
}
