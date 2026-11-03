# Miles Sound System: observed behaviour

What runtime/audio's Miles-compatible parts reproduce, as observed in
the original binaries (scratch Ghidra projects, no names applied).
Addresses are the evidence; constants are listed with where they live
and what they are for. The builds:

| File | Version | Image base | SHA-256 |
|---|---|---|---|
| mss32.dll | 6.5e | 0x21100000 | 7855b8fbae917cb8449f2d4361ab61b5ecec4df0a11130d797cb0aa99b4260ea |
| miles/msssoft.m3d | 6.5e ("Miles Fast 2D Positional Audio") | 0x22400000 | 62233bebccb6aeb97370a8499c577e2982f46924e3a1bc55c53fa1f8a2404252 |
| Mssfast.m3d | 6.1c ("Miles Fast 2D Positional Audio") | 0x22600000 | |

Arithmetic, unless stated otherwise: x87, control word never set by
Miles, so the Windows default 53-bit precision on its mixer thread:
every operation rounds to double, every "float(...)" to float.

## Miles 6.5e digital reverb
Implemented by runtime/audio/digital_reverb; checked bit for bit by
recomp_add_digital_reverb_test.

API and state
- Room type n (0x21115930) = set reverb (time, predelay, damping) and
  levels (dry 1, wet) from entry n of a 26-entry table at 0x21155838,
  16 bytes per entry: wet, time, damping, predelay (floats). n is the
  EAX environment number and is not range-checked.
- Set reverb (0x2112B0F0) stores the three values and the tail length
  round((time + predelay) * 1000) ms (1000.0f at 0x2114BE08), and
  recomputes the coefficients. Only when the driver wet level and time
  are both above 1e-4 (0x2114BD1C) does it also wake the reverb and move
  the read position to `round(rate * predelay)` samples behind the
  write position (wrapping in the ring).
- Set levels (0x2112AA10) stores dry/wet and wakes the reverb when wet
  and time are above 1e-4. Waking = timeout at now + tail + 100 ms
  (AIL_ms_count) and the active flag set.
- A sample starting, resuming or getting a buffer (0x2112B690/B760/
  B940), or getting new reverb levels while playing (0x2112A8B0), wakes
  it when the sample's wet, the driver's wet and the time are all above
  1e-4.
- Driver open: dry 1, wet 1, time 0 (off); a sample opens with dry 1,
  wet 0. The driver dry level multiplies every sample's dry gains.

Buffers (driver open, 0x2112AA70)
- One zeroed allocation holds a mono ring of 32-bit samples and the
  delay lines. Ring length: the maximum predelay (DIG preference 2,
  default 500 ms at 0x21156618) in samples, rounded up to whole mixer
  fragments.
- Four comb delay lines of round(rate * d) samples, d = 0.0297,
  0.0371, 0.0411, 0.0437 s (floats at 0x21142238..0x21142244), and two
  allpass lines of 0.005 and 0.0017 s (0x21142248, 0x2114224C).

Per mixer fragment (0x2112DF90)
- Before mixing (0x2112BCC0): if active and the ms clock has passed the
  timeout, it goes inactive (ring and delay lines keep their contents
  until it wakes again); otherwise the ring slot at the write position
  is cleared.
- While active, every sample whose wet scale round(driver wet x sample
  wet x sample volume x master x 2048) (max 2048) is non-zero is mixed
  mono into that slot (in 0x2112BD40) and pushes the timeout to
  now + tail + 100 ms if it is closer than that.
- After mixing, while active (0x2112B040): one fragment of ring samples
  starting at the read position runs through the reverb and is added
  to the stereo 32-bit build buffer (16-bit samples x gains out of
  2048); both positions advance by one fragment and wrap.

Coefficients (0x2112ACE0), each stored as a float
- inv = float(1 / time). Comb gain i = pow(0.001, inv * d_i), 0.001
  being the double at 0x2114BD38 (the float 0.001 widened): -60 dB
  after `time` seconds.
- Allpass gains: pow(0.001, 0.005 / 0.0968535) and
  pow(0.001, 0.0017 / 0.032924) (decay times at 0x21142250/54).
- Damping low-pass: 2nd-order Butterworth by the bilinear transform,
  c = 1 / tan(damping * pi/8) (pi/8 float at 0x2114BD34), so the cutoff
  is damping/8 of the rate: a0 = 1 / ((sqrt2 + c) c + 1),
  b1 = 2 (1 - c^2) a0, b2 = ((1 - sqrt2 c) + c^2) a0.
  Damping 0 (rooms 1 padded cell, 4 living room) gives c = inf and NaN
  coefficients: that reverb adds nothing (a NaN converted to an integer
  is the indefinite value, low 32 bits 0) and its state stays NaN until
  the driver reopens.

Per sample (0x2112ADE0), x the ring sample
- Low-pass: y = ((2 p1 + p2) + a0 x) - b1 y1 - b2 y2, where p1, p2 are
  the last two values of a0 x stored as floats, and y1, y2 the last two
  of float(y + 1e-10) (1e-10 at 0x2114BD30).
- Combs: s = float(((c1 + c0) + c3) + c2) of the four lines' current
  outputs, then each line's slot becomes float(g_i * c_i + y) (feed
  after the sum).
- Allpass 1 (gain g, line value a): v = g a + s, stored as a float;
  o = float(a - g v).
- Allpass 2 (gain h, line value b): p = float(h b); left: w = 0.25 o + p
  (0.25 at 0x2114BD2C), stored in the line; L = b - h w. Right uses
  0.15 (0x2114BD28) for w and is not stored: R = b - h (0.15 o + p).
- L and R are rounded to nearest integers (64-bit, low 32 bits kept)
  and added to the build buffer; mono drivers get L only.
- Rounding to single precision instead of double changes 112 of the
  120 test scenarios.

## Miles 6.5e sample levels and Fast 2D
Implemented by runtime/audio/miles65; checked bit for bit by
recomp_add_fast2d_test. pow is the CRT's (x87 or SSE2 path, by the
CRT's SSE2 flag: mss32 0x2115AD20, msssoft 0x22417228), acos the CRT's
x87 one (atan2(sqrt((1 + x)(1 - x)), x) by fpatan; exactly 0 at x = 1
and fldpi at x = -1; NaN with a domain error for |x| > 1).

Sample fields (mss32's sample block)
- 0x64 volume, 0x68 pan, 0x6C left level, 0x70 right level, all
  floats; 0x78 sample dry level, 0x74 sample wet level.
- AIL_set_sample_volume_pan (0x2112A930, also AIL_init_sample with
  1, 0.5 at 0x2112A700, and AIL_set_stream_volume_pan on the stream's
  sample): volume = float(pow(v, 5/3)) (double 5/3 at 0x2114BD50);
  pan = p as given; left = float(pow(1 - p, e) x volume), right =
  float(pow(p, e) x volume) with e = float 0.3 widened (0x2114BD48).
  No clamping: v < 0 or p outside 0..1 gives NaN levels.
- AIL_set_sample_volume_levels (0x2112A810): left and right stored as
  given; volume and pan are derived for the wet send and read-back:
  if left > 1e-4 (0x2114BD1C): q = pow(right / left, 10/3) (0x2114BD60),
  pan = float(q / (1 + q)), and volume = left if pan < 1e-4, else
  float(pow(pan, -e) x right) (-e at 0x2114BD58). Otherwise pan = 0,
  volume = right.
- AIL_sample_volume_pan (0x211153E0; streams 0x2111AD70) reports
  float(pow(volume, 0.6)) (0x2114BD40) and the stored pan;
  AIL_sample_volume_levels the two levels.

Channel gains (per mixer fragment, 0x2112BDB8)
- d = driver dry (0x108) x sample dry; gain = round(((level x master)
  x d) x 2048.0) (2048 double at 0x2114BDF8), per channel; the 64-bit
  result's low 32 bits are taken and capped at 2048 (0x800) from above
  only (so NaN or a huge level gives 0). master = driver block 0x10
  (1.0 from open).
- Digital reverb send: round((((driver wet x sample wet) x volume) x
  master) x 2048.0), same capping: it follows the derived volume, not
  the levels.
- The mixer scales 16-bit samples by these gains (out of 2048); no
  balance table, so a centred 2D sample (v = 1, p = 0.5) gets
  0.5^0.3 = -1.8 dB on each channel, a hard-panned one 0 dB on one.

Fast 2D (msssoft.m3d)
- Listener: position as given; orientation (0x224019B0) keeps face as
  given (0x22415654..5C, not normalised) and right = up x face, each
  component float(a - b) of the two float products (0x22413C40..48).
- Per sample: 0x08 volume (AIL_set_3D_sample_volume's float as
  given), 0x10 position, 0x44 max and 0x48 min distance (mss32
  0x2111F91D passes the larger as max, as 6.1c does), 0x54 the 2D
  sample. Setting the 3D sample's data (0x22401D30) requires PCM
  ("Not linear PCM data") and mono ("Not mono data"), and resets
  volume 1, max 200, min 1, face (0,0,1) up (0,1,0), velocity 0,
  effects level 0, obstruction and exclusion 0; not the position.
- Every change to a sample's position, volume, distances, effects
  levels, and every listener move or turn (0x22401470: all samples),
  runs the update (0x22401000):
  1. d = sqrt((dz^2 + dy^2) + dx^2) of the double differences
     sample - listener. If d > max: with "Mute at maximum" (provider
     preference 1, default 1, at 0x2241322C) the sample gets
     volume/pan (0, 0.5), i.e. silence (unless it is stopped, status 8:
     then nothing changes) and the update ends; without it d = max.
     Below, D = float(d) (or max).
  2. g = float(pow(volume, 5/3)) (0x2240E340). m = min if min >= 1e-4,
     else 1e-4 (0x2240E33C). If 1e-4 + m < D:
     g = float((m / ((D - m) x rolloff + m)) x g), rolloff = provider
     factor at 0x22413230, 1.0 by default: 1/distance falloff, -6 dB
     per doubling past min_dist.
  3. The direction n = the float differences normalised (0x22402980):
     length sqrt((x^2 + y^2) + z^2), each component float(c x 1/length);
     (1, 0, 0) if the length is 0.
  4. front = float(face . n); pan = 0.5 if D <= 1e-4, else
     acos(right . n) x float(1/pi) (0x2240E338), kept as a double; the
     dot products are ((x + y) + z) of the float products. (At -1 the
     x87's pi times float(1/pi) rounds to the same double as the
     double pi's product.)
  5. v = g, or g x 0.75 (0x2240E34C) if front < 0 (behind).
     AIL_set_sample_volume_levels(left = float(pan x v),
     right = float((1 - pan) x v)): a source on the right
     (right . n = 1) gets 0 on the left and v on the right; centred,
     v/2 each.
  6. Velocities would scale the playback rate (Doppler, 0.355 at
     0x2240E328; not modelled); then AIL_set_sample_reverb_levels(dry =
     1 - obstruction, wet = (1 - exclusion) x |effects level|)
     (0x224013A0 stores the effects level's absolute value).
- Consequences: centred 0.5 per channel (-6 dB relative to a hard pan's
  near channel), 45 degrees 0.75 / 0.25, side 1 / 0, behind x 0.75;
  a slightly-over-1 dot product (unit vectors rounded to floats) makes
  acos NaN and the sample silent until it moves.

Checked against an Audigy 2 ZS under Windows 98 SE (tools/eax_capture,
fast2d set, relative to front centre): +-45 degrees near +3.5, far
-6.0 dB (model 3.52 / -6.02); +-90 near +6.0, far -52.6 (model 0: the
recording's floor); 135 near +1.0, far -8.5 (1.02 / -8.52); 180 -2.5
both (-2.50); distance 2/4/8 x min: -6.0, -12.0, -18.1 (-6.02, -12.04,
-18.06).

## Miles 6.1c Fast 2D
Implemented by Mixer::TargetGains (runtime/audio/mixer.cpp), where the
falloff and pan law and Mss32's balance table are described.

Its asymmetry at +-90 degrees is the original's: Mssfast.m3d builds its
2049-entry pan table (0x22603950) with 127 - floor(acos(i / 1024 - 1) x
127 x c), c = 0.31830987732601135 (double at 0x2260B938, just under
1/pi), so a source hard left gets acos(-1) x 127 x c = 126.9999965, pan
1, and 2/128 (-36 dB) on the right channel, while one hard right gets
pan 127 and nothing on the left.
