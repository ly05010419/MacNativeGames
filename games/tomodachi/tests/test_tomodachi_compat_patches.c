#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void runtime_game_patches(Module *m);
_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"expected rejection: %s\n",message); exit(86); }
static uint32_t word(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void put(unsigned char *p,uint32_t v) { memcpy(p,&v,4); }
static uint64_t branch_target(uint32_t instruction,uint64_t site) {
    assert((instruction&0xfc000000u)==0x14000000u);
    int64_t imm=(int64_t)((instruction&0x03ffffffu)^0x02000000u)-0x02000000ll;
    return (uint64_t)((int64_t)site+imm*4);
}
int main(int argc,char **argv) {
    const size_t size=0x590000, cave=0x588000, site=0x32a5b0;
    unsigned char *image=calloc(1,size); assert(image);
    Module module={.base=image,.file_size=size,.text_end=cave,.ro_off=cave+0x1000};
    put(image+site,0x2a0103e2);
    put(image+0x580434,0x321b2fe0);
    put(image+0x57b9fc,0x52900003); put(image+0x57ba70,0x52900003);
    unsetenv("TOMODACHI_POOL_ALIGNMENT_PATCH"); unsetenv("TOMODACHI_MII_HEAP_PATCH");
    if(argc>1&&!strcmp(argv[1],"disabled")) {
        unsigned char *before=malloc(size); assert(before); memcpy(before,image,size);
        setenv("TOMODACHI_POOL_ALIGNMENT_PATCH","0",1);setenv("TOMODACHI_MII_HEAP_PATCH","0",1);
        runtime_game_patches(&module); assert(!memcmp(image,before,size));free(before);
    } else {
        if(argc>1&&!strcmp(argv[1],"wrong-instruction")) put(image+site,0xd503201f);
        if(argc>1&&!strcmp(argv[1],"occupied-padding")) image[cave]=1;
        runtime_game_patches(&module);
        assert(argc==1); /* negative scenarios must fail before returning */
        assert(branch_target(word(image+site),site)==cave);
        assert(word(image+cave)==0x2a0103e2&&word(image+cave+4)==0xb94000a9);
        assert(word(image+cave+8)==0x36400049&&word(image+cave+12)==0x531e7422);
        assert(branch_target(word(image+cave+16),cave+16)==site+4);
        assert(word(image+0x580434)==0x321b33e0);
        assert(word(image+0x57b9fc)==0x52a00023&&word(image+0x57ba70)==0x52a00023);
    }
    free(image); return 0;
}
