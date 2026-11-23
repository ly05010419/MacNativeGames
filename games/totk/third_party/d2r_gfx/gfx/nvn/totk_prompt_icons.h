/* Optional, game-specific prompt replacement. Texture storage stays untouched. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t codepoint, x, y, width, height; } TotkPromptRegion;
void totk_prompt_set_atlas(const uint8_t *memory, unsigned width, unsigned height,
                           size_t stride, const TotkPromptRegion *regions, unsigned count);
void totk_prompt_remove_pool(const void *memory, size_t size);
uint64_t totk_prompt_revision(void);
#ifdef __OBJC__
#import <Metal/Metal.h>
#include "nvn_obj.h"
id<MTLTexture> totk_prompt_texture(const tex_t *texture, id<MTLTexture> original,
                                  void (*resident)(id<MTLResource>), void (*retire)(id));
/* Optional (replay thread): where totk_prompt_texture builds a copy and what makes its texture. stage gives memory for
   the copy's bytes; upload makes a resident texture from the bytes staged last (width 0: none is made, the staging is
   given up). Both NULL: the copy is built in new memory and written with replaceRegion. */
void totk_prompt_set_upload(uint8_t *(*stage)(size_t bytes),
                            id<MTLTexture> (*upload)(MTLPixelFormat format, unsigned width, unsigned height));
void totk_prompt_stats(uint64_t *copies, uint64_t *nanoseconds);   // prompt copies built so far and the time they took
void totk_prompt_invalidate(void);   // any thread: an upload was lost, so every copy is built again at its next use
#endif
