# Persistent Metal libraries on macOS (PR #84, part B)

An already translated shader variant can still call Metal's source compiler on every launch when it has no matching
library in the App. This optional cache lets the runtime load a library compiled locally from those exact source
bytes, without repackaging the App. The warm-up scope (part A) was merged separately in `671905c`; it is the baseline
for this change.

## Preparing and using the cache

`games/totk/scripts/prepare_shader_libraries.py` reads the selected game's verified `shader-keys.json` and matching
observations from the translation cache. Only complete MSL/reflection pairs from the current translation chain and
matching stages are selected. The chain comes from `dmsh_chain.h`, so a chain upgrade does not relabel older cache
entries. Exact source hashes already present in the App are skipped. Use the installed Metal compiler and an
existing isolated Python environment; all inputs, libraries and reports below are outside the source checkout.

```sh
cache="$HOME/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v5-unverified"
uv run --offline --no-project --python /path/to/TOTK_build/venv/bin/python \
  python -B games/totk/scripts/prepare_shader_libraries.py \
  --cache "$cache" \
  --inventory /path/to/TOTK_build/totk/build/shader-seed/v121/shader-keys.json \
  --app-libraries '/path/to/Tears of the Kingdom.app/Contents/Resources/build/shader-libs' \
  --output "$cache/metal-libraries" \
  --compiler /path/to/Metal.xctoolchain/usr/bin/metal --jobs 2 \
  --report "$HOME/Library/Application Support/MacNativeGames/TOTK/tests/library-preparation.json"
```

Use the seed and cache produced by the same current build. `--dry-run` reports coverage without writing output.
Compiling again reuses successful outputs and adds only missing source hashes. A process lock prevents concurrent
writers; successful outputs and the manifest are published atomically. Compiler identity and safe-math options have
separate directories. Failed outputs do not replace valid libraries or enter the manifest. Inputs are read only.

On the next launch the Mac renderer tries the App's exact source-hash library, then the user manifest under
`<translation-cache>/metal-libraries`, then its existing source compiler. `TOTK_SHADER_LIBRARIES=<directory>` selects
another manifest root; `TOTK_SHADER_LIBRARIES=off` disables the user lookup. A malformed manifest, wrong chain,
missing file or Metal load failure leaves the source fallback available. `TOTK_FAST_MATH=1` continues to disable
safe-math precompiled libraries. The manifest is read once per process; restart after preparing more libraries.

This is a manual maintenance tool. The packager and launcher do not run it automatically. It does not predict
unknown variants, create Metal pipelines in advance or remove all first-use stalls. No isolated part-B gameplay or
startup speedup is claimed here; the earlier combined A+B timing cannot measure part B alone.

## File descriptors and iOS scope

The new path is compiled only for macOS. `totk_shader_library_load` uses the same `mmap` → `close` →
`newLibraryWithData` ownership pattern as the existing Mac loader. The mapping lives with its dispatch data and is
unmapped by that object's destructor. Runtime loading and the diagnostics share this helper; they do not use
`newLibraryWithURL` for this cache. Mapped files still consume other virtual-memory/vnode resources: a flat file
descriptor count is not evidence that thousands of libraries have no resource cost.

`check_shader_library_fds.m` lowers only its own process's soft descriptor limit to 256, loads distinct on-disk
copies of one tiny synthetic kernel, and retains every library and reflected function through the final sample.
It creates no command queue and renders nothing. On the test Mac, 2026-10-20:

| Loader | Libraries and functions retained | Baseline FDs | Peak FDs | Final FDs |
|---|---:|---:|---:|---:|
| Production memory loader | 512 each | 4 | 4 | 4 |
| File URL control | 64 each | 4 | 68 | 68 |

This measures descriptors on macOS, not iOS vnode pressure. `TARGET_OS_IPHONE=1` conditional-source selection
excludes the new header, parser, loader, environment lookup and counter. The selected renderer body matches current
upstream after removing blank lines and indentation. There is no iOS SDK or iOS device on this machine; neither an
iOS build nor iOS runtime resource usage has been tested. The existing iOS library-loading path is unchanged.

## Validation and reproduction

Run the focused tests with the isolated build environment. `MNG_TEST_METAL_COMPILER` selects an installed offline
compiler for the real-Metal retained-library test; that test skips when the compiler or device is unavailable.

```sh
MNG_TEST_METAL_COMPILER=/path/to/Metal.xctoolchain/usr/bin/metal \
  uv run --offline --no-project --python /path/to/TOTK_build/venv/bin/python \
  python -B -m unittest discover -s tests -p 'test_shader_librar*.py' -v
```

The 21 focused tests passed on the Mac, including real Foundation manifest parsing, incremental compilation,
compiler/source/chain separation, invalid inputs, platform conditionals and the 512-library Metal resource test.
The complete current macOS renderer also passed a syntax compilation. For manual GPU diagnostics, acquire the
project's shared game slot first so tests do not overlap the user's game.

`scripts/diag/check_shader_libraries.m` validates each matching real library and its reflected function against a
local translation cache. Its `--source` mode measures the source API separately; those timings are not game FPS.
`scripts/diag/check_shader_library_fds.m` supplies the descriptor stress test and optional `url` control. Build both
executables outside the checkout, linking Foundation and Metal with the NVN header directory on the include path.

Local raw evidence is outside the source tree at
`~/Library/Application Support/MacNativeGames/TOTK/tests/pr84-part-b-20261020` (`fd-results.json`, loader logs and test
logs). Conflict snapshots and compiler/conditional-selection checks are under
`/private/tmp/macnativegames-pr84-part-b-20261020`. Only source, tests and this explanation are committed.
