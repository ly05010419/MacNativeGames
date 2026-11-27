#include "lan_transport.h"
#include <CommonCrypto/CommonHMAC.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HEADER 96u
#define FRAME_MAX (HEADER + MNG_DATA_MAX_PAYLOAD)
static const uint8_t magic[8] = {'M','N','G','D','A','T','A',1};
typedef struct {
    int used, active;
    MngLanEndpoint endpoint;
    uint8_t key[32];
    uint64_t sent, highest, received;
} Peer;
struct MngLanTransport {
    pthread_mutex_t lock;
    int fd;
    uint64_t title;
    uint8_t room[16];
    MngLanEndpoint local;
    Peer peers[MNG_DATA_MAX_PEERS];
};
static void erase(void *data, size_t size) {
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}
static int nonzero(const uint8_t *p, size_t n) {
    uint8_t value = 0; for (size_t i = 0; i < n; ++i) value |= p[i]; return value != 0;
}
static int virtual_address(uint32_t address) {
    return (address & 0xffffff00u) == 0x0a4d0000u && (address & 255) && (address & 255) != 255;
}
static int native_address(uint32_t address_be) {
    uint32_t a = ntohl(address_be);
    /* Link-local 169.254/16 is usable on an explicitly selected LAN interface.
       Reject unspecified, multicast and reserved class-E addresses. */
    return (a >> 24) > 0 && (a >> 24) < 224;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = v << 8 | p[i]; return v; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; } }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | p[i]; return v; }
static int fail(int error) { errno = error; return -1; }
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static Peer *find_peer(MngLanTransport *t, uint32_t address) {
    for (unsigned i = 0; i < MNG_DATA_MAX_PEERS; ++i)
        if (t->peers[i].active && t->peers[i].endpoint.address == address) return &t->peers[i];
    return NULL;
}
static void authenticate(const uint8_t key[32], const uint8_t *frame, size_t size, uint8_t tag[32]) {
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA256, key, 32);
    CCHmacUpdate(&ctx, frame, 64);
    CCHmacUpdate(&ctx, frame + HEADER, size - HEADER);
    CCHmacFinal(&ctx, tag); erase(&ctx, sizeof ctx);
}
static int same_tag(const uint8_t a[32], const uint8_t b[32]) {
    volatile uint8_t difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
MngLanTransport *mng_transport_open(uint32_t interface_be, uint64_t title,
                                  const uint8_t room[16], uint32_t address) {
    if (!native_address(interface_be) || !title || !room || !nonzero(room,16) || !virtual_address(address)) {
        errno = EINVAL; return NULL;
    }
    MngLanTransport *t = calloc(1, sizeof *t); if (!t) return NULL;
    t->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->fd < 0) { int error = errno; free(t); errno = error; return NULL; }
    struct sockaddr_in local = {0}; local.sin_len = sizeof local; local.sin_family = AF_INET;
    local.sin_addr.s_addr = interface_be;
    if (fcntl(t->fd,F_SETFD,FD_CLOEXEC) || fcntl(t->fd,F_SETFL,O_NONBLOCK) ||
        bind(t->fd,(struct sockaddr *)&local,sizeof local)) goto error;
    socklen_t size = sizeof local;
    if (getsockname(t->fd,(struct sockaddr *)&local,&size)) goto error;
    int e = pthread_mutex_init(&t->lock,NULL);
    if (e) { errno = e; goto error; }
    t->title = title; memcpy(t->room,room,16);
    t->local.address = address; t->local.native = local;
    do { arc4random_buf(&t->local.cookie,sizeof t->local.cookie); } while (!t->local.cookie);
    return t;
error:;
    int error = errno; close(t->fd); erase(t,sizeof *t); free(t); errno = error; return NULL;
}
int mng_transport_context(MngLanTransport *t, uint64_t title, const uint8_t room[16]) {
    if (!t || !room || title != t->title || memcmp(room, t->room, 16)) return fail(EINVAL);
    return 0;
}
int mng_transport_assign(MngLanTransport *t, uint32_t address) {
    if (!t || !virtual_address(address)) return fail(EINVAL);
    pthread_mutex_lock(&t->lock);
    for (unsigned i = 0; i < MNG_DATA_MAX_PEERS; ++i) if (t->peers[i].used) {
        pthread_mutex_unlock(&t->lock); return fail(EBUSY);
    }
    t->local.address = address;
    pthread_mutex_unlock(&t->lock); return 0;
}
int mng_transport_endpoint(MngLanTransport *t, MngLanEndpoint *out) {
    if (!t || !out) return fail(EINVAL);
    *out = t->local; return 0; /* immutable after open */
}
int mng_transport_add_peer(MngLanTransport *t, const MngLanEndpoint *peer, const uint8_t key[32]) {
    if (!t || !peer || !key || !nonzero(key,32) || !virtual_address(peer->address) ||
        peer->address == t->local.address || !peer->cookie || peer->cookie == t->local.cookie ||
        peer->native.sin_family != AF_INET || !native_address(peer->native.sin_addr.s_addr) ||
        !peer->native.sin_port || (peer->native.sin_addr.s_addr == t->local.native.sin_addr.s_addr &&
                                  peer->native.sin_port == t->local.native.sin_port)) return fail(EINVAL);
    pthread_mutex_lock(&t->lock);
    int error = 0; Peer *slot = NULL;
    for (unsigned i = 0; i < MNG_DATA_MAX_PEERS; ++i) {
        Peer *p = &t->peers[i];
        if (!p->used) { if (!slot) slot = p; continue; }
        if (p->endpoint.address == peer->address || p->endpoint.cookie == peer->cookie ||
            !memcmp(p->key,key,32) || (p->endpoint.native.sin_addr.s_addr == peer->native.sin_addr.s_addr &&
                                     p->endpoint.native.sin_port == peer->native.sin_port)) { error = EEXIST; break; }
    }
    if (!error && !slot) error = ENOSPC;
    if (!error) { slot->endpoint = *peer; memcpy(slot->key,key,32); slot->used = slot->active = 1; }
    pthread_mutex_unlock(&t->lock);
    return error ? fail(error) : 0;
}
int mng_transport_has_peer(MngLanTransport *t, uint32_t address) {
    if (!t) return 0;
    pthread_mutex_lock(&t->lock); int active = find_peer(t,address) != NULL;
    pthread_mutex_unlock(&t->lock); return active;
}
int mng_transport_remove_peer(MngLanTransport *t, uint32_t address) {
    if (!t) return fail(EINVAL);
    pthread_mutex_lock(&t->lock); Peer *peer = find_peer(t,address);
    if (peer) peer->active = 0;
    /* Keep retired key/identity until close to reject reuse and late packets.
       Reconnection within this prototype requires a new transport lifetime. */
    pthread_mutex_unlock(&t->lock); return peer ? 0 : fail(ENOENT);
}
int mng_transport_send(MngLanTransport *t, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, size_t size) {
    if (!t || !source_port || !destination_port || (!data && size)) return fail(EINVAL);
    if (size > MNG_DATA_MAX_PAYLOAD) return fail(EMSGSIZE);
    uint8_t *frame = calloc(1,HEADER + size); if (!frame) return -1;
    pthread_mutex_lock(&t->lock);
    Peer *peer = find_peer(t,destination); int result = -1, error = 0;
    if (!peer) error = EHOSTUNREACH;
    else if (peer->sent == UINT64_MAX) error = EOVERFLOW;
    else {
        memcpy(frame,magic,8); put64(frame+8,t->title); memcpy(frame+16,t->room,16);
        put32(frame+32,t->local.address); put32(frame+36,destination);
        put16(frame+40,source_port); put16(frame+42,destination_port); put16(frame+44,(uint16_t)size);
        put64(frame+48,++peer->sent); put64(frame+56,peer->endpoint.cookie);
        if (size) memcpy(frame+HEADER,data,size);
        authenticate(peer->key,frame,HEADER+size,frame+64);
        ssize_t sent = sendto(t->fd,frame,HEADER+size,0,(struct sockaddr *)&peer->endpoint.native,sizeof peer->endpoint.native);
        if (sent == (ssize_t)(HEADER+size)) result = (int)size;
        else error = sent < 0 ? errno : EIO;
    }
    pthread_mutex_unlock(&t->lock); erase(frame,HEADER+size); free(frame);
    return error ? fail(error) : result;
}
/* lock held. Replay window accepts out-of-order datagrams within 64 sequence
   numbers; invalid MACs must never advance it or evict valid pending traffic. */
static int decode(MngLanTransport *t, const uint8_t *frame, size_t size,
                  const struct sockaddr_in *source, MngLanDatagram *out) {
    if (size < HEADER || size > FRAME_MAX || memcmp(frame,magic,8) || get64(frame+8) != t->title ||
        memcmp(frame+16,t->room,16) || get32(frame+36) != t->local.address ||
        get64(frame+56) != t->local.cookie || get16(frame+44) != size-HEADER ||
        get16(frame+46) || !get16(frame+40) || !get16(frame+42)) return 0;
    Peer *peer = find_peer(t,get32(frame+32));
    if (!peer || source->sin_family != AF_INET || source->sin_addr.s_addr != peer->endpoint.native.sin_addr.s_addr ||
        source->sin_port != peer->endpoint.native.sin_port) return 0;
    uint8_t tag[32]; authenticate(peer->key,frame,size,tag);
    int authenticated = same_tag(tag,frame+64); erase(tag,sizeof tag); if (!authenticated) return 0;
    uint64_t seq = get64(frame+48); if (!seq) return 0;
    if (seq > peer->highest) {
        uint64_t delta = seq-peer->highest;
        peer->received = delta >= 64 ? 1 : (peer->received << delta) | 1;
        peer->highest = seq;
    } else {
        uint64_t delta = peer->highest-seq;
        if (delta >= 64 || (peer->received & (UINT64_C(1)<<delta))) return 0;
        peer->received |= UINT64_C(1)<<delta;
    }
    /* No output mutation until all validation completes. */
    out->source = peer->endpoint.address; out->destination = t->local.address;
    out->source_port = get16(frame+40); out->destination_port = get16(frame+42);
    out->size = size-HEADER;
    if (out->size) memcpy(out->data,frame+HEADER,out->size);
    return 1;
}
int mng_transport_receive(MngLanTransport *t, MngLanDatagram *out, int timeout_ms) {
    if (!t || !out || timeout_ms < 0 || timeout_ms > 2000) return fail(EINVAL);
    uint8_t *frame = malloc(FRAME_MAX); if (!frame) return -1;
    uint64_t deadline = now_ms()+(unsigned)timeout_ms; int result = 0, error = 0;
    for (unsigned i = 0; i < 256; ++i) {
        uint64_t now = now_ms();
        if (i && now >= deadline && timeout_ms) break;
        int wait = now < deadline ? (int)(deadline-now) : 0;
        struct pollfd p = {t->fd,POLLIN,0};
        int ready = poll(&p,1,wait);
        if (ready < 0) { if (errno == EINTR) continue; error = errno; break; }
        if (!ready) break;
        struct sockaddr_in source = {0};
        struct iovec iov = {frame,FRAME_MAX};
        struct msghdr msg = {0};msg.msg_name = &source;msg.msg_namelen = sizeof source;
        msg.msg_iov = &iov;msg.msg_iovlen = 1;
        ssize_t size = recvmsg(t->fd,&msg,0);
        if (size < 0) { if (errno == EINTR || errno == EAGAIN) continue; error = errno; break; }
        if (msg.msg_flags & MSG_TRUNC) continue;
        pthread_mutex_lock(&t->lock);
        result = decode(t,frame,(size_t)size,&source,out);
        pthread_mutex_unlock(&t->lock);
        if (result) break;
    }
    erase(frame,FRAME_MAX); free(frame); return error ? fail(error) : result;
}
void mng_transport_close(MngLanTransport *t) {
    if (!t) return;
    close(t->fd); pthread_mutex_destroy(&t->lock); erase(t,sizeof *t); free(t);
}
