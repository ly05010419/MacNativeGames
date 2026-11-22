/* The services this port answers, end to end through the shared IPC server (shared/runtime/ipc.c, threads.c): requests
   are built as libnx builds them (HIPC + CMIF), sent with svcConnectToNamedPort / svcSendSyncRequest, and the answers
   checked: the firmware version, the applet's answers, the SD card as a folder (paths, files, a refused path), the RomFS
   image, the clock, hid's shared memory, and the screen -- a layer, nvmap, a preallocated block-linear buffer queued and
   arriving at the window as the picture the game drew. TOTK_PROFILE_DIR / TOTK_ROMFS point at temp folders. */
#include "../runtime/hb.h"
#include "../runtime/fb_convert.h"
#include "../runtime/keyboard_layout.h"
#include "runtime_tables.h"
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
uint64_t nvn_bl_offset(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, unsigned, unsigned);

/* ---------- what the loader and the window would give ---------- */
pthread_key_t runtime_keys[8];
_Noreturn void runtime_fail(const char *m) { printf("FAIL: runtime_fail(%s)\n", m); exit(1); }
void *runtime_symbol(const char *n) { (void)n; return NULL; }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void runtime_thread_init(void) {}
void runtime_gate(void) {}
void runtime_dispatch(uint64_t r, uint64_t *g) { (void)r; (void)g; }
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
void runtime_describe_address(uint64_t a, char *o, size_t n) { snprintf(o, n, "%#llx", (unsigned long long)a); }
uint64_t runtime_image_bytes(void) { return 0x400000; }
int runtime_module_query(uint64_t a, uint64_t *b, uint64_t *s, unsigned *st, unsigned *p, unsigned *at) { (void)a; (void)b; (void)s; (void)st; (void)p; (void)at; return 0; }
uint64_t runtime_module_permission(uint64_t a, uint64_t s, unsigned p) { (void)a; (void)s; (void)p; return 0; }
uint64_t runtime_module_lock_permission(uint64_t a, uint64_t s, unsigned m, unsigned at) { (void)a; (void)s; (void)m; (void)at; return 0; }
void runtime_front_request_close(void) {}
void runtime_boot_status(const char *a, const char *b) { (void)a; (void)b; }
void hb_input_snapshot(HbInput *o) { memset(o, 0, sizeof *o); }
static uint8_t *shown; static unsigned shown_w, shown_h;
void runtime_front_present(const uint32_t *rgba, unsigned w, unsigned h) { free(shown); shown = malloc((size_t)w * h * 4); memcpy(shown, rgba, (size_t)w * h * 4); shown_w = w; shown_h = h; }

/* ---------- a libnx-shaped client ---------- */
static uint32_t msg[64];
typedef struct { uint64_t addr, size; } Buf;
typedef struct { const void *data; uint32_t size; Buf x, a, b, c; int pid; uint32_t copy; } Req;
static void put_ab(uint32_t *p, Buf b) {
    p[0] = (uint32_t)b.size; p[1] = (uint32_t)b.addr;
    p[2] = (uint32_t)(((b.addr >> 36) & 0x7) << 2) | (uint32_t)(((b.size >> 32) & 0xf) << 24) | (uint32_t)(((b.addr >> 32) & 0xf) << 28);
}
typedef struct { uint32_t result; uint8_t data[0x100]; uint32_t handles[8]; unsigned nhandles; } Resp;
static Resp call(uint32_t handle, uint32_t cmd, Req q) {
    memset(msg, 0, sizeof msg);
    unsigned nx = q.x.size ? 1 : 0, na = q.a.size ? 1 : 0, nb = q.b.size ? 1 : 0, special = q.pid || q.copy;
    uint32_t *p = msg + 2;
    if (special) { *p++ = (q.pid ? 1 : 0) | (q.copy ? 1u << 1 : 0); if (q.pid) { *p++ = 0; *p++ = 0; } if (q.copy) *p++ = q.copy; }
    if (nx) { p[0] = (uint32_t)((q.x.size & 0xffff) << 16) | (uint32_t)(((q.x.addr >> 32) & 0xf) << 12) | (uint32_t)(((q.x.addr >> 36) & 0x3f) << 6); p[1] = (uint32_t)q.x.addr; p += 2; }
    if (na) { put_ab(p, q.a); p += 3; }
    if (nb) { put_ab(p, q.b); p += 3; }
    uint8_t *pay = (uint8_t *)msg + ((((uint8_t *)p - (uint8_t *)msg) + 15) & ~15);
    uint32_t h[4] = {0x49434653, 0, cmd, 0}; memcpy(pay, h, 16); if (q.size) memcpy(pay + 16, q.data, q.size);
    unsigned nwords = (unsigned)((pay - (uint8_t *)p) + 16 + q.size + 3) / 4;
    uint32_t *recv = p + nwords;
    if (q.c.size) { recv[0] = (uint32_t)q.c.addr; recv[1] = (uint32_t)((q.c.addr >> 32) & 0xffff) | (uint32_t)(q.c.size << 16); }
    msg[0] = 4 | nx << 16 | na << 20 | nb << 24; msg[1] = nwords | (q.c.size ? 2u << 10 : 0) | (special ? 1u << 31 : 0);
    uint64_t r[31] = {handle};
    if (!runtime_ipc_svc(0x21, r) || r[0]) runtime_fail("SendSyncRequest");
    Resp out; memset(&out, 0, sizeof out);
    p = msg + 2;
    if (msg[1] >> 31) { uint32_t hd = *p++; unsigned n = ((hd >> 1) & 15) + ((hd >> 5) & 15); for (unsigned i = 0; i < n; i++) out.handles[out.nhandles++] = *p++; }
    p += ((msg[0] >> 16) & 15) * 2;
    pay = (uint8_t *)msg + ((((uint8_t *)p - (uint8_t *)msg) + 15) & ~15);
    memcpy(h, pay, 16); if (h[0] != 0x4f434653) runtime_fail("CMIF reply magic");
    out.result = h[2]; memcpy(out.data, pay + 16, sizeof out.data - 16);
    return out;
}
#define REQ(...) ((Req){__VA_ARGS__})
#define BUF(p, n) ((Buf){(uintptr_t)(p), (n)})
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static uint32_t sm;
static uint32_t service(const char *name) { char n[8] = {0}; memcpy(n, name, strlen(name)); Resp r = call(sm, 1, REQ(.data = n, .size = 8)); return r.handles[0]; }
static uint32_t object(uint32_t h, uint32_t cmd, Req q) { Resp r = call(h, cmd, q); if (r.result || !r.nhandles) { printf("FAIL: cmd %u gave no interface (%#x)\n", cmd, r.result); exit(1); } return r.handles[0]; }

static void system_and_applet(void) {
    uint8_t fw[0x100] = {0}; Resp r = call(service("set:sys"), 3, REQ(.c = BUF(fw, sizeof fw)));
    CHECK(!r.result && fw[0] == HB_FW_MAJOR && fw[1] == HB_FW_MINOR && fw[2] == HB_FW_MICRO, "set:sys firmware %u.%u.%u", fw[0], fw[1], fw[2]);
    uint32_t proxy = object(service("appletOE"), 0, REQ(.pid = 1, .copy = 0xffff8001));
    r = call(object(proxy, 2, REQ()), 1, REQ()); uint64_t aruid; memcpy(&aruid, r.data, 8);
    CHECK(aruid == HB_ARUID, "applet resource user id");
    uint32_t common = object(proxy, 0, REQ());
    r = call(common, 9, REQ()); CHECK(r.data[0] == 1, "in focus");
    r = call(common, 1, REQ()); CHECK(r.result == 0x680, "no applet message (%#x)", r.result);
    r = call(common, 60, REQ()); int32_t wh[2]; memcpy(wh, r.data, 8); CHECK(wh[0] == 1280 && wh[1] == 720, "default display resolution");
    r = call(common, 5, REQ()); CHECK(r.data[0] == 0, "handheld");
    uint32_t tz = object(service("time:u"), 3, REQ());
    setenv("TZ", "UTC", 1); tzset();
    int64_t t = 86400 * 365; r = call(tz, 101, REQ(.data = &t, .size = 8));
    uint16_t year; memcpy(&year, r.data, 2); CHECK(year == 1971 && r.data[2] == 1 && r.data[3] == 1, "ToCalendarTimeWithMyRule: 1971-01-01 (%u-%u-%u)", year, r.data[2], r.data[3]);
}
static void filesystem(const char *profile, const char *romfs) {
    uint32_t fsp = service("fsp-srv");
    uint64_t pid = 0; CHECK(!call(fsp, 1, REQ(.data = &pid, .size = 8, .pid = 1)).result, "SetCurrentProcess");
    uint32_t sd = object(fsp, 18, REQ());
    char path[0x301] = "/switch/mj2013/save";
    CHECK(!call(sd, 2, REQ(.x = BUF(path, sizeof path))).result, "CreateDirectory");
    snprintf(path, sizeof path, "/switch/mj2013/save/update.rom");
    struct { uint32_t option, pad; int64_t size; } cf = {0, 0, 0};
    CHECK(!call(sd, 0, REQ(.data = &cf, .size = sizeof cf, .x = BUF(path, sizeof path))).result, "CreateFile");
    CHECK(call(sd, 0, REQ(.data = &cf, .size = sizeof cf, .x = BUF(path, sizeof path))).result == 0x402, "CreateFile again: PathAlreadyExists");
    uint32_t mode = 6, file = object(sd, 8, REQ(.data = &mode, .size = 4, .x = BUF(path, sizeof path)));
    const char text[] = "[FLAG]\r\nRESULT=0\r\n";
    struct { uint32_t option, pad; int64_t off; uint64_t size; } io = {0, 0, 0, sizeof text - 1};
    CHECK(!call(file, 1, REQ(.data = &io, .size = sizeof io, .a = BUF(text, sizeof text - 1))).result, "Write");
    char host[1024]; snprintf(host, sizeof host, "%s/sdmc/switch/mj2013/save/update.rom", profile);
    FILE *f = fopen(host, "rb"); char back[64] = {0}; size_t got = f ? fread(back, 1, sizeof back, f) : 0; if (f) fclose(f);
    CHECK(got == sizeof text - 1 && !memcmp(back, text, got), "the save is %s", host);
    char rd[64] = {0}; io.off = 8; io.size = 8; Resp r = call(file, 0, REQ(.data = &io, .size = sizeof io, .b = BUF(rd, sizeof rd)));
    uint64_t n; memcpy(&n, r.data, 8); CHECK(n == 8 && !memcmp(rd, "RESULT=0", 8), "Read at 8: %.8s", rd);
    r = call(sd, 7, REQ(.x = BUF(path, sizeof path))); uint32_t type; memcpy(&type, r.data, 4); CHECK(type == 1, "GetEntryType: file");
    snprintf(path, sizeof path, "/switch/mj2013/save/system/font_l24.rom");
    CHECK(call(sd, 8, REQ(.data = &mode, .size = 4, .x = BUF(path, sizeof path))).result == 0x202, "OpenFile of a missing file: PathNotFound");
    snprintf(path, sizeof path, "/switch/../../escape");
    CHECK(call(sd, 2, REQ(.x = BUF(path, sizeof path))).result == 0x2ee602, "a path out of the card is refused");
    uint32_t dmode = 3; snprintf(path, sizeof path, "/switch/mj2013/save");
    uint32_t dir = object(sd, 9, REQ(.data = &dmode, .size = 4, .x = BUF(path, sizeof path)));
    uint8_t entries[0x310 * 4]; r = call(dir, 0, REQ(.b = BUF(entries, sizeof entries))); int64_t count; memcpy(&count, r.data, 8);
    CHECK(count == 1 && !strcmp((char *)entries, "update.rom") && entries[0x304] == 1, "the save folder lists update.rom");
    /* the RomFS image */
    uint32_t storage = object(fsp, 200, REQ());
    r = call(storage, 4, REQ()); uint64_t size; memcpy(&size, r.data, 8); CHECK(size == ROMFS_SIZE, "RomFS size");
    struct { int64_t off; uint64_t size; } sr = {100, 16}; uint8_t sb[16];
    CHECK(!call(storage, 0, REQ(.data = &sr, .size = sizeof sr, .b = BUF(sb, sizeof sb))).result, "RomFS read");
    int same = 1; for (unsigned i = 0; i < 16; i++) same &= sb[i] == (uint8_t)(100 + i);
    CHECK(same, "RomFS bytes from %s", romfs);
}
static void hid(void) {
    uint32_t res = object(service("hid"), 0, REQ());
    Resp r = call(res, 0, REQ());
    void *runtime_handle_get(uint32_t h, int session);
    struct { uint32_t magic; uint64_t size; void *host; } *shm = runtime_handle_get(r.handles[0], 0);
    CHECK(r.nhandles == 1 && shm && shm->size == 0x40000, "hid shared memory: 256 KiB");
}
static void display(void) {
    uint32_t ads = object(service("vi:m"), 2, REQ());
    uint32_t relay = object(ads, 100, REQ());
    char name[0x40] = "Default"; Resp r = call(ads, 1010, REQ(.data = name, .size = sizeof name)); uint64_t display_id; memcpy(&display_id, r.data, 8);
    struct { char name[0x40]; uint64_t layer, aruid; } ol; memset(&ol, 0, sizeof ol); memcpy(ol.name, "Default", 8); ol.layer = 1;
    uint8_t nw[0x100]; r = call(ads, 2020, REQ(.data = &ol, .size = sizeof ol, .pid = 1, .b = BUF(nw, sizeof nw)));
    uint32_t hdr[4]; memcpy(hdr, nw, 16); uint32_t binder; memcpy(&binder, nw + hdr[1] + 8, 4);
    CHECK(!r.result && binder == 1, "OpenLayer: native window with binder id 1");
    /* nvmap: the game's frame memory */
    uint32_t nv = service("nvdrv"); uint32_t out[2];
    r = call(nv, 0, REQ(.a = BUF("/dev/nvmap", 10))); memcpy(out, r.data, 8); uint32_t fd = out[0];
    const uint32_t W = 1280, H = 720, pitch = W * 4, fb_size = pitch * 768;
    uint8_t *mem = aligned_alloc(0x1000, fb_size * 2); memset(mem, 0, fb_size * 2);
    uint32_t create[2] = {fb_size * 2, 0};
    struct { uint32_t fd, req; } in = {fd, 0xc0080101};
    r = call(nv, 1, REQ(.data = &in, .size = 8, .a = BUF(create, 8), .b = BUF(create, 8))); uint32_t handle = create[1];
    struct { uint32_t handle, heap, flags, align; uint8_t kind, pad[7]; uint64_t addr; } alloc = {handle, 0, 1, 0x20000, 0, {0}, (uintptr_t)mem};
    in.req = 0xc0200104; call(nv, 1, REQ(.data = &in, .size = 8, .a = BUF(&alloc, sizeof alloc)));
    uint32_t getid[2] = {0, handle}; in.req = 0xc008010e; call(nv, 1, REQ(.data = &in, .size = 8, .a = BUF(getid, 8), .b = BUF(getid, 8)));
    CHECK(getid[0] == handle, "nvmap GetId");
    /* IGraphicBufferProducer, as libnx's native window drives it */
    uint8_t parcel[0x200], reply[0x200];
    #define TOKEN(p, n) do { uint32_t t0 = 0x100, len = 34; memcpy(p + n, &t0, 4); memcpy(p + n + 4, &len, 4); n += 8; memset(p + n, 0, 72); n += 72; } while (0)
    #define TRANSACT(code, words, nwords) do { uint32_t n = 16; TOKEN(parcel, n); memcpy(parcel + n, words, 4 * (nwords)); n += 4 * (nwords); \
        uint32_t ph[4] = {n - 16, 16, 0, n}; memcpy(parcel, ph, 16); struct { int32_t id; uint32_t c, flags; } ti = {1, code, 0}; memset(reply, 0, sizeof reply); \
        r = call(relay, 3, REQ(.data = &ti, .size = 12, .x = BUF(parcel, n), .c = BUF(reply, sizeof reply))); } while (0)
    uint32_t connect[3] = {0, 2, 0}; TRANSACT(10, connect, 3);
    uint32_t ro[4]; memcpy(ro, reply + 16, 16); CHECK(ro[0] == 1280 && ro[1] == 720, "Connect: 1280x720");
    /* the graphic buffer libnx's framebufferCreate sends: GBFR header, then NvGraphicBuffer's ints */
    uint32_t gb[10 + 81]; memset(gb, 0, sizeof gb);
    uint32_t head[10] = {0x47424652, W, H, W, FB_RGBA_8888, 0xb00, 42, 0, 0, 81}; memcpy(gb, head, sizeof head);
    uint32_t *ints = gb + 10; ints[0] = (uint32_t)-1; ints[1] = handle; ints[3] = 0xdaffcaff; ints[4] = 42; ints[7] = FB_RGBA_8888; ints[9] = W; ints[10] = fb_size; ints[11] = 1;
    ints[13] = W; ints[14] = H; ints[15] = 0x0d; ints[17] = 3; ints[18] = pitch; ints[20] = 0; ints[21] = 0xfe; ints[22] = 4;
    uint32_t spb[4 + 91]; spb[0] = 0; spb[1] = 1; spb[2] = sizeof gb; spb[3] = 0; memcpy(spb + 4, gb, sizeof gb);
    TRANSACT(14, spb, 4 + 91);
    ints[20] = fb_size; spb[0] = 1; memcpy(spb + 4, gb, sizeof gb); TRANSACT(14, spb, 4 + 91);
    uint32_t dq[5] = {0, W, H, FB_RGBA_8888, 0xb00}; TRANSACT(3, dq, 5); int32_t slot; memcpy(&slot, reply + 16, 4);
    CHECK(slot == 0 || slot == 1, "DequeueBuffer slot %d", slot);
    /* the game draws: a pattern written in block-linear order straight into the slot's memory */
    uint8_t *lin = malloc((size_t)pitch * H); for (uint32_t i = 0; i < pitch * H; i++) lin[i] = (uint8_t)(i * 7 + i / pitch);
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < pitch; x += 16) {
        memcpy(mem + (size_t)slot * fb_size + nvn_bl_offset(x, y, 0, pitch, H, 4, 0), lin + (size_t)y * pitch + x, 16);
    }
    uint32_t qbuf[1 + 2 + 21]; memset(qbuf, 0, sizeof qbuf); qbuf[0] = (uint32_t)slot; qbuf[1] = 84; qbuf[2] = 0; qbuf[3 + 11] = 1;   /* BqBufferInput.swapInterval */
    TRANSACT(7, qbuf, 3 + 21);
    CHECK(shown && shown_w == W && shown_h == H && !memcmp(shown, lin, (size_t)W * 4) && !memcmp(shown + (size_t)(H - 1) * W * 4, lin + (size_t)(H - 1) * pitch, (size_t)W * 4),
          "QueueBuffer: the window shows the picture the game drew");
}
/* audout:u as libnx's audout drives it (silent real-time sink, BY3_AUDIO=0): two 10 ms buffers appended and the client
   spinning on GetReleasedAudioOutBuffers until both are back, as this game's port does between emulation slices. Both come
   back in order within their play time; how often it asked is printed (the test compares it with
   TOTK_AUDOUT_POLL_WAIT_US=0, the spin unanswered). A single ask after a pause is answered at once. */
static uint64_t probe_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void audio_out(void) {
    uint8_t name[0x100]; struct { uint32_t rate; uint16_t channels, pad; } open = {48000, 2, 0};
    uint32_t ao = object(service("audout:u"), 1, REQ(.data = &open, .size = sizeof open, .b = BUF(name, sizeof name)));
    CHECK(!strcmp((char *)name, "DeviceOut"), "OpenAudioOut names DeviceOut");
    CHECK(!call(ao, 1, REQ()).result, "StartAudioOut");
    static int16_t pcm[2][480 * 2];   /* 10 ms of stereo at 48 kHz each */
    struct { uint64_t next, sample_buffer, capacity, data_size, data_offset; } d[2];
    for (unsigned i = 0; i < 2; i++) {
        d[i] = (typeof(d[i])){0, (uintptr_t)pcm[i], sizeof pcm[i], sizeof pcm[i], 0};
        uint64_t tag = (uintptr_t)&d[i];
        CHECK(!call(ao, 3, REQ(.data = &tag, .size = 8, .a = BUF(&d[i], sizeof d[i]))).result, "AppendAudioOutBuffer %u", i);
    }
    uint64_t got[2] = {0}, out[4]; unsigned n = 0, polls = 0; const uint64_t t0 = probe_now_ns();
    while (n < 2 && probe_now_ns() - t0 < 1000000000ull) {
        Resp r = call(ao, 5, REQ(.b = BUF(out, sizeof out))); polls++;
        uint32_t count; memcpy(&count, r.data, 4);
        for (uint32_t i = 0; i < count && n < 2; i++) got[n++] = out[i];
    }
    const uint64_t took = probe_now_ns() - t0;
    CHECK(n == 2 && got[0] == (uintptr_t)&d[0] && got[1] == (uintptr_t)&d[1], "both buffers released, in order (%u)", n);
    CHECK(took >= 18000000ull && took < 80000000ull, "released after their play time (%llu us)", (unsigned long long)(took / 1000));
    uint64_t tag = (uintptr_t)&d[0];
    call(ao, 3, REQ(.data = &tag, .size = 8, .a = BUF(&d[0], sizeof d[0])));
    struct timespec pause = {0, 2000000}; nanosleep(&pause, NULL);
    uint64_t t1 = probe_now_ns(); Resp r = call(ao, 5, REQ(.b = BUF(out, sizeof out))); uint64_t one = probe_now_ns() - t1;
    uint32_t count; memcpy(&count, r.data, 4);
    CHECK(count == 0 && one < 500000ull, "a single ask after a pause is answered at once (%llu us)", (unsigned long long)(one / 1000));
    printf("audout polls=%u in %llu us\n", polls, (unsigned long long)(took / 1000));
}
int main(void) {
    for (unsigned i = 0; i < 8; i++) pthread_key_create(&runtime_keys[i], NULL);
    pthread_setspecific(runtime_keys[2], msg);
    char profile[] = "/tmp/mj2013-ipc-XXXXXX"; const char *tmp = getenv("PROBE_TMP"); char pbuf[512];
    if (tmp) { snprintf(pbuf, sizeof pbuf, "%s/profile", tmp); mkdir(pbuf, 0755); } else { mkdtemp(profile); snprintf(pbuf, sizeof pbuf, "%s", profile); }
    setenv("TOTK_PROFILE_DIR", pbuf, 1);
    char romfs[512]; snprintf(romfs, sizeof romfs, "%s/romfs", pbuf); mkdir(romfs, 0755);
    char image[600]; snprintf(image, sizeof image, "%s/romfs.bin", romfs); FILE *f = fopen(image, "wb"); for (unsigned i = 0; i < ROMFS_SIZE; i++) fputc((int)(i & 255), f); fclose(f);
    setenv("TOTK_ROMFS", romfs, 1);
    svc_thread_main_init();
    hb_services_init();
    char port[12] = "sm:"; uint64_t r[31] = {0, (uintptr_t)port}; runtime_ipc_svc(0x1f, r); sm = (uint32_t)r[1];
    CHECK(!call(sm, 0, REQ(.pid = 1)).result, "sm RegisterClient");
    system_and_applet(); filesystem(pbuf, romfs); hid(); display(); audio_out();
    printf(fails ? "ipc FAILED\n" : "ipc ok\n"); return fails != 0;
}
