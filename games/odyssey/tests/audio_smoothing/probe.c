/* Exercise the production renderer, including streamed EOS and BOTW's volume-ramp/voice-reuse regressions. */
#include "../../runtime/audio_renderer.c"
#include "../../runtime/audio_mixer.c"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "probe failure: %s\n", message); exit(2); }
void *ipc_guest(uint64_t address, uint64_t size) { (void)size; return (void *)(uintptr_t)address; }
void dm_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }
uint32_t runtime_event_new(void) { return 1; }
uint32_t runtime_event_readable(uint32_t handle) { return handle; }
void runtime_event_signal(uint32_t handle) { (void)handle; }
const void *ipc_arg(const IpcRequest *r, uint32_t at, uint32_t size) { (void)r; (void)at; (void)size; runtime_fail("unexpected IPC"); }
void ipc_out(IpcResponse *r, const void *p, uint32_t size) { (void)r; (void)p; (void)size; runtime_fail("unexpected IPC"); }
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *r) { (void)s; (void)r; runtime_fail("unexpected IPC"); }
uint32_t runtime_ipc_new_session(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    (void)name; (void)handler; (void)state; (void)close; runtime_fail("unexpected session");
}
void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *)) {
    (void)name; (void)handler; (void)open; (void)close;
}
int main(void) { runtime_audio_probe(); return 0; }
