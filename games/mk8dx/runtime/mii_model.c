/* Mario Kart 8 Deluxe: the console's Mii parts (system data MiiModel, 0100000000000802), which the game mounts as
   MiiResource: to build the Mii racers' heads. The host ships no firmware content: the build extracts the archive from the
   player's own firmware (scripts/extract_mii_model.py, as the system fonts are), into TOTK_MII_MODEL (default
   extracted/firmware/mii-model). The game reads its files whole and builds its memory pool from their headers, so a
   missing archive cannot be skipped: without one the start stops here with a message. */
#include "mii_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *const mii_model_files[] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat",
                                              "NXTextureMidSRGB.dat", "ShapeHigh.dat", "ShapeMid.dat"};
const char *runtime_mii_model_dir(void) {
    const char *dir = getenv("TOTK_MII_MODEL");
    if (!dir || !*dir) dir = "extracted/firmware/mii-model";
    for (size_t i = 0; i < sizeof mii_model_files / sizeof *mii_model_files; i++) {
        char path[4096]; struct stat st;
        if (snprintf(path, sizeof path, "%s/%s", dir, mii_model_files[i]) >= (int)sizeof path || stat(path, &st) || st.st_size < 0x100) {
            fprintf(stderr, "[mk8dx] MiiModel: %s missing; extract it from your own firmware with scripts/extract_mii_model.py\n", path);
            return NULL;
        }
    }
    return dir;
}
