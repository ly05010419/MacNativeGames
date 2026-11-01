/* Tomodachi adapter for upstream PR #1210's maps and bindings_diagram.h.
   See docs/tomodachi-upstream-controls.md for provenance and title differences. */
#import <GameController/GameController.h>
#include "keyboard_map.h"
#include "controller_map.h"
#include "gamepad_layout.h"
#define TOTK_UI(zh,en) en
#define TOMODACHI_BINDINGS 1
static KeyMap titleKeys;
static _Atomic unsigned titlePadMap[CONTROLLER_BUTTONS];
static _Atomic int titlePadType;
static _Atomic uint64_t titlePadButtons;
static _Atomic int titlePadLX,titlePadLY,titlePadConnected;
static const unsigned canonicalCodes[KEYMAP_ACTIONS]={38,40,34,32,6,8,12,14,58,56,36,48,3,17,4,5,13,1,0,2,126,125,123,124};
static const char *titleActionNames[KEYMAP_ACTIONS]={"Confirm (A)","Back (B)","X","Y","Left stick press","Right stick press","L","R","ZL","ZR","Plus (+)","Minus (−)","D-pad left","D-pad up","D-pad right","D-pad down","Move up","Move down","Move left","Move right","Zoom up","Zoom down","Zoom left","Zoom right"};
typedef struct { int action; const char *zh,*en,*tag; } SettingsAction;
enum { SETTINGS_BIND_PANEL=100 };
static const SettingsAction *settings_actions(int *count){
 static SettingsAction actions[KEYMAP_ACTIONS];for(int i=0;i<KEYMAP_ACTIONS;i++)actions[i]=(SettingsAction){i,titleActionNames[i],titleActionNames[i],NULL};*count=KEYMAP_ACTIONS;return actions;
}
static NSString *settings_text(const char *zh,const char *en){(void)zh;if(!strcmp(en,"Camera"))return @"Zoom";if(!strcmp(en,"Combat"))return @"Shoulders & triggers";return @(en);}
static NSString *settings_title_action_short(int action){
 static const char *names[]={"Confirm","Back","X","Y","L3","R3","L","R","ZL","ZR","Plus","Minus","D-pad ←","D-pad ↑","D-pad →","D-pad ↓","Move ↑","Move ↓","Move ←","Move →","Zoom ↑","Zoom ↓","Zoom ←","Zoom →"};return action>=0&&action<KEYMAP_ACTIONS?@(names[action]):@"";
}
static void title_keys_apply(void){
 for(unsigned i=0;i<128;i++)atomic_store(&keyMap[i],127);
 atomic_store(&keyMap[18],18);
 for(int a=0;a<KEYMAP_ACTIONS;a++)for(int j=0;j<KEYMAP_SLOTS;j++)if(titleKeys.slot[a][j]<128)atomic_store(&keyMap[titleKeys.slot[a][j]],canonicalCodes[a]);
 runtime_input_clear();
}
static void title_controls_load(void){
 memset(&titleKeys,KEYMAP_NONE,sizeof titleKeys);
 for(int input=0;input<128;input++)for(int a=0;a<KEYMAP_ACTIONS;a++)if(atomic_load(&keyMap[input])==canonicalCodes[a]){int slot=titleKeys.slot[a][0]==KEYMAP_NONE?0:1;titleKeys.slot[a][slot]=(uint8_t)input;}
 NSArray *saved=preferences[@"keyboardMap"];
 if([saved isKindOfClass:NSArray.class]&&saved.count==KEYMAP_ACTIONS*KEYMAP_SLOTS){KeyMap candidate;memset(&candidate,KEYMAP_NONE,sizeof candidate);BOOL valid=YES;NSMutableSet *seen=[NSMutableSet set];for(int i=0;i<KEYMAP_ACTIONS*KEYMAP_SLOTS;i++){id v=saved[i];if(![v isKindOfClass:NSNumber.class]){valid=NO;break;}int code=[v intValue];if(code!=KEYMAP_NONE&&(!keymap_bindable(code)||code>=128||[seen containsObject:v])){valid=NO;break;}if(code!=KEYMAP_NONE)[seen addObject:v];candidate.slot[i/2][i%2]=(uint8_t)code;}if(valid)titleKeys=candidate;}
 title_keys_apply();
 NSArray *pad=preferences[@"controllerMap"];
 BOOL valid=[pad isKindOfClass:NSArray.class]&&pad.count==CONTROLLER_BUTTONS;
 if(valid)for(id entry in pad)if(![entry isKindOfClass:NSNumber.class]||[entry longLongValue]<0||[entry longLongValue]>65535)valid=NO;
 for(int i=0;i<CONTROLLER_BUTTONS;i++)atomic_store(&titlePadMap[i],valid?[pad[i] unsignedIntValue]:(i<16?(1u<<i):0));
 if(!valid&&atomic_load(&swapButtons))for(int i=0;i<4;i++)atomic_store(&titlePadMap[i],1u<<(i^1));
 int type=[preferences[@"controllerType"] intValue];atomic_store(&titlePadType,type>0&&type<GAMEPAD_TYPE_COUNT?type:0);
}
static void runtime_keymap_get(KeyMap *out){*out=titleKeys;}
static void runtime_controller_map_get(uint16_t *map,uint8_t *modes){for(int i=0;i<CONTROLLER_BUTTONS;i++){map[i]=(uint16_t)atomic_load(&titlePadMap[i]);modes[i]=CONTROLLER_HOLD;}}
static int runtime_mouse_enabled(void){return 0;} // Mouse remains Tomodachi's touch input.
static int runtime_gamepad_settings_button(void){return -1;}
static int settings_pad_family(void){int type=atomic_load(&titlePadType);GCController *pad=GCController.controllers.firstObject;return type?type:pad?gamepad_type_detect(pad.productCategory.UTF8String,pad.vendorName.UTF8String):GAMEPAD_TYPE_XBOX;}
static NSString *settings_input_text(int input){const char *name=keymap_input_label(input);return name?@(name):@"—";}
/* Labels from upstream settings_panel.h; the physical positions stay the same. */
static NSString *settings_pad_button(int physical,int family){
 static const char *x[]={"A","B","X","Y","LS","RS","LB","RB","LT","RT","Menu","View"};static const char *p[]={"×","○","□","△","L3","R3","L1","R1","L2","R2","Options","Create"};static const char *n[]={"B","A","Y","X","L3","R3","L","R","ZL","ZR","+","−"};static const char *arrows[]={"←","↑","→","↓"};
 if(physical<0)return @"—";if(physical<12)return @((family==GAMEPAD_TYPE_PLAYSTATION?p:family==GAMEPAD_TYPE_SWITCH?n:x)[physical]);if(physical<16)return [NSString stringWithFormat:@"D-pad %@",@(arrows[physical-12])];if(physical==16)return @"Touchpad";return @"Touch input";
}
static NSString *settings_panel_pad_text(int family){return [NSString stringWithFormat:@"%@ + %@",settings_pad_button(4,family),settings_pad_button(5,family)];}
#include "bindings_diagram.h"
static void title_controls_save(void){
 NSMutableArray *keys=[NSMutableArray array],*pad=[NSMutableArray array];for(int i=0;i<KEYMAP_ACTIONS;i++)for(int j=0;j<2;j++)[keys addObject:@(titleKeys.slot[i][j])];for(int i=0;i<CONTROLLER_BUTTONS;i++)[pad addObject:@(atomic_load(&titlePadMap[i]))];preferences[@"keyboardMap"]=keys;preferences[@"controllerMap"]=pad;save_preferences();
}
static uint64_t title_map_buttons(uint64_t buttons){uint64_t out=buttons&~0xffffULL;for(int i=0;i<16;i++)if(buttons&(1ULL<<i))out|=atomic_load(&titlePadMap[i]);return out;}
/* Called once per native pad event, before guest input capture; no polling steals edges. */
void runtime_title_gamepad_event(RuntimeGamepadState state){atomic_store(&titlePadButtons,state.buttons);atomic_store(&titlePadLX,state.lx);atomic_store(&titlePadLY,state.ly);atomic_store(&titlePadConnected,state.connected);}
uint64_t runtime_title_physical_buttons(uint64_t buttons,const char *category,const char *vendor){
 int family=atomic_load(&titlePadType);if(!family)family=gamepad_type_detect(category,vendor);
 if(family==GAMEPAD_TYPE_SWITCH)buttons=(buttons&~15ULL)|((buttons&5)<<1)|((buttons&10)>>1);return buttons;
}
