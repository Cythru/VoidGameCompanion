#ifndef ACTION_QUEUE_H
#define ACTION_QUEUE_H

#include "gc_common.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus

#include <atomic>
#include <array>
#include <cstring>

namespace mc {

// ---------------------------------------------------------------------------
// Action types
// ---------------------------------------------------------------------------
enum class ActionType : uint32_t {
    None          = 0,
    MoveTo        = 1,
    MineBlock     = 2,
    PlaceBlock    = 3,
    AttackEntity  = 4,
    Chat          = 5,
    UseItem       = 6,
    Craft         = 7,
    LookAt        = 8,
    Jump          = 9,
    DropItem      = 10,
    SwapHand      = 11,
};

// ---------------------------------------------------------------------------
// Action data — fixed-size union for lock-free queue
// ---------------------------------------------------------------------------
struct ActionData {
    static constexpr size_t MAX_STRING_LEN = 240;

    ActionType type;
    uint32_t   sequence_id;    // monotonic for ordering/dedup

    union {
        struct { double x, y, z; float speed; }         move_to;
        struct { int32_t x, y, z; uint8_t face; }       mine_block;
        struct { int32_t x, y, z; uint8_t face;
                 int32_t block_state; }                  place_block;
        struct { int32_t entity_id; uint8_t attack_type; } attack_entity;
        struct { char text[MAX_STRING_LEN]; uint16_t len; } chat;
        struct { int32_t hand; int32_t item_id; }        use_item;
        struct { int32_t recipe_id; uint8_t count; }     craft;
        struct { double x, y, z; }                       look_at;
    } payload;

    ActionData() noexcept : type{ActionType::None}, sequence_id{0}, payload{} {}
};

static_assert(sizeof(ActionData) <= 256, "ActionData must fit in 256 bytes for cache efficiency");

// ---------------------------------------------------------------------------
// Lock-free SPSC (Single-Producer Single-Consumer) ring buffer
//
// One thread pushes actions, another consumes them. No mutex needed.
// Power-of-2 size for fast modulo via bitmask.
// ---------------------------------------------------------------------------
template <size_t N = 1024>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static constexpr size_t MASK = N - 1;

public:
    SPSCQueue() noexcept : head_{0}, tail_{0} {
        // Zero-init the buffer
        for (auto& slot : buf_) {
            slot = ActionData{};
        }
    }

    // Producer: push an action. Returns false if full.
    [[nodiscard]] bool push(const ActionData& action) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;

        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }

        buf_[h] = action;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer: pop an action. Returns false if empty.
    [[nodiscard]] bool pop(ActionData& out) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);

        if (t == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }

        out = buf_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }

    // Peek at front without consuming. Returns false if empty.
    [[nodiscard]] bool peek(ActionData& out) const noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf_[t];
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;
        return next == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept { return N - 1; }

    void clear() noexcept {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    // Pad to avoid false sharing between head (producer) and tail (consumer)
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::array<ActionData, N>       buf_;
};

// Default queue type
using ActionQueue = SPSCQueue<1024>;

// ---------------------------------------------------------------------------
// Helper: build ActionData for common actions
// ---------------------------------------------------------------------------
[[nodiscard]] inline ActionData make_move_to(
    uint32_t seq, double x, double y, double z, float speed = 1.0f) noexcept
{
    ActionData a;
    a.type = ActionType::MoveTo;
    a.sequence_id = seq;
    a.payload.move_to = {x, y, z, speed};
    return a;
}

[[nodiscard]] inline ActionData make_mine_block(
    uint32_t seq, int32_t x, int32_t y, int32_t z, uint8_t face = 1) noexcept
{
    ActionData a;
    a.type = ActionType::MineBlock;
    a.sequence_id = seq;
    a.payload.mine_block = {x, y, z, face};
    return a;
}

[[nodiscard]] inline ActionData make_place_block(
    uint32_t seq, int32_t x, int32_t y, int32_t z,
    uint8_t face, int32_t block_state) noexcept
{
    ActionData a;
    a.type = ActionType::PlaceBlock;
    a.sequence_id = seq;
    a.payload.place_block = {x, y, z, face, block_state};
    return a;
}

[[nodiscard]] inline ActionData make_attack_entity(
    uint32_t seq, int32_t entity_id, uint8_t attack_type = 0) noexcept
{
    ActionData a;
    a.type = ActionType::AttackEntity;
    a.sequence_id = seq;
    a.payload.attack_entity = {entity_id, attack_type};
    return a;
}

[[nodiscard]] inline ActionData make_chat(
    uint32_t seq, const char* text, size_t len) noexcept
{
    ActionData a;
    a.type = ActionType::Chat;
    a.sequence_id = seq;
    size_t copy_len = len < ActionData::MAX_STRING_LEN ? len : ActionData::MAX_STRING_LEN;
    std::memcpy(a.payload.chat.text, text, copy_len);
    if (copy_len < ActionData::MAX_STRING_LEN) {
        a.payload.chat.text[copy_len] = '\0';
    }
    a.payload.chat.len = static_cast<uint16_t>(copy_len);
    return a;
}

[[nodiscard]] inline ActionData make_use_item(
    uint32_t seq, int32_t hand, int32_t item_id = -1) noexcept
{
    ActionData a;
    a.type = ActionType::UseItem;
    a.sequence_id = seq;
    a.payload.use_item = {hand, item_id};
    return a;
}

[[nodiscard]] inline ActionData make_craft(
    uint32_t seq, int32_t recipe_id, uint8_t count = 1) noexcept
{
    ActionData a;
    a.type = ActionType::Craft;
    a.sequence_id = seq;
    a.payload.craft = {recipe_id, count};
    return a;
}

} // namespace mc

#endif // __cplusplus

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_action_queue_t mc_action_queue_t;

mc_action_queue_t* gc_mc_action_queue_new(void) noexcept;
void               gc_mc_action_queue_free(mc_action_queue_t* q) noexcept;

gc_result_t gc_mc_action_queue_push_move(mc_action_queue_t* q, uint32_t seq,
                                         double x, double y, double z, float speed) noexcept;
gc_result_t gc_mc_action_queue_push_mine(mc_action_queue_t* q, uint32_t seq,
                                         int32_t x, int32_t y, int32_t z, uint8_t face) noexcept;
gc_result_t gc_mc_action_queue_push_place(mc_action_queue_t* q, uint32_t seq,
                                          int32_t x, int32_t y, int32_t z,
                                          uint8_t face, int32_t block_state) noexcept;
gc_result_t gc_mc_action_queue_push_attack(mc_action_queue_t* q, uint32_t seq,
                                           int32_t entity_id) noexcept;
gc_result_t gc_mc_action_queue_push_chat(mc_action_queue_t* q, uint32_t seq,
                                         const char* text, uint32_t len) noexcept;
gc_result_t gc_mc_action_queue_push_use_item(mc_action_queue_t* q, uint32_t seq,
                                             int32_t hand) noexcept;
gc_result_t gc_mc_action_queue_push_craft(mc_action_queue_t* q, uint32_t seq,
                                          int32_t recipe_id, uint8_t count) noexcept;

// Pop next action. Returns action type (0 = none/empty).
// Fills out_* params if non-null based on action type.
uint32_t gc_mc_action_queue_pop(mc_action_queue_t* q,
                                uint32_t* out_seq,
                                double* out_x, double* out_y, double* out_z,
                                int32_t* out_i1, int32_t* out_i2,
                                char* out_text, uint32_t text_cap) noexcept;

int32_t gc_mc_action_queue_size(const mc_action_queue_t* q) noexcept;
void    gc_mc_action_queue_clear(mc_action_queue_t* q) noexcept;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ACTION_QUEUE_H
