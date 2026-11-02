/* Production parser, persistence format, remapping and Odyssey's independent button combinations. */
#include "../../runtime/keyboard_map.h"
#include <assert.h>

static KeyMapOutput hold(const KeyMap *map, int a, int b, int c, int mouse) {
    unsigned char held[KEYMAP_INPUTS] = {0};
    if (a >= 0) held[a] = 1;
    if (b >= 0) held[b] = 1;
    if (c >= 0) held[c] = 1;
    return keymap_apply(map, held, mouse);
}
int main(void) {
    KeyMap defaults, map, back; keymap_default(&defaults); const char *error = NULL;
    assert(hold(&defaults,38,-1,-1,0).buttons == 1u << KEYMAP_A); /* J confirms */
    assert(hold(&defaults,49,-1,-1,0).buttons == 1u << KEYMAP_B); /* Space jumps */
    assert(hold(&defaults,36,-1,-1,0).buttons == 1u << KEYMAP_PLUS);
    assert(!hold(&defaults,18,-1,-1,0).buttons && !keymap_bindable(18));
    KeyMapOutput out = hold(&defaults,13,2,123,0);
    assert(out.lx==32767 && out.ly==32767 && out.rx==-32767 && !out.buttons);
    /* Crouch and cap throw must coexist for Odyssey's roll/cap combinations; mouse mode cannot repurpose the camera. */
    assert(hold(&defaults,128,129,-1,1).buttons == (1u<<KEYMAP_Y | 1u<<KEYMAP_ZR));
    out=hold(&defaults,14,123,-1,1); assert(out.buttons==1u<<KEYMAP_R && out.rx==-32767);
    int seen[KEYMAP_INPUTS]={0};
    for(int a=0;a<KEYMAP_ACTIONS;a++)for(int slot=0;slot<KEYMAP_SLOTS;slot++){
        int input=defaults.slot[a][slot]; if(input==KEYMAP_NONE)continue;
        assert(keymap_bindable(input) && !seen[input]++);
    }
    map=defaults; assert(!keymap_parse("# layout\n  ZL = j # crouch\r\n",&map,&error));
    assert(map.slot[KEYMAP_ZL][0]==38 && map.slot[KEYMAP_A][0]==KEYMAP_NONE && map.slot[KEYMAP_PLUS][0]==36);
    const char *bad[]={"a = 1", "a = j\nb = j", "a = j j", "a = j k space", "a = none j", "a =", "a j", "fly = q", "a = hyperspace", "a = j\na = q"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){
        map=defaults; assert(keymap_parse(bad[i],&map,&error)>0 && error && !memcmp(&map,&defaults,sizeof map));
    }
    map=defaults;
    assert(keymap_bind(&map,KEYMAP_X,0,12) && map.slot[KEYMAP_X][0]==12 && map.slot[KEYMAP_L][0]==34);
    assert(!keymap_bind(&map,KEYMAP_A,1,18) && !keymap_bind(&map,KEYMAP_ACTIONS,0,38));
    assert(keymap_bind(&map,KEYMAP_X,1,122) && keymap_bind(&map,KEYMAP_X,1,12));
    assert(map.slot[KEYMAP_X][0]==122 && map.slot[KEYMAP_X][1]==12);
    char text[8192]; assert(keymap_format(&map,text,sizeof text)>0); back=defaults;
    assert(!keymap_parse(text,&back,&error) && !memcmp(&map,&back,sizeof map));
    assert(keymap_format(&map,text,10)==-1);
    map=defaults; assert(keymap_assign(&map,38,KEYMAP_X));
    assert(map.slot[KEYMAP_X][0]==34 && map.slot[KEYMAP_X][1]==38 && map.slot[KEYMAP_A][0]==KEYMAP_NONE);
    assert(keymap_assign(&map,38,-1) && keymap_action_of(&map,38,NULL)<0);
    assert(!keymap_assign(&map,18,KEYMAP_A) && !keymap_assign(&map,38,KEYMAP_ACTIONS));
    assert(keymap_input_parse("MOUSE_LEFT",10)==128 && keymap_input_parse("f12",3)==111);
    assert(!strcmp(keymap_input_label(53),"Esc") && !strcmp(keymap_input_label(130),"Wheel click"));
    puts("pass: Odyssey defaults, parser, saved layout, swaps, assignments and button combinations");
    return 0;
}
