/* Offline local user identity. Handles are validated; profile survives restarts. */
#include "runtime.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef struct{uint64_t lo,hi;} Uid;
typedef struct{Uid uid;uint64_t token;} UserHandle;
static Uid local;
/* Canaries around the lock: an intermittent hang showed this mutex's internal words changed while nobody held it. */
#define GUARD 0x6163636f756e7447ULL
static struct{uint64_t pre[8];pthread_mutex_t m;uint64_t post[8];} guarded={{GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD},PTHREAD_MUTEX_INITIALIZER,{GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD,GUARD}};
#define lock_ guarded.m
static _Atomic(uint64_t) lock_owner;
static void check_guard(void){for(unsigned i=0;i<8;i++)if(guarded.pre[i]!=GUARD||guarded.post[i]!=GUARD){fprintf(stderr,"[totk] account lock guard %s[%u] overwritten: %#llx\n",guarded.pre[i]!=GUARD?"pre":"post",i,(unsigned long long)(guarded.pre[i]!=GUARD?guarded.pre[i]:guarded.post[i]));runtime_fail("host memory near the account lock was overwritten");}}
static void lock_acquire(void){check_guard();pthread_mutex_lock(&lock_);uint64_t t;pthread_threadid_np(NULL,&t);lock_owner=t;}
static void lock_release(void){lock_owner=0;pthread_mutex_unlock(&lock_);check_guard();}
uint64_t runtime_account_lock_owner(void){const uint32_t *w=(const uint32_t *)(const void *)&guarded;fprintf(stderr,"[totk-dump] account lock with guards @%p:",(void *)&guarded);for(unsigned i=0;i<sizeof(guarded)/4;i++)fprintf(stderr," %08x",w[i]);fputc('\n',stderr);return lock_owner;}
static uint64_t tokens[64],nextToken=1;
static void initialize(void){
 lock_acquire();if(local.lo||local.hi){lock_release();return;}
 const char *dir=getenv("TOTK_PROFILE_DIR");if(!dir)dir="user-data";
 if(mkdir(dir,0700)&&errno!=EEXIST)runtime_fail("create local account directory");
 char path[4096];if(snprintf(path,sizeof(path),"%s/account.uid",dir)>=(int)sizeof(path))runtime_fail("local account path length");
 int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(fd<0||flock(fd,LOCK_EX))runtime_fail("open local account identity");
 struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode))runtime_fail("invalid local account identity file");
 if(st.st_size==0){do{arc4random_buf(&local,sizeof(local));}while(!local.lo&&!local.hi);if(write(fd,&local,sizeof(local))!=sizeof(local)||fsync(fd))runtime_fail("persist local account identity");}
 else if(st.st_size!=sizeof(local)||pread(fd,&local,sizeof(local),0)!=sizeof(local)||(!local.lo&&!local.hi))runtime_fail("corrupt local account identity");
 close(fd);lock_release();fprintf(stderr,"[totk] persistent local account initialized\n");
}
static bool same(const Uid *uid){return uid&&uid->lo==local.lo&&uid->hi==local.hi;}
static bool valid(const UserHandle *handle){if(!handle||!same(&handle->uid))return false;for(unsigned i=0;i<64;i++)if(tokens[i]&&tokens[i]==handle->token)return true;return false;}
static unsigned preselected(UserHandle *out){if(!out)runtime_fail("account null user handle");initialize();lock_acquire();for(unsigned i=0;i<64;i++)if(!tokens[i]){tokens[i]=nextToken++;*out=(UserHandle){local,tokens[i]};lock_release();return 1;}lock_release();return 0;}
static uint32_t user_id(Uid *out,const UserHandle *handle){lock_acquire();bool ok=out&&valid(handle);if(ok)*out=local;lock_release();return ok?0:0x7c67;}
static void close_user(const UserHandle *handle){lock_acquire();if(!valid(handle))runtime_fail("invalid local account handle close");for(unsigned i=0;i<64;i++)if(tokens[i]==handle->token){tokens[i]=0;break;}lock_release();}
static uint32_t existence(unsigned char *out,const Uid *uid){if(!out||!uid)return 0x7c67;initialize();*out=same(uid);return 0;}
static uint32_t nickname(char *out,const Uid *uid){initialize();if(!out||!same(uid))return 0x7c67;memset(out,0,33);memcpy(out,"Local",5);return 0;}
void runtime_account_uid(uint64_t out[2]){initialize();out[0]=local.lo;out[1]=local.hi;}
/* SDK 7.3.2 user enumeration and selection: one local user, selected without the applet. */
static uint32_t user_count(int *out){if(!out)return 0x7c67;initialize();*out=1;return 0;}
static uint32_t list_users(int *count,Uid *out,int max){if(!count||(max>0&&!out))return 0x7c67;initialize();*count=max>0;if(max>0)out[0]=local;return 0;}
static uint32_t last_opened(Uid *out){if(!out)return 0x7c67;initialize();*out=local;return 0;}
static uint32_t open_user(UserHandle *out,const Uid *uid){if(!out||!same(uid))return 0x7c67;return preselected(out)?0:0x7c67;}
static uint32_t select_user(Uid *out,const void *settings){(void)settings;return last_opened(out);}
/* ---------- Network service account / Nintendo Account: this host has no networking (2026-11-13) ----------
   Engage asks for these around its first boss fight. main imports 27 account entries and this file hosted 11: the rest fell
   to the SDK, whose IsNetworkServiceAccountAvailable needs the acc:u0 session the host never opens and read a null session
   object -- measured: signal 0xb, fault 0x1, esr 0x92000006 (data abort, translation fault L2, address 1), sdk+0x7de4,
   inside _ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE (starts 0x7dd0). The process ended there.

   Semantics follow the upstream host (dist/work/eng_src/dm/host/hle_account.c): no network service account is linked, so
   "is it available" succeeds and answers no, anything that needs the account itself fails, and an async request reports
   done at once with a failure. Nothing here reports a network this port does not have.

   Deliberately NOT taken over: _ZN2nn7account12AsyncContextC1Ev. The SDK's own constructor is correct -- it writes the
   object's vtable and nulls the IPC pointer at +0x18 and contacts nothing -- and upstream records why an empty host
   constructor is worse: it leaves the allocator's bytes in the vtable word, and the virtual deleting destructor a game
   reaches from its finalizer then jumps through them. Nothing below sets that pointer, so the SDK's destructor has no
   session to close. */
#define R_ACCOUNT_FAIL 0x7c67u   /* the failure code the entries above use; games only test IsSuccess */
#define ACC_ONCE(...) do { static int said; if (!said++) fprintf(stderr, "[totk-account] " __VA_ARGS__); } while (0)

/* The query itself succeeds and the answer is "no": a console without a linked network account answers exactly this. */
static uint32_t nsa_available(unsigned char *out,const UserHandle *handle){(void)handle;if(!out)return R_ACCOUNT_FAIL;*out=0;ACC_ONCE("IsNetworkServiceAccountAvailable -> false (no networking; the query itself succeeds)\n");return 0;}
/* Ensuring one cannot succeed: there is no account to be made available -- a failure, not a faked success. */
static uint32_t nsa_ensure(const UserHandle *handle){(void)handle;ACC_ONCE("EnsureNetworkServiceAccountAvailable -> failure (no network service account)\n");return R_ACCOUNT_FAIL;}
static uint32_t nsa_id(void *id,const UserHandle *handle){(void)id;(void)handle;ACC_ONCE("GetNetworkServiceAccountId -> failure (no network service account; the id is left untouched)\n");return R_ACCOUNT_FAIL;}
static uint32_t nsa_token(uint64_t *n,char *buf,uint64_t size,const UserHandle *handle){(void)buf;(void)size;(void)handle;if(n)*n=0;ACC_ONCE("LoadNetworkServiceAccountIdTokenCache -> failure, 0 bytes (no networking)\n");return R_ACCOUNT_FAIL;}
static uint32_t nsa_token_async(void *context,const UserHandle *handle){(void)context;(void)handle;ACC_ONCE("EnsureNetworkServiceAccountIdTokenCacheAsync -> failure (no networking)\n");return R_ACCOUNT_FAIL;}
/* The async context: no request was sent, so it is done at once and its result is the failure above. HasDone/GetResult are
   what a game polls after GetSystemEvent. */
static uint32_t async_event(void *context,void *event){(void)context;if(!event)return R_ACCOUNT_FAIL;hle_signal_system_event(event);ACC_ONCE("AsyncContext::GetSystemEvent -> signalled at once (no request is sent)\n");return 0;}
static uint32_t async_has_done(void *context,unsigned char *done){(void)context;if(!done)return R_ACCOUNT_FAIL;*done=1;return 0;}
static uint32_t async_result(void *context){(void)context;ACC_ONCE("AsyncContext::GetResult -> failure (networking is out of scope)\n");return R_ACCOUNT_FAIL;}
static uint32_t async_cancel(void *context){(void)context;return 0;}
static void async_destroy(void *context){(void)context;}
/* User lists: the one local user this port already has (see list_users / user_count above). */
static uint32_t list_open_users(int *count,Uid *out,int max){if(!count||(max>0&&!out))return R_ACCOUNT_FAIL;initialize();*count=max>0;if(max>0)out[0]=local;ACC_ONCE("ListOpenUsers -> the one local user\n");return 0;}
static uint32_t list_qualified_users(int *count,Uid *out,int max){if(!count||(max>0&&!out))return R_ACCOUNT_FAIL;initialize();*count=max>0;if(max>0)out[0]=local;ACC_ONCE("ListQualifiedUsers -> the one local user\n");return 0;}
/* The user-creation applet is a system screen this host does not draw: there is already a local user, so nothing to do. */
static uint32_t show_user_creator(void){ACC_ONCE("ShowUserCreator -> success (the host already has its local user; no applet)\n");return 0;}
/* No picture is stored for the local user, so this fails honestly rather than handing the game a buffer of garbage. Upstream
   ships a small JPEG instead because some games decode the result without checking it (Burnout Paradise's JpegDecoder
   asserted); if Engage ever does, the fix is to return that image here. */
static uint32_t load_profile_image(uint64_t *n,void *buf,uint64_t size,const Uid *uid){(void)buf;(void)size;(void)uid;if(n)*n=0;ACC_ONCE("LoadProfileImage -> failure, 0 bytes (no picture is stored)\n");return R_ACCOUNT_FAIL;}
/* The system's data-management screen shows this thumbnail; this host has no such screen and keeps none. */
static uint32_t store_thumbnail(const Uid *uid,const void *image,uint64_t size){(void)uid;(void)image;(void)size;ACC_ONCE("StoreSaveDataThumbnailImage -> success (nothing is stored: no data-management screen)\n");return 0;}
static uint32_t delete_thumbnail(const Uid *uid){(void)uid;return 0;}

void *runtime_account_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn7account10InitializeEv",initialize},{"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",preselected},
 {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",close_user},{"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",user_id},
 {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",existence},{"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",nickname},
 {"_ZN2nn7account12GetUserCountEPi",user_count},{"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_users},
 {"_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE",last_opened},{"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE",open_user},
 {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE",select_user},
 {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",nsa_available},
 {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",nsa_ensure},
 {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",nsa_id},
 {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",nsa_token},
 {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",nsa_token_async},
 {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",async_event},
 {"_ZN2nn7account12AsyncContext7HasDoneEPb",async_has_done},
 {"_ZN2nn7account12AsyncContext9GetResultEv",async_result},
 {"_ZN2nn7account12AsyncContext6CancelEv",async_cancel},
 {"_ZN2nn7account12AsyncContextD1Ev",async_destroy},
 {"_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi",list_open_users},
 {"_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi",list_qualified_users},
 {"_ZN2nn7account15ShowUserCreatorEv",show_user_creator},
 {"_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE",load_profile_image},
 {"_ZN2nn7account27StoreSaveDataThumbnailImageERKNS0_3UidEPKvm",store_thumbnail},
 {"_ZN2nn7account28DeleteSaveDataThumbnailImageERKNS0_3UidE",delete_thumbnail}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
