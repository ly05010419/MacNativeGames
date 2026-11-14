// Primitive assembly for geometry compute. Values are capture-record indices;
// restart separates runs before strips/fans are assembled. No game-specific state.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static int nvn_geo_topology(int prim) {
    switch (prim) {
        case 0: return 0;
        case 1: case 2: case 3: return 1;
        case 10: case 11: return 2;
        case 4: case 5: case 6: case 7: return 3;
        case 12: return 4;
        default: return -1;
    }
}
static int nvn_geo_width(int topology) { static const int width[] = {1, 2, 4, 3, 6}; return topology >= 0 && topology < 5 ? width[topology] : 0; }

// Returns words required/written. SIZE_MAX means unsupported topology or overflow.
// dst == NULL counts only. A short destination is never partially written.
static size_t nvn_geo_assemble(int prim, const uint32_t *src, size_t count, int restart, uint32_t restart_index, uint32_t *dst, size_t capacity) {
    int width = nvn_geo_width(nvn_geo_topology(prim));
    if (!width) return SIZE_MAX;
    if (!restart && (prim == 0 || prim == 1 || prim == 4 || prim == 10 || prim == 12)) {   // lists: the map is the indices up to the last whole primitive
        size_t words = count / (size_t)width * (size_t)width;
        if (dst && words > capacity) return SIZE_MAX;
        if (dst) memcpy(dst, src, words * sizeof *src);
        return words;
    }
    if (dst) {
        size_t needed = nvn_geo_assemble(prim, src, count, restart, restart_index, NULL, 0);
        if (needed == SIZE_MAX || needed > capacity) return SIZE_MAX;
    }
    size_t words = 0;
    for (size_t begin = 0; begin < count;) {
        size_t end = begin;
        while (end < count && !(restart && src[end] == restart_index)) end++;
        size_t n = end - begin, np = 0;
        switch (prim) {
            case 0: np = n; break;
            case 1: np = n / 2; break;
            case 2: np = n > 1 ? n : 0; break;
            case 3: np = n > 1 ? n - 1 : 0; break;
            case 4: np = n / 3; break;
            case 5: case 6: np = n > 2 ? n - 2 : 0; break;
            case 7: np = n / 4 * 2; break;
            case 10: np = n / 4; break;
            case 11: np = n > 3 ? n - 3 : 0; break;
            case 12: np = n / 6; break;
        }
        if (np > (SIZE_MAX - words) / (size_t)width) return SIZE_MAX;
        if (dst && words + np * width > capacity) return SIZE_MAX;
        for (size_t i = 0; dst && i < np; i++) {
            size_t k[6] = {0};
            switch (prim) {
                case 0: k[0] = i; break;
                case 1: k[0] = 2*i; k[1] = 2*i+1; break;
                case 2: k[0] = i; k[1] = (i+1)%n; break;
                case 3: k[0] = i; k[1] = i+1; break;
                case 4: for (int c=0;c<3;c++) k[c]=3*i+c; break;
                case 5: k[0]=i+(i&1); k[1]=i+1-(i&1); k[2]=i+2; break;
                case 6: k[0]=0; k[1]=i+1; k[2]=i+2; break;
                case 7: k[0]=4*(i/2); k[1]=k[0]+1+(i&1); k[2]=k[1]+1; break;
                case 10: for (int c=0;c<4;c++) k[c]=4*i+c; break;
                case 11: for (int c=0;c<4;c++) k[c]=i+c; break;
                case 12: for (int c=0;c<6;c++) k[c]=6*i+c; break;
            }
            for (int c=0;c<width;c++) dst[words+i*width+c]=src[begin+k[c]];
        }
        words += np * width;
        begin = end < count ? end + 1 : end;
    }
    return words;
}

// One ordered compaction per draw. Output is a point/line/triangle list, so it
// does not depend on Metal primitive-restart behaviour. Preserve triangle-strip
// winding and the last vertex. Unwritten GS index slots are UINT_MAX.
// Large draws use three dispatches: geo_pack_count scans 64-group blocks,
// geo_pack_scan scans their totals in parallel and writes indirect arguments,
// then geo_pack_write adds local and block offsets. A draw of up to 1024 groups
// instead runs geo_pack_small, doing the ordered compaction in one workgroup.
// A single thread used to
// walk every group: with it, each frame that drew OCTOPATH TRAVELER II's 129,032-triangle cube shadow mesh
// (drawn in runs, nvn_geometry.inc geo_split) took about 300 ms; now only the first one is long.
// The kernels: dm/gfx/msl/geo_pack.metal (host_msl_lib "geo_pack")
typedef struct { uint64_t raw, mask, map, dst, args; uint32_t groups, stride, first, first_instance, capacity, collapse; } nvn_geo_lean_job;
// The capture's record description (dmsh_geometry.capture dm_geometry_capture), vertex buffer 13: where the records go,
// {first vertex, vertices, first instance, instances}, the record's words, and each capture word's slot in it (-1: not stored)
enum { GEO_CAPTURE_WORDS = 144 };
typedef struct { uint64_t dst, pad; uint32_t info[4]; uint32_t stride; int16_t slot[GEO_CAPTURE_WORDS]; } nvn_geo_capture;
_Static_assert(__builtin_offsetof(nvn_geo_capture, info) == 16 && __builtin_offsetof(nvn_geo_capture, stride) == 32 && __builtin_offsetof(nvn_geo_capture, slot) == 36, "dm_geometry_capture layout");
// The job record geo_pack reads (host side): the counts / offsets buffer holds a word per group
typedef struct { uint64_t src, dst, args; uint32_t groups, stride, topology, capacity; uint64_t cnt; } nvn_geo_pack_job;
