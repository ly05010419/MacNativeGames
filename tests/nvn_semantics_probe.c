/* NVN semantics a game can observe, checked on the graphics layer's own hle_nvn.c (tests/test_nvn_semantics.py builds it
   once per game configuration and stubs every symbol outside the file, so no Metal device, runtime or game data is used).

   Command memory: games size GPU memory and heaps by nvnCommandBufferGetCommandMemoryUsed, so it must report what the
   original driver's commands take. Reference values are the driver's own (Ryujinx, TOTK 1.0.0, NVN 55; #1742):
   - a uniform buffer bind takes 20 bytes (CB_SIZE/ADDRESS 4 words + CB_BIND 1 word);
   - an empty recording takes nothing (no BEGIN / END in command memory);
   - CallCommands takes nothing (the driver puts the call in control memory);
   - CopyCommands takes the copied commands' size.

   Output: one line per check, "ok <name>" or "FAIL <name>: got <n> want <m>", then "done <failures>". */
#include NVN_HLE_SOURCE

static int g_fail;
static void expect(const char *name, size_t got, size_t want) {
    if (got == want) printf("ok %s\n", name);
    else { printf("FAIL %s: got %zu want %zu\n", name, got, want); g_fail++; }
}

static NVNcommandBuffer g_cb;
/* Each command buffer has its own control memory: adding control memory again retires the handles recorded in it. */
static uint8_t g_ctl[4][1 << 16];
static int g_nctl;

static void fresh(NVNcommandBuffer *c) {
    h_nvnCommandBufferInitialize(c, NULL);
    h_nvnCommandBufferAddCommandMemory(c, NULL, 0x100000, 1 << 16);
    if (g_nctl >= 4) { printf("FAIL too many command buffers\n"); exit(1); }
    h_nvnCommandBufferAddControlMemory(c, g_ctl[g_nctl], sizeof g_ctl[0]); g_nctl++;
}
static size_t used(NVNcommandBuffer *c) { return h_nvnCommandBufferGetCommandMemoryUsed(c); }

/* A recording of two uniform buffer binds, as TOTK records at start (main+0x219a770). */
static NVNcommandHandle two_ubo(NVNcommandBuffer *c, size_t *bytes) {
    size_t u0 = used(c);
    h_nvnCommandBufferBeginRecording(c);
    h_nvnCommandBufferBindUniformBuffer(c, NVN_SHADER_STAGE_VERTEX, 0, 0x1000, 0x100);
    h_nvnCommandBufferBindUniformBuffer(c, NVN_SHADER_STAGE_FRAGMENT, 1, 0x2000, 0x100);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(c);
    *bytes = used(c) - u0;
    return h;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fresh(&g_cb);
    size_t u0 = used(&g_cb);
    h_nvnCommandBufferBeginRecording(&g_cb);
    h_nvnCommandBufferEndRecording(&g_cb);
    expect("empty recording takes no command memory", used(&g_cb) - u0, 0);

    size_t body;
    NVNcommandHandle h = two_ubo(&g_cb, &body);
    expect("two uniform buffer binds take 2 x 20 bytes", body, 40);

    NVNcommandBuffer caller; fresh(&caller);
    u0 = used(&caller);
    h_nvnCommandBufferBeginRecording(&caller);
    h_nvnCommandBufferCallCommands(&caller, 1, &h);
    h_nvnCommandBufferEndRecording(&caller);
    expect("CallCommands takes no command memory", used(&caller) - u0, 0);

    u0 = used(&caller);
    h_nvnCommandBufferBeginRecording(&caller);
    h_nvnCommandBufferCopyCommands(&caller, 1, &h);
    h_nvnCommandBufferEndRecording(&caller);
    expect("CopyCommands takes the copied commands' size", used(&caller) - u0, 40);

    u0 = used(&caller);
    h_nvnCommandBufferBeginRecording(&caller);
    h_nvnCommandBufferBindUniformBuffer(&caller, NVN_SHADER_STAGE_VERTEX, 2, 0x3000, 0x100);
    h_nvnCommandBufferCallCommands(&caller, 1, &h);
    h_nvnCommandBufferCopyCommands(&caller, 1, &h);
    h_nvnCommandBufferEndRecording(&caller);
    expect("bind + call + copy take 20 + 0 + 40", used(&caller) - u0, 60);

    printf("done %d\n", g_fail);
    return g_fail != 0;
}
