# Mac game-frame ceiling

Key 8 cycles off → 120 → 90 → 60 → 40 → 30 → off. The default is off so
existing display pacing is unchanged until the player enables a ceiling. The
selection persists in `TOTKUserFPSLimit`. The overlay reports the selected value.

The ceiling applies to real game frames before frame interpolation, never speeds
up an existing slower mode and does not alter deterministic test pacing. A ceiling
that is stricter than display-link pacing uses the existing monotonic deadline pacer.
The shared graphics policy stays disabled on iOS (there is no iOS frontend setter).

Validation: `make probe-input` covers key-8 edges without other shortcut/button
side effects; `probe-gfx` exercises the production frame-period policy across
all interpolation modes and five ceilings. Local TOTK 1.0.0 Mac gameplay and HUD
checks exercised the five ceilings; other game versions and physical iOS hardware
have not been played with this feature. The public PR adds an off state to retain
upstream's default; the local installed customization initially used 120 FPS.
