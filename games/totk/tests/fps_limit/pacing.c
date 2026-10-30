/* Calls production NVN pacing helpers; renderer imports are not exercised.
   No GPU, game files, sleeps or runtime-only test switches are needed. */
#include "../../third_party/d2r_gfx/gfx/nvn/hle_nvn.c"
#include <assert.h>
extern int test_game_hold;
static int test_mode, test_detclock;
int mfx_enabled(void) { return test_mode; }
int dm_detclock_enabled(void) { return test_detclock; }
int main(void) {
 const int rates[]={60,100,120,144};
 const int limits[]={0,30,40,60,72,90,120};
 int cases=0;
 for(unsigned r=0;r<sizeof rates/sizeof *rates;r++)
 for(int mode=MFX_OFF;mode<MFX_NMODES;mode++)
 for(unsigned i=0;i<sizeof limits/sizeof *limits;i++) {
  win_t w={.interval=1};
  g_hz=g_paced_hz=rates[r];g_uncapped=1;g_display_mode=0;g_uncapped_interval=1;
  test_mode=mode;test_game_hold=0;test_detclock=0;
  nvn_fps_lock_set(limits[i]);
  int hold=mode==MFX_30?30:mode==MFX_40?40:mode==MFX_50?50:limits[i];
  uint64_t expected=1000000000ull/(hold?hold:rates[r]);
  assert(present_period(&w)==expected);
  assert(mfx_own_clock()==(hold&&rates[r]%hold!=0));
  if(hold)assert(!uncapped_now());
  if(limits[i]==72&&rates[r]==144&&!mfx_hold_fps(mode))assert(present_ticks(&w)==2);
  test_game_hold=30;
  assert(game_hold_fps()==30 && !uncapped_now());
  assert(present_period(&w)==1000000000ull/30);
  assert(mfx_own_clock()==(rates[r]%30!=0));
  assert(nvn_fps_lock()==limits[i]);
  test_game_hold=0;
  assert(present_period(&w)==expected && nvn_fps_lock()==limits[i]);
  cases++;
 }
 nvn_fps_lock_set(72);nvn_fps_lock_set(73);assert(nvn_fps_lock()==72);
 printf("PASS: %d production pacing scenarios: game hold/release, all modes, 60/100/120/144 Hz, 72 FPS display ticks, invalid limit\n",cases);
}
