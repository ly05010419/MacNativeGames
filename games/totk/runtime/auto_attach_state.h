/* Optional arrow material repeat. Only identities are remembered here, never an actor reference to dereference later. */
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t player, arrow;
    uint32_t player_id, arrow_id;
    int suppressed;
} AutoAttachState;
typedef struct {
    uint64_t player, arrow;
    uint32_t player_id, arrow_id;
    int active, paused, drawn, pending, attached, have_item;
} AutoAttachFrame;
enum { AUTO_ATTACH_NONE, AUTO_ATTACH_REMOVE, AUTO_ATTACH_REPEAT };

static inline int auto_attach_decide(AutoAttachState *s, AutoAttachFrame f, uint32_t remove_id) {
    if (!f.player || !f.arrow || !f.active || f.paused || !f.drawn) return AUTO_ATTACH_NONE;
    if (s->player != f.player || s->player_id != f.player_id)
        *s = (AutoAttachState){.player=f.player, .player_id=f.player_id};
    if (remove_id && remove_id == f.arrow_id && f.attached) {
        s->arrow=f.arrow; s->arrow_id=f.arrow_id; s->suppressed=1;
        return AUTO_ATTACH_REMOVE;
    }
    if (s->arrow == f.arrow && s->arrow_id == f.arrow_id) return AUTO_ATTACH_NONE;
    s->arrow=f.arrow; s->arrow_id=f.arrow_id;
    /* A manual request, even one still loading, owns this draw. Never replace it or retry a failed automatic request. */
    return !s->suppressed && !f.pending && !f.attached && f.have_item ? AUTO_ATTACH_REPEAT : AUTO_ATTACH_NONE;
}

/* Called only for a manual arrow-material request. Choosing the same material again also resumes repeat. */
static inline void auto_attach_manual(AutoAttachState *s) { s->suppressed=0; }
