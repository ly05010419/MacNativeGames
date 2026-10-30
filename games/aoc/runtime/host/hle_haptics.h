// Game vibration state, before adapting it to a phone or controller. Players use dm_input.h's slots.
#pragma once
#include "dm_input.h"

typedef struct { float amplitude_low, frequency_low, amplitude_high, frequency_high; } dm_vibration;
typedef struct { int player, motor; dm_vibration value; } dm_vibration_update;
typedef struct { int player; uint64_t epoch; dm_vibration motor[2]; } dm_haptic_state;
typedef struct { float intensity, sharpness; } dm_haptic_level;
enum { DM_HAPTICS_INACTIVE = 1, DM_HAPTICS_INTERRUPTED = 2 };

// The sink copies/enqueues each snapshot in order; called under the state lock, it must not reenter.
// No hardware calls on the sending thread. A changed epoch invalidates all older queued snapshots.
void dm_haptics_init(void (*sink)(dm_haptic_state), int enabled);
void dm_haptics_route(int player, int available);   // clears old state even when availability stays the same
void dm_haptics_clear(int player);                 // engine reset: needs a fresh game signal
uint64_t dm_haptics_epoch(int player);
uint32_t dm_haptics_available(void);
void dm_haptics_suspend(unsigned reason, int suspended);
void dm_haptics_send(const dm_vibration_update *updates, int count);   // one atomic batch
dm_vibration dm_haptics_actual(int player, int motor);
// Approximate amplitude/frequency as Core Haptics intensity/sharpness, for one motor or a phone mix.
dm_haptic_level dm_haptics_level(const dm_vibration *values, int count, float gain);
