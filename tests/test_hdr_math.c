/* Invariants independent of the renderer: reference white, monotonicity, safe
 * fallback and out-of-range inputs. The GPU probe tests the production MSL. */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr.h"
#include <assert.h>
int main(void){
 assert(totk_hdr_rolloff(NAN,4)==0);assert(totk_hdr_rolloff(INFINITY,4)==0);assert(totk_hdr_rolloff(-1,4)==0);
 for(int h=1;h<=16;h++){
  assert(totk_hdr_rolloff(0,h)==0);assert(totk_hdr_rolloff(1,h)==1);
  assert(totk_hdr_rolloff(.18f,h)==.18f);
  float last=0;
  for(int i=0;i<10000;i++){float p=i*.01f,x=totk_hdr_rolloff(p,h);assert(isfinite(x)&&x>=last-1e-6f&&x<=h+1e-5f&&x<=p+1e-5f);last=x;}
 }
 assert(totk_hdr_rolloff(10,0)==1);assert(totk_hdr_rolloff(10,NAN)==1);
 assert(totk_hdr_rolloff(2,8)==2);
 return 0;
}
