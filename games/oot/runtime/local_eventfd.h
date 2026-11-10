/* Local notification counters used by nn::socket even with networking disabled.
 * Horizon flag/errno ABI checked against nxnative ee9b7765 hle_net.c (MIT).
 * Counters, descriptor lifetime, readiness and timeouts are owned here. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#define LOCAL_FD_BASE 512
#define LOCAL_FD_COUNT 64
static struct { int used, flags; uint64_t value; } local_fds[LOCAL_FD_COUNT];
static pthread_mutex_t local_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t local_cv=PTHREAD_COND_INITIALIZER;
static int local_wait(const struct timespec *deadline) {
#ifndef LOCAL_EVENTFD_TEST
 int away=runtime_core_away();
#endif
 int result=deadline?pthread_cond_timedwait(&local_cv,&local_mu,deadline):pthread_cond_wait(&local_cv,&local_mu);
#ifndef LOCAL_EVENTFD_TEST
 /* Never wait for a guest core while holding the notification table mutex. */
 pthread_mutex_unlock(&local_mu);runtime_core_back(&away);pthread_mutex_lock(&local_mu);
#endif
 return result;
}
static int local_error(int e) {
 socket_errno=e;
#ifndef LOCAL_EVENTFD_TEST
 int *(*loc)(void)=runtime_symbol("__errno_location");
 if(loc)*loc()=e==35?11:e;
#endif
 return -1;
}
static int local_index(int fd) { int i=fd-LOCAL_FD_BASE;return i>=0&&i<LOCAL_FD_COUNT&&local_fds[i].used?i:-1; }
static int local_create(uint64_t value,int flags) {
 if(value==UINT64_MAX||(flags&~7))return local_error(22);
 pthread_mutex_lock(&local_mu);int i=0;while(i<LOCAL_FD_COUNT&&local_fds[i].used)i++;
 if(i<LOCAL_FD_COUNT){local_fds[i].used=1;local_fds[i].flags=flags;local_fds[i].value=value;}
 pthread_mutex_unlock(&local_mu);return i<LOCAL_FD_COUNT?LOCAL_FD_BASE+i:local_error(24);
}
static int local_read_counter(int fd,uint64_t *value) {
 if(!value)return local_error(14);
 pthread_mutex_lock(&local_mu);int i;
 while((i=local_index(fd))>=0&&!local_fds[i].value&&!(local_fds[i].flags&4))local_wait(NULL);
 int err=i<0?9:!local_fds[i].value?35:0;
 if(!err){*value=local_fds[i].flags&1?1:local_fds[i].value;local_fds[i].value-=*value;pthread_cond_broadcast(&local_cv);}
 pthread_mutex_unlock(&local_mu);return err?local_error(err):0;
}
static int local_write_counter(int fd,uint64_t value) {
 if(value==UINT64_MAX)return local_error(22);
 pthread_mutex_lock(&local_mu);int i;
 while((i=local_index(fd))>=0&&local_fds[i].value>UINT64_MAX-1-value&&!(local_fds[i].flags&4))local_wait(NULL);
 int err=i<0?9:local_fds[i].value>UINT64_MAX-1-value?35:0;
 if(!err){local_fds[i].value+=value;pthread_cond_broadcast(&local_cv);}
 pthread_mutex_unlock(&local_mu);return err?local_error(err):0;
}
static int64_t local_read(int fd,void *out,uint64_t size) { if(size<8)return local_error(22);return local_read_counter(fd,out)?-1:8; }
static int64_t local_write(int fd,const void *in,uint64_t size) { if(size<8||!in)return local_error(22);uint64_t v;memcpy(&v,in,8);return local_write_counter(fd,v)?-1:8; }
static int local_close(int fd) {
 pthread_mutex_lock(&local_mu);int i=local_index(fd);if(i>=0){local_fds[i].used=0;pthread_cond_broadcast(&local_cv);}pthread_mutex_unlock(&local_mu);return i<0?local_error(9):0;
}
static int local_fcntl(int fd,int command,...) {
 long arg=0;if(command==2||command==4){va_list ap;va_start(ap,command);arg=va_arg(ap,long);va_end(ap);}
 pthread_mutex_lock(&local_mu);int i=local_index(fd),result=0,error=0;
 if(i<0)error=9;
 else if(command==1)result=(local_fds[i].flags&2)?1:0;
 else if(command==2)local_fds[i].flags=(local_fds[i].flags&~2)|((arg&1)?2:0);
 else if(command==3)result=local_fds[i].flags&4;
 else if(command==4)local_fds[i].flags=(local_fds[i].flags&~4)|(arg&4);
 else error=22;
 pthread_cond_broadcast(&local_cv);pthread_mutex_unlock(&local_mu);return error?local_error(error):result;
}
typedef struct { int32_t fd;int16_t events,revents; } LocalPollFd;
static int local_poll(LocalPollFd *fds,uint64_t count,int timeout) {
 if(count>1024||(!fds&&count))return local_error(22);
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);
 if(timeout>=0){end.tv_sec+=timeout/1000;end.tv_nsec+=(timeout%1000)*1000000L;if(end.tv_nsec>=1000000000){end.tv_sec++;end.tv_nsec-=1000000000;}}
 pthread_mutex_lock(&local_mu);int result;
 for(;;){
  result=0;
  for(uint64_t j=0;j<count;j++){int i=local_index(fds[j].fd),ev=0;
   if(fds[j].fd>=0){if(i<0)ev=0x20;else {if(local_fds[i].value)ev|=fds[j].events&0x41;if(local_fds[i].value<UINT64_MAX-1)ev|=fds[j].events&4;}}
   fds[j].revents=(int16_t)ev;result+=ev!=0;
  }
  if(result||timeout==0)break;
  int r=local_wait(timeout<0?NULL:&end);
  if(r==ETIMEDOUT)break;
 }
 pthread_mutex_unlock(&local_mu);return result;
}
static int local_select(int nfds,uint64_t *r,uint64_t *w,uint64_t *e,const int64_t *tv) {
 if(nfds<0||nfds>1024||(tv&&(tv[0]<0||tv[1]<0||tv[1]>=1000000)))return local_error(22);
 LocalPollFd fds[1024];unsigned n=0;uint64_t *sets[3]={r,w,e};
 for(int fd=0;fd<nfds;fd++){int events=0;for(int k=0;k<3;k++)if(sets[k]&&(sets[k][fd/64]>>(fd%64)&1))events|=k==0?1:k==1?4:0x10;
 if(events)fds[n++]=(LocalPollFd){fd,(int16_t)events,0};}
 int ms=tv?(tv[0]>2147483?2147483647:(int)(tv[0]*1000+(tv[1]+999)/1000)):-1;
 int result=local_poll(fds,n,ms);if(result<0)return result;
 for(unsigned i=0;i<n;i++)if(fds[i].revents&0x20)return local_error(9);
 for(int k=0;k<3;k++)if(sets[k])memset(sets[k],0,128);
 result=0;for(unsigned i=0;i<n;i++)for(int k=0;k<3;k++)if(sets[k]&&(fds[i].revents&(k==0?1:k==1?4:0x10))){sets[k][fds[i].fd/64]|=1ull<<(fds[i].fd%64);result++;}
 return result;
}
