#include "stick_response.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(stick_response_gain(0,0,0)==0);
    assert(stick_response_gain(.1f,0,.12f)==0);
    assert(stick_response_gain(.12f,0,.12f)==0);
    float g=stick_response_gain(1,1,.12f);
    assert(fabsf(g-sqrtf(.5f))<1e-6f);
    assert(stick_response_gain(NAN,0,.12f)==0);
    assert(stick_response_gain(1,0,1)==0);
    for(int d=0;d<=50;d+=5) for(int ix=-100;ix<=100;ix++) for(int iy=-100;iy<=100;iy++) {
        float x=ix/100.f,y=iy/100.f,dz=d/100.f;
        float gain=stick_response_gain(x,y,dz),ox=x*gain,oy=y*gain;
        assert(isfinite(gain)&&gain>=0);
        assert(hypotf(ox,oy)<=1.000001f);
        assert(fabsf(ox*y-oy*x)<1e-6f); /* direction unchanged */
        if(hypotf(x,y)<=dz) assert(ox==0&&oy==0);
    }
    assert(fabsf(.56f*stick_response_gain(.56f,0,.12f)-.5f)<1e-6f);
    int32_t x=32767,y=32767;
    stick_response_limit(&x,&y);
    assert(hypotf(x,y)<=32767 && x==y);
    x=123;y=-456;stick_response_limit(&x,&y);assert(x==123&&y==-456);
    puts("radial stick response: pass");
}
