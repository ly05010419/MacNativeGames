/* Shared by the Mac settings panel and the graphics layer. A choice applies at
   the next launch; changing an environment variable at runtime is unnecessary. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline int async_shader_settings_path(char *out, size_t cap) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (!profile || !*profile) profile = "user-data";
    int n = snprintf(out, cap, "%s/async-shaders.txt", profile);
    return n > 0 && (size_t)n < cap;
}
static inline int async_shader_choice(void) {
    const char *override = getenv("TOTK_ASYNC_SHADERS");
    if (override) return !strcmp(override, "1");
    char path[4096];
    if (!async_shader_settings_path(path, sizeof path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char text[8] = {0};
    int ok = fgets(text, sizeof text, f) != NULL;
    fclose(f);
    return ok && (!strcmp(text, "1\n") || !strcmp(text, "1"));
}
static inline int async_shader_choose(int on) {
    if (getenv("TOTK_ASYNC_SHADERS")) return 0;
    char path[4096], temporary[4128];
    if (!async_shader_settings_path(path, sizeof path)) return 0;
    snprintf(temporary, sizeof temporary, "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) return 0;
    const char *value = on ? "1\n" : "0\n";
    int ok = write(fd, value, 2) == 2 && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok && rename(temporary, path) == 0) return 1;
    unlink(temporary);
    return 0;
}
