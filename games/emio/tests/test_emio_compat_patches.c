#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void runtime_game_patches(Module *m);
_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"expected rejection: %s\n",message); exit(86); }
static uint32_t word(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void put(unsigned char *p,uint32_t v) { memcpy(p,&v,4); }
int main(int argc, char **argv) {
    const size_t size=0x100000;
    unsigned char *image=calloc(1,size); assert(image);
    Module module={.base=image,.file_size=size,.text_end=0xf4000,.ro_off=0xf8000};
    put(image+0x9520, 0x52b40013);
    put(image+0x9524, 0xaa1303e0);
    put(image+0xe1bc0, 0xd10343ff);
    put(image+0xe2450, 0xd10103ff);
    if(argc>1 && !strcmp(argv[1],"wrong-instruction")) put(image+0x9520, 0xd503201f);
    runtime_game_patches(&module);
    assert(argc==1);
    assert(word(image+0x9520)==0x52b40013); /* authentic 2.5 GiB block restored (no CUT) */
    assert(word(image+0x9524)==0x52b80000);
    assert(word(image+0xe1bc0)==0x52800020);
    assert(word(image+0xe1bc4)==0xd65f03c0);
    assert(word(image+0xe2450)==0xd65f03c0);
    free(image);
    return 0;
}
