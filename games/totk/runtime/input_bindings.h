/* Physical keyboard/mouse bindings; game-button order follows the FullKey ABI.
   No AppKit or player data: also used by the focused input regression probes. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "controller_map.h"
enum { INPUT_ACTIONS=24, INPUT_ALTERNATES=2, INPUT_KEYS=128, INPUT_MOUSE_BUTTONS=8, INPUT_SOURCES=136 };
enum { INPUT_LEFT=16, INPUT_RIGHT, INPUT_UP, INPUT_DOWN, INPUT_LOOK_LEFT, INPUT_LOOK_RIGHT, INPUT_LOOK_UP, INPUT_LOOK_DOWN };
typedef struct {
    uint16_t source[INPUT_ACTIONS][INPUT_ALTERNATES]; /* 0 unused; keycode+1; 129..136 mouse buttons */
    int mouse_look, mouse_sensitivity, mouse_invert_y;
} RuntimeKeyboardBindings;
typedef struct {
    uint16_t map[CONTROLLER_BUTTONS];
    uint8_t modes[CONTROLLER_BUTTONS];
    int swap_sticks, invert_look_y;
} RuntimeControllerBindings;
static inline void input_bindings_default(RuntimeKeyboardBindings *b) {
    static const unsigned key[INPUT_ACTIONS]={38,40,34,32,6,8,12,14,58,56,36,48,3,17,4,5,0,2,13,1,123,124,126,125};
    memset(b,0,sizeof *b);
    for(int i=0;i<INPUT_ACTIONS;i++)b->source[i][0]=(uint16_t)(key[i]+1);
    b->source[1][1]=50;b->source[11][1]=54;b->mouse_sensitivity=100;
}
/* A physical source may drive only one game action. Equal defaults/alternates of
   the same action are also rejected, so releasing one source cannot stick another. */
static inline int input_bindings_valid(const RuntimeKeyboardBindings *b) {
    if((unsigned)b->mouse_look>1||(unsigned)b->mouse_invert_y>1||b->mouse_sensitivity<10||b->mouse_sensitivity>500)return 0;
    unsigned char seen[INPUT_SOURCES+1]={0};
    for(int i=0;i<INPUT_ACTIONS;i++)for(int j=0;j<INPUT_ALTERNATES;j++){
        unsigned s=b->source[i][j];if(s>INPUT_SOURCES||(s&&seen[s]++))return 0;
    }
    return 1;
}
static inline int controller_bindings_valid(const RuntimeControllerBindings *b) {
    if((unsigned)b->swap_sticks>1||(unsigned)b->invert_look_y>1)return 0;
    for(int i=0;i<CONTROLLER_BUTTONS;i++){
        unsigned m=b->map[i],kind=controller_mode_kind(b->modes[i]);
        if((m&&(m&(m-1)))||kind>CONTROLLER_AIM)return 0;
        if(!m&&b->modes[i]!=CONTROLLER_HOLD)return 0;
        if(kind==CONTROLLER_AIM){int fire=controller_mode_fire(b->modes[i]);if(fire>=CONTROLLER_BUTTONS||fire==i)return 0;}
        else if(controller_mode_fire(b->modes[i]))return 0;
    }
    return 1;
}
