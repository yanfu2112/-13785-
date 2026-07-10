#ifndef FACE_ACCESS_COMMON_H
#define FACE_ACCESS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_ACCESS_SHM_KEY ((size_t)0x0FA23001u)
#define FACE_ACCESS_MAGIC 0x46414345u
#define FACE_ACCESS_VERSION 1u
#define FACE_ACCESS_FRAME_SLOTS 2u
#define FACE_ACCESS_NAME_MAX 32u

enum face_access_slot_state
{
    FACE_SLOT_FREE = 0,
    FACE_SLOT_WRITING = 1,
    FACE_SLOT_READY = 2,
    FACE_SLOT_READING = 3
};

enum face_access_status
{
    FACE_STATUS_NONE = 0,
    FACE_STATUS_RECOGNIZED = 1,
    FACE_STATUS_UNKNOWN = 2,
    FACE_STATUS_REGISTERED = 3,
    FACE_STATUS_DB_RESET = 4,
    FACE_STATUS_ERROR = 5
};

enum face_access_command_type
{
    FACE_COMMAND_NONE = 0,
    FACE_COMMAND_REGISTER = 1,
    FACE_COMMAND_RESET_DB = 2,
    FACE_COMMAND_STOP = 3
};

typedef struct face_access_frame_slot
{
    volatile uint32_t state;
    uint32_t sequence;
    uint64_t physical_address;
    uint32_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint64_t captured_at_ms;
} face_access_frame_slot_t;

typedef struct face_access_result
{
    uint32_t frame_sequence;
    uint32_t status;
    int32_t person_id;
    float similarity;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint64_t captured_at_ms;
    uint64_t completed_at_ms;
    uint32_t latency_ms;
    char name[FACE_ACCESS_NAME_MAX];
} face_access_result_t;

typedef struct face_access_command
{
    uint32_t type;
    char name[FACE_ACCESS_NAME_MAX];
} face_access_command_t;

typedef struct face_access_shared
{
    uint32_t magic;
    uint32_t version;
    volatile uint32_t ready;
    volatile uint32_t stop_requested;
    volatile uint32_t capture_heartbeat;
    volatile uint32_t ai_heartbeat;
    volatile uint32_t control_heartbeat;
    face_access_frame_slot_t frames[FACE_ACCESS_FRAME_SLOTS];
    face_access_result_t result;
    face_access_command_t command;
    volatile uint32_t result_sequence;
    volatile uint32_t command_sequence;
} face_access_shared_t;

static inline uint32_t face_atomic_load_u32(volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void face_atomic_store_u32(volatile uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static inline int face_atomic_cas_u32(volatile uint32_t *value,
                                      uint32_t expected,
                                      uint32_t desired)
{
    return __atomic_compare_exchange_n(value, &expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif

#endif
