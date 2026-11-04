#include "runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
extern void runtime_game_patches(Module *m);
int main(void) {
    unsigned char image[64], before[64];
    memset(image, 0x5a, sizeof image); memcpy(before, image, sizeof image);
    Module module = {.base=image, .file_size=sizeof image, .text_end=sizeof image};
    setenv("TOMODACHI_MII_HEAP_PATCH", "1", 1);
    setenv("TOMODACHI_POOL_ALIGNMENT_PATCH", "1", 1);
    runtime_game_patches(&module);
    assert(!memcmp(image, before, sizeof image));
    return 0;
}
