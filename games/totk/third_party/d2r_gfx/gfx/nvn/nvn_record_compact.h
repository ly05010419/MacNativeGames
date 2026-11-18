/* Lossless draw recording, inspired by seedds/nxnative f02d0474 (MIT).
   Only the recorder's reusable scratch is compact. EndRecording expands it to
   the canonical, patchable handle representation; replay needs no format fork.
   Hardware command-memory accounting is independent of this host encoding. */
#define NVN_COMPACT_DRAW 0x80000000u
static _Atomic int g_record_compact = -1;
void nvn_record_compact_set(int on) { atomic_store(&g_record_compact,on!=0); }
static int record_compact_on(void) {
    int on = atomic_load(&g_record_compact);
    if (on < 0) { const char *e = getenv("TOTK_RECORD_COMPACT"); int value=e&&atoi(e),expected=-1;
        atomic_compare_exchange_strong(&g_record_compact,&expected,value); on=atomic_load(&g_record_compact); }
    return on;
}
static uint32_t record_draw_pack(uint8_t out[48], const p_draw *d, const p_draw_idx *i) {
    uint32_t kind, op; int prim = d ? d->prim : i->prim;
    if (prim < 0 || prim > 255 || (i && (i->itype < 0 || i->itype > 255))) return 0;
    memset(out, 0, 48);
    if (d) {
        if (d->pad_) return 0;
        if (d->ind) {
            if (d->first || d->base_instance || d->instances != 1 || d->count != (prim == 0x40)) return 0;
            kind = 3; memcpy(out + 8, &d->ind, 8);
        } else {
            kind = d->base_instance || d->instances != 1 ? 2 : 1;
            memcpy(out + 8, &d->first, 4); memcpy(out + 12, &d->count, 4);
            if (kind == 2) { memcpy(out + 16, &d->base_instance, 4); memcpy(out + 20, &d->instances, 4); }
        }
    } else {
        if (i->ind) {
            if (i->count || i->base_vertex || i->base_instance || i->instances != 1) return 0;
            kind = 6; memcpy(out + 8, &i->ibuf, 8); memcpy(out + 16, &i->ind, 8);
        } else {
            kind = i->base_instance || i->instances != 1 ? 5 : 4;
            memcpy(out + 8, &i->count, 4); memcpy(out + 12, &i->base_vertex, 4);
            memcpy(out + 16, &i->ibuf, 8);
            if (kind == 5) { memcpy(out + 24, &i->base_instance, 4); memcpy(out + 28, &i->instances, 4); }
        }
    }
    static const uint8_t sizes[] = {0,16,24,16,24,32,24};
    op = NVN_COMPACT_DRAW | kind | ((uint32_t)prim << 8) | (i ? (uint32_t)i->itype << 16 : 0);
    op_hdr h = {op,sizes[kind]}; memcpy(out,&h,8); return sizes[kind];
}
static uint32_t record_draw_expand(const uint8_t *in, uint8_t out[48]) {
    op_hdr h; memcpy(&h, in, 8);
    if (!(h.op & NVN_COMPACT_DRAW)) return 0;
    uint32_t k = h.op & 255, prim = (h.op >> 8) & 255;
    static const uint8_t sizes[] = {0,16,24,16,24,32,24};
    if (k < 1 || k > 6 || h.size != sizes[k] || (h.op & 0x7f000000u)) die("invalid compact draw");
    memset(out, 0, 48);
    if (k <= 3) {
        p_draw d = {.prim=(int)prim,.instances=1};
        if (k == 3) { memcpy(&d.ind, in + 8, 8); d.count = prim == 0x40; }
        else { memcpy(&d.first, in + 8, 4); memcpy(&d.count, in + 12, 4);
            if (k == 2) { memcpy(&d.base_instance, in + 16, 4); memcpy(&d.instances, in + 20, 4); } }
        h=(op_hdr){OP_DRAW,8+sizeof d}; memcpy(out,&h,8); memcpy(out + 8, &d, sizeof d); return 8 + sizeof d;
    }
    p_draw_idx i = {.prim=(int)prim,.itype=(h.op >> 16)&255,.instances=1};
    if (k == 6) { memcpy(&i.ibuf, in + 8, 8); memcpy(&i.ind, in + 16, 8); }
    else { memcpy(&i.count, in + 8, 4); memcpy(&i.base_vertex, in + 12, 4); memcpy(&i.ibuf, in + 16, 8);
        if (k == 5) { memcpy(&i.base_instance, in + 24, 4); memcpy(&i.instances, in + 28, 4); } }
    h=(op_hdr){OP_DRAW_IDX,8+sizeof i}; memcpy(out,&h,8); memcpy(out + 8, &i, sizeof i); return 8 + sizeof i;
}
static void record_draw(NVNcommandBuffer *c, const p_draw *d, const p_draw_idx *i) {
    uint8_t packed[48]; uint32_t n = record_compact_on() ? record_draw_pack(packed, d, i) : 0;
    uint32_t canonical = 8 + (d ? sizeof *d : sizeof *i);
    if (n) { memcpy(cmd_alloc(CB(c), n), packed, n); hw_fix(CB(c), canonical);
        uint32_t op=d?OP_DRAW:OP_DRAW_IDX; cs_op(op,n); t_rec_hist[op]++; }
    else { uint8_t *p = cmd_alloc(CB(c), canonical); put_hdr(p, d ? OP_DRAW : OP_DRAW_IDX, canonical);
        memcpy(p + 8, d ? (const void *)d : (const void *)i, canonical - 8); }
}
