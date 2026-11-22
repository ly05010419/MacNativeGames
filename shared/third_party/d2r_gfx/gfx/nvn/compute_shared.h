// A compute kernel's shared memory, for every Zelda-line port's runtime translation (totk_shader_rt.m).
//
// The translator declares a kernel's shared memory from the request's "shared" bytes (totk_xlate Program.cs
// QueryComputeSharedMemorySize) and falls back to 4 KiB when it is not given. NVN keeps the size in the compute control
// section at +0x754 (nxnative shader_split.py, checked against PC reflection on 22 Diablo II kernels). Without it a kernel
// that needs more writes outside its threadgroup array: Age of Calamity's first-battle particle kernel dc81d690 (68 bytes
// per thread from byte 304, 64 threads: 4656 bytes) corrupted the counts its loops run to and the GPU never finished;
// the maintainer's MacBook Air panicked five times (2026-11-22).
//
// Only kernels above the default change: their request carries the size and a cached translation whose threadgroup array
// is smaller is translated again. Every other translation stays byte-identical, so existing caches and seeds stay valid.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TOTK_RT_SHARED_DEFAULT 4096u

// Bytes of shared memory the kernel declares (control+0x754, rounded up to 16); 0 when absent or above Metal's 32 KiB.
static inline uint32_t totk_rt_compute_shared(const uint8_t *control) {
    uint32_t v; memcpy(&v, control + 0x754, 4);
    return v && v <= 32768u ? (v + 15u) / 16u * 16u : 0;
}

// The size a translation request must carry: only above the translator's default (0 = leave the request as it was).
static inline uint32_t totk_rt_compute_shared_request(const uint8_t *control) {
    uint32_t v = totk_rt_compute_shared(control);
    return v > TOTK_RT_SHARED_DEFAULT ? v : 0;
}

// 1 when a translated kernel's MSL gives its threadgroup arrays at least `need` bytes (or need fits the default, or the
// kernel declares no threadgroup array): the largest "threadgroup spvUnsafeArray<uint, N>" counts.
static inline int totk_rt_msl_shared_ok(const char *msl, uint32_t need) {
    if (need <= TOTK_RT_SHARED_DEFAULT || !msl) return 1;
    static const char tag[] = "threadgroup spvUnsafeArray<uint, ";
    unsigned long most = 0; int any = 0;
    for (const char *p = strstr(msl, tag); p; p = strstr(p + 1, tag)) {
        unsigned long n = strtoul(p + sizeof tag - 1, NULL, 10);
        any = 1; if (n > most) most = n;
    }
    return !any || most * 4 >= need;
}
