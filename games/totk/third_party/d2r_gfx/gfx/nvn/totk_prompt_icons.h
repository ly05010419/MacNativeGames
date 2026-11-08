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
#endif
