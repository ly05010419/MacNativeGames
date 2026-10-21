#include "../games/totk/runtime/control_policy.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_present_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 unsigned sizes[][2]={{1280,720},{1600,900},{1920,1080},{2560,1440}};
 for(unsigned i=0;i<4;i++){uint64_t previous=0;for(int q=0;q<4;q++){
  uint64_t r=totk_control_render(sizes[i][0],sizes[i][1],q);unsigned w=(unsigned)r,h=(unsigned)(r>>32);
  assert(w*9==h*16&&w<=sizes[i][0]&&h<=sizes[i][1]&&w>=640&&h>=360);
  uint64_t pixels=(uint64_t)w*h;if(q)assert(pixels<previous);previous=pixels;
 }}
 assert(totk_control_render(1920,1080,1)==((uint64_t)720<<32|1280));
 assert(totk_control_render(1920,1080,3)==((uint64_t)540<<32|960));
 assert(!totk_control_render(8000,4500,1)&&!totk_control_render(1920,1080,4));
 assert(totk_control_period(0,0,120)==0); // truly no application pacing limit
 assert(totk_control_period(1,0,120)==1000000000ull/120);
 assert(totk_control_period(0,40,120)==25000000); // non-divisor caps keep own clock
 assert(totk_control_period(1,40,120)==25000000);
 assert(totk_control_period(1,144,120)==1000000000ull/120);
 assert(totk_control_period(0,144,120)==1000000000ull/144);
 puts("PASS: true render-pixel reduction, exact aspect ratio, independent vsync and caps");
}
