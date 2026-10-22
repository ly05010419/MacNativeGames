# BotW rendering up to 120 FPS

BotW's real-time frame-step patches let the game render above 60 FPS without MetalFX frame interpolation. On a display reporting exactly 120 Hz, the uncapped configuration previously excluded equality (`hz < 120`). That left the guest's present interval active: an interval of two could pace the game at 60 FPS. BotW now enables the uncapped path at that boundary too. The existing ceiling remains 120; actual throughput depends on the scene and hardware.

The change is guarded by `GAME_BOTW`. TOTK retains its previous boundary policy. Explicit `DM_PRESENT_HZ`, `TOTK_UNCAPPED=0`, `TOTK_REALTIME=0` and `TOTK_FPS60=0` keep their existing pacing behavior. Interpolation modes retain their existing limits and can still be cycled with key 2.

For an installed app, fully quit before testing:

```sh
env -u DM_PRESENT_HZ \
  TOTK_REALTIME=1 TOTK_UNCAPPED=1 TOTK_UNCAPPED_HZ=120 \
  TOTK_UNCAPPED_INTERVAL=1 TOTK_MFX=0 \
  '/Applications/Breath of the Wild.app/Contents/MacOS/BotW'
```

## BotW 1.9.0 menu timing

The 1.9.0 controller loop uses floating-point hold counters but advances them by one per rendered frame. At 120 FPS a brief direction press can therefore pass several menu items. The `GAME_BOTW190` patch feeds that loop the same VFR ratio as the game clock, so held counts and repeat windows use elapsed game time. Trigger and release handling remain per frame. Original instructions are checked before writing either patch instruction; `TOTK_PAD_REPEAT=0` retains the shipped loop for diagnostics.

This guarded addition requires a BotW 1.9.0 version port such as #142 or #194; it does not add extraction, executable pins, SDK support or the 1.9.0 VFR patch sites. PR #194 already contains related 120 Hz and 1.9.0 menu work, so these overlapping implementations need reconciling if both changes are adopted. BotW 1.6.0 retains its existing integer/tick-based menu-repeat patch.

## Validation

The graphics probe exercises the production configuration and pacing functions at 60 and 120 Hz, including the interval-two case and explicit pacing/legacy overrides. The 1.9.0 instruction-level check runs the actual game loop using the player's executable:

```sh
python3.14 -m pip install unicorn
python3.14 games/botw/scripts/verify_pad_repeat_190.py /path/to/botw190/main.patched.bin
```

At 30, 60 and 120 FPS, the patched loop produces 28 repeats during the same two-second hold. Quick taps, release/repress and counter reset are checked. The original loop at 120 FPS produces 118 repeats, providing the negative control.

The local 1.9.0 integration run measured 113.4 real game FPS over 30 seconds with interpolation disabled and typical frame times near 8.3 ms. This is one scene/run, not a guaranteed 120 FPS throughout the game. Full high-rate physics and cutscene validation remains outstanding.
