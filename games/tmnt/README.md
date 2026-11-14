# Teenage Mutant Ninja Turtles: Shredder's Revenge — work in progress

This is an experimental Apple silicon Mac port with an [iPad target](ios/README.md). The host, graphics, audio and
private-input build flow are present, but the source in this contribution has
not been validated as a complete playable application.
Never commit game content, generated binaries, keys, shaders or saves. Use
your own game files and keys.

Only the base game labeled 1.0.0 with the exact original ExeFS and RomFS hashes in
`assets/expected-base-1.0.0.json` is supported. The extractor checks this table
before writing modules; the rewrite and RomFS extraction check it again against
the extraction manifest. The table comes from one locally verified base input;
other revisions must be reviewed and added explicitly rather than accepted by
title ID alone.

On an Apple silicon Mac with Xcode command-line tools, install the pinned Python
requirements and run from the repository root:

```sh
python games/tmnt/build.py '<base game.nsp or .xci>' --keys '<prod.keys>' \
  --work '<fresh work directory>' \
  --out '<fresh private output>/TMNT.app'
```

To include system fonts, pass `--shared-fonts` with the verified font directory and its manifest.
The builder verifies and extracts the game and obtains the static Opus and VPX
libraries, SPIRV-Cross CLI and .NET SDK from the repository's pinned dependencies.
TMNT decodes Vorbis movie packets with its embedded stb_vorbis. The inherited
movie fallback is built without its optional external Vorbis decoder.
For existing verified extractions use `scripts/build_private_bundle.py --help`.

Its private game inputs live under
`Contents/Resources/game`; `Contents/MacOS/PlayTMNT` sets the same graphics,
shader and input options as the directory launcher. Saves and shader cache live
under `~/Library/Application Support/MacNativeGames/TMNT-<runtime-hash-prefix>`, separate
from the original game and older test bundles. The `.app` is a local private
build, not a redistributable package. `--link-romfs` creates a path-dependent
app that saves disk space; omit it to copy the verified RomFS into the app.
The build runs a no-guest link check, which does not start the game. The
application still requires a separate visible startup/gameplay check.

The supported base image contains both Simplified and Traditional Chinese text
and glyphs, but its `title.dat` catalogue lists only seven other languages.
Private packaging generates a hashed `romfs-patches/title.dat` overlay adding the
game's `cn` and `zh-hant` identifiers; it keeps the default and existing languages
and leaves the verified RomFS untouched. Both Mac launchers load this overlay,
and iPad resource packaging verifies and copies it before signing. Rebuild older
private bundles to include it. The game follows the host language or its saved
in-game language option; `TOTK_LANG=zh-Hans` / `zh-Hant` explicitly selects either
script for diagnostics. Automatic selection respects `Hant` and Taiwan, Hong
Kong and Macao locale preferences, with an explicit `Hans` script taking priority.

This rebuilds the original-input rules, NRO, resource tables, host and shader
tools, validates the private bundle, and runs a no-guest link check. The bundle
contains copyrighted game data and must stay on your machine. Its launcher
keeps saves in its own `user-data` directory. `--link-romfs` saves disk space
but makes the bundle depend on the original RomFS path. A successful build or
link check is not a gameplay test. The historical investigation below records
earlier isolated stages and does not describe the current host's test coverage.

The examined base NSP has application ID `0100FE701475A000`; the provided update and DLC have matching application-family IDs, but their version and contents have not been independently verified and neither is supported by this extraction flow. The base NPDM indicates a 64-bit process. The executable contains NVN and MonoGame-related strings; this inventory does not establish compatibility.

The ExeFS reader checks the NCA filesystem header and all ExeFS data blocks. The RomFS reader checks all six IVFC levels. RSA signatures are not checked. The extraction manifest is a local record, not a signed provenance guarantee. The supplied base passed both checks and its RomFS contains 2,567 indexed files. See `third_party/COPIES.md` for copied code and licenses.

From the repository root, with dependencies from `requirements.txt`, use new private directories on a disk with enough free space:

```sh
python games/tmnt/scripts/extract_exefs.py '<base.nsp>' --keys '<prod.keys>' --output '<private-dir>/exefs'
python games/tmnt/scripts/extract_romfs.py --keys '<prod.keys>' --manifest '<private-dir>/extraction-manifest.json' --output '<private-dir>/romfs'
python games/tmnt/scripts/rewrite_modules.py --exefs '<private-dir>/exefs' --out '<new-private-dir>/rewritten'
```

The rewrite is exploratory, not a verified or launchable artifact. Its report fingerprints the inputs, rewriter sources, relevant dependency versions, options and generated outputs; these fingerprints are local records rather than signed provenance. Independently verify structure and semantics before use; a successful conversion does not imply correct execution. `rtld` is not converted by this experimental script. Update/DLC overlays, graphics, audio, services, initialization, input and saves still need implementation and end-to-end testing.

For the inspected base, a private initial rewrite of `main`, `sdk`, `subsdk0`, `subsdk1` and `subsdk2` passed the independent structure checker. Per-site randomized equivalence checks passed 49,551 sites with 300 negative controls per module. On `subsdk2` the first run missed one conditional-instruction mutation; a second run with at least 20 effective trials per site caught all 300 mutations. This is sampled local evidence, not a proof of whole-program behavior. The initial five-module rewrite was started before the current full fingerprint report existed; it is not an input for a production build.

The prepared NRO was rewritten in a separate private experiment and passed structure and per-site randomized equivalence checks at 14,580 sites and 300 negative controls. A subsequent rewrite through `rewrite_nro.py` produced matching artifacts. The NRO's real loading, relocations and initialization have not been tested.

`scripts/module_metadata.py --exefs '<private-dir>/exefs' --rewritten '<private-dir>/rewritten' --out '<new-private-file>'` prepares private symbol and relocation metadata for the five normal modules only when the input and complete rewrite report match. It does not cover `rtld` or the NRO, establish binding order, or execute a loader. An independent static survey checked the kinds and writable bounds of all 704,954 relocation records in those five modules; `tests/integration_metadata.py` compared each record against a separate reader.

`runtime/relocate.c` provides an isolated, fail-closed relocation primitive for the four observed kinds. `tests/integration_relocations.py --exefs '<private-dir>/exefs'` verified all 704,954 records against zero-filled synthetic buffers and synthetic symbol addresses. It did not resolve actual guest imports, run code, or test the NRO's relocations.

`runtime/image.c` maps verified after-image layouts into private non-executable memory, with a separate sealing primitive tested on synthetic data. A private integration probe checked all five fingerprinted rewritten modules in non-executable mappings; it did not execute them or validate the NRO. The five-module fingerprinted rewrite was regenerated and its artifacts match the earlier structure and per-site test inputs. Private loader metadata was generated, but has not been connected to a launcher.

`tests/integration_loader_primitives.py` joins the private mapping and relocation checks against the fingerprinted five-module output. All 704,954 relocation records were applied using synthetic symbol addresses, while code and patch areas were checked unchanged; no game code was executed. Real guest symbol resolution, the NRO, and host services remain untested.

`runtime/gates.c` is a standalone dispatch-table primitive with synthetic tests. An isolated probe routed the five modules' 236 gate records to fake handlers (152 system-call entries and 84 counter entries, including both identical SDK modules). It is not connected to an assembly bridge, actual system services, or game startup; these counts do not show which calls the game actually makes.

`runtime/symbols.c` provides a separately tested load-order lookup primitive with local, hidden, protected, absolute and weak-unresolved cases. It is not yet integrated into a verified game loader; tests use synthetic symbols and do not prove real game binding.

Developer-only per-site checks use `requirements-dev.txt`; Unicorn is not part of a game runtime.

`scripts/survey_linkage.py --exefs '<private-dir>/exefs'` inventories relocation kinds and candidate guest exports without emitting symbol names. For the inspected base, all six modules including `rtld` contain 704,991 relocation records. The scan found 7,679 undefined references with multiple candidate guest providers, all including both `sdk` and `subsdk1`; those two extracted modules have identical file hashes. It found 554 references without a candidate, including 40 associated only with weak imports and 514 not proven weak; those 514 are all type 1026 (JUMP_SLOT). These are static candidates, not a validated resolution order, proof of an error, or a list of required host services. Loading and binding semantics must be established before startup.

Adding `--plt` finds that all 514 of those slots are in `main` and have a standard PLT stub that passes the slot address in x16, a precondition of the SSBU port's lazy-binding bridge. A standalone copy of that bridge passed a synthetic native test covering integer, floating-point and stack arguments. This static pattern match does not show that every call uses the stub or that the bridge is correct for this game; the resolver, host-stack transition, unload handling and launcher integration are not implemented here.

Adding `--romfs '<private-dir>/romfs'` first verifies the indexed image digest, then includes exports from NRO files as hypothetical candidates. The inspected RomFS contains one NRO and one NRR; the NRO's SHA-256 is listed in the NRR. The NRR signature is not checked. Including that NRO's exports leaves 40 references without candidates, all associated only with weak imports. That does not establish when the NRO is loaded, whether its exports are visible to the other modules, whether its relocations resolve, or whether it can run. `scripts/prepare_nro.py --romfs '<private-dir>/romfs' --out '<new-private-dir>'` verifies the image and registration digest and prepares a synthetic NSO container for offline rewriting; `scripts/rewrite_nro.py --prepared '<prepared-dir>' --out '<new-private-dir>'` rewrites it and fingerprints the artifacts. Neither script implements dynamic loading. The SSBU port's self-contained NRO build and runtime provide a possible starting point, but their behavior has not been validated for this game.

The NRO itself has 55,914 relocation records; the static candidate scan found four references without a candidate among the inspected modules, all associated only with weak imports. `tests/integration_nro_primitives.py` mapped the verified private NRO rewrite without execute permission and checked every relocation against synthetic symbol addresses. This does not establish actual bindings or execution order.
