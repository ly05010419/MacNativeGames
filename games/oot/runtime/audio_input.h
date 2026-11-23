/* The Mac's microphone as the game's virtual input device: Ocarina of Time's ocarina can be played into it (the game's own
   "Ocarina Mic Input" option). After seedds/nxnative c25d4011 (PR #1822, dm/host/audio_input.h, MIT, Copyright (c) 2026
   seedds; co-authored by Felipe Parra). Off unless OOT_MIC=1 (the launcher sets it from microphone.txt, the settings
   panel's switch); the Mac only (user 2026-11-20: "只做 mac"): on the iPhone / iPad the list stays empty and opens fail.
   OOT_MIC_FILE=tone:<hz> or a raw 48 kHz mono PCM16 file feeds the streams instead of a device (probes, test runs). */
#ifndef OOT_AUDIO_INPUT_H
#define OOT_AUDIO_INPUT_H

#include <TargetConditionals.h>
#include <stdint.h>

typedef struct dm_audio_input_stream dm_audio_input_stream;

enum { DM_AUDIO_INPUT_PCM16 = 2, DM_AUDIO_INPUT_CHANNELS = 1, DM_AUDIO_INPUT_RATE = 48000, DM_AUDIO_INPUT_NAME_SIZE = 0x100 };

#define DM_AUDIO_INPUT_DEVICE "AudioVirtualInput"

#if TARGET_OS_OSX
int dm_audio_input_platform_device_available(void); /* a device (or the test feed) and granted permission; never prompts */
int dm_audio_input_platform_prepare(void);
int dm_audio_input_platform_start(void);
void dm_audio_input_platform_close(void);
int dm_audio_input_platform_failed(void);
int dm_audio_input_platform_route_changed(void);
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels);
#else
static inline int dm_audio_input_platform_device_available(void) { return 0; }
static inline int dm_audio_input_platform_prepare(void) { return -1; }
static inline int dm_audio_input_platform_start(void) { return -1; }
static inline void dm_audio_input_platform_close(void) { }
static inline int dm_audio_input_platform_failed(void) { return 1; }
static inline int dm_audio_input_platform_route_changed(void) { return 0; }
static inline void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    (void)name; (void)uid; (void)sample_rate; (void)channels;
}
#endif

int dm_audio_input_available(void);
void dm_audio_input_device_name(char out[DM_AUDIO_INPUT_NAME_SIZE]);
dm_audio_input_stream *dm_audio_input_open(int format, int channels, int rate, int capacity_frames);
void dm_audio_input_close(dm_audio_input_stream *stream);
int dm_audio_input_start(dm_audio_input_stream *stream);
void dm_audio_input_stop(dm_audio_input_stream *stream);
void dm_audio_input_purge(dm_audio_input_stream *stream);
int dm_audio_input_state(const dm_audio_input_stream *stream);
int dm_audio_input_capacity(const dm_audio_input_stream *stream);
int dm_audio_input_filled(const dm_audio_input_stream *stream);
int dm_audio_input_free(const dm_audio_input_stream *stream);
int dm_audio_input_read(dm_audio_input_stream *stream, void *dst, int frames);
float dm_audio_input_gain(const dm_audio_input_stream *stream);
void dm_audio_input_set_gain(dm_audio_input_stream *stream, float gain);

/* The capture callback's planar Float32 samples at the device rate; no locks, no allocation. */
void dm_audio_input_process(const float *const *channels, int channel_count, uint32_t frames, double sample_rate);
void dm_audio_input_route_changed(void);

/* A song from the settings panel's 陶笛 page in place of the capture (dm_audio_input_inject_begin / _end around 48 kHz
   mono pushes); dm_audio_input_listening: a stream the game has started (the ocarina is out). */
void dm_audio_input_inject_begin(void);
void dm_audio_input_inject_end(void);
void dm_audio_input_inject(const float *mono, uint32_t frames);
int dm_audio_input_listening(void);
#if TARGET_OS_OSX
/* Plays notes (Hz, each note_s long with gap_s of silence after) into the game's microphone streams, in real time; 1: it
   started, 0: the game is not listening (no stream started), -1: a song is already playing. audio_input_mac.m. */
int dm_audio_input_play_notes(const double *hz, int count, double note_s, double gap_s);
int dm_audio_input_playing(void);
#endif

#endif
