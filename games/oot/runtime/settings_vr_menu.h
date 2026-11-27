/* The VR menu in the headset (vr.md row 90; zeldaVR/gfx/oot_vr_menu_draw.h draws it, zeldaVR/runtime/oot_vr_menu.h lays it out) shows
   the VR page's own rows: this file hands them to the VR thread as oot_menu_row, and applies what the headset changes through each
   row's own set block, so the page is the one list (no second list of options). Also the page's rows for the menu itself and the
   big screen's drag (row 4). Included by settings_game.h before settings_vr_rows (GAME_OOT on the Mac), which calls
   settings_vr_menu_rows; the rows are built and their blocks run on the main thread only: the VR thread reads a copy, refreshed
   on the main thread when it asks (runtime_vr_menu_refresh), and its changes are queued to the main thread. */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include "../zeldaVR/runtime/oot_vr_menu.h"
int vr_oot_menu_button(void);
void vr_oot_set_menu_button(int v);
int vr_oot_menu_follow(void);
void vr_oot_set_menu_follow(int on);
int vr_oot_laser_deg(void);
void vr_oot_set_laser_deg(int v);
int vr_oot_screen_drag(void);
void vr_oot_set_screen_drag(int on);
int vr_oot_screen_yaw(void);
void vr_oot_set_screen_yaw(int v);
int vr_oot_screen_up(void);
void vr_oot_set_screen_up(int v);
static void settings_vr_rows(NSMutableArray<TOTKSettingRow *> *rows);
static void settings_camera_rows(NSMutableArray<TOTKSettingRow *> *rows);

static void settings_vr_menu_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 [rows addObject:settings_row(settings_text("头显里的 VR 菜单 · 打开按键","VR menu in the headset · open with"),@[settings_text("左摇杆单独按住 1 秒","Left stick click held alone 1 s"),
   settings_text("菜单键按住 1.5 秒（0.6 秒时照常回正）","Menu held 1.5 s (recentres at 0.6 s as before)"),settings_text("不用（只在 Mac 上设置）","None (settings on the Mac only)")],@[@1,@2,@0],
  ^int{return vr_oot_menu_button();},^(int v){vr_oot_set_menu_button(v);})];
 [rows addObject:settings_row(settings_text("VR 菜单 · 位置","VR menu · where"),@[settings_text("固定在打开时的面前","Fixed where it opened"),settings_text("跟着头（转开 20 度以上才跟）","Follows the head (past 20 degrees)")],@[@0,@1],
  ^int{return vr_oot_menu_follow();},^(int v){vr_oot_set_menu_follow(v);})];
 {TOTKSettingRow *r=settings_row(settings_text("激光的角度（度，+ 抬高）","Laser angle (degrees, + raises it)"),nil,nil,^int{return vr_oot_laser_deg();},^(int v){vr_oot_set_laser_deg(v);});
  r.sliderMin=-45;r.sliderMax=45;[rows addObject:r];}
 [rows addObject:settings_row(settings_text("大屏幕 · 激光对准后握把拖动，双握把缩放","Big screen · grip with the laser on it drags, both grips scale"),nil,nil,^int{return vr_oot_screen_drag();},^(int v){vr_oot_set_screen_drag(v);})];
 {TOTKSettingRow *r=settings_row(settings_text("大屏幕 · 左右转（度）","Big screen · turned (degrees)"),nil,nil,^int{return vr_oot_screen_yaw();},^(int v){vr_oot_set_screen_yaw(v);});
  r.sliderMin=-180;r.sliderMax=180;[rows addObject:r];}
 {TOTKSettingRow *r=settings_row(settings_text("大屏幕 · 上下（厘米）","Big screen · up / down (cm)"),nil,nil,^int{return vr_oot_screen_up();},^(int v){vr_oot_set_screen_up(v);});
  r.sliderMin=-150;r.sliderMax=150;[rows addObject:r];}
}

/* ---- the copy the VR thread reads ---- */
static NSArray<TOTKSettingRow *> *g_vrm_rows;   /* main thread */
static pthread_mutex_t g_vrm_mu = PTHREAD_MUTEX_INITIALIZER;
static oot_menu_row g_vrm_copy[OOT_MENU_ROWS_MAX]; static int g_vrm_n; static uint32_t g_vrm_version;   /* g_vrm_mu */
static _Atomic int g_vrm_asked;
static int settings_vrm_kind(TOTKSettingRow *r) {
 if(r.buttonAction||r.text||(r.choices.count&&!r.set))return OOT_MENU_INFO;
 if(r.slider&&!r.stops)return OOT_MENU_SLIDER;
 if(r.choices.count&&r.values.count==r.choices.count)return OOT_MENU_CHOICES;
 return r.choices?OOT_MENU_INFO:OOT_MENU_SWITCH;
}
static void settings_vrm_fill(TOTKSettingRow *r, oot_menu_row *o) {
 memset(o,0,sizeof *o);
 oot_menu_copy_utf8(o->title,sizeof o->title,r.title.UTF8String);
 o->kind=settings_vrm_kind(r);o->enabled=r.enabled?r.enabled():1;
 const int v=r.get?r.get():0;NSString *text=nil;
 if(o->kind==OOT_MENU_SLIDER){o->now=v;o->min=r.sliderMin;o->max=r.sliderMax;text=[NSString stringWithFormat:@"%d",v];}
 else if(o->kind==OOT_MENU_CHOICES){NSUInteger i=[r.values indexOfObject:@(v)];o->now=i==NSNotFound?-1:(int)i;o->max=(int)r.choices.count-1;
  text=i==NSNotFound?@"—":(i==0&&r.firstTitle?r.firstTitle():r.choices[i]);}
 else if(o->kind==OOT_MENU_SWITCH){o->now=v!=0;o->max=1;text=settings_on_off(v);}
 else{text=r.text?r.text():r.choices.count?r.choices[0]:@"";o->enabled=0;}
 oot_menu_copy_utf8(o->value,sizeof o->value,text.UTF8String);
}
/* Main thread: the rows built once (the VR page's, its order), every value read again; the version goes up when one changed. */
static void settings_vrm_snapshot(void) {
 if(!g_vrm_rows){NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];settings_camera_rows(rows);settings_vr_rows(rows);g_vrm_rows=rows;
  fprintf(stderr,"[vr-menu] the VR page's %lu rows for the headset's menu\n",(unsigned long)rows.count);}
 static oot_menu_row fresh[OOT_MENU_ROWS_MAX];int n=0;
 for(TOTKSettingRow *r in g_vrm_rows){if(n>=OOT_MENU_ROWS_MAX){static int said;if(!said++)fprintf(stderr,"[vr-menu] more than %d rows: the rest are not shown\n",OOT_MENU_ROWS_MAX);break;}
  settings_vrm_fill(r,&fresh[n++]);}
 pthread_mutex_lock(&g_vrm_mu);
 if(n!=g_vrm_n||memcmp(fresh,g_vrm_copy,sizeof fresh[0]*(size_t)n)){memcpy(g_vrm_copy,fresh,sizeof fresh[0]*(size_t)n);g_vrm_n=n;g_vrm_version++;}
 pthread_mutex_unlock(&g_vrm_mu);
}
/* Any thread: ask for the values to be read again (once at a time). */
void runtime_vr_menu_refresh(void) {
 if(!vr_enabled())return;
 int no=0;if(!atomic_compare_exchange_strong(&g_vrm_asked,&no,1))return;
 dispatch_async(dispatch_get_main_queue(),^{settings_vrm_snapshot();atomic_store(&g_vrm_asked,0);});
}
/* The VR thread: the rows as last read into out (up to max), and their version; 0 rows until the main thread has read them. */
int runtime_vr_menu_rows(oot_menu_row *out, int max, uint32_t *version) {
 pthread_mutex_lock(&g_vrm_mu);
 const int n=g_vrm_n<max?g_vrm_n:max;if(out&&n>0)memcpy(out,g_vrm_copy,sizeof out[0]*(size_t)n);if(version)*version=g_vrm_version;
 pthread_mutex_unlock(&g_vrm_mu);
 return n;
}
/* The VR thread: change row index (how: OOT_MENU_STEP by value +-1, OOT_MENU_SET a slider to value), on the main thread through
   the row's own block, as the page would; the Mac's page shows it at once. */
void runtime_vr_menu_change(int index, int how, int value) {
 dispatch_async(dispatch_get_main_queue(),^{
  if(!g_vrm_rows||index<0||(NSUInteger)index>=g_vrm_rows.count)return;
  TOTKSettingRow *r=g_vrm_rows[(NSUInteger)index];const int kind=settings_vrm_kind(r);
  if(!r.set||kind==OOT_MENU_INFO||(r.enabled&&!r.enabled()))return;
  const int v=r.get?r.get():0;int to=v;
  if(kind==OOT_MENU_SWITCH)to=!v;
  else if(kind==OOT_MENU_CHOICES){const int n=(int)r.values.count;NSUInteger i=[r.values indexOfObject:@(v)];
   int k=i==NSNotFound?(value>0?0:n-1):((int)i+(value>0?1:-1)+n)%n;to=r.values[(NSUInteger)k].intValue;}
  else if(kind==OOT_MENU_SLIDER){to=how==OOT_MENU_SET?value:v+(value>0?1:-1)*oot_menu_slider_step(r.sliderMin,r.sliderMax);
   to=to<r.sliderMin?r.sliderMin:to>r.sliderMax?r.sliderMax:to;}
  if(to!=v||kind==OOT_MENU_SWITCH){r.set(to);fprintf(stderr,"[vr-menu] %s: %d -> %d\n",r.title.UTF8String,v,to);}
  settings_refresh_all();settings_vrm_snapshot();
 });
}
