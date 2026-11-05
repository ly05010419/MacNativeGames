#include <assert.h>
#include <stdio.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn2_shader.h"
int main(int argc, char **argv) {
    uint8_t ctl[0x100] = {0}, code[4] = {0}; uint32_t inst, c1, total;
    uint32_t words[] = {0x98761234, 0xc9}; memcpy(ctl, words, sizeof words);
    assert(nvn2_control(ctl)); assert(!nvn2_lengths(ctl, &inst, &c1, &total));
    uint32_t n = 0x80; memcpy(ctl + 0x38, &n, 4); memcpy(ctl + 0x40, &n, 4); memcpy(ctl + 0x44, &n, 4);
    assert(nvn2_lengths(ctl, &inst, &c1, &total));
    n = 5 << 10; memcpy(code, &n, 4); assert(nvn2_stage(ctl, code) == 1);
    ctl[0x56] = 5; assert(nvn2_stage(ctl, code) == 5);
    ctl[0x56] = 6; assert(nvn2_stage(ctl, code) == 6);
    ctl[0x56] = 7; assert(nvn2_stage(ctl, code) == 7);
    for (int i = 1; i + 1 < argc; i += 2) {
        FILE *f = fopen(argv[i], "rb"); assert(f); assert(fread(ctl, 1, sizeof ctl, f) == sizeof ctl); fclose(f);
        f = fopen(argv[i+1], "rb"); assert(f); assert(fread(code, 1, sizeof code, f) == sizeof code);
        assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); fclose(f);
        assert(nvn2_control(ctl)); assert(nvn2_lengths(ctl, &inst, &c1, &total)); assert(total == size);
        assert(nvn2_stage(ctl, code) >= 0);
    }
    puts("NVN2 control layout passed");
}
