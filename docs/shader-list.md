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

## Adding what you played

Use `games/totk/scripts/shader_list.py`. It needs only Python 3.

```sh
# Mac: add the shaders your own translation cache holds
python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv \
  "$HOME/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v4-unverified"

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
  - `S`、`C`、`R` 行：预热用的着色器库和管线。标记 `t` 是标题画面用的（第一帧只等这些），`m` 是其余预热。
  - 只有标识，不含游戏数据。
- **怎么贡献**：
  - Mac：`python3 games/totk/scripts/shader_list.py merge games/totk/assets/shader_list.tsv <自己的翻译缓存目录>`，再运行 `locate_shaders.py` 补上定位行，用 `stats` 检查，提交清单并提 PR。
  - iOS：`fill_shaders.py` 补译后会自动把变体并进清单。
- **审核**：`stats` 和 `tests/test_shader_list.py` 要通过；PR 只能改这一个文件，不能带翻译出的 `.metal`、编好的 `.metallib` 或缓存。
