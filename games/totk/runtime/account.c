/* Offline local users, one save area each (fs_host.c names the save directory after the user's id). Handles are
   validated; the users survive restarts in <TOTK_PROFILE_DIR>/users.txt. The user in force is the preselected
   one, as on a console: chosen before the game starts (settings_panel.h's User page, from the next launch), and a
   running game keeps the user it opened with even when another one is chosen afterwards. */
#include "runtime.h"
#include "users.h"
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
static Uid local;   /* the user this run opened with; runtime_users_choose never touches it */
static RuntimeUser users[RUNTIME_USERS_MAX];
static int usersCount,usersActive;   /* usersActive: the user the next launch opens with */
static char usersDir[4096];
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

static const RuntimeUser *user_of(const Uid *uid){for(int i=0;i<usersCount;i++)if(users[i].lo==uid->lo&&users[i].hi==uid->hi)return &users[i];return NULL;}
static void hex32(char out[33],const RuntimeUser *u){snprintf(out,33,"%016llx%016llx",(unsigned long long)u->hi,(unsigned long long)u->lo);}
/* 32 hex digits, high half first, the way hex32 (and the save directory's name, fs_host.c) write them. strtoull
   would take the whole digit run for one half, so each half is cut out and read on its own. */
static int parse32(const char *s,Uid *uid){
 if(strlen(s)!=32)return 0;char half[17];half[16]=0;char *end;Uid u;
 memcpy(half,s,16);u.hi=strtoull(half,&end,16);if(end!=half+16)return 0;
 memcpy(half,s+16,16);u.lo=strtoull(half,&end,16);if(end!=half+16)return 0;
 *uid=u;return 1;
}
/* The name as the store keeps it: control characters (tab and newline included) dropped, trimmed, at most 31 bytes
   of UTF-8 cut at a character boundary, and never empty. */
static size_t clean_name(char *out,const char *name){
 size_t o=0;const unsigned char *p=(const unsigned char *)name;
 while(*p==' '||*p=='\t')p++;
 while(*p&&o<31){if(*p>=0x20&&*p!=0x7f){unsigned k=*p<0x80?1:*p<0xe0?2:*p<0xf0?3:4;int good=1;for(unsigned i=1;i<k&&good;i++)good=(p[i]&0xc0)==0x80;if(!good)k=1;if(o+k>31)break;memcpy(out+o,p,k);o+=k;p+=k;continue;}p++;}
 out[o]=0;while(o&&(out[o-1]==' '||out[o-1]=='\t'))out[--o]=0;return o;
}
/* users.txt, written whole so a half-written store never survives: an optional '*' first marks the user the next
   launch opens with, then 32 hex digits of id, a tab, and the name to the end of the line. */
static void users_file_save(void){
 char path[4200],tmp[4200];if(snprintf(path,sizeof path,"%s/users.txt",usersDir)>=(int)sizeof path)return;
 snprintf(tmp,sizeof tmp,"%s.tmp-%d",path,getpid());
 FILE *f=fopen(tmp,"w");if(!f){fprintf(stderr,"[totk] users: cannot write %s\n",tmp);return;}
 for(int i=0;i<usersCount;i++){char hex[33];hex32(hex,&users[i]);fprintf(f,"%s%s\t%s\n",i==usersActive?"*":"",hex,users[i].name);}
 if(fclose(f)){unlink(tmp);return;}
 if(rename(tmp,path))fprintf(stderr,"[totk] users: cannot replace %s\n",path);
}
/* Reads users.txt into users[]; returns the marked user's index, or -1 when none is marked. */
static int users_file_load(void){
 char path[4200];if(snprintf(path,sizeof path,"%s/users.txt",usersDir)>=(int)sizeof path)return -1;
 FILE *f=fopen(path,"r");if(!f)return -1;
 char line[512];int marked=-1;
 while(fgets(line,sizeof line,f)&&usersCount<RUNTIME_USERS_MAX){
  char *s=line;size_t n=strlen(s);while(n&&(s[n-1]=='\n'||s[n-1]=='\r'))s[--n]=0;
  int star=*s=='*';if(star)s++;
  char *tab=strchr(s,'\t');if(!tab)continue;*tab=0;
  Uid uid;if(!parse32(s,&uid))continue;if(user_of(&uid))continue;
  RuntimeUser *u=&users[usersCount];u->lo=uid.lo;u->hi=uid.hi;clean_name(u->name,tab+1);if(!u->name[0])strcpy(u->name,"Local");
  if(star&&marked<0)marked=usersCount;
  usersCount++;
 }
 fclose(f);return marked;
}
static void initialize(void){
 lock_acquire();if(local.lo||local.hi){lock_release();return;}
 const char *dir=getenv("TOTK_PROFILE_DIR");if(!dir)dir="user-data";
 if(mkdir(dir,0700)&&errno!=EEXIST)runtime_fail("create local account directory");
 if(snprintf(usersDir,sizeof usersDir,"%s",dir)>=(int)sizeof usersDir)runtime_fail("local account path length");
 int marked=users_file_load();
 if(!usersCount){
  /* A profile from before the users: its one identity (account.uid) becomes the user "Local". A profile with
     neither file is new: it takes a fresh identity, and account.uid, a thing of the single-user past, is not
     written any more. */
  Uid legacy={0,0};
  char path[4200];snprintf(path,sizeof path,"%s/account.uid",usersDir);
  int fd=open(path,O_RDWR|O_NOFOLLOW,0600);
  if(fd<0&&errno!=ENOENT)runtime_fail("open local account identity");
  if(fd<0){arc4random_buf(&legacy,sizeof legacy);while(!legacy.lo&&!legacy.hi)arc4random_buf(&legacy,sizeof legacy);}
  else{
   if(flock(fd,LOCK_EX))runtime_fail("lock local account identity");
   struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode))runtime_fail("invalid local account identity file");
   if(st.st_size==0){do{arc4random_buf(&legacy,sizeof legacy);}while(!legacy.lo&&!legacy.hi);if(write(fd,&legacy,sizeof(legacy))!=sizeof(legacy)||fsync(fd))runtime_fail("persist local account identity");}
   else if(st.st_size!=sizeof(legacy)||pread(fd,&legacy,sizeof(legacy),0)!=sizeof(legacy)||(!legacy.lo&&!legacy.hi))runtime_fail("corrupt local account identity");
   close(fd);
  }
  users[0].lo=legacy.lo;users[0].hi=legacy.hi;strcpy(users[0].name,"Local");usersCount=1;marked=0;
  users_file_save();
 }
 usersActive=marked>=0?marked:0;
 local.lo=users[usersActive].lo;local.hi=users[usersActive].hi;
 lock_release();
 fprintf(stderr,"[totk] %d local user(s), \"%s\" in force\n",usersCount,users[usersActive].name);
}
static bool same(const Uid *uid){return uid&&uid->lo==local.lo&&uid->hi==local.hi;}
static bool valid(const UserHandle *handle){if(!handle||!same(&handle->uid))return false;for(unsigned i=0;i<64;i++)if(tokens[i]&&tokens[i]==handle->token)return true;return false;}
static unsigned preselected(UserHandle *out){if(!out)runtime_fail("account null user handle");initialize();lock_acquire();for(unsigned i=0;i<64;i++)if(!tokens[i]){tokens[i]=nextToken++;*out=(UserHandle){local,tokens[i]};lock_release();return 1;}lock_release();return 0;}
static uint32_t user_id(Uid *out,const UserHandle *handle){lock_acquire();bool ok=out&&valid(handle);if(ok)*out=local;lock_release();return ok?0:0x7c67;}
static void close_user(const UserHandle *handle){lock_acquire();if(!valid(handle))runtime_fail("invalid local account handle close");for(unsigned i=0;i<64;i++)if(tokens[i]==handle->token){tokens[i]=0;break;}lock_release();}
static uint32_t existence(unsigned char *out,const Uid *uid){if(!out||!uid)return 0x7c67;initialize();lock_acquire();*out=user_of(uid)!=NULL;lock_release();return 0;}
static uint32_t nickname(char *out,const Uid *uid){initialize();if(!out||!uid)return 0x7c67;lock_acquire();const RuntimeUser *u=user_of(uid);if(u){memset(out,0,32);memcpy(out,u->name,strlen(u->name));}lock_release();return u?0:0x7c67;}
void runtime_account_uid(uint64_t out[2]){initialize();out[0]=local.lo;out[1]=local.hi;}

/* ---------- the settings panel's User page (runtime/users.h) ---------- */
int runtime_users_get(RuntimeUser *out,int max){
 initialize();if(!out||max<0)return 0;
 lock_acquire();int n=0;for(int i=0;i<usersCount&&n<max;i++)out[n++]=users[i];lock_release();return n;
}
int runtime_users_active(RuntimeUser *out){
 initialize();if(!out)return -1;
 lock_acquire();*out=users[usersActive];lock_release();return 0;
}
int runtime_users_add(const char *name){
 initialize();if(!name)return -1;
 lock_acquire();
 RuntimeUser *u=&users[usersCount];
 if(usersCount>=RUNTIME_USERS_MAX||!clean_name(u->name,name)){lock_release();return -1;}
 Uid uid;do{arc4random_buf(&uid,sizeof uid);}while((!uid.lo&&!uid.hi)||user_of(&uid));
 u->lo=uid.lo;u->hi=uid.hi;usersCount++;
 users_file_save();
 lock_release();
 fprintf(stderr,"[totk] local user \"%s\" added (%d of %d)\n",u->name,usersCount,RUNTIME_USERS_MAX);
 return usersCount-1;
}
int runtime_users_choose(int index){
 initialize();
 lock_acquire();
 if(index<0||index>=usersCount){lock_release();return -1;}
 if(index!=usersActive){usersActive=index;users_file_save();}
 lock_release();
 return 0;
}

#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md) opens the preselected user with the aborting variant and lists the users.
   A launched game plays as the one preselected user, so the list answers with that one, however many users the Mac has. */
static void open_preselected(UserHandle *out){if(!preselected(out))runtime_fail("open preselected user");}
static uint32_t list_users(int *count,Uid *out,int max){initialize();if(!count||(max>0&&!out))return 0x7c67;*count=0;if(max>0){out[0]=local;*count=1;}return 0;}
/* SDK 20.5 (1.9.0) takes the users the system opened at launch instead: the one preselected user. */
static uint32_t pop_open_users(int *count,UserHandle *out,int max){if(!count||(max>0&&!out))return 0x7c67;*count=0;if(max>0){open_preselected(&out[0]);*count=1;}return 0;}
#endif
void *runtime_account_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn7account10InitializeEv",initialize},{"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",preselected},
 {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",close_user},{"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",user_id},
 {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",existence},{"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",nickname}
#ifdef GAME_BOTW
 ,{"_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE",open_preselected},{"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",list_users}
 ,{"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi",pop_open_users}
#endif
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
