/* The VR page's rows for the sword and the shield in the hands (vr.md rows 42, 44, 46, 47, 52, 53; zeldaVR/gfx/oot_vr_gear_options.h).
   Included by settings_game.h before settings_vr_rows, which calls settings_vr_gear_rows after the swing's rows. */
#pragma once
int vr_oot_sword_tilt(void);
void vr_oot_set_sword_tilt(int deg);
int vr_oot_shield_pose(void);
void vr_oot_set_shield_pose(int v);
int vr_oot_draw_call(void);
void vr_oot_set_draw_call(int on);
int vr_oot_blade_contact(void);
void vr_oot_set_blade_contact(int on);
int vr_oot_jump_hits(void);
void vr_oot_set_jump_hits(int on);
int vr_oot_guard_body(void);
void vr_oot_set_guard_body(int on);

static void settings_vr_gear_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 {TOTKSettingRow *r=settings_row(settings_text("剑在手里的前倾（度）","The sword's forward tilt in the hand (degrees)"),nil,nil,^int{return vr_oot_sword_tilt();},^(int v){vr_oot_set_sword_tilt(v);});
  r.sliderMin=-30;r.sliderMax=60;[rows addObject:r];}
 [rows addObject:settings_row(settings_text("拔剑","Drawing the sword"),@[settings_text("游戏自己的拔出（不出招）","The game's own draw (no slash)"),settings_text("按攻击键","The attack button")],@[@1,@0],
  ^int{return vr_oot_draw_call();},^(int v){vr_oot_set_draw_call(v);})];
 [rows addObject:settings_row(settings_text("剑刃碰到就算（自己判定接触）","The blade's own contact"),nil,nil,^int{return vr_oot_blade_contact();},^(int v){vr_oot_set_blade_contact(v);})];
 [rows addObject:settings_row(settings_text("空中命中算跳劈","A hit in the air is a jump slash"),nil,nil,^int{return vr_oot_jump_hits();},^(int v){vr_oot_set_jump_hits(v);})];
 [rows addObject:settings_row(settings_text("盾的姿势","The shield"),@[settings_text("沿着前臂","Along the forearm"),settings_text("举盾时朝向拳头","Turned to the fist while guarding"),settings_text("一直朝向拳头","Always turned to the fist")],@[@0,@1,@2],
  ^int{return vr_oot_shield_pose();},^(int v){vr_oot_set_shield_pose(v);})];
 [rows addObject:settings_row(settings_text("没锁定时举盾朝摇杆方向","An untargeted guard faces the way you walk"),nil,nil,^int{return vr_oot_guard_body();},^(int v){vr_oot_set_guard_body(v);})];
}
