#include "mc_world.h"
#include <cstring>
#include <algorithm>
#include <mutex>
#include <cmath>

// ---------------------------------------------------------------------------
// Zig FFI for chunk decoding (optional hot path)
// ---------------------------------------------------------------------------
extern "C" {
    // Zig-optimized chunk section decoder
    // Returns bytes consumed, 0 on error
    int gc_zig_chunk_decode_section(
        const uint8_t* data, size_t len,
        int32_t* out_blocks, size_t block_count) __attribute__((weak));
}

namespace mc {

// ---------------------------------------------------------------------------
// Block categorization — simplified for common block state IDs
// MC 1.20.4 has thousands of block states. This covers the important ones.
// A full implementation would load the block registry from data files.
// ---------------------------------------------------------------------------
BlockCategory categorize_block(int32_t block_state_id) noexcept {
    if (block_state_id == 0) return BlockCategory::Air;

    // Water: block states ~80-95 (flowing + still)
    if (block_state_id >= 80 && block_state_id <= 95) return BlockCategory::Water;

    // Lava: block states ~96-111
    if (block_state_id >= 96 && block_state_id <= 111) return BlockCategory::Lava;

    // Ladders (~4268-4271), vines (~5765-5780)
    if ((block_state_id >= 4268 && block_state_id <= 4271) ||
        (block_state_id >= 5765 && block_state_id <= 5780)) {
        return BlockCategory::Climbable;
    }

    // Air variants: cave_air (12959), void_air (12960)
    if (block_state_id == 12959 || block_state_id == 12960) return BlockCategory::Air;

    // Non-solid: tall grass, flowers, torches, signs, etc.
    // This is a rough heuristic — ranges vary by version
    // Torches: ~2067-2072
    // Flowers: various ranges
    // For now, treat everything else as solid
    // A proper implementation loads the block registry at startup

    return BlockCategory::Solid;
}

bool is_solid(int32_t block_state_id) noexcept {
    return categorize_block(block_state_id) == BlockCategory::Solid;
}

bool is_walkable(int32_t block_state_id) noexcept {
    auto cat = categorize_block(block_state_id);
    return cat == BlockCategory::Air || cat == BlockCategory::NonSolid ||
           cat == BlockCategory::Water || cat == BlockCategory::Climbable;
}

// ---------------------------------------------------------------------------
// ChunkSection
// ---------------------------------------------------------------------------
ChunkSection::ChunkSection() noexcept : block_count_{0} {
    blocks_.fill(0); // air
}

void ChunkSection::set_block(int x, int y, int z, int32_t state_id) noexcept {
    if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16) return;
    size_t idx = index(x, y, z);
    int32_t old = blocks_[idx];

    if (old == 0 && state_id != 0) ++block_count_;
    else if (old != 0 && state_id == 0) --block_count_;

    blocks_[idx] = state_id;
}

int32_t ChunkSection::get_block(int x, int y, int z) const noexcept {
    if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16) return 0;
    return blocks_[index(x, y, z)];
}

gc_result_t ChunkSection::decode_from(
    const uint8_t* data, size_t len, size_t* bytes_consumed) noexcept
{
    if (!data || len == 0 || !bytes_consumed) return GC_ERR_INVALID_ARG;

    // Try Zig hot path first
    if (gc_zig_chunk_decode_section) {
        int consumed = gc_zig_chunk_decode_section(
            data, len, blocks_.data(), SECTION_SIZE);
        if (consumed > 0) {
            *bytes_consumed = static_cast<size_t>(consumed);
            // Recount non-air blocks
            block_count_ = 0;
            for (auto b : blocks_) {
                if (b != 0) ++block_count_;
            }
            return GC_OK;
        }
    }

    // C++ fallback: parse MC section format
    // Format: block_count (short), palette (bits_per_entry byte, palette varint array),
    //         data (long array)
    mc::PacketReader r(data, len);

    int16_t bc = static_cast<int16_t>(r.read_u16()); // block count (unused for us)
    (void)bc;

    // Block states paletted container
    uint8_t bits_per_entry = r.read_u8();
    if (r.error()) return GC_ERR_MALFORMED_PACKET;

    if (bits_per_entry == 0) {
        // Single-valued: entire section is one block type
        int32_t single_val = r.read_varint();
        int32_t data_array_len = r.read_varint(); // should be 0
        if (r.error()) return GC_ERR_MALFORMED_PACKET;

        blocks_.fill(single_val);
        block_count_ = (single_val != 0) ? static_cast<int32_t>(SECTION_SIZE) : 0;

        // Skip data array (should be empty)
        r.skip(static_cast<size_t>(data_array_len) * 8);
    } else {
        // Indirect or direct palette
        std::vector<int32_t> palette;

        if (bits_per_entry <= 8) {
            // Indirect palette
            int32_t palette_len = r.read_varint();
            if (r.error() || palette_len < 0 || palette_len > 4096) {
                return GC_ERR_MALFORMED_PACKET;
            }
            palette.resize(static_cast<size_t>(palette_len));
            for (int32_t i = 0; i < palette_len; ++i) {
                palette[i] = r.read_varint();
            }
        }
        // else: direct palette (bits_per_entry = 15 for blocks), no palette array

        // Data array of packed longs
        int32_t data_array_len = r.read_varint();
        if (r.error() || data_array_len < 0) return GC_ERR_MALFORMED_PACKET;

        uint8_t actual_bpe = bits_per_entry;
        if (actual_bpe < 4) actual_bpe = 4; // MC minimum for blocks

        int entries_per_long = 64 / actual_bpe;
        uint64_t mask = (1ULL << actual_bpe) - 1;

        block_count_ = 0;
        size_t block_idx = 0;

        for (int32_t li = 0; li < data_array_len && !r.error(); ++li) {
            uint64_t packed = static_cast<uint64_t>(r.read_i64());
            for (int ei = 0; ei < entries_per_long && block_idx < SECTION_SIZE; ++ei) {
                uint32_t val = static_cast<uint32_t>((packed >> (ei * actual_bpe)) & mask);

                int32_t state_id;
                if (!palette.empty()) {
                    state_id = (val < palette.size()) ? palette[val] : 0;
                } else {
                    state_id = static_cast<int32_t>(val);
                }

                blocks_[block_idx++] = state_id;
                if (state_id != 0) ++block_count_;
            }
        }

        // Fill remaining with air
        while (block_idx < SECTION_SIZE) {
            blocks_[block_idx++] = 0;
        }
    }

    if (r.error()) return GC_ERR_MALFORMED_PACKET;

    // Skip biome paletted container (same format but 4x4x4 = 64 entries)
    uint8_t biome_bpe = r.read_u8();
    if (r.error()) return GC_ERR_MALFORMED_PACKET;

    if (biome_bpe == 0) {
        r.read_varint(); // single value
        int32_t biome_data_len = r.read_varint();
        r.skip(static_cast<size_t>(biome_data_len) * 8);
    } else {
        if (biome_bpe <= 3) {
            int32_t biome_palette_len = r.read_varint();
            for (int32_t i = 0; i < biome_palette_len && !r.error(); ++i) {
                r.read_varint();
            }
        }
        int32_t biome_data_len = r.read_varint();
        r.skip(static_cast<size_t>(biome_data_len) * 8);
    }

    if (r.error()) return GC_ERR_MALFORMED_PACKET;

    *bytes_consumed = len - r.remaining();
    return GC_OK;
}

// ---------------------------------------------------------------------------
// ChunkColumn
// ---------------------------------------------------------------------------
ChunkColumn::ChunkColumn() noexcept = default;

void ChunkColumn::set_block(int x, int y, int z, int32_t state_id) noexcept {
    int si = section_index(y);
    if (si < 0 || si >= SECTIONS) return;
    sections_[si].set_block(x & 0xF, (y - MIN_Y) & 0xF, z & 0xF, state_id);
}

int32_t ChunkColumn::get_block(int x, int y, int z) const noexcept {
    int si = section_index(y);
    if (si < 0 || si >= SECTIONS) return 0;
    return sections_[si].get_block(x & 0xF, (y - MIN_Y) & 0xF, z & 0xF);
}

ChunkSection* ChunkColumn::section_at(int section_y) noexcept {
    int si = section_y - (MIN_Y >> 4);
    if (si < 0 || si >= SECTIONS) return nullptr;
    return &sections_[si];
}

const ChunkSection* ChunkColumn::section_at(int section_y) const noexcept {
    int si = section_y - (MIN_Y >> 4);
    if (si < 0 || si >= SECTIONS) return nullptr;
    return &sections_[si];
}

gc_result_t ChunkColumn::load_sections(const uint8_t* data, size_t len) noexcept {
    if (!data) return GC_ERR_INVALID_ARG;

    size_t offset = 0;
    for (int si = 0; si < SECTIONS && offset < len; ++si) {
        size_t consumed = 0;
        gc_result_t rc = sections_[si].decode_from(
            data + offset, len - offset, &consumed);
        if (rc != GC_OK) return rc;
        offset += consumed;
    }

    return GC_OK;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
World::World() noexcept : player_{} {
    player_.health = 20.0f;
    player_.food = 20;
    player_.saturation = 5.0f;
}

gc_result_t World::load_chunk(int32_t chunk_x, int32_t chunk_z,
                              const uint8_t* section_data, size_t len) noexcept
{
    if (!section_data) return GC_ERR_INVALID_ARG;

    ChunkColumn col;
    gc_result_t rc = col.load_sections(section_data, len);
    if (rc != GC_OK) return rc;

    std::unique_lock lock(chunk_mutex_);
    chunks_[{chunk_x, chunk_z}] = std::move(col);
    return GC_OK;
}

void World::unload_chunk(int32_t chunk_x, int32_t chunk_z) noexcept {
    std::unique_lock lock(chunk_mutex_);
    chunks_.erase({chunk_x, chunk_z});
}

int32_t World::get_block(int32_t x, int32_t y, int32_t z) const noexcept {
    int32_t cx = x >> 4;
    int32_t cz = z >> 4;

    std::shared_lock lock(chunk_mutex_);
    auto it = chunks_.find({cx, cz});
    if (it == chunks_.end()) return 0; // unloaded = air
    return it->second.get_block(x, y, z);
}

void World::set_block(int32_t x, int32_t y, int32_t z, int32_t state_id) noexcept {
    int32_t cx = x >> 4;
    int32_t cz = z >> 4;

    std::unique_lock lock(chunk_mutex_);
    auto it = chunks_.find({cx, cz});
    if (it == chunks_.end()) return; // can't set in unloaded chunk
    it->second.set_block(x, y, z, state_id);
}

bool World::is_chunk_loaded(int32_t chunk_x, int32_t chunk_z) const noexcept {
    std::shared_lock lock(chunk_mutex_);
    return chunks_.contains({chunk_x, chunk_z});
}

BlockCategory World::get_block_category(int32_t x, int32_t y, int32_t z) const noexcept {
    return categorize_block(get_block(x, y, z));
}

void World::spawn_entity(const Entity& ent) noexcept {
    std::unique_lock lock(entity_mutex_);
    entities_[ent.id] = ent;
}

void World::remove_entity(int32_t entity_id) noexcept {
    std::unique_lock lock(entity_mutex_);
    entities_.erase(entity_id);
}

void World::update_entity_position(int32_t entity_id,
                                   double dx, double dy, double dz) noexcept
{
    std::unique_lock lock(entity_mutex_);
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) {
        it->second.x += dx;
        it->second.y += dy;
        it->second.z += dz;
    }
}

void World::set_entity_position(int32_t entity_id,
                                double x, double y, double z) noexcept
{
    std::unique_lock lock(entity_mutex_);
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) {
        it->second.x = x;
        it->second.y = y;
        it->second.z = z;
    }
}

std::optional<Entity> World::get_entity(int32_t entity_id) const noexcept {
    std::shared_lock lock(entity_mutex_);
    auto it = entities_.find(entity_id);
    if (it == entities_.end()) return std::nullopt;
    return it->second;
}

std::vector<Entity> World::get_entities_in_radius(
    double cx, double cy, double cz, double radius) const noexcept
{
    double r2 = radius * radius;
    std::vector<Entity> result;

    std::shared_lock lock(entity_mutex_);
    for (const auto& [id, ent] : entities_) {
        double dx = ent.x - cx;
        double dy = ent.y - cy;
        double dz = ent.z - cz;
        if (dx*dx + dy*dy + dz*dz <= r2) {
            result.push_back(ent);
        }
    }
    return result;
}

void World::update_player_position(double x, double y, double z,
                                   float yaw, float pitch, bool on_ground) noexcept
{
    std::unique_lock lock(player_mutex_);
    player_.x = x;
    player_.y = y;
    player_.z = z;
    player_.yaw = yaw;
    player_.pitch = pitch;
    player_.on_ground = on_ground;
}

void World::update_player_health(float health, int32_t food, float saturation) noexcept {
    std::unique_lock lock(player_mutex_);
    player_.health = health;
    player_.food = food;
    player_.saturation = saturation;
}

void World::set_player_info(const std::string& name, const gc_uuid_t& uuid) noexcept {
    std::unique_lock lock(player_mutex_);
    player_.name = name;
    player_.uuid = uuid;
}

PlayerInfo World::get_player() const noexcept {
    std::shared_lock lock(player_mutex_);
    return player_;
}

} // namespace mc
