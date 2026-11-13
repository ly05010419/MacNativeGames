#pragma once
#include "runtime.h"

/* Applied before the loader seals main. Changes the NLG engine's own initial
   dimensions and dynamic-resolution output, not NVN texture/viewport semantics.
   LM3_RENDER_SIZE: 1280x720, 1600x900, 1920x1080, 2560x1440 (default), or 0
   to retain the original game. A changed size takes effect at the next boot. */
void runtime_lm3_render_patch(Module *main_module);

/* Additional application/guest-heap/GPU-pool reservation needed by native
   1440p. The host application pool must add this same amount before the guest
   starts. This does not alter the 4 GiB address-space or 8 GB Metal guards. */
uint64_t runtime_lm3_render_extra_bytes(void);
uint64_t lm3_render_extra_bytes_for_size(unsigned width, unsigned height);

/* Pure transaction used by the startup adapter and its instruction-guard tests.
   On failure, no byte of image is changed. build_id is the original 32-byte NSO
   build identifier; the normal loader separately verifies the rewritten image. */
int lm3_render_apply_image(uint8_t *image, size_t image_size,
                           const uint8_t build_id[32], unsigned width,
                           unsigned height, char *error, size_t error_size);
