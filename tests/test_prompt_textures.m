#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include "input_prompts.h"
#include "totk_prompt_icons.h"
#include "../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int runtime_setting_word(const char *n, char *w, size_t s) { (void)n; (void)w; (void)s; return 0; }
void runtime_setting_save(const char *n, const char *w, const char *why) { (void)n; (void)w; (void)why; }
static void resident(id<MTLResource> r) { assert(r); }
static void retire(id object) { assert(object); }
static id<MTLTexture> texture(id<MTLDevice> device, unsigned size, const uint8_t *pixels) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:size height:size mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> t = [device newTextureWithDescriptor:d]; assert(t);
    [t replaceRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0 withBytes:pixels bytesPerRow:size];
    return t;
}
int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "Metal device unavailable\n"); return 77; }
    uint8_t hud[108 * 108]; memset(hud, 17, sizeof hud);
    unsigned char sha[32]; CC_SHA256(hud, sizeof hud, sha);
    FILE *f = fopen("build/prompt-hud.tsv", "w"); assert(f);
    fprintf(f, "e040\t108\t108\t"); for (unsigned i = 0; i < 32; i++) fprintf(f, "%02x", sha[i]); fputc('\n', f); fclose(f);

    uint8_t source[32 * 32]; memset(source, 23, sizeof source);
    TotkPromptRegion region = {0xe040, 4, 4, 20, 20};
    totk_prompt_set_atlas(source, 32, 32, 32, &region, 1);
    tex_t atlas = {.serial = 1, .mem = source,
        .d = {.format = NVN_FORMAT_R8, .target = 2, .flags = 0x10, .width = 32, .height = 32, .stride = 32, .levels = 1}};
    id<MTLTexture> original = texture(device, 32, source);
    id<MTLTexture> keyboard = totk_prompt_texture(&atlas, original, resident, retire); assert(keyboard != original);
    uint8_t result[sizeof source]; [keyboard getBytes:result bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
    int differences = 0;
    for (unsigned y = 0; y < 32; y++) for (unsigned x = 0; x < 32; x++) {
        assert(source[y * 32 + x] == 23); /* Game memory remains untouched. */
        if (x >= 4 && x < 24 && y >= 4 && y < 24) differences += result[y * 32 + x] != 23;
        else assert(result[y * 32 + x] == 23);
    }
    assert(differences);
    PromptBindings bindings; runtime_prompt_snapshot(&bindings);
    uint8_t expected[20 * 20];
    assert(runtime_prompt_draw(&bindings, region.codepoint, expected, 20, 20, 20));
    for (unsigned y = 0; y < 20; y++) for (unsigned x = 0; x < 20; x++)
        assert(result[(y + 4) * 32 + x + 4] == expected[(19 - y) * 20 + x]);

    assert(totk_prompt_texture(&atlas, original, resident, retire) == keyboard);
    runtime_prompt_style_set(PROMPT_PLAYSTATION);
    id<MTLTexture> ps = totk_prompt_texture(&atlas, original, resident, retire); assert(ps != keyboard && ps != original);

    tex_t guide = {.serial = 2, .d = {.format = NVN_FORMAT_R8, .target = 2, .width = 108, .height = 108, .levels = 1}};
    nvn_tex_layout(&guide.d, &guide.L);
    guide.mem = calloc(1, guide.L.size); assert(guide.mem);
    assert(!nvn_swizzle(&guide.d, 0, 0, hud, 108, guide.mem));
    id<MTLTexture> hud_original = texture(device, 108, hud);
    assert(totk_prompt_texture(&guide, hud_original, resident, retire) != hud_original);
    guide.serial = 3; guide.mem[0] ^= 1; /* A different 108x108 UI image must not be replaced. */
    id<MTLTexture> unrelated = texture(device, 108, hud);
    assert(totk_prompt_texture(&guide, unrelated, resident, retire) == unrelated);
    /* The backend creates a new original when any atlas pixels change. A
       replacement must preserve new ordinary glyphs as well as live prompts. */
    source[0] = 81;
    id<MTLTexture> updated = texture(device, 32, source);
    id<MTLTexture> refreshed = totk_prompt_texture(&atlas, updated, resident, retire);
    assert(refreshed != ps && refreshed != updated);
    [refreshed getBytes:result bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
    assert(result[0] == 81);
    totk_prompt_remove_pool(source, sizeof source);
    assert(totk_prompt_texture(&atlas, original, resident, retire) == original);
    free(guide.mem);
    puts("live atlas, HUD matching, texture caching and isolation passed");
} }
