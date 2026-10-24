/* One controls.txt for the existing controller/mouse layout and Mac keyboard.
   Pure C so the launcher validates exactly what the editor and runtime read. */
#pragma once
#include "controller_map.h"
#include <stdio.h>
#include <stdlib.h>
enum { CONTROLS_KEYS=128, CONTROLS_TARGETS=24 };
typedef struct {
    uint16_t map[CONTROLLER_BUTTONS];
    uint8_t modes[CONTROLLER_BUTTONS], keys[CONTROLS_KEYS]; /* 0 none, 1..16 FullKey, 17..24 stick directions */
} ControlsLayout;
static const char *const controls_axis_names[]={"move_left","move_right","move_up","move_down","look_left","look_right","look_up","look_down"};
static inline const char *controls_target_name(unsigned target) {
    return !target ? "none" : target<=16 ? controller_switch_names[target-1] : target<=24 ? controls_axis_names[target-17] : NULL;
}
static inline int controls_key_reserved(unsigned k) {
    return k==18||k==19||k==20||k==21||k==22||k==23||k==26||k==54||k==55; /* 1..7, Command */
}
static inline void controls_layout_default(ControlsLayout *b) {
    controller_map_default(b->map,b->modes); memset(b->keys,0,sizeof b->keys);
    static const unsigned key[CONTROLS_TARGETS]={38,40,34,32,6,8,12,14,58,56,36,48,3,17,4,5,0,2,13,1,123,124,126,125};
    for(unsigned i=0;i<CONTROLS_TARGETS;i++)b->keys[key[i]]=(uint8_t)(i+1);
    b->keys[49]=2;b->keys[53]=12;
}
static inline int controls_layout_valid(const ControlsLayout *b) {
    for(unsigned i=0;i<CONTROLS_KEYS;i++)if(b->keys[i]>CONTROLS_TARGETS||(b->keys[i]&&controls_key_reserved(i)))return 0;
    for(unsigned i=0;i<CONTROLLER_BUTTONS;i++){
        unsigned m=b->map[i],kind=controller_mode_kind(b->modes[i]);
        if((m&&(m&(m-1)))||kind>CONTROLLER_AIM||(!m&&b->modes[i]))return 0;
        if(kind==CONTROLLER_AIM){unsigned fire=controller_mode_fire(b->modes[i]);if(fire>=CONTROLLER_BUTTONS||fire==i)return 0;}
        else if(controller_mode_fire(b->modes[i]))return 0;
    }
    return 1;
}
/* Transactional: on error neither controller nor keyboard entries change. */
static inline int controls_layout_parse(const char *text,ControlsLayout *b,const char **error) {
    ControlsLayout next=*b;int line=0;
    for(const char *p=text;p&&*p;){
        line++;const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);
        char row[16384];if(n>=sizeof row){*error="line too long";return line;}
        memcpy(row,p,n);row[n]=0;char *start=row;while(*start==' '||*start=='\t')start++;
        if(!strncasecmp(start,"key_",4)){
            char *number=start+4,*tail;unsigned long key=strtoul(number,&tail,10);
            if(number==tail||*number<'0'||*number>'9'||key>=CONTROLS_KEYS){*error="expected key_0 through key_127";return line;}
            while(*tail==' '||*tail=='\t')tail++;
            if(*tail++!='='){*error="expected key_<Mac keycode> = target";return line;}
            while(*tail==' '||*tail=='\t')tail++;
            char *stop=tail;while(*stop&&*stop!=' '&&*stop!='\t'&&*stop!='\r'&&*stop!='#')stop++;
            int target=-1;for(unsigned i=0;i<=CONTROLS_TARGETS;i++)if(strlen(controls_target_name(i))==(size_t)(stop-tail)&&!strncasecmp(controls_target_name(i),tail,(size_t)(stop-tail)))target=(int)i;
            if(target<0){*error="unknown keyboard target";return line;}
            while(*stop==' '||*stop=='\t'||*stop=='\r')stop++;
            if(*stop&&*stop!='#'){*error="keyboard bindings take no mode";return line;}
            if(target&&controls_key_reserved((unsigned)key)){*error="key reserved for application shortcuts";return line;}
            next.keys[key]=(uint8_t)target;
        }else if(controller_map_parse(row,next.map,next.modes,error))return line;
        p=end?end+1:NULL;
    }
    *b=next;return 0;
}
/* Canonical file, accepted by the same parser. Includes explicit cleared keys. */
static inline int controls_layout_format(const ControlsLayout *b,char *out,size_t cap) {
    if(!controls_layout_valid(b)||!cap)return 0;
    size_t used=0;
    for(unsigned i=0;i<CONTROLLER_BUTTONS;i++){
        unsigned target=0;while(target<16&&b->map[i]!=(1u<<target))target++;
        static const char *const mode[]={"hold","tap","toggle","aim"};unsigned kind=controller_mode_kind(b->modes[i]);
        int n=snprintf(out+used,cap-used,"%s = %s %s%s%s\n",controller_physical_names[i],target<16?controller_switch_names[target]:"none",mode[kind],kind==CONTROLLER_AIM?" ":"",kind==CONTROLLER_AIM?controller_physical_names[controller_mode_fire(b->modes[i])]:"");
        if(n<0||(size_t)n>=cap-used)return 0;used+=(size_t)n;
    }
    for(unsigned i=0;i<CONTROLS_KEYS;i++){
        int n=snprintf(out+used,cap-used,"key_%u = %s\n",i,controls_target_name(b->keys[i]));
        if(n<0||(size_t)n>=cap-used)return 0;used+=(size_t)n;
    }
    return (int)used;
}
