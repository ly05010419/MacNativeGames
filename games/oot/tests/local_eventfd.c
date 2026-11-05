#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define LOCAL_EVENTFD_TEST 1
static _Thread_local int socket_errno;
#include "../runtime/local_eventfd.h"
static void *wake(void *p){uint64_t v=17;assert(local_write((int)(intptr_t)p,&v,8)==8);return NULL;}
int main(void){
 uint64_t v;int fd=local_create(0,6);assert(fd>=512);assert(local_read_counter(fd,&v)==-1&&socket_errno==35);
 LocalPollFd p={fd,1,0};assert(local_poll(&p,1,5)==0&&p.revents==0);
 pthread_t t;assert(!pthread_create(&t,NULL,wake,(void *)(intptr_t)fd));assert(local_poll(&p,1,1000)==1&&p.revents==1);pthread_join(t,NULL);
 assert(local_read_counter(fd,&v)==0&&v==17);assert(local_write_counter(fd,UINT64_MAX)==-1&&socket_errno==22);
 assert(local_write_counter(fd,UINT64_MAX-1)==0);assert(local_write_counter(fd,1)==-1&&socket_errno==35);assert(local_read_counter(fd,&v)==0&&v==UINT64_MAX-1);
 assert(local_fcntl(fd,1)==1);assert(local_fcntl(fd,2,0L)==0&&local_fcntl(fd,1)==0);assert(local_close(fd)==0);assert(local_read_counter(fd,&v)==-1&&socket_errno==9);
 fd=local_create(2,5);assert(local_read_counter(fd,&v)==0&&v==1);assert(local_read_counter(fd,&v)==0&&v==1);assert(local_read_counter(fd,&v)==-1);assert(local_write_counter(fd,4)==0);
 uint64_t rs[16]={0};rs[fd/64]|=1ull<<(fd%64);int64_t tv[2]={0};assert(local_select(fd+1,rs,NULL,NULL,tv)==1);assert(rs[fd/64]>>(fd%64)&1);assert(!local_close(fd));puts("eventfd counters, overflow, semaphore, readiness, timeout and thread wake passed");
}
