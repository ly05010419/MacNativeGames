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
    puts("vertex tail checks passed");
    return 0;
}
