/* Mac BotW 1.9.0. Feed relative mouse motion to the game's camera actions after stick shaping,
   using the frame ratio that the actions multiply their turn by. The game still owns the camera
   pose, collisions, aiming speeds and transitions.
   Camera sites and axis scaling: https://github.com/Ctrl-Arnav/BreathWindRecomp/blob/main/tools/patches.py */
#include <pthread.h>
#include <stddef.h>

enum {
    MOUSE_CAMERA_PAGE_OFFSET = 0x80,
    MOUSE_CAMERA_PAUSE_STATE = 0x1e1611c,
    MOUSE_FRAME = 0, MOUSE_STICK = 1, MOUSE_CHASE = 4, MOUSE_SURF = 6, MOUSE_WAKEBOARD = 8,
    MOUSE_AIM = 10, MOUSE_ITEM = 12, MOUSE_REVERSE_X = 13, MOUSE_REVERSE_Y = 14,
};
typedef struct {
    _Atomic uint64_t motion;
    _Atomic uint32_t read, reverse_x, reverse_y;
    float pitch_scale;
} BotwMouseCameraPage;
_Static_assert(MOUSE_CAMERA_PAGE_OFFSET >= FOV_GAME_SLOT + sizeof(float) &&
               MOUSE_CAMERA_PAGE_OFFSET >= FOV_FACTOR_SLOT + sizeof(float),
               "mouse camera must have its own space after the FOV fields");
_Static_assert(MOUSE_CAMERA_PAGE_OFFSET + sizeof(BotwMouseCameraPage) <= 0x1000,
               "mouse camera fields must precede the font table");
enum {
    MOUSE_MOTION_SLOT = MOUSE_CAMERA_PAGE_OFFSET + offsetof(BotwMouseCameraPage, motion),
    MOUSE_READ_SLOT = MOUSE_CAMERA_PAGE_OFFSET + offsetof(BotwMouseCameraPage, read),
    MOUSE_REVERSE_X_SLOT = MOUSE_CAMERA_PAGE_OFFSET + offsetof(BotwMouseCameraPage, reverse_x),
    MOUSE_REVERSE_Y_SLOT = MOUSE_CAMERA_PAGE_OFFSET + offsetof(BotwMouseCameraPage, reverse_y),
    MOUSE_PITCH_SLOT = MOUSE_CAMERA_PAGE_OFFSET + offsetof(BotwMouseCameraPage, pitch_scale),
};
static BotwMouseCameraPage *mouse_camera_page;
static uint32_t *mouse_camera_pause;
static _Atomic int mouse_camera_active;
static pthread_mutex_t mouse_camera_lock = PTHREAD_MUTEX_INITIALIZER;
static double mouse_camera_x, mouse_camera_y;

int runtime_botw_mouse_camera_active(void) {
    return atomic_load_explicit(&mouse_camera_active, memory_order_relaxed);
}

int runtime_botw_mouse_camera_move(double dx, double dy, int sensitivity, int invert_y) {
    if (!runtime_botw_mouse_camera_active()) return 0;
    if (!isfinite(dx) || !isfinite(dy)) return 1;
    const double degrees_per_point = 0.09 * sensitivity / 50.0;
    pthread_mutex_lock(&mouse_camera_lock);
    mouse_camera_x += dx * degrees_per_point;
    mouse_camera_y += dy * (invert_y ? degrees_per_point : -degrees_per_point);
    pthread_mutex_unlock(&mouse_camera_lock);
    return 1;
}

void runtime_botw_mouse_camera_reset(void) {
    if (!mouse_camera_page) return;
    pthread_mutex_lock(&mouse_camera_lock);
    mouse_camera_x = mouse_camera_y = 0;
    atomic_store_explicit(&mouse_camera_page->motion, 0, memory_order_release);
    pthread_mutex_unlock(&mouse_camera_lock);
}

static void botw_mouse_camera_frame(float ratio) {
    static uint64_t last_camera_read;
    uint64_t now = runtime_now_ns();
    pthread_mutex_lock(&mouse_camera_lock);
    if (atomic_exchange_explicit(&mouse_camera_page->read, 0, memory_order_relaxed))
        last_camera_read = now;
    /* The map and motion puzzles still use the usual stick and gyro paths. A few frames of
       grace keep the routes stable when a camera action skips reading its input. */
    int active = runtime_mouse_enabled() &&
                 !__atomic_load_n(mouse_camera_pause, __ATOMIC_RELAXED) &&
                 last_camera_read && now - last_camera_read < 80000000;
    float input[2] = {0, 0};
    if (active && (mouse_camera_x || mouse_camera_y)) {
        const double yaw_per_tilt = 1.6 * 1.78;
        const double scale = 1.0 / (yaw_per_tilt * fmaxf(ratio, 0.05f));
        int reverse_x = atomic_load_explicit(&mouse_camera_page->reverse_x, memory_order_relaxed) & 1;
        int reverse_y = atomic_load_explicit(&mouse_camera_page->reverse_y, memory_order_relaxed) & 1;
        if (mouse_camera_x) input[0] = (float)(mouse_camera_x * (reverse_x ? -scale : scale));
        if (mouse_camera_y) input[1] = (float)(mouse_camera_y * (reverse_y ? -scale : scale) * 1.78);
    }
    uint64_t packed;
    memcpy(&packed, input, sizeof packed);
    mouse_camera_x = mouse_camera_y = 0;
    atomic_store_explicit(&mouse_camera_page->motion, packed, memory_order_release);
    atomic_store_explicit(&mouse_camera_active, active, memory_order_relaxed);
    pthread_mutex_unlock(&mouse_camera_lock);
}

static void botw_mouse_camera_install(Module *m) {
    const char *option = getenv("TOTK_MOUSE_DIRECT");
    if (ios_code() || vr_labo_enabled() || (option && !strcmp(option, "0"))) return;
    for (size_t i = 0; i < sizeof mouse_camera_sites / sizeof *mouse_camera_sites; i++) {
        uint32_t off = mouse_camera_sites[i].off;
        if (off + 4 > m->text_end ||
            memcmp(m->base + off, &mouse_camera_sites[i].original, 4)) {
            fprintf(stderr, "[botw] direct mouse camera disabled: unexpected instruction at main+%#x\n", off);
            return;
        }
    }

    /* Branch veneers. Within the camera action, keep x16/x17 and NZCV intact. At the
       stick entry they are caller-saved. Only the VFR veneer calls the host. */
    const uint32_t load_motion = 0xf9400211u | ((MOUSE_MOTION_SLOT / 8) << 10); /* ldr x17,[x16,#motion] */
    uint32_t stick[] = {
        0, 0x52800031u, 0xb9000211u | ((MOUSE_READ_SLOT / 4) << 10), /* adrp; mov w17,#1; str w17,[x16,#read] */
        load_motion, 0xb4000071u,                /* load motion; cbz x17, original */
        0xf9000011u, 0xd65f03c0u,                /* str x17,[x0]; ret */
        0xd100c3ffu, 0,                          /* original: sub sp,sp,#0x30; b resume */
    };
    uint32_t frame[] = {
        0xf81f0ffeu, 0xbd403e60u, 0x580000d0u, 0xd63f0200u, /* save LR; ldr s0,[x19,#0x3c]; ldr/blr host */
        0xf84107feu, 0xb9407a68u, 0, 0xd503201fu,            /* restore LR; ldr w8,[x19,#0x78]; b resume */
        0, 0,                                               /* host wrapper address */
    };
    uint32_t pitch[] = {
        0xa9bf47f0u, 0, load_motion, 0xb40000b1u, /* save x16/x17; adrp; load motion; cbz original */
        0xbd400201u | ((MOUSE_PITCH_SLOT / 4) << 10), /* ldr s1,[x16,#pitch_scale] */
        0x1e210801u, 0x1e204023u, 0x14000002u,   /* fmul s1,s0,s1; fmov s3,s1; b restore */
        0x1e208803u, 0xa8c147f0u, 0,               /* original fnmul; restore x16/x17; b resume */
    };
    uint32_t aim[] = {
        0xa9bf47f0u, 0, load_motion, 0xb4000091u,
        0, 0xbd400221u, 0x14000002u,              /* load the horizontal scale through x19; b restore */
        0xbd400101u, 0xa8c147f0u, 0,              /* original ldr s1,[x8]; restore; b resume */
    };
    uint32_t reverse[] = {
        0xf81f0ffeu, 0, 0,                        /* save LR; bl original getter; adrp x16,page */
        0, 0xf84107feu, 0xd65f03c0u,              /* store w0 into the corresponding axis field; restore LR; ret */
    };
    /* Text padding between the VR/heap veneers and the moving platform veneer at +0x300. */
    const uint64_t start = ((m->text_end + 15) & ~15ULL) + 0x180;
    const uint64_t text_page_end = (m->text_end + 0xfff) & ~0xfffULL;
    const uint64_t data = (m->file_size + 0x3fff) & ~0x3fffULL;
    const size_t size = sizeof frame + sizeof stick + 3 * sizeof pitch + 2 * sizeof aim + 2 * sizeof reverse;
    if (data + 0x4000 != m->size || start + size > text_page_end || MOUSE_CAMERA_PAUSE_STATE + 4 > m->patch_off) {
        fprintf(stderr, "[botw] direct mouse camera disabled: module layout\n");
        return;
    }
    for (size_t i = 0; i < size; i++) {
        if (m->base[start + i]) {
            fprintf(stderr, "[botw] direct mouse camera disabled: veneer padding is occupied\n");
            return;
        }
    }
    mouse_camera_page = (void *)(m->base + data + MOUSE_CAMERA_PAGE_OFFSET);
    mouse_camera_page->pitch_scale = (1.6f * 1.78f) / (2.0892f * 1.78f);
    mouse_camera_pause = (void *)(m->base + MOUSE_CAMERA_PAUSE_STATE);
    uintptr_t host = (uintptr_t)runtime_hle_wrap_named((void *)botw_mouse_camera_frame, "BotW mouse camera frame");
    memcpy(frame + 8, &host, sizeof host);

    uint64_t at = start, site = mouse_camera_sites[MOUSE_FRAME].off;
    frame[6] = branch_to(at + 24, site + 4);
    put_words(m, at, frame, sizeof frame / 4, "mouse camera frame");
    put_word(m, site, branch_to(site, at), "mouse camera frame");
    at += sizeof frame;

    site = mouse_camera_sites[MOUSE_STICK].off;
    stick[0] = adrp_reg(16, at, data);
    stick[8] = branch_to(at + 32, site + 4);
    put_words(m, at, stick, sizeof stick / 4, "mouse camera read");
    put_word(m, site, branch_to(site, at), "mouse camera read");
    at += sizeof stick;

    const unsigned pitch_sites[] = {MOUSE_CHASE, MOUSE_SURF, MOUSE_WAKEBOARD};
    for (unsigned i = 0; i < sizeof pitch_sites / sizeof *pitch_sites; i++) {
        site = mouse_camera_sites[pitch_sites[i]].off;
        pitch[1] = adrp_reg(16, at + 4, data);
        pitch[10] = branch_to(at + 40, site + 4);
        put_words(m, at, pitch, sizeof pitch / 4, "mouse camera pitch");
        put_word(m, site, branch_to(site, at), "mouse camera pitch");
        at += sizeof pitch;
    }
    const unsigned aim_sites[] = {MOUSE_AIM, MOUSE_ITEM};
    for (unsigned i = 0; i < sizeof aim_sites / sizeof *aim_sites; i++) {
        site = mouse_camera_sites[aim_sites[i]].off;
        aim[1] = adrp_reg(16, at + 4, data);
        aim[4] = i ? 0xf9416271u : 0xf9416a71u; /* ldr x17,[x19,#0x2c0 or #0x2d0] */
        aim[9] = branch_to(at + 36, site + 4);
        put_words(m, at, aim, sizeof aim / 4, "mouse aim scale");
        put_word(m, site, branch_to(site, at), "mouse aim scale");
        at += sizeof aim;
    }
    const uint64_t getters[] = {0x0d35af0, 0x0d35b10};
    for (unsigned i = 0; i < 2; i++) {
        site = mouse_camera_sites[MOUSE_REVERSE_X + i].off;
        reverse[1] = branch_to(at + 4, getters[i]) | 0x80000000u;
        reverse[2] = adrp_reg(16, at + 8, data);
        reverse[3] = 0xb9000200u | (((i ? MOUSE_REVERSE_Y_SLOT : MOUSE_REVERSE_X_SLOT) / 4) << 10);
        put_words(m, at, reverse, sizeof reverse / 4, "mouse camera inversion");
        put_word(m, site, branch_to(site, at) | 0x80000000u, "mouse camera inversion");
        at += sizeof reverse;
    }
    fprintf(stderr, "[botw] direct mouse camera installed (TOTK_MOUSE_DIRECT=0: stick input)\n");
}
