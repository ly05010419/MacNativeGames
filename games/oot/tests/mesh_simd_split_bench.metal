// PoC microbenchmark: Ocarina of Time mesh-stage structure at fed302d7 scale, Apple silicon.
// GROUPS=1117 threadgroups, 128 vertices x 10 float4 exported per group, ~13 KB device scratch
// slot taken/released with atomics, and 80 raw word reads per logical lane through an 8-word
// range cache (the port's raw-read path; 61 4-wide + 19 1-wide in the real shader).
// Entries: t32/t64/t128 = THREADS threads per group, reads issued per thread (as shipped at 32);
//          s64/s128    = reads of each logical lane staged once per SIMD group in threadgroup
//                        memory, then broadcast (split over 2 / 4 SIMD groups).
#include <metal_stdlib>
#include <metal_atomic>
using namespace metal;

struct V { float4 pos [[position]]; float4 a0 [[user(locn0)]]; float4 a1 [[user(locn1)]]; float4 a2 [[user(locn2)]]; float4 a3 [[user(locn3)]]; float4 a4 [[user(locn4)]]; float4 a5 [[user(locn5)]]; float4 a6 [[user(locn6)]]; float4 a7 [[user(locn7)]]; float4 a8 [[user(locn8)]]; };

struct RangeCache {                       // the port's raw read range cache (one 8-word block)
    ulong c_lo; uint c[8]; bool hit;
    uint read(device const uint* src, ulong a) {
        ulong base = a & ~7ul;
        if (!hit || base != c_lo) { for (int k = 0; k < 8; k++) c[k] = src[base + k]; c_lo = base; hit = true; }
        return c[a & 7u];
    }
};

#define VMAX 128
#define MESH_BODY(THREADS) \
    threadgroup uint my_slot; \
    if (lid == 0) { uint s; for (;;) { s = atomic_fetch_add_explicit((device atomic_uint*)hdr, 1u, memory_order_relaxed) & hdr[1]; \
        if (atomic_exchange_explicit((device atomic_uint*)(hdr + 4 + s), 1u, memory_order_relaxed) == 0) break; } my_slot = s; } \
    threadgroup_barrier(mem_flags::mem_threadgroup); \
    device uint* st = slot + 8192 + my_slot * hdr[2]; \
    for (uint i = lid; i < 3285u; i += (THREADS)) st[i] = 0u; \
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device); \
    if (lid == 0) st[0] = tg; \
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);

#define EXPORT_ALL(THREADS) \
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device); \
    for (uint n = lid; n < VMAX; n += (THREADS)) { \
        uint b = 36u + (n / 32u) * 800u + n % 32u; \
        V v; \
        v.pos = float4(as_type<float>(st[b+0u]), as_type<float>(st[b+32u]), as_type<float>(st[b+64u]), as_type<float>(st[b+96u])); \
        v.a0=float4(as_type<float>(st[b+128u]),0,0,1);v.a1=float4(as_type<float>(st[b+160u]),0,0,1);v.a2=float4(as_type<float>(st[b+192u]),0,0,1);v.a3=float4(as_type<float>(st[b+224u]),0,0,1);v.a4=float4(as_type<float>(st[b+256u]),0,0,1);v.a5=float4(as_type<float>(st[b+288u]),0,0,1);v.a6=float4(as_type<float>(st[b+320u]),0,0,1);v.a7=float4(as_type<float>(st[b+352u]),0,0,1);v.a8=float4(as_type<float>(st[b+384u]),0,0,1); \
        out.set_vertex(n, v); } \
    out.set_primitive_count(64u); \
    if (lid == 0) atomic_store_explicit((device atomic_uint*)(hdr + 4 + my_slot), 0u, memory_order_relaxed);

#define KERNEL_T(NAME, THREADS) \
[[mesh]] void NAME(device const uint* addr_table [[buffer(0)]], device const uint* src [[buffer(1)]], \
                   device uint* slot [[buffer(2)]], device uint* hdr [[buffer(3)]], \
                   uint tg [[threadgroup_position_in_grid]], uint lid [[thread_position_in_threadgroup]], \
                   mesh<V, void, VMAX, 64, metal::topology::triangle> out [[stage_out]]) { \
    MESH_BODY(THREADS) \
    constexpr int READS = 80 * 32 / (THREADS); \
    RangeCache rc; rc.hit = false; \
    float acc = 0; \
    uint logical = lid * 32 / (THREADS); \
    for (int k = 0; k < READS; k++) \
        acc += as_type<float>(rc.read(src, addr_table[(tg * 32 + logical) * 80 + k])); \
    st[100 + lid] = as_type<uint>(acc); \
    EXPORT_ALL(THREADS) \
}

// split variant: THREADS = 2/4 SIMD groups; the 80 reads of the logical lanes a SIMD group owns are
// issued once and staged in threadgroup memory; every lane then reads the staged words.
#define KERNEL_S(NAME, THREADS) \
[[mesh]] void NAME(device const uint* addr_table [[buffer(0)]], device const uint* src [[buffer(1)]], \
                   device uint* slot [[buffer(2)]], device uint* hdr [[buffer(3)]], \
                   uint tg [[threadgroup_position_in_grid]], uint lid [[thread_position_in_threadgroup]], \
                   mesh<V, void, VMAX, 64, metal::topology::triangle> out [[stage_out]]) { \
    constexpr int SIMD = (THREADS) / 32; \
    constexpr int READS = 80 * 32 / (THREADS); \
    constexpr int LANES_PER_GROUP = 32 / SIMD; \
    threadgroup uint tgstage[2560];   /* SIMD groups x LANES_PER_GROUP x 80 words <= 10 KB */ \
    MESH_BODY(THREADS) \
    uint group = lid / 32, in_group = lid % 32; \
    uint lane0 = group * LANES_PER_GROUP; \
    RangeCache rc; rc.hit = false; \
    float acc = 0; \
    for (int k = 0; k < READS; k++) { \
        uint ll = lane0 + (in_group % LANES_PER_GROUP); \
        uint w = rc.read(src, addr_table[(tg * 32 + ll) * 80 + k]); \
        tgstage[group * LANES_PER_GROUP * 80 + (in_group % LANES_PER_GROUP) * 80 + k] = w; \
        acc += as_type<float>(w); } \
    threadgroup_barrier(mem_flags::mem_threadgroup); \
    float acc2 = 0; \
    for (int k = 0; k < READS; k++) acc2 += as_type<float>(tgstage[group * LANES_PER_GROUP * 80 + (in_group % LANES_PER_GROUP) * 80 + k]); \
    st[100 + lid] = as_type<uint>(acc + acc2); \
    EXPORT_ALL(THREADS) \
}

KERNEL_T(t32, 32)
KERNEL_T(t64, 64)
KERNEL_T(t128, 128)
KERNEL_S(s64, 64)
KERNEL_S(s128, 128)

struct FOut { float4 c [[color(0)]]; };
[[fragment]] FOut frag_main(V v [[stage_in]]) { return FOut{ float4(1.0) }; }

// sgid-only variant: the body's lane id is thread_index_in_simdgroup only (as in the real guest
// mesh body, which also hardcodes a 32-lane stride), so extra threads per threadgroup duplicate
// the work instead of splitting it. Measured on the M2: g32 41.7, g64 36.4, g128 42.3 ms —
// the real shader shape gains almost nothing from more threads without an IR-level lane flattening,
// while the lid-strided bench body wins big (t128 16.6 ms, -69 %).
#define KERNEL_G(NAME, THREADS) \
[[mesh]] void NAME(device const uint* addr_table [[buffer(0)]], device const uint* src [[buffer(1)]], \
                   device uint* slot [[buffer(2)]], device uint* hdr [[buffer(3)]], \
                   uint tg [[threadgroup_position_in_grid]], uint lid [[thread_position_in_threadgroup]], \
                   uint sgid [[thread_index_in_simdgroup]], \
                   mesh<V, void, VMAX, 64, metal::topology::triangle> out [[stage_out]]) { \
    MESH_BODY(THREADS) \
    RangeCache rc; rc.hit = false; \
    float acc = 0; \
    for (int k = 0; k < 80; k++) \
        acc += as_type<float>(rc.read(src, addr_table[(tg * 32 + sgid) * 80 + k])); \
    st[100 + sgid] = as_type<uint>(acc); \
    EXPORT_ALL(THREADS) \
}
KERNEL_G(g32, 32)
KERNEL_G(g64, 64)
KERNEL_G(g128, 128)
