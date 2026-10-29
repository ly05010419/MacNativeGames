/* Exercise production fs operations, with only external game/ROM services stubbed. */
#include "../games/totk/runtime/fs_host.c"
#include <time.h>

static unsigned checks;
static _Atomic int commits, writer_started, writer_finished;
static _Atomic uint64_t sealed_generation;
static _Atomic int enable_started,enable_finished;
#define VERIFY(value, note) do { checks++; if (!(value)) { fprintf(stderr,"FAIL: %s\n",note); exit(1); } } while (0)
_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"runtime failure: %s\n",message); exit(2); }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
void runtime_account_uid(uint64_t out[2]) { out[0]=1;out[1]=2; }
int runtime_romfs_owns(const char *p) { (void)p; return 0; }
int runtime_romfs_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_open(uint64_t *o,const char *p,int m) { (void)o;(void)p;(void)m; return 1; }
unsigned runtime_romfs_read(uint64_t *o,uint64_t h,int64_t off,void *b,uint64_t n) { (void)o;(void)h;(void)off;(void)b;(void)n;return 1; }
unsigned runtime_romfs_size(int64_t *o,uint64_t h) { (void)o;(void)h;return 1; }
void runtime_romfs_close(uint64_t h) { (void)h; }
unsigned runtime_romfs_entry_type(int *o,const char *p) { (void)o;(void)p;return 1; }
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_fs_log(const char *what,const char *path) { (void)what;(void)path; }
void dm_guest_write_prepare(void *p,size_t n) { (void)p;(void)n; }
void totk_progress_save_committed(void) { atomic_fetch_add(&commits,1); }
void totk_progress_save_boundary(uint64_t epoch) {
    VERIFY(runtime_fs_save_busy(),"window close waits while committed local version is being sealed");
    VERIFY(pthread_rwlock_tryrdlock(&progress_gate)!=0,"commit seals under exclusive save gate");
    VERIFY(!atomic_load(&save_dirty),"exclusive commit seals committed bytes even if a writer handle stays open");
    atomic_store(&sealed_generation,epoch);atomic_fetch_add(&commits,1);
}
static void *writer(void *unused) {
    (void)unused; atomic_store(&writer_started,1);
    VERIFY(create_file("save:/other.sav",8)==0,"writer resumes after snapshot release");
    atomic_store(&writer_finished,1); return NULL;
}
static void *enable_sync(void *unused) {
    (void)unused;atomic_store(&enable_started,1);runtime_fs_progress_enable();atomic_store(&enable_finished,1);return NULL;
}
int main(void) {
    char root[]="/tmp/totk-progress-gate-XXXXXX";
    VERIFY(mkdtemp(root)!=NULL,"isolated profile");
    char device_profile[1024],managed_profile[1024],temp_file[1200];
    snprintf(device_profile,sizeof device_profile,"%s/device",root);snprintf(managed_profile,sizeof managed_profile,"%s/managed",root);
    setenv("TOTK_SAVE_PROFILE_DIR",managed_profile,1);setenv("TOTK_PROFILE_DIR",device_profile,1);
    uint64_t uid[]={1,2}, epoch=999, handle=0;
    VERIFY(mount_save("save",uid)==0,"save mount");
    VERIFY(!strcmp(profile,device_profile)&&!strncmp(save_root,managed_profile,strlen(managed_profile)),"managed saves do not redirect device cache and temporary storage");
    VERIFY(mount_temp("temp")==0&&create_file("temp:/device.tmp",4)==0,"device temporary mount stays available");
    snprintf(temp_file,sizeof temp_file,"%s/temp/device.tmp",device_profile);VERIFY(!access(temp_file,F_OK),"temporary file remains in device profile");
    snprintf(temp_file,sizeof temp_file,"%s/temp/device.tmp",managed_profile);VERIFY(access(temp_file,F_OK)!=0,"temporary file never enters a synced generation");
    VERIFY(!runtime_fs_progress_snapshot_begin(&epoch),"disabled feature cannot freeze snapshots");
    int guard=progress_read_enter();pthread_t enabling;
    VERIFY(pthread_create(&enabling,NULL,enable_sync,NULL)==0,"enable during existing mutation");
    while(!atomic_load(&enable_started))sched_yield();usleep(20000);
    VERIFY(!atomic_load(&enable_finished)&&!atomic_load(&progress_enabled),"opt-in waits for a mutation that started before enabling");
    progress_read_leave(&guard);pthread_join(enabling,NULL);
    VERIFY(atomic_load(&enable_finished),"enable finishes at a safe boundary");
    VERIFY(!runtime_fs_progress_snapshot_begin(&epoch),"enabling during a session waits for a subsequent commit");
    VERIFY(create_file("save:/progress.sav",16)==0,"create save");
    atomic_store(&save_changed_ns,runtime_now_ns()-10000000000ull);
    VERIFY(!runtime_fs_save_busy(),"old close heuristic has expired");
    VERIFY(!runtime_fs_progress_snapshot_begin(&epoch),"uncommitted save still cannot be snapshotted");
    VERIFY(commit("save")==0 && atomic_load(&commits)==1,"commit event");
    VERIFY(atomic_load(&sealed_generation)==1,"local version sealed before commit returns");
    VERIFY(runtime_fs_progress_snapshot_begin(&epoch) && epoch==1,"only committed generation eligible");
    pthread_t thread; VERIFY(pthread_create(&thread,NULL,writer,NULL)==0,"concurrent mutation starts");
    while (!atomic_load(&writer_started)) sched_yield();
    usleep(20000);
    VERIFY(!atomic_load(&writer_finished),"snapshot holds all fs mutation paths");
    runtime_fs_progress_snapshot_end(); pthread_join(thread,NULL);
    VERIFY(atomic_load(&writer_finished),"writer completed after release");
    VERIFY(!runtime_fs_progress_snapshot_begin(&epoch),"new uncommitted mutation blocks next snapshot");
    VERIFY(commit("save")==0,"commit following write");
    VERIFY(open_file(&handle,"save:/progress.sav",OPEN_WRITE)==0,"open writer handle");
    VERIFY(commit("save")==0 && !runtime_fs_progress_snapshot_begin(&epoch),"open writer blocks snapshot even after commit");
    VERIFY(atomic_load(&sealed_generation)==3,"open handle cannot suppress the synchronous frozen commit snapshot");
    close_file(handle);
    VERIFY(!runtime_fs_progress_snapshot_begin(&epoch),"close without final commit is not complete");
    VERIFY(commit("save")==0 && runtime_fs_progress_snapshot_begin(&epoch),"final commit permits snapshot"); runtime_fs_progress_snapshot_end();
    VERIFY(remove_tree(root)==0,"remove only fixture");
    printf("PASS: %u save gate checks (production fs, isolated profile)\n",checks);
}
