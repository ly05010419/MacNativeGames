/* Kernel-held save ownership. Lock files are permanent: never unlink/recreate them.
   Lock order: account mutex -> users.lock -> user-NN.lock (nonblocking).
   The registry lock must cover validation, reservation and delete/archive together.
   Closing an account handle does NOT release ownership: mounts/async writes may survive it. */
#ifndef MNG_USER_LOCK_H
#define MNG_USER_LOCK_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int mng_lock_file(const char *root, const char *name, int operation) {
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(dir<0)return -1;
 int fd=-1;
 /* Concurrent first creation can transiently report ENOENT on the host filesystem.
    Retry against the same pinned directory, never unlink or replace the lock inode. */
 for(unsigned attempt=0;attempt<4;attempt++){
  fd=openat(dir,name,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
  if(fd>=0||(errno!=EINTR&&errno!=ENOENT))break;
 }
 int saved=errno;close(dir);errno=saved;
 if(fd<0)return -1;
 struct stat st;
 if(fstat(fd,&st)){saved=errno;close(fd);errno=saved;return -1;}
 if(!S_ISREG(st.st_mode)||st.st_nlink!=1){close(fd);errno=EINVAL;return -1;}
 int result;do{result=flock(fd,operation);}while(result&&errno==EINTR);
 if(result){saved=errno;close(fd);errno=saved;return -1;}
 return fd;
}
static inline int mng_registry_lock(const char *root) {
 return mng_lock_file(root,"users.lock",LOCK_EX);
}
/* Caller holds users.lock. Failure is always fail-closed, including filesystem errors. */
static inline int mng_user_lock(const char *root,unsigned uid) {
 if(!uid||uid>32){errno=EINVAL;return -1;}
 char name[32];snprintf(name,sizeof name,"user-%02u.lock",uid);
 return mng_lock_file(root,name,LOCK_EX|LOCK_NB);
}
/* Epoch changes on deletion, so a running process's stale account list can never
   reserve a newly-created person who reused the same numeric ID. Registry lock held. */
static inline int mng_user_epoch_fd(int fd,uint64_t *epoch){
 unsigned char bytes[8];ssize_t n=pread(fd,bytes,sizeof bytes,0);
 if(n==0){*epoch=0;return 1;}if(n!=8)return 0;
 *epoch=0;for(unsigned i=0;i<8;i++)*epoch|=(uint64_t)bytes[i]<<(8*i);return 1;
}
static inline int mng_user_epoch(const char *root,unsigned uid,uint64_t *epoch){
 char name[32];snprintf(name,sizeof name,"user-%02u.lock",uid);
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 0;
 int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(dir);errno=saved;
 if(fd<0){if(errno==ENOENT){*epoch=0;return 1;}return 0;}
 struct stat st;int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_nlink==1&&mng_user_epoch_fd(fd,epoch);
 close(fd);return ok;
}
static inline int mng_user_next_epoch(int fd){
 uint64_t epoch;if(!mng_user_epoch_fd(fd,&epoch)||epoch==UINT64_MAX)return 0;
 epoch++;unsigned char bytes[8];for(unsigned i=0;i<8;i++)bytes[i]=(unsigned char)(epoch>>(8*i));
 return pwrite(fd,bytes,sizeof bytes,0)==8&&!fsync(fd);
}
/* Only the explicitly selected user can inherit a launcher reservation through exec.
   Verify inode and take/retain the lock; an environment variable alone grants nothing. */
static inline int mng_user_lock_adopt(const char *root,unsigned uid,const char *value) {
 if(!value||!uid||uid>32){errno=EINVAL;return -1;}
 char *end;errno=0;long number=strtol(value,&end,10);
 if(errno||!*value||*end||number<3||number>INT_MAX){errno=EINVAL;return -1;}
 int fd=(int)number;
 char name[32];snprintf(name,sizeof name,"user-%02u.lock",uid);
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(dir<0)return -1;
 struct stat a,b;
 int ok=!fstat(fd,&a)&&!fstatat(dir,name,&b,AT_SYMLINK_NOFOLLOW)&&
  S_ISREG(a.st_mode)&&a.st_nlink==1&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
 close(dir);
 if(!ok){errno=EINVAL;return -1;}
 if(flock(fd,LOCK_EX|LOCK_NB)||fcntl(fd,F_SETFD,FD_CLOEXEC))return -1;
 return fd;
}
#endif
