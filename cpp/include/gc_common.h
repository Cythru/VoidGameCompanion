#ifndef GC_COMMON_H
#define GC_COMMON_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------
enum gc_result_t : int32_t {
    GC_OK                   =  0,
    GC_ERR_INVALID_ARG      = -1,
    GC_ERR_BUFFER_TOO_SMALL = -2,
    GC_ERR_MALFORMED_PACKET = -3,
    GC_ERR_CONNECTION_LOST  = -4,
    GC_ERR_TIMEOUT          = -5,
    GC_ERR_AUTH_FAILED      = -6,
    GC_ERR_QUEUE_FULL       = -7,
    GC_ERR_QUEUE_EMPTY      = -8,
    GC_ERR_NO_PATH          = -9,
    GC_ERR_OUT_OF_MEMORY    = -10,
    GC_ERR_CHUNK_NOT_LOADED = -11,
    GC_ERR_UNKNOWN          = -99,
};

// ---------------------------------------------------------------------------
// Buffer type — non-owning view
// ---------------------------------------------------------------------------
typedef struct gc_buf_t {
    const uint8_t* data;
    size_t         len;
} gc_buf_t;

// Mutable buffer variant
typedef struct gc_buf_mut_t {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} gc_buf_mut_t;

// ---------------------------------------------------------------------------
// Game event types
// ---------------------------------------------------------------------------
enum gc_event_type_t : uint32_t {
    GC_EVENT_NONE              = 0,
    GC_EVENT_CHAT              = 1,
    GC_EVENT_ENTITY_SPAWN      = 2,
    GC_EVENT_ENTITY_DESPAWN    = 3,
    GC_EVENT_ENTITY_MOVE       = 4,
    GC_EVENT_BLOCK_CHANGE      = 5,
    GC_EVENT_PLAYER_MOVE       = 6,
    GC_EVENT_DEATH             = 7,
    GC_EVENT_KEEPALIVE         = 8,
    GC_EVENT_CHUNK_LOAD        = 9,
    GC_EVENT_CHUNK_UNLOAD      = 10,
    GC_EVENT_HEALTH_UPDATE     = 11,
    GC_EVENT_INVENTORY_CHANGE  = 12,
    GC_EVENT_RESPAWN           = 13,
    GC_EVENT_DISCONNECT        = 14,
    GC_EVENT_LOGIN_SUCCESS     = 15,
    GC_EVENT_POSITION_SYNC     = 16,
};

// ---------------------------------------------------------------------------
// Block position (packed int64 for map keys)
// ---------------------------------------------------------------------------
typedef struct gc_block_pos_t {
    int32_t x;
    int32_t y;
    int32_t z;
} gc_block_pos_t;

// ---------------------------------------------------------------------------
// 3D double-precision position
// ---------------------------------------------------------------------------
typedef struct gc_pos_t {
    double x;
    double y;
    double z;
} gc_pos_t;

// ---------------------------------------------------------------------------
// UUID (128-bit)
// ---------------------------------------------------------------------------
typedef struct gc_uuid_t {
    uint64_t hi;
    uint64_t lo;
} gc_uuid_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // GC_COMMON_H
