/* Player operation codes, distinct from Npad/UI button bits. Each gameplay source
   (raw A/B/X/Y/L/R/ZL/ZR/D-pad/L-stick/R-stick click/Plus/Minus) selects one code; menus,
   dialogue and the map keep their raw A/B path. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
enum {
    ACTION_NONE=0,
    ACTION_INTERACT=0x1,      /* raw A */
    ACTION_CANCEL=0x2,        /* raw B: cancel on press, run when held */
    ACTION_JUMP=0x4,          /* raw X */
    ACTION_ATTACK=0x8,        /* raw Y */
    ACTION_ABILITY=0x10,      /* raw L */
    ACTION_THROW=0x20,        /* raw R */
    ACTION_SHIELD=0x40,       /* raw ZL */
    ACTION_BOW=0x80,          /* raw ZR */
    ACTION_DPAD_UP=0x100,
    ACTION_DPAD_DOWN=0x200,
    ACTION_DPAD_LEFT=0x400,
    ACTION_DPAD_RIGHT=0x800,
    ACTION_CROUCH=0x1000,     /* L-stick click */
    ACTION_SCOPE=0x2000,      /* R-stick click */
    ACTION_PAUSE=0x4000,      /* Plus: pause / inventory */
    ACTION_MAP=0x8000,        /* Minus: map */
    ACTION_BINDING_COUNT=16,
    ACTION_BINDING_CODE_COUNT=17
};
/* Source order: A, B, X, Y, L, R, ZL, ZR, Up, Down, Left, Right, L-stick, R-stick, Plus, Minus. */
typedef struct { uint16_t target[ACTION_BINDING_COUNT]; } RuntimeActionBindings;
static inline uint16_t action_binding_code_at(unsigned i) {
    static const uint16_t codes[] = {ACTION_NONE,ACTION_INTERACT,ACTION_CANCEL,ACTION_JUMP,ACTION_ATTACK,
        ACTION_ABILITY,ACTION_THROW,ACTION_SHIELD,ACTION_BOW,ACTION_DPAD_UP,ACTION_DPAD_DOWN,
        ACTION_DPAD_LEFT,ACTION_DPAD_RIGHT,ACTION_CROUCH,ACTION_SCOPE,ACTION_PAUSE,ACTION_MAP};
    return i < sizeof codes / sizeof *codes ? codes[i] : ACTION_NONE;
}
static inline unsigned action_binding_code_count(void) {
    return ACTION_BINDING_CODE_COUNT;
}
static inline int action_binding_code_index(uint16_t code) {
    for (unsigned i = 0; i < action_binding_code_count(); i++) if (action_binding_code_at(i) == code) return (int)i;
    return 0;
}
static inline int action_binding_code_valid(uint16_t code) {
    return action_binding_code_at((unsigned)action_binding_code_index(code)) == code;
}
static inline void action_bindings_default(RuntimeActionBindings *b) {
    static const uint16_t d[ACTION_BINDING_COUNT] = {ACTION_INTERACT, ACTION_CANCEL, ACTION_JUMP, ACTION_ATTACK,
        ACTION_ABILITY, ACTION_THROW, ACTION_SHIELD, ACTION_BOW, ACTION_DPAD_UP, ACTION_DPAD_DOWN,
        ACTION_DPAD_LEFT, ACTION_DPAD_RIGHT, ACTION_CROUCH, ACTION_SCOPE, ACTION_PAUSE, ACTION_MAP};
    memcpy(b->target, d, sizeof d);
}
static inline void action_bindings_xbox(RuntimeActionBindings *b) {
    action_bindings_default(b);
    b->target[0] = ACTION_JUMP;
    b->target[2] = ACTION_ATTACK;
    b->target[3] = ACTION_INTERACT;
}
static inline int action_bindings_valid(const RuntimeActionBindings *b) {
    for (unsigned i = 0; i < ACTION_BINDING_COUNT; i++) if (!action_binding_code_valid(b->target[i])) return 0;
    return 1;
}
static inline int action_bindings_equal(const RuntimeActionBindings *a, const RuntimeActionBindings *b) {
    return memcmp(a->target, b->target, sizeof a->target) == 0;
}
static inline const char *action_binding_name(unsigned n) {
    switch (n) {
        case ACTION_NONE: return "none";
        case ACTION_INTERACT: return "interact";
        case ACTION_CANCEL: return "cancel/run";
        case ACTION_JUMP: return "jump";
        case ACTION_ATTACK: return "attack";
        case ACTION_ABILITY: return "ability";
        case ACTION_THROW: return "throw";
        case ACTION_SHIELD: return "shield/aim";
        case ACTION_BOW: return "bow";
        case ACTION_DPAD_UP: return "quick-up";
        case ACTION_DPAD_DOWN: return "quick-down";
        case ACTION_DPAD_LEFT: return "quick-left";
        case ACTION_DPAD_RIGHT: return "quick-right";
        case ACTION_CROUCH: return "crouch";
        case ACTION_SCOPE: return "scope";
        case ACTION_PAUSE: return "pause";
        case ACTION_MAP: return "map";
        default: return "invalid";
    }
}
static inline const char *action_binding_source_name(unsigned i) {
    static const char *names[ACTION_BINDING_COUNT] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR",
        "Up", "Down", "Left", "Right", "L-stick", "R-stick", "+", "-"};
    return i < ACTION_BINDING_COUNT ? names[i] : "?";
}

static inline int action_bindings_parse(const char *s,RuntimeActionBindings *b){
 action_bindings_default(b);
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
  char *end;unsigned long v=strtoul(s,&end,0);
  if(end==s||v>0xffff||!action_binding_code_valid((uint16_t)v))return 0;
  b->target[i]=(uint16_t)v;
  if(i+1<ACTION_BINDING_COUNT){if(!*end&&i==13)return 1;if(*end!=',')return 0;s=end+1;}
  else if(*end)return 0;
 }
 return 1;
}
