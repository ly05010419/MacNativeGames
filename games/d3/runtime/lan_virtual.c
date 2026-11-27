#include "lan_virtual.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS 128
#define QUEUE_PACKETS 32
#define QUEUE_BYTES (1024u * 1024u)
typedef struct Packet {
    struct Packet *next;
    MngUdpAddress from;
    size_t size;
    unsigned char data[];
} Packet;
typedef struct {
    int handle, nonblock, rd, wr;
    int reuseaddr, reuseport, broadcast, sndbuf, rcvbuf;
    MngUdpAddress local, peer;
    Packet *head, *tail;
    unsigned queued;
    size_t bytes;
} Socket;
/* Lock order: UDP registry (prepare only) -> this lock -> transport lock.
   No worker thread: receive/poll pumps a bounded batch nonblocking, then waits
   in 5ms slices. A single mutex serializes transport receives and queue edits. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Socket sockets[SLOTS];
static int next_handle = 1;
static unsigned next_port = 49152;
static MngLanTransport *transport;
static uint32_t local_ip;
static uint64_t epoch;
static size_t queued_bytes;
static int transport_error;
static int finish(int result, int error, int *out) {
    pthread_mutex_unlock(&lock);
    if (out) *out = result < 0 ? error : 0;
    return result;
}
static int host_error(int e) {
    switch (e) {
    case EAGAIN: return 35;
    case EMSGSIZE: return 40;
    case EHOSTUNREACH: return 65;
    case ENETUNREACH: return 51;
    case ENOBUFS: return 55;
    case ENOMEM: return 12;
    case EINVAL: return 22;
    default: return 5;
    }
}
static Socket *find(int h) {
    for (unsigned i=0;i<SLOTS;++i) if (h>0 && sockets[i].handle==h) return &sockets[i];
    return NULL;
}
static void pop(Socket *s) {
    Packet *p=s->head; if (!p) return;
    s->head=p->next;if (!s->head) s->tail=NULL;
    --s->queued;s->bytes-=p->size;queued_bytes-=sizeof *p+p->size;free(p);
}
static void flush(Socket *s) { while (s->head) pop(s); }
static void purge_revoked(Socket *s) {
    /* No address reuse within a transport lifetime. Serialize queue access
       here; transport membership lock never calls back into virtual sockets. */
    Packet **link=&s->head;
    s->tail=NULL;
    while (*link) {
        Packet *p=*link;
        if (!transport || !mng_transport_has_peer(transport,ntohl(p->from.address))) {
            *link=p->next;--s->queued;s->bytes-=p->size;
            queued_bytes-=sizeof *p+p->size;free(p);
        } else {s->tail=p;link=&p->next;}
    }
}
static int port_used(uint16_t port) {
    for (unsigned i=0;i<SLOTS;++i) if (sockets[i].handle && sockets[i].local.port==port) return 1;
    return 0;
}
static int autobind(Socket *s) {
    if (s->local.port) return 0;
    for (unsigned i=0;i<16384;++i) {
        unsigned p=next_port++;if (next_port>65535) next_port=49152;
        if (!port_used(htons((uint16_t)p))) {s->local.port=htons((uint16_t)p);return 0;}
    }
    return 48;
}
static int64_t now_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void pause_locked(void) {
    pthread_mutex_unlock(&lock);
    struct timespec t={0,5000000};nanosleep(&t,NULL);
    pthread_mutex_lock(&lock);
}
static void pump(void) {
    if (!transport || transport_error) return;
    MngLanDatagram d;
    /* Max eight accepted datagrams per pump; transport also bounds invalid
       frame work per receive. Drop newest on pressure, never grow unbounded. */
    for (unsigned n=0;n<8;++n) {
        int rc=mng_transport_receive(transport,&d,0);
        if (rc<0) {transport_error=host_error(errno);return;}
        if (!rc) return;
        MngUdpAddress source={htonl(d.source),htons(d.source_port)};
        for (unsigned i=0;i<SLOTS;++i) {
            Socket *s=&sockets[i];
            if (!s->handle || s->rd || s->local.port!=htons(d.destination_port) ||
                (s->local.address && s->local.address!=htonl(d.destination)) ||
                (s->peer.port && (s->peer.address!=source.address || s->peer.port!=source.port))) continue;
            if (s->queued>=QUEUE_PACKETS || s->bytes+d.size>(size_t)s->rcvbuf ||
                queued_bytes+sizeof(Packet)+d.size>QUEUE_BYTES) break;
            Packet *p=malloc(sizeof *p+d.size);if (!p) break;
            p->next=NULL;p->from=source;p->size=d.size;
            if (d.size) memcpy(p->data,d.data,d.size);
            if (s->tail) s->tail->next=p;else s->head=p;
            s->tail=p;++s->queued;s->bytes+=d.size;queued_bytes+=sizeof *p+d.size;
            break; /* Exclusive binding even when reuse options were requested. */
        }
    }
}
int mng_vudp_attach(MngLanTransport *t) {
    if (!t) {errno=EINVAL;return -1;}
    MngLanEndpoint ep;if (mng_transport_endpoint(t,&ep)) return -1;
    pthread_mutex_lock(&lock);
    int error=transport?EBUSY:0;
    /* Interface changes do not rebind or close existing sockets. An explicit
       old-address binding remains inert until that address returns or the game
       closes it; it must not veto an unrelated LDN connection. */
    if (!error) {transport=t;local_ip=htonl(ep.address);transport_error=0; ++epoch;}
    pthread_mutex_unlock(&lock);if (error) errno=error;return error?-1:0;
}
void mng_udp_virtual_detach(void) {
    pthread_mutex_lock(&lock);transport=NULL;local_ip=0;transport_error=0;++epoch;
    for (unsigned i=0;i<SLOTS;++i) flush(&sockets[i]);
    pthread_mutex_unlock(&lock);
}
int mng_vudp_open(int *error) {
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<SLOTS && next_handle<INT_MAX;++i) if (!sockets[i].handle) {
        Socket *s=&sockets[i];memset(s,0,sizeof *s);s->handle=next_handle++;
        s->sndbuf=s->rcvbuf=256*1024;return finish(s->handle,0,error);
    }
    return finish(-1,24,error);
}
int mng_vudp_close(int h,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    flush(s);memset(s,0,sizeof *s);return finish(0,0,error);
}
int mng_vudp_nonblock(int h,int enabled,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    s->nonblock=!!enabled;return finish(0,0,error);
}
int mng_vudp_get_nonblock(int h,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);return finish(s?s->nonblock:-1,9,error);
}
int mng_vudp_bind(int h,const MngUdpAddress *a,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (!a) return finish(-1,14,error);
    if (s->local.port) return finish(-1,22,error);
    if (a->address && (!transport || a->address!=local_ip)) return finish(-1,49,error);
    if (a->port && port_used(a->port)) return finish(-1,48,error);
    s->local=*a;int e=autobind(s);return finish(e?-1:0,e,error);
}
int mng_vudp_name(int h,MngUdpAddress *a,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (!a) return finish(-1,14,error);
    *a=s->local;
    if (s->peer.port && !a->address) a->address=local_ip;
    return finish(0,0,error);
}
int mng_vudp_connect(int h,const MngUdpAddress *a,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (!a) return finish(-1,14,error);
    if (!transport) return finish(-1,50,error);
    uint32_t ip=ntohl(a->address);
    if ((ip&0xffffff00u)!=0x0a4d0000u || !(ip&255) || (ip&255)==255 || !a->port) return finish(-1,51,error);
    int e=autobind(s);if (!e) {flush(s);s->peer=*a;}
    return finish(e?-1:0,e,error);
}
int mng_vudp_peer(int h,MngUdpAddress *a,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (!a) return finish(-1,14,error);
    if (!s->peer.port) return finish(-1,57,error);
    *a=s->peer;return finish(0,0,error);
}
int mng_vudp_shutdown(int h,int how,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (how<0 || how>2) return finish(-1,22,error);
    if (!s->peer.port) return finish(-1,57,error);
    if (how!=1) {s->rd=1;flush(s);}if (how!=0) s->wr=1;
    return finish(0,0,error);
}
int mng_vudp_option(int h,int set,int level,int option,int *value,int *error) {
    pthread_mutex_lock(&lock);Socket *s=find(h);if (!s) return finish(-1,9,error);
    if (!value) return finish(-1,14,error);
    if (level!=0xffff) return finish(-1,42,error);
    int *v=NULL,boolean=0;
    switch (option) {
    case 4:v=&s->reuseaddr;boolean=1;break;
    case 0x200:v=&s->reuseport;boolean=1;break;
    case 0x20:v=&s->broadcast;boolean=1;break;
    case 0x1001:v=&s->sndbuf;break;
    case 0x1002:v=&s->rcvbuf;break;
    case 0x1007:case 0x1008:
        if (set) return finish(-1,42,error);
        *value=option==0x1008?2:transport_error;return finish(0,0,error);
    default:return finish(-1,42,error);
    }
    if (set) {
        if (!boolean && (*value<1 || *value>(int)QUEUE_BYTES)) return finish(-1,22,error);
        *v=boolean?!!*value:*value;
    } else *value=*v;
    return finish(0,0,error);
}
int mng_vudp_send_flags(int h,const void *data,size_t size,const MngUdpAddress *to,int flags,int *error) {
    pthread_mutex_lock(&lock);uint64_t start=epoch;
    for (;;) {
        Socket *s=find(h);if (!s) return finish(-1,9,error);
        if (flags&~0x80) return finish(-1,45,error);
        if (!data && size) return finish(-1,14,error);
        if (size>MNG_DATA_MAX_PAYLOAD) return finish(-1,40,error);
        if (!transport || epoch!=start) return finish(-1,50,error);
        if (transport_error) return finish(-1,transport_error,error);
        if (s->local.address && s->local.address!=local_ip) return finish(-1,49,error);
        if (s->wr) return finish(-1,58,error); /* ESHUTDOWN */
        const MngUdpAddress *dest=to?to:&s->peer;
        if (!dest->port) return finish(-1,39,error);
        int e=autobind(s);if (e) return finish(-1,e,error);
        int rc=mng_transport_send(transport,ntohl(dest->address),ntohs(s->local.port),ntohs(dest->port),data,size);
        e=rc<0?host_error(errno):0;
        if (e!=35 || s->nonblock || (flags&0x80)) return finish(rc,e,error);
        pause_locked();
    }
}
int mng_vudp_receive_flags(int h,void *data,size_t size,MngUdpAddress *from,int flags,int *error) {
    pthread_mutex_lock(&lock);uint64_t start=epoch;
    for (;;) {
        Socket *s=find(h);if (!s) return finish(-1,9,error);
        if (flags&~(0x80|2)) return finish(-1,45,error);
        if (!data && size) return finish(-1,14,error);
        if (size>INT_MAX) return finish(-1,22,error);
        if (!transport || epoch!=start) return finish(-1,50,error);
        if (transport_error) return finish(-1,transport_error,error);
        if (s->rd) return finish(0,0,error);
        pump();purge_revoked(s);
        if (s->head) {
            Packet *p=s->head;size_t n=size<p->size?size:p->size;
            if (n) memcpy(data,p->data,n);if (from) *from=p->from;
            if (!(flags&2)) pop(s);
            return finish((int)n,0,error);
        }
        if (transport_error) return finish(-1,transport_error,error);
        if (s->nonblock || (flags&0x80)) return finish(-1,35,error);
        pause_locked();
    }
}
int mng_vudp_poll(MngUdpPoll *fds,size_t count,int timeout_ms,int *error) {
    pthread_mutex_lock(&lock);
    if (count>SLOTS || (!fds && count)) return finish(-1,22,error);
    MngUdpPoll out[SLOTS];
    for (size_t i=0;i<count;++i) {
        if (fds[i].events&~(1|2|4|0x40|0x80|0x100)) return finish(-1,22,error);
        out[i]=fds[i];out[i].revents=0;
    }
    int64_t end=now_ms()+(timeout_ms<0?0:timeout_ms);uint64_t start=epoch;
    for (;;) {
        pump();int ready=0;
        for (size_t i=0;i<count;++i) {
            out[i].revents=0;if (out[i].handle<0) continue;
            Socket *s=find(out[i].handle);
            if (!s) out[i].revents=0x20;
            else if (!transport || epoch!=start || transport_error) out[i].revents=8;
            else {
                purge_revoked(s);
                if (s->head || s->rd) out[i].revents|=out[i].events&(1|0x40);
                /* UDP send may still fail (unknown destination, size, host
                   backpressure); readiness never promises peer admission. */
                if (!s->wr) out[i].revents|=out[i].events&(4|0x100);
            }
            ready+=!!out[i].revents;
        }
        if (ready || (timeout_ms>=0 && now_ms()>=end)) {
            if (count) memcpy(fds,out,count*sizeof *fds);return finish(ready,0,error);
        }
        pause_locked();
    }
}
