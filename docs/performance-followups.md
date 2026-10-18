# Mac 性能待做与复测方法

本页保留原开发树 `native-port/TODO.md`、`docs/testing-methods.md` 中 2026-10-17 的重要结论，并与本工具的 [1.4.3 记录](v1.4.3-port.md) 分开。旧实验的开关、源码和二进制须按具体分支核对，不能把旧版通过或未合入的实验当作当前 1.4.3 验收。这里只安排 Mac；不代表 iPhone 已测。

## Metal HUD

Mac 保留原有 FPS、逐核 CPU/GPU 负载和着色器收集提示，额外在左上角显示 Apple Metal Performance HUD，避免遮挡右上角的原 FPS 面板。按键 **1** 统一隐藏或恢复全部提示。启动器在执行游戏前设置环境；直接开发启动在创建 Metal device/layer 前补齐默认值。显式环境参数优先，不改系统全局偏好。

| 参数 | 默认值 | 用途 |
|---|---|---|
| `MTL_HUD_ENABLED` | `1` | 额外的官方性能面板；`0` 仅关闭这一面板 |
| `MTL_HUD_LOG_ENABLED` | `0` | `1` 开启官方逐帧性能日志 |
| `MTL_HUD_ALIGNMENT` | `topleft` | 额外面板的位置 |
| `TOTK_FPS_OVERLAY` | `1` | 原游戏 FPS/补帧分项显示；`0` 单独关闭 |
| `TOTK_LOAD_OVERLAY` | `1` | 原 CPU/GPU 负载采样及面板；`0` 单独关闭 |

Metal HUD 的 FPS 按呈现帧统计，包括补帧，与游戏提交帧率不一定相同；GPU 时间也不等于原面板的 GPU 利用率百分比。着色器收集提示表示本项目翻译缓存的收集情况，与 Metal 的编译活动是两种指标。内部计时、音频、等待、翻译诊断及 `DM_NVN_TSV` 等原有开关保留。

默认不启用额外 encoder GPU counter 跟踪。需要深入分析时单独验证其与现有计数器采样的兼容性。官方日志可用 Console 按游戏进程及 `com.apple.metal.hud` 过滤，不能假定所有系统版本都会把它写入 `totk.log`。

### 验证

`tests/metal_hud_ui_probe.m` 使用真实 Mac 前端与按键 1 的异步派发，不运行游戏、不提交 GPU 命令；检查原面板保持开启、HUD 单独关闭、日志开关、显隐恢复及 layer 位置设置保留。它需要 macOS 图形会话。将编译产物放在源码树外，依次以默认环境、`MTL_HUD_ENABLED=0`、`MTL_HUD_LOG_ENABLED=1` 运行；参数是用户数据目录 `tests` 下的独立 profile。

```sh
hud_build=$(mktemp -d "${TMPDIR:-/tmp}/mng-metal-hud.XXXXXX")
xcrun clang -std=gnu11 -fobjc-arc -Wall -Wextra -Werror \
  -Wno-deprecated-declarations -undefined dynamic_lookup \
  -Igames/totk/third_party/d2r_gfx/gfx/nvn/include \
  tests/metal_hud_ui_probe.m games/totk/third_party/d2r_gfx/gfx/nvn/fps_overlay.c \
  -framework Cocoa -framework Metal -framework QuartzCore -framework CoreVideo \
  -framework CoreGraphics -framework CoreText -o "$hud_build/probe"
hud_profile="$HOME/Library/Application Support/MacNativeGames/TOTK/tests/metal-hud-controls/profile"
mkdir -p "$hud_profile"
env -u MTL_HUD_ENABLED -u MTL_HUD_LOG_ENABLED -u MTL_HUD_ALIGNMENT \
  -u TOTK_FPS_OVERLAY -u TOTK_LOAD_OVERLAY "$hud_build/probe" "$hud_profile"
env MTL_HUD_ENABLED=0 MTL_HUD_ALIGNMENT=topleft TOTK_FPS_OVERLAY=1 \
  TOTK_LOAD_OVERLAY=1 "$hud_build/probe" "$hud_profile"
env MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 MTL_HUD_ALIGNMENT=topleft \
  TOTK_FPS_OVERLAY=1 TOTK_LOAD_OVERLAY=1 "$hud_build/probe" "$hud_profile"
```

本机已构建的 1.4.3 App 标题画面确认左上 Metal HUD、右上原 FPS、底部负载及着色器提示同时存在；这属于显示验证，不是性能跑分或长局验证。历史截图、签名与测试记录位于源码树外的 `/Volumes/mac-APFS/MacNativeGames-artifacts/2026-10-18/metal-hud-additional`；该 App 包含其他本地改动，不能代替本分支的独立源码检查。

参考：[启用与日志](https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance)、[layer 开关与位置](https://developer.apple.com/documentation/xcode/customizing-metal-performance-hud)、[指标定义](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics)。

## 已有实验应保留的结论

旧版 M4 / macOS 26.5.2、监视堡垒存档副本，取消显示节拍限制的渲染能力测试；不是实际上屏 FPS。每组同一二进制，8 个 30 秒平衡交替窗口。下表是每条件 4 个窗口分位数的算术均值，不是合并帧分位数；天气和时间仍自然变化。

| 旧实验 | 基线 p50 / p95（ms） | 实验 p50 / p95（ms） | 结论 |
|---|---|---|---|
| 同次绘制准备结果复用 | 11.839 / 12.624 | 11.904 / 12.661 | 未发现收益，p50 慢约 0.55% |
| 复制批处理 | 11.741 / 12.549 | 11.727 / 12.516 | 实际优化 ops/barriers/saved 均为 0，性能未覆盖 |
| 500 µs 提交预算 | 11.690 / 12.517 | 11.636 / 12.497 | 约 0.46% 差异不足以启用 |

后续 5 组提交预算的 p50 变化约 −0.68% 到 +0.28%，仍没有可靠收益；保留首段 60 / 后续 75 draw 的默认策略。全段较小预算可以让 GPU 更早开工，却增加后续空档，不能只看首次开工就称 GPU 等待已解决。

旧成本计数：编码阶段 resource-ID 查询 99.46% 为快路，约 11 ns；准备阶段快路约 90.22%，慢路约 566 ns。这些包括计量开销、只计持锁后的局部解析，不含锁等待或整个函数，不能直接乘调用数换算提帧。

旧实验树对读后写、写后读、写后写及别名做过受控 GPU 验证：普通 Device 屏障不足以覆盖 `mach_vm_remap` / 多个 no-copy buffer 包装的别名，曾需要 ResourceAlias 可见性。该正确性结论应保留；移植到本工具的具体路径仍须独立核查和正反例验证，不能用另一个树的通过记录代替。fill 的 helper 覆盖也不等于真实游戏 fill 路径已覆盖。

## 后续优先级

1. **1.4.3 成本基线。** 固定构建、存档、镜头、内部与输出尺寸、缓存和呈现方式。先分别量总 CPU 工作、关键路径、GPU 执行/空档、复制字节和资源解析成本。
2. **准备阶段慢路和失效范围。** 先证明重复工作及误失效，保留上传、别名切换、uniform 快照、GPU 写回和完成寿命。已有编码线程不自动意味着再加线程会提帧。
3. **实际复制负载。** 找到真实 buffer copy / update fallback，确认计数非零，再测依赖批处理。保留现有 uniform rename，不关闭已有优化制造命中；未知依赖、区间重叠和别名必须保守处理。
4. **长期内存。** 区分游戏堆、序号表、贴图池槽位、管线/驱动缓存、驻留集合与 GPU 完成后的实际对象。`mtl_dead_forget` 当前保守保留已销毁序号；缩表前须补齐槽位引用关系，不能重现旧的过早遗忘错误。1.4.3 的补帧配置复用只针对重复构建，其他增长仍须归因。
5. **首次编译卡顿。** 同路线分别记录冷/热缓存，收集完整帧时间。相对尖峰日志会漏掉普通慢帧；截图 FPS、固定镜头平均值、补帧数都不能证明稳定 30/60。
6. **场景、动作及真实显示器。** 复杂能力/战斗/过场、菜单传送与跨场景资源退役、长时间快速切档、物理 4K 和高刷新率分别检查。版本对比必须同机同负载，不比较不同游戏/设备的单张 FPS。

## 每轮测试的最低记录

- 记录提交、未提交 diff、新增源码、实际二进制 SHA-256、所选版本/资源哈希和环境；只有 commit 名不够。
- 使用独立 profile 和唯一共享 game-slot，用户进程优先；禁止两份游戏/GPU 探针并行。
- 每个性能候选至少 8 × 30 秒 A/B/B/A/B/A/A/B，保留全部慢帧。采样、参考解析核对、截图和重型诊断与正式测速分窗。
- 区分世界、菜单、加载和过场；记录真实单调时间、帧号、位置/视角、天气和缓存变化。没有严格固定的变量写明限制。
- 报告完整帧分布的 p50/p95/p99/最大值和超预算比例；不要平均分位数后当作合并结果，也不要将重叠 CPU/GPU 时间相加。
- 同时核对实际画面及执行错误。复制组记录 ops/barriers/saved 差值；计数为零就是未覆盖。
- 内存分别保留 RSS、footprint、进程峰值、Metal 分配、驻留对象及完成后释放；一个总数不能证明泄漏或平台期。重型诊断记录时间窗口。
- 通过、无收益、未覆盖、负对照失败和被中断的测试分别记录；不为追求分数删掉卡顿或异常。

旧版代表场景 p95≤10 ms 是阶段目标，尚不是全图承诺；1.4.3 须建立自己的基线。单项正确性修复、版本适配和内存改进也应分别交代，不能都称为 FPS 提升。
