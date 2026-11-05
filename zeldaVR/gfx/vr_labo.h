/* Breath of the Wild's native Labo renderer. The mode is opt-in on Mac; the
 * iOS runtime and Mac-side iOS export always report it unavailable. */
#pragma once
#include <stdint.h>
#include <stddef.h>

int vr_labo_enabled(void);
/* Game option policy, separate from our renderer/sensor. Always unavailable,
 * including in our VR and in explicitly enabled research sessions. */
unsigned char vr_labo_option_permitted(unsigned char show_error);
int vr_labo_active(void);
void vr_labo_begin(void);
void vr_labo_end(void);

/* Complete shader-code SHA256, excluding the translator/settings suffix. */
int vr_labo_shader_pair(const char *vertex, const char *fragment);
int vr_labo_lens_pair(const char *vertex, const char *fragment);
int vr_labo_mask_pair(const char *vertex, const char *fragment);
int vr_labo_plain_mesh(const char *vertex);
int vr_native_ui_vertex(const char *vertex);

int vr_native_ui_text_vertex(const char *vertex);
