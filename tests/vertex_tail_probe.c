#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_vertex_tail.h"
#include <assert.h>
#include <stdio.h>

#define INPUT "struct Inputs { float4 pos [[attribute(0)]]; uint4 color [[attribute(4)]]; }; "
#define MAIN "vertex Out main0(Inputs in [[stage_in]]) { "

int main(void) {
    assert(nvn_vertex_components(INPUT MAIN "return float4(in.pos.xyz, 1); }", 0) == 3);
    assert(nvn_vertex_components(INPUT MAIN "return in.pos; }", 0) == 4);
    assert(nvn_vertex_components(INPUT MAIN "return in.pos.w; }", 0) == 4);
    assert(nvn_vertex_components(INPUT MAIN "return in.pos[0]; }", 0) == 4);
    assert(nvn_vertex_components(INPUT MAIN "return helper(in); }", 0) == 4);
    assert(nvn_vertex_components(INPUT MAIN "return in.pos.z + in.pos.x; }", 0) == 3);
    assert(nvn_vertex_components(INPUT MAIN "return in.color.rgb; }", 4) == 3);
    assert(nvn_vertex_components(INPUT MAIN "return in.color.a; }", 4) == 4);
    assert(nvn_vertex_components(INPUT MAIN "return in.color; }", 0) == 0);
    assert(nvn_vertex_components(INPUT MAIN "/* in.pos.w */ return in . pos . xy; // in.pos.w\n}", 0) == 2);
    assert(nvn_vertex_components(INPUT MAIN "const char *s = \"in.pos.w\"; return in.pos.xyz; }", 0) == 3);
    assert(nvn_vertex_components(INPUT MAIN "return in.pos; }", 8) == 4);
    assert(nvn_vertex_components(NULL, 0) == 4);
    assert(nvn_vertex_components("unexpected source", 0) == 4);
    assert(nvn_vertex_components("struct Other { float4 pos [[attribute(0)]]; }; "
                                 INPUT MAIN "return in.pos.x; }", 0) == 1);
    assert(nvn_vertex_tail_format(0x29, 0, 6, 3) == 0x1d);  /* Half4 -> Half3 */
    assert(nvn_vertex_tail_format(0x25, 9, 12, 3) == 0x19); /* UChar4Normalized -> UChar3Normalized */
    assert(nvn_vertex_tail_format(0x29, 0, 6, 4) == 0x29);  /* fourth component is read */
    assert(nvn_vertex_tail_format(0x29, 0, 0, 0) == 0x29);  /* constant attribute */
    assert(nvn_vertex_tail_format(0x29, 6, 6, 0) == 0x29);  /* offset beyond stride */
    assert(nvn_vertex_tail_format(0x29, 5, 6, 0) == 0x29);  /* not even one half fits */
    assert(nvn_vertex_tail_format(0x29, 0, 8, 3) == 0x29);  /* already fits */
    assert(nvn_vertex_tail_format(0x29, 0, 4, 3) == 0x29);  /* a read lane would be removed */
    assert(nvn_vertex_tail_format(0x29, 0, 4, 2) == 0x11);  /* Half4 -> Half2 */
    assert(nvn_vertex_tail_format(0x51 + 18, 0, 3, 3) == 0x51 + 12); /* integer-to-float */
    assert(nvn_vertex_tail_format(0x40, 0, 3, 0) == 0x40); /* packed/unknown format */
    puts("vertex tail checks passed");
    return 0;
}
