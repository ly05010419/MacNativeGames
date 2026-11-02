#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_quad_strip.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { double x, y; } Point;
static double cross(Point a, Point b, Point p) {
    return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
}
static int inside(Point p, const Point *v, const uint32_t *indices, unsigned n) {
    for (unsigned i=0; i<n; i++)
        if (cross(v[indices[i]],v[indices[(i+1)%n]],p)>0) return 0;
    return 1;
}
int main(void) {
    for (uint32_t count=0; count<4; count++) assert(nvn_quad_strip_quads(count)==0);
    assert(nvn_quad_strip_quads(4)==1 && nvn_quad_strip_quads(5)==1);
    assert(nvn_quad_strip_quads(6)==2 && nvn_quad_strip_quads(7)==2);
    uint32_t indices[12]; nvn_quad_strip_indices(indices,2);
    Point vertices[]={{0,0},{0,2},{3,0},{3,2},{5,1},{5,3}};
    for (unsigned q=0; q<2; q++) {
        uint32_t quad[]={q*2,q*2+1,q*2+3,q*2+2};
        const uint32_t *tri=indices+q*6;
        /* Both triangles have the quad's winding and its provoking (last) vertex. */
        for (unsigned t=0; t<2; t++) {
            assert(cross(vertices[tri[t*3]],vertices[tri[t*3+1]],vertices[tri[t*3+2]])<0);
            assert(tri[t*3+2]==quad[2]);
        }
        /* The triangles must cover exactly the quad, including the second, sloping strip segment. */
        for (int y=-10; y<40; y++) for (int x=-10; x<60; x++) {
            Point p={(x+0.37)/10,(y+0.19)/10};
            assert(inside(p,vertices,quad,4)==
                (inside(p,vertices,tri,3)||inside(p,vertices,tri+3,3)));
        }
    }
    uint32_t n=65536; uint32_t *large=malloc((size_t)n*6*sizeof(*large)); assert(large);
    nvn_quad_strip_indices(large,n);
    for (uint32_t i=0; i<n*6; i++) assert(large[i]<2*n+2);
    assert(large[(n-1)*6+5]==2*n+1);
    free(large);
    /* The restart is deliberately at an odd position: each strip's pair alignment starts afresh. */
    const uint32_t expected[]={10,11,13,12,10,13,12,13,15,14,12,15};
    for (unsigned width=1; width<=4; width*=2) {
        uint32_t restart=width==1?UINT8_MAX:width==2?UINT16_MAX:UINT32_MAX;
        uint32_t values[]={1,2,3,restart,10,11,12,13,14,15,16,restart,20,21,22};
        uint8_t packed[sizeof(values)+1]; memset(packed,0,sizeof(packed));
        for (unsigned i=0; i<sizeof(values)/sizeof(*values); i++)
            memcpy(packed+1+i*width,&values[i],width);
        uint32_t output[40]; for (unsigned i=0;i<40;i++) output[i]=0xfeedface;
        size_t written=nvn_quad_strip_indexed(output,packed+1,15,width);
        assert(written==12 && !memcmp(output,expected,sizeof(expected)));
        assert(output[written]==0xfeedface);
        assert(nvn_quad_strip_indexed(output,packed+1,3,width)==0);
        for (unsigned i=0;i<15;i++) memcpy(packed+1+i*width,&restart,width);
        assert(nvn_quad_strip_indexed(output,packed+1,15,width)==0);
    }
    puts("quad strip coverage, winding, pair bounds and provoking vertices: pass");
}
