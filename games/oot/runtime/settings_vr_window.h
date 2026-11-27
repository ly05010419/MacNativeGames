/* The VR page's rows for the window view (vr.md row 3; zeldaVR/gfx/oot_vr_window_draw.h has the options and their defaults).
   Included by settings_game.h, which calls settings_vr_window_rows from settings_vr_rows (GAME_OOT on the Mac). */
#pragma once
int vr_oot_screen_shape(void);
void vr_oot_set_screen_shape(int v);
int vr_oot_win_w(void);
void vr_oot_set_win_w(int cm);
int vr_oot_win_169(void);
void vr_oot_set_win_169(int on);
int vr_oot_win_h(void);
void vr_oot_set_win_h(int cm);
int vr_oot_win_round(void);
void vr_oot_set_win_round(int pct);
int vr_oot_win_feather(void);
void vr_oot_set_win_feather(int mm);
int vr_oot_win_tilt(void);
void vr_oot_set_win_tilt(int deg);
int vr_oot_win_dist(void);
void vr_oot_set_win_dist(int cm);
int vr_oot_win_back(void);
void vr_oot_set_win_back(int on);
void vr_oot_window_place(void);
static TOTKSettingRow *settings_vr_window_slider(NSString *title, int lo, int hi, int (^get)(void), void (^set)(int)) {
 TOTKSettingRow *r=settings_row(title,nil,nil,get,set);r.sliderMin=lo;r.sliderMax=hi;
 r.enabled=^BOOL{return vr_oot_screen_shape()==1;};return r;
}
static void settings_vr_window_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 [rows addObject:settings_row(settings_text("标题、菜单、过场与大屏幕的形状","Title, menu, cutscene and big screen shape"),
   @[settings_text("弧形屏幕（同以前）","Curved screen (as before)"),settings_text("窗口（固定在房间里的框）","Window (a frame fixed in the room)")],@[@0,@1],
   ^int{return vr_oot_screen_shape();},^(int v){vr_oot_set_screen_shape(v);})];
 [rows addObject:settings_vr_window_slider(settings_text("窗口 · 宽度（厘米）","Window · width (cm)"),20,300,^int{return vr_oot_win_w();},^(int v){vr_oot_set_win_w(v);})];
 {TOTKSettingRow *r=settings_row(settings_text("窗口 · 保持 16:9","Window · keep 16:9"),nil,nil,^int{return vr_oot_win_169();},^(int v){vr_oot_set_win_169(v);});
  r.enabled=^BOOL{return vr_oot_screen_shape()==1;};[rows addObject:r];}
 {TOTKSettingRow *r=settings_vr_window_slider(settings_text("窗口 · 高度（厘米，不保持 16:9 时）","Window · height (cm, without 16:9)"),10,200,^int{return vr_oot_win_h();},^(int v){vr_oot_set_win_h(v);});
  r.enabled=^BOOL{return vr_oot_screen_shape()==1&&!vr_oot_win_169();};[rows addObject:r];}
 [rows addObject:settings_vr_window_slider(settings_text("窗口 · 圆角（%，100 = 椭圆）","Window · rounded corners (%, 100 = an oval)"),0,100,^int{return vr_oot_win_round();},^(int v){vr_oot_set_win_round(v);})];
 [rows addObject:settings_vr_window_slider(settings_text("窗口 · 边缘羽化（毫米）","Window · feathered edge (mm)"),0,100,^int{return vr_oot_win_feather();},^(int v){vr_oot_set_win_feather(v);})];
 [rows addObject:settings_vr_window_slider(settings_text("窗口 · 倾斜（度，正数上沿向后）","Window · tilt (degrees, more leans its top away)"),-45,45,^int{return vr_oot_win_tilt();},^(int v){vr_oot_set_win_tilt(v);})];
 [rows addObject:settings_vr_window_slider(settings_text("窗口 · 距离（厘米）","Window · distance (cm)"),40,300,^int{return vr_oot_win_dist();},^(int v){vr_oot_set_win_dist(v);})];
 {TOTKSettingRow *r=settings_row(settings_text("窗口 · 从背面看是深色背板","Window · a dark panel seen from behind"),nil,nil,^int{return vr_oot_win_back();},^(int v){vr_oot_set_win_back(v);});
  r.enabled=^BOOL{return vr_oot_screen_shape()==1;};[rows addObject:r];}
 {TOTKSettingRow *r=[TOTKSettingRow new];r.title=settings_text("窗口 · 重新摆放","Window · place again");
  r.text=^NSString *{return settings_text("摆到你现在的面前","Before you, where you are now");};r.buttonTitle=settings_text("摆放","Place");
  r.buttonAction=^{vr_oot_window_place();settings_refresh_all();};r.enabled=^BOOL{return vr_oot_screen_shape()==1;};[rows addObject:r];}
}
