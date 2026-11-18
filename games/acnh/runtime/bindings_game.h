/* Animal Crossing: New Horizons: what the settings panel's key bindings drawings (bindings_diagram.h, shared/) write and how they colour it:
   the kind of each action (moving and camera, combat, actions, menus: DIAGRAM_* there), the few words on a key, the
   legend's words, the mouse's title and what moving it does, and the right stick's callout. Included by bindings_diagram.h
   after the kinds and their colours. */
#pragma once

static int diagram_kind(int action) {
 if(action<0)return DIAGRAM_NONE;
 if(action==KEYMAP_SCREENSHOT)return DIAGRAM_SYSTEM;
 if(action>=KEYMAP_MOVE_UP)return DIAGRAM_MOVE;
 if(action>=KEYMAP_DLEFT&&action<=KEYMAP_DDOWN)return DIAGRAM_COMBAT;   /* Animal Crossing: the tools */
 if(action==KEYMAP_PLUS||action==KEYMAP_MINUS||action==KEYMAP_ZL||action==KEYMAP_ZR)return DIAGRAM_MENU;
 return DIAGRAM_ACTION;
}
/* The few words written on a key (a line break where two do not fit). */
static NSString *diagram_action_short(int action) {
 switch(action){
  case KEYMAP_A:return settings_text("确认\n使用","Use\nTalk");
  case KEYMAP_B:return settings_text("跑步\n取消","Run\nCancel");
  case KEYMAP_X:return settings_text("口袋","Pockets");
  case KEYMAP_Y:return settings_text("捡起","Pick up");
  case KEYMAP_ZR:return settings_text("表情","React");
  case KEYMAP_ZL:return settings_text("狸端","Phone");
  case KEYMAP_L:return @"L";
  case KEYMAP_R:return @"R";
  case KEYMAP_LSTICK:return @"L3";
  case KEYMAP_RSTICK:return @"R3";
  case KEYMAP_PLUS:return @"+";
  case KEYMAP_MINUS:return @"−";
  case KEYMAP_DLEFT:return settings_text("十字 ←","D-pad ←");
  case KEYMAP_DUP:return settings_text("工具环","Tool ring");
  case KEYMAP_DRIGHT:return settings_text("十字 →","D-pad →");
  case KEYMAP_DDOWN:return settings_text("收工具","Put away");
  case KEYMAP_MOVE_UP:return settings_text("前进","Forward");
  case KEYMAP_MOVE_DOWN:return settings_text("后退","Back");
  case KEYMAP_MOVE_LEFT:return settings_text("左移","Left");
  case KEYMAP_MOVE_RIGHT:return settings_text("右移","Right");
  case KEYMAP_CAMERA_UP:return settings_text("视角 ↑","Cam ↑");
  case KEYMAP_CAMERA_DOWN:return settings_text("视角 ↓","Cam ↓");
  case KEYMAP_CAMERA_LEFT:return settings_text("视角 ←","Cam ←");
  case KEYMAP_CAMERA_RIGHT:return settings_text("视角 →","Cam →");
  case KEYMAP_SCREENSHOT:return settings_text("截图","Screen\nshot");
 }
 return @"";
}
/* The legend's words for the four kinds of action (the panel's and the unused inputs' are the same in every game). */
static NSString *diagram_kind_name(int kind) {
 switch(kind){
  case DIAGRAM_MOVE:return settings_text("移动和视角","Moving & camera");
  case DIAGRAM_COMBAT:return settings_text("工具（十字键）","Tools (d-pad)");
  case DIAGRAM_ACTION:return settings_text("动作","Actions");
  case DIAGRAM_MENU:return settings_text("菜单和狸端","Menus & NookPhone");
 }
 return @"";
}
/* The mouse drawing's title: this game has no mouse mode; its buttons click as the keyboard layout says. */
static NSString *diagram_mouse_title(void) { return settings_text("鼠标","Mouse"); }
#define DIAGRAM_MOUSE_MOVE (-1)   /* moving the mouse does nothing in this game */
/* The right stick's callout on the controller drawing. */
static NSString *diagram_right_stick_text(void) { return settings_text("视角","Camera"); }
