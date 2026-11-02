# EAX on Creative hardware: measured behaviour

What runtime/audio/reverb and runtime/audio/eax reproduce of an EAX
card, measured as a black box (no driver code was read): a Creative
Audigy 2 ZS under Windows 98 SE, driven through Miles 6.5e's EAX 1-3
providers by tools/eax_capture (eaxcap.exe), 48 kHz, impulse and sweep
stimuli. Fits and constants are next to the code; this is the structure
behind them. Times are after the direct sound; D is the reflections
delay, R the reverb delay (counted from D).

## Reflections
- First order: five taps after D. R channel only at D; both channels at
  D + x and D + 2.67 ms; L only at D + 1.657x; both at D + 2.29x, with
  x = 0.974 ms + 0.0951 R (fits R = 1.5 to 100 ms). At reflections and
  room 0 dB the taps read -18 / -18 / -21.1 / -18 / -21.1 dB peak.
- Later orders: a decaying train of taps at a + b x (a, b fitted from R =
  11, 50, 100 ms), the first two about 6 dB above the first-order taps,
  then -3, -8, -11, -16, -19 dB. They carry most of the reflections'
  energy and last about 80 ms at R = 11 ms, longer as x grows.
- Setting D only moves the whole pattern; the environment size changes
  it only through the properties its *SCALE flags rescale.

## Late reverb
- From D + R: a burst of 32 taps per channel over 44 ms at a flat level,
  whatever the decay time (eight clusters of four: L at +0, 0.44, 0.92,
  2.19 ms, R at -1.29, 0, 0.44, 0.92 ms; clusters at 1.31, 3.77, 6.94,
  10.96, 16.04, 22.52, 30.79, 41.31 ms). Without diffusion only four taps
  remain (1.75, 4.69, 9.13 or 9.65, 10.94 ms) at the same energy; at 0.5
  the four are 0.8 dB down and the rest 3.5 dB.
- The burst is low-passed by the decay HF ratio (about the HF decay in
  excess of the mid band over 80 ms); the decay LF ratio leaves it alone.
- Then a recirculating tail at the decay time: its energy density is an
  exponential at about -3.7 dB re the burst's at 30 ms after D + R,
  present from about 55 ms (80 ms later without diffusion, sooner in
  rooms under about 3 m). Tap times do not depend on the
  environment size otherwise.
- The total late energy (burst and tail) follows the reverb level at any
  decay time: the burst's level from 0.3 to 10 s fits that within 0.5 dB.
  The decay HF ratio does not renormalise it.
- Left/right correlation of the late reverb is about 0.7.
- Per band: the HF damping reaches into the mid band at low HF ratios;
  HF decay barely exceeds the mid decay at ratios above 1; a short LF
  decay shortens decays up to about 2 kHz; the LF decay runs up to about
  10% long at short decay times. The decay HF limit follows air
  absorption (60 dB at its loss per metre at 343.3 m/s).
- Echo depth shortens the burst and makes the tail periodic; modulation
  depth costs some HF decay. Both only approximated.

## Levels and filters
- Room LF: a one-pole high-pass on the reverb input, -30 dB at 250 Hz
  giving about -2.6 dB at 8 kHz.
- Room HF (listener or source), occlusion and exclusion on the send: a
  low-pass broader than the direct path's: one real pole for the first
  15 dB of cut at 5 kHz, a second for the rest.
- Air absorption also lowers and shortens the reverb (-100 mB/m:
  generic decays in 0.53 s, the reverb about 13 dB down).
- The presets read back as eax.cpp's table; pans are not readable
  through Miles.
