/* IPv4 UDP subset of Horizon BSD IPC. Wire ABI: switchbrew/libnx bsd.c.
   No host fd, host sockaddr, or native option number crosses the guest ABI.
   TCP/IPv6/DNS remain unsupported; explicit transport mode selects virtual UDP. */
#include "lan_bsd.h"
#include "lan_mode.h"
#include "lan_udp.h"
#include "lan_virtual.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_BUFFER (1024u * 1024u)
/* Verified from D3 main+0xc0807c (ORR #0x800), passed unchanged by SDK
   Fcntl into BSD IPC. Do not substitute Darwin/FreeBSD O_NONBLOCK (4). */
#define GUEST_O_NONBLOCK 0x800
#define GUEST_O_RDWR 2
static int enabled(void) {
    return mng_lan_mode()!=MNG_LAN_OFF;
}
static int32_t arg(const IpcRequest *rq, unsigned offset) {
    int32_t value; memcpy(&value, rq->data + offset, sizeof value); return value;
}
static int args(const IpcRequest *rq, unsigned size) { return rq->data && rq->data_size >= size; }
/* AutoSelect preserves descriptor positions with zero-size placeholders. */
static IpcBuffer buffer(const IpcRequest *rq, int output, unsigned index) {
    const IpcBuffer *alias = output ? rq->b : rq->a, *pointer = output ? rq->c : rq->x;
    unsigned na = output ? rq->nb : rq->na, np = output ? rq->nc : rq->nx;
    if (index < na && alias[index].size) return alias[index];
    if (output) { if (index < np) return pointer[index]; }
    else for (unsigned i = 0; i < np; ++i) if (pointer[i].index == index) return pointer[i];
    return index < na ? alias[index] : (IpcBuffer){0};
}
static int valid(IpcBuffer b, size_t minimum) {
    return b.size >= minimum && b.size <= MAX_BUFFER && (!b.size || b.addr) && b.addr + b.size >= b.addr;
}
static void *map(IpcBuffer b) { return b.size ? ipc_guest(b.addr, b.size) : NULL; }
static void publish(const IpcRequest *rq, IpcResponse *rs, unsigned index, size_t size) {
    if (!(index < rq->nb && rq->b[index].size) && index < rq->nc && size)
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[index].addr, size, index};
}
static int read_address(IpcBuffer b, MngUdpAddress *out) {
    if (!valid(b, 16)) return 22;
    const unsigned char *p = map(b);
    if (p[1] != 2) return 47; /* sa_len:u8, sa_family:u8 (FreeBSD) */
    /* Input extent comes from the IPC descriptor (the SDK socklen argument),
       not sa_len. ITT main+0x3468440..45c writes family/port/address into a
       reused stack slot without initializing byte 0. Ignore that byte; never
       relax the 16-byte mapped-input bound or AF_INET check above. Output
       sockaddr values still have a canonical length byte of 16. */
    memcpy(&out->port, p + 2, 2); memcpy(&out->address, p + 4, 4); return 0;
}
static size_t write_address(IpcBuffer b, const MngUdpAddress *address) {
    unsigned char bytes[16] = {16, 2};
    memcpy(bytes + 2, &address->port, 2); memcpy(bytes + 4, &address->address, 4);
    size_t n = b.size < 16 ? (size_t)b.size : 16;
    if (n) memcpy(map(b), bytes, n);
    return n;
}
static void result(IpcResponse *rs, int32_t ret, int error) {
    int32_t err = ret < 0 ? error : 0; IPC_OUT(rs, ret); IPC_OUT(rs, err);
}
static void trace(unsigned cmd, int ret, int error) {
    static _Atomic unsigned lines;
    /* Empty nonblocking receives/polls must not consume all diagnostics. */
    if ((cmd == 8 || cmd == 9 || cmd == 25) && error == 35) return;
    if ((cmd == 5 || cmd == 6) && ret >= 0) return;
    if (atomic_fetch_add(&lines, 1) < 160)
        fprintf(stderr, "[mng-bsd] cmd=%u ret=%d errno=%d\n", cmd, ret, error);
}
int mng_lan_bsd_dispatch(const IpcRequest *rq, IpcResponse *rs) {
    if (!enabled()) return 0;
    if(mng_lan_mode()==MNG_LAN_TRANSPORT && mng_udp_virtual_prepare()) {
        result(rs,-1,50);return 1;
    }
    int ret = -1, error = 22;
    uint32_t extra = 0; int has_extra = 0;
    IpcBuffer in = buffer(rq, 0, 0), out = buffer(rq, 1, 0);
    switch (rq->cmd) {
    case 0: { uint64_t pid = 0; IPC_OUT(rs, pid); return 1; }
    case 1: return 1;
    case 2: case 3:
        if (!args(rq, 12)) break;
        {
            static _Atomic unsigned socket_lines;
            if(atomic_fetch_add(&socket_lines,1)<32)
                fprintf(stderr,"[mng-bsd] Socket family=%d type=%d protocol=%d\n",arg(rq,0),arg(rq,4),arg(rq,8));
        }
        if (arg(rq, 0) != 2) { error = 47; break; }
        if (arg(rq, 4) != 2 || (arg(rq, 8) != 0 && arg(rq, 8) != 17)) { error = 43; break; }
        ret = mng_udp_open(&error); break;
    case 8: case 9: case 25: {
        has_extra = rq->cmd == 9;
        if (!args(rq, rq->cmd == 25 ? 4 : 8) || !valid(out, 0)) break;
        IpcBuffer address = buffer(rq, 1, 1);
        if (has_extra && !valid(address, 0)) break;
        MngUdpAddress from = {0};
        ret = mng_udp_receive_flags(arg(rq, 0), map(out), out.size, has_extra ? &from : NULL,
                                   rq->cmd == 25 ? 0 : arg(rq, 4), &error);
        if (ret >= 0) {
            publish(rq, rs, 0, (size_t)ret);
            if (has_extra) { extra = 16; publish(rq, rs, 1, write_address(address, &from)); }
        }
        break;
    }
    case 10: case 11: case 24: {
        if (!args(rq, rq->cmd == 24 ? 4 : 8) || !valid(in, 0)) break;
        if (in.size > 65507) { error = 40; break; }
        MngUdpAddress address, *to = NULL;
        if (rq->cmd == 11) {
            IpcBuffer b = buffer(rq, 0, 1);
            if (b.size) { error = read_address(b, &address); if (error) break; to = &address; }
        }
        ret = mng_udp_send_flags(arg(rq, 0), map(in), in.size, to,
                                rq->cmd == 24 ? 0 : arg(rq, 4), &error); break;
    }
    case 13: case 14: {
        if (!args(rq, 4)) break;
        MngUdpAddress address; error = read_address(in, &address); if (error) break;
        ret = rq->cmd == 13 ? mng_udp_bind(arg(rq, 0), &address, &error)
                            : mng_udp_connect(arg(rq, 0), &address, &error); break;
    }
    case 15: case 16: {
        has_extra = 1;
        if (!args(rq, 4) || !valid(out, 0)) break;
        MngUdpAddress address;
        ret = rq->cmd == 15 ? mng_udp_peer(arg(rq, 0), &address, &error)
                            : mng_udp_name(arg(rq, 0), &address, &error);
        if (!ret) { extra = 16; publish(rq, rs, 0, write_address(out, &address)); }
        break;
    }
    case 17: case 21: {
        has_extra = rq->cmd == 17;
        IpcBuffer b = has_extra ? out : in;
        if (!args(rq, 12) || !valid(b, 4) || (!has_extra && b.size != 4)) break;
        int value = 0;
        if (!has_extra) memcpy(&value, map(b), 4);
        ret = mng_udp_option(arg(rq, 0), !has_extra, arg(rq, 4), arg(rq, 8), &value, &error);
        if (!ret && has_extra) { memcpy(map(out), &value, 4); extra = 4; publish(rq, rs, 0, 4); }
        break;
    }
    case 20:
        if (!args(rq, 12)) break;
        {
            static _Atomic unsigned fcntl_lines;
            if (atomic_fetch_add(&fcntl_lines, 1) < 32)
                fprintf(stderr, "[mng-bsd] Fcntl handle=%d command=%d flags=0x%x\n",
                        arg(rq, 0), arg(rq, 4), (unsigned)arg(rq, 8));
        }
        if (arg(rq, 4) == 3) {
            ret = mng_udp_get_nonblock(arg(rq, 0), &error);
            if (ret >= 0) ret = GUEST_O_RDWR | (ret ? GUEST_O_NONBLOCK : 0);
        } else if (arg(rq, 4) == 4 && !(arg(rq, 8) & ~(GUEST_O_RDWR | GUEST_O_NONBLOCK)))
            ret = mng_udp_nonblock(arg(rq, 0), arg(rq, 8) & GUEST_O_NONBLOCK, &error);
        else error = 45;
        break;
    case 22:
        if (args(rq, 8)) ret = mng_udp_shutdown(arg(rq, 0), arg(rq, 4), &error);
        break;
    case 26:
        if (args(rq, 4)) ret = mng_udp_close(arg(rq, 0), &error);
        break;
    case 5: {
        /* s32 nfds; padding; s64 seconds; s64 microseconds; bool is_null.
           SDK fd_set is a 1024-bit array, independent of host FD_SETSIZE. */
        if (!args(rq, 25)) break;
        int nfds = arg(rq, 0), timeout = -1;
        if (nfds < 0 || nfds > 1024) break;
        if (!rq->data[24]) {
            int64_t seconds, micros;
            memcpy(&seconds, rq->data + 8, 8); memcpy(&micros, rq->data + 16, 8);
            if (seconds < 0 || micros < 0 || micros >= 1000000 || seconds > (INT_MAX - 1000) / 1000) break;
            timeout = (int)(seconds * 1000 + (micros + 999) / 1000);
        }
        unsigned char input[3][128] = {{0}}, output[3][128] = {{0}};
        IpcBuffer outputs[3]; int good = 1;
        for (unsigned i = 0; i < 3; ++i) {
            IpcBuffer b = buffer(rq, 0, i); outputs[i] = buffer(rq, 1, i);
            if (!valid(b, b.size ? 128 : 0) || !valid(outputs[i], b.size ? 128 : 0) ||
                (!b.size && outputs[i].size)) { good = 0; break; }
            if (b.size) memcpy(input[i], map(b), 128);
        }
        if (!good) break;
        MngUdpPoll fds[128]; size_t count = 0;
        for (int fd = 0; fd < nfds; ++fd) {
            int events = 0;
            for (unsigned i = 0; i < 3; ++i) if (input[i][fd / 8] & (1u << (fd % 8)))
                events |= i == 0 ? 1 : i == 1 ? 4 : 2;
            if (events) {
                if (count == 128) { good = 0; break; }
                fds[count++] = (MngUdpPoll){fd, (int16_t)events, 0};
            }
        }
        if (!good) break;
        ret = mng_udp_poll(fds, count, timeout, &error);
        if (ret < 0) break;
        ret = 0;
        for (size_t n = 0; n < count; ++n) {
            if (fds[n].revents & 0x20) { ret = -1; error = 9; break; }
            int fd = fds[n].handle;
            for (unsigned i = 0; i < 3; ++i) {
                int ready = i == 0 ? fds[n].revents & (1 | 8 | 0x10)
                          : i == 1 ? fds[n].revents & (4 | 8) : fds[n].revents & 2;
                if (ready && (input[i][fd / 8] & (1u << (fd % 8)))) {
                    output[i][fd / 8] |= 1u << (fd % 8); ++ret;
                }
            }
        }
        if (ret >= 0) for (unsigned i = 0; i < 3; ++i) if (outputs[i].size) {
            memcpy(map(outputs[i]), output[i], 128); publish(rq, rs, i, 128);
        }
        break;
    }
    case 6: {
        if (!args(rq, 8)) break;
        int count = arg(rq, 0);
        if (count < 0 || count > 128 || !valid(in, (size_t)count * 8) || !valid(out, (size_t)count * 8)) break;
        MngUdpPoll fds[128]; _Static_assert(sizeof(MngUdpPoll) == 8, "poll ABI");
        if (count) memcpy(fds, map(in), (size_t)count * 8);
        ret = mng_udp_poll(fds, (size_t)count, arg(rq, 4), &error);
        if (ret >= 0 && count) { memcpy(map(out), fds, (size_t)count * 8); publish(rq, rs, 0, (size_t)count * 8); }
        break;
    }
    default: error = 45; break; /* unsupported, not a fatal runtime exit */
    }
    result(rs, ret, error);
    if (has_extra) IPC_OUT(rs, extra);
    trace(rq->cmd, ret, ret < 0 ? error : 0);
    return 1;
}
