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
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
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
/* The one local user is the only qualified user, the selector picks it, and it has no network service account. */
#define NSA_UNAVAILABLE (124u|(280u<<9))
static uint32_t qualified_users(int *count,Uid *out,int max){if(!count)return 0x7c67;initialize();*count=max>0&&out?1:0;if(*count)out[0]=local;return 0;}
static uint32_t user_count(int *out){if(!out)return 0x7c67;initialize();*out=1;return 0;}
static uint32_t open_user(UserHandle *out,const Uid *uid){if(!out)runtime_fail("account null user handle");initialize();if(!same(uid))return 0x7c67;lock_acquire();for(unsigned i=0;i<64;i++)if(!tokens[i]){tokens[i]=nextToken++;*out=(UserHandle){local,tokens[i]};lock_release();return 0;}lock_release();return 0x7c67;}
static uint32_t select_user(Uid *out,const void *settings){(void)settings;if(!out)return 0x7c67;initialize();*out=local;return 0;}
static uint32_t nsa_available(unsigned char *out,const UserHandle *handle){if(!out)return 0x7c67;lock_acquire();bool ok=valid(handle);lock_release();*out=0;return ok?0:0x7c67;}
static uint32_t nsa_unavailable(void){return NSA_UNAVAILABLE;}
/* The console's profile image is a 256x256 JPEG; the local user's is a plain one, made once (under the account lock). */
static CFMutableDataRef jpeg;
static CFMutableDataRef make_profile_jpeg(void){
 CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();CGContextRef c=CGBitmapContextCreate(NULL,256,256,8,0,cs,kCGImageAlphaNoneSkipLast);
 CGContextSetRGBFillColor(c,0.85,0.15,0.15,1);CGContextFillRect(c,CGRectMake(0,0,256,256));CGImageRef img=CGBitmapContextCreateImage(c);
 CFMutableDataRef d=CFDataCreateMutable(NULL,0);CGImageDestinationRef dst=CGImageDestinationCreateWithData(d,CFSTR("public.jpeg"),1,NULL);
 bool ok=img&&dst;if(ok){CGImageDestinationAddImage(dst,img,NULL);ok=CGImageDestinationFinalize(dst);}
 if(dst)CFRelease(dst);if(img)CGImageRelease(img);CGContextRelease(c);CGColorSpaceRelease(cs);if(!ok){CFRelease(d);d=NULL;}
 return d;
}
static uint32_t profile_image(uint64_t *size,void *out,uint64_t capacity,const Uid *uid){
 initialize();if(!size||!same(uid))return 0x7c67;lock_acquire();if(!jpeg)jpeg=make_profile_jpeg();lock_release();if(!jpeg)runtime_fail("local profile image");
 uint64_t n=(uint64_t)CFDataGetLength(jpeg);*size=n;if(out)memcpy(out,CFDataGetBytePtr(jpeg),n<capacity?n:capacity);return 0;
}
void runtime_account_uid(uint64_t out[2]){initialize();out[0]=local.lo;out[1]=local.hi;}
#if defined(GAME_BOTW) || defined(WONDER_V121)
/* SDK 7 (Breath of the Wild 1.6.0, botw.md) opens the preselected user with the aborting variant and lists the users. */
static void open_preselected(UserHandle *out){if(!preselected(out))runtime_fail("open preselected user");}
static uint32_t list_users(int *count,Uid *out,int max){initialize();if(!count||(max>0&&!out))return 0x7c67;*count=0;if(max>0){out[0]=local;*count=1;}return 0;}
/* SDK 20.5 (1.9.0) and Super Mario Bros. Wonder 1.2.1 take the users the system opened at launch instead: the one local user. */
static uint32_t pop_open_users(int *count,UserHandle *out,int max){if(!count||(max>0&&!out))return 0x7c67;*count=0;if(max>0){open_preselected(&out[0]);*count=1;}return 0;}
#endif
void *runtime_account_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn7account10InitializeEv",initialize},{"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",preselected},
 {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",close_user},{"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",user_id},
 {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",existence},{"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",nickname},
 {"_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi",qualified_users},{"_ZN2nn7account12GetUserCountEPi",user_count},
 {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE",open_user},{"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE",select_user},
 {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",nsa_available},
 {"_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE",profile_image},
 {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",nsa_unavailable},
 {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",nsa_unavailable},
 {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",nsa_unavailable},
 {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",nsa_unavailable}
#if defined(GAME_BOTW) || defined(WONDER_V121)
 ,{"_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE",open_preselected},{"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_users}
 ,{"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi",pop_open_users}
#endif
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
