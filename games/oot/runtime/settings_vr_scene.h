/* The VR page's rows for the game's scenes and the ocarina in the hands (vr.md rows 69-71, 75-79; zeldaVR/gfx/oot_vr_scene_draw.h
   has the options and their defaults, HookshotVR's). Included by settings_game.h, which calls settings_vr_scene_rows from
   settings_vr_rows (GAME_OOT on the Mac). */
#pragma once
int vr_oot_cutscene_view(void);
void vr_oot_set_cutscene_view(int v);
int vr_oot_cutscene_3d(void);
void vr_oot_set_cutscene_3d(int v);
int vr_oot_cutscene_light(void);
void vr_oot_set_cutscene_light(int v);
int vr_oot_cutscene_cull(void);
void vr_oot_set_cutscene_cull(int v);
int vr_oot_realign(void);
void vr_oot_set_realign(int v);
int vr_oot_item_get_view(void);
void vr_oot_set_item_get_view(int on);
int vr_oot_ocarina(void);
void vr_oot_set_ocarina(int on);
int vr_oot_song_fx(void);
void vr_oot_set_song_fx(int on);
int vr_oot_song_menu(void);
void vr_oot_set_song_menu(int on);
static void settings_vr_scene_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 [rows addObject:settings_row(settings_text("过场的看法","Cutscenes"),@[settings_text("全部放在屏幕上","All on a screen"),settings_text("剧情放在屏幕上，其余用自己的眼睛（同以前）","Story on a screen, the rest from my eyes (as before)"),
   settings_text("剧情也在 3D 里（推荐）","Story in 3D too (recommended)")],@[@0,@1,@2],^int{return vr_oot_cutscene_view();},^(int v){vr_oot_set_cutscene_view(v);})];
 [rows addObject:settings_row(settings_text("3D 过场 · 从哪里看","3D cutscenes · from where"),@[settings_text("导演的镜头","The director's camera"),settings_text("剧场座位","A theatre seat"),
   settings_text("林克站的地方","Link's spot"),settings_text("跟着林克（他的眼睛）","Following Link (his eyes)")],@[@0,@1,@2,@3],^int{return vr_oot_cutscene_3d();},^(int v){vr_oot_set_cutscene_3d(v);})];
 [rows addObject:settings_row(settings_text("过场的灯光","Cutscene light"),@[settings_text("自动","Auto"),settings_text("每个镜头","Every shot"),settings_text("保持第一个镜头的","Hold the first shot's"),
   settings_text("用场地的","The area's")],@[@0,@1,@2,@3],^int{return vr_oot_cutscene_light();},^(int v){vr_oot_set_cutscene_light(v);})];
 [rows addObject:settings_row(settings_text("过场的剔除（测试）","Cutscene culling (test)"),@[settings_text("按你的视角","From your view"),settings_text("按游戏的镜头","From the game's camera")],@[@0,@1],
  ^int{return vr_oot_cutscene_cull();},^(int v){vr_oot_set_cutscene_cull(v);})];
 [rows addObject:settings_row(settings_text("事件后对正方向","Re-align after events"),@[settings_text("关","Off"),settings_text("快转","Quick turn"),settings_text("结束时瞬转","Snap at the end"),
   settings_text("跟着林克转","Follow Link")],@[@0,@1,@2,@3],^int{return vr_oot_realign();},^(int v){vr_oot_set_realign(v);})];
 [rows addObject:settings_row(settings_text("得到物品时","Item get"),@[settings_text("摆在面前，视角不跟林克转","Before my eyes, my view kept"),settings_text("照游戏的","As the game shows it")],@[@1,@0],
  ^int{return vr_oot_item_get_view();},^(int v){vr_oot_set_item_get_view(v);})];
 [rows addObject:settings_row(settings_text("陶笛拿在手里（腰带上取，放到嘴边吹）","Ocarina in the hands (from the belt, to the mouth)"),nil,nil,^int{return vr_oot_ocarina();},^(int v){vr_oot_set_ocarina(v);})];
 [rows addObject:settings_row(settings_text("陶笛 · 另一只手握拳打开曲目菜单","Ocarina · the other hand's grip opens the songs"),nil,nil,^int{return vr_oot_song_menu();},^(int v){vr_oot_set_song_menu(v);})];
 [rows addObject:settings_row(settings_text("陶笛 · 吹对曲子时身边的特效","Ocarina · a song's effects around you"),@[settings_text("显示","Shown"),settings_text("隐藏","Hidden")],@[@1,@0],
  ^int{return vr_oot_song_fx();},^(int v){vr_oot_set_song_fx(v);})];
}
