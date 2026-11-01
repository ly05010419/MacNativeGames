# Audio volume transitions and voice reuse

Port BOTW's volume ramps and stopped/reused-voice smoothing into Odyssey's mixer, retaining Odyssey's streamed
end-of-stream continuation. Backlog-trim counters distinguish a device stall from an output-buffer underrun.
The production probe checks volume changes, stops, reuse, mute/pause and stream boundaries under UBSan; disabling
smoothing must fail its discontinuity check. These fixes do not establish that every reported audio cutout is resolved.

## Source

Volume ramps/depop in runtime/audio_mixer.c, audio_mixer.h, audio_renderer.c and audio_ramp.h follow games/totk at
0fd9358 (BOTW e38682d plus voice-reuse fixes). Odyssey's end-of-stream handling is retained.
