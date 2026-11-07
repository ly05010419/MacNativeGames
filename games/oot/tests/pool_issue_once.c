/* Calls the production HLE getters/Initialize/Finalize. Backend mocks track
   owner identity independently, including overlapping storage and reuse.
   Link this CPU-only test with -Wl,-undefined,dynamic_lookup; unrelated NVN
   entry points are never called. No GPU or guest executable is used. */
#define NVN_POOL_ISSUE_PROBE 1 // no unrelated dispatch-table bootstrap in this CPU test
#include "../third_party/d2r_gfx/gfx/nvn/hle_nvn.c"
#include <assert.h>

static pthread_mutex_t probe_mu=PTHREAD_MUTEX_INITIALIZER;
static struct {const void*owner;const void*mem;uint64_t size;} probe_owners[8];
static _Atomic uint64_t probe_marks;
uint64_t g_draw_skip[16],g_frame_pub,g_t0_ns;
_Atomic int g_totk_sr_on;
void*nvn_thunk[NVN_NFUNC];
const char*nvn_names[NVN_NFUNC];
void nvn_prof_entry(void){abort();}
void nvn_unimpl_entry(void){abort();}
__thread sigjmp_buf*runtime_fault_guard;
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2]={0};
void mtl_pool_add(const void*mem,uint64_t size){(void)mem;(void)size;}
void mtl_pool_del(const void*mem){(void)mem;}
void nvn_dirty_add_range(const void*mem,size_t size){(void)mem;(void)size;}
void nvn_dirty_remove_range(const void*mem){(void)mem;}
int totk_vpool_fini(pool_t*p){(void)p;return 0;}
void mtl_take_submit_thread(void){}
int runtime_core_away(void){return 0;}
void runtime_core_back(int*p){(void)p;}
void mtl_pool_addressed(const void*owner,const void*mem,uint64_t size){
    atomic_fetch_add(&probe_marks,1);pthread_mutex_lock(&probe_mu);
    int at=-1;for(int i=0;i<8;i++)if(probe_owners[i].owner==owner){at=i;break;}
    if(at<0)for(int i=0;i<8;i++)if(!probe_owners[i].owner){at=i;break;}
    assert(at>=0);probe_owners[at].owner=owner;probe_owners[at].mem=mem;probe_owners[at].size=size;
    pthread_mutex_unlock(&probe_mu);
}
void mtl_pool_forget_address(const void*owner){pthread_mutex_lock(&probe_mu);for(int i=0;i<8;i++)if(probe_owners[i].owner==owner)probe_owners[i].owner=NULL;pthread_mutex_unlock(&probe_mu);}
static int registered(const NVNmemoryPool*p){
    int found=0;const pool_t*o=(const void*)p;pthread_mutex_lock(&probe_mu);
    for(int i=0;i<8;i++)if(probe_owners[i].owner==p&&probe_owners[i].mem==o->mem&&probe_owners[i].size==o->size)found=1;
    pthread_mutex_unlock(&probe_mu);return found;
}
static void initialize(NVNmemoryPool*p,void*mem,size_t size){
    NVNmemoryPoolBuilder b={0};h_nvnMemoryPoolBuilderSetDefaults(&b);h_nvnMemoryPoolBuilderSetFlags(&b,0);
    h_nvnMemoryPoolBuilderSetStorage(&b,mem,size);assert(h_nvnMemoryPoolInitialize(p,&b));
    assert(!atomic_load(&((pool_t*)p)->address_issued));
}
static void*query(void*arg){NVNmemoryPool*p=arg;for(int i=0;i<10000;i++){assert(h_nvnMemoryPoolGetBufferAddress(p)==(uintptr_t)((pool_t*)p)->mem);assert(registered(p));}return NULL;}
int main(int argc,char**argv){
    (void)argv;setenv("TOTK_ASYNC_REPLAY","0",1);uint8_t memory[8192];_Alignas(64) NVNmemoryPool a,b;
    nvn_pool_issue_once_set(1);initialize(&a,memory,4096);
    if(argc>1)atomic_store(&((pool_t*)&a)->address_issued,1); // deliberate missing publication
    assert(h_nvnMemoryPoolGetBufferAddress(&a)==(uintptr_t)memory);assert(registered(&a));
    uint64_t first=atomic_load(&probe_marks);query(&a);assert(atomic_load(&probe_marks)==first);
    NVNbufferBuilder bb={0};NVNbuffer buffer;
    h_nvnBufferBuilderSetDefaults(&bb);h_nvnBufferBuilderSetStorage(&bb,&a,128,64);assert(h_nvnBufferInitialize(&buffer,&bb));
    for(int i=0;i<10000;i++)assert(h_nvnBufferGetAddress(&buffer)==(uintptr_t)(memory+128));
    assert(atomic_load(&probe_marks)==first);
    initialize(&b,memory,4096);query(&b);assert(registered(&a)&&registered(&b));
    h_nvnBufferFinalize(&buffer);h_nvnMemoryPoolFinalize(&a);assert(!registered(&a)&&registered(&b));
    initialize(&a,memory+4096,4096);pthread_t threads[8];
    for(int i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,query,&a));
    for(int i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));assert(registered(&a)&&registered(&b));
    assert(h_nvnBufferInitialize(&buffer,&bb));
    first=atomic_load(&probe_marks);query(&a);assert(atomic_load(&probe_marks)==first);
    nvn_pool_issue_once_set(0);for(int i=0;i<10;i++)h_nvnBufferGetAddress(&buffer);assert(atomic_load(&probe_marks)==first+10);
    nvn_pool_issue_once_set(1);query(&a);assert(atomic_load(&probe_marks)==first+10);
    // The opaque public object type has no declared alignment in our header.
    // Unaligned storage must keep the locked path rather than issue an ARM
    // exclusive atomic access at an unaligned address.
    uint8_t raw[sizeof(NVNmemoryPool)+8];NVNmemoryPool*unaligned=(void*)(raw+1);
    NVNmemoryPoolBuilder ub={0};h_nvnMemoryPoolBuilderSetStorage(&ub,memory,4096);
    assert(h_nvnMemoryPoolInitialize(unaligned,&ub));first=atomic_load(&probe_marks);
    for(int i=0;i<10;i++)assert(h_nvnMemoryPoolGetBufferAddress(unaligned)==(uintptr_t)memory);
    assert(atomic_load(&probe_marks)==first+10);h_nvnMemoryPoolFinalize(unaligned);
    h_nvnBufferFinalize(&buffer);h_nvnMemoryPoolFinalize(&a);h_nvnMemoryPoolFinalize(&b);assert(!registered(&a)&&!registered(&b));
    printf("PASS pool_issue_once: getters, buffer offsets, 8 readers, overlapping owners, Finalize/reinitialize, on/off\n");return 0;
}
