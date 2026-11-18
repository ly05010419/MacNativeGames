# OOT 全量离线着色器

OOT 的标准 Mac 构建从已核对的原始游戏数据提取着色器，经本游戏的运行时翻译，再编成 Metal 库。种子位于外部工作目录的 `oot/build/shader-seed/oot`，库位于 `oot/build/shader-libs`；打包后保存在独立签名的 `Contents/Helpers/ShaderData-oot.bundle`，运行时从 `Contents/Resources/totk/build/shader-seed` 和 `shader-libs` 的相对链接读取。

## 数据来源与数量

[nxnative PR #1299](https://github.com/seedds/nxnative/pull/1299) 的固定参考版本是 `ee9b7765d579c46e45a7297493565e098f3bda33`。它提供提取方法、键和来源清单；我们的 MSL、反射、Metal 库从用户的游戏数据及本地翻译链生成，不直接使用其他运行时的库。

本地更新 131072 的完整集合：

| 阶段 | 数量 |
| --- | ---: |
| vertex | 15,851 |
| pixel | 22,352 |
| task | 20 |
| mesh | 500 |
| compute | 4,122 |
| 合计 | 42,845 |

42,845 个键全部属于 PR 的 42,851 项。多出的六个短键为 `18be578dfdaaffa0`、`269e3cb1372760f6`、`2bb5cb2b65db33ad`、`68bbe98b3bfd9876`、`7a4936fccf82094e`、`e87e4c611ade7637`。逐项从原文件复现后，六项所谓的 BAHS 条目均位于一个有效控制块的 `+0x38`，属于该块的长度字段。把这些字段误读为条目会拼出伪造的控制/代码配对。提取器按控制块的所有权排除它们，不用数量门槛补齐。

NVN2 BNSH 的 task/mesh 指针位于 program 的 slots 7/8。提取器读取这些槽位并检查范围；离线运行时也必须接收 stage 3/4，不能沿用 NVN1 的 tessellation 排除规则。

## 翻译与构建校验

使用真实游戏记录的 `minus-w` 深度模式。缓存上下文包括运行时、翻译器、翻译链及深度模式；每次准备在自己的上下文目录保留独立翻译器，避免其他打包覆盖共享可执行文件导致 macOS `Code Signature Invalid`。

离线入口的退出码不能单独证明完整：还要从二进制 corpus 重新计数和计算键，检查每个默认变体和已记录绑定变体、反射链及入口，任何缺项都停止发布。Metal 库按 MSL 的 SHA-256 命名，编译器版本与精确数学选项记录在 `libraries-manifest.json`，已有库复用前核对哈希。打包再次核对种子和对应库，并将两个清单的哈希写入 `build/game-data/oot/precompiled-shaders.json`，纳入原有完整构建记录。

着色器资源包按上述两个清单的哈希复用签名，避免更新运行时时重新签名十多万个文件。复用哈希校验结果时，检查设备号、inode、大小、mtime 和 ctime；同大小、恢复 mtime 的内容修改也必须重新核验。`test_shader_package.py` 覆盖该失效路径、不同 RomFS 或翻译器、未完成库集合和真实的嵌套代码签名。

两个真实 compute 着色器（键前缀 `1f78730053fe`、`e172bd7ec218`，来源 `agl_technique_pfx.sharcb`）的结构化 MSL 有 257 层花括号，超过 Apple Metal 编译器默认的 256 层解析限制。只有编译器明确报告这个错误时，构建才以 `-fbracket-depth=1024` 重试，原 MSL 和数学选项保持不变；清单的 `parser_depths` 记录这些库。其他失败仍停止打包，已核验的成功库可供重试复用。

首次运行默认仅准备真实管线清单中的项目，其余预编译库留在磁盘，用到时加载。完整原始着色器集合并不等于完整管线集合；运行时绑定组合、采样器边界等派生变体仍可能产生新管线。

`build_shader_libs.py` 可重复传 `--cache`，同时准备完整原始集合和已有的真实绑定变体；同名变体内容冲突时停止。后续构建保留同一编译器已核验的库，打包也校验并包含这些附加库。不能只按原始默认集合筛掉既有库：2026-11-06 本机 986 份实际运行 MSL 中有 202 个库不属于默认集合，漏带会使已有准备退回现场编译。

## ISBERD 输出负载读取

原始 task `5d3a096025e375a16a5bb4da22b58e9c91ee7fa51b785cd4d90774f119570500` 在代码 `+0xf0` 用 `ISBEWR 0xb8` 写输出负载 `+0x10`，紧接 `+0x100` 用 `ISBERD 0xb8` 读取同一位置。旧翻译器拒绝该读取。

[Mesa 的 ISBE 说明](https://docs.mesa3d.org/drivers/nvk/isbe_layout.html) 区分输入和输出空间；其 NAK `sm70_encode.rs` 的 `OpIsberd` 编码明确将 bit 79 定义为 output，bits 74..77 为数据宽度和区域。补丁 `0024-task-output-payload-read.patch` 将该读取路由到与输出写入相同的 stage memory 区域，保留原有输入读取语义。

`tests/stage_payload_gpu.py` 生成真实指令编码，经生产翻译器生成任务主体；只把外层入口换成可读回的 compute 包装。`stage_payload_gpu.m` 在 Metal 4 队列执行 28 项，独立检查输入/输出区隔离、寄存器与立即数偏移、workgroup 号和完整 32 位数据。`OOT_STAGE_READ_NEGATIVE=1` 故意改错输出期望值，必须失败。该探针验证指令语义，不代替游戏画面验收。

2026-11-06：修复后完整翻译 42,845/42,845，缺项为零。原先成功的 42,844 项全部逐字节对拍：42,844 个 SPIR-V、42,844 个 MSL、85,688 个 JSON（反射及中间结果）和 42,844 个变体索引，变化和缺失均为零。新增语义的 28 项 GPU 读回通过，负对照在首个输出读取用例返回失败。

Apple Metal `32023.883` 将 42,845 份 MSL 去重为 41,217 个库，全部编译通过。首次 41,215 个通过，另外两个按上述解析深度选项补编通过；最终 `failed.log` 为空，清单 `complete=true`。这证明完整语料的翻译和编译覆盖，实际游玩中使用的绑定变体及画面仍需单独验收。

加入本机已记录的 986 份绑定变体后，合计覆盖 43,831 份 MSL、41,419 个不同内容的 Metal 库；额外的 202 个库全部补编通过。原始 corpus 仍为 42,845 个，不能把绑定变体算成新的原始着色器。

最终资源已装入同一个桌面 App，构建记录与 worktree 源码核对无差异。2026-11-06 的最终包可执行文件 SHA-256 为 `be0f836086e25aa3b935ad265fc431797d7749e8db985b5ed1d7ed3f7724f45b`。用户操作 BotW 期间让出游戏槽；结束后在同包、同活动档、全新着色器缓存下完成验收，保持默认预热，未使用关闭预热的开关。

最终冷缓存局：72.47 秒达到世界条件，按右方向键 4 秒转镜头，世界内运行 40 秒后正常 close，exit 0，总计 113.16 秒。全部 42,845 种子原地读取、零复制；首次无管线存档时只建实际清单，不把 42k 库全部载入内存。日志最后一次库计数为预编译 801、现场编译 0；新增的 168 份绑定变体 MSL，其全部源哈希均有现成库。无 GPU、翻译或库加载错误，两张 1920×1080 截图可见人物、草地、树屋、光照和 HUD 完整，镜头位置确实改变；这是短局可见画面检查，不代表全游戏逐像素验证。

同包另有内存会话完成 90 秒世界左右镜头和短对照，均正常退出。执行前系统检查与首次设备 PSO 编译等待单独记录，不算稳态性能。完整原始库及已记录绑定库预编译通过，未来新的绑定组合、合并程序和设备 PSO 仍可能首次生成；此次没有 OOT iOS 真机验收。

## 本机证据

外部目录：`MacNativeGames-artifacts/codex/oot-native-20261105/shader-prewarm-20261106/`。`inventory.json`、`upstream-comparison.json`、`upstream-six-extra-records.json` 保留数量及六项差异的来源；`seed.log` 是修复前完整翻译的失败基线，`verified/` 是修复后的完整准备日志。生成的机器码、MSL 和库不得放入源码仓库。

`preparation-result.json` 汇总全量翻译、GPU 指令读回、库编译、资源签名及 12 项回归；`installed-app.json` 核对实际安装包；`runtime/result.json`、`runtime/game.log`、`runtime/world.png` 和 `runtime/turned.png` 是最终冷缓存实机证据。

## Render pipelines: the shared list (2026-11-09)

Until today the packaged App warmed only compute pipelines (`build/pipelines`: the seed's compute variants); every render pipeline was built in play. The game records what it builds into the profile's `pso_user.tsv` (S / C / R / T rows, this build's variant names) and the contribution file `shader-list-contribution.txt` (V rows with bind states, R / T rows renamed portably). `scripts/shader_list.py` (the TOTK list code, which now knows `oot`) merges them into **`games/oot/assets/shader_list.tsv`**: V rows for the bind states the game met, A rows naming them, and the warm rows. First merge: the 21 runtime shader-cache folders of the active profile (their `.variants` indexes rename locally named rows to the listed names), the App's seed index, the contribution and `pso_user.tsv`: 43,422 V over 42,848 shaders (1,909 bind states), 3,168 R, 2,681 S, 244 C, 19 T; 103 rows refused (bind states no cache knows).

```
python3 -I scripts/shader_list.py merge assets/shader_list.tsv <profile>/shader-cache/*/ <App seed dir> <profile>/shader-list-contribution.txt <profile>/profile/pso_user.tsv
python3 -I scripts/shader_list.py stats assets/shader_list.tsv
```

Packaging (`build.py`): `prepare_ns2_seed.py --list assets/shader_list.tsv` translates the listed bind states into the seed; at packaging `resolve_lists` resolves the current list against that seed into `pso_title.tsv` / `pso_manifest.tsv` (each variant named as the seed serves it, `shader_list.py resolve`), and `compute_pipeline_archive` passes those same files, the ones the App carries in `build/`, to the game's `pipeline-archive` mode (a fast update keeps the seed, so the seed's own copies can be of an older list: 2026-11-11 they named 2,957 render pipelines, the App's 7,516) with the compute plan, so the Metal pipeline archive in `build/pipelines` holds the listed render pipelines too, and the first start loads them (`par_load_prebuilt`) instead of building them. 
### What came from seedds/nxnative (MIT, main `79741184af15`, 2026-11-09)

- **Shader keys.** Its `dm/games/0400486024DEC000/shader_map.tsv` lists 42,848 keys, our seed 42,845: the three more are `agl_ndsr_vr` / `agl_ndsr_vr_light` compute kernels. Our census walked NDSR records in fixed 16-byte steps and lost whatever followed an 8-byte constant-data entry; `ns2_shader_corpus.py` now walks them as its `ndsr_shaders` does (regression test in `tests/test_shader_corpus.py`). Read from the game files, all three keys appear. The game uses at least one of them (`d17eefaff4f0bc23`: 86 files in the runtime caches of the active profile), so it had been translated at run time.
- **Render pipelines.** Its `pso_manifest.tsv` was recorded over a first start. `scripts/import_nxnative_manifest.py` converts its R rows: the same `pso_skey` with 24-byte names against our 80 and three additions at the end of `pso_key` (`atest`, `logic`, `attr_hi`). All 1,112 convert (none uses state we cannot hold); 636 are byte for byte rows we had recorded ourselves, which is the check that the fields mean the same, and the other 476 are new. They are named after the default variant, so a draw that picks another variant does not hit them; that costs a pipeline built at packaging and nothing in play.
- **Mesh pipelines.** Its 123 M rows convert the same way (`-` or the task shader, then the key with the mesh shader in the vertex place). All 123 are in the list; they were recorded in Kokiri Forest, Hyrule Field, Castle Town and Hyrule Castle.
- **Not imported.** C rows (every compute variant is already planned from the seed) and S rows (`fill_row` and `warm_build_row` load a warm pipeline's shaders themselves).

### Mesh pipelines (M rows, 2026-11-09)

`mtk_pso` (`nvn_mesh_tasks.inc`) built each mesh pipeline at its first draw, on the submit thread, through `MTLDevice`: 49-50 per run in the measured scenes, nothing recorded. nxnative measured the same design on a cold Metal cache: an M5 Max entering Kokiri Forest built 63 pipelines in 59.5 s with one frame of 42.9 s, an M1 took 103.5 s with a 12.76 s frame.

- **Row:** `M <task variant or -> <pipeline key>`, the mesh shader in the key's vertex place. `mtk_pso` writes one to the profile's `pso_user.tsv` when it builds at a draw; `shader_list.py` keeps them like R rows.
- **Warm-up:** `warm_build_row` hands them to `mtk_warm_row`, on the warm-up threads, through the same `mtk_build` the draw uses (the shared-state paths take `g_vpatch_lock` / `g_fn_src_lock`; rows are skipped while a diagnostic variant is on). `warm_claim` files the pipeline in `g_mtk_cache` under the live shader objects (kind 3). Not in the pipeline archive: `fill_row` ignores M rows, so every start builds them, from the Metal system cache after the first.
- **Which variant:** the name in a row is what that run's cache chose. A fresh profile asks for the default variant (64 of 66 mesh shaders on a first start, `fs1`), the long-played profile for `-04ec2b7c22af_b`, so both kinds are listed.
- **Verified in the game** (same App, active save, default settings): `mr1` built 52 at the draw and recorded them; `mr3`, with the rows, built 52 in the warm-up (`[mesh] warm-up: 52 mesh pipelines built, 0 not built`) and **0 at a draw**, 0 white frames, the same picture, no new red lines. `probe-gpu-mesh` 7,920, `probe-gpu-mesh-batch` 50, `probe-gpu-draw-barriers` 32,896 pass. On this Mac the Metal cache was warm (66 builds at the draw: 689 ms in all), so the seconds per pipeline of a cold cache were not reproduced here; nxnative's figures above are the reason for the work.

### Recording a first start

`scripts/dev/record_first_start.py --out <new folder> --app <the .app> [--watchdog <script>] [--merge]` runs the App on an empty profile, shader cache and contribution file, presses A every 4 s (title, New Game, the opening, Link's house), walks after 5 minutes, screenshots every 30 s. Two runs on 2026-11-09 reached Link's house and the forest outside. `fs1`: 1,078 R, 124 C, 1,241 S rows, 312 bind states; one frame at the start of the opening (frame 750, the first 3D scene) took 38.6 s with 286 pipelines built on the spot (`fs2`, right after it on the warm Metal cache: 10.5 s for the same moment).
