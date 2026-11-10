/* The MSL rewrite of #2070: a float add on device memory (what SPIRV-Cross emits for OpAtomicFAddEXT) becomes one
   compare-exchange per SIMD group. Text only, no GPU: float_rmw_gpu_test.m runs the result. */
#include "../third_party/d2r_gfx/gfx/nvn/nvn_float_rmw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what, const char *got) { fprintf(stderr, "FAIL: %s\n%s\n", what, got ? got : "(null)"); return 1; }

int main(void) {
    const char *src =
        "#include <metal_stdlib>\n#include <metal_atomic>\n\nusing namespace metal;\n\n"
        "int _54(thread const int& _659, thread const int& _660, device _30& cp_sf0)\n{\n"
        "    uint _663 = uint(int(uint(_659) >> uint(2)));\n"
        "    float _676 = atomic_fetch_add_explicit((device atomic_float*)&cp_sf0._m0[int(_663)], as_type<float>(_660), memory_order_relaxed);\n"
        "    float _677 = atomic_fetch_add_explicit((device atomic_float*)&cp_sf3._m0[f(a, b)[1]], (x, y), memory_order_relaxed);\n"
        "    return as_type<int>(_676);\n}\n";
    int n = 0;
    char *out = frmw_lower(src, &n);
    if (!out || n != 2) return fail("two adds lowered", out);
    if (strstr(out, "atomic_fetch_add_explicit((device atomic_float*)")) return fail("no native float add left", out);
    if (!strstr(out, "float _676 = oot_frmw_add(&cp_sf0._m0[int(_663)], as_type<float>(_660));")) return fail("first call", out);
    if (!strstr(out, "float _677 = oot_frmw_add(&cp_sf3._m0[f(a, b)[1]], (x, y));")) return fail("nested commas and brackets kept", out);
    const char *helper = strstr(out, "static inline float oot_frmw_add(");
    const char *ns = strstr(out, "using namespace metal;");
    if (!helper || !ns || helper < ns || helper > strstr(out, "int _54(")) return fail("helper after using namespace, before use", out);
    if (!strstr(out, "atomic_compare_exchange_weak_explicit")) return fail("compare-exchange that reloads", out);
    free(out);
    if (frmw_lower("kernel void k(device atomic_uint *a) { atomic_fetch_add_explicit(a, 1u, memory_order_relaxed); }", &n) || n != 0)
        return fail("integer atomics untouched", "");
    if (!frmw_has_device_float_rmw(src) || frmw_has_device_float_rmw("float x = 1.0;")) return fail("detection", "");
    char *bad = frmw_lower("using namespace metal;\nfloat a = atomic_fetch_add_explicit((device atomic_float*)&b[0], 1.0", &n);
    if (bad) return fail("an unterminated call is left alone", bad);
    puts("float rmw lowering: all checks pass");
    return 0;
}
