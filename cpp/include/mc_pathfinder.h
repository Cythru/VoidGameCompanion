#ifndef MC_PATHFINDER_H
#define MC_PATHFINDER_H

#include "gc_common.h"
#include "mc_world.h"
#include <cstdint>

#ifdef __cplusplus

#include <vector>
#include <cmath>

namespace mc {

// ---------------------------------------------------------------------------
// Movement costs
// ---------------------------------------------------------------------------
struct MoveCosts {
    float walk    = 1.0f;
    float jump    = 2.0f;
    float swim    = 3.0f;
    float fall    = 1.0f;
    float ladder  = 1.5f;
    float sprint  = 0.8f;   // slightly cheaper than walk (incentivize)
};

// ---------------------------------------------------------------------------
// Pathfinder node (internal, exposed for testing)
// ---------------------------------------------------------------------------
struct PathNode {
    int32_t x, y, z;
    float   g_cost;   // cost from start
    float   f_cost;   // g + heuristic
    int32_t parent_x, parent_y, parent_z;
    bool    has_parent;
};

// ---------------------------------------------------------------------------
// Path result
// ---------------------------------------------------------------------------
struct PathResult {
    gc_result_t              status;
    std::vector<gc_block_pos_t> waypoints;  // start -> goal
    float                    total_cost;
    int32_t                  nodes_explored;
};

// ---------------------------------------------------------------------------
// Pathfinder configuration
// ---------------------------------------------------------------------------
struct PathfinderConfig {
    int32_t   max_nodes       = 5000;    // max nodes explored before giving up
    float     max_fall_height = 4.0f;    // blocks
    float     max_jump_height = 1.0f;    // blocks (single jump)
    bool      can_swim        = true;
    bool      can_sprint      = true;
    bool      allow_diagonal  = false;   // diagonal movement on same Y
    MoveCosts costs;
};

// ---------------------------------------------------------------------------
// Move type — describes how to move between nodes
// ---------------------------------------------------------------------------
enum class MoveType : uint8_t {
    Walk,
    Jump,
    Fall,
    Swim,
    Climb,
};

// ---------------------------------------------------------------------------
// Pathfinder
// ---------------------------------------------------------------------------
class Pathfinder {
public:
    explicit Pathfinder(const World& world) noexcept;
    Pathfinder(const World& world, PathfinderConfig config) noexcept;

    // Find path from start to goal. Thread-safe (reads world with shared lock).
    [[nodiscard]] PathResult find_path(
        gc_block_pos_t start, gc_block_pos_t goal) const noexcept;

    // Setters
    void set_config(PathfinderConfig config) noexcept { config_ = config; }
    [[nodiscard]] const PathfinderConfig& config() const noexcept { return config_; }

private:
    const World&     world_;
    PathfinderConfig config_;

    // Heuristic: octile distance (3D)
    [[nodiscard]] float heuristic(
        int32_t x1, int32_t y1, int32_t z1,
        int32_t x2, int32_t y2, int32_t z2) const noexcept;

    // Check if a position is walkable (solid below, passable at feet+head)
    [[nodiscard]] bool is_standable(int32_t x, int32_t y, int32_t z) const noexcept;

    // Check if position is passable (air, water, climbable, non-solid)
    [[nodiscard]] bool is_passable(int32_t x, int32_t y, int32_t z) const noexcept;

    // Generate valid neighbor moves from a position
    struct Neighbor {
        int32_t  x, y, z;
        float    cost;
        MoveType type;
    };

    void get_neighbors(int32_t x, int32_t y, int32_t z,
                       std::vector<Neighbor>& out) const noexcept;
};

} // namespace mc

#endif // __cplusplus

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_pathfinder_t mc_pathfinder_t;
struct mc_world_t; // forward decl

mc_pathfinder_t* gc_mc_pathfinder_new(const mc_world_t* world) noexcept;
void             gc_mc_pathfinder_free(mc_pathfinder_t* pf) noexcept;

void gc_mc_pathfinder_set_max_nodes(mc_pathfinder_t* pf, int32_t max_nodes) noexcept;

// Find path, writes waypoints to out_positions (x,y,z triples).
// Returns number of waypoints written, or negative gc_result_t on error.
int32_t gc_mc_pathfinder_find(const mc_pathfinder_t* pf,
                              int32_t sx, int32_t sy, int32_t sz,
                              int32_t gx, int32_t gy, int32_t gz,
                              gc_block_pos_t* out_positions, int32_t max_out,
                              float* out_cost) noexcept;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MC_PATHFINDER_H
