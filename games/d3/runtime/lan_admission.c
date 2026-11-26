#include "lan_admission.h"
#include <CommonCrypto/CommonHMAC.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME 1368u
#define SIGNED 1336u
#define NODE_OFFSET 120u
#define NETWORK_OFFSET 184u
#define HELLO 1
#define CHALLENGE 2
#define CONFIRM 3
#define ACCEPT 4
#define HEARTBEAT 5
#define ALIVE 6
#define LEAVE 7
#define PENDING_MS 6000u
#define LEASE_MS 3000u
#define HEARTBEAT_MS 500u
static const uint8_t magic[8]={'M','N','G','J','O','I','N',3};
typedef struct {
    int used, committed, retired;
    uint64_t expires, sequence;
    uint8_t lease_key[32];
    struct sockaddr_in source;
    uint8_t transcript[FRAME];
} Admission;
struct MngLanAdmission {
    int fd;
    pthread_t worker;
    pthread_mutex_t lock;
    _Atomic int stop;
    MngLanTransport *transport;
    MngLanEndpoint local;
    uint64_t title;
    uint8_t room[16], credential[64];
    size_t credential_size;
    unsigned capacity;
    Admission members[7];
    int failed, has_network;
    MngLdnNetwork network;
};
struct MngLanMember {
    int fd;
    pthread_t worker;
    _Atomic int stop, active;
    pthread_mutex_t lock;
    int has_network;
    MngLdnNetwork network;
    MngLanTransport *transport;
    struct sockaddr_in destination;
    uint8_t transcript[FRAME], lease_key[32];
    uint64_t sent, acknowledged, deadline;
};
static void erase(void *p,size_t n) {volatile uint8_t *v=p;while(n--) *v++=0;}
static int nonzero(const uint8_t *p,size_t n) {uint8_t v=0;while(n--) v|=*p++;return v!=0;}
static void put(uint8_t *p,uint64_t v,unsigned n) {while(n) {p[--n]=(uint8_t)v;v>>=8;}}
static uint64_t get(const uint8_t *p,unsigned n) {uint64_t v=0;while(n--) v=v<<8|*p++;return v;}
static uint64_t now_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int equal(const uint8_t *a,const uint8_t *b,size_t n) {
    volatile uint8_t d=0;while(n--) d|=*a++^*b++;return !d;
}
static void sign(uint8_t f[FRAME],const void *key,size_t n) {
    CCHmac(kCCHmacAlgSHA256,key,n,f,SIGNED,f+SIGNED);
}
static int verified(const uint8_t f[FRAME],const void *key,size_t n) {
    uint8_t tag[32];CCHmac(kCCHmacAlgSHA256,key,n,f,SIGNED,tag);
    int ok=equal(tag,f+SIGNED,32);erase(tag,sizeof tag);return ok;
}
static void derive_key(const uint8_t transcript[FRAME],const void *key,size_t n,const char *domain,uint8_t out[32]) {
    /* Domain separate control proofs from the data-plane pairwise secret. */
    CCHmacContext ctx;CCHmacInit(&ctx,kCCHmacAlgSHA256,key,n);
    CCHmacUpdate(&ctx,domain,strlen(domain));
    uint8_t copy[NETWORK_OFFSET];memcpy(copy,transcript,sizeof copy);copy[100]=0;memset(copy+112,0,8);
    CCHmacUpdate(&ctx,copy,sizeof copy);CCHmacFinal(&ctx,out);
    erase(copy,sizeof copy);erase(&ctx,sizeof ctx);
}
static void data_key(const uint8_t f[FRAME],const void *key,size_t n,uint8_t out[32]) {
    derive_key(f,key,n,"MNG LDN pairwise data v3",out);
}
static void lease_key(const uint8_t f[FRAME],const void *key,size_t n,uint8_t out[32]) {
    derive_key(f,key,n,"MNG LDN membership lease v3",out);
}
static int endpoint_equal(const struct sockaddr_in *a,const struct sockaddr_in *b) {
    return a->sin_family==AF_INET && b->sin_family==AF_INET &&
           a->sin_addr.s_addr==b->sin_addr.s_addr && a->sin_port==b->sin_port;
}
static int valid_context(const uint8_t f[FRAME],uint64_t title,const uint8_t room[16]) {
    return !memcmp(f,magic,8) && get(f+8,8)==title && !memcmp(f+16,room,16) &&
           !nonzero(f+101,3) && !nonzero(f+108,4);
}
static int socket_open(uint32_t ip,struct sockaddr_in *address) {
    uint32_t host=ntohl(ip);if (!(host>>24) || (host>>24)>=224) {errno=EINVAL;return -1;}
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0) return -1;
    *address=(struct sockaddr_in){0};address->sin_len=sizeof *address;
    address->sin_family=AF_INET;address->sin_addr.s_addr=ip;
    socklen_t n=sizeof *address;
    if(fcntl(fd,F_SETFD,FD_CLOEXEC) || fcntl(fd,F_SETFL,O_NONBLOCK) ||
       bind(fd,(struct sockaddr *)address,sizeof *address) ||
       getsockname(fd,(struct sockaddr *)address,&n)) {
        int e=errno;close(fd);errno=e;return -1;
    }
    return fd;
}
static int receive_frame(int fd,uint8_t frame[FRAME],struct sockaddr_in *source) {
    struct iovec iov={frame,FRAME};struct msghdr msg={0};
    memset(source,0,sizeof *source);msg.msg_name=source;msg.msg_namelen=sizeof *source;
    msg.msg_iov=&iov;msg.msg_iovlen=1;
    ssize_t n=recvmsg(fd,&msg,0);
    if(n<0) return -1;
    return n==FRAME && !(msg.msg_flags&MSG_TRUNC) && source->sin_family==AF_INET?1:0;
}
static int same_transcript(const uint8_t a[FRAME],const uint8_t b[FRAME]) {
    /* Message kind and MAC differ between challenge/confirm/accept. */
    return !memcmp(a,b,100) && !memcmp(a+101,b+101,11) && !memcmp(a+NODE_OFFSET,b+NODE_OFFSET,sizeof(MngLdnNode));
}
static void retire(MngLanAdmission *h,Admission *p) {
    if(p->committed) (void)mng_transport_remove_peer(h->transport,(uint32_t)get(p->transcript+96,4));
    p->committed=0;p->retired=1;erase(p->lease_key,sizeof p->lease_key);
}
static void expire(MngLanAdmission *h) {
    uint64_t now=now_ms();
    for(unsigned i=0;i<h->capacity;++i) {
        Admission *p=&h->members[i];
        if(!p->used || p->retired || now<p->expires) continue;
        if(p->committed) retire(h,p);else erase(p,sizeof *p);
    }
}
static void snapshot_network(MngLanAdmission *h,MngLdnNetwork *out) {
    *out=h->network;out->node_count=1;
    memset(out->nodes+1,0,sizeof out->nodes-sizeof out->nodes[0]);
    for(unsigned i=0;i<h->capacity;++i) if(h->members[i].committed) {
        MngLdnNode *node=&out->nodes[i+1];
        memcpy(node,h->members[i].transcript+NODE_OFFSET,sizeof *node);
        node->id=(int8_t)(i+1);node->connected=1;node->ipv4=0x0a4d0002+i;
        /* Locally administered virtual MAC, unique by room and node ID. */
        memcpy(node->mac,h->room,6);node->mac[0]=(node->mac[0]&0xfc)|2;node->mac[5]^=(uint8_t)(i+1);
        ++out->node_count;
    }
}
static int network_valid(const uint8_t f[FRAME]) {
    MngLdnNetwork n;memcpy(&n,f+NETWORK_OFFSET,sizeof n);
    unsigned id=(unsigned)(get(f+96,4)-0x0a4d0001);
    if(id<1 || id>7 || memcmp(n.network_id.session,f+16,16) || n.max_nodes<2 || n.max_nodes>8 ||
       id>=(unsigned)n.max_nodes || n.node_count<2 || n.node_count>(unsigned)n.max_nodes ||
       n.advertise_size>384 || n.security<1 || n.security>2 || n.accept_policy>3 ||
       n.common.ssid.length<1 || n.common.ssid.length>32 || n.common.ssid.value[n.common.ssid.length] ||
       !n.nodes[0].connected || !n.nodes[id].connected) return 0;
    unsigned count=0;
    for(unsigned i=0;i<8;++i) {
        const MngLdnNode *node=&n.nodes[i];
        if(node->connected>1) return 0;
        if(!node->connected) continue;
        if(i>=(unsigned)n.max_nodes || node->id!=(int)i || node->ipv4!=0x0a4d0001+i ||
           !memchr(node->name,0,sizeof node->name) || node->version<0) return 0;
        ++count;
    }
    MngLdnNode requested;memcpy(&requested,f+NODE_OFFSET,sizeof requested);
    return count==n.node_count && n.nodes[id].version==requested.version &&
           !memcmp(n.nodes[id].name,requested.name,sizeof requested.name);
}
static void publish_network(MngLanAdmission *h,uint8_t f[FRAME]) {
    if(h->has_network) {MngLdnNetwork n;snapshot_network(h,&n);memcpy(f+NETWORK_OFFSET,&n,sizeof n);}
}
static void host_frame(MngLanAdmission *h,uint8_t f[FRAME],const struct sockaddr_in *source) {
    expire(h);
    if(!valid_context(f,h->title,h->room) ||
       get(f+84,4)!=ntohl(source->sin_addr.s_addr) || get(f+104,2)!=ntohs(source->sin_port) ||
       get(f+106,2)!=ntohs(h->local.native.sin_port)) return;
    uint64_t now=now_ms();Admission *match=NULL,*free_slot=NULL;
    for(unsigned i=0;i<h->capacity;++i) {
        Admission *p=&h->members[i];
        if(!p->used) {if(!free_slot) free_slot=p;continue;}
        if(!memcmp(f+32,p->transcript+32,16)) match=p;
    }
    if(f[100]==HEARTBEAT || f[100]==LEAVE) {
        if(!match || !match->committed || !endpoint_equal(source,&match->source) ||
           !same_transcript(f,match->transcript) || !verified(f,match->lease_key,32)) return;
        uint64_t seq=get(f+112,8);
        if(!seq || seq<=match->sequence) return;
        match->sequence=seq;
        if(f[100]==LEAVE) {retire(h,match);return;}
        match->expires=now+LEASE_MS;f[100]=ALIVE;publish_network(h,f);sign(f,match->lease_key,32);
        (void)sendto(h->fd,f,FRAME,0,(const struct sockaddr *)source,sizeof *source);return;
    }
    if(get(f+112,8) || !verified(f,h->credential,h->credential_size) || (match && match->retired)) return;
    if(f[100]==HELLO) {
        if(nonzero(f+NETWORK_OFFSET,sizeof(MngLdnNetwork))) return;
        if(h->has_network) {
            MngLdnNode node;memcpy(&node,f+NODE_OFFSET,sizeof node);
            if(!memchr(node.name,0,sizeof node.name) || node.version!=h->network.nodes[0].version ||
               node.ipv4 || node.id || node.connected || nonzero(node.mac,sizeof node.mac) ||
               node.platform || nonzero(node.reserved,sizeof node.reserved)) return;
        } else if(nonzero(f+NODE_OFFSET,sizeof(MngLdnNode))) return;
        if(!nonzero(f+32,16) || nonzero(f+48,16) || !get(f+64,2) || get(f+66,2) ||
           !get(f+68,8) || get(f+76,8) || nonzero(f+88,12)) return;
        if(match) {
            if(!endpoint_equal(source,&match->source) || memcmp(f+64,match->transcript+64,2) ||
               memcmp(f+68,match->transcript+68,8)) return;
        } else {
            if(!free_slot) return;
            for(unsigned i=0;i<h->capacity;++i) {
                Admission *p=&h->members[i];if(!p->used) continue;
                if(endpoint_equal(source,&p->source) ||
                   (get(f+64,2)==get(p->transcript+64,2) && get(f+84,4)==get(p->transcript+84,4)) ||
                   get(f+68,8)==get(p->transcript+68,8)) return;
            }
            match=free_slot;match->used=1;match->expires=now+PENDING_MS;match->source=*source;
            memcpy(match->transcript,f,FRAME);uint8_t *t=match->transcript;
            arc4random_buf(t+48,16);t[48]|=1;
            MngLanEndpoint data;if(mng_transport_endpoint(h->transport,&data)) {erase(match,sizeof *match);return;}
            put(t+66,ntohs(data.native.sin_port),2);put(t+76,data.cookie,8);
            put(t+88,ntohl(data.native.sin_addr.s_addr),4);put(t+92,0x0a4d0001,4);
            put(t+96,0x0a4d0002+(unsigned)(match-h->members),4);
        }
        memcpy(f,match->transcript,FRAME);f[100]=CHALLENGE;
    } else if(f[100]==CONFIRM) {
        if(!match || !endpoint_equal(source,&match->source) || !same_transcript(f,match->transcript)) return;
        if(!match->committed) {
            MngLanEndpoint peer={0};peer.address=(uint32_t)get(f+96,4);peer.cookie=get(f+68,8);
            peer.native.sin_family=AF_INET;peer.native.sin_len=sizeof peer.native;
            peer.native.sin_addr=source->sin_addr;peer.native.sin_port=htons((uint16_t)get(f+64,2));
            uint8_t key[32];data_key(f,h->credential,h->credential_size,key);
            int rc=mng_transport_add_peer(h->transport,&peer,key);erase(key,sizeof key);
            if(rc) return;
            lease_key(f,h->credential,h->credential_size,match->lease_key);
            match->committed=1;match->expires=now+LEASE_MS;
        }
        /* Retrying confirmation must not renew the membership lease. */
        f[100]=ACCEPT;
    } else return;
    publish_network(h,f);sign(f,h->credential,h->credential_size);
    (void)sendto(h->fd,f,FRAME,0,(const struct sockaddr *)source,sizeof *source);
}
static void *worker(void *arg) {
    MngLanAdmission *h=arg;
    while(!atomic_load(&h->stop)) {
        pthread_mutex_lock(&h->lock);expire(h);pthread_mutex_unlock(&h->lock);
        struct pollfd p={h->fd,POLLIN,0};int n=poll(&p,1,25);
        if(n<0 && errno==EINTR) continue;
        if(n<0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) {
            pthread_mutex_lock(&h->lock);h->failed=1;pthread_mutex_unlock(&h->lock);break;
        }
        if(!n) continue;
        uint8_t frame[FRAME];struct sockaddr_in source;
        int rc=receive_frame(h->fd,frame,&source);
        if(rc==1) {pthread_mutex_lock(&h->lock);host_frame(h,frame,&source);pthread_mutex_unlock(&h->lock);}
        erase(frame,sizeof frame);
        /* Bound authenticated/invalid packet processing under a flood. */
        struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
    pthread_mutex_lock(&h->lock);
    for(unsigned i=0;i<h->capacity;++i) if(h->members[i].committed) retire(h,&h->members[i]);
    pthread_mutex_unlock(&h->lock);
    return NULL;
}
static int config_valid(uint64_t title,const uint8_t *room,const void *key,size_t size) {
    return title && room && nonzero(room,16) && key && size>=16 && size<=64;
}
static MngLanAdmission *host_start(MngLanTransport *t,uint64_t title,const uint8_t room[16],
                                   const void *key,size_t size,unsigned max_nodes,const MngLdnNetwork *network) {
    if(!t || !config_valid(title,room,key,size) || max_nodes<1 || max_nodes>8) {errno=EINVAL;return NULL;}
    if(network && (memcmp(network->network_id.session,room,16) || network->node_count!=1 ||
        !network->nodes[0].connected || network->nodes[0].id || network->nodes[0].ipv4!=0x0a4d0001 ||
        !memchr(network->nodes[0].name,0,sizeof network->nodes[0].name) || network->nodes[0].version<0 ||
        network->advertise_size>384)) {errno=EINVAL;return NULL;}
    if(mng_transport_context(t,title,room)) return NULL;
    MngLanEndpoint ep;if(mng_transport_endpoint(t,&ep)) return NULL;
    if(ep.address!=0x0a4d0001) {errno=EINVAL;return NULL;}
    MngLanAdmission *h=calloc(1,sizeof *h);if(!h) return NULL;
    h->fd=socket_open(ep.native.sin_addr.s_addr,&h->local.native);
    if(h->fd<0) {free(h);return NULL;}
    h->transport=t;h->title=title;h->capacity=max_nodes-1;
    if(network) {h->network=*network;h->has_network=1;}
    memcpy(h->room,room,16);memcpy(h->credential,key,size);h->credential_size=size;
    int e=pthread_mutex_init(&h->lock,NULL);
    if(e) goto error;
    e=pthread_create(&h->worker,NULL,worker,h);
    if(e) {pthread_mutex_destroy(&h->lock);goto error;}
    return h;
error:
    close(h->fd);erase(h,sizeof *h);free(h);errno=e;return NULL;
}
MngLanAdmission *mng_admission_host(MngLanTransport *t,uint64_t title,const uint8_t room[16],
                                   const void *key,size_t size,unsigned max_nodes) {
    return host_start(t,title,room,key,size,max_nodes,NULL);
}
MngLanAdmission *mng_admission_host_network(MngLanTransport *t,uint64_t title,const uint8_t room[16],
                                   const void *key,size_t size,const MngLdnNetwork *network) {
    if(!network) {errno=EINVAL;return NULL;}
    return host_start(t,title,room,key,size,(unsigned)network->max_nodes,network);
}
int mng_admission_network(MngLanAdmission *h,MngLdnNetwork *out) {
    if(!h || !out) {errno=EINVAL;return -1;}
    pthread_mutex_lock(&h->lock);expire(h);int ok=h->has_network && !h->failed;
    if(ok) snapshot_network(h,out);
    pthread_mutex_unlock(&h->lock);if(!ok) errno=ENETDOWN;return ok?0:-1;
}
int mng_admission_advertise(MngLanAdmission *h,const void *data,size_t size) {
    if(!h || size>384 || (!data && size)) {errno=EINVAL;return -1;}
    pthread_mutex_lock(&h->lock);int ok=h->has_network && !h->failed;
    if(ok) {memset(h->network.advertise,0,384);if(size) memcpy(h->network.advertise,data,size);h->network.advertise_size=(uint16_t)size;}
    pthread_mutex_unlock(&h->lock);if(!ok) errno=ENETDOWN;return ok?0:-1;
}
int mng_admission_endpoint(MngLanAdmission *h,struct sockaddr_in *out) {
    if(!h || !out) {errno=EINVAL;return -1;}*out=h->local.native;return 0;
}
int mng_admission_count(MngLanAdmission *h) {
    if(!h) {errno=EINVAL;return -1;}
    pthread_mutex_lock(&h->lock);expire(h);int n=1;
    for(unsigned i=0;i<h->capacity;++i) n+=h->members[i].committed;
    if(h->failed) n=-1;
    pthread_mutex_unlock(&h->lock);if(n<0) errno=EIO;return n;
}
void mng_admission_stop(MngLanAdmission *h) {
    if(!h) return;
    atomic_store(&h->stop,1);pthread_join(h->worker,NULL);close(h->fd);
    pthread_mutex_destroy(&h->lock);erase(h,sizeof *h);free(h);
}
static void member_send(MngLanMember *m,unsigned kind) {
    if(m->sent==UINT64_MAX) return;
    uint8_t f[FRAME];memcpy(f,m->transcript,FRAME);f[100]=(uint8_t)kind;
    put(f+112,++m->sent,8);sign(f,m->lease_key,32);
    (void)sendto(m->fd,f,FRAME,0,(const struct sockaddr *)&m->destination,sizeof m->destination);
    erase(f,sizeof f);
}
static void *member_worker(void *arg) {
    MngLanMember *m=arg;uint64_t next_send=0;
    while(!atomic_load(&m->stop) && now_ms()<m->deadline) {
        uint64_t now=now_ms();
        if(now>=next_send) {member_send(m,HEARTBEAT);next_send=now+HEARTBEAT_MS;}
        struct pollfd p={m->fd,POLLIN,0};int n=poll(&p,1,25);
        if(n<0 && errno==EINTR) continue;
        if(n<0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) break;
        if(!n) continue;
        uint8_t f[FRAME];struct sockaddr_in source;int rc=receive_frame(m->fd,f,&source);
        if(now_ms()>=m->deadline) {erase(f,sizeof f);break;}
        if(rc==1 && endpoint_equal(&source,&m->destination) &&
           same_transcript(f,m->transcript) && f[100]==ALIVE && verified(f,m->lease_key,32) &&
           (!m->has_network || network_valid(f))) {
            uint64_t seq=get(f+112,8);
            /* Only acknowledgements to our recent sends renew the lease;
               old delayed replies must not keep a dead host alive. */
            if(seq>m->acknowledged && seq<=m->sent && m->sent-seq<=1) {
                m->acknowledged=seq;m->deadline=now_ms()+LEASE_MS;
                if(m->has_network) {
                    pthread_mutex_lock(&m->lock);memcpy(&m->network,f+NETWORK_OFFSET,sizeof m->network);pthread_mutex_unlock(&m->lock);
                }
            }
        }
        erase(f,sizeof f);struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
    if(atomic_load(&m->stop)) member_send(m,LEAVE);
    (void)mng_transport_remove_peer(m->transport,0x0a4d0001);
    atomic_store(&m->active,0);return NULL;
}
MngLanTransport *mng_member_transport(MngLanMember *m) {return m?m->transport:NULL;}
int mng_member_network(MngLanMember *m,MngLdnNetwork *out) {
    if(!m || !out) {errno=EINVAL;return -1;}
    pthread_mutex_lock(&m->lock);int ok=m->has_network && atomic_load(&m->active);
    if(ok) *out=m->network;
    pthread_mutex_unlock(&m->lock);if(!ok) errno=ENETDOWN;return ok?0:-1;
}
int mng_member_active(MngLanMember *m) {return m && atomic_load(&m->active);}
void mng_member_leave(MngLanMember *m) {
    if(!m) return;
    atomic_store(&m->stop,1);pthread_join(m->worker,NULL);close(m->fd);
    mng_transport_close(m->transport);pthread_mutex_destroy(&m->lock);erase(m,sizeof *m);free(m);
}
static MngLanMember *join_start(uint32_t interface_be,const struct sockaddr_in *destination,
                                  uint64_t title,const uint8_t room[16],const void *key,size_t size,
                                  int timeout_ms,const MngLdnNode *node,uint32_t *assigned) {
    if(!destination || destination->sin_family!=AF_INET || !destination->sin_port ||
       !config_valid(title,room,key,size) || timeout_ms<100 || timeout_ms>5000 || !assigned) {
        errno=EINVAL;return NULL;
    }
    if(node && (!memchr(node->name,0,sizeof node->name) || node->version<0 || node->ipv4 || node->id ||
        node->connected || nonzero(node->mac,sizeof node->mac) || node->platform || nonzero(node->reserved,sizeof node->reserved))) {errno=EINVAL;return NULL;}
    struct sockaddr_in control;int fd=socket_open(interface_be,&control);if(fd<0) return NULL;
    /* Placeholder is never published/attached. Host assigns before peer install. */
    MngLanTransport *t=mng_transport_open(interface_be,title,room,0x0a4d00fe);
    if(!t) {int e=errno;close(fd);errno=e;return NULL;}
    MngLanEndpoint local;mng_transport_endpoint(t,&local);
    uint8_t request[FRAME]={0},response[FRAME]={0};
    if(node) memcpy(request+NODE_OFFSET,node,sizeof *node);
    memcpy(request,magic,8);put(request+8,title,8);memcpy(request+16,room,16);
    arc4random_buf(request+32,16);request[32]|=1;
    put(request+64,ntohs(local.native.sin_port),2);put(request+68,local.cookie,8);
    put(request+84,ntohl(interface_be),4);put(request+104,ntohs(control.sin_port),2);
    put(request+106,ntohs(destination->sin_port),2);request[100]=HELLO;sign(request,key,size);
    uint64_t deadline=now_ms()+(unsigned)timeout_ms,next_send=0;int error=ETIMEDOUT;
    MngLanMember *member=NULL;
    unsigned inspected=0;
    while(now_ms()<deadline && inspected<4096) {
        uint64_t now=now_ms();
        if(now>=next_send) {
            ssize_t sent=sendto(fd,request,FRAME,0,(const struct sockaddr *)destination,sizeof *destination);
            if(sent<0 && errno!=EAGAIN && errno!=EINTR) {error=errno;break;}
            next_send=now+100;
        }
        struct pollfd p={fd,POLLIN,0};int rc=poll(&p,1,20);
        if(rc<0) {if(errno==EINTR) continue;error=errno;break;}
        if(!rc) continue;
        struct sockaddr_in source;rc=receive_frame(fd,response,&source);++inspected;
        /* Apply pacing to invalid traffic too, not only accepted packets. */
        struct timespec delay={0,1000000};nanosleep(&delay,NULL);
        if(rc!=1 || !endpoint_equal(&source,destination) || !valid_context(response,title,room) ||
           get(response+112,8) || !verified(response,key,size) || memcmp(response+32,request+32,16) ||
           memcmp(response+64,request+64,2) || memcmp(response+68,request+68,8) ||
           memcmp(response+NODE_OFFSET,request+NODE_OFFSET,sizeof(MngLdnNode)) ||
           memcmp(response+84,request+84,4) || memcmp(response+104,request+104,4)) continue;
        if(request[100]==HELLO && response[100]==CHALLENGE) {
            uint32_t address=(uint32_t)get(response+96,4);
            if(!nonzero(response+48,16) || !get(response+66,2) || !get(response+76,8) ||
               get(response+88,4)!=ntohl(destination->sin_addr.s_addr) ||
               get(response+92,4)!=0x0a4d0001 || address<0x0a4d0002 || address>0x0a4d0008) continue;
            if(mng_transport_assign(t,address)) {error=errno;break;}
            memcpy(request,response,FRAME);request[100]=CONFIRM;sign(request,key,size);next_send=0;
        } else if(request[100]==CONFIRM && response[100]==ACCEPT && same_transcript(request,response)) {
            if(node && !network_valid(response)) continue;
            MngLanEndpoint peer={0};peer.address=0x0a4d0001;peer.cookie=get(response+76,8);
            peer.native=*destination;peer.native.sin_port=htons((uint16_t)get(response+66,2));
            uint8_t data[32];data_key(response,key,size,data);
            int result=mng_transport_add_peer(t,&peer,data);erase(data,sizeof data);
            if(result) {error=errno;break;}
            member=calloc(1,sizeof *member);
            if(!member) {error=ENOMEM;break;}
            int mutex_error=pthread_mutex_init(&member->lock,NULL);
            if(mutex_error) {free(member);member=NULL;error=mutex_error;break;}
            if(node) {member->has_network=1;memcpy(&member->network,response+NETWORK_OFFSET,sizeof member->network);}
            member->fd=fd;member->transport=t;member->destination=*destination;
            memcpy(member->transcript,response,FRAME);lease_key(response,key,size,member->lease_key);
            member->deadline=now_ms()+LEASE_MS;atomic_store(&member->active,1);
            int thread_error=pthread_create(&member->worker,NULL,member_worker,member);
            if(thread_error) {pthread_mutex_destroy(&member->lock);erase(member,sizeof *member);free(member);member=NULL;error=thread_error;break;}
            *assigned=(uint32_t)get(response+96,4);error=0;break;
        }

    }
    erase(request,sizeof request);erase(response,sizeof response);
    if(error) {close(fd);mng_transport_close(t);errno=error;return NULL;}return member;
}

MngLanMember *mng_admission_join(uint32_t ip,const struct sockaddr_in *dest,uint64_t title,
 const uint8_t room[16],const void *key,size_t size,int timeout,uint32_t *assigned) {
 return join_start(ip,dest,title,room,key,size,timeout,NULL,assigned);
}
MngLanMember *mng_admission_join_network(uint32_t ip,const struct sockaddr_in *dest,uint64_t title,
 const uint8_t room[16],const void *key,size_t size,int timeout,const MngLdnNode *node,uint32_t *assigned) {
 if(!node) {errno=EINVAL;return NULL;}
 return join_start(ip,dest,title,room,key,size,timeout,node,assigned);
}
