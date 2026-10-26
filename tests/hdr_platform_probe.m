/* Use the real support gate and interpolation implementation; no game data or GPU. */
#define TOTK_HDR_FRONTEND 1
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_mfx_mode.h"
#include <assert.h>
static int enabled;
int front_hdr_supported(void){return totk_hdr_available();}
int front_hdr_enabled(void){return enabled;}
void mfx_set(int mode);
int mfx_latch(void);
int mfx_requested(void);
int main(void){
#if defined(TOTK_GAME_V143) && !defined(GAME_BOTW)
 assert(totk_hdr_available());
#else
 assert(!totk_hdr_available());
#endif
 mfx_set(MFX_40);mfx_latch();assert(mfx_enabled()==MFX_40);
 enabled=1;mfx_latch();
 assert(mfx_enabled()==(totk_hdr_available()?MFX_OFF:MFX_40));
 assert(mfx_requested()==MFX_40);
 enabled=0;mfx_latch();assert(mfx_enabled()==MFX_40);
 return 0;
}
