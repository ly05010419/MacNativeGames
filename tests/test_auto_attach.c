/* Fake live actor objects: exercise the real callbacks and input filter without loading game content. */
#define TOTK_GAME_V143 1
#include "../games/totk/runtime/auto_attach.c"
#include <assert.h>

static int focused=1, saved=-1;
int runtime_front_game_input(void) { return focused; }
int runtime_setting_word(const char *file,char *word,size_t n) { (void)file;(void)word;(void)n;return 0; }
void runtime_setting_save(const char *file,const char *word,const char *what) {
    (void)what;assert(!strcmp(file,"auto-attach.txt"));saved=!strcmp(word,"on");
}
void *runtime_hle_wrap_named(void *fn,const char *name) { (void)name;return fn; }
_Noreturn void runtime_fail(const char *why) { fprintf(stderr,"unexpected failure: %s\n",why);abort(); }
static void put64(void *obj,size_t off,uint64_t v) { memcpy((char *)obj+off,&v,8); }
static void put32(void *obj,size_t off,uint32_t v) { memcpy((char *)obj+off,&v,4); }
#define PTR(x) ((uint64_t)(uintptr_t)(x))

int main(void) {
    /* The exporting Mac runtime must leave iOS game code untouched, before reading any module fields. */
    Module unused={0};
    setenv("TOTK_EXPORT_IOS","test-export",1);
    runtime_auto_attach_install(&unused);assert(!runtime_auto_attach_available());
    unsetenv("TOTK_EXPORT_IOS");
    unsigned char player[0x1000]={0},arrow[0x500]={0},equipment[0x4800]={0},bullet[0x200]={0},bow[0x600]={0};
    unsigned char block[0x800]={0},draw[16]={0},component[0x200]={0},link[32]={0};
    put64(player,0x218,PTR("Player"));put64(arrow,0x218,PTR("NormalArrow"));
    put64(equipment,0x18,PTR(player));put64(bullet,0x18,PTR(arrow));
    put64(player,0x248,PTR(block));put64(block,0x760,PTR(draw));draw[9]=0x20;
    put64(bullet,0x50,PTR(component));put64(component,0x130,PTR(link));
    put32(player,0x380,100);put32(arrow,0x380,200);
    /* The same resident-actor path as the production bow reader; a paused actor may keep its drawn bit. */
    main_base=calloc(1,GAME_ADDR(0x4663f08)+8);assert(main_base);
    unsigned char module[0x200]={0},scene[0x60]={0},components[0x70]={0},resident[0x30]={0},links[0x70]={0},record[0x50]={0};
    put64(main_base,GAME_ADDR(0x4663f08),PTR(module));put64(module,0x1e8,PTR(scene));
    put32(scene,0x50,14);put64(scene,0x58,PTR(components));put64(components,13*8,PTR(resident));
    put32(resident,0x20,1);put64(resident,0x28,PTR(links));put64(links,8,PTR(record));put64(record,0x40,PTR(player));
    atomic_store(&installed,1);runtime_auto_attach_choose(1);assert(saved==1);
    const char *fire="FireFruit",*seed="LightBall_Small";
    /* Enabling cannot mistake a last weapon/shield fuse for an arrow selection. */
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));
    manual(PTR(equipment),&fire);put32(arrow,0x380,201);
    uintptr_t request=decide(PTR(equipment),PTR(bullet),PTR(bow));
    assert(request>1 && !strcmp(*(const char *const *)request,fire));
    for (int i=0;i<240;i++) assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));
    /* The same allocation with a new actor identity is a new arrow. */
    put32(arrow,0x380,202);put64(bow,0x2b8,123);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));put64(bow,0x2b8,0);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow))); /* never overwrite a manual request after it finishes */
    manual(PTR(equipment),&seed);put32(arrow,0x380,203);
    request=decide(PTR(equipment),PTR(bullet),PTR(bow));assert(request>1 && !strcmp(*(const char *const *)request,seed));
    put64(link,8,123);put32(link,0x10,1);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));
    assert(runtime_auto_attach_input(1|8)==8);
    assert(decide(PTR(equipment),PTR(bullet),PTR(bow))==1);
    assert(runtime_auto_attach_input(1|8)==8); /* whole held press swallowed */
    assert(runtime_auto_attach_input(0)==0);
    put32(player,0x5ac,0x40);
    assert(runtime_auto_attach_input(1)==1); /* confirm in a paused menu must remain ordinary A */
    assert(!atomic_load(&remove_arrow));runtime_auto_attach_input(0);put32(player,0x5ac,0);
    put64(link,8,0);put32(arrow,0x380,204);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow))); /* stopped until a manual choice */
    manual(PTR(equipment),&seed);put32(arrow,0x380,205);
    assert(decide(PTR(equipment),PTR(bullet),PTR(bow))>1); /* same manual item resumes */
    put32(arrow,0x380,206);focused=0;
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));assert(runtime_auto_attach_input(1)==1);
    focused=1;put32(player,0x5ac,0x40);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));put32(player,0x5ac,0);
    assert(decide(PTR(equipment),PTR(bullet),PTR(bow))>1); /* may act only after focus and pause clear */
    put32(arrow,0x380,207);draw[9]=0;
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow)));draw[9]=0x20;
    assert(decide(PTR(equipment),PTR(bullet),PTR(bow))>1);
    put32(player,0x380,101);put32(arrow,0x380,208);
    assert(!decide(PTR(equipment),PTR(bullet),PTR(bow))); /* a new world/player clears the remembered material */
    runtime_auto_attach_choose(0);assert(!runtime_auto_attach_choice() && saved==0);
    assert(runtime_auto_attach_input(0)==0 && runtime_auto_attach_input(1)==1);
    free(main_base);
    puts("PASS: arrow identity, manual priority, pause/focus, removal, resume and input consumption");
}
