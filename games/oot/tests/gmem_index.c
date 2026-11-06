/* The directory is only an accelerator: every address must select the same
 * original range as an independent full-table predecessor search. */
#include "../third_party/d2r_gfx/gfx/nvn/nvn_gmem_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static uint64_t rng = 0x371db34289132ull;
static uint64_t random64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t oracle(const uint64_t *t, uint32_t used, uint64_t address) {
    uint32_t lo=0, hi=used;
    while(lo+1<hi){uint32_t mid=lo+(hi-lo)/2;if(t[(size_t)mid*4]<=address)lo=mid;else hi=mid;}
    return address<t[(size_t)lo*4+1]?lo:0;
}
static uint32_t shader_search(const uint64_t *t, uint32_t capacity, uint64_t address) {
    const uint32_t *h=(const uint32_t *)t;
    uint64_t base=(uint64_t)h[6]|((uint64_t)h[7]<<32);
    uint64_t offset=(address>>GMEM_INDEX_SHIFT)-base;
    uint32_t index=0, step=capacity/2, last=capacity-1;
    if(offset<h[5]){const uint32_t *b=h+(size_t)capacity*8+(size_t)offset*3;index=b[0];step=b[1];last=b[2];}
    while(step){uint32_t candidate=index+step;if(candidate>last)candidate=last;
        if(t[(size_t)candidate*4]<=address)index=candidate;step>>=1;}
    return address<t[(size_t)index*4+1]?index:0;
}
int main(void) {
    size_t checked=0;
    const uint64_t bases[]={UINT64_C(0x300000000),UINT64_C(0xfffffff0),UINT64_C(0x3fffffffffff00),UINT64_C(0xffffffff00000000)};
    for(unsigned layout=0;layout<8;layout++){
        uint32_t used=8194,cap=16384;uint64_t start=bases[layout%4],p=start;
        uint64_t *t=calloc((size_t)cap*32+GMEM_INDEX_MAX_BUCKETS*12,1);assert(t);
        ((uint32_t *)t)[4]=cap;
        for(uint32_t i=1;i<used;i++){
            uint64_t length=layout<4?128:1+(random64()%10000);
            t[(size_t)i*4]=p;t[(size_t)i*4+1]=p+length;
            p+=length+random64()%1000;
        }
        for(uint32_t i=used;i<cap;i++)t[(size_t)i*4]=UINT64_MAX;
        uint32_t count=gmem_index_count(start,p);assert(count);
        gmem_index_build(t,used,cap,count);
        for(unsigned mode=0;mode<2;mode++){
            ((uint32_t *)t)[5]=mode?count:0;
            for(uint32_t i=1;i<used;i++)for(int edge=-1;edge<=1;edge++)for(int end=0;end<2;end++){
                uint64_t a=t[(size_t)i*4+end]+edge;
                assert(shader_search(t,cap,a)==oracle(t,used,a));checked++;
            }
            for(unsigned i=0;i<100000;i++){
                uint64_t a=i&1?start-0x1000+random64()%(p-start+0x2000):random64();
                assert(shader_search(t,cap,a)==oracle(t,used,a));checked++;
            }
        }
        free(t);
    }
    assert(gmem_index_count(0,UINT64_MAX)==0);
    assert(gmem_index_count(1,(uint64_t)GMEM_INDEX_MAX_BUCKETS<<GMEM_INDEX_SHIFT)==GMEM_INDEX_MAX_BUCKETS);
    assert(gmem_index_count(1,((uint64_t)GMEM_INDEX_MAX_BUCKETS<<GMEM_INDEX_SHIFT)+1)==0);
    /* Production memory can contain guest and host pools over 256 GiB apart.
       The earlier 65536-bucket limit silently disabled the directory there. */
    uint32_t count=gmem_index_count(UINT64_C(0x100000000),UINT64_C(0x7100001000));
    assert(count>65536 && count<=GMEM_INDEX_MAX_BUCKETS);
    uint64_t *wide=calloc(4*32+(size_t)count*12,1);assert(wide);
    ((uint32_t *)wide)[4]=4;
    wide[4]=UINT64_C(0x100000000);wide[5]=wide[4]+4096;
    wide[8]=UINT64_C(0x7100000000);wide[9]=wide[8]+4096;
    wide[12]=UINT64_MAX;gmem_index_build(wide,3,4,count);
    for(unsigned i=0;i<100000;i++){
        uint64_t address=i<12?wide[(i/6+1)*4+(i/3)%2]+(int)(i%3)-1:random64()%wide[9];
        assert(shader_search(wide,4,address)==oracle(wide,3,address));checked++;
    }
    free(wide);
    printf("raw address directory: %zu independent predecessor checks passed\n",checked);
    return 0;
}
