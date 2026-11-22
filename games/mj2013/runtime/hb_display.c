/* The screen of a libnx homebrew: vi (display, layer, vsync), the binder relay with Android's IGraphicBufferProducer
   (libnx's native window: connect, the preallocated buffers, dequeue / queue), and nvdrv's /dev/nvmap and
   /dev/nvhost-ctrl, which tie a preallocated buffer's nvmap id to the game's memory. A queued buffer is the finished
   software-rendered picture: it is converted (fb_convert.c) and handed to the window -- no GPU commands exist here.
   Protocols: switchbrew "Display services" and "NV services"; libnx vi.c, binder.c, buffer_producer.c, native_window.c,
   framebuffer.c, nvmap.c. */
#include "hb.h"
#include "fb_convert.h"
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define SLOTS 8
#define DISPLAY_W 1280
#define DISPLAY_H 720
#define BINDER_ID 1
typedef struct { int set; uint32_t width, height, stride_px, format, nvmap_id, offset, pitch, block_height_log2, layout; } Slot;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[SLOTS];
static int last_queued = -1;
static uint64_t last_present_ns;
static uint32_t frames;

/* ---------- nvmap: handles (= ids) to the game's memory ---------- */
typedef struct { uint32_t size; uint64_t addr; } NvMap;
#define NVMAPS 256
static NvMap nvmaps[NVMAPS]; static unsigned nnvmaps;   /* handle i+1 */
static uint64_t nvmap_addr(uint32_t id, uint32_t *size) { if (!id || id > nnvmaps) return 0; if (size) *size = nvmaps[id - 1].size; return nvmaps[id - 1].addr; }

/* ---------- screenshots (TOTK_INPUT_FILE "shot <path>", dm_gfx_shot_at) ---------- */
#define SHOTS 64
static struct { long frame; char path[1024]; } shots[SHOTS]; static unsigned nshots;
void dm_gfx_shot_at(long frames_ahead, const char *path) {
    pthread_mutex_lock(&lock);
    if (nshots < SHOTS) { shots[nshots].frame = frames + frames_ahead; snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", path); nshots++; }
    pthread_mutex_unlock(&lock);
}
int hb_write_png(const char *path, const uint8_t *rgba, unsigned w, unsigned h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)rgba, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; int ok = 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url && img ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (url) CFRelease(url); if (img) CGImageRelease(img); if (ctx) CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return ok ? 0 : -1;
}
static uint8_t *picture;   /* the last presented frame, RGBA */
static unsigned picture_w, picture_h;
static void present(int slot) {
    Slot s = slots[slot]; uint32_t size = 0; uint64_t base = nvmap_addr(s.nvmap_id, &size);
    if (!base || (uint64_t)s.offset + (uint64_t)s.pitch * s.height > size) { HB_LOG("display: slot %d has no memory (nvmap %u)", slot, s.nvmap_id); return; }
    if (!picture || picture_w != s.width || picture_h != s.height) { free(picture); picture = malloc((size_t)s.width * s.height * 4); picture_w = s.width; picture_h = s.height; }
    if (!picture) runtime_fail("frame buffer");
    if (fb_to_rgba((const uint8_t *)(uintptr_t)(base + s.offset), s.width, s.height, s.pitch, s.layout == 3, s.block_height_log2, s.format, picture)) {
        HB_LOG("display: pixel format %u not supported", s.format); runtime_fail("framebuffer format");
    }
    runtime_front_present((const uint32_t *)picture, s.width, s.height);
    pthread_mutex_lock(&lock);
    frames++;
    for (unsigned i = 0; i < nshots;) {
        if (shots[i].frame <= (long)frames) {
            int r = hb_write_png(shots[i].path, picture, s.width, s.height);
            fprintf(stderr, "[totk-input] screenshot %s %s (frame %u)\n", shots[i].path, r ? "FAILED" : "written", frames);
            shots[i] = shots[--nshots];
        } else i++;
    }
    pthread_mutex_unlock(&lock);
    static uint64_t window_start; static uint32_t window_frames; uint64_t now = runtime_now_ns();
    if (!window_start) window_start = now;
    if (++window_frames, now - window_start >= 5000000000ull || frames == 1) {   /* the frame rate, every 5 s */
        HB_LOG("display: frame %u (%ux%u format %u), %.1f fps", frames, s.width, s.height, s.format, window_frames * 1e9 / (double)(now - window_start + 1));
        window_start = now; window_frames = 0;
    }
}

/* ---------- parcels (Android's Parcel as libnx writes it) ---------- */
typedef struct { const uint8_t *p; uint32_t size, pos; } ParcelIn;
typedef struct { uint8_t p[0x400]; uint32_t size; } ParcelOut;
static uint32_t pin_u32(ParcelIn *in) { uint32_t v = 0; if (in->pos + 4 <= in->size) memcpy(&v, in->p + in->pos, 4); in->pos += 4; return v; }
static const uint8_t *pin_data(ParcelIn *in, uint32_t n) { const uint8_t *d = in->pos + n <= in->size ? in->p + in->pos : NULL; in->pos += (n + 3) & ~3u; return d; }
static void pin_token(ParcelIn *in) { pin_u32(in); uint32_t len = pin_u32(in); pin_data(in, (len + 1) * 2); }   /* strict mode, String16 */
static const uint8_t *pin_flat(ParcelIn *in, uint32_t *len) { *len = pin_u32(in); pin_u32(in); return pin_data(in, *len); }
static void pout(ParcelOut *o, const void *d, uint32_t n) { if (o->size + ((n + 3) & ~3u) > sizeof o->p) runtime_fail("parcel reply too large"); memcpy(o->p + o->size, d, n); o->size += (n + 3) & ~3u; }
static void pout_u32(ParcelOut *o, uint32_t v) { pout(o, &v, 4); }
/* The reply as libnx's parcelTransact reads it: a 16-byte header, then the payload. */
static void parcel_reply(const IpcRequest *rq, const ParcelOut *o) {
    uint8_t buf[0x410]; uint32_t h[4] = {o->size, 0x10, 0, 0x10 + o->size};
    memcpy(buf, h, 16); memcpy(buf + 16, o->p, o->size);
    hb_write_out(rq, 0, buf, 16 + o->size);
}
static void slot_from_buffer(int slot, const uint8_t *d, uint32_t len) {
    /* {magic "GBFR", width, height, stride, format, usage, pid, refcount, num_fds, num_ints, ints[]}; ints are libnx's
       NvGraphicBuffer after its NativeHandle header: [1] nvmap id, [3] 0xdaffcaff, [9] stride in pixels, then plane 0 at
       ints[13] (no padding before it: Horizon's layout is packed): width, height, color format (2 words), layout, pitch,
       unused, offset, kind, block height log2. */
    uint32_t w[64] = {0}; memcpy(w, d, len < sizeof w ? len : sizeof w);
    const uint32_t *ints = w + 10;
    if (w[0] != 0x47424652 || w[8] != 0 || w[9] < 24 || ints[3] != 0xdaffcaffu) { HB_LOG("display: unexpected graphic buffer (magic %#x ints %u)", w[0], w[9]); runtime_fail("graphic buffer layout"); }
    Slot s = {1, ints[13], ints[14], ints[9], w[4], ints[1], ints[20], ints[18], ints[22], ints[17]};
    pthread_mutex_lock(&lock); slots[slot] = s; pthread_mutex_unlock(&lock);
    HB_LOG("display: slot %d = %ux%u format %u pitch %u layout %u block height 2^%u nvmap %u offset %#x", slot, s.width, s.height, s.format, s.pitch, s.layout, s.block_height_log2, s.nvmap_id, s.offset);
}
static void wait_frame(uint32_t swap_interval) {   /* the display's 60 Hz: a queued picture is shown at the next vsync */
    uint64_t period = 16666667ull * (swap_interval ? swap_interval : 1), now = runtime_now_ns();
    if (last_present_ns && now < last_present_ns + period) { uint64_t d = last_present_ns + period - now; struct timespec t = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)}; nanosleep(&t, NULL); }
    now = runtime_now_ns(); last_present_ns = (last_present_ns && now - last_present_ns < 2 * period) ? last_present_ns + period : now;
}
enum { REQUEST_BUFFER = 1, SET_BUFFER_COUNT, DEQUEUE_BUFFER, DETACH_BUFFER, DETACH_NEXT_BUFFER, ATTACH_BUFFER, QUEUE_BUFFER, CANCEL_BUFFER, QUERY,
       CONNECT, DISCONNECT, SET_SIDEBAND_STREAM, ALLOCATE_BUFFERS, SET_PREALLOCATED_BUFFER };
static void transact(const IpcRequest *rq, uint32_t code) {
    const IpcBuffer *b = hb_in_buffer(rq, 0); if (!b || b->size < 16) runtime_fail("binder parcel");
    const uint8_t *raw = ipc_guest(b->addr, b->size); uint32_t h[4]; memcpy(h, raw, 16);
    if (h[1] > b->size || h[0] > b->size - h[1]) runtime_fail("binder parcel bounds");
    ParcelIn in = {raw + h[1], h[0], 0}; ParcelOut out = {{0}, 0};
    pin_token(&in);
    static int trace = -1; if (trace < 0) trace = getenv("HB_TRACE_IPC") != NULL;
    if (trace) HB_LOG("display: binder transaction %u", code);
    switch (code) {
    case CONNECT: { uint32_t o[4] = {DISPLAY_W, DISPLAY_H, 0, 0}; pout(&out, o, sizeof o); pout_u32(&out, 0); break; }
    case DISCONNECT: pout_u32(&out, 0); break;
    case SET_PREALLOCATED_BUFFER: {
        int32_t slot = (int32_t)pin_u32(&in); uint32_t has = pin_u32(&in);
        if (slot < 0 || slot >= SLOTS) runtime_fail("graphic buffer slot");
        if (has) { uint32_t len; const uint8_t *d = pin_flat(&in, &len); if (!d) runtime_fail("graphic buffer data"); slot_from_buffer(slot, d, len); }
        else { pthread_mutex_lock(&lock); slots[slot].set = 0; pthread_mutex_unlock(&lock); }
        break;
    }
    case DEQUEUE_BUFFER: {   /* the slot after the one queued last; no fence (the picture is the CPU's) */
        pthread_mutex_lock(&lock);
        int slot = -1; for (int i = 1; i <= SLOTS && slot < 0; i++) { int k = (last_queued + i) % SLOTS; if (k >= 0 && slots[k].set) slot = k; }
        pthread_mutex_unlock(&lock);
        if (slot < 0) runtime_fail("dequeue without buffers");
        uint32_t fence[9] = {0};   /* NvMultiFence: num_fences 0 */
        pout_u32(&out, (uint32_t)slot); pout_u32(&out, 1); pout_u32(&out, sizeof fence); pout_u32(&out, 0); pout(&out, fence, sizeof fence); pout_u32(&out, 0);
        break;
    }
    case REQUEST_BUFFER: pin_u32(&in); pout_u32(&out, 0); pout_u32(&out, 0); break;
    case QUEUE_BUFFER: {
        int32_t slot = (int32_t)pin_u32(&in); uint32_t len; const uint8_t *d = pin_flat(&in, &len);
        uint32_t swap = 1; if (d && len >= 0x30) memcpy(&swap, d + 0x2c, 4);   /* BqBufferInput.swapInterval */
        if (slot < 0 || slot >= SLOTS || !slots[slot].set) runtime_fail("queue of an unknown slot");
        present(slot); last_queued = slot; wait_frame(swap);
        uint32_t o[4] = {DISPLAY_W, DISPLAY_H, 0, 0}; pout(&out, o, sizeof o); pout_u32(&out, 0);
        break;
    }
    case CANCEL_BUFFER: break;
    case QUERY: { uint32_t what = pin_u32(&in); uint32_t v = what == 0 ? DISPLAY_W : what == 1 ? DISPLAY_H : what == 2 ? 1 : 0; pout_u32(&out, v); pout_u32(&out, 0); break; }
    default: fprintf(stderr, "[hb] display: IGraphicBufferProducer transaction %u\n", code); runtime_fail("binder transaction not implemented");
    }
    parcel_reply(rq, &out);
}
static uint32_t buffer_event;   /* the producer's "buffer available" event: always signalled (dequeue paces itself) */
static void binder_relay(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 3: {                                            /* TransactParcel(Auto)(s32 id, u32 code, u32 flags) */
        int32_t id = IPC_ARG(rq, int32_t, 0); uint32_t code = IPC_ARG(rq, uint32_t, 4);
        if (id != BINDER_ID) runtime_fail("unknown binder id");
        transact(rq, code); return;
    }
    case 1: return;                                              /* AdjustRefcount */
    case 2: {                                                    /* GetNativeHandle -> event */
        if (!buffer_event) { hb_event(&buffer_event); runtime_event_signal(buffer_event); }
        hb_copy(rs, runtime_event_readable(buffer_event)); return;
    }
    default: HB_UNKNOWN(s, rq);
    }
}

/* ---------- vi ---------- */
#define VSYNCS 8
static uint32_t vsync_events[VSYNCS]; static unsigned nvsync;
static void *vsync_thread(void *unused) {
    (void)unused; uint64_t next = runtime_now_ns();
    for (;;) {
        next += 16666667ull; uint64_t now = runtime_now_ns();
        if (next > now) { uint64_t d = next - now; struct timespec t = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)}; nanosleep(&t, NULL); } else next = now;
        pthread_mutex_lock(&lock); unsigned n = nvsync; uint32_t ev[VSYNCS]; memcpy(ev, vsync_events, sizeof ev); pthread_mutex_unlock(&lock);
        for (unsigned i = 0; i < n; i++) runtime_event_signal(ev[i]);
    }
    return NULL;
}
static void native_window(const IpcRequest *rq, IpcResponse *rs) {
    uint8_t parcel[0x100] = {0}; uint32_t h[4] = {0x28, 0x10, 0, 0x38}, payload[10] = {2, 0, BINDER_ID, 0, 0, 0, 0x6e766864 /* "dhvn" */, 0x7265766e, 0, 0};
    memcpy(parcel, h, 16); memcpy(parcel + 16, payload, sizeof payload);
    hb_write_out(rq, 0, parcel, sizeof parcel); HB_OUT_U64(rs, 0x38);
}
static void display_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
static void empty_display(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {   /* ISystemDisplayService / IManagerDisplayService */
    switch (rq->cmd) {
    case 2010: HB_OUT_U64(rs, 1); return;                        /* CreateManagedLayer -> layer id */
    case 2011: case 2205: case 2207: case 2312: case 6000: case 6002: return;   /* DestroyManagedLayer, SetLayerZ, SetLayerVisibility, AddToLayerStack ... */
    default: HB_UNKNOWN(s, rq);
    }
}
static void display_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 100: case 103: hb_session(rs, "IHOSBinderDriver", binder_relay, NULL, NULL); return;   /* GetRelayService / GetIndirectDisplayTransactionService */
    case 101: hb_session(rs, "ISystemDisplayService", empty_display, NULL, NULL); return;
    case 102: hb_session(rs, "IManagerDisplayService", empty_display, NULL, NULL); return;
    case 1010: HB_OUT_U64(rs, 1); return;                        /* OpenDisplay(name) -> display id */
    case 1020: return;                                           /* CloseDisplay */
    case 1102: { uint64_t wh[2] = {DISPLAY_W, DISPLAY_H}; IPC_OUT(rs, wh); return; }   /* GetDisplayResolution */
    case 2020: case 2030: native_window(rq, rs); if (rq->cmd == 2030) { uint64_t layer = 1; memmove(rs->data + 8, rs->data, 8); memcpy(rs->data, &layer, 8); rs->data_size = 16; } return;   /* OpenLayer / CreateStrayLayer */
    case 2021: case 2031: return;                                /* CloseLayer / DestroyStrayLayer */
    case 2101: return;                                           /* SetLayerScalingMode */
    case 5202: {                                                 /* GetDisplayVsyncEvent */
        uint32_t w; uint32_t r = hb_event(&w);
        pthread_mutex_lock(&lock); if (nvsync < VSYNCS) vsync_events[nvsync++] = w; int first = nvsync == 1; pthread_mutex_unlock(&lock);
        if (first) { pthread_t t; if (pthread_create(&t, NULL, vsync_thread, NULL)) runtime_fail("vsync thread"); pthread_detach(t); }
        hb_copy(rs, r); return;
    }
    default: HB_UNKNOWN(s, rq);
    }
}
static void vi_root(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 1: case 2: hb_session(rs, "IApplicationDisplayService", display_service, NULL, NULL); return;   /* GetDisplayService */
    default: HB_UNKNOWN(s, rq);
    }
}

/* ---------- nvdrv ---------- */
enum { FD_NVMAP = 1, FD_CTRL = 2, FD_CTRL_GPU = 3, FD_AS_GPU = 4 };
static uint32_t nvmap_ioctl(uint32_t nr, uint8_t *d) {
    uint32_t v[8]; memcpy(v, d, sizeof v);
    pthread_mutex_lock(&lock); uint32_t r = 0;
    switch (nr) {
    case 0x01: if (nnvmaps == NVMAPS) r = 10; else { nvmaps[nnvmaps] = (NvMap){v[0], 0}; v[1] = ++nnvmaps; } break;   /* Create(size) -> handle */
    case 0x03: if (!v[0] || v[0] > nnvmaps) r = 4; else v[1] = v[0]; break;                                       /* FromId: ids are handles */
    case 0x04: { uint64_t addr; memcpy(&addr, d + 24, 8); if (!v[0] || v[0] > nnvmaps) r = 4; else nvmaps[v[0] - 1].addr = addr; break; }   /* Alloc(handle, heap, flags, align, kind, addr) */
    case 0x05: if (!v[0] || v[0] > nnvmaps) r = 4; else { uint64_t refs = 0; memcpy(d + 8, &refs, 8); v[4] = nvmaps[v[0] - 1].size; v[5] = 0; nvmaps[v[0] - 1].addr = 0; memcpy(d + 16, &v[4], 8); pthread_mutex_unlock(&lock); return 0; } break;   /* Free */
    case 0x09: if (!v[0] || v[0] > nnvmaps) r = 4; else v[2] = v[1] == 1 ? nvmaps[v[0] - 1].size : v[1] == 2 ? 0x1000 : 0; break;   /* Param(handle, param) */
    case 0x0e: if (!v[1] || v[1] > nnvmaps) r = 4; else v[0] = v[1]; break;                                       /* GetId(handle) -> id */
    default: HB_LOG("nvmap ioctl %#x", nr); r = 1;
    }
    pthread_mutex_unlock(&lock);
    if (nr != 0x04) memcpy(d, v, nr == 0x09 ? 12 : 8);
    return r;
}
static void nvdrv(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {                                                    /* Open(path) -> fd, error */
        char path[64]; hb_in_string(rq, 0, path, sizeof path);
        uint32_t fd = !strcmp(path, "/dev/nvmap") ? FD_NVMAP : !strcmp(path, "/dev/nvhost-ctrl") ? FD_CTRL : !strcmp(path, "/dev/nvhost-ctrl-gpu") ? FD_CTRL_GPU : !strcmp(path, "/dev/nvhost-as-gpu") ? FD_AS_GPU : 0;
        if (!fd) { HB_LOG("nvdrv: open %s", path); runtime_fail("nvdrv device not implemented"); }
        uint32_t out[2] = {fd, 0}; IPC_OUT(rs, out); return;
    }
    case 1: {                                                    /* Ioctl(fd, request) -> error */
        uint32_t fd = IPC_ARG(rq, uint32_t, 0), request = IPC_ARG(rq, uint32_t, 4), size = (request >> 16) & 0x3fff, nr = request & 0xff;
        uint8_t d[0x100] = {0}; if (size > sizeof d) runtime_fail("nvdrv ioctl size");
        const IpcBuffer *in = hb_in_buffer(rq, 0); if (in) memcpy(d, ipc_guest(in->addr, in->size < size ? in->size : size), in->size < size ? in->size : size);
        uint32_t error = 0;
        if (fd == FD_NVMAP && ((request >> 8) & 0xff) == 0x01) error = nvmap_ioctl(nr, d);
        else if (fd == FD_CTRL && nr == 0x14) { memset(d + 4, 0, 4); }                       /* SyncptRead: 0 */
        else if (fd == FD_CTRL && (nr == 0x1f || nr == 0x20 || nr == 0x15)) {}               /* EventRegister / EventUnregister / SyncptIncr */
        else { HB_LOG("nvdrv: ioctl fd %u request %#x", fd, request); runtime_fail("nvdrv ioctl not implemented"); }
        hb_write_out(rq, 0, d, size); HB_OUT_U32(rs, error); return;
    }
    case 2: HB_OUT_U32(rs, 0); return;                           /* Close */
    case 3: HB_OUT_U32(rs, 0); return;                           /* Initialize(tmem) */
    case 4: hb_copy(rs, hb_event(NULL)); HB_OUT_U32(rs, 0); return;   /* QueryEvent */
    case 8: HB_OUT_U32(rs, 0); return;                           /* SetAruid */
    case 13: return;                                             /* SetGraphicsFirmwareMemoryMarginEnabled */
    default: HB_UNKNOWN(s, rq);
    }
}
void hb_display_register(void) {
    runtime_ipc_register("vi:m", vi_root, NULL, NULL);
    runtime_ipc_register("vi:s", vi_root, NULL, NULL);
    runtime_ipc_register("vi:u", vi_root, NULL, NULL);
    runtime_ipc_register("nvdrv", nvdrv, NULL, NULL);
}
/* The capture button (TOTK_INPUT_FILE "capture"): the next frame as a PNG in the profile's screenshots folder. */
void runtime_capture_press(void) {
    char path[1200]; snprintf(path, sizeof path, "%s/screenshots", runtime_profile_dir()); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/screenshots/mj2013-%llu.png", runtime_profile_dir(), (unsigned long long)time(NULL)); dm_gfx_shot_at(1, path);
}
