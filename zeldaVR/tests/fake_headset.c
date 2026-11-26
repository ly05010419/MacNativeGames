// TOTK VR: a stand-in for `basalt-streamer --runtime <file>` and a Quest, for testing without a headset.
//   fake_headset selftest                       protocol round trip against totk_vr_link.c in one process (make probe-vr-link)
//   fake_headset serve <control file> [seconds] [png prefix] [yaw degrees per second]
//                                               waits for the game (TOTK_VR_CONTROL=<control file>), sends a head pose at
//                                               72 Hz (turning by the given rate), checks every FRAME_READY against the pose
//                                               it was rendered with, saves frame 30 and every 1440th (20 s) as <prefix>-<n>.png
//                                               FAKE_INPUT=<file>: the controllers, from lines appended to that file (never
//                                               edited in place): button a|b|x|y|menu|l3|r3 0|1, axis lx|ly|rx|ry|lt|rt|lg|rg <v>,
//                                               hand 0|1 <x y z> [<qx qy qz qw> [<vx vy vz>]] (held and sent at 72 Hz),
//                                               eye <file.png> (the next frame the game sends is saved there);
//                                               each vibration the game sends is printed as "haptic <hand> <ms> <Hz> <amp>"
// It speaks the BasaltVR v9 control protocol (runtime/pe/basalt_control_protocol.h) from the daemon side.
#include "../gfx/totk_vr_link.h"
#include <ApplicationServices/ApplicationServices.h>
#include <IOSurface/IOSurface.h>
#include <ImageIO/ImageIO.h>
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
enum { MAX_MSG = 4096 };
static const char TOKEN[] = "0123456789abcdef0123456789abcdef";
static const float FOV[2][4] = { { -0.942f, 0.698f, 0.768f, -0.890f }, { -0.698f, 0.942f, 0.768f, -0.890f } };   // Quest 3-like

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int rd(int fd, void *p, size_t n) { uint8_t *b = p; while (n) { ssize_t r = recv(fd, b, n, 0); if (r <= 0) return -1; b += r; n -= (size_t)r; } return 0; }
static pthread_mutex_t g_send = PTHREAD_MUTEX_INITIALIZER;
static int snd(int fd, uint16_t type, const void *p, uint32_t n) {
    uint8_t b[MAX_MSG + 6]; uint32_t len = n + 2; memcpy(b, &len, 4); memcpy(b + 4, &type, 2); if (n) memcpy(b + 6, p, n);
    pthread_mutex_lock(&g_send); ssize_t w = send(fd, b, n + 6, 0); pthread_mutex_unlock(&g_send);
    return w == (ssize_t)(n + 6) ? 0 : -1;
}
static int rmsg(int fd, uint16_t *type, uint8_t *p, uint32_t *n) {
    uint32_t len; if (rd(fd, &len, 4) || len < 2 || len > MAX_MSG || rd(fd, type, 2)) return -1;
    *n = len - 2; return *n ? rd(fd, p, *n) : 0;
}

typedef struct { int listen_fd, fd; double seconds, yaw_rate; const char *png, *pose_file; int sent_poses; _Atomic int stop; } daemon_t;
#define POSE_RING 512
static struct { uint64_t ts; float q[4]; } g_poses[POSE_RING];
static int g_npose;
static pthread_mutex_t g_pose_mu = PTHREAD_MUTEX_INITIALIZER;

static void *pose_thread(void *arg) {
    daemon_t *d = arg; uint64_t t0 = now_ns();
    while (!d->stop) {
        uint64_t t = now_ns(); double s = (t - t0) / 1e9, yaw = d->yaw_rate * s * M_PI / 180.0;
        uint8_t p[36]; float q[4] = { 0, (float)sin(yaw / 2), 0, (float)cos(yaw / 2) }, pos[3] = { 0, 1.6f, 0 };
        if(d->pose_file){FILE *f=fopen(d->pose_file,"r");if(f){
            float a[7];if(fscanf(f,"%f %f %f %f %f %f %f",a,a+1,a+2,a+3,a+4,a+5,a+6)==7){
                int valid=1;for(int k=0;k<7;k++)valid&=isfinite(a[k]);
                if(valid){memcpy(q,a,16);memcpy(pos,a+4,12);}}
            fclose(f);}}
        memcpy(p, &t, 8); memcpy(p + 8, q, 16); memcpy(p + 24, pos, 12);
        pthread_mutex_lock(&g_pose_mu); g_poses[g_npose % POSE_RING].ts = t; memcpy(g_poses[g_npose % POSE_RING].q, q, 16); g_npose++; pthread_mutex_unlock(&g_pose_mu);
        if (snd(d->fd, 9, p, 36)) break;
        d->sent_poses++;
        usleep(13889);   // 72 Hz
    }
    return NULL;
}

/* The controllers, as the streamer sends them: BUTTON_STATE (11) by the ALVR path ids, CONTROLLER_MOTION (10). */
static const struct { const char *name; uint64_t id; } k_inputs[] = {
    {"a", 0xc72a0ba6ca52fba7ull}, {"b", 0x955047466e34669bull}, {"x", 0xb1a13c485685e33cull}, {"y", 0x521496d23e3e9883ull},
    {"menu", 0x6268a669e87cf910ull}, {"l3", 0xdfc8a61273cf2569ull}, {"r3", 0x00ac9a92f4453d61ull},
    {"lx", 0xc3d709ed0d794ea1ull}, {"ly", 0xcb7d928706263554ull}, {"rx", 0xd7c23648f060c6dfull}, {"ry", 0xcee8c338e02e0131ull},
    {"lt", 0x0b66bbe3dc2dc49full}, {"rt", 0xede7e982d5eedf21ull}, {"lg", 0x01f1b9c689804b82ull}, {"rg", 0xdd5935cc005136f1ull},
};
static int send_input(int fd, const char *name, float v) {
    for (size_t i = 0; i < sizeof k_inputs / sizeof k_inputs[0]; i++) {
        if (strcmp(k_inputs[i].name, name)) continue;
        uint8_t m[16]; const uint32_t kind = i < 7 ? 0 : 1;
        memcpy(m, &k_inputs[i].id, 8); memcpy(m + 8, &kind, 4); memcpy(m + 12, &v, 4);
        return snd(fd, 11, m, sizeof m);
    }
    printf("input: no such control \"%s\"\n", name);
    return 0;
}
typedef struct { int have; float q[4], p[3], v[3]; } fake_hand;
static char g_eye_path[1024]; static _Atomic int g_eye_wanted;   /* "eye <file>": save the next frame */
static void input_line(int fd, const char *line, fake_hand hands[2]) {
    char name[16]; float v; int hand; float a[10];
    if (!strncmp(line, "eye ", 4)) { if (sscanf(line, "eye %1023s", g_eye_path) == 1) atomic_store(&g_eye_wanted, 1); return; }
    if (sscanf(line, "button %15s %f", name, &v) == 2 || sscanf(line, "axis %15s %f", name, &v) == 2) { send_input(fd, name, v); return; }
    if (sscanf(line, "hand %d", &hand) != 1 || hand < 0 || hand > 1) { if (*line && *line != '\n') printf("input: cannot read \"%s\"\n", line); return; }
    const int n = sscanf(line, "hand %*d %f %f %f %f %f %f %f %f %f %f", a, a+1, a+2, a+3, a+4, a+5, a+6, a+7, a+8, a+9);
    if (n != 3 && n != 7 && n != 10) { printf("input: a hand takes 3, 7 or 10 numbers: \"%s\"\n", line); return; }
    fake_hand *h = &hands[hand];
    memcpy(h->p, a, 12);
    if (n >= 7) memcpy(h->q, a + 3, 16); else if (!h->have) { h->q[0] = h->q[1] = h->q[2] = 0; h->q[3] = 1; }
    if (n == 10) memcpy(h->v, a + 7, 12); else memset(h->v, 0, sizeof h->v);
    h->have = 1;
}
static void *input_thread(void *arg) {
    daemon_t *d = arg; const char *path = getenv("FAKE_INPUT"); FILE *f = NULL; char line[256]; fake_hand hands[2] = {{0}};
    while (!d->stop) {
        if (!f) f = fopen(path, "r");
        if (f) { clearerr(f); while (fgets(line, sizeof line, f)) input_line(d->fd, line, hands); }
        const uint64_t t = now_ns();
        for (uint32_t i = 0; i < 2; i++) {
            if (!hands[i].have) continue;
            uint8_t m[68] = {0}; memcpy(m, &t, 8); memcpy(m + 8, &i, 4); memcpy(m + 16, hands[i].q, 16); memcpy(m + 32, hands[i].p, 12); memcpy(m + 44, hands[i].v, 12);
            if (snd(d->fd, 10, m, sizeof m)) { if (f) fclose(f); return NULL; }
        }
        usleep(13889);
    }
    if (f) fclose(f);
    return NULL;
}

static void save_png(IOSurfaceRef s, const int32_t rect[4], const char *path) {
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    size_t row = IOSurfaceGetBytesPerRow(s); uint8_t *base = IOSurfaceGetBaseAddress(s);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(base + (size_t)rect[1] * row + (size_t)rect[0] * 4, (size_t)rect[2] * 2, (size_t)rect[3], 8, row, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); printf("%s %s\n", CGImageDestinationFinalize(dst) ? "saved" : "could not save", path); CFRelease(dst); }
    if (img) CGImageRelease(img); if (ctx) CGContextRelease(ctx); CGColorSpaceRelease(cs); CFRelease(url);
}

// Average of the left eye's centre 64x64 (BGRA); used to check the frame is not empty.
static double centre_mean(IOSurfaceRef s, const int32_t r[4]) {
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    size_t row = IOSurfaceGetBytesPerRow(s); const uint8_t *b = IOSurfaceGetBaseAddress(s); double sum = 0;
    for (int y = r[1] + r[3] / 2 - 32; y < r[1] + r[3] / 2 + 32; y++)
        for (int x = r[0] + r[2] / 2 - 32; x < r[0] + r[2] / 2 + 32; x++) { const uint8_t *p = b + (size_t)y * row + (size_t)x * 4; sum += p[0] + p[1] + p[2]; }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return sum / (64.0 * 64 * 3);
}

static int serve(daemon_t *d) {
    const char *capture_env=getenv("TOTK_VR_TEST_CAPTURE_EVERY");
    int capture_every=capture_env?atoi(capture_env):1440;
    if(capture_every<1||capture_every>72000)FAIL("invalid capture interval");
    uint16_t type; uint8_t p[MAX_MSG]; uint32_t n;
    if (rmsg(d->fd, &type, p, &n) || type != 1 || n < 2 + 32 + 8 + 2) FAIL("no hello");
    uint16_t ver; memcpy(&ver, p, 2);
    if (ver != 9 || memcmp(p + 2, TOKEN, 32)) FAIL("hello: version %u or token wrong", ver);
    uint16_t rl; memcpy(&rl, p + 42, 2); printf("hello from \"%.*s\"\n", rl, (const char *)p + 44);
    uint16_t ack = 9; snd(d->fd, 2, &ack, 2);
    uint8_t vc[36]; float ipd = 0.063f; memcpy(vc, &ipd, 4); memcpy(vc + 4, FOV, 32); snd(d->fd, 15, vc, 36);
    pthread_t pt, it; pthread_create(&pt, NULL, pose_thread, d);
    const int scripted = getenv("FAKE_INPUT") != NULL;
    if (scripted) pthread_create(&it, NULL, input_thread, d);

    IOSurfaceRef surf[8] = {0}; uint64_t chain = 0; int frames = 0, bad = 0, nimg = 0, gaps = 0; uint64_t last_fr = 0; uint64_t t_end = now_ns() + (uint64_t)(d->seconds * 1e9), first_ns = 0;
    double mean_sum = 0;
    while (now_ns() < t_end) {
        struct pollfd pf = { d->fd, POLLIN, 0 };
        if (poll(&pf, 1, 100) == 0) continue;
        if (rmsg(d->fd, &type, p, &n)) { printf("runtime disconnected\n"); break; }
        if (type == 3) {
            uint32_t w, h, fmt; memcpy(&chain, p, 8); memcpy(&w, p + 8, 4); memcpy(&h, p + 12, 4); memcpy(&fmt, p + 16, 4); memcpy(&nimg, p + 20, 4);
            if (fmt != 87 || nimg <= 0 || nimg > 8 || n != 24u + 4u * (uint32_t)nimg) FAIL("create swapchain: format %u, %d images", fmt, nimg);
            for (int i = 0; i < nimg; i++) { uint32_t id; memcpy(&id, p + 24 + 4 * i, 4); surf[i] = IOSurfaceLookup(id);
                if (!surf[i] || IOSurfaceGetWidth(surf[i]) != w || IOSurfaceGetHeight(surf[i]) != h || IOSurfaceGetPixelFormat(surf[i]) != 'BGRA')
                    FAIL("swapchain image %d (IOSurface %u) not found or wrong size / format", i, id); }
            printf("swapchain %llu: %d images %ux%u BGRA\n", (unsigned long long)chain, nimg, w, h);
            uint8_t a[12]; uint32_t gen = 1; memcpy(a, &chain, 8); memcpy(a + 8, &gen, 4); snd(d->fd, 4, a, 12);
        } else if (type == 7) {
            uint64_t fr, ts; uint32_t nv; memcpy(&fr, p, 8); memcpy(&ts, p + 16, 8); memcpy(&nv, p + 24, 4);
            if (nv != 2 || n != 28 + 76 * nv) FAIL("frame %llu: %u views", (unsigned long long)fr, nv);
            int known = 0; pthread_mutex_lock(&g_pose_mu);
            for (int i = 0; i < POSE_RING && i < g_npose; i++) if (g_poses[i].ts == ts) known = 1;
            pthread_mutex_unlock(&g_pose_mu);
            if (!known) bad++;
            uint64_t sc; uint32_t img; int32_t rect[4]; memcpy(&sc, p + 28, 8); memcpy(&img, p + 36, 4); memcpy(rect, p + 44, 16);
            if (sc != chain || (int)img >= nimg || !surf[img]) FAIL("frame %llu: swapchain %llu image %u", (unsigned long long)fr, (unsigned long long)sc, img);
            mean_sum += centre_mean(surf[img], rect);
            if (!first_ns) first_ns = now_ns();
            frames++;
            if (d->png && (frames == 30 || frames % capture_every == 0)) { char path[1024]; snprintf(path, sizeof path, "%s-%d.png", d->png, frames); save_png(surf[img], rect, path); }
            if (atomic_exchange(&g_eye_wanted, 0)) { save_png(surf[img], rect, g_eye_path); printf("eye saved: %s (frame %d)\n", g_eye_path, frames); }
            /* Like basalt-streamer (RuntimeLink.markFrameConsumed): frames are acknowledged strictly in order, so a
               missing frame number stops every later acknowledgement. */
            if (fr != last_fr + 1) { if (!gaps++) printf("frame number %llu after %llu: the streamer would acknowledge nothing after %llu\n",
                                                        (unsigned long long)fr, (unsigned long long)last_fr, (unsigned long long)last_fr); }
            else { last_fr = fr; snd(d->fd, 17, &fr, 8); }
        } else if (type == 12 && n == 20) {   /* HAPTIC: u32 hand, u64 ns, f32 Hz, f32 0..1 */
            uint32_t hand; uint64_t ns; float hz, amp; memcpy(&hand, p, 4); memcpy(&ns, p + 4, 8); memcpy(&hz, p + 12, 4); memcpy(&amp, p + 16, 4);
            printf("haptic %u %.0f %.0f %.2f\n", hand, ns / 1e6, hz, amp);
        } else if (type == 8) { printf("runtime teardown\n"); break; }
    }
    d->stop = 1; pthread_join(pt, NULL);
    if (scripted) pthread_join(it, NULL);
    double secs = first_ns ? (now_ns() - first_ns) / 1e9 : 0;
    printf("frames %d (%.1f fps), poses sent %d, frames with an unknown pose timestamp %d, frame-number gaps %d, left-eye centre mean %.1f\n",
           frames, secs > 0 ? frames / secs : 0, d->sent_poses, bad, gaps, frames ? mean_sum / frames : 0);
    uint32_t reason = 1; snd(d->fd, 8, &reason, 4);
    return frames > 0 && !bad && !gaps ? 0 : 1;
}

static int listen_and_write(const char *control) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }; socklen_t al = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 1) || getsockname(fd, (struct sockaddr *)&a, &al)) FAIL("listen");
    FILE *f = fopen(control, "w"); if (!f) FAIL("cannot write %s", control);
    fprintf(f, "%d %s %d %d\n", ntohs(a.sin_port), TOKEN, 1344, 1408); fclose(f);
    return fd;
}

// ---- selftest: our client against this daemon ----
static void *selftest_daemon(void *arg) {
    daemon_t *d = arg; d->fd = accept(d->listen_fd, NULL, NULL);
    if (d->fd < 0) FAIL("accept");
    int r = serve(d); close(d->fd); return (void *)(intptr_t)r;
}

static IOSurfaceRef make_surface(int w, int h, uint8_t fill) {
    int bpr = (int)IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)w * 4);
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int bpe = 4; uint32_t fmt = 'BGRA';
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h), ne = CFNumberCreate(NULL, kCFNumberIntType, &bpe),
                nr = CFNumberCreate(NULL, kCFNumberIntType, &bpr), nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    CFDictionarySetValue(p, kIOSurfaceWidth, nw); CFDictionarySetValue(p, kIOSurfaceHeight, nh); CFDictionarySetValue(p, kIOSurfaceBytesPerElement, ne);
    CFDictionarySetValue(p, kIOSurfaceBytesPerRow, nr); CFDictionarySetValue(p, kIOSurfacePixelFormat, nf); CFDictionarySetValue(p, CFSTR("IOSurfaceIsGlobal"), kCFBooleanTrue);
    IOSurfaceRef s = IOSurfaceCreate(p);
    CFRelease(nw); CFRelease(nh); CFRelease(ne); CFRelease(nr); CFRelease(nf); CFRelease(p);
    if (s) { IOSurfaceLock(s, 0, NULL); memset(IOSurfaceGetBaseAddress(s), fill, IOSurfaceGetAllocSize(s)); IOSurfaceUnlock(s, 0, NULL); }
    return s;
}

static int selftest(void) {
    char control[] = "/tmp/totk-vr-selftest-XXXXXX"; int cf = mkstemp(control); if (cf < 0) FAIL("mkstemp"); close(cf);
    daemon_t d = { .seconds = 3, .yaw_rate = 0 }; d.listen_fd = listen_and_write(control);
    pthread_t dt; pthread_create(&dt, NULL, selftest_daemon, &d);
    char err[256];
    if (vr_link_open("/nonexistent/control", "x", err, sizeof err) == 0) FAIL("opened a missing control file");
    if (vr_link_open(control, "MacNativeGames selftest", err, sizeof err)) FAIL("open: %s", err);
    int w, h; vr_link_view_size(&w, &h); if (w != 1344 || h != 1408) FAIL("view size %dx%d", w, h);
    vr_pose pose = {0}; if (!vr_link_wait_pose(&pose, 0, 1000)) FAIL("no head pose");
    vr_views v; for (int i = 0; i < 100 && !vr_link_views(&v); i++) usleep(10000);
    if (!vr_link_views(&v) || fabsf(v.ipd - 0.063f) > 1e-6f || v.fov[1][1] != FOV[1][1]) FAIL("view config");
    IOSurfaceRef s[3]; uint32_t ids[3];
    for (int i = 0; i < 3; i++) { s[i] = make_surface(2 * w, h, (uint8_t)(0x40 * (i + 1))); if (!s[i]) FAIL("IOSurface"); ids[i] = IOSurfaceGetID(s[i]); }
    if (vr_link_create_swapchain(7, (uint32_t)(2 * w), (uint32_t)h, VR_LINK_FORMAT_BGRA8, 3, ids)) FAIL("swapchain not acknowledged");
    uint64_t last = 0; int sent = 0;
    for (int f = 1; f <= 20; f++) {
        if (!vr_link_wait_pose(&pose, last, 100)) FAIL("pose stalled at frame %d", f);
        last = pose.ts;
        vr_view_submit vs[2] = { { 7, (uint32_t)(f % 3), { 0, 0, w, h }, pose, {0} }, { 7, (uint32_t)(f % 3), { w, 0, w, h }, pose, {0} } };
        if (vr_link_frame_ready((uint64_t)f, 0, pose.ts, 2, vs)) FAIL("frame_ready %d", f);
        sent++;
    }
    uint64_t got[32]; int ng = 0;
    for (int i = 0; i < 100 && ng < sent; i++) { ng += vr_link_take_consumed(got + ng, 32 - ng); usleep(10000); }
    if (ng != sent || got[0] != 1 || got[sent - 1] != (uint64_t)sent) FAIL("consumed %d of %d", ng, sent);
    void *r; pthread_join(dt, &r);
    for (int i = 0; i < 50 && vr_link_alive(); i++) usleep(10000);
    if (vr_link_alive()) FAIL("still connected after the daemon's teardown");
    char st[128]; vr_link_stats(st, sizeof st); printf("link: %s\n", st);
    for (int i = 0; i < 3; i++) CFRelease(s[i]);
    unlink(control);
    if (r) FAIL("daemon side reported a problem");
    printf("vr link selftest: OK\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered: a test driver reads the log while a run goes */
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc >= 3 && !strcmp(argv[1], "serve")) {
        daemon_t d = { .seconds = argc > 3 ? atof(argv[3]) : 60, .png = argc > 4 ? argv[4] : NULL, .yaw_rate = argc > 5 ? atof(argv[5]) : 0,
                       .pose_file = argc > 6 ? argv[6] : NULL };
        d.listen_fd = listen_and_write(argv[2]);
        printf("waiting for the game: TOTK_VR_CONTROL=%s\n", argv[2]); fflush(stdout);
        d.fd = accept(d.listen_fd, NULL, NULL); if (d.fd < 0) FAIL("accept");
        int r = serve(&d); unlink(argv[2]); return r;
    }
    fprintf(stderr, "usage: %s selftest | serve <control file> [seconds] [png prefix] [yaw deg/s]\n", argv[0]);
    return 2;
}
