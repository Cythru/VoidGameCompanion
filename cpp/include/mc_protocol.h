#ifndef MC_PROTOCOL_H
#define MC_PROTOCOL_H

#include "gc_common.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus

#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <variant>

// ---------------------------------------------------------------------------
// Minecraft 1.20.4 Packet IDs
// ---------------------------------------------------------------------------
namespace mc {

// Protocol version for MC 1.20.4
inline constexpr int32_t PROTOCOL_VERSION = 765;

// Connection states
enum class ConnState : uint8_t {
    Handshake = 0,
    Status    = 1,
    Login     = 2,
    Play      = 3,
};

// -- Serverbound (client -> server) packet IDs --
namespace sb {
    // Handshake state
    inline constexpr int32_t HANDSHAKE             = 0x00;

    // Login state
    inline constexpr int32_t LOGIN_START           = 0x00;
    inline constexpr int32_t ENCRYPTION_RESPONSE   = 0x01;
    inline constexpr int32_t LOGIN_ACKNOWLEDGED     = 0x03;

    // Play state
    inline constexpr int32_t CONFIRM_TELEPORT      = 0x00;
    inline constexpr int32_t CHAT_COMMAND           = 0x04;
    inline constexpr int32_t CHAT_MESSAGE           = 0x05;
    inline constexpr int32_t CLIENT_STATUS          = 0x08;
    inline constexpr int32_t PLAYER_POSITION        = 0x17;
    inline constexpr int32_t PLAYER_POSITION_ROT    = 0x18;
    inline constexpr int32_t PLAYER_ROTATION        = 0x19;
    inline constexpr int32_t PLAYER_ON_GROUND       = 0x1A;
    inline constexpr int32_t PLAYER_DIGGING         = 0x2C;
    inline constexpr int32_t PLAYER_BLOCK_PLACEMENT = 0x35;
    inline constexpr int32_t USE_ITEM               = 0x36;
    inline constexpr int32_t KEEP_ALIVE             = 0x15;
    inline constexpr int32_t SWING_ARM              = 0x33;
    inline constexpr int32_t INTERACT_ENTITY        = 0x13;
} // namespace sb

// -- Clientbound (server -> client) packet IDs --
namespace cb {
    inline constexpr int32_t LOGIN_SUCCESS          = 0x02;
    inline constexpr int32_t SET_COMPRESSION        = 0x03;

    // Play state
    inline constexpr int32_t KEEP_ALIVE             = 0x24;
    inline constexpr int32_t CHUNK_DATA             = 0x25;
    inline constexpr int32_t BLOCK_UPDATE           = 0x09;
    inline constexpr int32_t CHAT_MESSAGE           = 0x37;
    inline constexpr int32_t SYSTEM_CHAT            = 0x69;
    inline constexpr int32_t ENTITY_POSITION        = 0x2C;
    inline constexpr int32_t ENTITY_POSITION_ROT    = 0x2D;
    inline constexpr int32_t ENTITY_ROTATION        = 0x2E;
    inline constexpr int32_t SYNC_PLAYER_POSITION   = 0x3E;
    inline constexpr int32_t SPAWN_ENTITY           = 0x01;
    inline constexpr int32_t REMOVE_ENTITIES        = 0x40;
    inline constexpr int32_t DEATH_COMBAT_EVENT     = 0x3A;
    inline constexpr int32_t SET_HEALTH             = 0x59;
    inline constexpr int32_t RESPAWN                = 0x45;
    inline constexpr int32_t DISCONNECT_PLAY        = 0x1B;
} // namespace cb

// ---------------------------------------------------------------------------
// VarInt encode/decode
// ---------------------------------------------------------------------------
struct VarIntResult {
    int32_t value;
    int     bytes_read; // 0 on error (need more data)
};

[[nodiscard]] VarIntResult read_varint(const uint8_t* data, size_t len) noexcept;
int write_varint(uint8_t* out, int32_t value) noexcept;          // returns bytes written (1-5)
int varint_size(int32_t value) noexcept;                          // how many bytes needed

// ---------------------------------------------------------------------------
// Packet buffer — growable write buffer for encoding
// ---------------------------------------------------------------------------
class PacketBuffer {
public:
    PacketBuffer() noexcept;
    explicit PacketBuffer(size_t reserve) noexcept;

    void write_varint(int32_t v) noexcept;
    void write_u8(uint8_t v) noexcept;
    void write_u16(uint16_t v) noexcept;      // big-endian
    void write_i32(int32_t v) noexcept;       // big-endian
    void write_i64(int64_t v) noexcept;       // big-endian
    void write_f32(float v) noexcept;         // big-endian
    void write_f64(double v) noexcept;        // big-endian
    void write_bool(bool v) noexcept;
    void write_string(std::string_view s) noexcept;   // varint-prefixed UTF-8
    void write_bytes(const uint8_t* d, size_t n) noexcept;
    void write_position(int32_t x, int32_t y, int32_t z) noexcept; // packed position

    [[nodiscard]] const uint8_t* data() const noexcept { return buf_.data(); }
    [[nodiscard]] size_t size() const noexcept { return buf_.size(); }
    void clear() noexcept { buf_.clear(); }

    // Build a framed packet: [varint length][varint packet_id][payload]
    [[nodiscard]] std::vector<uint8_t> frame(int32_t packet_id) const noexcept;

private:
    std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// Packet reader — zero-copy cursor over received data
// ---------------------------------------------------------------------------
class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t len) noexcept;

    [[nodiscard]] size_t remaining() const noexcept { return len_ - pos_; }
    [[nodiscard]] bool has(size_t n) const noexcept { return remaining() >= n; }
    [[nodiscard]] bool error() const noexcept { return error_; }

    int32_t  read_varint() noexcept;
    uint8_t  read_u8() noexcept;
    uint16_t read_u16() noexcept;
    int32_t  read_i32() noexcept;
    int64_t  read_i64() noexcept;
    float    read_f32() noexcept;
    double   read_f64() noexcept;
    bool     read_bool() noexcept;
    std::string read_string() noexcept;
    void     read_bytes(uint8_t* out, size_t n) noexcept;
    void     skip(size_t n) noexcept;

    // Packed position: x(26) | z(26) | y(12)
    gc_block_pos_t read_position() noexcept;

    // Access to remaining raw data
    [[nodiscard]] const uint8_t* cursor() const noexcept { return data_ + pos_; }

private:
    const uint8_t* data_;
    size_t         len_;
    size_t         pos_;
    bool           error_;
};

// ---------------------------------------------------------------------------
// Decoded packet variants (key packets only)
// ---------------------------------------------------------------------------
struct PktLoginSuccess {
    gc_uuid_t   uuid;
    std::string  username;
};

struct PktKeepAlive {
    int64_t keep_alive_id;
};

struct PktChatMessage {
    std::string message;
    // Simplified — full chat has signatures, etc.
};

struct PktSystemChat {
    std::string content;     // JSON text component
    bool        overlay;
};

struct PktChunkData {
    int32_t              chunk_x;
    int32_t              chunk_z;
    std::vector<uint8_t> heightmaps_nbt;   // raw NBT
    std::vector<uint8_t> chunk_sections;   // raw section data
    // Block entities omitted for now
};

struct PktBlockUpdate {
    gc_block_pos_t position;
    int32_t        block_state_id;
};

struct PktEntityPosition {
    int32_t entity_id;
    int16_t delta_x;
    int16_t delta_y;
    int16_t delta_z;
    bool    on_ground;
};

struct PktSyncPlayerPosition {
    double x, y, z;
    float  yaw, pitch;
    uint8_t flags;
    int32_t teleport_id;
};

struct PktSpawnEntity {
    int32_t   entity_id;
    gc_uuid_t uuid;
    int32_t   entity_type;
    double    x, y, z;
    float     pitch, yaw;
    float     head_yaw;
    int32_t   data;
    int16_t   vel_x, vel_y, vel_z;
};

struct PktRemoveEntities {
    std::vector<int32_t> entity_ids;
};

struct PktDeathCombatEvent {
    int32_t     player_id;
    std::string message; // JSON text component
};

struct PktSetHealth {
    float   health;
    int32_t food;
    float   saturation;
};

struct PktDisconnect {
    std::string reason; // JSON text component
};

using DecodedPacket = std::variant<
    PktLoginSuccess,
    PktKeepAlive,
    PktChatMessage,
    PktSystemChat,
    PktChunkData,
    PktBlockUpdate,
    PktEntityPosition,
    PktSyncPlayerPosition,
    PktSpawnEntity,
    PktRemoveEntities,
    PktDeathCombatEvent,
    PktSetHealth,
    PktDisconnect
>;

// ---------------------------------------------------------------------------
// Packet encoder functions (serverbound)
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<uint8_t> encode_handshake(
    std::string_view server_addr, uint16_t port, ConnState next_state) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_login_start(
    std::string_view username, const gc_uuid_t& uuid) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_keep_alive(int64_t id) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_chat_command(std::string_view command) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_chat_message(
    std::string_view message, int64_t timestamp, int64_t salt) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_player_position(
    double x, double y, double z, bool on_ground) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_player_position_rotation(
    double x, double y, double z, float yaw, float pitch, bool on_ground) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_player_digging(
    int32_t status, gc_block_pos_t pos, uint8_t face, int32_t sequence) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_player_block_placement(
    int32_t hand, gc_block_pos_t pos, uint8_t face,
    float cursor_x, float cursor_y, float cursor_z,
    bool inside_block, int32_t sequence) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_use_item(int32_t hand, int32_t sequence) noexcept;

[[nodiscard]] std::vector<uint8_t> encode_confirm_teleport(int32_t teleport_id) noexcept;

// ---------------------------------------------------------------------------
// Packet decoder — returns nullopt on malformed/unknown
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<DecodedPacket> decode_play_packet(
    int32_t packet_id, const uint8_t* payload, size_t len) noexcept;

[[nodiscard]] std::optional<PktLoginSuccess> decode_login_success(
    const uint8_t* payload, size_t len) noexcept;

} // namespace mc

#endif // __cplusplus

// ---------------------------------------------------------------------------
// C ABI exports
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_protocol_t mc_protocol_t;

gc_result_t gc_mc_varint_read(const uint8_t* data, size_t len,
                              int32_t* out_value, int* out_bytes) noexcept;
gc_result_t gc_mc_varint_write(uint8_t* out, size_t cap,
                               int32_t value, int* out_bytes) noexcept;

gc_result_t gc_mc_encode_handshake(const char* addr, uint16_t port,
                                   int32_t next_state,
                                   uint8_t* out, size_t cap, size_t* out_len) noexcept;

gc_result_t gc_mc_encode_login_start(const char* username, gc_uuid_t uuid,
                                     uint8_t* out, size_t cap, size_t* out_len) noexcept;

gc_result_t gc_mc_encode_keep_alive(int64_t id,
                                    uint8_t* out, size_t cap, size_t* out_len) noexcept;

gc_result_t gc_mc_encode_player_position(double x, double y, double z, int on_ground,
                                         uint8_t* out, size_t cap, size_t* out_len) noexcept;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MC_PROTOCOL_H
