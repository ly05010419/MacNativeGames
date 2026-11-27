# Contribution validation — 2026-10-31

Historical contributor evidence below predates integration. Only Metal 4 / macOS 26+ is now supported; the Metal 3 entry points have been removed. The PR's original instruction-checked physical-pool alignment and Mii/cloth heap patches are enabled by default, restored by explicit selection on 2026-11-04. This is adoption of the contribution's compatibility scheme, not a new root-cause fix or gameplay acceptance.

## Scope

Parent base: `92ec79bd10c006e1cb7ca32d7ac1d8f66c758a15`. This is a source-only,
experimental Mac port of Emio – The Smiling Man: Famicom Detective Club 1.0.0
(`0100A9501759E000`). Builds verify the original input identities. The player
supplies the original game, keys, firmware/fonts, and external SPIRV-Cross tool.
The required NativeAOT shader translator is included with source, checksum, and
licenses; it contains no translated game shaders.

The only root change is one README entry. Runtime, graphics, translator,
rewriter, scripts, tests, dependency notices, and copy-input provenance are local
to this title. No other game's build files or defaults are changed.

## Platform evidence

- Apple silicon, macOS 27.2: both Metal 3 and Metal 4 compiled and packaged from
  this self-contained folder. Both packages passed strict signature verification.
- Both binaries passed the native loader probe: 4,000 cross-module calls on four
  threads. These use the player's verified modules and establish loading/linkage.
- Both binaries passed 3,117 synthetic GPU checks with Metal validation for depth
  feedback: pending clears, read-only pass reuse, pass changes, depth writes,
  source replacement, and D16/D32/depth-stencil sampling/readback.
- Earlier Metal 4 play sessions exercised setup/menus, an existing island copied
  into a native profile, building selection/interiors, Mii interactions, settings
  diagrams, and native keyboard entry. The submitted name remained accepted after
  the field closed. This does not prove save round-trips across launches.
- The reproduced block-shaped character highlights were isolated with a
  same-process on/off/on test: clean for 8 captured frames, patches restored for
  16, clean again for 24. This is evidence for that defect only. Extra depth
  copies/pass boundaries have a cost; no performance improvement is claimed.
- Metal 3 was built/probed on the same development Mac. Gameplay on an actual
  macOS 15 machine is unverified. iPhone/iPad targets are not provided or built;
  no iOS compatibility is claimed. No new full gameplay sweep was performed after
  organizing this branch.

## Test result

The required parent suite passed on the desktop-enabled development Mac:
**1,313 tests, 169 skipped, zero failures/errors**. The title-local Python and
native checks passed. An initial restricted run could not use desktop display
metrics or temporary preferences; the desktop-enabled rerun is the reported
result. External build/package/probe/test logs use the prefix
`/tmp/emio-upstream-`; no generated logs or test data are in this folder.

## Repeatable checks

Use Python 3.14 with the title's requirements installed, and run from the repo
root. All generated outputs and game data must remain outside source.

```sh
python -B -m unittest discover -s tests
python -B games/emio/tests/run_tests.py
make -C games/emio WORK=/path/to/host BACKEND=metal4
/path/to/host/build/emio-native-metal4 /path/to/host/rewritten probe
MTL_DEBUG_LAYER=1 /path/to/host/build/emio-native-metal4 \
  /path/to/host/rewritten probe-gpu-depth-feedback
```

Close games before GPU probes; use a separate native test profile. Packaging
commands are in the title README. Title tests use synthetic inputs and temporary
external directories: four Python tests plus native clock/DST, gesture ABI,
keyboard lifecycle/Unicode, preference validation, and simulated input mapping.
Physical controller families and reconnection have not had a complete sweep.

Source review also checked Python parsing, translator checksum, forbidden game
file paths using the parent's content hook, symlinks, cross-game dependencies,
and whitespace errors. Inherited shader-tool documentation was corrected to
local paths; inherited shader-cache validation claims are not claimed here.

## Remaining limits

Photo capture previously reached an unsupported geometry-stage shader. One Mii
dream-bubble transition aborted on a null shader entry; the bubble disappeared
before the instrumented retry, so that cause remains unverified. Later play
without a crash does not establish a fix. Temporary dream-crash diagnostics are
kept on the development branch and omitted from this proposed contribution.
Broader picture accuracy, long gameplay sessions, save round-trips, incomplete
services/audio effects, and guarded Mii/cloth heap changes still need review.
The contribution remains experimental with these gaps. Normal packaged diagnostics
are off by default (`EMIO_DEBUG_LOG=1` opts in).

## Interactive title bring-up (Emio 1.0.0)

Autoplay to title only (A advances the system-data dialog; no input on the title menu itself; stop at title, do not enter Begin Investigation/Settings as stages). Private evidence (frame PNGs, logs) stays out of git; described here by log hash, not attached.

- System-data creation dialog → A → `SAVEDATA_JPN.DAT` written (13 MB) → title movie (62 VP9 frames, libvpx, 0 errors, EOS, no loop) → movie-pipeline teardown (`SendMessageQueue(quit=2)` → consumer exits → `WaitThread` join → `DestroyThread` → `FinalizeMessageQueue`) → interactive title (`Begin Investigation / Settings`, 1920×1080, frames sustain, main running not parked).
- Dialogs render (system-data prompts), BGM/voice packs open (silent output), input polls (controller disconnect reads), A advances state machine through dialogs. Title responds to input polling (menu present; A on title not pressed per stop-at-title bound).
- No faults, no aborts on the title path. RomFS serves title assets with no fallback and no NOT_FOUND. `0xFF` writer canary clean on title path (writer still at large globally — entry blocker for config/save/load stages, listed below).

### Host sync (shipped: deletion of a divergence, validated by title)

`SVC 0x1C` now always `lock_handoff` under G — restoring `tomodachi`'s main pattern. Record correction: an earlier `FREE=1` free+wake-all path replaced handoff and reopened a gap main had closed; the "wake-all is load-bearing" finding is retracted as an artifact of that divergence, not a property of the contract. Retained: B5 stale-free predicate (skip park if `[addr]==0` so a stale read retries CAS) and null-safe 0x1D logging. Guarded by ported `t_condvar` (both orders) in `run_tests.py`. Validated: 15/15 titles, zero aborts, no bypass/force-signal/stub-return.

### Teardown fix (shipped, flagged guest-behavior change)

Consumer echo looped back into its own queue (`+0x230`), regenerating work and starving quit Send (queue pinned full) — feedback starvation, not a missed wake. Fix: `main+0xF00E8` bl→nop (drop echo; queue drains, quit dispatches, thread exits, join succeeds). The `+0x48` vtable is guest SDK (method `0x118F708C0`), so this masks no host bug. Echo purpose beyond flow unknown; revert is pre-authorized suspect #1 on message-loss regression.

### CUTs (flagged)

- `main+0xE1BC0`: CUT REMOVED for M2 A/B (genuine display validation via `GetNpadStyleSet`; batch in progress vs 15/15 baseline). Was `mov w0,#1;ret`.
- `main+0xE2450` motion-sensor init skipped (`ret`). Dead-on-path not claimed; docked title uses buttons. Revisit with mode mapping.

### Watchpoint negative (measured, not retried)

`mprotect`+SIGSEGV+rearm is unworkable here: arming a lock-containing queue page stalls the producer 0/2 vs 14/14 baseline (exclusive-monitor vs fault/rearm interaction), and arming a lock-free scratch page yields zero faults on a direct RO write (protection ineffective in this host). Filed as a negative with numbers. G2 proceeds via self-tested scanner (`ffscan` unit: fires on `0xFF` runs, silent on address bytes) over sync dumps (6 dumps, 0 hits, limited coverage stated) plus hash seams at the host fs path in M2 Phase 3. Save structures unplayed.

### Known issues (stop conditions for later stages)

- Pre-movie `#1` startup race (~10% residual): waiter vs lock-free signaler read, kernel-less losing branch; needs guest-semantic change. Does not block title at 15/15.
- `0xFF` writer: sync-dump-clean (limited); save structures pending M2 Phase 3 with hash seams + ffscan.
- BGM/voice silent output (packs open, decode ok). `0xE2450` CUT above.
