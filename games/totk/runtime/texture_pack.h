/* TOTK_4K=1 (Mac, 1.4.3): the HDPacks.com 4K texture pack is installed (scripts/make_texture_pack_tables.py, docs/4k-texture-pack.md).
   Its HeapDef grows TextureHeap and the grass heap, the application pool grows by the same (TEXPACK_EXTRA_KIB), and the HeapDef
   overlay is the pack's with the output's growth added (scripts/make_output_res_overlay.py --base-heapdef). */
#pragma once
#include <TargetConditionals.h>
#include <stdlib.h>
#include <string.h>

#define TEXPACK_EXTRA_KIB (3419000 + 10000)   /* TextureHeap 506880 -> 3925880 KiB, grass 9318 -> 19318 KiB */

static inline int texture_pack_requested(void) {
#if TARGET_OS_IPHONE
    return 0;
#else
    const char *e = getenv("TOTK_4K");
    return e && *e && strcmp(e, "0") && !getenv("TOTK_EXPORT_IOS");
#endif
}
