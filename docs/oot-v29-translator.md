# 时之笛翻译链 v29（停放的分支 `oot-v29-translator`）

## 这个分支是什么

`main` 在 2026-11-19 用 `778ab5f9` 把时之笛撤回到翻译链 v28。这个分支在当时的 `main` 上把那次撤回再撤回，
所以它的文件和 v29 最后一次构建所用的 `6a1ab7a3` 逐字节相同（`git diff 6a1ab7a3 oot-v29-translator` 为空）。

v29 比 v28 多两处翻译输出的改动，都从 nxnative 移植：

| 改动 | 提交 | 改什么 | 有没有不改翻译器的做法 |
|---|---|---|---|
| 无 task 的 mesh 着色器按 1024 分块 | `d47c358b`（翻译器部分 `MeshStage.cs`） | 多生成 object 函数 `main0_o`，按每块 1024 个发射工作组；运行时部分已在 `main` | 有：宿主把一次绘制拆成最多 8 次（上限 8,192），nxnative 没采用 |
| 标量 `spvMulExtended` 内联 | `f361b840`（`Service.cs`） | 去掉 SPIRV-Cross 的 optnone 模板，常数除法能折叠 | 部分：运行时改写 `OOT_UMUL_INLINE`，只覆盖游玩中现编的着色器 |
| 链名 v29、翻译器二进制、账本 | `02989095` | `oot-native-v29-mesh-blocks-mulext`，`totk_xlate` 代码哈希 `0628c398…` | — |

数据：路线 oot-fps90 n90d 里 608 次无 task 的间接 mesh 绘制有 40 次超过 1024（最多 1,238，树叶 mesh d98076e4 /
fed302d7）；nxnative 报 1,771 个越界、GPU page fault。乘法内联 nxnative 报 Kokiri 54.0 → 59.1 fps（M4 Pro），我们
2026-11-19 的回放在系统后台分析占满 GPU 时测不出差别。

## 为什么停放

翻译器一变，每个玩家下次更新都要把全部 42,911 个着色器重翻、重编着色器库、重建管线存档：这台 M4 上 83 分钟
（2026-11-19 桌面 App 的 v29 更新：翻译和编库约 25 分钟，管线存档 51 分钟，其中约 17% 的管线真的变了）。
这次改翻译器之前没有问用户必要性（用户 2026-11-19：“我说了 更新 翻译器 要 询问我必要性 加了测试 为什么 没有问我？”），
用户决定撤回，留着这份工作以后用（“v29 以后还是需要的 我不想再等一小时了”）。

## 再用之前

1. **先问用户**：为什么要换翻译器、有没有必要、玩家要等多久、上表的替代做法为什么不够。用户同意了才合。
2. 把这个分支变基到最新的 `main`。冲突多半在 `tools/translator_changes.json`（`main` 的账本多了撤回那一条）和
   `dmsh_chain.h`。账本：保留 `main` 的全部历史，再加一条“重新启用 v29”，`why` / `necessary` / `evidence` 写清，
   附上用户同意的原话和日期；`code_sha256` 设成 `0628c398…`。`python3.15 -m unittest tests.test_translator_changes`。
3. 如果 `main` 上 `games/oot/tools/totk_xlate` 的源码又改过，翻译器要用仓库源码重编（NativeAOT，做法见
   `issues_lessons.md` 2026-10-28“预编译二进制重编核对”），代码哈希就不是 `0628c398…` 了，下面保存的翻译和管线不能直接用。
4. 测试：`tests.test_oot_mesh_blocks tests.test_oot_mul_extended tests.test_oot_mesh_output_layout tests.test_translator_changes`
   （走 storage_guard `--kind gpu`）。

## 编好的着色器和管线（不用再等一小时）

都在仓库外，APFS 克隆，几乎不占额外空间（`MacNativeGames-artifacts/keep/oot-chains/`，说明在那里的 `README.md`）：

| 文件 | v29（`6a1ab7a3` 构建） | v28（v29 之前的桌面 App，`4ffc237`） |
|---|---|---|
| App | `v29/Ocarina of Time.app` | `v28/Ocarina of Time.app` |
| 翻译缓存（工作文件夹 `oot/build/ns2-seed-cache/oot/<上下文>`） | `v29/ns2-seed-cache-7360b`（上下文 `7360b69c…`） | `v28/ns2-seed-cache-2ce84`（上下文 `2ce84250…`） |
| 种子、着色器库 | `v29/shader-seed`、`v29/shader-libs`、`v29/shader-libs.verified.json` | 没存（下次 v28 更新用翻译缓存重做） |
| 管线存档工具的缓存 | `v29/compute-pipelines/archive`、`compute.tsv`、`v29/pipelines`、`v29/pipeline-bundles` | 没存（v28 更新会重编 v29 改过的那些管线） |
| 预热清单 | `v29/warm-lists` | — |

翻译上下文 = 链名 + 深度模式 + 翻译器代码（`games/oot/scripts/prepare_ns2_seed.py` `translation_context`），所以
只要链名和翻译器代码对得上，缓存就会被沿用；运行时的改动不影响它。

### 只想玩或测 v29

关掉游戏，先把当前的桌面 App 存好，再换上：

```sh
A=/Volumes/开发/MacNativeGames-artifacts
rm -rf "$A/keep/apps/Ocarina of Time.app"
cp -c -R "$A/keep/oot-chains/v29/Ocarina of Time.app" "$A/keep/apps/Ocarina of Time.app"
codesign --verify --deep --strict "$A/keep/apps/Ocarina of Time.app"
```

第一次启动：本机缓存 `~/Library/Application Support/MacNativeGames/OcarinaOfTime/shader-cache/rt-oot-native-v29-…`
新建，App 的种子原地读取；本机的管线存档（`pipelines`、`pso_collected.tsv`）会从 v28 的文件夹**挪**过来
（`totk_rt_carry.h`），换回 v28 时再挪回去，各自重编变了的那几条。

### 在这个分支上继续构建（update_runtime 不重翻）

先把工作文件夹里现在的东西存好（`update_runtime` 收尾会删掉别的上下文的翻译缓存，2026-11-19 就删了 v28 的），再放回 v29 的：

```sh
A=/Volumes/开发/MacNativeGames-artifacts; B=$A/scratch/build/oot/oot/build; V=$A/keep/oot-chains/v29
cp -c -R $V/ns2-seed-cache-7360b $B/ns2-seed-cache/oot/7360b69ce87c2dd2570cc15cbbd24d1164594ca969e2cbe0eefd7d857a665fe1
for d in shader-seed shader-libs pipelines pipeline-bundles warm-lists; do rm -rf $B/$d; cp -c -R $V/$d $B/$d; done
cp -c $V/shader-libs.verified.json $B/
rm -rf $B/compute-pipelines/archive; cp -c -R $V/compute-pipelines/archive $B/compute-pipelines/archive
cp -c $V/compute-pipelines/compute.tsv $B/compute-pipelines/
```

然后在这个分支的 worktree 里按 CLAUDE.md 的命令跑 `update_runtime.py "<App>" --work $A/scratch/build/oot`
（`--plan` 先看；storage_guard `--kind build`）。预期：翻译全部沿用，着色器库不重编，管线存档逐条核对、几乎全部从存档取。

### 换回 v28

同样的做法用 `v28/` 的 App 和 `v28/ns2-seed-cache-2ce84`（放回 `ns2-seed-cache/oot/2ce8425037fbd8ae2bf2fca0f849f4b16f6121edb5f44d2a6ca59216cc8d1d9b`）。
v28 没存种子、着色器库和管线缓存：下次 v28 更新会用翻译缓存重做种子，重编 v29 改过的库和管线。
