/* PLA: the SDK's audio-device client, answered by the host. The game asks for the active output device, its channel count
   and the device-change notification before it opens its AudioOut (main imports exactly these four plus the AudioOut
   family); the reference host answers them directly instead of running the SDK's client. Until they were answered here the
   game's sound engine stopped after the queries and never opened an output at all (no audout:u session, 2026-11-01).
   The audren IPC device service in audio_mixer.c stays for paths that go through the service. */
#include "runtime.h"
#include <stdatomic.h>
#include <stdlib.h>
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);   /* loader.c */
void hle_signal_system_event(void *ev);   /* threads.c */
#include <stdio.h>
#include <string.h>

enum { DEVNAME = 0x100 };
static void dev_name(char *out) { memset(out, 0, DEVNAME); strlcpy(out, getenv("PLA_AUDIO_DEVNAME") ? getenv("PLA_AUDIO_DEVNAME") : "AudioTvOutput", DEVNAME); }
static int dev_list(char *names, int n) { if (getenv("PLA_AUDIO_TRACE")) fprintf(stderr, "[totkaudio] ListAudioDeviceName(%d)\n", n); if (n <= 0 || !names) return 0; dev_name(names); return 1; }
static void caller(const char *what) {
    const char *m = "?"; uint64_t off = 0;
    runtime_module_offset((uint64_t)(uintptr_t)__builtin_return_address(0), &m, &off);
    fprintf(stderr, "[totkaudio] %s <- %s+0x%llx\n", what, m, (unsigned long long)off);
}
static void dev_active(char *name) { if (getenv("PLA_AUDIO_TRACE") || getenv("PLA_AUDIO_CALLER")) caller("GetActiveAudioDeviceName"); if (name) dev_name(name); }
static int dev_channels(void) { const char *e = getenv("PLA_AUDIO_CHANNELS");
    if (getenv("PLA_AUDIO_TRACE")) fprintf(stderr, "[totkaudio] GetActiveAudioDeviceChannelCountForOutput -> %s\n", e ? e : "2");
    return e ? atoi(e) : 2; }
/* AudioOutParameter defaults: the SDK's initializer zeroes the first words; the sink then sets rate / channels / format. */
static void au_init_parameter(unsigned *p) {
    if (getenv("PLA_AUDIO_CALLER") || getenv("PLA_AUDIO_TRACE")) caller("InitializeAudioOutParameter");
    if (p) memset(p, 0, 8);
}

/* A device that never changes: the event is bound and never signalled, and a second acquisition answers 0x499 (the SDK
   keeps its own flag for the process). */
static atomic_int g_notify_out, g_notify_in;
static uint32_t dev_notify_output(void *ev) {
    if (getenv("PLA_AUDIO_TRACE") || getenv("PLA_AUDIO_CALLER")) caller("AcquireAudioDeviceNotificationForOutput");
    /* Every acquisition gets its own event (PLA asks twice, from two components: one for the device kind, one for the
       sound engine), each auto-clear and signalled once as "the device is available". A single shared event would be
       consumed by the first waiter and leave the other one parked. PLA_AUDIO_NOTIFY_SIGNAL=0 binds without a signal. */
    (void)g_notify_out;
    if (ev) { hle_bind_system_event(ev, 1); const char *e = getenv("PLA_AUDIO_NOTIFY_SIGNAL");
              if (!(e && !strcmp(e, "0"))) hle_signal_system_event(ev); }
    return 0;
}
static uint32_t dev_notify_input(void *ev) {
    if (atomic_exchange(&g_notify_in, 1)) return 0x499;
    if (ev) hle_bind_system_event(ev, 0);
    return 0;
}
static void dev_notify_switch(void *ev) { if (ev) hle_bind_system_event(ev, 0); }

void *runtime_audio_device_override(const char *name) {
    static const struct { const char *name; void *fn; } e[] = {
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", au_init_parameter},
        {"_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", dev_list},
        {"_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", dev_list},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", dev_active},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", dev_active},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", dev_channels},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", dev_notify_output},
        {"_ZN2nn5audio38AcquireAudioDeviceNotificationForInputEPNS_2os11SystemEventE", dev_notify_input},
        {"_ZN2nn5audio36AcquireAudioDeviceSwitchNotificationEPNS_2os11SystemEventE", dev_notify_switch},
    };
    for (size_t i = 0; i < sizeof e / sizeof e[0]; i++) if (!strcmp(name, e[i].name)) return e[i].fn;
    return NULL;
}
