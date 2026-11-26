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
#include "account_users.h"
#include "user_lock.h"
typedef struct{Uid uid;uint64_t token;} UserHandle;
#define MAX_USERS MNG_MAX_USERS
static unsigned user_count;
static Uid users[MAX_USERS];
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
static unsigned tokenUser[64];
/* Indexed by numeric UID, not selected-first account index. FD+1 allows zero initialization. */
static int userLocks[33];
static uint64_t userEpochs[33];
static const char *profile_dir(void){const char *p=getenv("TOTK_PROFILE_DIR");return p?p:"user-data";}
/* Called under account mutex; re-read registry while holding its transaction lock so
   delete/archive cannot pass between membership validation and ownership acquisition. */
static int reserve_user(unsigned u){
 unsigned id=(unsigned)users[u].lo;
 if(userLocks[id])return 1;
 int registry=mng_registry_lock(profile_dir());if(registry<0)return 0;
 account_user_count=0;account_users_load(profile_dir());
 uint64_t epoch;
 int fd=account_user_position(id)>=0&&mng_user_epoch(profile_dir(),id,&epoch)&&epoch==userEpochs[id]
  ?mng_user_lock(profile_dir(),id):-1;
 close(registry);
 if(fd<0)return 0;
 userLocks[id]=fd+1;return 1;
}
static void initialize(void){
 lock_acquire();if(users[0].lo||users[0].hi){lock_release();return;}
 const char *dir=getenv("TOTK_PROFILE_DIR");if(!dir)dir="user-data";
 if(mkdir(dir,0700)&&errno!=EEXIST)runtime_fail("create account profile directory");
 int registry=mng_registry_lock(dir);if(registry<0){perror("lock account registry");runtime_fail("lock account registry");}
 account_users_load(dir);
 for(unsigned i=0;i<account_user_count;i++){
  unsigned id=account_user_ids[i];
  if(!mng_user_epoch(dir,id,&userEpochs[id]))runtime_fail("read account generation");
 }
 const char *inherited=getenv("MNG_USER_LOCK_FD");
 int fd=inherited?mng_user_lock_adopt(dir,account_selected,inherited):mng_user_lock(dir,account_selected);
 if(fd<0)runtime_fail("selected user is already in use or cannot be locked");
 userLocks[account_selected]=fd+1;unsetenv("MNG_USER_LOCK_FD");
 close(registry);
 users[0]=(Uid){account_selected,0};user_count=1;
 for(unsigned i=0;i<account_user_count;i++)if(account_user_ids[i]!=account_selected)
  users[user_count++]=(Uid){account_user_ids[i],0};
 lock_release();fprintf(stderr,"[totk] persistent local accounts (%u registered) initialized\n",user_count);
}
static int find_user(const Uid *uid){
 if(!uid)return -1;
 for(unsigned u=0;u<user_count;u++)if(uid->lo==users[u].lo&&uid->hi==users[u].hi)return (int)u;
 return -1;
}
static int valid_slot(const UserHandle *handle){
 if(!handle)return -1;
 int u=find_user(&handle->uid);if(u<0)return -1;
 for(unsigned i=0;i<64;i++)if(tokens[i]&&tokens[i]==handle->token&&tokenUser[i]==(unsigned)u)return (int)i;
 return -1;
}
static unsigned open_user_index(UserHandle *out,unsigned u){
 initialize();
 if(!out||u>=user_count)runtime_fail("account null user handle");
 lock_acquire();
 if(!reserve_user(u)){lock_release();return 0;}
 for(unsigned i=0;i<64;i++)if(!tokens[i]){tokens[i]=nextToken++;tokenUser[i]=u;*out=(UserHandle){users[u],tokens[i]};lock_release();return 1;}
 lock_release();return 0;
}
static unsigned preselected(UserHandle *out){return open_user_index(out,0);}
static uint32_t user_id(Uid *out,const UserHandle *handle){lock_acquire();int s=out?valid_slot(handle):-1;if(s>=0)*out=users[tokenUser[s]];lock_release();return s>=0?0:0x7c67;}
static void close_user(const UserHandle *handle){lock_acquire();int s=valid_slot(handle);if(s<0)runtime_fail("invalid local account handle close");tokens[s]=0;lock_release();}
static uint32_t existence(unsigned char *out,const Uid *uid){if(!out||!uid)return 0x7c67;initialize();*out=find_user(uid)>=0;return 0;}
static uint32_t nickname(char *out,const Uid *uid){
 initialize();int u=out?find_user(uid):-1;if(u<0)return 0x7c67;
 memset(out,0,33);
 lock_acquire();snprintf(out,33,"%s",account_user_label((unsigned)users[u].lo));lock_release();
 return 0;
}
static uint32_t open_preselected(UserHandle *out){return preselected(out)?0:0x7c67;}
static uint32_t open_user(UserHandle *out,const Uid *uid){initialize();int u=out?find_user(uid):-1;if(u<0)return 0x7c67;return open_user_index(out,(unsigned)u)?0:0x7c67;}
static uint32_t list_all_users(int32_t *out_count,Uid *out_uids,int32_t max){
 initialize();if(!out_count)return 0x7c67;
 int32_t n=(int32_t)user_count;
 if(out_uids&&max>0){int32_t copy=max<n?max:n;for(int32_t i=0;i<copy;i++)out_uids[i]=users[i];n=copy;}
 *out_count=n;return 0;
}
static uint32_t list_open_users(int32_t *out_count,Uid *out_uids,int32_t max){
 initialize();if(!out_count)return 0x7c67;
 lock_acquire();
 int32_t n=0;unsigned seen=0;
 for(unsigned i=0;i<64;i++)if(tokens[i]){
  unsigned u=tokenUser[i];
  if(!(seen&(1u<<u))){seen|=1u<<u;if(out_uids&&n<max)out_uids[n]=users[u];n++;}
 }
 lock_release();
 *out_count=n;return 0;
}
static uint32_t push_open_users(void){return 0;}
static uint32_t pop_open_users(int32_t *out_count,UserHandle *out_handles,int32_t max){
 initialize();if(!out_count)return 0x7c67;
 if(out_handles&&max>=1){*out_count=open_user_index(&out_handles[0],0)?1:0;}
 else *out_count=0;
 return 0;
}
static uint32_t show_user_selector(Uid *out_uid,const void *settings){
 initialize();if(!out_uid)return 0x7c67;
 const Uid *invalid=(const Uid *)settings;
 lock_acquire();
 for(unsigned u=0;u<user_count;u++){
   bool skip=false;
   if(invalid){
    for(unsigned k=0;k<8;k++)if(invalid[k].lo==users[u].lo&&invalid[k].hi==users[u].hi){skip=true;break;}
   }
   if(!skip){
    for(unsigned i=0;i<64;i++)if(tokens[i]&&tokenUser[i]==u){skip=true;break;}
   }
   if(!skip&&reserve_user(u)){*out_uid=users[u];lock_release();fprintf(stderr,"[totk] ShowUserSelector selected user %llu\n",(unsigned long long)users[u].lo);return 0;}
 }
 lock_release();
 // Never assign an already-open account to another player. Add users before boot.
 memset(out_uid,0,sizeof *out_uid);return 0x7c67;
}
static uint32_t is_nsa_available(bool *out,const UserHandle *handle){(void)handle;if(out)*out=false;return 0;}
static uint32_t ensure_nsa_available(const UserHandle *handle){(void)handle;return 0;}
static uint32_t get_nsa_id(uint64_t *out,const UserHandle *handle){(void)handle;if(out)*out=0;return 0;}
static uint32_t load_nsa_token_cache(uint64_t *out_size,char *buf,uint64_t cap,const UserHandle *handle){(void)buf;(void)cap;(void)handle;if(out_size)*out_size=0;return 0;}
static uint32_t show_license_req(const UserHandle *handle){(void)handle;return 0;}
void runtime_account_uid(uint64_t out[2]){initialize();out[0]=users[0].lo;out[1]=users[0].hi;}
int runtime_account_uid_index(const uint64_t uid[2]){if(!uid)return -1;initialize();Uid u={uid[0],uid[1]};return find_user(&u);}
/* All save entry points must reserve, even if a title never called OpenUser. */
int runtime_account_reserve_uid(const uint64_t uid[2]){
 if(!uid)return 0;initialize();Uid value={uid[0],uid[1]};
 lock_acquire();int u=find_user(&value);int ok=u>=0&&reserve_user((unsigned)u);lock_release();return ok;
}
void *runtime_account_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn7account10InitializeEv",initialize},{"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",preselected},
 {"_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE",open_preselected},
 {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE",open_user},
 {"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_all_users},
 {"_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi",list_all_users},
 {"_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi",list_open_users},
 {"_ZN2nn7account13PushOpenUsersEv",push_open_users},
 {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi",pop_open_users},
 {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE",show_user_selector},
 {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",is_nsa_available},
 {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",ensure_nsa_available},
 {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",get_nsa_id},
 {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",load_nsa_token_cache},
 {"_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE",show_license_req},
 {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",close_user},{"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",user_id},
 {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",existence},{"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",nickname}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
