/* The system Spatial Audio output (audio_spatial.m, macOS): AVSampleBufferAudioRenderer fed from the mixer's ring. */
#pragma once

/* Fills dst with `frames` interleaved frames (zero-filled on an underrun); called on the feeder thread. */
typedef unsigned (*AudioSpatialRead)(void *ctx, float *dst, unsigned frames);
/* NULL when the renderer is unavailable or could not start. channels must be 6 (MPEG 5.1 A layout); block is the frames
   per CMSampleBuffer, lead_ms how far ahead of the clock the feeder keeps it, gain a linear make-up gain (clipped). */
void *audio_system_spatial_open(int rate, int channels, unsigned block, int lead_ms, float gain, AudioSpatialRead read, void *ctx);
void audio_system_spatial_close(void *handle);   /* stops the feeder; read is not called after it returns */
int audio_system_spatial_available(void);
