/* Production scene-cap validation and aspect selection with synthetic game state. No game data or executable patching. */
#include "../../runtime/game_patches.c"
#include "../../runtime/odyssey_aspect.c"
#include <assert.h>
void runtime_heap_range(uint8_t **base,size_t *size){*base=NULL;*size=0;}
void settings_test_output_init(void){g_render_w=2560;g_render_h=1440;atomic_store(&g_scene_cap,1600u|900u<<16);}
void settings_test_output_size(unsigned w,unsigned h){assert(runtime_scene_resolution_set(w,h));}
void settings_test_aspect_init(double view){static Module main_module;g_aspect_main=&main_module;g_wide=21.0f/9;atomic_store(&g_step,ASPECT_STEP_GAME);runtime_aspect_view(view*1080,1080);}
void settings_test_aspect_frame(void){atomic_store(&g_shown,atomic_load(&g_want));}
