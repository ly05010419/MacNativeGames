/* Synthetic instruction fixtures only; no game files required. */
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <setjmp.h>
#include "../../runtime/game_patches.c"
static jmp_buf guard_return;
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "guard: %s\n", message); longjmp(guard_return,77); }
int main(void) {
    const size_t size = 0x2ab4000, data = 0x2ab0000;
    unsigned char *base = mmap(0, size, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
    assert(base != MAP_FAILED);
    pthread_jit_write_protect_np(0);
    Module m = {.base=base, .size=size, .text_end=0x2aa6a10};
    const uint64_t site=0x1ce78e8, getter=0x1cd3678, stub=m.text_end+400;
    uint32_t words[] = {0x97ffaf64,0xaa1303e0}, get[] = {0xbd4c2c00,0xd65f03c0};
    memcpy(base+site,words,8); memcpy(base+getter,get,8);
    setenv("TOTK_ULTRAHAND_REALTIME","0",1); patch_ultrahand_vertical(&m,data);
    assert(!memcmp(base+site,words,8) && !*(uint32_t *)(base+stub));
    unsetenv("TOTK_ULTRAHAND_REALTIME");
    base[site]=0;
    if (!setjmp(guard_return)) { patch_ultrahand_vertical(&m,data); assert(0); }
    memcpy(base+site,words,8);
    base[stub]=1;
    if (!setjmp(guard_return)) { patch_ultrahand_vertical(&m,data); assert(0); }
    base[stub]=0;
    patch_ultrahand_vertical(&m,data);
    assert(!memcmp(base+getter,get,8));
    assert(*(uint32_t *)(base+site)==(branch_to(site,stub)|0x80000000u));
    assert(*(uint32_t *)(base+site+4)==words[1]);
    unsigned char camera[0xc30]={0};
    typedef float (*Getter)(void *);
    Getter f=(Getter)(base+stub);
    int count=0;
    for (int fps=30;fps<=120;fps+=10) for(int v=-4;v<=4;v++) {
        pthread_jit_write_protect_np(0);
        float sensitivity=v*.125f, ratio=30.0f/fps;
        memcpy(camera+0xc2c,&sensitivity,4); memcpy(base+data,&ratio,4);
        __builtin___clear_cache((char *)base+stub,(char *)base+stub+44);
        pthread_jit_write_protect_np(1);
        float got=f(camera), expected=sensitivity*ratio;
        assert(fabsf(got-expected)<1e-6f);
        /* Accumulated turn over one second must equal the 30 FPS baseline. */
        assert(fabsf(got*fps-sensitivity*30)<1e-4f);
        count++;
    }
    printf("PASS: %d native ARM64 cases; 30–120 FPS invariant; disabled and mismatch guards; shared getter unchanged\n",count);
    munmap(base,size);
}
