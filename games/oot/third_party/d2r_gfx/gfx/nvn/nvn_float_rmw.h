/* #2070: float adds on device memory, for a Mac whose Metal compiler cannot build them.

   The translator turns Ampere's RED.F32 / ATOM.F32 into OpAtomicFAddEXT (patch 0029), which SPIRV-Cross writes as
   atomic_fetch_add_explicit((device atomic_float*)&BUF._m0[I], V, memory_order_relaxed). On an Apple M5 with macOS
   26.6.2 the compiler service crashes on that construct in one of Ocarina of Time's shaders (#2070: AGX3 "cannot select
   ... llvm.agx3.disable.smashing.float"; atomic_float load / store / compare-exchange and integer adds compile). The
   runtime then builds that one shader again with every such add lowered here (nvn_mtl.m frmw_retry): the lanes of a
   SIMD group that add to the same word sum their values (simd_sum) and one lane adds the sum with a uint compare-exchange
   that reloads the word on failure; each lane gets back the word as it was plus the values of the lanes before it, an
   order the native add could have taken. No barrier, so it is legal wherever the add is. Measured on an M4 (1536x864
   threads, all adding to one word): exact; 18-29 ms against 0.11 ms native, which is why only a shader whose native form
   fails is lowered. A compare-exchange per lane was ~9 s there, and one that keeps comparing its first value never ends
   (the v1 of patch 0028 froze the GPU), so the loop must take the reloaded value (atomic_compare_exchange_weak_explicit
   writes it into `old`). Plain C: tests/float_rmw_lower_test.c checks the text, tests/float_rmw_gpu_test.m the result. */
#pragma once
#include <stdlib.h>
#include <string.h>

static const char k_frmw_call[] = "atomic_fetch_add_explicit((device atomic_float*)&";
static const char k_frmw_helpers[] =
    "\n/* MacNativeGames #2070: float adds on device memory without a float atomic (nvn_float_rmw.h). */\n"
    "static inline float oot_frmw_cas(device float *p, float v) {\n"
    "    device atomic_uint *a = (device atomic_uint *)p;\n"
    "    uint old = atomic_load_explicit(a, memory_order_relaxed);\n"
    "    while (!atomic_compare_exchange_weak_explicit(a, &old, as_type<uint>(as_type<float>(old) + v), memory_order_relaxed, memory_order_relaxed)) {}\n"
    "    return as_type<float>(old);\n"
    "}\n"
    "static inline float oot_frmw_add(device float *p, float v) {\n"
    "    ulong at = (ulong)p; uint lo = uint(at), hi = uint(at >> 32);\n"
    "    if (simd_all(lo == simd_broadcast_first(lo) && hi == simd_broadcast_first(hi))) {\n"
    "        float s = simd_sum(v), before = simd_prefix_exclusive_sum(v), base = 0.0f;\n"
    "        if (simd_is_first()) base = s != 0.0f ? oot_frmw_cas(p, s) : as_type<float>(atomic_load_explicit((device atomic_uint *)p, memory_order_relaxed));\n"
    "        return simd_broadcast_first(base) + before;\n"
    "    }\n"
    "    return oot_frmw_cas(p, v);\n"
    "}\n";

static inline int frmw_has_device_float_rmw(const char *msl) { return msl && strstr(msl, k_frmw_call) != NULL; }

/* The end of an argument starting at s: the first ',' or ')' outside brackets, or NULL. */
static inline const char *frmw_arg_end(const char *s) {
    int depth = 0;
    for (; *s; s++) {
        if (*s == '(' || *s == '[' || *s == '{') depth++;
        else if (*s == ')' || *s == ']' || *s == '}') { if (!depth) return s; depth--; }
        else if (*s == ',' && !depth) return s;
    }
    return NULL;
}

/* A malloc'd copy of msl with every device float add lowered (*count of them) and the helpers after "using namespace
   metal;", or NULL when there is none or the text is not in the form above (then the shader is left as it was). */
static inline char *frmw_lower(const char *msl, int *count) {
    *count = 0;
    const char *ns = msl ? strstr(msl, "using namespace metal;") : NULL;
    if (!ns || !frmw_has_device_float_rmw(msl)) return NULL;
    size_t cap = strlen(msl) + sizeof k_frmw_helpers + 64, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
#define FRMW_PUT(p, n) do { size_t n_ = (n); if (len + n_ + 1 > cap) { cap = (len + n_ + 1) * 2; char *g_ = (char *)realloc(out, cap); if (!g_) { free(out); return NULL; } out = g_; } \
                            memcpy(out + len, (p), n_); len += n_; } while (0)
    const char *ns_end = ns + strlen("using namespace metal;");
    FRMW_PUT(msl, (size_t)(ns_end - msl));
    FRMW_PUT(k_frmw_helpers, strlen(k_frmw_helpers));
    const char *s = ns_end;
    for (const char *at; (at = strstr(s, k_frmw_call));) {
        const char *ptr = at + strlen(k_frmw_call), *ptr_end = frmw_arg_end(ptr);
        if (!ptr_end || *ptr_end != ',' || ptr_end[1] != ' ') { free(out); return NULL; }
        const char *val = ptr_end + 2, *val_end = frmw_arg_end(val);
        static const char order[] = ", memory_order_relaxed)";
        if (!val_end || strncmp(val_end, order, strlen(order))) { free(out); return NULL; }
        FRMW_PUT(s, (size_t)(at - s));
        FRMW_PUT("oot_frmw_add(&", 14);
        FRMW_PUT(ptr, (size_t)(ptr_end - ptr));
        FRMW_PUT(", ", 2);
        FRMW_PUT(val, (size_t)(val_end - val));
        FRMW_PUT(")", 1);
        s = val_end + strlen(order);
        (*count)++;
    }
    FRMW_PUT(s, strlen(s));
#undef FRMW_PUT
    out[len] = 0;
    return out;
}
