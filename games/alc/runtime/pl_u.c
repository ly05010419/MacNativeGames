/* The pl:u service (shared fonts). Astral Chain's older SDK drives it over IPC —
 * nn::pl::RequestSharedFontLoad then GetSharedFont (command 6, with a language code) — and reads the
 * SharedFontData entries' memory addresses directly. The runtime maps guest memory in the same address
 * space as the host, so the entries can carry the shared_font.c region's real pointers; no shared-memory
 * mapping dance is needed. GetSharedFontLoadState/GetSharedFontAddress/GetSize, the non-IPC wrappers, are
 * already answered by name in shared_font.c. Commands before they are implemented dump and stop loudly. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>

const void *runtime_shared_font_address(int type);
size_t runtime_shared_font_size(int type);
unsigned runtime_shared_font_count(void);
uint32_t runtime_shared_font_base(void);
uint32_t runtime_shared_font_offset(int type);

static void pl_u_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    unsigned type = rq->data_size >= 4 ? *(const unsigned *)ipc_arg(rq, 0, 4) : 0;
    switch (rq->cmd) {
    case 0: {                                                                /* RequestLoad(type) */
        fprintf(stderr, "[totk-ipc] pl:u RequestLoad font %u\n", type);
        if ((int)type < 0 || type >= runtime_shared_font_count()) runtime_fail("pl:u font type out of range");
        return;
    }
    case 1: {                                                                /* GetLoadState(type) */
        uint32_t state = 1;                                                  /* SharedFontLoadState_Loaded */
        ipc_out(rs, &state, sizeof state);
        return;
    }
    case 2: {                                                                /* GetSize(type) */
        uint32_t size = (uint32_t)runtime_shared_font_size((int)type);
        ipc_out(rs, &size, sizeof size);
        return;
    }
    case 3: {                                                                /* Load(type, buffer) */
        const IpcBuffer *b = rq->na ? &rq->a[0] : rq->nx ? &rq->x[0] : NULL;
        if (!b) ipc_unimplemented(s, rq);
        size_t size = runtime_shared_font_size((int)type);
        if (b->size < size) runtime_fail("pl:u Load buffer smaller than the font");
        memcpy(ipc_guest(b->addr, b->size), runtime_shared_font_address((int)type), size);
        if (rq->na) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, size, b->index};
        fprintf(stderr, "[totk-ipc] pl:u Load font %u (%zu bytes)\n", type, size);
        return;
    }
    case 4: {                                                                /* GetSharedMemoryAddress */
        uint32_t base = runtime_shared_font_base();
        ipc_out(rs, &base, sizeof base);
        fprintf(stderr, "[totk-ipc] pl:u shared memory base = %#x\n", base);
        return;
    }
    case 5: {                                                                /* GetSharedFontInOrderOfPriority:
                                                                                in-raw {…}, B: types/offsets/sizes */
        fprintf(stderr, "[totk-ipc] pl:u cmd 5 in-raw:");
        for (unsigned i = 0; i + 4 <= rq->data_size && i < 20; i += 4) {
            uint32_t v; memcpy(&v, rq->data + i, 4);
            fprintf(stderr, " %08x", v);
        }
        fputc('\n', stderr);
        unsigned count = runtime_shared_font_count();
        uint32_t listed = rq->data_size >= 4 ? *(const uint32_t *)ipc_arg(rq, 0, 4) : count;
        if (listed > count) listed = count;
        const IpcBuffer *bufs[3] = {rq->nb > 0 ? &rq->b[0] : NULL, rq->nb > 1 ? &rq->b[1] : NULL, rq->nb > 2 ? &rq->b[2] : NULL};
        for (unsigned i = 0; i < listed; i++) {
            if (bufs[0]) *(uint32_t *)ipc_guest(bufs[0]->addr + 4 * i, 4) = i;                       /* font type */
            if (bufs[1]) *(uint32_t *)ipc_guest(bufs[1]->addr + 4 * i, 4) = runtime_shared_font_offset((int)i);
            if (bufs[2]) *(uint32_t *)ipc_guest(bufs[2]->addr + 4 * i, 4) = (uint32_t)runtime_shared_font_size((int)i);
        }
        ipc_out(rs, &listed, sizeof listed);
        fprintf(stderr, "[totk-ipc] pl:u priority list: %u fonts (base %#x)\n", listed, runtime_shared_font_base());
        return;
    }
    case 6: case 9: case 10: {                                               /* GetSharedFont(InOrderOfPriority*) */
        unsigned count = runtime_shared_font_count();
        uint8_t raw[4 + 4 + 6 * 16] = {0};
        memcpy(raw, &count, sizeof count);
        raw[4] = 1;                                                          /* SharedFontLoadState_Loaded */
        uint8_t *entry = raw + 8;
        for (unsigned i = 0; i < count; i++, entry += 16) {
            const void *addr = runtime_shared_font_address((int)i);
            size_t size = runtime_shared_font_size((int)i);
            memcpy(entry, &addr, sizeof addr);
            memcpy(entry + 8, &size, sizeof size);
            memcpy(entry + 12, &i, sizeof i);
        }
        ipc_out(rs, raw, sizeof raw);
        fprintf(stderr, "[totk-ipc] pl:u GetSharedFont: %u fonts, %u bytes of pointers\n", count,
                (unsigned)(count * 16));
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_pl_u(void) { runtime_ipc_register("pl:u", pl_u_handler, NULL, NULL); }
