#include "lan_host.h"
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct MngLanHost {
    pthread_mutex_t lock;
    pthread_t worker;
    int fd, stopping, error;
    uint16_t port;
    uint64_t title;
    MngLanRoom room;
};
static void *serve(void *context) {
    MngLanHost *h = context;
    for (;;) {
        pthread_mutex_lock(&h->lock);
        int stopping = h->stopping;
        pthread_mutex_unlock(&h->lock);
        if (stopping) break;
        struct pollfd socket = {h->fd, POLLIN, 0};
        int result = poll(&socket, 1, 100);
        if (result > 0) {
            /* Snapshot only after readability, not before the blocking wait.
               Serialize the nonblocking reply with publication so an update
               that has returned cannot be followed by an old advertisement. */
            pthread_mutex_lock(&h->lock);
            result = h->stopping ? 0 : mng_lan_serve_once(h->fd, h->title, &h->room, 0);
            pthread_mutex_unlock(&h->lock);
        }
        if (result < 0 && errno != EINTR && errno != EAGAIN) {
            int error = errno;
            pthread_mutex_lock(&h->lock);
            h->error = error ? error : EIO;
            pthread_mutex_unlock(&h->lock);
            break;
        }
        /* Bound work/replies even if the socket is continuously readable. */
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    return NULL;
}
MngLanHost *mng_lan_host_start(uint32_t interface_be, uint16_t port, int multicast,
                             uint64_t title, const MngLanRoom *room) {
    uint8_t probe[MNG_LAN_PACKET_MAX], nonce[16] = {1};
    if (!mng_lan_advert_encode(probe, title, nonce, room)) { errno = EINVAL; return NULL; }
    MngLanHost *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fd = mng_lan_listener_open(interface_be, port, multicast);
    if (h->fd < 0) { int error = errno; free(h); errno = error; return NULL; }
    struct sockaddr_in address;
    socklen_t size = sizeof address;
    if (getsockname(h->fd, (struct sockaddr *)&address, &size)) {
        int error = errno; close(h->fd); free(h); errno = error; return NULL;
    }
    h->port = ntohs(address.sin_port); h->title = title; h->room = *room;
    int error = pthread_mutex_init(&h->lock, NULL);
    if (error) { close(h->fd); free(h); errno = error; return NULL; }
    error = pthread_create(&h->worker, NULL, serve, h);
    if (error) {
        pthread_mutex_destroy(&h->lock); close(h->fd); free(h); errno = error; return NULL;
    }
    return h;
}
uint16_t mng_lan_host_port(const MngLanHost *h) { return h ? h->port : 0; }
int mng_lan_host_update(MngLanHost *h, const MngLdnNetwork *network) {
    if (!h || !mng_ldn_network_valid(network)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&h->lock);
    int error = h->error;
    if (!error && memcmp(&network->network_id, &h->room.network.network_id, sizeof network->network_id)) error = EINVAL;
    if (!error && h->room.revision == UINT64_MAX) error = EOVERFLOW;
    if (!error) { h->room.network = *network; ++h->room.revision; }
    pthread_mutex_unlock(&h->lock);
    if (error) { errno = error; return -1; }
    return 0;
}
int mng_lan_host_snapshot(MngLanHost *h, MngLanRoom *out) {
    if (!h || !out) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&h->lock);
    int error = h->error;
    if (!error) *out = h->room;
    pthread_mutex_unlock(&h->lock);
    if (error) { errno = error; return -1; }
    return 0;
}
void mng_lan_host_stop(MngLanHost *h) {
    if (!h) return;
    pthread_mutex_lock(&h->lock); h->stopping = 1; pthread_mutex_unlock(&h->lock);
    pthread_join(h->worker, NULL);
    close(h->fd); pthread_mutex_destroy(&h->lock); free(h);
}
