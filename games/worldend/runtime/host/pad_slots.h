// Player slots of the physical controllers (front_mac.m; pure, so tests/t_hid.c checks it without controllers): each controller holds one slot, slot p is player p + 1
// (hle_hid.c dm_player_npad gives its npad). A connecting controller takes, in this order: a free slot whose last holder had its name (the same controller reconnected gets its
// player back; GameController gives a controller no identity across a reconnect, so two controllers of one model may trade places), player 1 when it is free, the lowest slot
// no controller has held yet (so a slot stays kept for a controller that went away while others are in use), then the lowest free slot. With every slot held it gets none (-1).
// A disconnected controller frees its slot and leaves its name on it. The owner is any pointer that stays the same while the controller is connected (the front end keeps the
// GCController); a holder that never disconnects (the keyboard with DM_KBD_PLAYER=own) takes slot 0 first and keeps it.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dm_input.h"

typedef struct { const void *owner[DM_PLAYERS]; char last[DM_PLAYERS][64]; } pad_slots;

static int pad_slot_of(const pad_slots *s, const void *owner) {
    for (int i = 0; owner && i < DM_PLAYERS; i++) if (s->owner[i] == owner) return i;
    return -1;
}
static int pad_slot_take(pad_slots *s, const void *owner, const char *name) {
    int p = pad_slot_of(s, owner); if (p >= 0 || !owner) return p;
    if (!name) name = "";
    for (int i = 0; p < 0 && *name && i < DM_PLAYERS; i++) if (!s->owner[i] && !strcmp(s->last[i], name)) p = i;
    if (p < 0 && !s->owner[0]) p = 0;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i] && !s->last[i][0]) p = i;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i]) p = i;
    if (p < 0) return -1;
    s->owner[p] = owner; snprintf(s->last[p], sizeof s->last[p], "%s", name);
    return p;
}
static int pad_slot_free(pad_slots *s, const void *owner) {
    int p = pad_slot_of(s, owner); if (p >= 0) s->owner[p] = NULL;
    return p;
}
static uint32_t pad_slot_mask(const pad_slots *s) {   // bit p: slot p is held
    uint32_t m = 0; for (int i = 0; i < DM_PLAYERS; i++) if (s->owner[i]) m |= 1u << i;
    return m;
}
