#include "lan_udp.h"
#include "lan_virtual.h"
#include <stdatomic.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

/* Bounded process-local namespace, independent of per-session BSD IPC clones.
   Every operation duplicates the descriptor under the registry lock, preventing
   close/reuse from redirecting an in-flight operation to an unrelated socket.
   Native sockets remain nonblocking. Guest blocking receives wait in bounded
   polls so closing their handle cancels them even without incoming traffic.
   Handles are monotonic and never reused (including after close), which also
   makes stale callers fail rather than operate on a new endpoint. */
#define UDP_SLOTS 128
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static struct { int handle, fd, nonblock; } sockets[UDP_SLOTS];
static int next_handle = 1;
static _Atomic int virtual_mode;
int mng_udp_virtual_prepare(void) {
    pthread_mutex_lock(&registry);
    /* Never switch an existing or formerly used native handle namespace. */
    int ok = next_handle == 1;
    if (ok) atomic_store(&virtual_mode, 1);
    pthread_mutex_unlock(&registry);
    if (!ok) errno = EBUSY;
    return ok ? 0 : -1;
}
int mng_udp_virtual_attach(MngLanTransport *transport) {
    if (!atomic_load(&virtual_mode)) { errno = EINVAL; return -1; }
    return mng_vudp_attach(transport);
}
static int translate_error(int value) {
    /* Horizon BSD follows FreeBSD numbers, not Darwin's extended errno range. */
    switch (value) {
    case 0: return 0;
    case EPERM: return 1;
    case EINTR: return 4;
    case EIO: return 5;
    case EBADF: return 9;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case EAGAIN: return 35;
    case EINPROGRESS: return 36;
    case EALREADY: return 37;
    case ENOTSOCK: return 38;
    case EDESTADDRREQ: return 39;
    case EMSGSIZE: return 40;
    case EPROTOTYPE: return 41;
    case ENOPROTOOPT: return 42;
    case EPROTONOSUPPORT: return 43;
    case EOPNOTSUPP: return 45;
    case EAFNOSUPPORT: return 47;
    case EADDRINUSE: return 48;
    case EADDRNOTAVAIL: return 49;
    case ENETDOWN: return 50;
    case ENETUNREACH: return 51;
    case ENETRESET: return 52;
    case ECONNABORTED: return 53;
    case ECONNRESET: return 54;
    case ENOBUFS: return 55;
    case EISCONN: return 56;
    case ENOTCONN: return 57;
    case ETIMEDOUT: return 60;
    case ECONNREFUSED: return 61;
    case EHOSTDOWN: return 64;
    case EHOSTUNREACH: return 65;
    default: return 5; /* unknown host error: EIO, never leak a Darwin number */
    }
}
static int fail(int *error, int value) { if (error) *error = value; return -1; }
static int slot_for(int handle) {
    if (handle > 0) for (int i = 0; i < UDP_SLOTS; ++i)
        if (sockets[i].handle == handle) return i;
    return -1;
}
static int acquire(int handle, int *nonblock, int *error) {
    pthread_mutex_lock(&registry);
    int i = slot_for(handle), fd = -1, saved = EBADF;
    if (i >= 0) {
        fd = fcntl(sockets[i].fd, F_DUPFD_CLOEXEC, 0); saved = errno;
        if (nonblock) *nonblock = sockets[i].nonblock;
    }
    pthread_mutex_unlock(&registry);
    if (error) *error = fd < 0 ? translate_error(saved) : 0;
    return fd;
}
static int live(int handle) {
    pthread_mutex_lock(&registry); int yes = slot_for(handle) >= 0;
    pthread_mutex_unlock(&registry); return yes;
}
static int finish(int fd, int result, int *error) {
    int saved = errno; close(fd);
    if (error) *error = result < 0 ? translate_error(saved) : 0;
    return result;
}
static struct sockaddr_in native_address(const MngUdpAddress *address) {
    struct sockaddr_in out = {0};
#ifdef __APPLE__
    out.sin_len = sizeof out;
#endif
    out.sin_family = AF_INET; out.sin_addr.s_addr = address->address;
    out.sin_port = address->port; return out;
}
static void guest_address(MngUdpAddress *out, const struct sockaddr_in *in) {
    if (out) { out->address = in->sin_addr.s_addr; out->port = in->sin_port; }
}
int mng_udp_open(int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_open(error);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return fail(error, translate_error(errno));
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK))
        return finish(fd, -1, error);
    pthread_mutex_lock(&registry);
    if (atomic_load(&virtual_mode)) {
        pthread_mutex_unlock(&registry); close(fd); return mng_vudp_open(error);
    }
    int handle = -1;
    for (int i = 0; i < UDP_SLOTS && next_handle < INT_MAX; ++i) if (!sockets[i].handle) {
        handle = next_handle++;
        sockets[i].handle = handle; sockets[i].fd = fd; sockets[i].nonblock = 0;
        break;
    }
    pthread_mutex_unlock(&registry);
    if (handle < 0) { close(fd); return fail(error, 24); }
    if (error) *error = 0;
    return handle;
}
int mng_udp_close(int handle, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_close(handle, error);
    pthread_mutex_lock(&registry);
    int i = slot_for(handle), fd = -1;
    if (i >= 0) { fd = sockets[i].fd; memset(&sockets[i], 0, sizeof sockets[i]); }
    pthread_mutex_unlock(&registry);
    if (fd < 0) return fail(error, 9);
    /* Do not retry close on EINTR: the OS may already have released the fd. */
    int result = close(fd);
    if (error) *error = result < 0 ? translate_error(errno) : 0;
    return result;
}
int mng_udp_nonblock(int handle, int enabled, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_nonblock(handle, enabled, error);
    pthread_mutex_lock(&registry);
    int i = slot_for(handle);
    if (i >= 0) sockets[i].nonblock = !!enabled;
    pthread_mutex_unlock(&registry);
    if (i < 0) return fail(error, 9);
    if (error) *error = 0;
    return 0;
}
int mng_udp_bind(int handle, const MngUdpAddress *address, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_bind(handle, address, error);
    if (!address) return fail(error, 14);
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    struct sockaddr_in a = native_address(address);
    return finish(fd, bind(fd, (struct sockaddr *)&a, sizeof a), error);
}
int mng_udp_name(int handle, MngUdpAddress *address, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_name(handle, address, error);
    if (!address) return fail(error, 14);
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    struct sockaddr_in a = {0}; socklen_t size = sizeof a;
    int result = getsockname(fd, (struct sockaddr *)&a, &size);
    if (!result) guest_address(address, &a);
    return finish(fd, result, error);
}
static int wait_ready(int fd, int handle, short events) {
    while (live(handle)) {
        struct pollfd p = {fd, events, 0};
        int result = poll(&p, 1, 50);
        if (result < 0) return -1;
        if (result && live(handle)) return 0;
    }
    errno = EBADF; return -1;
}
int mng_udp_send_flags(int handle, const void *data, size_t size, const MngUdpAddress *to, int flags, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_send_flags(handle, data, size, to, flags, error);
    if (flags & ~0x80) return fail(error, 45);
    if (size > 65507) return fail(error, 40);
    if (!data && size) return fail(error, 14);
    int nonblock, fd = acquire(handle, &nonblock, error); if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    if (to) a = native_address(to);
    nonblock |= !!(flags & 0x80);
    int result;
    for (;;) {
        if (!live(handle)) { errno = EBADF; result = -1; break; }
        result = (int)sendto(fd, data, size, 0, to ? (struct sockaddr *)&a : NULL, to ? sizeof a : 0);
        if (result >= 0 || errno != EAGAIN || nonblock) break;
        if (wait_ready(fd, handle, POLLOUT)) { result = -1; break; }
    }
    return finish(fd, result, error);
}
int mng_udp_receive_flags(int handle, void *data, size_t size, MngUdpAddress *from, int flags, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_receive_flags(handle, data, size, from, flags, error);
    if (flags & ~(0x80 | 2)) return fail(error, 45);
    if (size > INT_MAX) return fail(error, 22);
    if (!data && size) return fail(error, 14);
    int nonblock, fd = acquire(handle, &nonblock, error); if (fd < 0) return -1;
    struct sockaddr_in a = {0}; int result;
    nonblock |= !!(flags & 0x80);
    for (;;) {
        if (!live(handle)) { errno = EBADF; result = -1; break; }
        socklen_t length = sizeof a;
        result = (int)recvfrom(fd, data, size, flags & 2 ? MSG_PEEK : 0, (struct sockaddr *)&a, &length);
        if (result >= 0 || errno != EAGAIN || nonblock) break;
        if (wait_ready(fd, handle, POLLIN)) { result = -1; break; }
    }
    if (result >= 0) guest_address(from, &a);
    return finish(fd, result, error);
}

int mng_udp_send(int handle, const void *data, size_t size, const MngUdpAddress *to, int *error) {
    if (!to) return fail(error, 14);
    return mng_udp_send_flags(handle, data, size, to, 0, error);
}
int mng_udp_receive(int handle, void *data, size_t size, MngUdpAddress *from, int *error) {
    return mng_udp_receive_flags(handle, data, size, from, 0, error);
}
int mng_udp_get_nonblock(int handle, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_get_nonblock(handle, error);
    int nonblock, fd = acquire(handle, &nonblock, error);
    return fd < 0 ? -1 : finish(fd, nonblock, error);
}
int mng_udp_connect(int handle, const MngUdpAddress *address, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_connect(handle, address, error);
    if (!address) return fail(error, 14);
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    struct sockaddr_in a = native_address(address);
    return finish(fd, connect(fd, (struct sockaddr *)&a, sizeof a), error);
}
int mng_udp_peer(int handle, MngUdpAddress *address, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_peer(handle, address, error);
    if (!address) return fail(error, 14);
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    struct sockaddr_in a = {0}; socklen_t size = sizeof a;
    int result = getpeername(fd, (struct sockaddr *)&a, &size);
    if (!result) guest_address(address, &a);
    return finish(fd, result, error);
}
int mng_udp_shutdown(int handle, int how, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_shutdown(handle, how, error);
    if (how < 0 || how > 2) return fail(error, 22);
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    int mode = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
    return finish(fd, shutdown(fd, mode), error);
}
int mng_udp_option(int handle, int set, int level, int option, int *value, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_option(handle, set, level, option, value, error);
    if (!value) return fail(error, 14);
    if (level != 0xffff) return fail(error, 42);
    int native;
    switch (option) {
    case 4: native = SO_REUSEADDR; break;
    case 0x20: native = SO_BROADCAST; break;
    case 0x200: native = SO_REUSEPORT; break;
    case 0x1001: native = SO_SNDBUF; break;
    case 0x1002: native = SO_RCVBUF; break;
    case 0x1007: if (set) return fail(error, 42); native = SO_ERROR; break;
    case 0x1008: if (set) return fail(error, 42); native = SO_TYPE; break;
    default: return fail(error, 42);
    }
    int fd = acquire(handle, NULL, error); if (fd < 0) return -1;
    int result, v = *value; socklen_t size = sizeof v;
    if (set) result = setsockopt(fd, SOL_SOCKET, native, &v, sizeof v);
    else {
        result = getsockopt(fd, SOL_SOCKET, native, &v, &size);
        if (!result) *value = option == 0x1007 ? translate_error(v) : option == 0x1008 ? 2 :
                            (option == 4 || option == 0x20 || option == 0x200) ? !!v : v;
    }
    return finish(fd, result, error);
}
static int64_t milliseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int mng_udp_poll(MngUdpPoll *fds, size_t count, int timeout_ms, int *error) {
    if (atomic_load(&virtual_mode)) return mng_vudp_poll(fds, count, timeout_ms, error);
    if (count > UDP_SLOTS || (!fds && count)) return fail(error, 22);
    struct pollfd native[UDP_SLOTS];
    MngUdpPoll output[UDP_SLOTS];
    for (size_t i = 0; i < count; ++i) {
        /* FreeBSD readiness bits supported by this UDP-only layer. */
        if (fds[i].events & ~(1 | 2 | 4 | 0x40 | 0x80 | 0x100)) return fail(error, 22);
    }
    for (size_t i = 0; i < count; ++i) {
        output[i] = fds[i]; output[i].revents = 0;
        native[i] = (struct pollfd){-1, 0, 0};
        if (fds[i].handle < 0) continue;
        int acquire_error;
        native[i].fd = acquire(fds[i].handle, NULL, &acquire_error);
        if (native[i].fd < 0) {
            if (acquire_error != 9) {
                for (size_t j = 0; j < i; ++j) if (native[j].fd >= 0) close(native[j].fd);
                return fail(error, acquire_error);
            }
            output[i].revents = 0x20; /* POLLNVAL */
        }
        if (fds[i].events & (1 | 0x40)) native[i].events |= POLLIN;
        if (fds[i].events & 2) native[i].events |= POLLPRI;
        if (fds[i].events & (4 | 0x100)) native[i].events |= POLLOUT;
        /* RDBAND has no UDP equivalent: wait, do not report invented data. */
    }
    int result = 0, saved = 0;
    int64_t deadline = milliseconds() + (timeout_ms >= 0 ? timeout_ms : 0);
    for (;;) {
        int invalid = 0;
        for (size_t i = 0; i < count; ++i) {
            if (fds[i].handle >= 0 && !live(fds[i].handle)) output[i].revents = 0x20;
            invalid |= !!output[i].revents;
        }
        int64_t remaining = deadline - milliseconds();
        int wait = invalid || timeout_ms == 0 ? 0 : timeout_ms < 0 ? 50 : remaining <= 0 ? 0 : remaining < 50 ? (int)remaining : 50;
        int n = poll(native, (nfds_t)count, wait);
        if (n < 0) { saved = translate_error(errno); result = -1; break; }
        for (size_t i = 0; i < count; ++i) {
            if (fds[i].handle >= 0 && !live(fds[i].handle)) output[i].revents = 0x20;
            if (output[i].revents == 0x20) { ++result; continue; }
            short re = native[i].revents, out = 0;
            if (re & POLLIN) out |= fds[i].events & (1 | 0x40);
            if (re & POLLOUT) out |= fds[i].events & (4 | 0x100);
            if (re & POLLPRI) out |= fds[i].events & 2;
            if (re & POLLERR) out |= 8;
            if (re & POLLHUP) out |= 0x10;
            if (re & POLLNVAL) out |= 0x20;
            output[i].revents = out;
            if (out) ++result;
        }
        if (result || (timeout_ms >= 0 && milliseconds() >= deadline)) break;
    }
    for (size_t i = 0; i < count; ++i) if (native[i].fd >= 0) close(native[i].fd);
    if (result >= 0 && count) memcpy(fds, output, count * sizeof *fds);
    if (error) *error = saved;
    return result;
}
