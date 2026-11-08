#pragma once
#include "prompt_bindings.h"
#include <stddef.h>
#include <stdint.h>

/* Notifications copy the mapping; the font/render threads never read mutable
   input state. Synthetic test/autoload input deliberately does not select a device. */
void runtime_prompt_keyboard(const KeyMap *map);
void runtime_prompt_controller(const uint16_t *map);
void runtime_prompt_mouse_mode(int enabled);
void runtime_prompt_note_input(PromptDevice device);
int runtime_prompt_style(void);
int runtime_prompt_style_set(int style);
uint64_t runtime_prompt_snapshot(PromptBindings *out);
uint64_t runtime_prompt_generation(void);
/* Draw an 8-bit coverage mask, top row first. Returns zero for an unrelated glyph. */
int runtime_prompt_draw(const PromptBindings *bindings, unsigned cp, uint8_t *pixels,
                        unsigned width, unsigned height, size_t stride);
