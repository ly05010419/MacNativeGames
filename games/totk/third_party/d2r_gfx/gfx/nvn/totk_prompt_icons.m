/* Renderer-owned copies of the icon font's atlas and the eight HUD button masks.
   CPU game memory, glyph metrics, and non-prompt pixels remain unchanged. The
   replay thread owns Metal objects; the font thread only publishes rectangles. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdatomic.h>
#include "totk_prompt_icons.h"
#include "../tex/nvn_deswizzle.h"
#include "../../../../runtime/input_prompts.h"

/* The game's runtime draws the prompts (games/totk/runtime/input_prompts.m) and its definitions replace these weak ones
   at link time. A game whose runtime has none (Ocarina of Time) gets prompts off: the font's glyphs stay the game's. */
__attribute__((weak)) uint64_t runtime_prompt_generation(void) { return 0; }
__attribute__((weak)) uint64_t runtime_prompt_snapshot(PromptBindings *out) { (void)out; return 0; }
__attribute__((weak)) int runtime_prompt_off(void) { return 1; }
__attribute__((weak)) int runtime_prompt_draw(const PromptBindings *b, unsigned cp, uint8_t *px, unsigned w, unsigned h, size_t stride) {
    (void)b; (void)cp; (void)px; (void)w; (void)h; (void)stride; return 0; }

enum { MAX_ATLASES = 8, MAX_REGIONS = 512, MAX_TEXTURES = 64 };
typedef struct {
    const uint8_t *memory;
    unsigned width, height, count;
    size_t stride;
    TotkPromptRegion regions[MAX_REGIONS];
} PromptAtlas;
static PromptAtlas atlases[MAX_ATLASES];
static _Atomic uintptr_t atlas_memory[MAX_ATLASES];   /* read without the lock: most R8 textures are not an atlas */
static pthread_mutex_t atlas_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t atlas_revision;
static _Atomic int enabled;

void totk_prompt_set_atlas(const uint8_t *memory, unsigned width, unsigned height,
                           size_t stride, const TotkPromptRegion *regions, unsigned count) {
    if (!memory || !width || !height || width > 8192 || height > 16384 || stride < width || count > MAX_REGIONS) return;
    pthread_mutex_lock(&atlas_lock);
    for (unsigned i = 0; i < MAX_ATLASES; i++) {
        PromptAtlas *a = &atlases[i];
        if (a->memory && a->memory != memory) continue;
        if (a->memory != memory || a->width != width || a->height != height || a->stride != stride ||
            a->count != count || memcmp(a->regions, regions, count * sizeof *regions)) {
            a->memory = memory; a->width = width; a->height = height; a->stride = stride; a->count = count;
            memcpy(a->regions, regions, count * sizeof *regions);
            atomic_store(&atlas_memory[i], (uintptr_t)memory);
            atomic_fetch_add(&atlas_revision, 1);
        }
        atomic_store(&enabled, 1);
        break;
    }
    pthread_mutex_unlock(&atlas_lock);
}
void totk_prompt_remove_pool(const void *memory, size_t size) {
    uintptr_t begin = (uintptr_t)memory;
    pthread_mutex_lock(&atlas_lock);
    for (unsigned i = 0; i < MAX_ATLASES; i++) {
        uintptr_t address = (uintptr_t)atlases[i].memory;
        if (address >= begin && address - begin < size) {
            memset(&atlases[i], 0, sizeof atlases[i]); atomic_store(&atlas_memory[i], 0); atomic_fetch_add(&atlas_revision, 1);
        }
    }
    pthread_mutex_unlock(&atlas_lock);
}
uint64_t totk_prompt_revision(void) {
    if (!atomic_load(&enabled) || !runtime_prompt_generation) return 0;
    return atomic_load(&atlas_revision) + runtime_prompt_generation();
}

@interface TotkPromptTexture : NSObject
@property(nonatomic) id<MTLTexture> texture;
@property(nonatomic) uintptr_t source;
@property(nonatomic) uint64_t revision, used;
@property(nonatomic) unsigned hud;
@end
@implementation TotkPromptTexture
@end

typedef struct { unsigned cp, width, height; unsigned char sha[32]; } HudMask;
static HudMask hud_masks[8];
static unsigned hud_count;
static void load_hud_masks(void) {
    FILE *f = fopen("build/prompt-hud.tsv", "r");
    if (!f) return; /* Older apps can still use the live font prompts. */
    unsigned cp, w, h; char hex[65];
    while (hud_count < 8 && fscanf(f, "%x\t%u\t%u\t%64s", &cp, &w, &h, hex) == 4) {
        if (strlen(hex) != 64 || w > 512 || h > 512 || !w || !h) break;
        PromptBindings b = {.device = PROMPT_KEYBOARD}; PromptIcon icon;
        if (!prompt_resolve(&b, cp, &icon)) break;
        HudMask *mask = &hud_masks[hud_count];
        mask->cp = cp; mask->width = w; mask->height = h;
        int valid = 1;
        for (unsigned i = 0; i < 32; i++) {
            char byte[3] = {hex[2 * i], hex[2 * i + 1], 0}, *end;
            mask->sha[i] = (unsigned char)strtoul(byte, &end, 16); if (*end) valid = 0;
        }
        if (!valid) break;
        hud_count++;
    }
    fclose(f);
}
static unsigned identify_hud(const tex_t *t, const uint8_t *pixels) {
    unsigned char sha[32]; CC_SHA256(pixels, (CC_LONG)(t->d.width * t->d.height), sha);
    for (unsigned i = 0; i < hud_count; i++)
        if (hud_masks[i].width == t->d.width && hud_masks[i].height == t->d.height && !memcmp(sha, hud_masks[i].sha, 32))
            return hud_masks[i].cp;
    return 0;
}

id<MTLTexture> totk_prompt_texture(const tex_t *t, id<MTLTexture> original,
                                  void (*resident)(id<MTLResource>), void (*retire)(id)) {
    if (!atomic_load(&enabled) || !original || !t->mem || t->d.format != NVN_FORMAT_R8 || t->d.levels != 1 ||
        !runtime_prompt_snapshot || !runtime_prompt_draw || !runtime_prompt_off || runtime_prompt_off()) return original;
    int known = t->d.width == t->d.height && (t->d.width == 72 || t->d.width == 108);   /* a HUD mask's sizes */
    for (unsigned i = 0; i < MAX_ATLASES && !known; i++) known = atomic_load_explicit(&atlas_memory[i], memory_order_relaxed) == (uintptr_t)t->mem;
    if (!known) return original;
    static dispatch_once_t once; dispatch_once(&once, ^{ load_hud_masks(); });
    PromptAtlas atlas = {0};
    pthread_mutex_lock(&atlas_lock);
    for (unsigned i = 0; i < MAX_ATLASES; i++)
        if (atlases[i].memory == t->mem && atlases[i].width == t->d.width && atlases[i].height == t->d.height) {
            atlas = atlases[i]; break;
        }
    pthread_mutex_unlock(&atlas_lock);
    int hud_candidate = !atlas.memory && t->d.width == t->d.height && (t->d.width == 72 || t->d.width == 108);
    if ((!atlas.memory || !atlas.count) && !hud_candidate) return original;

    /* Only the replay thread enters this function. Each cached replacement is
       retired through the renderer's fence queue, including on eviction. */
    static NSMutableDictionary<NSNumber *, TotkPromptTexture *> *cache;
    static uint64_t tick;
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSNumber *key = @(t->serial);
    TotkPromptTexture *entry = cache[key];
    if (!entry) { entry = [TotkPromptTexture new]; cache[key] = entry; }
    entry.used = ++tick;
    uint64_t revision = totk_prompt_revision();
    if (entry.source == (uintptr_t)(__bridge void *)original && entry.revision == revision)
        return entry.texture ?: original;

    PromptBindings b; runtime_prompt_snapshot(&b);
    size_t width = t->d.width, height = t->d.height;
    NSMutableData *data = [NSMutableData dataWithLength:width * height];
    uint8_t *pixels = data.mutableBytes;
    if (atlas.memory && nvn_tex_is_linear(&t->d)) {
        for (size_t y = 0; y < height; y++) memcpy(pixels + y * width, t->mem + y * atlas.stride, width);
    } else if (nvn_deswizzle(&t->d, 0, 0, t->mem, pixels, width)) return original;
    int changed = 0;
    if (atlas.memory) {
        for (unsigned i = 0; i < atlas.count; i++) {
            const TotkPromptRegion *r = &atlas.regions[i];
            if (r->width > width || r->height > height || r->x > width - r->width || r->y > height - r->height) continue;
            uint8_t *mask = pixels + r->y * width + r->x;
            if (runtime_prompt_draw(&b, r->codepoint, mask, r->width, r->height, width)) {
                /* ScalableFontEngine copies glyph scanlines bottom-up into
                   this atlas. BNTX HUD masks instead use top-first rows. */
                for (unsigned y = 0; y < r->height / 2; y++) for (unsigned x = 0; x < r->width; x++) {
                    uint8_t value = mask[y * width + x];
                    mask[y * width + x] = mask[(r->height - 1 - y) * width + x];
                    mask[(r->height - 1 - y) * width + x] = value;
                }
                changed = 1;
            }
        }
    } else {
        if (entry.source != (uintptr_t)(__bridge void *)original) entry.hud = identify_hud(t, pixels);
        if (entry.hud) changed = runtime_prompt_draw(&b, entry.hud, pixels, (unsigned)width, (unsigned)height, width);
    }
    id<MTLTexture> replacement = nil;
    if (changed) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:original.pixelFormat
                                                                                   width:width height:height mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        replacement = [original.device newTextureWithDescriptor:desc];
        if (!replacement) return original;
        [replacement replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels bytesPerRow:width];
        resident(replacement);
    }
    if (entry.texture) retire(entry.texture);
    entry.texture = replacement; entry.source = (uintptr_t)(__bridge void *)original; entry.revision = revision;
    if (cache.count > MAX_TEXTURES) {
        NSNumber *oldest = nil; uint64_t age = UINT64_MAX;
        for (NSNumber *k in cache) if (cache[k].used < age) { oldest = k; age = cache[k].used; }
        if (oldest && ![oldest isEqual:key]) { if (cache[oldest].texture) retire(cache[oldest].texture); [cache removeObjectForKey:oldest]; }
    }
    return replacement ?: original;
}
