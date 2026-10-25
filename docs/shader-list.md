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
| `L` | shader key, source, control offset, code offset, length | Where a shader that is not a BNSH file sits in the player's game files. |
| `S` | `<variant>_b`, stage, flags | A shader library the warm-up loads. |
| `C` | `<variant>_b`, flags | A compute pipeline the warm-up builds. |
| `R` | pipeline key, flags | A render pipeline the warm-up builds. |
| `B` | pipeline key, vertex border, fragment border, flags | That render pipeline with the sampler border colours Metal lacks, emulated in its shaders (`-` for a stage without). The warm-up builds the patched shaders and the pipeline. |

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

The build exports the warm rows to `build/pso_title.tsv` (rows marked `t`) and `build/pso_manifest.tsv` (rows marked
`m`), which the runtime reads. Edit the list, never those files.

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
becomes an `R` row (and a `B` row with its border recipe), written off the submit thread (`nvn_mtl.m pso_contribute`).
Its shaders are named as a build names them: a default or depth-bound variant keeps its name, any other is named after
its deps as `prepare_shader_seed.py` names a list row (`totk_contrib_listed_name`), so another player's build has it.

- Mac: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- iPhone / iPad: Files app › On My iPhone (or iPad) › Tears of the Kingdom › `shader-list-contribution.txt`

On a Mac, every 100 new rows of either kind the launcher asks once, before the game starts, whether to send the file. An iPhone or
iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi),
`games/totk/ios/tools/shader_contrib_watch.py` (a LaunchAgent the iOS build installs) fetches the device's rows every
10 minutes at most and adds the ones not there yet to the same file the Mac writes:
`shader-list-contribution/shader-list-contribution.txt` in your clone (the build makes the folder; Breath of the Wild's
is `shader-list-contribution-botw.txt`). One file for the Mac and every device, since the list does not tell platforms
apart; the folder opens in Finder with each reminder, which comes every 100 new rows in that file. "Remind me next time" waits for the next 100;
"Don't remind me again" stops the reminders for good (the file is still kept). "Send" opens the [shader list contribution](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)
issue form; drag the file into it. Later reminders come at 200, 300 and so on, so sending the whole file each time
is fine: rows the list already has are skipped. The game never uploads anything by itself.

## Breath of the Wild

Breath of the Wild has its own list, `games/botw/assets/shader_list.tsv`, in the same format and kept with the same
tools (`shader_list.py merge ... games/botw/assets/shader_list.tsv <folder or file>`). Its build reads the game's
Yaz0 packages (`Pack/`, `Effect/`, `Event/`, `System/`: the `uking_*.sbfsha` material archives and the engine's agl
packages) and translates the boot and title packages' shaders for the default bind state plus every state the list
names (`games/botw/scripts/prepare_shader_seed.py`). Its players' contribution file is
`~/Library/Application Support/MacNativeGames/BotW/shader-list-contribution.txt`, sent with the
[Breath of the Wild issue form](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list-botw.yml)
(label `shader-list-botw`; `collect_shader_issues.py` merges both games' issues). The lists never mix: a key names a
shader of one game only.

Its shader key hashes the control section's own length (the word at +0x20: 0x7d1 for the game's archives, 0x848 for
agl's; Tears of the Kingdom's is 0x880, so its keys are unchanged) and, for the agl programs the game loads from
embedded BNSH files, the bytes as the game rewrites them before binding (`prepare_shader_seed.py bound_control`); the
agl packages' own records are bound unchanged.

Title rows (`t`) of either game must be recorded in a cache that has the prepared seed imported (the warm-up loads a
row's shader by its file name, and the seed names its variants by their bind-state answers, not as a cache translated
on the spot would): run the game with `DM_PSO_MANIFEST=off`, an empty `DM_NVN_SHMAP` folder and
`TOTK_SHADER_SEED_DIR=<prepared seed>`, then `shader_list.py merge --title ... <profile>/pso_user.tsv`.

## Merging what players sent (maintainers)

```sh
# what each open shader-list issue of either game would add (changes nothing). Each --seed gives a game version's
# shader keys (its shader-keys.json) and drops the rows its built-in variants serve; --keys <game>=<file> adds a key list
S=<work>/totk/build/shader-seed
python3 games/totk/scripts/collect_shader_issues.py --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160
# write the lists; --close also thanks each merged issue and closes it
python3 games/totk/scripts/collect_shader_issues.py --seed $S/v143 --seed <botw work>/totk/build/shader-seed/botw160 --apply --close
# drop rows already in the list that the seed's default (or depth-bound) variants serve
python3 games/totk/scripts/shader_list.py prune games/totk/assets/shader_list.tsv <work>/totk/build/shader-seed/v143
```

It takes only `V` rows and render pipeline `R` / `B` rows from an attachment (or from a comment's, when the upload
failed) and drops everything else. One contribution file serves both games, so every row goes to the game its shader
keys belong to, whatever the issue's title says. Rows of shaders neither game's keys hold are not merged and keep their
issue open: a key list may lack a game version (give a seed of every version collected; a seed made before 2026-10-22
has no shader-keys.json and is refused), or the file is another game's (#525, Super Mario Odyssey). Once checked,
`--drop-unknown` leaves them out and closes the issue. Then check with `shader_list.py stats`, commit
the list and push; the next builds carry the new variants.

## Adding what you played (with git)

Use `games/totk/scripts/shader_list.py`. It needs only Python 3.

```sh
# Mac: add the shaders your own translation cache holds
python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv \
  "$HOME/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v8-unverified"

# check the result, then commit the list and open a pull request
python3 games/totk/scripts/shader_list.py stats games/totk/assets/shader_list.tsv
```

- **iPhone / iPad:** `games/totk/ios/tools/fill_shaders.py` translates what a device missed. It also merges those
  variants into the list automatically.
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
  - `S`、`C`、`R` 行：预热用的着色器库和管线。`B` 行：同一条管线带着它当时采样器的边框颜色（Metal 没有的颜色要在着色器里模拟），预热按它建好补丁着色器和这条管线。标记 `t` 是标题画面用的（第一帧只等这些），`m` 是其余预热。
  - 只有标识，不含游戏数据。
- **玩家怎么贡献**：玩的时候什么都不用做。Mac 现场翻译的变体、iPhone / iPad 缺的变体，都说明清单里还没有，游戏会把它记进 `shader-list-contribution.txt`（Mac 在 `~/Library/Application Support/MacNativeGames/TOTK/`，iPhone / iPad 在“文件”App › 我的 iPhone（或 iPad）› Tears of the Kingdom）。每次打包都带着每个着色器的默认变体，所以只记默认变体用不了的状态，默认变体从不记（清单只收非默认变体，2026-10-22）。玩的时候建的、App 预热清单里没有的渲染管线也一起记（`R` 行，带边框色的再加 `B` 行；变体名换成别人打包时也会有的名字）。着色器和管线合计每攒够 100 条，启动器会在游戏开始前问一次要不要发送；iPhone / iPad 玩的时候从不提醒，设备连着打包它的那台 Mac 时，由 Mac 上的 `ios/tools/shader_contrib_watch.py`（打包 iOS 版时装的后台任务）把文件拷过来，每台设备每多 100 条在 Mac 上问一次。“下次提醒”等到下一个 100 条再问，“不要提醒了”以后就不再问（文件照样记）；点“发送”会打开 GitHub 的[提交着色器清单](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)页面，把文件拖进去即可。下次在 200、300 条时再提醒，每次发整个文件也没关系，清单里已有的会跳过。游戏不会自己上传任何东西。
- **维护者合并**：`python3 games/totk/scripts/collect_shader_issues.py --seed <打包目录>/totk/build/shader-seed/v143 --seed <旷野之息打包目录>/totk/build/shader-seed/botw160` 先看两款游戏的每个 Issue 能加多少；`--apply --close` 写进清单、回复并关闭 Issue。附件（上传失败时也看评论里补传的）里只取 `V` 行和管线的 `R` / `B` 行，种子自带的默认变体能用的 `V` 行丢掉。贡献文件两款游戏共用，所以每一行按它的着色器键归到所属游戏，不看 Issue 标题；每个 `--seed` 提供该版本的键（`shader-keys.json`，2026-10-22 以前的旧种子没有，会被拒绝），`--keys <游戏>=<文件>` 可再加键清单。两款游戏的键都不认识的行不并入、Issue 保持开着（可能少给了某个版本的种子，也可能是别的游戏的文件，如奥德赛 #525）；核对后用 `--drop-unknown` 丢掉并关闭。旧行用 `shader_list.py prune <清单> <种子目录>` 清掉。
- **用 git 贡献**：
  - Mac：`python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv <自己的翻译缓存目录>`，再运行 `locate_shaders.py` 补上定位行，用 `stats` 检查，提交清单并提 PR。
  - iOS：`fill_shaders.py` 补译后会自动把变体并进清单。
- **审核**：`stats` 和 `tests/test_shader_list.py` 要通过；PR 只能改这一个文件，不能带翻译出的 `.metal`、编好的 `.metallib` 或缓存。
- **旷野之息**：自己的一份清单 `games/botw/assets/shader_list.tsv`，格式、工具都一样（`merge` 时写它的路径）。打包时读它的 Yaz0 包（`Pack/`、`Effect/`、`Event/`、`System/`），启动包和标题包的着色器按默认绑定状态翻译、清单里的绑定状态也翻译。玩家的贡献文件在 `~/Library/Application Support/MacNativeGames/BotW/`，用旷野之息自己的 Issue 表单（标签 `shader-list-botw`），`collect_shader_issues.py --game botw` 合并。两份清单不混：一个键只属于一个游戏。它的键按控制段自己的长度算（+0x20 处的字：档案里 0x7d1、agl 的 0x848；王国之泪是 0x880，键不变）；agl 包里嵌着的 BNSH 程序还按游戏绑定前改写过的字节算（`bound_control`），agl 包自己的记录原样绑定。
