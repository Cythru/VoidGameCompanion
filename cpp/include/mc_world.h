#ifndef MC_WORLD_H
#define MC_WORLD_H

#include "gc_common.h"
#include "mc_protocol.h"
#include <cstdint>

#ifdef __cplusplus

#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <cmath>

namespace mc {

// ---------------------------------------------------------------------------
// Block state constants (simplified — full registry is huge)
// ---------------------------------------------------------------------------
enum class BlockCategory : uint8_t {
    Air       = 0,
    Solid     = 1,
    Water     = 2,
    Lava      = 3,
    Climbable = 4, // ladders, vines
    NonSolid  = 5, // flowers, grass, torches, etc.
};

[[nodiscard]] BlockCategory categorize_block(int32_t block_state_id) noexcept;
[[nodiscard]] bool is_solid(int32_t block_state_id) noexcept;
[[nodiscard]] bool is_walkable(int32_t block_state_id) noexcept;

// ---------------------------------------------------------------------------
// Chunk section (16x16x16 = 4096 blocks)
// ---------------------------------------------------------------------------
class ChunkSection {
public:
    ChunkSection() noexcept;

    void set_block(int x, int y, int z, int32_t state_id) noexcept;
    [[nodiscard]] int32_t get_block(int x, int y, int z) const noexcept;
    [[nodiscard]] bool is_empty() const noexcept { return block_count_ == 0; }

    // Decode from MC protocol section format
    // Returns GC_OK on success
    gc_result_t decode_from(const uint8_t* data, size_t len, size_t* bytes_consumed) noexcept;

private:
    static constexpr size_t SECTION_SIZE = 16 * 16 * 16;
    std::array<int32_t, SECTION_SIZE> blocks_;
    int32_t block_count_;

    static size_t index(int x, int y, int z) noexcept {
        return static_cast<size_t>((y << 8) | (z << 4) | x);
    }
};

// ---------------------------------------------------------------------------
// Chunk column (24 sections for -64 to 319, MC 1.18+ world height)
// ---------------------------------------------------------------------------
class ChunkColumn {
public:
    static constexpr int SECTIONS       = 24;
    static constexpr int MIN_Y          = -64;
    static constexpr int MAX_Y          = 319;
    static constexpr int SECTION_HEIGHT = 16;

    ChunkColumn() noexcept;

    void set_block(int x, int y, int z, int32_t state_id) noexcept;
    [[nodiscard]] int32_t get_block(int x, int y, int z) const noexcept;

    [[nodiscard]] ChunkSection* section_at(int section_y) noexcept;
    [[nodiscard]] const ChunkSection* section_at(int section_y) const noexcept;

    // Load from decoded chunk data
    gc_result_t load_sections(const uint8_t* data, size_t len) noexcept;

private:
    std::array<ChunkSection, SECTIONS> sections_;

    static int section_index(int y) noexcept {
        return (y - MIN_Y) >> 4;
    }
};

// ---------------------------------------------------------------------------
// Chunk coordinate key for hashing
// ---------------------------------------------------------------------------
struct ChunkCoord {
    int32_t x;
    int32_t z;

    bool operator==(const ChunkCoord& o) const noexcept {
        return x == o.x && z == o.z;
    }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const noexcept {
        // Cantor-style hash
        auto h1 = static_cast<size_t>(static_cast<uint32_t>(c.x));
        auto h2 = static_cast<size_t>(static_cast<uint32_t>(c.z));
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// ---------------------------------------------------------------------------
// Entity
// ---------------------------------------------------------------------------
struct Entity {
    int32_t   id;
    gc_uuid_t uuid;
    int32_t   entity_type;
    double    x, y, z;
    float     yaw, pitch;
    float     health;      // -1 if unknown
    bool      alive;
};

// ---------------------------------------------------------------------------
// Player info
// ---------------------------------------------------------------------------
struct PlayerInfo {
    gc_uuid_t   uuid;
    std::string  name;
    double       x, y, z;
    float        yaw, pitch;
    float        health;
    int32_t      food;
    float        saturation;
    bool         on_ground;
};

// ---------------------------------------------------------------------------
// World — thread-safe world state
// ---------------------------------------------------------------------------
class World {
public:
    World() noexcept;

    // -- Chunk operations (write-locked) --
    gc_result_t load_chunk(int32_t chunk_x, int32_t chunk_z,
                           const uint8_t* section_data, size_t len) noexcept;
    void unload_chunk(int32_t chunk_x, int32_t chunk_z) noexcept;

    // -- Block access (read-locked) --
    [[nodiscard]] int32_t get_block(int32_t x, int32_t y, int32_t z) const noexcept;
    void set_block(int32_t x, int32_t y, int32_t z, int32_t state_id) noexcept;
    [[nodiscard]] bool is_chunk_loaded(int32_t chunk_x, int32_t chunk_z) const noexcept;
    [[nodiscard]] BlockCategory get_block_category(int32_t x, int32_t y, int32_t z) const noexcept;

    // -- Entity tracking --
    void spawn_entity(const Entity& ent) noexcept;
    void remove_entity(int32_t entity_id) noexcept;
    void update_entity_position(int32_t entity_id,
                                double dx, double dy, double dz) noexcept;
    void set_entity_position(int32_t entity_id,
                             double x, double y, double z) noexcept;
    [[nodiscard]] std::optional<Entity> get_entity(int32_t entity_id) const noexcept;
    [[nodiscard]] std::vector<Entity> get_entities_in_radius(
        double cx, double cy, double cz, double radius) const noexcept;

    // -- Player state --
    void update_player_position(double x, double y, double z,
                                float yaw, float pitch, bool on_ground) noexcept;
    void update_player_health(float health, int32_t food, float saturation) noexcept;
    void set_player_info(const std::string& name, const gc_uuid_t& uuid) noexcept;
    [[nodiscard]] PlayerInfo get_player() const noexcept;

private:
    mutable std::shared_mutex chunk_mutex_;
    std::unordered_map<ChunkCoord, ChunkColumn, ChunkCoordHash> chunks_;

    mutable std::shared_mutex entity_mutex_;
    std::unordered_map<int32_t, Entity> entities_;

    mutable std::shared_mutex player_mutex_;
    PlayerInfo player_;
};

} // namespace mc

#endif // __cplusplus

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_world_t mc_world_t;

mc_world_t* gc_mc_world_new(void) noexcept;
void        gc_mc_world_free(mc_world_t* w) noexcept;

gc_result_t gc_mc_world_load_chunk(mc_world_t* w, int32_t cx, int32_t cz,
                                   const uint8_t* data, size_t len) noexcept;
void        gc_mc_world_unload_chunk(mc_world_t* w, int32_t cx, int32_t cz) noexcept;

int32_t     gc_mc_world_get_block(const mc_world_t* w,
                                  int32_t x, int32_t y, int32_t z) noexcept;
void        gc_mc_world_set_block(mc_world_t* w,
                                  int32_t x, int32_t y, int32_t z,
                                  int32_t state_id) noexcept;

gc_result_t gc_mc_world_spawn_entity(mc_world_t* w, int32_t id, gc_uuid_t uuid,
                                     int32_t etype, double x, double y, double z) noexcept;
void        gc_mc_world_remove_entity(mc_world_t* w, int32_t id) noexcept;

int32_t     gc_mc_world_get_entities_in_radius(const mc_world_t* w,
                                               double cx, double cy, double cz,
                                               double radius,
                                               int32_t* out_ids, int32_t max_out) noexcept;

void        gc_mc_world_update_player(mc_world_t* w,
                                      double x, double y, double z,
                                      float yaw, float pitch, int on_ground) noexcept;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MC_WORLD_H
