# Pipeline prediction on the submission thread

## What prediction does

A render pipeline that is not built yet is compiled on the submission thread when a draw needs it. On the first
such miss of a submission, `pso_get` first scans the rest of that submission (`pso_predict_rest`: state only, no
drawing), works out the pipeline keys of the later draws, and starts compiling the missing ones on background
threads. When the submission thread reaches one of those draws it takes the pipeline, or waits for what is left
of its compile (`pso_pend_take`). A scan also guesses the other ways the pass may attach depth / stencil (which
planes a pass attaches is decided only when it begins, `ds_scan`); wrong guesses only cost background work.
`TOTK_PSO_PREDICT=0` turns prediction off.

## What PR #29 changed, and the follow-ups

- **No program is resolved for a guess.** The scan used `prog_get`, which translates and compiles a program the
  first time it is seen. Every new program of a submission was therefore translated and compiled on the
  submission thread before the current draw, while nothing had been started in the background yet. Now a guess
  uses only a program that is already resolved, and only the variant for the binding state at that draw
  (`prog_prediction_stages`); the program's active variant is not changed. New programs are left to their draws.
- **The scan keeps the future pass's render targets** (`R.rt`), so a fragment variant's signature describes the
  pass it will draw in.
- **Bounded.** At most 8,192 commands, 32 keys per queue and 2 ms per scan; keys with the pass's own attachments
  start as soon as they are found, the other attach modes after the scan. `TOTK_PSO_PREDICT_SCAN`,
  `TOTK_PSO_PREDICT_KEYS` (up to 160) and `TOTK_PSO_PREDICT_US` change the three limits; the log prints them once.
  The time is checked between commands, so it is not a hard bound on descriptors or driver calls.
- **Stale guesses are freed.** The pending table has 384 slots. A guess no draw takes (a wrong attach mode, a
  draw that did not happen) used to keep its slot forever; after a few scans the table was full and nothing was
  predicted any more. Now a completed guess no draw took is freed two submissions later. A slot whose compile has
  not finished is never reused: its worker still writes into it.
- **Follow-ups (MacNativeGames):** a program with a stage not translated yet (`missing`, iOS) is not predicted
  (its draws are skipped, and its fragment stage may be absent); a fragment function that exports depth gets no
  guesses without the depth buffer (`pred_guess_wanted`: `ds_scan` attaches the depth buffer for it, so those
  guesses were never used, and `pso_desc` compiled their depth-less library on the submission thread); a
  statistics line every 100 scans (`并行预编 至提交 …`: scans and the budget that ended them, pipelines started,
  pipelines taken by draws and how long draws waited for them, guesses refused because the table was full, stale
  guesses freed).

## Still done on the submission thread

- `pso_desc` for each guess: a vertex function patched for varyings the fragment reads but the vertex shader does
  not write, or for a layered pass (`vs_fn_for`), and the depth-less fragment function of a pass without a depth
  buffer (`fs_fn_without_depth`), compile their libraries there.
- `ds_scan` at the start of each pass calls `prog_ps_writes_depth` → `prog_get`, which can translate and compile a
  program of that pass. This is the path the pass needs anyway.
- A new program at its first draw: translation, library compile and, because it could not be guessed, its
  pipeline compile, one after the other.

## Checks

`scripts/verify_gfx.py` (`make probe-gfx`, no GPU drawing): prediction leaves the active scissor, conditional
counters and bindings untouched; an unresolved future program is skipped without translation or compilation; a
cached alternate variant is used without being selected; a missing variant is skipped; a program with an
untranslated stage is skipped; a depth-exporting fragment gets no depth-less guess; old completed slots are freed,
recent ones and unfinished ones are kept. `TOTK_PROBE_NEGATIVE=24` (take the active variant whatever the state),
`25` (free every slot), `26` (ignore untranslated stages) and `27` (guess depth-less pipelines for depth export)
each make the probe fail at its own check.

## In-game comparison (1.2.1, Apple M4, 2026-10-19)

Copies of a field save and of the player's shader cache; warm-up off (`DM_PSO_MANIFEST=off`), so the route meets
many pipelines for the first time; after the save loads, the player is moved to Kakariko, Hateno, Rito and Goron,
40 s each. Long frames are the entries of the long-frame log between the first and the last move.

| | long frames | over 100 ms | over 1 s | total | worst |
|---|---|---|---|---|---|
| before PR #29 | 208 | 40 | 1 | 13.0 s | 1,151 ms |
| PR #29 and the follow-ups | 168 | 40 | 0 | 14.3 s | 682 ms |
| the same, `TOTK_PSO_PREDICT=0` | 224 | 32 | 0 | 14.3 s | 574 ms |

One session each, and another agent built and tested on the same Mac in between (a build overlapped the start of
the first row's session): the rows differ by no more than a session varies. No speed-up is claimed either way.

- Before the change the pending table was full by the end of the loading screen (about 500 guesses in the first
  14 scans, 384 slots); in the world a scan could start only one or two pipelines. Prediction was effectively off.
- With the change: no guess refused, 4,240 stale guesses freed, 5,113 pipelines started in the background and 859
  taken by draws; 120 of 200 scans stopped at the 8,192-command budget (0.51 ms per scan on average).
- Most of the stall on this route is programs seen for the first time: translation, library and then their
  pipeline. Prediction leaves them alone by design, and their cost is the same whether it is paid during the scan
  (before) or at the draw (now). What removes them from play is the shared shader list (`docs/shader-list.md`),
  whose rows the warm-up builds before the world shows.

## The contributor's observation

On an Apple M3 (24 GB, macOS 27.2, game 1.0.0) one session logged 32 long frames above one second, all with
library or pipeline creation; the worst was 10,384.9 ms of submission and presentation with 17.5 ms of GPU time.
With a broader local build (it also prefetched shaders, which is not part of this change) the player found severe
stalls much reduced. That was not a same-route comparison of this change alone.
