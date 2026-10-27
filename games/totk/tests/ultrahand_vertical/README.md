# Ultrahand vertical targeting (1.0.0, Mac)

At high frame rates, right-stick pitch while targeting with Ultrahand before grabbing
an object uses a per-frame sensitivity without the real-time factor. Scale this
call site by the existing measured 30 Hz frame ratio. Ordinary camera input, the
shared getter and the separate held-object camera remain unchanged.

`TOTK_ULTRAHAND_REALTIME=0` disables the correction for comparison.

Run `make -C games/totk probe-ultrahand-vertical` on Apple silicon. The probe executes
the production ARM64 veneer on synthetic memory for 90 input/frame-rate cases and
checks disabled mode, invalid instructions, occupied padding and the shared getter.

Live validation: TOTK 1.0.0 on an Apple M5 Pro, macOS 27.0.1, at the Great Sky Island
ruins, using a copied save. Before grabbing, a 200 ms vertical input with a 120 FPS
limit moved the camera nearly to its limit before the fix. After the fix, identical
inputs at 30 and 120 FPS limits produced visually comparable small movements.
Keyboard arrows supplied the guest right-stick values; a physical DualSense was
not manipulated during this comparison. No game assets or saves are included.
