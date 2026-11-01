# Xenoblade Chronicles 3: facts per version

Only numbers, names, sizes and hashes measured on the player's own dump. Never game content. Mark each fact with how it was measured.

## Titles

| | Title ID |
|---|---|
| Base game | `010074F013262000` |
| Update | `010074F013262800` (confirmed with 2.2.1) |
| DLC | `010074F013263xxx` (out of scope) |

## 1.0.0 (v0)

### NSP container
PFS0 header, measured 2026-10-28.

| Entry | Size |
|---|---|
| `010074f013262000000000000000000d.tik` | ticket (common ticket, key generation `0x0d`) |
| `010074f013262000000000000000000d.cert` | cert |
| `46a2aa0a7252277a55feb06fc47abe98.cnmt.nca` | meta |
| `edc8bdc59243b32462735c322cbb451a.nca` | 14,808,186,880 bytes at offset 38,848: Program (ExeFS + RomFS), confirmed; key generation 13, SDK add-on 12.3.9 |
| `dcd32f0ca28a882a4b95614a9e0d7ee2.nca` | 0.9 MB: probably Control (`control.nacp`, icon) |
| `1bbc27a5b1fa6cf1d9fd019463203af7.nca` | 0.2 MB: probably LegalInformation |

### ExeFS (stage 2)
Measured 2026-10-28 with `scripts/extract_code.py` and a header read. SHA-256 values are in `assets/expected-v1.0.0.json`.

| Module | NSO bytes | text | ro | data | bss | Build ID (first 20 bytes) | Notes |
|---|---|---|---|---|---|---|---|
| `main` | 11,612,145 | 16,420,880 | 7,539,928 | 760,168 | 8,369,816 | `60887A25B66529910275A1066003A00217914F5E` | 31.6 MiB image (small, so `b` reaches any patch area); needs `multimedia.nss`, `nnSdk.nss`; 410 imports, 29 exports, 526 `nvn` strings |
| `subsdk0` | 3,385,976 | 3,406,400 | 2,153,716 | 197,528 | 1,162,344 | `BE1EE79A9968A35A6B63FC8F1036A6D4447F4AC3` | the SDK multimedia library (`NintendoSDK_movie-12_3_9`); 658 imports, 10,895 exports |
| `sdk` | 5,786,666 | 5,831,072 | 6,920,376 | 418,784 | 760,864 | `E8CDCEB1E77A8465FE20F21928E8D296EEA3117E` | `nnSdk`, NVN 12.3.9; 76 `svc` candidates |
| `rtld` | 7,117 | 5,904 | 3,384 | 664 | 3,432 | `E79AB9AF2AB0517496B6C5630B5C7A69769806D4` | 4 `svc` candidates |
| `main.npdm` | 1,572 | | | | | | name `Application`, 64-bit, **address space type 3 (39-bit)**, system resource size 32 MiB, main thread priority 44 on core 0, stack 1 MiB |

- The modules are the same set as Octopath Traveler 0 (`main`, `subsdk0`, `sdk`, plus `rtld`), so ot0's load order applies.
- SDK 12.3.9 (`NintendoSDK_NVN-12_3_9`, `NintendoSDK_libz-12_3_9`, `NintendoSDK_movie-12_3_9`).
- `main` contains one mention each of `Wwise` and `FMOD`. The RomFS shows Wwise is the sound engine.

### RomFS (stage 2)
Measured 2026-10-28 with `scripts/merge_romfs.py` in base-only mode.

- 14,758,271,292 bytes, 246 files, SHA-256 `d96ca057…5091a`. All six IVFC levels verified (1, 1, 1, 4, 1,760 and 900,774 blocks). The section is plain AES-CTR, not sparse.
- File kinds:
  - 238 `.webm` files under `movie/`: VP9 video through the SDK `movie` library, so `runtime/movie_host.c` and libvpx matter in stage 6.
  - 5 `.pck` files under `sound/`: Wwise file packages.
  - One `bf3.ard`/`bf3.arh` pair: the engine's own archive (data and header), which holds almost everything else.
  - One `.bin`.
- Movie format (stage 6): WebM (VP9 expected; confirm the codec in stage 6).
- Audio middleware (stage 6): Wwise. It mixes in game code and outputs through `nn::audio`.

### Control data (control.nacp, stage 5)
- Display version 1.0.0.
- Supported languages (`0x70de`): British English, Japanese, French, German, Spanish, Italian, Korean, Traditional and Simplified Chinese. **No American English.** Text and voice folders are `gb` / `en`.
- User save 10 MiB, journal 10 MiB; size maximum 0 (the save never grows).
- Icons are present for the same languages.

### Rewrite (stage 3)

Measured 2026-10-28: `DM_RULESET=native DM_PRIVATE_GATES=0`, no cuts and no observation gates for these build IDs.

| Module | sites | x18 | `tpidrro_el0` read | `tpidr_el0` read / write | `svc` | `cntpct` | windows | jump tables | gates | patch area | time |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `sdk` | 11,502 | 11,319 | 65 | 2 / 3 | 76 | 33 | 4 | 8 | 109 | 485,124 bytes after the image (`PATCH_OFF` 0xd50000) | 7.2 s |
| `subsdk0` | 10,525 | 10,525 | 0 | 0 | 0 | 0 | 0 | 4 | 0 | 1,217,448 bytes after the image (0x6a0000) | 4.7 s |
| `main` | 4,944 | 4,944 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 198,132 bytes after the image (0x1fa0000) | 18.0 s |

- Only `sdk` talks to the kernel (`svc`, counters, TLS). `main` and `subsdk0` only use x18.
- Every patch area fits after its module, so the before-the-module placement inherited from ot0 is not used.

### Runtime (stages 4–5)
- Constructors run before `nnMain`: main 805, subsdk0 45, sdk 112 (2026-10-28).
- Guest memory: the game fills its pool in 1 MiB `AllocateMemoryPages` blocks until one fails at about 3.16 GiB. `GUEST_ALIAS_HIGH` is kept.
- Threads soon after `nnMain`: 39 `CreateThread`.
- Swap chain: 2 images, 1920×1080, format 0x25, interval 1.
- NVN names loaded / implemented: see [nvn-coverage.md](nvn-coverage.md).

## 2.2.1 (v720896)
Measured 2026-10-29 from the player's update NSP (`Patch`, version 720896 = 0xB0000; the display version 2.2.1 is read from its `control.nacp`, not the file name).

| Entry | Size |
|---|---|
| `fd6baab822ac7180838dfb09783e61b9.nca` | 598,929,920 bytes at offset 1,146,647: Program (full ExeFS + BKTR RomFS patch) |
| `d0517148fff72b2fd3ec73e334215b91.nca` | Control: display version 2.2.1, same languages, user save 20 MiB (1.0.0: 10 MiB), journal 10 MiB |
| `abcce21dcf308a4c575e595cb44f8939.nca` | LegalInformation |
| `df432498aea49529524a1e87261c4ab4.cnmt.nca` | meta |

- **Code:** only `main` (13,506,612 bytes NSO, build ID `82D187FE9EF9BE921B8E1B1CDDF7BD287CDB22C1`) and `main.npdm` change. `sdk`, `subsdk0` and `rtld` are byte-identical to 1.0.0, and so are their rewritten images.
- **Rewrite:** `main` has 5,065 sites (1.0.0: 4,944); `structck` OK, `equiv --min-eff 8` 0 divergent sites, 300 of 300 mutations caught.
- **RomFS:** BKTR merge with the base: 15,194,654,528 bytes, 250 files (4 more than 1.0.0), SHA-256 `5c2491c6…10eb6`; 576,084,880 bytes come from the update. All six IVFC levels verified (927,409 level-6 blocks).
- **New SDK call:** `nn::oe::GetDisplayVersion` at start (see `services.md`).

## Add-on content (Expansion Pass)
Measured 2026-10-29 from the player's own NSPs (`scripts/extract_aoc.py --measure`). Each NSP holds a meta NCA and one data NCA; the add-on index is the title minus `010074F013263000`.

| Index | Title | Data NCA | RomFS | Files |
|---|---|---|---|---|
| 1 | `010074F013263001` (v0) | `d1ed81f1…`, 41,811,968 bytes | 41,641,984 bytes | 2 (`bf3_dlc01.arh` / `.ard`) |
| 2 | `010074F013263002` (v65536) | `96c25737…`, 211,271,680 | 210,749,168 | 6 |
| 3 | `010074F013263003` (v65536) | `4ecf19cc…`, 189,874,176 | 189,402,144 | 6 |
| 4 | `010074F013263004` (v65536, Future Redeemed) | `4ff0ef8a…`, 4,282,007,552 | 4,273,556,608 | 80 |

The game mounts them as `aoc1`–`aoc4` at start (2.2.1).
