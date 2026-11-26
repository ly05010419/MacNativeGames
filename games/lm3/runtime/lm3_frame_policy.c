#include "lm3_frame_policy.h"
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>

int runtime_lm3_target_fps(void) {
    if (strcmp(TOTK_GAME_DIR, "v140")) return 0;
    const char *value = getenv("LM3_TARGET_FPS");
    if (!value || !*value || !strcmp(value, "60")) return 60;
    if (!strcmp(value, "30")) return 30;
    runtime_fail("LM3_TARGET_FPS must be 30 or 60");
}

void runtime_lm3_frame_patch(Module *module) {
    int fps = runtime_lm3_target_fps();
    if (!fps) return;
    uint8_t header[0x60];
    FILE *file = fopen("extracted/" TOTK_GAME_DIR "/exefs/main", "rb");
    if (!file) runtime_fail("LM3 frame limiter: original main build identity is missing");
    size_t count = fread(header, 1, sizeof header, file);
    fclose(file);
    if (count != sizeof header || memcmp(header, "NSO0", 4) ||
        strcmp(module->name, "main") ||
        !lm3_frame_guard(module->base, module->file_size, header + 0x40))
        runtime_fail("LM3 frame limiter: main build, target or consumer guard mismatch");
    /* The image is still private RW here. loader.c seals rodata after all
       launch patches; there are no later writes and no TOTK time-step hook. */
    float milliseconds = 1000.0f / (float)fps - (fps == 60 ? 0.5f : 0.0f);
    memcpy(module->base + LM3_FRAME_MS_OFFSET, &milliseconds, sizeof milliseconds);
    fprintf(stderr, "[lm3-fps] engine target %d FPS, limiter %.6f ms; real elapsed-time step retained\n", fps, milliseconds);
}
