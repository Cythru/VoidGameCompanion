#include "mc_pathfinder.h"
#include <queue>
#include <unordered_set>
#include <cmath>
#include <algorithm>

namespace mc {

// ---------------------------------------------------------------------------
// Position hash for closed set
// ---------------------------------------------------------------------------
struct PosHash {
    size_t operator()(const gc_block_pos_t& p) const noexcept {
        // FNV-1a style mixing
        size_t h = 0xcbf29ce484222325ULL;
        h ^= static_cast<size_t>(static_cast<uint32_t>(p.x));
        h *= 0x100000001b3ULL;
        h ^= static_cast<size_t>(static_cast<uint32_t>(p.y));
        h *= 0x100000001b3ULL;
        h ^= static_cast<size_t>(static_cast<uint32_t>(p.z));
        h *= 0x100000001b3ULL;
        return h;
    }
};

struct PosEqual {
    bool operator()(const gc_block_pos_t& a, const gc_block_pos_t& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

// ---------------------------------------------------------------------------
// Open set entry for priority queue
// ---------------------------------------------------------------------------
struct OpenEntry {
    float   f_cost;
    float   g_cost;
    int32_t x, y, z;

    // Min-heap: lowest f_cost first
    bool operator>(const OpenEntry& o) const noexcept {
        return f_cost > o.f_cost;
    }
};

// ---------------------------------------------------------------------------
// Pathfinder
// ---------------------------------------------------------------------------
Pathfinder::Pathfinder(const World& world) noexcept
    : world_{world}, config_{} {}

Pathfinder::Pathfinder(const World& world, PathfinderConfig config) noexcept
    : world_{world}, config_{config} {}

float Pathfinder::heuristic(
    int32_t x1, int32_t y1, int32_t z1,
    int32_t x2, int32_t y2, int32_t z2) const noexcept
{
    // 3D Manhattan distance (admissible for grid movement)
    float dx = static_cast<float>(std::abs(x2 - x1));
    float dy = static_cast<float>(std::abs(y2 - y1));
    float dz = static_cast<float>(std::abs(z2 - z1));
    return dx + dy + dz;
}

bool Pathfinder::is_passable(int32_t x, int32_t y, int32_t z) const noexcept {
    auto cat = world_.get_block_category(x, y, z);
    return cat == BlockCategory::Air ||
           cat == BlockCategory::Water ||
           cat == BlockCategory::Climbable ||
           cat == BlockCategory::NonSolid;
}

bool Pathfinder::is_standable(int32_t x, int32_t y, int32_t z) const noexcept {
    // Must have solid (or climbable) below, and passable at feet (y) and head (y+1)
    auto below = world_.get_block_category(x, y - 1, z);
    bool ground_ok = (below == BlockCategory::Solid || below == BlockCategory::Climbable);

    // Water standing is also valid if swimming is allowed
    if (!ground_ok && config_.can_swim && below == BlockCategory::Water) {
        ground_ok = true;
    }

    if (!ground_ok) return false;

    return is_passable(x, y, z) && is_passable(x, y + 1, z);
}

void Pathfinder::get_neighbors(
    int32_t x, int32_t y, int32_t z,
    std::vector<Neighbor>& out) const noexcept
{
    out.clear();

    // 4 cardinal directions (same Y level — walking)
    static constexpr int dx[] = {1, -1, 0, 0};
    static constexpr int dz[] = {0, 0, 1, -1};

    for (int i = 0; i < 4; ++i) {
        int32_t nx = x + dx[i];
        int32_t nz = z + dz[i];

        // Walk: same level
        if (is_standable(nx, y, nz)) {
            out.push_back({nx, y, nz, config_.costs.walk, MoveType::Walk});
            continue;
        }

        // Jump up: one block higher
        if (is_standable(nx, y + 1, nz) &&
            is_passable(x, y + 2, z) && // headroom above current
            is_passable(nx, y + 2, nz))  // headroom at destination
        {
            out.push_back({nx, y + 1, nz, config_.costs.jump, MoveType::Jump});
            continue;
        }

        // Fall down: check up to max_fall_height blocks down
        int max_fall = static_cast<int>(config_.max_fall_height);
        for (int drop = 1; drop <= max_fall; ++drop) {
            int32_t ny = y - drop;
            if (is_standable(nx, ny, nz)) {
                // Check that the entire fall path is passable
                bool clear = true;
                for (int fy = 0; fy < drop && clear; ++fy) {
                    if (!is_passable(nx, y - fy, nz)) clear = false;
                    if (!is_passable(nx, y - fy + 1, nz)) clear = false;
                }
                if (clear) {
                    float cost = config_.costs.fall * static_cast<float>(drop);
                    out.push_back({nx, ny, nz, cost, MoveType::Fall});
                }
                break; // Stop at first solid ground
            }
        }
    }

    // Vertical: ladder/vine climbing (up and down)
    auto here_cat = world_.get_block_category(x, y, z);
    auto above_cat = world_.get_block_category(x, y + 1, z);

    if (here_cat == BlockCategory::Climbable || above_cat == BlockCategory::Climbable) {
        // Climb up
        if (is_passable(x, y + 2, z) && is_passable(x, y + 3, z)) {
            out.push_back({x, y + 1, z, config_.costs.ladder, MoveType::Climb});
        }
    }

    auto below_cat = world_.get_block_category(x, y - 1, z);
    if (here_cat == BlockCategory::Climbable || below_cat == BlockCategory::Climbable) {
        // Climb down
        if (is_passable(x, y - 1, z)) {
            out.push_back({x, y - 1, z, config_.costs.ladder, MoveType::Climb});
        }
    }

    // Swimming (if in water)
    if (config_.can_swim && here_cat == BlockCategory::Water) {
        // Can move in all 6 directions in water
        static constexpr int sdx[] = {1, -1, 0, 0, 0, 0};
        static constexpr int sdy[] = {0, 0, 0, 0, 1, -1};
        static constexpr int sdz[] = {0, 0, 1, -1, 0, 0};

        for (int i = 0; i < 6; ++i) {
            int32_t nx2 = x + sdx[i];
            int32_t ny2 = y + sdy[i];
            int32_t nz2 = z + sdz[i];

            auto ncat = world_.get_block_category(nx2, ny2, nz2);
            if (ncat == BlockCategory::Water || ncat == BlockCategory::Air) {
                out.push_back({nx2, ny2, nz2, config_.costs.swim, MoveType::Swim});
            }
        }
    }
}

PathResult Pathfinder::find_path(
    gc_block_pos_t start, gc_block_pos_t goal) const noexcept
{
    PathResult result;
    result.status = GC_ERR_NO_PATH;
    result.total_cost = 0.0f;
    result.nodes_explored = 0;

    // Quick check: start and goal should be standable
    // (relax for goal — might be approaching a target position)
    if (!is_standable(start.x, start.y, start.z)) {
        // Try one block down (common: player pos is feet, block pos is below)
        if (is_standable(start.x, start.y - 1, start.z)) {
            start.y -= 1;
        } else {
            result.status = GC_ERR_INVALID_ARG;
            return result;
        }
    }

    // Same position
    if (start.x == goal.x && start.y == goal.y && start.z == goal.z) {
        result.status = GC_OK;
        result.waypoints.push_back(start);
        return result;
    }

    // A* search
    using OpenQueue = std::priority_queue<OpenEntry, std::vector<OpenEntry>,
                                          std::greater<OpenEntry>>;
    OpenQueue open;

    // Closed set
    std::unordered_set<gc_block_pos_t, PosHash, PosEqual> closed;

    // Parent map for path reconstruction
    struct ParentInfo {
        int32_t px, py, pz;
        float   g_cost;
    };
    std::unordered_map<gc_block_pos_t, ParentInfo, PosHash, PosEqual> parents;

    float h = heuristic(start.x, start.y, start.z, goal.x, goal.y, goal.z);
    open.push({h, 0.0f, start.x, start.y, start.z});
    parents[start] = {start.x, start.y, start.z, 0.0f};

    std::vector<Neighbor> neighbors;
    neighbors.reserve(16);

    while (!open.empty() && result.nodes_explored < config_.max_nodes) {
        auto current = open.top();
        open.pop();

        gc_block_pos_t cur_pos = {current.x, current.y, current.z};

        // Already visited?
        if (closed.contains(cur_pos)) continue;
        closed.insert(cur_pos);
        ++result.nodes_explored;

        // Goal reached?
        if (current.x == goal.x && current.y == goal.y && current.z == goal.z) {
            // Reconstruct path
            result.status = GC_OK;
            result.total_cost = current.g_cost;

            std::vector<gc_block_pos_t> path;
            gc_block_pos_t pos = goal;
            int safety = config_.max_nodes + 1; // prevent infinite loop

            while (--safety > 0) {
                path.push_back(pos);
                if (pos.x == start.x && pos.y == start.y && pos.z == start.z) break;

                auto it = parents.find(pos);
                if (it == parents.end()) break;
                pos = {it->second.px, it->second.py, it->second.pz};
            }

            std::reverse(path.begin(), path.end());
            result.waypoints = std::move(path);
            return result;
        }

        // Expand neighbors
        get_neighbors(current.x, current.y, current.z, neighbors);

        for (const auto& nb : neighbors) {
            gc_block_pos_t nb_pos = {nb.x, nb.y, nb.z};
            if (closed.contains(nb_pos)) continue;

            float new_g = current.g_cost + nb.cost;

            auto it = parents.find(nb_pos);
            if (it != parents.end() && new_g >= it->second.g_cost) {
                continue; // Already found a better path
            }

            parents[nb_pos] = {current.x, current.y, current.z, new_g};
            float new_h = heuristic(nb.x, nb.y, nb.z, goal.x, goal.y, goal.z);
            open.push({new_g + new_h, new_g, nb.x, nb.y, nb.z});
        }
    }

    // If we exhausted max_nodes, status remains GC_ERR_NO_PATH
    return result;
}

} // namespace mc
