/* Spatial audio and device-paced mixing (macOS): the pure decisions, without Core Audio, so tests/test_audio_route.c
   checks them without a device. audio_mixer.c applies them; audio_spatial.m is the AVSampleBufferAudioRenderer output.

   TOTK_SPATIAL_AUDIO = auto (default for Tears of the Kingdom on the Mac) | 0 (the output as before: the game is told the
   device's channel count, stereo with the game's own downmix, the mixer paced by the wall clock). The settings panel's row
   saves audio-spatial.txt (auto / off); the environment wins. With auto the game is always told 6 channels (it renders its
   5.1 mix), and each output device gets its own route, decided again when the Mac's default output changes:
     - a device with 6+ channels and a centre and an LFE channel (Studio Display: 8 ch, labels L R C LFE Ls Rs ...): the
       5.1 float mix straight to the device (AudioUnit, MPEG 5.1 A order = the Switch's FL FR C LFE RL RR);
     - headphones (Bluetooth: AirPods; the built-in headphone jack 'hdpn'): the 5.1 float mix with its channel layout
       through AVSampleBufferAudioRenderer, so macOS offers Spatial Audio (Fixed / Head Tracked) in the Sound menu;
     - the built-in speakers ('bltn' with data source 'ispk', or none): AUSpatialMixer, output type built-in speakers;
     - anything else (a USB DAC, AirPlay, HDMI stereo): stereo, folded from 5.1 with the game's own downmix coefficients.
   docs/audio-troubleshooting.md "Spatial audio and 5.1" has the routes, the switches and how it was checked. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { AUDIO_SPATIAL_OFF = 0, AUDIO_SPATIAL_AUTO = 1, AUDIO_SPATIAL_UNSET = -1, AUDIO_SPATIAL_UNKNOWN = -2 };

/* "auto" / "on" / "1" -> auto, "0" / "off" -> off; NULL or "" -> AUDIO_SPATIAL_UNSET, anything else -> AUDIO_SPATIAL_UNKNOWN. */
static inline int audio_spatial_parse(const char *s) {
    if (!s || !*s) return AUDIO_SPATIAL_UNSET;
    if (!strcmp(s, "auto") || !strcmp(s, "on") || !strcmp(s, "1")) return AUDIO_SPATIAL_AUTO;
    if (!strcmp(s, "0") || !strcmp(s, "off")) return AUDIO_SPATIAL_OFF;
    return AUDIO_SPATIAL_UNKNOWN;
}
/* The effective mode: the environment (TOTK_SPATIAL_AUDIO) wins over the settings file's word, then the build's default.
   A word that is not understood counts as not set (so a typo never turns the feature off silently: it is logged). */
static inline int audio_spatial_mode(const char *env, const char *file_word, int build_default) {
    int e = audio_spatial_parse(env); if (e >= 0) return e;
    int f = audio_spatial_parse(file_word); if (f >= 0) return f;
    return build_default;
}

/* Output routes. */
typedef enum {
    AUDIO_ROUTE_NONE = 0,         /* no device (TOTK_AUDIO_OFF, or no output unit): mixing without sound */
    AUDIO_ROUTE_LEGACY,           /* spatial off: the output as before (width from the device, game downmix in the mixer) */
    AUDIO_ROUTE_STEREO,           /* 5.1 mix folded to stereo in the output callback (the game's coefficients), AudioUnit */
    AUDIO_ROUTE_SURROUND,         /* 5.1 float straight to a 5.1-capable device, AudioUnit */
    AUDIO_ROUTE_SYSTEM_SPATIAL,   /* 5.1 float through AVSampleBufferAudioRenderer (headphones: the Sound menu's Spatial Audio) */
    AUDIO_ROUTE_SPEAKER_SPATIAL,  /* 5.1 through AUSpatialMixer, output type built-in speakers, then the AudioUnit */
} AudioRoute;

static inline const char *audio_route_name(AudioRoute r) {
    switch (r) {
    case AUDIO_ROUTE_NONE: return "none";
    case AUDIO_ROUTE_LEGACY: return "legacy";
    case AUDIO_ROUTE_STEREO: return "stereo";
    case AUDIO_ROUTE_SURROUND: return "surround";
    case AUDIO_ROUTE_SYSTEM_SPATIAL: return "system-spatial";
    case AUDIO_ROUTE_SPEAKER_SPATIAL: return "speaker-spatial";
    }
    return "?";
}

#define AUDIO_FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
/* Core Audio's transport types and data sources (kAudioDeviceTransportType*, the built-in data source codes). */
#define AUDIO_TRANSPORT_BUILTIN      AUDIO_FOURCC('b', 'l', 't', 'n')
#define AUDIO_TRANSPORT_BLUETOOTH    AUDIO_FOURCC('b', 'l', 'u', 'e')
#define AUDIO_TRANSPORT_BLUETOOTH_LE AUDIO_FOURCC('b', 'l', 'e', 'a')
#define AUDIO_SOURCE_SPEAKERS        AUDIO_FOURCC('i', 's', 'p', 'k')
#define AUDIO_SOURCE_HEADPHONES      AUDIO_FOURCC('h', 'd', 'p', 'n')

typedef struct {
    int present;              /* a default output device was found */
    uint32_t transport;       /* kAudioDevicePropertyTransportType */
    uint32_t data_source;     /* kAudioDevicePropertyDataSource, 0 when it has none */
    int channels;             /* output channels (all output streams) */
    int centre_lfe;           /* its preferred layout has a centre and an LFE channel */
} AudioDeviceInfo;

typedef struct {
    int spatial;              /* AUDIO_SPATIAL_OFF / AUDIO_SPATIAL_AUTO */
    int audio_off;            /* TOTK_AUDIO_OFF: no device at all */
    int tap;                  /* a stereo tap (zeldaVR's headset audio) is installed: keep a stereo AudioUnit */
    int system_spatial_ok;    /* AVSampleBufferAudioRenderer can be used (audio_spatial.m linked, macOS 12+) */
    int speaker_spatial_ok;   /* AUSpatialMixer can be used */
} AudioRouteInputs;

/* The route for one output device. */
static inline AudioRoute audio_route_decide(const AudioRouteInputs *in, const AudioDeviceInfo *dev) {
    if (in->audio_off) return AUDIO_ROUTE_NONE;
    if (in->spatial != AUDIO_SPATIAL_AUTO) return AUDIO_ROUTE_LEGACY;
    if (!dev->present) return AUDIO_ROUTE_NONE;
    if (dev->channels >= 6 && dev->centre_lfe) return AUDIO_ROUTE_SURROUND;
    if (in->tap) return AUDIO_ROUTE_STEREO;
    int headphones = dev->transport == AUDIO_TRANSPORT_BLUETOOTH || dev->transport == AUDIO_TRANSPORT_BLUETOOTH_LE ||
                     (dev->transport == AUDIO_TRANSPORT_BUILTIN && dev->data_source == AUDIO_SOURCE_HEADPHONES);
    if (headphones) return in->system_spatial_ok ? AUDIO_ROUTE_SYSTEM_SPATIAL : AUDIO_ROUTE_STEREO;
    int speakers = dev->transport == AUDIO_TRANSPORT_BUILTIN && dev->channels <= 2 &&
                   (dev->data_source == AUDIO_SOURCE_SPEAKERS || dev->data_source == 0);
    if (speakers) return in->speaker_spatial_ok ? AUDIO_ROUTE_SPEAKER_SPATIAL : AUDIO_ROUTE_STEREO;
    return AUDIO_ROUTE_STEREO;
}
/* Channels the route sends to the device (or the system renderer). The mix itself is 6 wide whenever spatial is on. */
static inline int audio_route_device_channels(AudioRoute r) {
    return r == AUDIO_ROUTE_SURROUND || r == AUDIO_ROUTE_SYSTEM_SPATIAL || r == AUDIO_ROUTE_SPEAKER_SPATIAL ? 6 : 2;
}
/* The channel count the game is told (IAudioDevice GetActiveChannelCount) with spatial on: 6, so it renders its 5.1 mix
   whatever the device (fixed when the renderer opens: TotK never gets a device-change event, cmds 4/11/12 are never
   signalled); TOTK_AUDIO_CHANNELS=2 keeps it at 2. */
static inline int audio_spatial_game_channels(const char *channels_env) {
    return channels_env && !strcmp(channels_env, "2") ? 2 : 6;
}

/* 5.1 (FL FR C LFE RL RR) to stereo with the game's downmix coefficients (front, centre, surround; the mixer's own default
   1 / 0.7071 / 0.7071 when the game set none), LFE dropped, clipped to +-1: the same fold as audio_mixer_render's stereo
   branch, done at the output so the route can change without changing the mix. */
static inline void audio_fold_51(const float *in, const float coef[3], float *l, float *r) {
    float c = coef[1] * in[2];
    float a = coef[0] * in[0] + c + coef[2] * in[4], b = coef[0] * in[1] + c + coef[2] * in[5];
    *l = a > 1.0f ? 1.0f : a < -1.0f ? -1.0f : a;
    *r = b > 1.0f ? 1.0f : b < -1.0f ? -1.0f : b;
}

/* Device-paced mixing. The renderer thread renders a frame when the ring's backlog plus that frame fits the target
   (prefill + one device period), else it waits for the consumer (the device callback or the AVSampleBufferAudioRenderer
   feeder) to take some; a wait that times out more than AUDIO_PACE_STALL times in a row is a stalled device (Bluetooth
   reconnecting, a route being rebuilt): the renderer then goes back to the wall clock until the consumer moves again,
   so the game's audio clock never stops. */
#define AUDIO_PACE_STALL 3
typedef struct { uint64_t last_rd; int timeouts; int stalled; } AudioPace;
typedef enum { AUDIO_PACE_RENDER, AUDIO_PACE_WAIT, AUDIO_PACE_CLOCK } AudioPaceStep;

static inline uint64_t audio_pace_target(uint64_t prefill_frames, uint64_t period_frames, uint64_t ring_frames) {
    uint64_t t = prefill_frames + period_frames;
    return t > ring_frames ? ring_frames : t;
}
/* One decision: rd / wr are the ring's read and write positions, frame the renderer's frame, timed_out whether the last
   wait ended by its timeout. */
static inline AudioPaceStep audio_pace_step(AudioPace *p, uint64_t rd, uint64_t wr, uint64_t frame, uint64_t target, int timed_out) {
    if (rd != p->last_rd) { p->last_rd = rd; p->timeouts = 0; p->stalled = 0; }   /* the consumer moved */
    else if (timed_out && ++p->timeouts > AUDIO_PACE_STALL) p->stalled = 1;
    if (p->stalled) return AUDIO_PACE_CLOCK;
    uint64_t backlog = wr - rd;
    return backlog + frame <= target ? AUDIO_PACE_RENDER : AUDIO_PACE_WAIT;
}
/* How long to wait for the consumer: one device period plus one frame (a 512-frame device pulls every 10.7 ms, and a
   shorter timeout would count every gap between its pulls as a stall), at least two frames. In nanoseconds. */
static inline uint64_t audio_pace_timeout_ns(uint64_t period_frames, uint64_t frame, int rate) {
    uint64_t f = period_frames + frame; if (f < 2 * frame) f = 2 * frame;
    return rate > 0 ? f * 1000000000ull / (uint64_t)rate : 10000000ull;
}
/* The device buffer size asked for: the renderer's frame (240 at 48 kHz, 5 ms), within the device's range. */
static inline uint32_t audio_buffer_frames(uint32_t frame, double range_min, double range_max) {
    double f = frame;
    if (range_max > 0 && f > range_max) f = range_max;
    if (f < range_min) f = range_min;
    return (uint32_t)f;
}
