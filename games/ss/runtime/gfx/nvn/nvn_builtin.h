// The 4 Maxwell compute shaders embedded in the main program itself (no counterpart in the PC version) -> hand-written MSL, equivalent instruction by instruction to the envydis disassembly.
// Source: control segments in the .rodata of the original main image @0x1df10e8 / 0x1df1a94 / 0x1df2440 / 0x1df2dec (envydis -m gm107).
// Common skeleton (original Maxwell):
//   id = (ctaid.x << 5) + tid.x                       // thread group 32x1x1
//   if (id >= c3[0x0]) exit                           // element count = word 0 of uniform buffer 0 (u32 compare)
//   src = ssbo0 (c0[0x310]), dst = ssbo1 (c0[0x320]), address = base + 32-bit offset
// Differences between kernels (an NVN counter report is 16 bytes = {count u64, timestamp u64}):
//   q_val0   dst[id] = *(u64*)(src + (id<<4))                         take the count
//   q_val8   dst[id] = *(u64*)(src + (id<<4) + 8)                     take the timestamp
//   q_dlt0   dst[id] = *(u64*)(src + (id<<5) + 16) - *(u64*)(src + (id<<5))        count difference between two consecutive reports
//   q_dlt8   dst[id] = *(u64*)(src + (id<<5) + 24) - *(u64*)(src + (id<<5) + 8)    timestamp difference between two consecutive reports
#pragma once

// The kernels: dm/gfx/msl/builtin.metal (host_msl_lib "builtin")

// Switch control segment hash (first 16 hex digits of sha256(0x880 bytes of the control segment)) -> kernel name
static const struct { const char *ctl; const char *fn; } k_nvn_builtin_map[] = {
    {"d0f769bb916bbba9", "q_val0"},
    {"16479cc6d75c2b28", "q_val8"},
    {"0687e2ef95133577", "q_dlt0"},
    {"4df5909e6dabee05", "q_dlt8"},
};
