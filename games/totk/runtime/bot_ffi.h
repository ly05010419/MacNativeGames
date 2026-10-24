#pragma once
#include <TargetConditionals.h>
#include <stdint.h>

/* C <-> Rust FastHands / smoke-bot seam (macOS only; iOS never links libbotw_bot.a).
   Keep layout identical to games/totk/bot_engine/src/lib.rs (#[repr(C)]). */
#if TARGET_OS_OSX

enum {
    BOT_ABI_VERSION = 1,
    BOT_ACTOR_SLOTS = 8,
    BOT_ASHOT_PATH_MAX = 256,
};

typedef struct {
    float x, y, z;
} BotVec3;

typedef struct {
    uint32_t name_hash;
    uint32_t flags;
    BotVec3  pos;
    BotVec3  vel;
    float    dist_m;
} BotActorSlot;

typedef struct {
    uint32_t abi_version;
    uint32_t state_flags;
    uint64_t frame_index;
    uint64_t now_ns;
    float    vfr_dt_s;
    uint32_t this_frame_draws;
    uint32_t wb_stage_count;
    BotVec3  player_pos;
    BotVec3  player_fwd;
    BotVec3  camera_pos;
    BotVec3  camera_fwd;
    BotVec3  camera_up;
    float    camera_fovy;
    BotActorSlot actors[BOT_ACTOR_SLOTS];
} SchematicState;

typedef struct {
    int32_t  pad_connected;
    uint8_t  request_close;
    uint8_t  reserved[3];
    uint64_t buttons;
    int32_t  lx, ly, rx, ry;
    uint32_t pin_frame_step_us;
    uint32_t request_ashot_id;
    char     ashot_path[BOT_ASHOT_PATH_MAX];
} BotFrameCommand;

_Static_assert(sizeof(SchematicState) == 392, "SchematicState ABI size");
_Static_assert(sizeof(BotFrameCommand) == 296, "BotFrameCommand ABI size");

/* Weak import: NULL when build/libbotw_bot.a is absent; never linked into probe-* targets. */
extern void bot_on_frame_tick(const SchematicState *state, BotFrameCommand *cmd)
    __attribute__((weak_import));

#endif /* TARGET_OS_OSX */
