/* Breath of the Wild heap sizes (botw_heap.h). The game's heaps are constants in its code (no HeapDef):
   SetMemoryHeapSize takes 0xc3000000 and the root block 0xc1000000; a few heaps inside it are sized by single movs.
   Two plans write them:
   - the 2K output (game_patches_botw.c patch_output_res, 1.6.0): heap and root +224 MiB, as mov w (the words it wrote
     before);
   - the 8 GB mode (TOTK_DRAM_8GB, Mac): heap 6 GiB (+224 MiB with the 2K output), root 32 MiB under it, and twelve heap
     sizes inside it larger. The sizes are those of HDPacks' "HDTexture Memory" ExeFS patch for 1.6.0 (shipped with their HD
     texture pack, made for the emulators' 8 GB DRAM mode; credited, not copied). Its 64-bit heap / root are written
     there with movz + movk over the frame setup (main+0xe536d8) and a call (main+0xe53f80); here each site branches to
     a veneer in the padding after the code instead (movz, movk, b back), and both neighbours stay.
   1.6.0: heap main+0xe536dc (mov w0), root main+0xe53fa0 (mov w21, later mov x1, x21 and str x21, [x19, #8]). Its
     resource-heap block (main+0x182fad0..0x182fc34) is widened to 64 bits by the pack and stored with an 8-byte stur at
     +0xd4, whose high half the next instruction (str w19, [x8, #0xd8], the same object) overwrites; the sizes fit 32
     bits, so here only its two movs change (500 -> 2032 MiB and 30 -> 512 MiB), as 32-bit movs. Unicorn ran the block
     the three ways in 108 cases, the call inside it simulated: the same registers, memory, writes and state at each
     call as the pack's (an experiment, E9 S9.5; not a proof for every input). At main+0x1c8f0a0 (mov w0, #0x4000 then
     movk w0, #2, lsl #16) the pack's 0x38000 ends as 0x28000 after the movk; that effective size is written
     (mov w0, #0x8000), the movk kept. ResourceMgrTask's prepare reads the size with ldr w (main+0x1c775f4): unsigned.
   1.9.0 (the same table for both mains, 6d48 and e84d: their rewriters differ in no word here): the same fourteen sites,
     found by function: by the heap names, the functions that create them and where each heap is kept.
     Heap main+0x71c (mov w0); root main+0xc617c0 (mov w8, then stp x8, xzr, [sp, #0x20] to main+0x3304, which hands
     it to RootHeap's allocation at main+0x4a28). HavokMainHeap's size goes in w3 to a wrapper (main+0x1571938) that
     moves it to w0. The resource-heap size is already 64 bits there (csel x26, add x8, str x8, [cfg, #0xd8], read with
     ldr x by ResourceMgrTask's prepare at main+0x1560838), so its two movs change and nothing is widened.
   Prepare reads every word it relies on, checks the budget exactly as SetHeapSize will (services.c: size <= heap cap,
   a multiple of 2 MiB, size + image <= pool; the head room left is for MapPhysicalMemory and later allocations, which
   this does not guarantee) and the veneer bytes; apply only writes the prepared words. */
#include "botw_heap.h"
#include <stdio.h>
#include <string.h>

enum {
    VENEER_SKIP = 0x10,          /* the sound-link veneer's 16 bytes at (text_end + 15) & ~15 (game_patches_botw.c, 1.6.0) */
    VENEER_SLOT = 0x10,          /* one veneer: 3 words, 16 bytes apart */
    HEAP_GROUP = 6,              /* the first six entries: the heap and root sites and their neighbours */
};
#define RETAIL_HEAP 0xc3000000ull
#define ROOT_GAP (32ull << 20)
#define OUTPUT_2K_EXTRA (224ull << 20)
#define BIG_HEAP (6ull << 30)
#define ALIGN_2M (2ull << 20)

/* Sites (big != 0: written in the 8 GB mode; the heap and root sites: written by both plans; the rest: neighbours,
   read only). */
static const BotwHeapSite sites_160[] = {
    {0xe536d8, 0x910043fd, 0}, {0xe536dc, 0x52b86000, 0}, {0xe536e0, 0x944b5318, 0},          /* add x29 / mov w0, #0xc3000000 / bl */
    {0xe53f9c, 0x913d4273, 0}, {0xe53fa0, 0x52b82015, 0}, {0xe53fa4, 0x9000f374, 0},           /* add x19 / mov w21, #0xc1000000 / adrp */
    {0xdfa574, 0x14000002, 0}, {0xdfa578, 0x52a03600, 0x52a06c00}, {0xdfa57c, 0xd000c288, 0},   /* 0x1b00000 -> 0x3600000 */
    {0x182f364, 0x91213508, 0}, {0x182f368, 0x52a2ec00, 0x52a8a000}, {0x182f36c, 0x9105c3e1, 0}, /* 0x17600000 -> 0x45000000 */
    {0x182f398, 0x91112508, 0}, {0x182f39c, 0x320a03e0, 0x52a01800}, {0x182f3a0, 0x9105c3e1, 0}, /* 0x400000 -> 0xc00000 */
    {0x182f3dc, 0x91215908, 0}, {0x182f3e0, 0x320607e0, 0x52a6e000}, {0x182f3e4, 0x9105c3e1, 0}, /* 0xc000000 -> 0x37000000 */
    {0x1c8f05c, 0x91092508, 0}, {0x1c8f060, 0x52a05a00, 0x52a06f00}, {0x1c8f064, 0x910003e1, 0}, /* 0x2d00000 -> 0x3780000 */
    {0x1c8f09c, 0x912ad508, 0}, {0x1c8f0a0, 0x52880000, 0x52900000}, {0x1c8f0a4, 0x72a00040, 0}, /* 0x24000 -> 0x28000 (movk kept) */
    {0x1c8f0cc, 0x9120e108, 0}, {0x1c8f0d0, 0x320f07e0, 0x52a00120}, {0x1c8f0d4, 0x910003e1, 0}, /* 0x60000 -> 0x90000 */
    {0x1c8f104, 0x910b6108, 0}, {0x1c8f108, 0x321007e0, 0x52a00080}, {0x1c8f10c, 0x910003e1, 0}, /* 0x30000 -> 0x40000 */
    {0x1620704, 0x910f5d08, 0}, {0x1620708, 0x320c0fe0, 0x52a02500}, {0x162070c, 0xf90353fb, 0}, /* 0xf00000 -> 0x1280000 */
    {0x16216dc, 0xf9402322, 0}, {0x16216e0, 0x52a00a00, 0x52a00f00}, {0x16216e4, 0x12800004, 0}, /* 0x500000 -> 0x780000 */
    {0x182facc, 0x7100091f, 0}, {0x182fad0, 0x52a3e808, 0x52afe008}, {0x182fad4, 0x3941478b, 0}, /* w8 0x1f400000 -> 0x7f000000 */
    {0x182fb20, 0x52a56e17, 0}, {0x182fb24, 0x320b0fea, 0x52a4000a}, {0x182fb28, 0x0b0a010a, 0}, /* w10 0x1e00000 -> 0x20000000 */
};
static const BotwHeapSite sites_190[] = {
    {0x718, 0x910043fd, 0}, {0x71c, 0x52b86000, 0}, {0x720, 0x94567e6c, 0},                     /* add x29 / mov w0, #0xc3000000 / bl */
    {0xc617bc, 0x94250009, 0}, {0xc617c0, 0x52b82008, 0}, {0xc617c4, 0x52800020, 0},             /* bl / mov w8, #0xc1000000 / mov w0, #1 */
    {0xe61344, 0xd65f03c0, 0}, {0xe61348, 0x52a03600, 0x52a06c00}, {0xe6134c, 0x17ffffe9, 0},   /* UiHeap 0x1b00000 -> 0x3600000 */
    {0x131aef4, 0xa90023f5, 0}, {0x131aef8, 0x52a2ec00, 0x52a8a000}, {0x131aefc, 0x97ee10f3, 0}, /* FixedHeap 0x17600000 -> 0x45000000 */
    {0x131b274, 0x910003e1, 0}, {0x131b278, 0x52a00800, 0x52a01800}, {0x131b27c, 0x52800103, 0}, /* GameDataWork 0x400000 -> 0xc00000 */
    {0x131b498, 0x910003e1, 0}, {0x131b49c, 0x52a18000, 0x52a6e000}, {0x131b4a0, 0x52800103, 0}, /* TexArcWork 0xc000000 -> 0x37000000 */
    {0x156dfc4, 0x9118c529, 0}, {0x156dfc8, 0x52a05a03, 0x52a06f03}, {0x156dfcc, 0x910003e4, 0}, /* HavokMainHeap w3 0x2d00000 -> 0x3780000 */
    {0x156dff8, 0x913e054a, 0}, {0x156dffc, 0x52880000, 0x52900000}, {0x156e000, 0x72a00040, 0}, /* PhysicsSystem 0x24000 -> 0x28000 (movk kept) */
    {0x156e030, 0x910003e1, 0}, {0x156e034, 0x52a000c0, 0x52a00120}, {0x156e038, 0xaa1303e2, 0}, /* PhysicsTempDefault 0x60000 -> 0x90000 */
    {0x156e07c, 0x7901b008, 0}, {0x156e080, 0x52a00060, 0x52a00080}, {0x156e084, 0x97aa57a3, 0}, /* PhysicsTempLow 0x30000 -> 0x40000 */
    {0x112b194, 0xf90213e8, 0}, {0x112b198, 0x52a01e00, 0x52a02500}, {0x112b19c, 0xaa1503e2, 0}, /* GameDataHeap 0xf00000 -> 0x1280000 */
    {0x112b35c, 0x91360d08, 0}, {0x112b360, 0x52a00a00, 0x52a00f00}, {0x112b364, 0x12800005, 0}, /* SaveAreaHeap 0x500000 -> 0x780000 */
    {0x127b6dc, 0xa908abe9, 0}, {0x127b6e0, 0x52a3e809, 0x52afe009}, {0x127b6e4, 0xf9000feb, 0}, /* w9 0x1f400000 -> 0x7f000000 */
    {0x127b770, 0xb9400149, 0}, {0x127b774, 0x52a03c08, 0x52a40008}, {0x127b778, 0x8b080348, 0}, /* w8 0x1e00000 -> 0x20000000 */
};
const BotwHeapTable botw_heap_160 = {"1.6.0", sites_160, sizeof sites_160 / sizeof *sites_160,
                                     0xe536dc, 0xe536e0, 0xe53fa0, 0xe53fa4, 0, 21};
const BotwHeapTable botw_heap_190 = {"1.9.0", sites_190, sizeof sites_190 / sizeof *sites_190,
                                     0x71c, 0x720, 0xc617c0, 0xc617c4, 0, 8};

static uint32_t movz_w(unsigned rd, uint32_t imm16, unsigned hw) { return 0x52800000u | (hw << 21) | (imm16 << 5) | rd; }
static uint32_t movz_x(unsigned rd, uint32_t imm16, unsigned hw) { return 0xd2800000u | (hw << 21) | (imm16 << 5) | rd; }
static uint32_t movk_x(unsigned rd, uint32_t imm16, unsigned hw) { return 0xf2800000u | (hw << 21) | (imm16 << 5) | rd; }
static int branch(uint64_t site, uint64_t target, uint32_t *word) {   /* b <target>; 0 outside the ±128 MiB range */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1ll << 27) || delta >= (1ll << 27)) return 0;
    *word = 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
    return 1;
}
static int refuse(char *err, size_t errlen, const char *what) { snprintf(err, errlen, "%s", what); return -1; }
static void put(BotwHeapPlan *p, uint64_t off, uint32_t word) { p->put[p->n].off = (uint32_t)off; p->put[p->n].word = word; p->n++; }
/* A size whose 16-bit chunks are bits 16..47 only (a multiple of 64 KiB under 256 TiB): two words, movz then movk. */
static int veneer(BotwHeapPlan *p, uint64_t at, unsigned rd, uint64_t size, uint64_t back, char *err, size_t errlen) {
    if (size & 0xffff || size >> 48) return refuse(err, errlen, "a heap size the veneer cannot load");
    uint32_t b;
    if (!branch(at + 8, back, &b)) return refuse(err, errlen, "the veneer's branch back is out of range");
    put(p, at, movz_x(rd, (uint32_t)(size >> 16) & 0xffff, 1));
    put(p, at + 4, movk_x(rd, (uint32_t)(size >> 32) & 0xffff, 2));
    put(p, at + 8, b);
    return 0;
}
/* The heap and root sites must be the second and fifth entries, each with its neighbours around it, and every write
   must fit the plan. */
static int table_ok(const BotwHeapTable *t) {
    unsigned writes = 8;                                               /* two branches and two 3-word veneers */
    for (unsigned i = HEAP_GROUP; i < t->count; i++) writes += t->sites[i].big != 0;
    return t->count >= HEAP_GROUP && t->sites[1].off == t->heap_at && t->sites[2].off == t->heap_back &&
           t->sites[0].off + 4 == t->heap_at && t->heap_at + 4 == t->heap_back && t->sites[4].off == t->root_at &&
           t->sites[5].off == t->root_back && t->sites[3].off + 4 == t->root_at && t->root_at + 4 == t->root_back &&
           t->heap_rd < 31 && t->root_rd < 31 && writes <= BOTW_HEAP_MAX_PUT;
}

int botw_heap_prepare(const BotwHeapTable *t, const uint8_t *base, uint64_t size, uint64_t text_end, int output_2k,
                      int dram_8gb, uint64_t pool, uint64_t heap_cap, uint64_t image, BotwHeapPlan *plan, char *err,
                      size_t errlen) {
    memset(plan, 0, sizeof *plan);
    plan->heap = RETAIL_HEAP; plan->root = RETAIL_HEAP - ROOT_GAP;
    if (!output_2k && !dram_8gb) return 0;                            /* the shipped sizes: nothing to write */
    if (!table_ok(t)) return refuse(err, errlen, "the heap site table is malformed");
    unsigned checked = dram_8gb ? t->count : HEAP_GROUP;
    for (unsigned i = 0; i < checked; i++) {
        const BotwHeapSite *s = &t->sites[i];
        if ((uint64_t)s->off + 4 > text_end || text_end > size) return refuse(err, errlen, "a heap site lies outside the code");
        uint32_t w; memcpy(&w, base + s->off, 4);
        if (w != s->original) {
            snprintf(err, errlen, "main+%#x is %08x, not the %s word %08x", s->off, w, t->version, s->original);
            return -1;
        }
    }
    uint64_t heap = RETAIL_HEAP + (output_2k ? OUTPUT_2K_EXTRA : 0);
    if (!dram_8gb) {                                                  /* the 2K output alone: the words it always wrote */
        plan->heap = heap; plan->root = heap - ROOT_GAP;
        put(plan, t->heap_at, movz_w(t->heap_rd, (uint32_t)(plan->heap >> 16), 1));
        put(plan, t->root_at, movz_w(t->root_rd, (uint32_t)(plan->root >> 16), 1));
        return 0;
    }
    heap = BIG_HEAP + (output_2k ? OUTPUT_2K_EXTRA : 0);
    uint64_t root = heap - ROOT_GAP, used;
    if (heap % ALIGN_2M || root % ALIGN_2M) return refuse(err, errlen, "the heap size is not a multiple of 2 MiB");
    if (heap > heap_cap) {
        snprintf(err, errlen, "the heap %#llx is over the heap cap %#llx", (unsigned long long)heap, (unsigned long long)heap_cap);
        return -1;
    }
    if (__builtin_add_overflow(heap, image, &used) || used > pool) {
        snprintf(err, errlen, "the heap %#llx and the code %#llx do not fit the application pool %#llx (TOTK_APP_POOL, if set, "
                 "must be at least their sum)", (unsigned long long)heap, (unsigned long long)image, (unsigned long long)pool);
        return -1;
    }
    uint64_t stub = (text_end + 15) & ~15ull, v = stub + VENEER_SKIP, page_end = (text_end + 0xfff) & ~0xfffull;
    if (stub < text_end || v + 2 * VENEER_SLOT > page_end || v + 2 * VENEER_SLOT > size)
        return refuse(err, errlen, "no room for the heap veneers in the padding after the code");
    for (uint64_t i = v; i < v + 2 * VENEER_SLOT; i++)
        if (base[i]) return refuse(err, errlen, "the padding for the heap veneers is in use");
    uint32_t to_heap, to_root;
    if (!branch(t->heap_at, v, &to_heap) || !branch(t->root_at, v + VENEER_SLOT, &to_root))
        return refuse(err, errlen, "the heap veneers are out of branch range");
    plan->heap = heap; plan->root = root; plan->veneer = v;
    if (veneer(plan, v, t->heap_rd, heap, t->heap_back, err, errlen) ||
        veneer(plan, v + VENEER_SLOT, t->root_rd, root, t->root_back, err, errlen)) {
        memset(plan, 0, sizeof *plan);
        return -1;
    }
    put(plan, t->heap_at, to_heap);
    put(plan, t->root_at, to_root);
    for (unsigned i = HEAP_GROUP; i < t->count; i++)
        if (t->sites[i].big) put(plan, t->sites[i].off, t->sites[i].big);
    return 0;
}

void botw_heap_apply(uint8_t *base, const BotwHeapPlan *plan) {
    for (unsigned i = 0; i < plan->n; i++) memcpy(base + plan->put[i].off, &plan->put[i].word, 4);
}
