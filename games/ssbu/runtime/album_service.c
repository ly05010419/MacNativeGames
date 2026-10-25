/* nn::capsrv album services for nn::album, as a console whose album holds nothing the game made. Super Smash Bros. Ultimate
   13.0.2 starts its adventure mode (World of Light) with nn::album::Initialize and GetExtraMovieFileList(count, entries, 1)
   (main+0x36e8738); the SDK's LibraryState::EnsureAlbumAvailable aborts unless both capsrv shims initialize: caps:su
   (IScreenShotApplicationService, capsrv::InitializeForApplication) and caps:u (IAlbumApplicationService,
   InitializeAlbumAccessForApplication), each with SetShimLibraryVersion. The extra movie list is GetAlbumFileList3AaeAruid.
   Command ids and argument layouts are read from the SDK's own client proxies (CmifProxyImpl vtables and their
   Hipc2ClientCoreProcessor argument lists, sdk+0xd8770.. / +0xda600..); names follow switchbrew "Capture services".
   Saving, loading, deleting and movie streams still stop the runtime with their id: the album never hands out an entry. */
#include "ipc.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* SetShimLibraryVersion(u64 version, aruid; pid), command 32 on both services. */
static void shim_version(const IpcSession *s, const IpcRequest *rq) {
    uint64_t version = IPC_ARG(rq, uint64_t, 0);
    fprintf(stderr, "[ssbu] album: %s shim library version %llu, an empty album\n", s->name, (unsigned long long)version);
}

/* ---------- caps:su: nn::capsrv::sf::IScreenShotApplicationService ---------- */
static void screenshot_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    switch (rq->cmd) {
    case 32: shim_version(s, rq); return;   /* SetShimLibraryVersion */
    default: ipc_unimplemented(s, rq);      /* 201/203/205/210 SaveScreenShot(Ex0/Ex1/Ex2) */
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
