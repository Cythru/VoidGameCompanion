#include "mc_protocol.h"
#include <cstring>
#include <bit>
#include <algorithm>

// ---------------------------------------------------------------------------
// Forward-declare Zig FFI for hot-path VarInt (falls back to C++ if unavailable)
// ---------------------------------------------------------------------------
extern "C" {
    // Zig-optimized varint decode — returns bytes consumed, 0 on error
    int gc_zig_varint_decode(const uint8_t* data, size_t len, int32_t* out) __attribute__((weak));
    int gc_zig_varint_encode(uint8_t* out, int32_t value) __attribute__((weak));
}

namespace mc {

// ---------------------------------------------------------------------------
// VarInt
// ---------------------------------------------------------------------------
VarIntResult read_varint(const uint8_t* data, size_t len) noexcept {
    // Try Zig hot path if available
    if (gc_zig_varint_decode) {
        int32_t val = 0;
        int bytes = gc_zig_varint_decode(data, len, &val);
        if (bytes > 0) return {val, bytes};
    }

    // C++ fallback
    int32_t result = 0;
    int shift = 0;
    size_t i = 0;

    while (i < len && i < 5) {
        uint8_t b = data[i];
        result |= static_cast<int32_t>(b & 0x7F) << shift;
        ++i;
        if ((b & 0x80) == 0) {
            return {result, static_cast<int>(i)};
        }
        shift += 7;
    }

    return {0, 0}; // error: need more data or too many bytes
}

int write_varint(uint8_t* out, int32_t value) noexcept {
    if (gc_zig_varint_encode) {
        int bytes = gc_zig_varint_encode(out, value);
        if (bytes > 0) return bytes;
    }

    auto uval = static_cast<uint32_t>(value);
    int i = 0;
    while (uval > 0x7F) {
        out[i++] = static_cast<uint8_t>((uval & 0x7F) | 0x80);
        uval >>= 7;
    }
    out[i++] = static_cast<uint8_t>(uval);
    return i;
}

int varint_size(int32_t value) noexcept {
    auto uval = static_cast<uint32_t>(value);
    if (uval == 0) return 1;
    int bytes = 0;
    while (uval > 0) {
        uval >>= 7;
        ++bytes;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// PacketBuffer
// ---------------------------------------------------------------------------
PacketBuffer::PacketBuffer() noexcept {
    buf_.reserve(256);
}

PacketBuffer::PacketBuffer(size_t reserve) noexcept {
    buf_.reserve(reserve);
}

void PacketBuffer::write_varint(int32_t v) noexcept {
    uint8_t tmp[5];
    int n = mc::write_varint(tmp, v);
    buf_.insert(buf_.end(), tmp, tmp + n);
}

void PacketBuffer::write_u8(uint8_t v) noexcept {
    buf_.push_back(v);
}

void PacketBuffer::write_u16(uint16_t v) noexcept {
    buf_.push_back(static_cast<uint8_t>(v >> 8));
    buf_.push_back(static_cast<uint8_t>(v));
}

void PacketBuffer::write_i32(int32_t v) noexcept {
    auto u = static_cast<uint32_t>(v);
    buf_.push_back(static_cast<uint8_t>(u >> 24));
    buf_.push_back(static_cast<uint8_t>(u >> 16));
    buf_.push_back(static_cast<uint8_t>(u >> 8));
    buf_.push_back(static_cast<uint8_t>(u));
}

void PacketBuffer::write_i64(int64_t v) noexcept {
    auto u = static_cast<uint64_t>(v);
    for (int i = 56; i >= 0; i -= 8) {
        buf_.push_back(static_cast<uint8_t>(u >> i));
    }
}

void PacketBuffer::write_f32(float v) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    write_i32(static_cast<int32_t>(bits));
}

void PacketBuffer::write_f64(double v) noexcept {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    write_i64(static_cast<int64_t>(bits));
}

void PacketBuffer::write_bool(bool v) noexcept {
    buf_.push_back(v ? 1 : 0);
}

void PacketBuffer::write_string(std::string_view s) noexcept {
    write_varint(static_cast<int32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void PacketBuffer::write_bytes(const uint8_t* d, size_t n) noexcept {
    buf_.insert(buf_.end(), d, d + n);
}

void PacketBuffer::write_position(int32_t x, int32_t y, int32_t z) noexcept {
    // MC packed position: x(26 bits) << 38 | z(26 bits) << 12 | y(12 bits)
    uint64_t packed = (static_cast<uint64_t>(x & 0x3FFFFFF) << 38) |
                      (static_cast<uint64_t>(z & 0x3FFFFFF) << 12) |
                      (static_cast<uint64_t>(y & 0xFFF));
    write_i64(static_cast<int64_t>(packed));
}

std::vector<uint8_t> PacketBuffer::frame(int32_t packet_id) const noexcept {
    int id_size = varint_size(packet_id);
    int payload_len = static_cast<int>(buf_.size());
    int inner_len = id_size + payload_len;
    int len_size = varint_size(inner_len);

    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(len_size + inner_len));

    // Write length prefix
    uint8_t tmp[5];
    int n = mc::write_varint(tmp, inner_len);
    result.insert(result.end(), tmp, tmp + n);

    // Write packet ID
    n = mc::write_varint(tmp, packet_id);
    result.insert(result.end(), tmp, tmp + n);

    // Write payload
    result.insert(result.end(), buf_.begin(), buf_.end());

    return result;
}

// ---------------------------------------------------------------------------
// PacketReader
// ---------------------------------------------------------------------------
PacketReader::PacketReader(const uint8_t* data, size_t len) noexcept
    : data_{data}, len_{len}, pos_{0}, error_{false} {}

int32_t PacketReader::read_varint() noexcept {
    if (error_) return 0;
    auto [val, bytes] = mc::read_varint(data_ + pos_, len_ - pos_);
    if (bytes == 0) { error_ = true; return 0; }
    pos_ += static_cast<size_t>(bytes);
    return val;
}

uint8_t PacketReader::read_u8() noexcept {
    if (error_ || !has(1)) { error_ = true; return 0; }
    return data_[pos_++];
}

uint16_t PacketReader::read_u16() noexcept {
    if (error_ || !has(2)) { error_ = true; return 0; }
    uint16_t v = (static_cast<uint16_t>(data_[pos_]) << 8) |
                  static_cast<uint16_t>(data_[pos_ + 1]);
    pos_ += 2;
    return v;
}

int32_t PacketReader::read_i32() noexcept {
    if (error_ || !has(4)) { error_ = true; return 0; }
    uint32_t v = (static_cast<uint32_t>(data_[pos_])     << 24) |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 8)  |
                  static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ += 4;
    return static_cast<int32_t>(v);
}

int64_t PacketReader::read_i64() noexcept {
    if (error_ || !has(8)) { error_ = true; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(data_[pos_ + i]);
    }
    pos_ += 8;
    return static_cast<int64_t>(v);
}

float PacketReader::read_f32() noexcept {
    int32_t bits = read_i32();
    float v;
    auto ubits = static_cast<uint32_t>(bits);
    std::memcpy(&v, &ubits, 4);
    return v;
}

double PacketReader::read_f64() noexcept {
    int64_t bits = read_i64();
    double v;
    auto ubits = static_cast<uint64_t>(bits);
    std::memcpy(&v, &ubits, 8);
    return v;
}

bool PacketReader::read_bool() noexcept {
    return read_u8() != 0;
}

std::string PacketReader::read_string() noexcept {
    int32_t len = read_varint();
    if (error_ || len < 0 || !has(static_cast<size_t>(len))) {
        error_ = true;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return s;
}

void PacketReader::read_bytes(uint8_t* out, size_t n) noexcept {
    if (error_ || !has(n)) { error_ = true; return; }
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
}

void PacketReader::skip(size_t n) noexcept {
    if (error_ || !has(n)) { error_ = true; return; }
    pos_ += n;
}

gc_block_pos_t PacketReader::read_position() noexcept {
    int64_t packed = read_i64();
    if (error_) return {0, 0, 0};

    auto uval = static_cast<uint64_t>(packed);
    int32_t x = static_cast<int32_t>(uval >> 38);
    int32_t z = static_cast<int32_t>((uval >> 12) & 0x3FFFFFF);
    int32_t y = static_cast<int32_t>(uval & 0xFFF);

    // Sign-extend 26-bit values
    if (x >= (1 << 25)) x -= (1 << 26);
    if (z >= (1 << 25)) z -= (1 << 26);
    // Sign-extend 12-bit value
    if (y >= (1 << 11)) y -= (1 << 12);

    return {x, y, z};
}

// ---------------------------------------------------------------------------
// Serverbound packet encoders
// ---------------------------------------------------------------------------
std::vector<uint8_t> encode_handshake(
    std::string_view server_addr, uint16_t port, ConnState next_state) noexcept
{
    PacketBuffer buf;
    buf.write_varint(PROTOCOL_VERSION);
    buf.write_string(server_addr);
    buf.write_u16(port);
    buf.write_varint(static_cast<int32_t>(next_state));
    return buf.frame(sb::HANDSHAKE);
}

std::vector<uint8_t> encode_login_start(
    std::string_view username, const gc_uuid_t& uuid) noexcept
{
    PacketBuffer buf;
    buf.write_string(username);
    buf.write_i64(static_cast<int64_t>(uuid.hi));
    buf.write_i64(static_cast<int64_t>(uuid.lo));
    return buf.frame(sb::LOGIN_START);
}

std::vector<uint8_t> encode_keep_alive(int64_t id) noexcept {
    PacketBuffer buf;
    buf.write_i64(id);
    return buf.frame(sb::KEEP_ALIVE);
}

std::vector<uint8_t> encode_chat_command(std::string_view command) noexcept {
    PacketBuffer buf;
    buf.write_string(command);
    // Timestamp (current time in ms — placeholder, caller should set)
    buf.write_i64(0);
    // Salt
    buf.write_i64(0);
    // No argument signatures
    buf.write_varint(0);
    // Message acknowledged
    buf.write_varint(0);
    // Bit set (empty)
    buf.write_bytes(nullptr, 0);
    return buf.frame(sb::CHAT_COMMAND);
}

std::vector<uint8_t> encode_chat_message(
    std::string_view message, int64_t timestamp, int64_t salt) noexcept
{
    PacketBuffer buf;
    buf.write_string(message);
    buf.write_i64(timestamp);
    buf.write_i64(salt);
    // No signature
    buf.write_bool(false);
    // Message acknowledged
    buf.write_varint(0);
    // Bit set (empty)
    buf.write_bytes(nullptr, 0);
    return buf.frame(sb::CHAT_MESSAGE);
}

std::vector<uint8_t> encode_player_position(
    double x, double y, double z, bool on_ground) noexcept
{
    PacketBuffer buf;
    buf.write_f64(x);
    buf.write_f64(y);
    buf.write_f64(z);
    buf.write_bool(on_ground);
    return buf.frame(sb::PLAYER_POSITION);
}

std::vector<uint8_t> encode_player_position_rotation(
    double x, double y, double z, float yaw, float pitch, bool on_ground) noexcept
{
    PacketBuffer buf;
    buf.write_f64(x);
    buf.write_f64(y);
    buf.write_f64(z);
    buf.write_f32(yaw);
    buf.write_f32(pitch);
    buf.write_bool(on_ground);
    return buf.frame(sb::PLAYER_POSITION_ROT);
}

std::vector<uint8_t> encode_player_digging(
    int32_t status, gc_block_pos_t pos, uint8_t face, int32_t sequence) noexcept
{
    PacketBuffer buf;
    buf.write_varint(status);
    buf.write_position(pos.x, pos.y, pos.z);
    buf.write_u8(face);
    buf.write_varint(sequence);
    return buf.frame(sb::PLAYER_DIGGING);
}

std::vector<uint8_t> encode_player_block_placement(
    int32_t hand, gc_block_pos_t pos, uint8_t face,
    float cursor_x, float cursor_y, float cursor_z,
    bool inside_block, int32_t sequence) noexcept
{
    PacketBuffer buf;
    buf.write_varint(hand);
    buf.write_position(pos.x, pos.y, pos.z);
    buf.write_u8(face);
    buf.write_f32(cursor_x);
    buf.write_f32(cursor_y);
    buf.write_f32(cursor_z);
    buf.write_bool(inside_block);
    buf.write_varint(sequence);
    return buf.frame(sb::PLAYER_BLOCK_PLACEMENT);
}

std::vector<uint8_t> encode_use_item(int32_t hand, int32_t sequence) noexcept {
    PacketBuffer buf;
    buf.write_varint(hand);
    buf.write_varint(sequence);
    return buf.frame(sb::USE_ITEM);
}

std::vector<uint8_t> encode_confirm_teleport(int32_t teleport_id) noexcept {
    PacketBuffer buf;
    buf.write_varint(teleport_id);
    return buf.frame(sb::CONFIRM_TELEPORT);
}

// ---------------------------------------------------------------------------
// Clientbound packet decoders
// ---------------------------------------------------------------------------
std::optional<PktLoginSuccess> decode_login_success(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktLoginSuccess pkt;
    pkt.uuid.hi = static_cast<uint64_t>(r.read_i64());
    pkt.uuid.lo = static_cast<uint64_t>(r.read_i64());
    pkt.username = r.read_string();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktKeepAlive> decode_keep_alive(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktKeepAlive pkt;
    pkt.keep_alive_id = r.read_i64();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktChatMessage> decode_chat_message(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    // 1.20.4 player chat message is complex (signatures etc.)
    // We skip sender UUID and read the message body
    r.skip(16); // sender UUID
    r.read_varint(); // index
    bool has_sig = r.read_bool();
    if (has_sig) r.skip(256); // signature bytes
    PktChatMessage pkt;
    pkt.message = r.read_string(); // message body
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktSystemChat> decode_system_chat(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktSystemChat pkt;
    pkt.content = r.read_string();
    pkt.overlay = r.read_bool();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktChunkData> decode_chunk_data(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktChunkData pkt;
    pkt.chunk_x = r.read_i32();
    pkt.chunk_z = r.read_i32();

    // Heightmap NBT (we read it as raw bytes — length-prefixed compound tag)
    // In practice we'd need a proper NBT parser; for now store raw
    // The NBT is NOT length-prefixed in protocol, it's just inline.
    // We'll skip it by reading until we hit the section data.
    // For now, store empty and read section data length
    pkt.heightmaps_nbt.clear();

    // Skip the heightmap NBT — read the compound tag
    // This is simplified; a real implementation would parse NBT
    // We skip by reading the data array length that follows
    // The packet has: chunk_x, chunk_z, NBT heightmaps, varint data_size, data[], ...

    // In 1.20.4, after NBT comes: Size (VarInt) + Data (byte array)
    // We need to properly skip NBT. Minimal approach: look for size varint
    // after a plausible NBT compound.
    // HACK: scan for a reasonable data_size varint after some offset
    // A proper implementation would integrate an NBT decoder here.

    // Skip remaining NBT by reading until we find section data size
    // Real impl would call gc_zig_nbt_skip() or similar
    int32_t data_size = r.read_varint();
    if (r.error() || data_size < 0 || !r.has(static_cast<size_t>(data_size))) {
        // Fallback: try to recover
        return std::nullopt;
    }

    pkt.chunk_sections.resize(static_cast<size_t>(data_size));
    r.read_bytes(pkt.chunk_sections.data(), static_cast<size_t>(data_size));

    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktBlockUpdate> decode_block_update(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktBlockUpdate pkt;
    pkt.position = r.read_position();
    pkt.block_state_id = r.read_varint();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktEntityPosition> decode_entity_position(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktEntityPosition pkt;
    pkt.entity_id = r.read_varint();
    pkt.delta_x = static_cast<int16_t>(r.read_u16());
    pkt.delta_y = static_cast<int16_t>(r.read_u16());
    pkt.delta_z = static_cast<int16_t>(r.read_u16());
    pkt.on_ground = r.read_bool();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktSyncPlayerPosition> decode_sync_player_position(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktSyncPlayerPosition pkt;
    pkt.x = r.read_f64();
    pkt.y = r.read_f64();
    pkt.z = r.read_f64();
    pkt.yaw = r.read_f32();
    pkt.pitch = r.read_f32();
    pkt.flags = r.read_u8();
    pkt.teleport_id = r.read_varint();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktSpawnEntity> decode_spawn_entity(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktSpawnEntity pkt;
    pkt.entity_id = r.read_varint();
    pkt.uuid.hi = static_cast<uint64_t>(r.read_i64());
    pkt.uuid.lo = static_cast<uint64_t>(r.read_i64());
    pkt.entity_type = r.read_varint();
    pkt.x = r.read_f64();
    pkt.y = r.read_f64();
    pkt.z = r.read_f64();
    // Angles are in 256ths of a full turn (stored as bytes)
    pkt.pitch = static_cast<float>(r.read_u8()) * (360.0f / 256.0f);
    pkt.yaw = static_cast<float>(r.read_u8()) * (360.0f / 256.0f);
    pkt.head_yaw = static_cast<float>(r.read_u8()) * (360.0f / 256.0f);
    pkt.data = r.read_varint();
    pkt.vel_x = static_cast<int16_t>(r.read_u16());
    pkt.vel_y = static_cast<int16_t>(r.read_u16());
    pkt.vel_z = static_cast<int16_t>(r.read_u16());
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktRemoveEntities> decode_remove_entities(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktRemoveEntities pkt;
    int32_t count = r.read_varint();
    if (r.error() || count < 0 || count > 10000) return std::nullopt;
    pkt.entity_ids.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        pkt.entity_ids.push_back(r.read_varint());
    }
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktDeathCombatEvent> decode_death_combat(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktDeathCombatEvent pkt;
    pkt.player_id = r.read_varint();
    pkt.message = r.read_string();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktSetHealth> decode_set_health(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktSetHealth pkt;
    pkt.health = r.read_f32();
    pkt.food = r.read_varint();
    pkt.saturation = r.read_f32();
    if (r.error()) return std::nullopt;
    return pkt;
}

static std::optional<PktDisconnect> decode_disconnect(
    const uint8_t* payload, size_t len) noexcept
{
    PacketReader r(payload, len);
    PktDisconnect pkt;
    pkt.reason = r.read_string();
    if (r.error()) return std::nullopt;
    return pkt;
}

// ---------------------------------------------------------------------------
// Top-level play packet dispatcher
// ---------------------------------------------------------------------------
std::optional<DecodedPacket> decode_play_packet(
    int32_t packet_id, const uint8_t* payload, size_t len) noexcept
{
    switch (packet_id) {
    case cb::KEEP_ALIVE: {
        auto p = decode_keep_alive(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::CHUNK_DATA: {
        auto p = decode_chunk_data(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    case cb::BLOCK_UPDATE: {
        auto p = decode_block_update(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::CHAT_MESSAGE: {
        auto p = decode_chat_message(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    case cb::SYSTEM_CHAT: {
        auto p = decode_system_chat(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    case cb::ENTITY_POSITION: {
        auto p = decode_entity_position(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::SYNC_PLAYER_POSITION: {
        auto p = decode_sync_player_position(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::SPAWN_ENTITY: {
        auto p = decode_spawn_entity(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::REMOVE_ENTITIES: {
        auto p = decode_remove_entities(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    case cb::DEATH_COMBAT_EVENT: {
        auto p = decode_death_combat(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    case cb::SET_HEALTH: {
        auto p = decode_set_health(payload, len);
        if (p) return DecodedPacket{*p};
        break;
    }
    case cb::DISCONNECT_PLAY: {
        auto p = decode_disconnect(payload, len);
        if (p) return DecodedPacket{std::move(*p)};
        break;
    }
    default:
        break; // Unknown packet — ignore
    }
    return std::nullopt;
}

} // namespace mc

// ---------------------------------------------------------------------------
// C ABI exports
// ---------------------------------------------------------------------------
extern "C" {

gc_result_t gc_mc_varint_read(const uint8_t* data, size_t len,
                              int32_t* out_value, int* out_bytes) noexcept
{
    if (!data || !out_value || !out_bytes) return GC_ERR_INVALID_ARG;
    auto [val, bytes] = mc::read_varint(data, len);
    if (bytes == 0) return GC_ERR_MALFORMED_PACKET;
    *out_value = val;
    *out_bytes = bytes;
    return GC_OK;
}

gc_result_t gc_mc_varint_write(uint8_t* out, size_t cap,
                               int32_t value, int* out_bytes) noexcept
{
    if (!out || !out_bytes || cap < 5) return GC_ERR_INVALID_ARG;
    *out_bytes = mc::write_varint(out, value);
    return GC_OK;
}

gc_result_t gc_mc_encode_handshake(const char* addr, uint16_t port,
                                   int32_t next_state,
                                   uint8_t* out, size_t cap, size_t* out_len) noexcept
{
    if (!addr || !out || !out_len) return GC_ERR_INVALID_ARG;
    auto pkt = mc::encode_handshake(addr, port, static_cast<mc::ConnState>(next_state));
    if (pkt.size() > cap) return GC_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, pkt.data(), pkt.size());
    *out_len = pkt.size();
    return GC_OK;
}

gc_result_t gc_mc_encode_login_start(const char* username, gc_uuid_t uuid,
                                     uint8_t* out, size_t cap, size_t* out_len) noexcept
{
    if (!username || !out || !out_len) return GC_ERR_INVALID_ARG;
    auto pkt = mc::encode_login_start(username, uuid);
    if (pkt.size() > cap) return GC_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, pkt.data(), pkt.size());
    *out_len = pkt.size();
    return GC_OK;
}

gc_result_t gc_mc_encode_keep_alive(int64_t id,
                                    uint8_t* out, size_t cap, size_t* out_len) noexcept
{
    if (!out || !out_len) return GC_ERR_INVALID_ARG;
    auto pkt = mc::encode_keep_alive(id);
    if (pkt.size() > cap) return GC_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, pkt.data(), pkt.size());
    *out_len = pkt.size();
    return GC_OK;
}

gc_result_t gc_mc_encode_player_position(double x, double y, double z, int on_ground,
                                         uint8_t* out, size_t cap, size_t* out_len) noexcept
{
    if (!out || !out_len) return GC_ERR_INVALID_ARG;
    auto pkt = mc::encode_player_position(x, y, z, on_ground != 0);
    if (pkt.size() > cap) return GC_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, pkt.data(), pkt.size());
    *out_len = pkt.size();
    return GC_OK;
}

} // extern "C"
