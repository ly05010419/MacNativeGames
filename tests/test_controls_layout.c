#include "../games/totk/runtime/controls_layout.h"
#include <assert.h>
int main(void){
 ControlsLayout b,original;controls_layout_default(&b);original=b;const char *error=NULL;
 assert(b.keys[38]==1&&b.keys[49]==2&&b.keys[53]==12&&b.keys[0]==17&&b.keys[126]==23);
 assert(!controls_layout_parse("# existing layout\nsouth = x tap\nmouse_right = zr aim mouse5\nkey_38 = none\nkey_7 = a\nkey_0 = look_left\n",&b,&error));
 assert(b.map[0]==4&&b.modes[0]==CONTROLLER_TAP&&controller_mode_fire(b.modes[18])==21);
 assert(!b.keys[38]&&b.keys[7]==1&&b.keys[0]==21);
 char text[16384];assert(controls_layout_format(&b,text,sizeof text)>0);ControlsLayout roundtrip;controls_layout_default(&roundtrip);
 assert(!controls_layout_parse(text,&roundtrip,&error));assert(!memcmp(&b,&roundtrip,sizeof b));
 const char *bad[]={"key_128 = a","key_-1 = a","key_1x = a","key_7 = a tap","key_18 = b","key_38 = invalid","mouse_right = zr aim mouse_right"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){char row[256];snprintf(row,sizeof row,"south = b\n%s",bad[i]);ControlsLayout keep=b;assert(controls_layout_parse(row,&b,&error)==2);assert(!memcmp(&keep,&b,sizeof b));}
 assert(!controls_layout_parse("KEY_7 = B # comment\nkey_18 = none\n",&b,&error));
 assert(!controls_layout_format(&b,text,8));
 assert(controls_layout_valid(&original));puts("PASS: shared controls.txt parsing, validation, rollback and round-trip");
}
