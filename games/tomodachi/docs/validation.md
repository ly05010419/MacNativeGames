# Contribution validation — 2026-10-31

## Scope

Parent base: `92ec79bd10c006e1cb7ca32d7ac1d8f66c758a15`. This is a source-only,
experimental Mac port of Tomodachi Life: Living the Dream 1.0.0
(`010051F0207B2000`). Builds verify the original input identities. The player
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
`/tmp/tomodachi-upstream-`; no generated logs or test data are in this folder.

## Repeatable checks

Use Python 3.14 with the title's requirements installed, and run from the repo
root. All generated outputs and game data must remain outside source.

```sh
python -B -m unittest discover -s tests
python -B games/tomodachi/tests/run_tests.py
make -C games/tomodachi WORK=/path/to/host BACKEND=metal3
make -C games/tomodachi WORK=/path/to/host BACKEND=metal4
/path/to/host/build/tomodachi-native /path/to/host/rewritten probe
/path/to/host/build/tomodachi-native-metal4 /path/to/host/rewritten probe
MTL_DEBUG_LAYER=1 /path/to/host/build/tomodachi-native \
  /path/to/host/rewritten probe-gpu-depth-feedback
MTL_DEBUG_LAYER=1 /path/to/host/build/tomodachi-native-metal4 \
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
are off by default (`TOMODACHI_DEBUG_LOG=1` opts in).
