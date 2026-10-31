# Player mods

[English](#english) | [中文](#chinese)

<a id="english"></a>
## English

A mod reaches the game in one of two ways:

- **RomFS content** is loaded while the game runs. Whether a mod is used is decided by one file:
  `manifest.tsv`. The app does not scan folders for mods.
- **Code patches for the main module** (`exefs/main.pchtxt`, `exefs/*.ips`, `exefs/*.ips32`) are merged into
  the app **at build time** with `--mod`. See [Code patches](#code) below.

Nothing under `exefs/` is loaded while the game runs: the app runs the three code modules it was built from,
rewritten for Apple silicon and bound to their SHA-256. The build is the only place code can change.

The launcher points `TOTK_ROMFS_MODS` at

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

where `<version>` is the game version the app was built from: `v0` (1.0.0), `v112` (1.1.2), `v121`
(1.2.1) or `v143` (1.4.3).

When no controller is connected at start and `mods/<version>-keyboard/manifest.tsv` exists, the launcher points
`TOTK_ROMFS_MODS` at that folder instead, so the prompts show the Mac keyboard's keys while you play on the keyboard:

```sh
python3.14 scripts/make_button_icons.py "$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v0-keyboard" \
  --app "Tears of the Kingdom.app" --version v0 --style azerty   # or --style keyboard for a US layout
```

The choice is made once per start: connecting a controller later keeps the keyboard icons until the next start.

### What `manifest.tsv` is

Only the files listed in `manifest.tsv` are served. Each line has four tab-separated fields:

```text
<RomFS path>	<size>	<SHA-256 of the mod file>	<SHA-256 of the original, or ->
```

At startup the loader checks each listed file's size and SHA-256, and checks the original's SHA-256
against the game image. `-` in the last field adds a file the game's RomFS does not have (used by mods
that add assets). Paths are case-sensitive and keep the RomFS spelling.

A folder without `manifest.tsv` means "no mod". Files copied into the folder but not listed are not
used. In detail:

- A **new path** that is not listed is treated as absent: the game cannot see the file.
- A **replacement whose size or hash changed** without regenerating the manifest stops the start with
  `RomFS overlay file missing or wrong size` or `RomFS overlay integrity mismatch`.
- A path that exists in the game but is not listed is read from the game, so the copied file has no
  effect.

### Install a mod

A build can perform steps 2 and 3 with `--mod <mod folder> --install-mod-romfs`. That flag replaces the version
folder whole (what it held is kept next to it as a backup - see [the mod's RomFS files](#data)); without it the build
leaves the folder alone
and prints the two commands that fill it by hand. The steps below are the manual route either way.

1. Quit the game completely.
2. Copy the mod's RomFS paths **directly** into the version folder. If the mod ships a `romfs/` folder,
   copy its contents, not the folder itself:

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. Generate the manifest with the built app. Run this from the repository root:

   ```bash
   python3.14 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   The script reads `Contents/Resources/totk/extracted/<version>/romfs` from the app, hashes each file
   and its original, leaves out files that are identical to the original, and writes `manifest.tsv`.
   It prints a line such as `...: 12 files (10 replaced, 2 added)`.

   `--check`, `--exclude` and `--version` belong to this script, not to `build_app.py` (which only takes
   `--mod` and `--install-mod-romfs` and runs the script itself, `--check` included, whatever flags were passed):
   `--check` reports what the manifest would hold and writes nothing, `--exclude` leaves a path to the app's own
   version of it.

4. Start the game and check the log:

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   It should say `.../mods/v121 checked, N files`; additions also print `M files added to the RomFS`.

### Switching versions

The game reads the installed folder as it is; it does not follow the folder that was downloaded or unpacked.
If the new version removes files, set the old version folder aside (or delete it after a backup) before
copying, or the removed files stay active. Generate the manifest again after every change.

<a id="code"></a>
### Code patches (build time)

`--mod <mod folder>` merges a mod's main-module code patches into the app while it is built. The app has no module
loader: at start it maps the three code modules it was built from (main, sdk and subsdk0), rewritten for Apple
silicon and bound by SHA-256. The patches are applied to the verified retail main of the version being built before
that rewrite, so the app still matches the hash it is bound to; RomFS (data) mods are unaffected and are still
loaded at start.

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" --mod "/path/to/Mod"
```

Several mods with code patches are combined in a mod manager first: TKMM's merged output is one folder with one
`exefs/` and one `romfs/` (a manager may also pack several mods' records into one `.ips`), and `--mod` takes that
folder as it is. Two patch files that change the same bytes are refused with both names, so a merge that overwrote
one mod's change with another's stops the build instead of shipping silently.

The folder holds the mod's files in the usual layout - `exefs/` for code, `romfs/` for data - and either half is
enough: without `exefs/` there is no code to merge, and without `romfs/` the version folder is never touched. An
`exefs/` that holds nothing to merge into main (`subsdk9`, a `main.npdm`) is a note, not a failure: the build
carries on, its code half is in the app, and its data half takes effect once the folder is installed
(`--install-mod-romfs`, or by hand). With `--update` the patch is merged into the updated module, which is the one
the app runs.

### Code patches at launch (`TOTK_PCHTXT`, Mac)

A main `.pchtxt` can also be applied when the game starts, without a build:

```bash
open --env TOTK_PCHTXT="/path/to/mods.pchtxt" "/path/to/Tears of the Kingdom.app"
```

It is read the way `--mod` reads it, and its `@nsobid` must match main's build ID, which a built app keeps in
`extracted/<version>/exefs/main.build-id`. The rewriter doesn't see these bytes, so a patch that makes system calls,
uses the thread-pointer or counter registers, or uses x18 still needs a build with `--mod`. A line on a site the app
patches itself stops the start.

<a id="data"></a>
### The mod's RomFS files: `--install-mod-romfs`

A build does not write to the version folder (`~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`;
Breath of the Wild's is `BotW/mods/botw160` or `botw190`, see [below](#breath-of-the-wild)) unless this flag is given: the folder may hold another mod's files, or files copied by hand. With the flag, the
build replaces it with the mod's `romfs/` and writes its `manifest.tsv`:

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" \
  --mod "/path/to/Mod" --install-mod-romfs
```

Then, at the very end, the build:

- **replaces** `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>` with a copy of the mod's
  `romfs/`, so after the build the folder holds exactly this mod's RomFS and nothing else. What it held
  before - another mod's files, files copied by hand - is not deleted: it is moved next to it as
  `<version>.backup-<date>-<time>` (the game never loads that folder; delete it yourself once you no longer
  need it). A mod whose files the game would all take from the app instead replaces nothing, and then the
  folder is left alone: see below;
- writes `manifest.tsv` with `scripts/make_mod_manifest.py` against the app just built, so a path the app
  replaces itself is left out (the heap table included: the 2K overlay carries the mod's heap sizes; the UI
  archive is merged instead, which leaves the mod's own copy next to it as `<name>.mod-original` - the game
  ignores it), and a misspelt path stops the build at its last step - the app is already written by then - with
  the reason instead of stopping the game later.

The folder is not touched until a complete replacement exists: the build copies the mod's `romfs/` into a folder
next to it, writes the manifest there, and moves the new copy into place only when it is complete (the old folder
becomes the backup). Before any of it, the manifest tool runs with `--check` on the mod's own folder, so a
misspelt path stops the build while the version folder still holds what it held; and a mod whose files the game
would all take from the app instead (every one identical to the original, or one the app replaces itself) replaces
nothing the game loads - the build says so and leaves the folder alone rather than swapping in a folder with no
manifest. The build warns when it starts and again right before the copy:

```text
--install-mod-romfs: the player's mod folder for the version this build makes will be REPLACED by The Second Quest
1.4.3/romfs (and its manifest.tsv) when the build ends; what it holds now is moved next to it as
<folder>.backup-<date>-<time>, not deleted. Without the flag the build leaves the folder alone.
...
.../mods/v143: replaced by The Second Quest 1.4.3/romfs and its manifest.tsv (--install-mod-romfs); what it held before is kept in .../mods/v143.backup-20261021-213000
4/4 app: .../mods/v143 holds 5250 files from The Second Quest 1.4.3/romfs now (what it held before: .../mods/v143.backup-20261021-213000; delete that folder once you no longer need it)
   left out (the app replaces it, and the 2K overlay already carries these heap sizes): System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
   .../mods/v143/manifest.tsv: 5229 files (4200 replaced, 1029 added)
```

Without the flag the build leaves the folder untouched and prints the two commands that perform the copy and the
manifest by hand.

Nothing changes at run time: the game loads this folder and its manifest as before. The iPhone / iPad build has
no player mod folder yet, so there `--mod` merges the code patches only (and `--install-mod-romfs` is refused
there: there is no folder to fill).

What the build does, in order:

1. Extracts and verifies the retail ExeFS, as always. The patch is only ever applied to the verified
   retail module of the version being built, never to an already rewritten one.
2. Applies the mod's patches. Offsets are **main addresses** - the address space the game's code lives
   in, the same numbers the app's own patch tables use:

   - `.pchtxt`: the community format, read the same way as the runtime's own `TOTK_PCHTXT` reader (section
     switches and `@flag` lines included, see below). The first line must be
     `@nsobid-<build id>`: the 16 bytes the rewriter's tables use (32 hex digits) or the 20 bytes the
     runtime compares (40 hex digits; the runtime's own `TOTK_PCHTXT` experiment takes only that longer
     form). Patch lines `<hex main address> <hex bytes>` follow, and only lines after `@enabled` count:
     `@disabled` switches the following section off again (a file that keeps an edit around without applying
     it), `@enabled` switches it back on, and `@stop` ends the file. A `@flag offset_shift 0x100` line must
     come before the first patch line - it says the addresses leave out the 0x100-byte NSO header an IPS form
     counts, which is how they are applied; a file without it, or with another shift, is refused by file and
     line rather than patched 0x100 bytes off. `@flag print_values` (a display option of the patching tools,
     in every community pchtxt we have seen) is accepted; any other flag is refused. The `@nsobid-` line identifies
     the target build, so the file name does not matter.
   - `.ips` / `.ips32`: applied as emulators apply them, whose offsets count the 0x100-byte NSO header,
     so the build subtracts 0x100. A record below 0x100, a truncated file or a missing `EOF` is an error.
     The file names neither its module nor its build, so it is read as a main patch only when it is named
     `main`, or after main's build id (`exefs/<build id>.ips`, the Atmosphere and mod-manager
     convention). Anything else is left out with a note: a patch for `sdk` or `subsdk9`, or one named
     after another build of main. An `.ips` named `main` has no id to check; naming it after the build
     id, or shipping a `.pchtxt`, provides that check.

3. Checks the result before building on it. The patch is refused, with the address and the reason, when
   it lands outside main's text, rodata or data (the padding after text is reserved), when two patch
   files change the same bytes, when it changes nothing, or when it touches a site the app patches
   itself - the 60 FPS and real-time frame-step sites, render/output resolution, ultrawide, cloth, and
   the rewriter's registered cuts. Those features are set by the app (`TOTK_FPS60`, `TOTK_REALTIME`,
   `TOTK_RENDER_RES`, `TOTK_OUTPUT_RES`, `TOTK_ASPECT`): the runtime re-checks the original instruction
   at each of its sites and stops the game if it changed.
4. Rewrites the patched module for Apple silicon and binds the app to its SHA-256. The report in `--work`
   (`analysis/phase12-validation.json` for 1.0.0, `analysis/<version>-port/phase12-validation.json`
   otherwise, under `mod_exefs`) records the mod folder, every patch file's SHA-256, the addresses it
   changed and the merged module's hash.

The iPhone / iPad build merges the same way: `--mod` with `--platform ios`.

What else a mod's `exefs/` may hold:

| In the mod's `exefs/` | Result |
| --- | --- |
| `*.pchtxt`, `*.ips`, `*.ips32` for main | merged into the build; an `.ips` must be named `main` or after main's build id, while a `.pchtxt` names its build in `@nsobid-` and may be named anything |
| `main.npdm` | ignored with a note: the native loader never reads a module's metadata |
| `subsdk9` and other extra code modules | **not loaded**, with a note in the build output and in the report: the app loads the three modules it was built from (main, sdk, subsdk0), so the module's own code never runs |
| a whole replacement `main` | refused: a patch is required instead, so the build can check it against this version |
| nothing for main (`subsdk9` alone, a `main.npdm` alone, even an empty `exefs/`) | noted, not refused: nothing is merged into main and the build carries on - its code is not in the app, and its data applies once the folder is installed |

An extra module is how a mod built with a hooking framework (exlaunch and the like) changes code while
the game runs: its trampolines never install here. Such a mod - The Second Quest is one - keeps its main
patches and its whole RomFS, but not the part of its code changes that lives in that module. Making one
work means the app growing a module loader (rewriting the module for Apple silicon and answering its
service calls), which this version does not do.

Changing the mod's code after a build means building again (the mod's bytes are part of the app), and a
build with `--mod --install-mod-romfs` also replaces its data folder. Changing only RomFS files needs no build:
copy them into the version folder and write the manifest again (steps 2 and 3 above). A merge that already
happened is reused: the rewrite is skipped while the recorded hash matches, and the record names the
rewriter's own SHA-256 too, so a changed `patch_nso.py` makes it rewrite instead of reusing the old image.
The 2K output overlay records the heap table it was built on: a build without `--mod` after a build with it
rebuilds the overlay from the retail table instead of keeping the mod's heap sizes.

### Large texture packs (8 GB mode)

Some texture packs are made for an emulator's 8 GB DRAM mode and do not fit the 3.2 GiB a retail console gives the game.
One such pack is the **[Tears of the Kingdom 4K Texture Pack by HDPacks](https://www.hdpacks.com/)** (made by
HDPacks.com, who offer it as a free download on their site; it is their work and is not part of this repository: get it
from them and follow their terms). Played on the Mac with 1.4.3:

1. Put the pack's `romfs` files (for the HDPacks pack, `4kTextures/RomFS`) into the version's mod folder
   (`mods/v143`), together with any other mod you use.
2. The pack's heap table and resource size tables are made for the version it was built for (the HDPacks pack: up to
   1.2.1). Fit them to the app's version:

   ```sh
   python3.14 games/totk/scripts/texture_pack_tables.py ~/Library/Application\ Support/MacNativeGames/TOTK/mods/v143 \
       --app "Tears of the Kingdom.app"
   ```

   It writes 1.4.3's own `HeapDef` with only the heaps the pack makes bigger raised (for the HDPacks pack: `TextureHeap`,
   `ゲームグラフィックス` and `草`), and 1.4.3's `ResourceSizeTable.Product.143` with every replaced texture's entry grown
   by as much as the file grew. Then write the manifest with `make_mod_manifest.py` as usual. Run both again whenever
   the folder changes.
3. Start the game in 8 GB mode:

   ```sh
   open --env TOTK_DRAM_8GB=1 "Tears of the Kingdom.app"
   ```

   `TOTK_DRAM_8GB` reports a 6.8 GiB application pool to the game (as the emulators' 8 GB mode does); the pack's
   `TextureHeap` pool, about 3.8 GiB, becomes one zero-copy GPU buffer.

Checked on a 24 GB M-series Mac: about 8.7 GB resident in play and the GPU near full load (the GPU is the
bottleneck). A Mac with less memory than that is not expected to run it.

### Breath of the Wild

The Breath of the Wild app reads its mods from the folder for its version, `botw160` for 1.6.0 or `botw190` for 1.9.0:

```text
~/Library/Application Support/MacNativeGames/BotW/mods/botw160
~/Library/Application Support/MacNativeGames/BotW/mods/botw190
```

The RomFS half above applies: copy the mod's RomFS contents into that folder and generate the manifest with the app,
naming the folder `botw160` or `botw190`, or passing `--version botw160` or `--version botw190` (1.6.0 shown):

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw160"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Breath of the Wild.app"
```

Check `grep "mods/botw160 checked" ~/Library/Logs/MacNativeGames/botw.log | tail -1` after the start
(`mods/botw190 checked` on 1.9.0).

- A mod that makes a resource larger than the original also has to ship the matching resource size table,
  `System/Resource/ResourceSizeTable.product.srsizetable`, built for the same version. Without it the game reserves the
  original size and stops at start (for example a translation whose `Pack/Bootup_<lang>.pack` grew).
- The game's language follows the app's preferred language, which also picks the language folder a mod must use (`USen` for
  `en-US`). To give only this game another language:
  `defaults write org.macnativegames.botw AppleLanguages -array en-US`.

### Notes

- Do not edit `manifest.tsv` by hand; regenerate it.
- Do not change files while the game runs. The loader records each file's identity at start and stops
  if a file changes mid-session.
- There is one overlay directory for player mods: to use several mods, merge their files into one
  version folder and generate one manifest. Do not nest mod folders. If two mods change the same path,
  choose one file.
- The build replaces a few RomFS files itself: `UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs` (quick-menu
  timing; `<n>` is 100, 110 or 140 depending on the version), `Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`, and with the
  2K output `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`. Two overlays cannot replace one path (the start would
  stop with "two overlays replace the same file"), so:
  - the **UI archive** is *merged*: a translation's archive keeps its textures and takes the build's quick-menu edit - the
    tool writes the merged archive in the mod folder, keeps the mod's own copy next to it as
    `<name>.mod-original`, and the merged file replaces the build's overlay (#73). It refuses when the mod changed the
    members the build patches (the capture animations) rather than silently dropping one side;
  - the **heap table** is folded in by the build: with `--mod` the 2K overlay is built on top of the mod's table;
  - any other one (the game-over flow) is left to the app, with a line saying which file it left out.
- `make_mod_manifest.py` handles all of them itself: the UI archive is merged as above, and any other mod file whose
  path the app replaces is left out of the manifest (with a line saying so), because both overlays cannot be loaded. For
  the heap table the line says whether the app's 2K overlay carries the mod's sizes (`already carries these heap sizes`,
  which the build arranges with `--mod`) or says to build with `--mod`.
- A mod manager (TKMM, or Ryujinx's `mods/contents/<title id>/<name>`) provides `exefs/` and `romfs/` in one folder:
  `--mod` takes that folder - its `exefs` patches are merged, and a manager's merged `.ips` may hold several mods'
  records in one file - and `--install-mod-romfs` has the build replace the version folder with its `romfs/` and
  write the manifest itself (no `--exclude` needed: the manager's heap table and the app's 2K overlay are reconciled).
  Re-merge in the manager and build again after every change: the game checks every file's hash
  at start. This is also how several mods with code patches are
  combined: merge them in the manager first, then build with `--mod <the manager's output folder>
  --install-mod-romfs`; the build refuses
  two patch files that change the same bytes and names both of them.
- A mod with a code patch also needs its `exefs/` patch to match the game version the app is built from.
  The build checks what the file itself says: a `.pchtxt` is refused when its `@nsobid-` names another
  main, and an `.ips` / `.ips32` named after a build id is left out when that id is not this main's.
- This guide covers the Mac app; installing mods into the iPhone/iPad app container is not covered.

### When the start stops

The game closes the window and writes one line to `~/Library/Logs/MacNativeGames/totk.log` instead of starting with data
it cannot trust. The line above it names the file. The log is off by default: turn it on with
`defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` (`org.macnativegames.botw` for Breath of the Wild) and start
the game again. The `grep` checks on this page need it too.

- `two overlays replace the same file` - the manifest lists a path the app replaces itself: a manifest written by hand
  or by another tool, since `make_mod_manifest.py` leaves those paths out by itself (the UI archive it merges instead).
  Regenerate it with that script.
- `two overlays add the same file` - two overlays (the app's, or two mod folders) add one path the RomFS lacks.
- `RomFS overlay integrity mismatch` - a file in the mods folder differs from the SHA-256 in the manifest: files were
  copied in after the manifest was written. Regenerate it (`make_mod_manifest.py` writes nothing for a mismatching dump).
- `RomFS overlay names a file the RomFS lacks` / `adds a file the RomFS already has` - a replacement's original is wrong
  (a file copied from another game version); regenerate the manifest and rebuild the app for that version.
- `RomFS overlay file changed while the game ran` - the folder was edited during play; restart the game (and regenerate
  the manifest).

The window can also vanish with no line of ours: a mod's data can make Metal abort the process (the log then has a host
call stack, for example `-[MTLTextureDescriptorInternal validateWithDevice:]` "requests 9 mipmap levels, but the
dimensions (1, 1, 1) can only support 1"). A texture with more mip levels than its size holds is something the Switch
never checks and Metal refuses to build; the app caps the chain at what the dimensions allow and logs
`[tex] <w>x<h>x<d> ... 声明 N 级 mip ...`, so check the mod's texture if the picture looks wrong.

### Three packaging traps that look like a crash

These come from how a pack was built, not from its content, and none of them prints one of the lines above.

- **A `.pchtxt` without `@flag offset_shift 0x100`.** Its addresses are `main` addresses, while an IPS file's offsets
  count the 0x100-byte NSO header. Both `TOTK_PCHTXT` and `--mod` stop the start when a patch line comes before that
  flag, or when it names another shift: the alternative is every byte landing 0x100 bytes off without an error.
  Converting an IPS by hand: `main address = IPS32 offset - 0x100`.
- **A `.zs` whose zstd frame does not declare its content size.** The game's decompressor has no size to read for those
  and decompresses to a null destination: the window dies right after `ENTER nnMain` and the log holds nothing of ours.
  `zstd` writes such frames when compressing from a pipe (`zstd -c`); the descriptor byte of the frame header (`raw[4]`)
  must have bits 7-6 set. Compress from the bytes with a library, assert `(raw[4] >> 6) & 3`, and keep the window size,
  checksum and dictionary of the file being replaced (`zstd -l -v`).
- **A merged `ResourceSizeTable` that kept the records but lost the slots.** A manager that merges `.rsizetable` files
  can write the record array only; actors whose slot is gone then load with size 0. Slot names are logical resource
  paths (`Actor/...`), not RomFS paths, so not finding them in the index is normal. Rebuild the table as a union of the
  tables being merged - each value copied byte for byte, the record array kept sorted by key, since the game
  binary-searches it - and check the frame header of the compressed result before installing it. `grow_restbl` in
  `games/totk/scripts/make_button_icons.py` is one writer of that format.

<a id="chinese"></a>
## 中文

一个 mod 有两种方式进入游戏：

- **RomFS 内容**在游戏运行时加载，是否生效完全由 `manifest.tsv` 决定，App 不会扫描文件夹找 mod。
- **主模块的代码补丁**（`exefs/main.pchtxt`、`exefs/*.ips`、`exefs/*.ips32`）在**构建时**用 `--mod`
  合并进 App，见下面的[代码补丁](#code-zh)。

`exefs/` 里的文件在运行时一律不加载：App 跑的是构建时改写好的三个代码模块，按 SHA-256 绑定。代码只能
在构建这一环节改变。

启动器把 `TOTK_ROMFS_MODS` 指向

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

`<version>` 是构建这个 App 时使用的游戏版本：`v0`（1.0.0）、`v112`（1.1.2）、`v121`（1.2.1）、
`v143`（1.4.3）。

启动时没有连接手柄、且 `mods/<version>-keyboard/manifest.tsv` 存在时，启动器改把 `TOTK_ROMFS_MODS` 指向那个目录，
用键盘玩的时候提示里显示的就是 Mac 键盘的按键：

```sh
python3.14 scripts/make_button_icons.py "$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v0-keyboard" \
  --app "Tears of the Kingdom.app" --version v0 --style azerty   # 美式键盘用 --style keyboard
```

每次启动只选一次：游戏中途再接上手柄，键盘图标会保留到下次启动。

### manifest.tsv 是什么

只有列在 `manifest.tsv` 里的文件才会被提供给游戏。每一行是四个用制表符分开的字段：

```text
<RomFS 路径>	<大小>	<mod 文件的 SHA-256>	<原版文件的 SHA-256，或 ->
```

启动时加载器会逐个核对 mod 文件的大小和 SHA-256，并核对原版文件的 SHA-256。最后一个字段是 `-`
表示这是游戏 RomFS 里没有的新文件（用于新增资源）。路径区分大小写，必须和 RomFS 里的写法一致。

没有 `manifest.tsv` 就等于没有 mod。只把文件复制进目录但不写进清单，游戏不会使用：

- 清单里没有的**新路径**会被当成不存在，游戏完全看不到这个文件。
- 覆盖已有文件但大小或哈希变了、又没有重新生成清单，启动会直接报
  `RomFS overlay file missing or wrong size` 或 `RomFS overlay integrity mismatch`。
- 原版有这个路径、但清单里没列，游戏会读取原版文件，复制进去的文件看起来“没有效果”。

### 安装步骤

构建可以用 `--mod <mod 目录> --install-mod-romfs` 完成第 2、3 步。这个参数会把版本目录整个替换掉（原来的内容
作为备份留在旁边，见下文[模组的 RomFS 文件](#data-zh)）；不带它时构建不会碰这个目录，只在结束时打印手动执行的两条命令。下面是
手动流程（构建做的是同一件事，只是先在旁边的目录里做好再整体换进来）。

1. 先完全退出游戏。
2. 把 mod 的 RomFS 路径**直接**放进版本目录。如果 mod 带一层 `romfs/` 文件夹，复制的是它里面的
   内容，不是 `romfs` 这一层：

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. 用构建好的 App 生成清单。在仓库根目录执行：

   ```bash
   python3.14 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   脚本会读取 App 里的 `Contents/Resources/totk/extracted/<version>/romfs`，计算每个文件和原版
   文件的哈希，跳过与原始完全相同的文件，然后写入 `manifest.tsv`。成功时会打印类似
   `...: 12 files (10 replaced, 2 added)` 的一行。

   `--check`、`--exclude`、`--version` 都是**这个脚本**的选项，不是 `build_app.py` 的（构建只接收 `--mod` 和
   `--install-mod-romfs`，自己带 `--check` 调用这个脚本，与传入的参数无关）：`--check` 只报告清单会
   包含什么、不写任何文件；`--exclude` 把某个路径留给 App 自己的版本。

4. 启动游戏，确认日志：

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   应出现 `.../mods/v121 checked, N files`；有新增文件时还会打印 `M files added to the RomFS`。

### 换版本时

游戏读取的是安装目录本身，不会跟着下载或解压出来的那个文件夹走。如果新版本删掉了旧版本的某些
文件，先把旧的版本目录整个移走（或备份后删除）再复制，否则被删掉的文件会继续生效。每次改动后
都要重新生成清单。

<a id="code-zh"></a>
### 代码补丁（构建时合并）

`--mod <模组文件夹>` 在构建 App 时把模组主模块的代码补丁（IPS、IPS32，或文本格式 `.pchtxt`）合并进去。

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" --mod "/path/to/Mod"
```

App 没有模块加载器：启动时它映射的是构建时的那三个代码模块（main、sdk、subsdk0），已经改写为 Apple 芯片版本
并按 SHA-256 绑定。`--mod` 在改写之前把补丁应用到当前版本核对过的原版 main 上，所以构建出的 App 仍与它绑定的
哈希一致；RomFS（数据）部分不受影响，仍按上面的说明在启动时加载。

多个带代码补丁的 mod 先在模组管理器里合并：TKMM 的合并输出就是一个文件夹，里面一套 `exefs/`、一套
`romfs/`（管理器也可能把几个 mod 的记录合进同一个 `.ips`），`--mod` 原样接收这个文件夹。两个补丁文件改到同一批
字节时构建会拒绝并报出两个文件名，所以「合并时被另一个 mod 覆盖」这种情况会停在构建阶段，不会不声不响地打进去。

文件夹按 mod 的常见结构放文件——代码在 `exefs/`、数据在 `romfs/`；只有一半也可以：没有 `exefs/`
就是没有代码要合并，没有 `romfs/` 时构建不会碰版本目录。`exefs/` 里没有可合并进 main 的东西（只有
`subsdk9`、只有 `main.npdm`，甚至空的 `exefs/`）只算提示、不算失败：构建照常往下走，只是这个 mod 只生效数据
那一半。带 `--update` 时补丁合并进更新后的模块，也就是 App 实际运行的那个。

### 启动时的代码补丁（`TOTK_PCHTXT`，Mac）

main 的 `.pchtxt` 也可以在游戏启动时应用，无需重新构建：

```bash
open --env TOTK_PCHTXT="/path/to/mods.pchtxt" "/path/to/Tears of the Kingdom.app"
```

读法与 `--mod` 相同，`@nsobid` 必须与 main 的构建 ID 一致；构建好的 App 把它存放在 `extracted/<版本>/exefs/main.build-id`。
改写器看不到这些字节，所以含系统调用、使用线程指针或计数器寄存器、或使用 x18 的补丁仍需在构建时用 `--mod`。补丁行落在 App
自己修改的位置时启动会停止。

<a id="data-zh"></a>
### 模组的 RomFS 文件：`--install-mod-romfs`

不加这个参数时，构建不会写版本目录（`~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`）：
里面可能有另一个 mod 的文件，也可能是手动复制进去的。加上这个参数，构建会用模组的 `romfs/` 替换它并生成
`manifest.tsv`：

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" \
  --mod "/path/to/Mod" --install-mod-romfs
```

这时构建最后会：

- 用 mod 的 `romfs/` 的副本**替换** `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`，
  所以构建结束后这个目录里就是这一个 mod 的 RomFS，没有别的。原来的内容（另一个 mod 的文件、手动复制进去的
  文件）不会删除，而是挪到旁边的 `<version>.backup-<日期>-<时间>`（游戏不读这个目录，不需要了自己删掉）。如果
  这个 mod 的文件最终一个都不会被游戏加载，那就什么都没替换，目录保持不动，见下文；
- 用 `scripts/make_mod_manifest.py` 对着刚构建好的 App 生成 `manifest.tsv`，所以 App 自己替换的路径会被排除
  （包括堆表：2K 覆盖文件已经带着 mod 的堆大小；UI 归档则是合并，模组原文件会以 `<name>.mod-original` 留在旁边，
  游戏不会读它）；路径大小写写错的会终止构建并给出原因，而不是等到启动时才停止。

版本目录在完整副本就绪之前不会被动：构建先把 mod 的 `romfs/` 复制到它旁边的一个目录里、在那里写好清单，确认完整
之后才把新的一份换进来（旧的目录变成备份）。在此之前还会用 `--check` 在 mod 自己的目录上跑一遍清单工具，所以路径写错
的 mod 会在版本目录还没被碰过时就停下；另外，如果这个 mod 的文件最终一个都不会被游戏加载（全部与原版相同，或者全是
App 自己会替换的文件），它其实什么都没替换——这时构建会说明并保持版本目录不动，而不是换成一个没有清单的目录。
构建开始时和复制前各提示一次：

```text
--install-mod-romfs: the player's mod folder for the version this build makes will be REPLACED by The Second Quest
1.4.3/romfs (and its manifest.tsv) when the build ends; what it holds now is moved next to it as
<folder>.backup-<date>-<time>, not deleted. Without the flag the build leaves the folder alone.
...
.../mods/v143: replaced by The Second Quest 1.4.3/romfs and its manifest.tsv (--install-mod-romfs); what it held before is kept in .../mods/v143.backup-20261021-213000
4/4 app: .../mods/v143 holds 5250 files from The Second Quest 1.4.3/romfs now (what it held before: .../mods/v143.backup-20261021-213000; delete that folder once you no longer need it)
   left out (the app replaces it, and the 2K overlay already carries these heap sizes): System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
   .../mods/v143/manifest.tsv: 5229 files (4200 replaced, 1029 added)
```

不带这个参数时，构建完全不碰这个目录，只打印手动执行的两条命令。

运行时没有任何变化，游戏仍然在启动时加载这个目录和清单；构建只负责复制和写清单。iPhone / iPad 构建
目前没有玩家 mod 目录，所以那里 `--mod` 只合并代码补丁（`--install-mod-romfs` 会被直接拒绝：没有可填的目录）。

构建做的事，按顺序：

1. 照常解出并核对原版 ExeFS。补丁只打在当前版本、核对过的原版模块上，绝不会打在已经改写过的模块上。
2. 打补丁。偏移量都是**主地址**（游戏代码所在的地址空间，和 App 自己补丁表用的数字一致）：

   - `.pchtxt`：社区文本格式，读取规则和运行时一致。第一行必须是 `@nsobid-<build id>`：改写器表里用的
     16 字节（32 位十六进制）或运行时比较的 20 字节（40 位十六进制；运行时自己的 `TOTK_PCHTXT` 实验只认长
     的这种）。后面是 `<十六进制主地址> <十六进制字节>` 行，只有 `@enabled` 之后的行生效：`@disabled` 把后面的
     段落重新关掉（作者留着但不启用某段改动的写法），`@enabled` 再打开，`@stop` 结束整个文件。第一条补丁行之前
     必须有 `@flag offset_shift 0x100`（说明这些地址不含 IPS 算法里的 0x100 字节 NSO 头，构建正是这样打的）；
     没有这一行、或者是别的位移值，都按文件和行号拒绝，而不是打偏 0x100 字节。`@flag print_values`（补丁工具的
     显示选项，我们见过的社区 pchtxt 都带）可以出现；其它 flag 会拒绝。`@nsobid-` 那一行决定目标 build，所以文件名任意。
   - `.ips` / `.ips32`：按模拟器的用法应用，其偏移量包含 0x100 字节的 NSO 头，构建时会减掉 0x100。
     偏移小于 0x100、文件截断、缺少 `EOF` 都算错误。这种文件本身不写模块名和 build id，所以只有名字是
     `main`，或者是 main 的 build id（`exefs/<build id>.ips`，Atmosphere 和模组管理器的约定）时才会
     当作 main 补丁读入；其余情况（给 `sdk`、`subsdk9` 的补丁，或者名字里是另一个 build id 的补丁）
     会留出清单并提示。名为 `main` 的 `.ips` 没有 id 可核对；按 build id 命名，或改用 `.pchtxt`，才会有这项检查。

3. 打完后先检查再往下走。以下情况会被拒绝，并指出地址和原因：补丁落在 main 的 text、rodata、data
   之外（text 之后的填充区是保留的）；两个补丁文件改同一批字节；补丁什么都没改；补丁改到了 App 自己
   打补丁的位置——60 帧与实时单帧步进的地址、渲染/输出分辨率、超宽屏、布料，以及改写器登记的 cut。
   这些功能由 App 设置（`TOTK_FPS60`、`TOTK_REALTIME`、`TOTK_RENDER_RES`、`TOTK_OUTPUT_RES`、
   `TOTK_ASPECT`）：运行时会逐个核对那里原来的指令，被改了就直接停止游戏。
4. 把打过补丁的模块改写为 Apple 芯片版本，App 按它的 SHA-256 绑定。`--work` 里的报告（1.0.0 是
   `analysis/phase12-validation.json`，其他版本是 `analysis/<version>-port/phase12-validation.json`，
   字段 `mod_exefs`）记录 mod 目录、每个补丁文件的 SHA-256、改动的地址和合并后模块的哈希。

iPhone / iPad 构建同样处理：`--mod` 加 `--platform ios`。

mod 的 `exefs/` 里可能还有这些文件，构建这样处理：

| mod 的 `exefs/` 里 | 结果 |
| --- | --- |
| 给 main 的 `*.pchtxt`、`*.ips`、`*.ips32` | 合并进构建；`.ips` 必须叫 `main` 或按 main 的 build id 命名，`.pchtxt` 由 `@nsobid-` 指定版本、文件名任意 |
| `main.npdm` | 忽略并提示：原生加载器不读模块元数据 |
| `subsdk9` 等额外代码模块 | **不加载**：构建输出和报告里都会注明；App 只加载构建时的三个模块（main、sdk、subsdk0），模块自己的代码不会运行 |
| 整个替换的 `main` | 拒绝：只能用补丁，构建才能按版本核对 |
| 没有可合并进 main 的东西（只有 `subsdk9`、只有 `main.npdm`，甚至空的 `exefs/`） | 只提示、不拒绝：main 不做任何合并，构建继续；这个 mod 的代码不进 App，数据部分要装进版本目录才生效 |

用 exlaunch 这类 hook 框架做的 mod，运行时改代码的部分就在这种额外模块里：它的 trampoline 在本 App
里不会安装。像 The Second Quest 这样的 mod，主模块补丁和整个 RomFS 都会生效，但依赖那个模块的那部分
代码改动不会。要支持它就得让 App 具备模块加载能力（把模块也改写为 Apple 芯片版本，并实现它用到的
服务），本版本没有做。

构建之后再改 mod 的代码就要重新构建（mod 的字节已经是 App 的一部分），而带 `--mod --install-mod-romfs` 的
构建也会顺带替换它的数据目录。只改 RomFS 文件不需要构建：把文件放进版本目录、重新生成清单即可（上面的第 2、3
步）。已经合并过的结果会复用：记录的哈希一致时跳过改写；记录里也写了改写器自己的 SHA-256，所以 `patch_nso.py`
一改就会重新改写，而不是复用旧镜像。2K 输出覆盖文件会记下它是在哪份堆表上生成的：先带 `--mod` 构建、之后不带
`--mod` 再构建时，会按原版堆表重新生成，而不是继续沿用 mod 的堆大小。

### 大型材质包（8 GB 模式）

有些材质包是给模拟器的 8 GB DRAM 模式做的，装不进真机给游戏的 3.2 GiB。例如 **[HDPacks 的王国之泪 4K 材质包](https://www.hdpacks.com/)**
（HDPacks.com 制作，可在其网站免费下载；这是他们的作品，不在本仓库里：请从他们那里获取并遵守他们的条款）。在 Mac 上配合
1.4.3 使用：

1. 把材质包的 `romfs` 文件（HDPacks 的包是 `4kTextures/RomFS`）和你用的其他 mod 一起放进该版本的 mod 文件夹（`mods/v143`）。
2. 材质包自带的堆表和资源尺寸表是按它对应的版本做的（HDPacks 的包最高到 1.2.1），先改成 App 的版本：

   ```sh
   python3.14 games/totk/scripts/texture_pack_tables.py ~/Library/Application\ Support/MacNativeGames/TOTK/mods/v143 \
       --app "Tears of the Kingdom.app"
   ```

   它写出 1.4.3 自己的 `HeapDef`，只把材质包加大的堆调大（HDPacks 的包：`TextureHeap`、`ゲームグラフィックス`、`草`），
   并写出 1.4.3 的 `ResourceSizeTable.Product.143`，每个被替换贴图的条目按文件变大的量加大。然后照常用
   `make_mod_manifest.py` 生成清单。文件夹有变化时两步都要重跑。
3. 用 8 GB 模式启动：

   ```sh
   open --env TOTK_DRAM_8GB=1 "Tears of the Kingdom.app"
   ```

   `TOTK_DRAM_8GB` 向游戏报告 6.8 GiB 的应用内存池（和模拟器的 8 GB 模式一样）；材质包约 3.8 GiB 的 `TextureHeap`
   池做成一个零拷贝 GPU 缓冲。

在 24 GB 的 Apple 芯片 Mac 上验证：游玩时常驻内存约 8.7 GB，GPU 接近满载（瓶颈在 GPU）。内存更小的 Mac 预计跑不动。

### 旷野之息

旷野之息 App 按版本从对应目录读取 mod，1.6.0 是 `botw160`，1.9.0 是 `botw190`：

```text
~/Library/Application Support/MacNativeGames/BotW/mods/botw160
~/Library/Application Support/MacNativeGames/BotW/mods/botw190
```

上面的 RomFS 部分都适用：把 mod 的 RomFS 内容复制进这个目录，再用 App 生成清单（目录名为 `botw160` 或 `botw190`，或加 `--version botw160` 或 `--version botw190`；下面以 1.6.0 为例）：

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw160"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Breath of the Wild.app"
```

启动后用 `grep "mods/botw160 checked" ~/Library/Logs/MacNativeGames/botw.log | tail -1` 检查（1.9.0 是 `mods/botw190 checked`）。

- 如果 mod 让某个资源比原版大，还必须带上为同一版本生成的资源大小表
  `System/Resource/ResourceSizeTable.product.srsizetable`。没有它游戏会按原版大小分配，启动时就会停止（例如
  `Pack/Bootup_<语言>.pack` 变大的翻译 mod）。
- 游戏语言跟随 App 的首选语言，也由它决定 mod 要放在哪个语言目录（`en-US` 对应 `USen`）。只给这个游戏换语言：
  `defaults write org.macnativegames.botw AppleLanguages -array en-US`。

### 注意

- 不要手改 `manifest.tsv`，每次重新生成。
- 游戏运行中不要改 mod 文件；加载器会在启动时记录每个文件的身份，运行中变化会停止游戏。
- 玩家 mod 只有一个覆盖目录：多个 mod 就合并到同一个版本目录，最后生成一份清单；不要把 mod
  文件夹嵌套进去。两个 mod 改同一个路径时，只能选一个文件。
- 构建本身也会替换几个 RomFS 文件：`UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs`（快捷菜单时机，`<n>`
  按版本是 100、110 或 140）、`Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`，开 2K 输出时还有
  `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`。两份覆盖无法替换同一个路径（启动会报“two overlays replace the
  same file”），所以：
  - **UI 归档**是**合并**的：翻译模组的归档保留自己的贴图，同时接受构建对快捷菜单的改动——工具把合并后的归档写进模组
    目录，模组原文件保留为旁边的 `<name>.mod-original`，合并后的文件取代构建的覆盖层（#73）。如果模组改动了构建也会
    修改的成员（那些 capture 动画），工具会拒绝，而不是悄悄丢掉一侧；
  - **堆表**由构建叠加：带 `--mod` 时 2K 覆盖文件是在模组那份堆表之上生成的；
  - 其余路径（比如 game-over 流程）留给 App，并打印说明排除了哪个文件。
- 这些文件由 `make_mod_manifest.py` 自己处理：UI 归档按上面的方式合并，其它与 App 自身替换项相同的模组文件不会写进
  清单（并打印一行说明），因为两份覆盖无法同时加载。堆表那一行说明 App 是否已经带着这些堆大小，
  否则提示用 `--mod` 重新构建。
- 模组管理器（TKMM，或 Ryujinx 的 `mods/contents/<title id>/<name>`）给出的是同一个文件夹里的 `exefs/` 和 `romfs/`：
  把这个文件夹交给 `--mod`（里面的 `exefs` 补丁会合并，管理器合并出来的 `.ips` 可能一个文件里装着好几个模组的记录），
  再加上 `--install-mod-romfs` 让构建把对应版本的 mods 目录整个换成它的 `romfs/` 并生成清单（不需要再手动
  `--exclude`：管理器的堆表和 App 的 2K 覆盖文件会自动协调）。每次在管理器里重新合并后都要重新构建，因为游戏启动时会逐个校验文件哈希。多个带代码
  补丁的 mod 也是这样合的：先在管理器里合并，再用 `--mod <管理器的输出文件夹> --install-mod-romfs` 构建；两个补丁文件改到同一批字节
  时构建会拒绝，并同时报出这两个文件。
- 带代码补丁的 mod，其 `exefs/` 补丁也要和构建所用的游戏版本一致。构建按文件自己写的东西核对：`.pchtxt` 的
  `@nsobid-` 不是这个 main 就拒绝；按 build id 命名的 `.ips` / `.ips32`，id 不是这个 main 就留出并提示。
- 本文面向 Mac App；iPhone/iPad 的 App 容器内安装未在此说明。

### 启动停止时

游戏会关掉窗口，并在 `~/Library/Logs/MacNativeGames/totk.log` 写下最后一行原因，它上面一行就是出问题的文件。日志默认关闭：先用 `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`（旷野之息是 `org.macnativegames.botw`）打开，再启动一次。本页的 `grep` 检查也需要先打开日志。

- `two overlays replace the same file`：清单里列了 App 自己也会替换的路径——通常是手工写的清单或别的工具写的清单（UI 归档
  的情况会改成合并而不是排除），
  因为 `make_mod_manifest.py` 自己就会把这类路径排除。用那个脚本重新生成即可。
- `two overlays add the same file`：两个覆盖（App 的或两个模组目录）新增了同一个 RomFS 里没有的路径。
- `RomFS overlay integrity mismatch`：模组目录里的文件和清单里的 SHA-256 不一致，也就是生成清单之后又拷了文件进去。
  重新生成清单即可（`make_mod_manifest.py` 遇到对不上的文件不会写清单）。
- `RomFS overlay names a file the RomFS lacks` / `adds a file the RomFS already has`：替换项的“原件”对不上（文件是从
  别的游戏版本拷来的）；按对应版本重新生成清单并重新构建 App。
- `RomFS overlay file changed while the game ran`：游戏运行中改了文件夹；重启游戏并重新生成清单。

窗口也可能直接消失、日志里没有上面这些行：某个模组的数据能让 Metal 直接终止进程（此时日志里有宿主调用栈，例如
`-[MTLTextureDescriptorInternal validateWithDevice:]` 报 “requests 9 mipmap levels, but the dimensions (1, 1, 1) can only
support 1”）。mip 级数超过尺寸允许的贴图在 Switch 上没人检查，Metal 却拒绝创建；App 会把级数裁到尺寸允许的数量并记一行
`[tex] <w>x<h>x<d> ... 声明 N 级 mip ...`，画面不对时应检查该模组的贴图。

### 三种打包环节的陷阱，看起来像崩溃

这三处出在包的打包方式上，而不是包的内容；它们都不会打印上面那些行。

- **`.pchtxt` 缺 `@flag offset_shift 0x100`。** 文件里的地址是 `main` 地址，而 IPS 的偏移把 0x100 字节的 NSO 头也算了进去。
  补丁行出现在这个 flag 之前、或写的不是 0x100，`TOTK_PCHTXT` 和 `--mod` 都会直接停止启动——否则每个字节都会静默地
  偏 0x100 落错位置。手工把 IPS 转成 pchtxt：`main 地址 = IPS32 偏移 - 0x100`。
- **`.zs` 的 zstd 帧没有声明内容大小。** 游戏的解压器无从得知解压后多大，会解到空指针上：窗口在 `ENTER nnMain` 之后
  立刻消失，日志里没有我们这边的任何一行。`zstd` 从管道压缩（`zstd -c`）时会写出这种帧；帧头描述符字节（`raw[4]`）
  的 bit7-6 必须非 0。改为对字节数组调用库压缩，断言 `(raw[4] >> 6) & 3`，窗口大小、校验和、字典都与被替换的原文件
  保持一致（`zstd -l -v` 可看）。
- **合并后的 `ResourceSizeTable` 留下了记录、丢了槽。** 只合并 `.rsizetable` 记录数组的管理器，会让槽位消失的那批 actor
  按 0 大小加载。槽名是逻辑资源路径（`Actor/...`），不是 RomFS 路径，在索引里查不到属正常。按参与合并的各张表求并集
  重建——值逐字节照抄，记录数组保持按 key 升序（游戏按 key 二分查找）——装进去之前再核对压缩结果的帧头。
  `games/totk/scripts/make_button_icons.py` 里的 `grow_restbl` 就是这种格式的一个写入示例。
