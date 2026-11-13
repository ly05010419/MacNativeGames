# The shared shader list

`games/totk/assets/shader_list.tsv` is the one list of the game shaders and pipelines this project prepares ahead of
play. The Mac, iPhone and iPad builds all use it, and every player's discoveries go into it. Please send new entries
in a pull request.

## Why it exists

The game builds a shader *variant* from each shader for the bind state of a draw: which kind of texture sits in each
slot, attribute types, output formats. A variant can only be translated once it is known.

- On the Mac, a missing variant is translated while you play. That costs a short stall the first time.
- An iPhone or iPad cannot translate. It skips any draw whose variant the app does not already carry. A new install
  with only the default variants shows little or nothing of the picture.

The list names the variants players have met, so each build carries them from the start.

## What is in it

The list holds identifiers only, never game data. Each build makes the shaders from the player's own game files.

| Row | Fields | Meaning |
|---|---|---|
| `V` | shader key, deps | A shader and the bind state it was translated for. |
| `A` | suffix, answers | A bind state warm rows name: a variant is `<shader key>-<suffix>_b`, the suffix the first 12 hex digits of the SHA-256 of the answers it was translated for. The list keeps one for every bind state its warm rows name but the default one, also after `prune` drops the `V` row that brought it. |
| `L` | shader key, source, control offset, code offset, length | Where a shader that is not a BNSH file sits in the player's game files. |
| `S` | `<variant>_b`, stage, flags | A shader library the warm-up loads. |
| `C` | `<variant>_b`, flags | A compute pipeline the warm-up builds. |
| `R` | pipeline key, flags | A render pipeline the warm-up builds. |
| `B` | pipeline key, vertex border, fragment border, flags | That render pipeline with the sampler border colours Metal lacks, emulated in its shaders (`-` for a stage without). The warm-up builds the patched shaders and the pipeline. |
| `T` | pipeline key, tile reads, flags | That render pipeline with the fragment variant that reads the draw's own colour attachments from the tile, `<argument id>:<attachment>;` per read (`nvn_mtl.m warm_tileread`). Before 2026-10-26 only `pso_user.tsv` had these, so every new install built them in play (all 58 a 5-minute route built in the world with the list). |

- **Pipeline key:** the runtime's 544-byte `pso_skey` (`nvn_pso_skey.h`). The Tears of the Kingdom and Breath of the
  Wild lists keep it packed: `<vertex variant>:<fragment variant>:<the other 384 bytes in hex>`, each run of four or
  more zero bytes written `z<count in hex>.`. This is about a quarter of the hex size. Contribution files and the
  exported warm-up lists use plain hex; `shader_list.py` reads both.
- **Shader key:** the SHA-256 of the shader's NVN control section and code, computed the same way as the runtime.
- **Deps:** the translator's `deps` as compact, sorted JSON. They record the texture, image, attribute and fragment
  output types the shader depends on.
- **Flags:** `t` marks the title screen, its menu and the save list. The first frame waits only for these. `m` marks
  the rest of the warm-up.

**Locator rows:** the build finds the shaders of BNSH files, of agl's `.sharcb` packages and of the game module by
itself. A locator is for a shader kept anywhere else. It names the RomFS path
(each `/` after a package enters it, `.zs` decompressed) or `module:<name>`, plus hex offsets of the NVN control section
and the code and the code length. The build reads the bytes there from the player's own files and keeps them only when
they hash to the key, so another game version's bytes are never used.

An entry is a shader plus its deps, never a variant name. The runtime picks a variant by matching deps, and the same
shader and deps can carry different names on different machines.

`V` rows are sorted. Warm rows keep the order they were recorded in. Keys of every game version may be present; a
build uses only those found in the selected game files.

Packaging writes the app's `build/pso_title.tsv` (rows marked `t`) and `build/pso_manifest.tsv` (the other rows marked
`m`), which the runtime reads, with `shader_list.py resolve <seed> <list> <pso_manifest.tsv> <pso_title.tsv>`: only
rows of the selected game version's shaders (its seed's `shader-keys.json`), each variant named as that seed serves its
bind state (the seed's `.variants` index, picked as the runtime picks one), and no row the seed cannot build. Edit the
list, never those files. Merging refuses a warm row naming a bind state that neither the list nor the merged file
keeps (the game writes the `V` row behind every name in a pipeline row it contributes, `totk_shader_rt.m
totk_rt_portable_variant`). Until 2026-10-30 the list had no `A` rows: the 4,561 Tears of the Kingdom rows naming bind
states nothing recorded (none of them could be built: 45,673 of 73,336 exported rows failed the Mac pipeline archive's
fill, mostly other versions' shaders and names from before vertex input and colour output types stopped making
variants) were dropped once, and the bind states of the rest recovered from the list's history and the translation
caches on the maintainer's Mac.

## Warm-up scope

The prepared seed also carries `shader-keys.json`: every shader key found in the selected game files, including shaders
that were not translated. The background warm-up warms only cache entries of those shaders, plus shaders the game was
seen to use on this Mac (`observed/<RomFS SHA>.keys` in the translation cache). Entries of other game versions in the
shared cache are skipped during warm-up only, never deleted or refused for drawing. Without a valid scope (an older
app) the whole cache is warmed as before; `TOTK_WARM_ALL=1` does that for comparison. Not used on iPhone / iPad.

## Sending what you played (players)

You do not need to do anything while playing. When the Mac has to translate a shader variant, or an iPhone or iPad
misses one, that variant is not in the list yet, so the game writes it to `shader-list-contribution.txt`: one `V` row
per shader and bind state, nothing else (`third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h`). Since every build carries
each shader's default variant ([shader-variants.md](shader-variants.md); `all_default` in the seed's manifest), only
states those variants do not serve are written, never a default variant (`totk_shader_rt.m contrib_wanted`): the list
keeps non-default variants only (2026-10-22; Breath of the Wild too since 2026-10-23).
Render pipelines are collected with them (2026-10-22): one the game builds in play that the app's warm-up lists lack
becomes an `R` row (and a `B` row with its border recipe, a `T` row with its tile reads), written off the submit thread
(`nvn_mtl.m pso_contribute`). A `B` or `T` row the lists lack goes in also when they have its plain `R` row (until
2026-10-26 it went only with a new `R` row).
Its shaders are named as a build names them: a default or depth-bound variant keeps its name, any other is named after
its deps as `prepare_shader_seed.py` names a list row (`totk_contrib_listed_name`), so another player's build has it.

- Mac: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- iPhone / iPad: Files app › On My iPhone (or iPad) › Tears of the Kingdom › `shader-list-contribution.txt`

Once the file holds 100 new rows of either kind, the game asks once, before it starts, on the Mac and on the iPhone or
iPad alike: "Contribute shaders" or "Don't contribute". With "Contribute shaders" it uploads the rows by itself over
HTTPS to the project's shader inbox (https://mac-native-games.com), about 20 seconds after it starts and then every half
hour, and never asks again; "Don't contribute" sends nothing and never asks again. "Contribute shaders automatically" in
Settings (the Mac's settings panel, display page; the iPhone / iPad Settings app) turns it on or off at any time. Only
shader and pipeline identifiers, the game and app versions, the device model and system version, and a random id are
sent. On the Mac the file is `shader-list-contribution/shader-list-contribution.txt` in your clone (Breath of the
Wild's `shader-list-contribution-botw.txt`, Bananza's `-dkb`, Pokopia's `-pkp`). The upload is the only way in: since
2026-11-12 no file is taken from a GitHub issue.

Each file belongs to one game (2026-11-12): every game's shaders stay apart from writing to sending. The launcher claims
the file before the game starts (`totk_contrib_upload.h totk_inbox_claim`): its line `# Inbox game: <id>` names the game
(the inbox's id, `GAME_INBOX_ID`). The game writes only to a file of its own id, and the upload sends only such a file.
A file of another game, or one from before this check (no such line), moves aside as `<file>.of-<game>-<time>` or
`<file>.unclaimed-<time>` and is never sent; the game starts a new one. Before, one file could hold several games' rows
(Breath of the Wild, Bananza and Pokopia shared `-botw` until 2026-11-12) and the app that sent it named its own game for
all of them.

The file holds only what still has to be sent (2026-11-01; before, every issue carried the whole file, and of one
day's 158 issues 83% of the Tears of the Kingdom shader rows and 99% of the pipeline rows were already in the list):

- **Rows the app serves leave it.** Before the Mac launcher counts the file for its reminder, and before an iPhone or
  iPad starts the game, rows the app's own seed and warm lists already serve move out (`totk_contrib_prune`): a `V`
  row whose bind state a seed variant serves (picked as the runtime picks one), an `R` / `B` / `T` row the warm lists
  hold. They were written by an older app, or another player's file brought them into the list since. A shader of
  another game version, and a pipeline the build names otherwise (0.6%), stay. This runs once per app build.
- **Rows the inbox took leave it.** Each upload moves the rows the inbox took out of the file, so each row is sent
  once; rows not taken (no network, a server error) stay for the next try.
- Both go to a hidden `.shader-list-contribution.txt.done` beside the file. The game treats its rows as ones it has, so
  it never writes or sends them again.

## Breath of the Wild

Breath of the Wild has its own list, `games/botw/assets/shader_list.tsv`, in the same format and kept with the same
tools (`shader_list.py merge ... games/botw/assets/shader_list.tsv <folder or file>`). Its build reads the game's
Yaz0 packages (`Pack/`, `Effect/`, `Event/`, `System/`: the `uking_*.sbfsha` material archives and the engine's agl
packages) and translates the boot and title packages' shaders for the default bind state plus every state the list
names (`games/botw/scripts/prepare_shader_seed.py`). Its players' contribution file is
`~/Library/Application Support/MacNativeGames/BotW/shader-list-contribution.txt`, uploaded to the shader inbox under
the game id `botw` (`collect_shaders.py` merges every game's rows). The lists never mix: a key names a shader of one
game only.

Its shader key hashes the control section's own length (the word at +0x20: 0x7d1 for the game's archives, 0x848 for
agl's; Tears of the Kingdom's is 0x880, so its keys are unchanged) and, for the agl programs the game loads from
embedded BNSH files, the bytes as the game rewrites them before binding (`prepare_shader_seed.py bound_control`); the
agl packages' own records are bound unchanged.

Title rows (`t`) of either game must be recorded in a cache that has the prepared seed imported (the warm-up loads a
row's shader by its file name, and the seed names its variants by their bind-state answers, not as a cache translated
on the spot would): run the game with `DM_PSO_MANIFEST=off`, an empty `DM_NVN_SHMAP` folder and
`TOTK_SHADER_SEED_DIR=<prepared seed>`, then `shader_list.py merge --title ... <profile>/pso_user.tsv`.

## Other games

Every other game that records a contribution file keeps its own list, `games/<game>/assets/shader_list.tsv`, never
Tears of the Kingdom's (2026-10-28): Super Mario Odyssey (`odyssey`), Super Smash Bros. Ultimate (`ssbu`), Super Mario
Bros. Wonder (`wonder`) and Mario vs. Donkey Kong (`mvdk`). Each game's own `totk_shader_contrib.h` names it: the
contribution file `shader-list-contribution-<game>.txt` (Wonder and Mario vs. Donkey Kong, in the clone's
`shader-list-contribution/` folder; Odyssey and Smash write `shader-list-contribution.txt` in their own data folder), and
its own `totk_contrib_upload.h` names the game's id in the shader inbox (`GAME_INBOX_ID`). Before that the files of
Wonder and Mario vs. Donkey Kong were `shader-list-contribution-botw.txt`, like Breath of the Wild's.

`collect_shaders.py` gives a row to the game whose shader keys hold its shaders; the game the app named decides only
when no other game holds them (the script's docstring has the rules). There is no seed of those games here, so their
rows are told apart by the keys already in their lists, and no default variants are dropped. Odyssey's and Smash's builds read their lists
(their own `prepare_shader_seed.py`); Wonder's and Mario vs. Donkey Kong's do not yet, so their lists only collect for now.

## Merging what players sent (maintainers)

Everything players contribute arrives in the shader inbox; nothing is read from GitHub issues (since 2026-11-12).

```sh
# what the inbox holds for each game and what it would add (changes nothing). SHADER_INBOX_TOKEN is the inbox's admin
# token. Each --seed gives a game version's shader keys (its shader-keys.json) and drops the rows its built-in variants
# serve: give a seed of every version collected; --keys <game>=<file> adds a key list
export SHADER_INBOX_TOKEN=<token>
S=<work>/totk/build/shader-seed
python3 games/totk/scripts/collect_shaders.py --inbox https://mac-native-games.com --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160
# write the lists
python3 games/totk/scripts/collect_shaders.py --inbox https://mac-native-games.com --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160 --apply
# once the lists are committed: the same with --mark-inbox <commit>, so the inbox counts those rows as done
# drop rows already in the list that the seed's default (or depth-bound) variants serve
python3 games/totk/scripts/shader_list.py prune games/totk/assets/shader_list.tsv <work>/totk/build/shader-seed/v143
```

It takes `V` rows and render pipeline `R` / `B` / `T` rows. Every row goes to the game its shader keys belong to,
whatever game the app named; a row no one can place stays in the inbox. A seed made before 2026-10-22 has no
shader-keys.json and is refused. Then check with `shader_list.py stats`, commit the lists and push; the next builds
carry the new variants.

## Adding what you played (with git)

Use `games/totk/scripts/shader_list.py`. It needs only Python 3.

```sh
# Mac: add the shaders your own translation cache holds
python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv \
  "$HOME/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v9-unverified"

# check the result, then commit the list and open a pull request
python3 games/totk/scripts/shader_list.py stats games/totk/assets/shader_list.tsv
```

- **iPhone / iPad:** `games/totk/ios/tools/fill_shaders.py` translates what a device missed. Like the Mac, it puts the
  new variants in the contribution file, not in the shipped list: add `--update-list` when you want the next build from
  this tree to carry them too. For Breath of the Wild it checks Breath of the Wild's own list (`--game botw`, the default
  for a `.botw` bundle identifier), and `--from-cache <rt-folder> --seed <seed>` packs the shaders the Mac's own Breath
  of the Wild cache has and the app lacks, already translated, for the device.
- **Pipelines:** merging a cache folder adds only its `V` rows. A cache's `pso_collected.tsv` holds every pipeline ever
  built, tens of thousands of them. To add pipelines, name a `pso_user.tsv` or `pso_collected.tsv` file explicitly.
- **Title rows:** record them with the warm-up off, then add them with `merge --title`.
- **Locator rows:** after merging a cache, run
  `python3 games/totk/scripts/locate_shaders.py --romfs <work>/totk/extracted/<version>/romfs --modules <work>/totk/rewritten/<version> <cache folder>`.
  It looks up every listed shader the build would not find by itself, using the code the cache recorded, and adds its
  locator after checking the key. It needs a built work folder and takes about 3 minutes.

The tool drops rows that are not shader identifiers. Merging the same source twice adds nothing.

## Reviewing a contribution

- `shader_list.py stats` must pass, and `python -m unittest tests.test_shader_list` must pass.
- The diff may change only `shader_list.tsv`. Do not add translated sources (`.metal`), compiled libraries
  (`.metallib`) or caches: they are made from the game.

---

## 共享着色器清单（中文摘要）

`games/totk/assets/shader_list.tsv` 是唯一的一份着色器清单。Mac、iPhone、iPad 都用它，所有人玩出来的新变体都加进这一份，欢迎提 PR。

- **为什么要有它**：iOS 不能当场翻译着色器，缺哪个变体就跳过用到它的绘制。清单里的变体在打包时就用玩家自己的游戏文件翻译好。
- **里面是什么**：
  - `V` 行：着色器键 + 绑定状态（deps）。
  - `L` 行：打包程序自己找不到的着色器在玩家游戏文件中的位置：文件路径、控制段和代码的偏移、代码长度。打包时从玩家自己的文件读出，核对哈希等于键才用。BNSH 文件、agl 的 `.sharcb` 包和游戏程序里的着色器，打包程序会自己找到。
  - `S`、`C`、`R` 行：预热用的着色器库和管线。`B` 行：同一条管线带着它当时采样器的边框颜色（Metal 没有的颜色要在着色器里模拟），预热按它建好补丁着色器和这条管线。`T` 行：同一条管线换成从 tile 直接读本次绘制自己颜色附件的片元变体（2026-10-26 以前只记在 `pso_user.tsv`，新装的人每次都在游玩中现建）。标记 `t` 是标题画面用的（第一帧只等这些），`m` 是其余预热。
  - 只有标识，不含游戏数据。
- **玩家怎么贡献**：玩的时候什么都不用做。Mac 现场翻译的变体、iPhone / iPad 缺的变体，都说明清单里还没有，游戏会把它记进 `shader-list-contribution.txt`（Mac 在 `~/Library/Application Support/MacNativeGames/TOTK/`，iPhone / iPad 在“文件”App › 我的 iPhone（或 iPad）› Tears of the Kingdom）。每次打包都带着每个着色器的默认变体，所以只记默认变体用不了的状态，默认变体从不记（清单只收非默认变体，2026-10-22）。玩的时候建的、App 预热清单里没有的渲染管线也一起记（`R` 行，带边框色的再加 `B` 行，从 tile 读附件的再加 `T` 行；普通管线清单里已有时，`B` / `T` 行也照记；变体名换成别人打包时也会有的名字）。着色器和管线合计攒够 100 条时，Mac 和 iPhone / iPad 都会在游戏开始前问一次：“贡献着色器”还是“不贡献”。选“贡献着色器”，游戏会自己通过 HTTPS 上传到项目的着色器收件箱（https://mac-native-games.com）：开始后约 20 秒一次，之后每半小时一次，以后不再问；选“不贡献”就什么都不发，也不再问。之后随时可以在“设置”里用“自动贡献着色器”开关打开或关闭（Mac 在游戏内设置面板的画面页，iPhone / iPad 在系统“设置”App）。只发送着色器和管线的标识、游戏和 App 版本、设备型号和系统版本号，以及一个随机编号。上传是唯一的途径：从 2026-11-12 起不再收 GitHub Issue 里的文件。
- **维护者合并**：玩家的贡献全在着色器收件箱里，不再看 GitHub Issue（2026-11-12 起）。`SHADER_INBOX_TOKEN=<收件箱管理口令> python3 games/totk/scripts/collect_shaders.py --inbox https://mac-native-games.com --seed <打包目录>/totk/build/shader-seed/v143 --seed <旷野之息打包目录>/totk/build/shader-seed/botw160` 先看每个游戏能加多少；加 `--apply` 写进清单；清单提交后再加 `--mark-inbox <提交号>`，收件箱把这些行记为已处理。只取 `V` 行和管线的 `R` / `B` / `T` 行，种子自带的默认变体能用的 `V` 行丢掉。每一行按它的着色器键归到所属游戏，不看 App 报的游戏；谁都认不出的行留在收件箱里。每个 `--seed` 提供该版本的键（`shader-keys.json`，2026-10-22 以前的旧种子没有，会被拒绝），`--keys <游戏>=<文件>` 可再加键清单。旧行用 `shader_list.py prune <清单> <种子目录>` 清掉。
- **每个贡献文件只属于一个游戏**（2026-11-12）：启动器在游戏开始前认领文件（`totk_inbox_claim`），文件里的 `# Inbox game: <id>` 行写明是哪个游戏（收件箱里的 id，`GAME_INBOX_ID`）；游戏只往写着自己 id 的文件里写，上传也只发这样的文件。别的游戏的文件、或这项检查之前留下的没有这一行的文件，挪到 `<文件>.of-<游戏>-<时间>` / `<文件>.unclaimed-<时间>`，永远不发，游戏另起一个新文件。以前一个文件里可能有几个游戏的行（旷野之息、大金刚、Pokopia 到 2026-11-12 共用 `-botw`），哪个 App 上传就全标成自己的游戏。
- **其他游戏**（2026-10-28）：记贡献文件的其他游戏各有自己的清单 `games/<游戏>/assets/shader_list.tsv`，不混进王国之泪的：奥德赛（`odyssey`）、大乱斗（`ssbu`）、惊奇（`wonder`）、马力欧 vs. 大金刚（`mvdk`）。每个游戏自己的 `totk_shader_contrib.h` 写明它的贡献文件名 `shader-list-contribution-<游戏>.txt`（惊奇和大金刚在项目的 `shader-list-contribution/` 文件夹里；奥德赛和大乱斗写在各自数据文件夹的 `shader-list-contribution.txt`），它自己的 `totk_contrib_upload.h` 写明它在着色器收件箱里的游戏 id（`GAME_INBOX_ID`）。以前惊奇和大金刚的文件叫 `shader-list-contribution-botw.txt`，和旷野之息一样。`collect_shaders.py` 把一行归给着色器键包含它的着色器的那个游戏；只有别的游戏都没有这些键时，才按 App 报的游戏归（规则见脚本开头的说明）。这些游戏这里没有种子，所以靠清单里已有的键区分，也不去掉默认变体。奥德赛和大乱斗打包时读自己的清单；惊奇和大金刚的打包还不读，清单目前只是先收着。
- **用 git 贡献**：
  - Mac：`python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv <自己的翻译缓存目录>`，再运行 `locate_shaders.py` 补上定位行，用 `stats` 检查，提交清单并提 PR。
  - iOS：`fill_shaders.py` 补译后和 Mac 一样先写进贡献文件（`shader-list-contribution.txt`）；想让下一次本地打包就带上，加 `--update-list` 同时并进共享清单。旷野之息对照旷野之息自己的清单（`--game botw`，bundle id 带 `.botw` 时默认如此）；`--from-cache <rt 文件夹> --seed <seed>` 把 Mac 上旷野之息缓存里有、App 里没有的着色器（已翻译好）打包给设备。
- **审核**：`stats` 和 `tests/test_shader_list.py` 要通过；PR 只能改这一个文件，不能带翻译出的 `.metal`、编好的 `.metallib` 或缓存。
- **旷野之息**：自己的一份清单 `games/botw/assets/shader_list.tsv`，格式、工具都一样（`merge` 时写它的路径）。打包时读它的 Yaz0 包（`Pack/`、`Effect/`、`Event/`、`System/`），启动包和标题包的着色器按默认绑定状态翻译、清单里的绑定状态也翻译。玩家的贡献文件在 `~/Library/Application Support/MacNativeGames/BotW/`，以游戏 id `botw` 上传到着色器收件箱，`collect_shaders.py` 合并。两份清单不混：一个键只属于一个游戏。它的键按控制段自己的长度算（+0x20 处的字：档案里 0x7d1、agl 的 0x848；王国之泪是 0x880，键不变）；agl 包里嵌着的 BNSH 程序还按游戏绑定前改写过的字节算（`bound_control`），agl 包自己的记录原样绑定。
