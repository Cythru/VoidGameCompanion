#include "gc_common.h"
#include "mc_protocol.h"
#include "mc_world.h"
#include "mc_pathfinder.h"
#include "action_queue.h"
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Opaque handle wrappers
// ---------------------------------------------------------------------------

struct mc_world_t {
    mc::World world;
};

struct mc_pathfinder_t {
    mc::Pathfinder pathfinder;

    explicit mc_pathfinder_t(const mc::World& w) noexcept : pathfinder(w) {}
};

struct mc_action_queue_t {
    mc::ActionQueue queue;
    uint32_t next_seq;

    mc_action_queue_t() noexcept : next_seq{1} {}
};

// ---------------------------------------------------------------------------
// World FFI
// ---------------------------------------------------------------------------
extern "C" {

mc_world_t* gc_mc_world_new(void) noexcept {
    auto* w = new (std::nothrow) mc_world_t;
    return w;
}

void gc_mc_world_free(mc_world_t* w) noexcept {
    delete w;
}

gc_result_t gc_mc_world_load_chunk(mc_world_t* w, int32_t cx, int32_t cz,
                                   const uint8_t* data, size_t len) noexcept
{
    if (!w) return GC_ERR_INVALID_ARG;
    return w->world.load_chunk(cx, cz, data, len);
}

void gc_mc_world_unload_chunk(mc_world_t* w, int32_t cx, int32_t cz) noexcept {
    if (w) w->world.unload_chunk(cx, cz);
}

int32_t gc_mc_world_get_block(const mc_world_t* w,
                              int32_t x, int32_t y, int32_t z) noexcept
{
    if (!w) return 0;
    return w->world.get_block(x, y, z);
}

void gc_mc_world_set_block(mc_world_t* w,
                           int32_t x, int32_t y, int32_t z,
                           int32_t state_id) noexcept
{
    if (w) w->world.set_block(x, y, z, state_id);
}

gc_result_t gc_mc_world_spawn_entity(mc_world_t* w, int32_t id, gc_uuid_t uuid,
                                     int32_t etype,
                                     double x, double y, double z) noexcept
{
    if (!w) return GC_ERR_INVALID_ARG;

    mc::Entity ent;
    ent.id = id;
    ent.uuid = uuid;
    ent.entity_type = etype;
    ent.x = x;
    ent.y = y;
    ent.z = z;
    ent.yaw = 0.0f;
    ent.pitch = 0.0f;
    ent.health = -1.0f;
    ent.alive = true;

    w->world.spawn_entity(ent);
    return GC_OK;
}

void gc_mc_world_remove_entity(mc_world_t* w, int32_t id) noexcept {
    if (w) w->world.remove_entity(id);
}

int32_t gc_mc_world_get_entities_in_radius(const mc_world_t* w,
                                           double cx, double cy, double cz,
                                           double radius,
                                           int32_t* out_ids, int32_t max_out) noexcept
{
    if (!w || !out_ids || max_out <= 0) return 0;

    auto entities = w->world.get_entities_in_radius(cx, cy, cz, radius);
    int32_t count = static_cast<int32_t>(
        std::min(entities.size(), static_cast<size_t>(max_out)));

    for (int32_t i = 0; i < count; ++i) {
        out_ids[i] = entities[i].id;
    }
    return count;
}

void gc_mc_world_update_player(mc_world_t* w,
                               double x, double y, double z,
                               float yaw, float pitch, int on_ground) noexcept
{
    if (w) w->world.update_player_position(x, y, z, yaw, pitch, on_ground != 0);
}

// ---------------------------------------------------------------------------
// Pathfinder FFI
// ---------------------------------------------------------------------------

mc_pathfinder_t* gc_mc_pathfinder_new(const mc_world_t* world) noexcept {
    if (!world) return nullptr;
    auto* pf = new (std::nothrow) mc_pathfinder_t(world->world);
    return pf;
}

void gc_mc_pathfinder_free(mc_pathfinder_t* pf) noexcept {
    delete pf;
}

void gc_mc_pathfinder_set_max_nodes(mc_pathfinder_t* pf, int32_t max_nodes) noexcept {
    if (!pf || max_nodes <= 0) return;
    auto cfg = pf->pathfinder.config();
    cfg.max_nodes = max_nodes;
    pf->pathfinder.set_config(cfg);
}

int32_t gc_mc_pathfinder_find(const mc_pathfinder_t* pf,
                              int32_t sx, int32_t sy, int32_t sz,
                              int32_t gx, int32_t gy, int32_t gz,
                              gc_block_pos_t* out_positions, int32_t max_out,
                              float* out_cost) noexcept
{
    if (!pf || !out_positions || max_out <= 0) return GC_ERR_INVALID_ARG;

    gc_block_pos_t start = {sx, sy, sz};
    gc_block_pos_t goal  = {gx, gy, gz};

    auto result = pf->pathfinder.find_path(start, goal);

    if (result.status != GC_OK) {
        return result.status; // negative error code
    }

    int32_t count = static_cast<int32_t>(
        std::min(result.waypoints.size(), static_cast<size_t>(max_out)));

    for (int32_t i = 0; i < count; ++i) {
        out_positions[i] = result.waypoints[i];
    }

    if (out_cost) *out_cost = result.total_cost;

    return count;
}

// ---------------------------------------------------------------------------
// Action Queue FFI
// ---------------------------------------------------------------------------

mc_action_queue_t* gc_mc_action_queue_new(void) noexcept {
    return new (std::nothrow) mc_action_queue_t;
}

void gc_mc_action_queue_free(mc_action_queue_t* q) noexcept {
    delete q;
}

gc_result_t gc_mc_action_queue_push_move(mc_action_queue_t* q, uint32_t seq,
                                         double x, double y, double z, float speed) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_move_to(seq ? seq : q->next_seq++, x, y, z, speed);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_mine(mc_action_queue_t* q, uint32_t seq,
                                         int32_t x, int32_t y, int32_t z, uint8_t face) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_mine_block(seq ? seq : q->next_seq++, x, y, z, face);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_place(mc_action_queue_t* q, uint32_t seq,
                                          int32_t x, int32_t y, int32_t z,
                                          uint8_t face, int32_t block_state) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_place_block(seq ? seq : q->next_seq++, x, y, z, face, block_state);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_attack(mc_action_queue_t* q, uint32_t seq,
                                           int32_t entity_id) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_attack_entity(seq ? seq : q->next_seq++, entity_id);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_chat(mc_action_queue_t* q, uint32_t seq,
                                         const char* text, uint32_t len) noexcept
{
    if (!q || !text) return GC_ERR_INVALID_ARG;
    auto action = mc::make_chat(seq ? seq : q->next_seq++, text, len);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_use_item(mc_action_queue_t* q, uint32_t seq,
                                             int32_t hand) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_use_item(seq ? seq : q->next_seq++, hand);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

gc_result_t gc_mc_action_queue_push_craft(mc_action_queue_t* q, uint32_t seq,
                                          int32_t recipe_id, uint8_t count) noexcept
{
    if (!q) return GC_ERR_INVALID_ARG;
    auto action = mc::make_craft(seq ? seq : q->next_seq++, recipe_id, count);
    return q->queue.push(action) ? GC_OK : GC_ERR_QUEUE_FULL;
}

uint32_t gc_mc_action_queue_pop(mc_action_queue_t* q,
                                uint32_t* out_seq,
                                double* out_x, double* out_y, double* out_z,
                                int32_t* out_i1, int32_t* out_i2,
                                char* out_text, uint32_t text_cap) noexcept
{
    if (!q) return 0;

    mc::ActionData action;
    if (!q->queue.pop(action)) return 0;

    if (out_seq) *out_seq = action.sequence_id;

    switch (action.type) {
    case mc::ActionType::MoveTo:
        if (out_x) *out_x = action.payload.move_to.x;
        if (out_y) *out_y = action.payload.move_to.y;
        if (out_z) *out_z = action.payload.move_to.z;
        break;

    case mc::ActionType::MineBlock:
        if (out_x) *out_x = static_cast<double>(action.payload.mine_block.x);
        if (out_y) *out_y = static_cast<double>(action.payload.mine_block.y);
        if (out_z) *out_z = static_cast<double>(action.payload.mine_block.z);
        if (out_i1) *out_i1 = action.payload.mine_block.face;
        break;

    case mc::ActionType::PlaceBlock:
        if (out_x) *out_x = static_cast<double>(action.payload.place_block.x);
        if (out_y) *out_y = static_cast<double>(action.payload.place_block.y);
        if (out_z) *out_z = static_cast<double>(action.payload.place_block.z);
        if (out_i1) *out_i1 = action.payload.place_block.face;
        if (out_i2) *out_i2 = action.payload.place_block.block_state;
        break;

    case mc::ActionType::AttackEntity:
        if (out_i1) *out_i1 = action.payload.attack_entity.entity_id;
        break;

    case mc::ActionType::Chat:
        if (out_text && text_cap > 0) {
            uint32_t copy_len = action.payload.chat.len;
            if (copy_len >= text_cap) copy_len = text_cap - 1;
            std::memcpy(out_text, action.payload.chat.text, copy_len);
            out_text[copy_len] = '\0';
        }
        break;

    case mc::ActionType::UseItem:
        if (out_i1) *out_i1 = action.payload.use_item.hand;
        if (out_i2) *out_i2 = action.payload.use_item.item_id;
        break;

    case mc::ActionType::Craft:
        if (out_i1) *out_i1 = action.payload.craft.recipe_id;
        if (out_i2) *out_i2 = action.payload.craft.count;
        break;

    default:
        break;
    }

    return static_cast<uint32_t>(action.type);
}

int32_t gc_mc_action_queue_size(const mc_action_queue_t* q) noexcept {
    if (!q) return 0;
    return static_cast<int32_t>(q->queue.size());
}

void gc_mc_action_queue_clear(mc_action_queue_t* q) noexcept {
    if (q) q->queue.clear();
}

} // extern "C"
