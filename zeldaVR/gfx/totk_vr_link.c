// TOTK VR: BasaltVR runtime control socket client. See totk_vr_link.h. Framing (little-endian, packed):
// u32 length (of what follows), u16 type, payload. One reader thread keeps the latest pose, calibration and the
// acknowledgements; the compositor thread sends.
#include "totk_vr_link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    MSG_HELLO = 1, MSG_HELLO_ACK = 2, MSG_CREATE_SWAPCHAIN = 3, MSG_SWAPCHAIN_ACK = 4, MSG_DESTROY_SWAPCHAIN = 5,
    MSG_DESTROY_SWAPCHAIN_ACK = 6, MSG_FRAME_READY = 7, MSG_TEARDOWN = 8, MSG_HEAD_POSE = 9, MSG_CONTROLLER_MOTION = 10,
    MSG_BUTTON_STATE = 11, MSG_HAPTIC = 12, MSG_INTERACTION_PROFILE = 13, MSG_INPUT_RESET = 14, MSG_VIEW_CONFIG = 15,
    MSG_CONTROLLER_MASK = 16, MSG_FRAME_CONSUMED = 17,
    MSG_HEADSET_STATUS = 18,   // local streamer change (zeldaVR): what the Quest reports it showed, once a second
    MSG_GAME_AUDIO = 100,   // local streamer change (zeldaVR third_party_patches): the game's own mix
};
enum { TOKEN_LEN = 32, MAX_MSG = 4096, CONSUMED_RING = 32 };

static struct {
    pthread_mutex_t mu; pthread_cond_t cv; pthread_mutex_t send_mu;
    int fd, alive, view_w, view_h;
    vr_pose pose; int have_pose;
    vr_views views; int have_views;
    uint64_t acked_swapchain; int swapchain_acked;
    uint64_t consumed[CONSUMED_RING]; int consumed_n;
    uint64_t poses_n, frames_n, consumed_total;
    float hs_shown, hs_latency; struct timespec hs_at; int have_hs;   // MSG_HEADSET_STATUS
    vr_controls pad; int have_pad; uint64_t buttons_n;
    vr_hand hands[2]; uint64_t motion_n;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .send_mu = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

static void fail(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t w = send(fd, b, n, 0); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; b += w; n -= (size_t)w; }
    return 0;
}
static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t r = recv(fd, b, n, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}

static int send_msg(uint16_t type, const void *payload, uint32_t n) {
    if (n + 2 > MAX_MSG) return -1;
    uint8_t buf[MAX_MSG + 4]; uint32_t len = n + 2;
    memcpy(buf, &len, 4); memcpy(buf + 4, &type, 2); if (n) memcpy(buf + 6, payload, n);
    pthread_mutex_lock(&g.send_mu);
    int fd = g.fd, r = fd >= 0 ? write_all(fd, buf, n + 6) : -1;
    pthread_mutex_unlock(&g.send_mu);
    return r;
}

static int read_msg(int fd, uint16_t *type, uint8_t *payload, uint32_t *n) {
    uint32_t len; if (read_all(fd, &len, 4) || len < 2 || len > MAX_MSG) return -1;
    if (read_all(fd, type, 2)) return -1;
    *n = len - 2; return *n ? read_all(fd, payload, *n) : 0;
}

typedef struct { uint8_t *b; uint32_t n, cap; } wbuf;
static void put(wbuf *w, const void *p, uint32_t n) { if (w->n + n <= w->cap) memcpy(w->b + w->n, p, n); w->n += n; }
static void put_u16(wbuf *w, uint16_t v) { put(w, &v, 2); }
static void put_u32(wbuf *w, uint32_t v) { put(w, &v, 4); }
static void put_u64(wbuf *w, uint64_t v) { put(w, &v, 8); }
static void put_f32(wbuf *w, float v) { put(w, &v, 4); }

static void handle(uint16_t type, const uint8_t *p, uint32_t n) {
    pthread_mutex_lock(&g.mu);
    switch (type) {
    case MSG_HEAD_POSE:
        if (n == 8 + 7 * 4) {
            vr_pose s; memcpy(&s.ts, p, 8); memcpy(s.q, p + 8, 16); memcpy(s.p, p + 24, 12);
            float qn = s.q[0] * s.q[0] + s.q[1] * s.q[1] + s.q[2] * s.q[2] + s.q[3] * s.q[3];
            if (qn > 0.25f && qn < 2.25f && (!g.have_pose || s.ts >= g.pose.ts)) { g.pose = s; g.have_pose = 1; g.poses_n++; }
        }
        break;
    case MSG_VIEW_CONFIG:
        if (n == 4 + 8 * 4) { memcpy(&g.views.ipd, p, 4); memcpy(g.views.fov, p + 4, 32); g.have_views = 1; }
        break;
    case MSG_SWAPCHAIN_ACK:
        if (n == 12) { memcpy(&g.acked_swapchain, p, 8); g.swapchain_acked = 1; }
        break;
    case MSG_HEADSET_STATUS:   // f32 frames shown in the last second, f32 their average latency in ms
        if (n == 8) { memcpy(&g.hs_shown, p, 4); memcpy(&g.hs_latency, p + 4, 4); clock_gettime(CLOCK_MONOTONIC, &g.hs_at); g.have_hs = 1; }
        break;
    case MSG_FRAME_CONSUMED:
        if (n == 8) {
            if (g.consumed_n == CONSUMED_RING) { memmove(g.consumed, g.consumed + 1, sizeof g.consumed - 8); g.consumed_n--; }
            memcpy(&g.consumed[g.consumed_n++], p, 8); g.consumed_total++;
        }
        break;
    case MSG_BUTTON_STATE:   // u64 path id, u32 kind (0 binary, 1 scalar), f32 value
        if (n == 16) {
            static const struct { uint64_t id; int what; } k_paths[] = {   // what: < 0 a button bit (-bit), >= 0 an axis
                {0xc72a0ba6ca52fba7ull, -VR_PAD_A}, {0x955047466e34669bull, -VR_PAD_B},
                {0xb1a13c485685e33cull, -VR_PAD_X}, {0x521496d23e3e9883ull, -VR_PAD_Y},
                {0x6268a669e87cf910ull, -VR_PAD_MENU},
                {0xdfc8a61273cf2569ull, -VR_PAD_LSTICK}, {0x00ac9a92f4453d61ull, -VR_PAD_RSTICK},
                {0xc3d709ed0d794ea1ull, 0}, {0xcb7d928706263554ull, 1},   // left stick x, y
                {0xd7c23648f060c6dfull, 2}, {0xcee8c338e02e0131ull, 3},   // right stick x, y
                {0x0b66bbe3dc2dc49full, 4}, {0xede7e982d5eedf21ull, 5},   // left, right trigger
                {0x01f1b9c689804b82ull, 6}, {0xdd5935cc005136f1ull, 7},   // left, right squeeze (grip)
            };
            uint64_t id; float v; memcpy(&id, p, 8); memcpy(&v, p + 12, 4);
            if (!(v == v)) break;
            for (size_t i = 0; i < sizeof k_paths / sizeof k_paths[0]; i++) {
                if (k_paths[i].id != id) continue;
                if (k_paths[i].what < 0) { uint32_t bit = (uint32_t)-k_paths[i].what; if (v > 0.5f) g.pad.buttons |= bit; else g.pad.buttons &= ~bit; }
                else { float *ax[8] = {&g.pad.lx, &g.pad.ly, &g.pad.rx, &g.pad.ry, &g.pad.ltrig, &g.pad.rtrig, &g.pad.lgrip, &g.pad.rgrip};
                       *ax[k_paths[i].what] = v < -1 ? -1 : v > 1 ? 1 : v; }
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g.pad.at_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
                g.have_pad = 1; g.buttons_n++;
                break;
            }
        }
        break;
    case MSG_CONTROLLER_MOTION:   // u64 ts, u32 hand, u32 0, f32 q[4], p[3], v[3], w[3]
        if (n == 68) {
            uint32_t hand; memcpy(&hand, p + 8, 4);
            if (hand < 2) { vr_hand *x = &g.hands[hand]; memcpy(&x->ts, p, 8); memcpy(x->q, p + 16, 16); memcpy(x->p, p + 32, 12); memcpy(x->v, p + 44, 12); memcpy(x->w, p + 56, 12); x->have = 1; g.motion_n++; }
        }
        break;
    case MSG_INPUT_RESET: memset(&g.pad, 0, sizeof g.pad); memset(g.hands, 0, sizeof g.hands); break;
    case MSG_TEARDOWN: g.alive = 0; break;
    default: break;   // controller motion, haptics: not used yet
    }
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

static void *reader(void *arg) {
    int fd = (int)(intptr_t)arg; uint8_t p[MAX_MSG]; uint16_t type; uint32_t n;
    while (!read_msg(fd, &type, p, &n)) { handle(type, p, n); if (type == MSG_TEARDOWN) break; }
    pthread_mutex_lock(&g.mu); g.alive = 0; pthread_cond_broadcast(&g.cv); pthread_mutex_unlock(&g.mu);
    pthread_mutex_lock(&g.send_mu); if (g.fd == fd) g.fd = -1; pthread_mutex_unlock(&g.send_mu);
    close(fd);
    return NULL;
}

static struct timespec deadline(int ms) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}
static int timed_wait(int ms) { struct timespec ts = deadline(ms); return pthread_cond_timedwait(&g.cv, &g.mu, &ts); }

int vr_link_open(const char *path, const char *renderer, char *err, size_t en) {
    if (!path || !*path) { fail(err, en, "no control file"); return -1; }
    FILE *f = fopen(path, "r");
    if (!f) { fail(err, en, "control file %s: %s (start basalt-streamer --runtime first)", path, strerror(errno)); return -1; }
    int port = 0, w = 0, h = 0; char token[TOKEN_LEN + 2] = {0};
    int got = fscanf(f, "%d %33s %d %d", &port, token, &w, &h); fclose(f);
    if (got < 2 || port <= 0 || port > 65535 || strlen(token) != TOKEN_LEN) { fail(err, en, "control file %s is malformed", path); return -1; }
    if (got < 4 || w < 256 || h < 256 || w > 2048 || h > 2304 || (w | h) & 1) { w = 1344; h = 1408; }   // outside 2048x2304 (H.264: 4096 wide side by side): the streamer's quality preset

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fail(err, en, "socket: %s", strerror(errno)); return -1; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { fail(err, en, "connect 127.0.0.1:%d: %s", port, strerror(errno)); close(fd); return -1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

    uint8_t hb[256]; wbuf b = { hb, 0, sizeof hb };
    const char *r = renderer ? renderer : ""; uint16_t rl = (uint16_t)strnlen(r, 128);
    uint64_t session = ((uint64_t)arc4random() << 32) | arc4random();
    put_u16(&b, VR_LINK_PROTOCOL); put(&b, token, TOKEN_LEN); put_u64(&b, session); put_u16(&b, rl); put(&b, r, rl);
    g.fd = fd;
    if (send_msg(MSG_HELLO, hb, b.n)) { fail(err, en, "hello: %s", strerror(errno)); close(fd); g.fd = -1; return -1; }
    uint16_t type; uint8_t p[MAX_MSG]; uint32_t n;
    if (read_msg(fd, &type, p, &n) || type != MSG_HELLO_ACK || n != 2) {
        fail(err, en, "the streamer refused the hello (wrong token or protocol version %u)", VR_LINK_PROTOCOL); close(fd); g.fd = -1; return -1; }

    pthread_mutex_lock(&g.mu);
    g.alive = 1; g.view_w = w; g.view_h = h; g.have_pose = g.have_views = g.swapchain_acked = 0; g.consumed_n = 0; g.have_hs = 0;
    pthread_mutex_unlock(&g.mu);
    pthread_t th;
    if (pthread_create(&th, NULL, reader, (void *)(intptr_t)fd)) { fail(err, en, "reader thread"); close(fd); g.fd = -1; g.alive = 0; return -1; }
    pthread_detach(th);
    return 0;
}

void vr_link_close(void) {
    uint32_t reason = 1;   // BASALT_TEARDOWN_SESSION_ENDED
    if (g.fd >= 0) { send_msg(MSG_TEARDOWN, &reason, 4); pthread_mutex_lock(&g.send_mu); if (g.fd >= 0) shutdown(g.fd, SHUT_RDWR); pthread_mutex_unlock(&g.send_mu); }
    pthread_mutex_lock(&g.mu); g.alive = 0; pthread_cond_broadcast(&g.cv); pthread_mutex_unlock(&g.mu);
}

int vr_link_alive(void) { pthread_mutex_lock(&g.mu); int a = g.alive; pthread_mutex_unlock(&g.mu); return a; }
void vr_link_view_size(int *w, int *h) { *w = g.view_w; *h = g.view_h; }

int vr_link_views(vr_views *out) {
    pthread_mutex_lock(&g.mu); int ok = g.have_views; if (ok) *out = g.views; pthread_mutex_unlock(&g.mu); return ok;
}

int vr_link_wait_pose(vr_pose *out, uint64_t after_ts, int timeout_ms) {
    pthread_mutex_lock(&g.mu);
    struct timespec until = deadline(timeout_ms > 0 ? timeout_ms : 0);
    while (timeout_ms > 0 && g.alive && (!g.have_pose || g.pose.ts <= after_ts))
        if (pthread_cond_timedwait(&g.cv, &g.mu, &until) == ETIMEDOUT) break;
    int fresh = g.have_pose && g.pose.ts > after_ts;
    if (g.have_pose) *out = g.pose;
    pthread_mutex_unlock(&g.mu);
    return fresh;
}

int vr_link_create_swapchain(uint64_t id, uint32_t w, uint32_t h, uint32_t format, int count, const uint32_t *surfaces) {
    if (count <= 0 || count > 8) return -1;
    uint8_t pb[64]; wbuf b = { pb, 0, sizeof pb };
    put_u64(&b, id); put_u32(&b, w); put_u32(&b, h); put_u32(&b, format); put_u32(&b, (uint32_t)count);
    for (int i = 0; i < count; i++) put_u32(&b, surfaces[i]);
    pthread_mutex_lock(&g.mu); g.swapchain_acked = 0; pthread_mutex_unlock(&g.mu);
    if (send_msg(MSG_CREATE_SWAPCHAIN, pb, b.n)) return -1;
    pthread_mutex_lock(&g.mu);
    for (int i = 0; i < 20 && g.alive && !(g.swapchain_acked && g.acked_swapchain == id); i++) timed_wait(100);
    int ok = g.swapchain_acked && g.acked_swapchain == id;
    pthread_mutex_unlock(&g.mu);
    return ok ? 0 : -1;
}

int vr_link_frame_ready(uint64_t frame, int64_t display_time, uint64_t tracking_ts, int nviews, const vr_view_submit *v) {
    if (nviews <= 0 || nviews > 4) return -1;
    uint8_t pb[28 + 4 * 76]; wbuf b = { pb, 0, sizeof pb };
    put_u64(&b, frame); put(&b, &display_time, 8); put_u64(&b, tracking_ts); put_u32(&b, (uint32_t)nviews);
    for (int i = 0; i < nviews; i++) {
        put_u64(&b, v[i].swapchain); put_u32(&b, v[i].image); put_u32(&b, 0);
        for (int k = 0; k < 4; k++) put_u32(&b, (uint32_t)v[i].rect[k]);
        for (int k = 0; k < 4; k++) put_f32(&b, v[i].pose.q[k]);
        for (int k = 0; k < 3; k++) put_f32(&b, v[i].pose.p[k]);
        for (int k = 0; k < 4; k++) put_f32(&b, v[i].fov[k]);
    }
    int r = send_msg(MSG_FRAME_READY, pb, b.n);
    if (!r) { pthread_mutex_lock(&g.mu); g.frames_n++; pthread_mutex_unlock(&g.mu); }
    return r;
}

int vr_link_headset(float *shown, float *latency_ms) {
    pthread_mutex_lock(&g.mu);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    const double age = (t.tv_sec - g.hs_at.tv_sec) + (t.tv_nsec - g.hs_at.tv_nsec) / 1e9;
    const int fresh = g.alive && g.have_hs && age < 2.5;
    if (fresh) { *shown = g.hs_shown; *latency_ms = g.hs_latency; }
    pthread_mutex_unlock(&g.mu);
    return fresh;
}

int vr_link_wait_consumed(int timeout_ms) {
    pthread_mutex_lock(&g.mu);
    struct timespec until = deadline(timeout_ms > 0 ? timeout_ms : 0);
    while (timeout_ms > 0 && g.alive && !g.consumed_n)
        if (pthread_cond_timedwait(&g.cv, &g.mu, &until) == ETIMEDOUT) break;
    int got = g.consumed_n > 0;
    pthread_mutex_unlock(&g.mu);
    return got;
}

int vr_link_take_consumed(uint64_t *out, int max) {
    pthread_mutex_lock(&g.mu);
    int k = g.consumed_n < max ? g.consumed_n : max;
    memcpy(out, g.consumed, (size_t)k * 8);
    memmove(g.consumed, g.consumed + k, (size_t)(g.consumed_n - k) * 8); g.consumed_n -= k;
    pthread_mutex_unlock(&g.mu);
    return k;
}

int vr_link_send_audio(const int16_t *pcm, int frames) {
    if (frames <= 0 || frames > 1000 || !vr_link_alive()) return -1;
    return send_msg(MSG_GAME_AUDIO, pcm, (uint32_t)frames * 4);
}

int vr_link_send_haptic(int hand, uint64_t duration_ns, float frequency, float amplitude) {   // HAPTIC (12): u32 hand, u64 ns, f32 Hz, f32 0..1
    if (hand < 0 || hand > 1 || !(amplitude >= 0 && amplitude <= 1) || !(frequency >= 0) || !vr_link_alive()) return -1;
    uint8_t p[20]; uint32_t h = (uint32_t)hand; memcpy(p, &h, 4); memcpy(p + 4, &duration_ns, 8); memcpy(p + 12, &frequency, 4); memcpy(p + 16, &amplitude, 4);
    return send_msg(MSG_HAPTIC, p, sizeof p);
}
void vr_link_hands(vr_hand out[2]) { pthread_mutex_lock(&g.mu); memcpy(out, g.hands, sizeof g.hands); pthread_mutex_unlock(&g.mu); }
int vr_link_pose(vr_pose *out) { pthread_mutex_lock(&g.mu); int have = g.have_pose; if (have) *out = g.pose; pthread_mutex_unlock(&g.mu); return have; }
int vr_link_controls(vr_controls *out) {
    pthread_mutex_lock(&g.mu); int have = g.have_pad; if (have) *out = g.pad; pthread_mutex_unlock(&g.mu);
    return have;
}
void vr_link_stats(char *out, size_t n) {
    pthread_mutex_lock(&g.mu);
    snprintf(out, n, "poses %llu, frames sent %llu, consumed %llu, controller inputs %llu, hand poses %llu, %s", (unsigned long long)g.poses_n,
             (unsigned long long)g.frames_n, (unsigned long long)g.consumed_total, (unsigned long long)g.buttons_n, (unsigned long long)g.motion_n, g.alive ? "connected" : "disconnected");
    pthread_mutex_unlock(&g.mu);
}
