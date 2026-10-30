/* nn::capsrv album services for nn::album, as a console whose album holds nothing the game made. Super Smash Bros. Ultimate
   13.0.2 starts its adventure mode (World of Light) with nn::album::Initialize and GetExtraMovieFileList(count, entries, 1)
   (main+0x36e8738); the SDK's LibraryState::EnsureAlbumAvailable aborts unless both capsrv shims initialize: caps:su
   (IScreenShotApplicationService, capsrv::InitializeForApplication) and caps:u (IAlbumApplicationService,
   InitializeAlbumAccessForApplication), each with SetShimLibraryVersion. The extra movie list is GetAlbumFileList3AaeAruid.
   Command ids and argument layouts are read from the SDK's own client proxies (CmifProxyImpl vtables and their
   Hipc2ClientCoreProcessor argument lists, sdk+0xd8770.. / +0xda600..); names follow switchbrew "Capture services".
   Saving a photo is answered as saved and the picture written as a PNG into a Mac folder (Q56, below); loading, deleting
   and movie streams still stop the runtime with their id: the album never hands out an entry. */
#include "ipc.h"
#include "runtime.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* SetShimLibraryVersion(u64 version, aruid; pid), command 32 on both services. */
static void shim_version(const IpcSession *s, const IpcRequest *rq) {
    uint64_t version = IPC_ARG(rq, uint64_t, 0);
    fprintf(stderr, "[ssbu] album: %s shim library version %llu, an empty album\n", s->name, (unsigned long long)version);
}

/* ---------- caps:su: nn::capsrv::sf::IScreenShotApplicationService ----------
   Q56 (2026-10-25): the player took a photo in a fight and Smash stopped at "caps:su: command 205 not implemented (type 6,
   data 92 bytes, x0 a2 b0 w0 c0)": SaveScreenShotEx1, {ScreenShotAttribute 0x40, aruid; pid} with the application data
   and the picture as two A buffers, answered with an ApplicationAlbumEntry (0x20 bytes). The player's wish: no crash and
   no photo. So 201/203/205/210 (SaveScreenShot, Ex0, Ex1, Ex2, the same answer) succeed with an empty entry and the
   picture is dropped; the album lists stay empty (caps:u below). A made-up error result instead could stop the game in
   the SDK, which aborts on results it does not expect.
   Then (the player: "nice wäre wenn die fotos gespeichert werden in einem custom ordner oder default ordner"): the picture,
   the last A buffer, 1280x720 RGBA (3,686,400 bytes, measured), is written as a PNG to SSBU_PHOTO_DIR (the app's Photos
   menu; the launcher reads <data>/photo-folder.txt) or to ~/Pictures/Super Smash Bros. Ultimate. It is copied and written
   on a background queue, so the game's thread does not wait for the file; the game is told "saved" either way. */
#define PHOTO_W 1280
#define PHOTO_H 720
static pthread_mutex_t photo_dir_lock = PTHREAD_MUTEX_INITIALIZER;
static char photo_dir_chosen[1024]; static int photo_dir_set;
/* The Photos menu (runtime/frontend.m): a folder chosen while playing applies to the next photo. */
void runtime_album_set_photo_dir(const char *dir) {
    pthread_mutex_lock(&photo_dir_lock);
    snprintf(photo_dir_chosen, sizeof photo_dir_chosen, "%s", dir ? dir : ""); photo_dir_set = 1;
    pthread_mutex_unlock(&photo_dir_lock);
}
/* The folder for the next photo: the menu's choice, else SSBU_PHOTO_DIR from the launcher, else ~/Pictures/<game>. */
static void photo_dir(char *out, size_t n) {
    pthread_mutex_lock(&photo_dir_lock);
    const char *chosen = photo_dir_set ? photo_dir_chosen : getenv("SSBU_PHOTO_DIR"), *home = getenv("HOME");
    if (chosen && *chosen) snprintf(out, n, "%s", chosen);
    else snprintf(out, n, "%s/Pictures/Super Smash Bros. Ultimate", home ? home : "/tmp");
    pthread_mutex_unlock(&photo_dir_lock);
}
static int make_dirs(const char *path) {
    char p[1024]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
static void photo_write(uint8_t *rgba, char *dir) {
    char path[1200];
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &t);
    for (int n = 1; n < 100; n++) {   /* two photos in one second get " 2", " 3", ... */
        if (n == 1) snprintf(path, sizeof path, "%s/Super Smash Bros. Ultimate %s.png", dir, stamp);
        else snprintf(path, sizeof path, "%s/Super Smash Bros. Ultimate %s %d.png", dir, stamp, n);
        if (access(path, F_OK) != 0) break;
    }
    int ok = 0;
    if (make_dirs(dir) == 0) {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef data = CGDataProviderCreateWithData(NULL, rgba, PHOTO_W * PHOTO_H * 4, NULL);
        CGImageRef img = CGImageCreate(PHOTO_W, PHOTO_H, 8, 32, PHOTO_W * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast,
                                       data, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dst = url && img ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
        if (url) CFRelease(url);
        CGImageRelease(img); CGDataProviderRelease(data); CGColorSpaceRelease(cs);
    }
    if (ok) fprintf(stderr, "[ssbu] album: photo saved to %s\n", path);
    else fprintf(stderr, "【红】[ssbu] album: the photo could not be written to %s (folder %s)\n", path, dir);
    free(rgba); free(dir);
}
static void screenshot_save(const IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static _Atomic unsigned said;
    const IpcBuffer *pic = rq->na ? &rq->a[rq->na - 1] : NULL;
    uint64_t picture = pic ? pic->size : 0;
    if (picture == (uint64_t)PHOTO_W * PHOTO_H * 4 && pic->addr) {
        char dir[1024]; photo_dir(dir, sizeof dir);
        uint8_t *copy = malloc(picture); char *where = strdup(dir);
        if (copy && where) {
            memcpy(copy, ipc_guest(pic->addr, picture), picture);
            static dispatch_queue_t q; static dispatch_once_t once;
            dispatch_once(&once, ^{ q = dispatch_queue_create("ssbu.photos", DISPATCH_QUEUE_SERIAL); });
            dispatch_async(q, ^{ photo_write(copy, where); });
        } else { free(copy); free(where); }
    } else if (said++ < 8)
        fprintf(stderr, "[ssbu] album: the game saves a photo (%s command %u, %llu-byte picture, not %dx%d RGBA): not kept, answered as saved\n",
                s->name, rq->cmd, (unsigned long long)picture, PHOTO_W, PHOTO_H);
    uint8_t entry[0x20] = {0};   /* ApplicationAlbumEntry */
    ipc_out(rs, entry, sizeof entry);
}
static void screenshot_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 32: shim_version(s, rq); return;                                   /* SetShimLibraryVersion */
    case 201: case 203: case 205: case 210: screenshot_save(s, rq, rs); return;   /* SaveScreenShot (Ex0/Ex1/Ex2) */
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- caps:u: nn::capsrv::sf::IAlbumApplicationService ----------
   The file lists answer {u64 count} with an out-array B buffer (ApplicationAlbumFileEntry 0x30 bytes for 102/140/141,
   ApplicationAlbumEntry 0x20 bytes for 142/143), from {u8 contents (0 screenshot, 1 movie, 2 extra screenshot, 3 extra
   movie), range, [Uid], aruid; pid}: 102 takes the range as two s64 at 8/16, 140-143 as two AlbumFileDateTime at 2/10,
   141/143 the Uid at 24. Nothing is saved on this host, so every list is empty and the array is left as it is. */
static void album_list(const IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    uint8_t contents = IPC_ARG(rq, uint8_t, 0);
    if (contents > 3) { fprintf(stderr, "[ssbu] album: %s command %u, contents %u\n", s->name, rq->cmd, contents); runtime_fail("album list: unknown contents type"); }
    static const char *const kinds[] = {"screenshots", "movies", "extra screenshots", "extra movies"};
    static _Atomic unsigned said;
    if (said++ < 8) fprintf(stderr, "[ssbu] album: list of %s (command %u): none\n", kinds[contents], rq->cmd);
    uint64_t count = 0; IPC_OUT(rs, count);
}
static void album_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 32: shim_version(s, rq); return;                                   /* SetShimLibraryVersion */
    case 102: case 140: case 141: case 142: case 143: album_list(s, rq, rs); return;
    default: ipc_unimplemented(s, rq);   /* 103-105 delete/size, 110/120 load, 130 precheck, 60002 movie accessor */
    }
}

__attribute__((constructor)) static void register_album(void) {
    runtime_ipc_register("caps:su", screenshot_handler, NULL, NULL);
    runtime_ipc_register("caps:u", album_handler, NULL, NULL);
}
