#pragma once
#include "switch2_proto.h"

typedef struct s2_pad { s2_pad_state st; int received_report; uint64_t previous_buttons; float previous_axes[4]; struct s2_pad *next; } s2_pad;

// The front end's side. All calls come on the main queue.
typedef struct {
    void (*attach)(s2_pad *p);   // a new pad: take it a player slot (player() then names it, -1 for none)
    void (*detach)(s2_pad *p);   // the pad's last controller went: it is already off the list and is freed after the call
    void (*read)(s2_pad *p);     // the pad's state changed
    int (*input_active)(void);  // existing foreground/background controller policy
    int (*player)(s2_pad *p);    // 0-based player, -1 without one
} dm_switch2_front;

// Call on the main queue after the player enables Bluetooth input.
int dm_switch2_start(const dm_switch2_front *front);
s2_pad *dm_switch2_pads(void);
