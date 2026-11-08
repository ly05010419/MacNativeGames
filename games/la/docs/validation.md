# Link's Awakening 1.1.0: validation records

What was measured on a Mac for this target, how, and what is still open. Game files, saves and logs stay with the
person who ran them; this file keeps only the method and the numbers.

## Pipeline warm-up seed (2026-11-08)

`runtime/games/01006BB00C6F0000/pso_manifest.tsv` is the list of shaders and pipelines the app builds in the
background at start, so the game does not compile them while it is played. It holds 862 rows (418 shader libraries
`S`, 437 render pipelines `R`, 7 compute pipelines `C`) merged by `scripts/pso_seed.py` from three Mac recordings,
named in its header with their SHA-256, in the order merged. Its SHA-256 is
`830c16fcd66b89a54b0e206cc8311b2542223f4e456127511edb12dd3973bcd2`.

**Warm-up.** A run from a new profile read the 862 rows and built 418 / 437 / 7, with no fragment variant skipped, no
key missing from the shader map and no pipeline error, so no row is ignored. A packaged app carried the seed with the
same SHA-256 (`build-info.json`), and `assets/expected-1.1.0.json` did not change: the seed is not a shader-chain input.

**Comparison.** Two 7-minute sessions with the same host, the same starting save and no earlier `pso_user.tsv`: the
first with the seed turned off (`DM_PSO_MANIFEST=off`), the second with it. A person walked the same route in both,
then left Link standing.

The phases come from the game's render structure, identical in both sessions (menu, a loading screen of 1 to 3 render
passes, then the first world frame with about 472 draws), not from frame times. Load: the loading screen up to the
first world frame. Walk: the first world frame and the 180 seconds after it. A hitch is a frame over 25 ms; a compile
is nearby when a new pipeline or library was made in that frame or the three before.

| Walk, 180 s | Seed off | Seed on | Target |
|---|---|---|---|
| Hitches | 34 (11.3 / min), 33 with a compile nearby | 17 (5.7 / min), 5 with a compile nearby | at most 7.5 / min |
| On-demand pipeline builds | 329 | 20 | fewer than 50 |
| On-demand library loads | 308 | 18 | |
| Time lost in hitches | 934 ms (5.2 ms/s) | 611 ms (3.4 ms/s) | |
| Time lost, all frames over 16.7 ms | 12.7 ms/s | 11.0 ms/s | |

The load phase lasted 0.7 s: one hitch and 4 builds without the seed, none with it. About 1,360 ms of the lost time
in each session are frames of 16.7 to 25 ms, nearly equal in both (1,358 and 1,366 ms).

**Open incident.** The session with the seed had eight display-link stalls of the host (279 to 1,157 ms, logged as
`display link silent` / `recovered`); the session without it had none. The first one holds the 150 ms frames at the
start of the walk, 10 of its 12 hitches without a compile nearby; the others came after the walk. The game stayed in
focus during all of them (one focus change, after the last), it was never minimised (the host logs that as focus
state 3), and every power line reported thermal state 0, which shows only the state reported. The host does not log
a window covered by another.
Their cause is not known: it is not shown that they are unrelated to the seed, nor that the seed causes them.

## Checks by hand (2026-11-08)

Disposable profiles with a copy of one save; the logs and recordings stay with the person who ran them.

**Corner box.** The settings log of one session shows keys 1 to 6 each doing their part: 1 showed the box,
2, 3 and 6 removed and added their lines, 1 hid it and showed it again with the same lines, 4 and 5 added
timing and the graph; the settings file kept `hud_lines=fps,timing,graph,keys`, the last lines shown. The
graph, frames per second from 0 to 60, was seen right on screen.

**Shader list export.** From a copy of a real user list with 38 rows the seed lacks, Settings → Shader list →
Export shader list wrote a file with its two header lines and 38 rows: none repeated, none in the seed, the
user list's own order. `scripts/pso_seed.py --shader-map` accepted every row (all keys in the 1.1.0 map) and
would make a seed of 904 lines.

**Controller and sound.** The player used a game controller without problems and heard the sound right. A
recording of what the game sent to the output (`DM_AUDIO_DUMP`) has sound: 61 of 70 half-second windows peak
above 500, at most 5,907. In an earlier session the player heard nothing while the game's outputs ran (their
device callbacks were steady, with a few underruns); where the player listened was not established.

**Frame rate outside.** While the player walked outside Marin and Tarin's house (Mabe Village, 30 s), the game
ran at 60.0 frames per second, 1 frame with present interval 2, and the display link presented every tick.

**Marin's dialogue and scripted input.** Each A press, about 3 s apart, reached the game as 9 to 10 reads down
and a read up; the game reads one state a frame (no history). Talking to Marin again, her two lines then the
next A closed the box. Twelve scripted presses sent back to back used to reach the game as one press held for
111 frames; scripted presses now wait for the game to read the release, and the same twelve reached it as
twelve, with no time-out logged. Facing Marin, the burst ended with her first line shown again, compatible
with the box closing and opening within it; those frames were not captured. Nothing here shows how the
original game behaves on a Switch.
