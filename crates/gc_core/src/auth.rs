use anyhow::Result;
use serde::{Deserialize, Serialize};

// ── Minecraft offline-mode auth ─────────────────────────────────────────────

/// Offline-mode authentication: just a username, no token verification.
/// Minecraft offline mode generates a UUID from the username deterministically.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OfflineAuth {
    pub username: String,
    pub uuid:     String,
}

impl OfflineAuth {
    /// Create offline auth from a username.
    /// UUID is derived as "OfflinePlayer:<name>" MD5, formatted as UUID v3.
    pub fn new(username: impl Into<String>) -> Self {
        let name = username.into();
        let uuid = offline_uuid(&name);
        Self { username: name, uuid }
    }
}

/// Generate a Minecraft offline-mode UUID from a username.
/// Java edition: MD5("OfflinePlayer:" + name), then set version 3 and variant bits.
fn offline_uuid(name: &str) -> String {
    use std::io::Write;

    let input = format!("OfflinePlayer:{}", name);
    let digest = md5_hash(input.as_bytes());

    // Set version (3) and variant (RFC 4122) bits
    let mut bytes = digest;
    bytes[6] = (bytes[6] & 0x0f) | 0x30; // version 3
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // variant

    // Format as UUID string
    let mut buf = Vec::with_capacity(36);
    for (i, b) in bytes.iter().enumerate() {
        if i == 4 || i == 6 || i == 8 || i == 10 {
            write!(buf, "-").unwrap();
        }
        write!(buf, "{:02x}", b).unwrap();
    }
    String::from_utf8(buf).unwrap()
}

/// Minimal MD5 implementation (offline UUID generation only).
/// For production, consider using the `md5` crate — but this avoids an extra dep.
fn md5_hash(data: &[u8]) -> [u8; 16] {
    // Constants
    const S: [u32; 64] = [
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
    ];
    const K: [u32; 64] = [
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
    ];

    let mut a0: u32 = 0x67452301;
    let mut b0: u32 = 0xefcdab89;
    let mut c0: u32 = 0x98badcfe;
    let mut d0: u32 = 0x10325476;

    // Pre-processing: pad message
    let orig_len_bits = (data.len() as u64) * 8;
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&orig_len_bits.to_le_bytes());

    // Process each 512-bit chunk
    for chunk in msg.chunks_exact(64) {
        let mut m = [0u32; 16];
        for (i, word) in chunk.chunks_exact(4).enumerate() {
            m[i] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
        }

        let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);

        for i in 0..64 {
            let (f, g) = match i {
                0..=15  => ((b & c) | (!b & d),             i),
                16..=31 => ((d & b) | (!d & c),             (5 * i + 1) % 16),
                32..=47 => (b ^ c ^ d,                      (3 * i + 5) % 16),
                _       => (c ^ (b | !d),                   (7 * i) % 16),
            };

            let f = f.wrapping_add(a).wrapping_add(K[i]).wrapping_add(m[g]);
            a = d;
            d = c;
            c = b;
            b = b.wrapping_add(f.rotate_left(S[i]));
        }

        a0 = a0.wrapping_add(a);
        b0 = b0.wrapping_add(b);
        c0 = c0.wrapping_add(c);
        d0 = d0.wrapping_add(d);
    }

    let mut result = [0u8; 16];
    result[0..4].copy_from_slice(&a0.to_le_bytes());
    result[4..8].copy_from_slice(&b0.to_le_bytes());
    result[8..12].copy_from_slice(&c0.to_le_bytes());
    result[12..16].copy_from_slice(&d0.to_le_bytes());
    result
}

// ── Auth container ──────────────────────────────────────────────────────────

/// Unified auth type — extensible for online mode later.
#[derive(Debug, Clone)]
pub enum Auth {
    /// Minecraft offline mode — username only, no token.
    MinecraftOffline(OfflineAuth),

    /// Placeholder for Microsoft OAuth2 flow (online Minecraft).
    MinecraftOnline {
        username:     String,
        uuid:         String,
        access_token: String,
    },

    /// Factorio RCON password.
    FactorioRcon {
        password: String,
    },

    /// No auth needed.
    None,
}

impl Auth {
    /// Create offline Minecraft auth from a username.
    pub fn minecraft_offline(username: impl Into<String>) -> Self {
        Self::MinecraftOffline(OfflineAuth::new(username))
    }

    /// Create Factorio RCON auth.
    pub fn factorio_rcon(password: impl Into<String>) -> Self {
        Self::FactorioRcon { password: password.into() }
    }

    /// Get the username, if applicable.
    pub fn username(&self) -> Option<&str> {
        match self {
            Self::MinecraftOffline(a)          => Some(&a.username),
            Self::MinecraftOnline { username, .. } => Some(username),
            _                                       => None,
        }
    }

    /// Get the UUID, if applicable.
    pub fn uuid(&self) -> Option<&str> {
        match self {
            Self::MinecraftOffline(a)      => Some(&a.uuid),
            Self::MinecraftOnline { uuid, .. } => Some(uuid),
            _                                   => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_offline_uuid_format() {
        let auth = OfflineAuth::new("VoidBot");
        // Should be a valid UUID format: 8-4-4-4-12
        let parts: Vec<&str> = auth.uuid.split('-').collect();
        assert_eq!(parts.len(), 5);
        assert_eq!(parts[0].len(), 8);
        assert_eq!(parts[1].len(), 4);
        assert_eq!(parts[2].len(), 4);
        assert_eq!(parts[3].len(), 4);
        assert_eq!(parts[4].len(), 12);
        // Version should be 3
        assert!(parts[2].starts_with('3'));
    }
}
