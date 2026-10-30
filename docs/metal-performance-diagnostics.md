# Metal performance diagnostics on macOS 27

macOS 27 records frame timing and related statistics of every Metal layer all the time (`man metalperftrace`). With
`TOTK_STATE_REPORTING=1`, Tears of the Kingdom and Breath of the Wild also tell the system what they are doing, through
Apple's StateReporting framework, so Instruments and `metalperftrace` can split that recording by game state.
`games/totk/scripts/metal_perf_report.py` turns a trace into a report. Both are diagnostics: off by default; nothing is
recorded for the game, collected or sent anywhere unless you ask; and the picture, the game and its pacing are the same
with them on.

[中文说明在下面](#macos-27-上的-metal-性能诊断)

## Quick start

```sh
# 1. Play with state reports (and, optionally, per-frame data for percentiles)
open --env TOTK_STATE_REPORTING=1 --env METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1 "Tears of the Kingdom.app"
# 2. Quit the game, then: the last 15 minutes into a report folder
python3 games/totk/scripts/metal_perf_report.py collect --last 15m --out ~/Desktop/totk-perf \
    --app "Tears of the Kingdom.app" --note route=lookout-landing
# or a trace you collected before
python3 games/totk/scripts/metal_perf_report.py report MetalPerfTrace_....atrc --out ~/Desktop/totk-perf --app "Tears of the Kingdom.app"
```

`flag state_reporting 1` / `0` in the input file (`TOTK_INPUT_FILE`) switches the reports on and off while playing.
The folder then holds:

| File | What it is |
|---|---|
| `trace.atrc` | Apple's raw trace. Instruments opens it (`xcrun xctrace import --input trace.atrc` makes a `.trace`; the states are in the State Reporting lane). It holds the Metal statistics, names and process IDs of **every** process that drew in the time range, not only the game: look before you share it. |
| `performance.json` | The game's numbers only: the whole run, visible play, every state change and the numbers per state, a one-second timeline. |
| `performance.md` | The same, to read. |
| `metadata.json` | Per run: game and version, source commit, runtime UUID, runtime and launcher SHA-256 (when `--app` is the build that ran), the Mac and macOS the game reported, the measurement switches it ran with (values without paths); your `--note`s, the trace's SHA-256, the Mac that made the report. No user paths, keys, saves or device identifiers. |

Exit status: 0 report written; 2 usage; 3 `metalperftrace` missing (macOS 26) or failed; 4 no `totk-native` process in
the trace. A failed step is written into the report rather than skipped.

Collect a range that starts before the game does: a trace holds the state changes inside its range, plus the current
states, which the system repeats about every 90 seconds (the report marks those "began before the trace"). Quit the game
before collecting, so the last state has an end.

## The states

Two domains. Only what the port knows for certain is reported: there is no title, menu, world or loading state, because
nothing in the port can tell them apart reliably.

**`org.macnativegames.Lifecycle`**

| Label | When |
|---|---|
| `starting` | the window opened |
| `shaderWarmup (archive reused)` / `(archive rebuilt)` | the start-up shader and pipeline warm-up began and the start-up screen still shows; the pipeline archive had this Mac's pipelines, or not (a first start, a new macOS or GPU driver) |
| `focused` | the game picture shows and the window has the keyboard |
| `unfocused` | the picture shows, another app has the keyboard. The Mac never pauses the game, so it keeps running; `TOTK_ASSUME_FOCUS=1` test runs are `unfocused` throughout |
| `settings` | the settings panel (key 1) is open: the game runs, but gets no input |
| `occluded` | the window is covered or on another Space: macOS stops presenting it, so these seconds are no measure of the game |
| `closing` | the window is closing |
| `reportingOff` | `flag state_reporting 0` switched the reports off (both domains) |

Stable metadata: `game` (`totk`, `botw`), `gameVersion`, `sourceCommit` (when the app records it), `runtimeUUID` (the game
binary's LC_UUID, matched against `--app`), `shaderChain`, `hostModel`, `hostChip`, `hostOS`, and `settings` (the
switches the run was started with that change what is measured: frame rate, MetalFX, TAA, resolution, aspect ratio,
pacing and present test switches, the Metal HUD and signposts; values without paths, `defaults` when none is set).

**`org.macnativegames.Graphics`**: the configuration the renderer runs with, in one label, for example
`1920x1080 | cap 120/game interval | window@120Hz` or
`2560x1440 | MetalFX+output | TAA | FI 50+50 | held 50 | fullscreen@120Hz`:

- the game picture's size (the crop of its last frame), and the aspect ratio when it is not 16:9;
- `MetalFX` (MetalFX upscaling on), `MetalFX+output` (also the window output stage), `TAA` (edge smoothing),
  `FI 30+30` / `40+40` / `50+50` / `max` (frame interpolation, as the renderer took it over);
- the frame-rate cap: `cap N` (uncapped clock), `cap N/game interval` (the uncapped clock, divided by the game's own
  present interval: Tears of the Kingdom's frame-rate controller picks 1, 2 or 3 refreshes per frame), `lock N` (the
  settings' frame lock), `held N` (interpolation holds the game), `movie 30` (Breath of the Wild's pre-rendered movies),
  `game interval` (`TOTK_UNCAPPED=0`), `display interval` and `no cap` (`DM_PRESENT_*` test switches); `own clock` when a
  held rate does not divide the display's and the game's own clock paces it;
- `present thread` when the window is not driven by the display link, `(display link stalled)` after macOS stopped
  calling it back (a covered window) until the port tries it again;
- `window` or `fullscreen`, the refresh rate of the screen the window is on, and `(paced N Hz)` when the pacing still uses
  another display's rate (it reads the rate once, at the first frame).

The stable metadata repeats these as fields, plus the window's drawable size (`view`): a window resize shows in
Instruments as a new interval of the same state. Every state has its own label because `metalperftrace` tells states
apart by label only. A change is reported at once; further changes within half a second are merged until the
configuration has held for half a second (at most 2 s), so dragging a window larger is two reports, not hundreds.

Whether a stage actually ran depends on what is on screen (a menu has no depth to smooth, a scene as large as the canvas
needs no upscale), so it is not a state. Every state change carries the game's counters (volatile metadata):
`gameFrames` (the game's presents, as the FPS box counts them), `generatedFrames` (interpolated frames handed to the
screen), `mergedFrames` (game frames the display link merged away), `upscaledFrames` and `antiAliasedFrames` (game frames
a MetalFX upscale or edge smoothing ran on), `paceTicks1` / `2` / `3` (game frames paced at 1, 2, 3 or more display
refreshes), `shaderTranslations`, `librariesCreated`, `pipelinesCreated`, `creationMs` (shaders and Metal objects made
while playing), `footprintMiB`, `metalAllocatedMiB`, `scene` (the internal resolution at that moment), and `sampledAt`.

## Reading the report

- Three frame rates, never added or subtracted. **Output FPS**: every drawable the window showed (the trace); with frame
  interpolation each generated frame is a drawable of its own. **Game FPS**: the game's own presents between two counter
  readings. **Generated / merged**: interpolated frames handed to the screen, game frames merged away. With interpolation
  on, the report never calls the output rate a game frame rate.
- **Visible play** and the per-state **visible** columns take only whole one-second buckets while the game picture
  showed (focused, unfocused, settings): no start-up, warm-up, covered window or closing, and no second that straddles a
  change. metalperftrace's own per-state aggregate is shown next to it and covers every second of a state, whatever the
  other domain said.
- metalperftrace keeps one-second summaries (count, minimum, average, maximum, standard deviation) of on-glass frame
  time, CPU begin-to-present, GPU time, next-drawable wait and present lateness, and the process's memory and CPU time.
  **p50 / p90 / p95 / p99 need per-frame data**: start the game with `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1` (or run
  `metalperftrace setup --enable per-frame-metrics --pid <pid>`). Then the report gives nearest-rank percentiles over all
  drawables, per run and per state. They describe the window's drawables (interpolated ones included), not the game's own
  frames. Without per-frame data the report says they are not available; it never estimates them.
- metalperftrace's per-drawable data has a MetalFX frame interpolator field of its own. It reads false for this port's
  interpolated frames (measured on macOS 27.2), so the report never uses it: interpolated frames are counted from the
  game's `generatedFrames`.
- Metal's shader compiler statistics need `METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS=1`; the game's own counters
  above are always there.
- metalperftrace aggregates whole seconds, so states shorter than a second are listed as not aggregated; it also skips
  states in which the game window showed no drawable (the start-up screen during the warm-up). It gives no reason, so
  the report says only what the trace shows.
- On the Mac the GPU keep-alive (`TOTK_GPU_KEEPALIVE`) runs short kernels on its own queue between the game's command
  buffers; the trace's GPU time is the game layer's, the system's GPU utilisation includes the keep-alive.
- The source commit is what the app recorded at build time (uncommitted changes do not show); the runtime UUID and
  SHA-256 identify the build.

## Platforms

- macOS 27: StateReporting is opened at run time (`dlopen`, no `-framework`), so the app builds with any SDK.
- macOS 26: an app built there has no StateReporting to open; with `TOTK_STATE_REPORTING=1` the log says so once and the
  game runs as without it. `metalperftrace` does not exist there either: the report command stops with status 3 and says
  why. (An app built with the macOS 27 SDK needs macOS 27 anyway.)
- iPhone, iPad, Vision Pro: not built in (`TARGET_OS_OSX` only); nothing changes. `flag state_reporting 1` logs that it is
  Mac only.

## Overhead

Off (the default), each hook reads one atomic integer: no queue, no framework, no Objective-C. On, the game thread
compares a snapshot of 17 integers once per frame; every StateReporting call runs on a queue of its own; no render pass,
GPU wait or lock on the frame path is added (the snapshot is published under a mutex only when it changed). Timed on its
own on an M5 Pro, the game thread's hook costs about 2 ns a frame with the reports on, also with a change reported every
10,000 frames. In the games, with the repository's method (one binary, the switch toggled in alternating
30-second windows), the difference stayed within the window-to-window spread; the numbers are in the pull request.

---

# macOS 27 上的 Metal 性能诊断

macOS 27 会一直记录每个 Metal 图层的帧时间等统计（`man metalperftrace`）。设了 `TOTK_STATE_REPORTING=1` 后，王国之泪和旷野之息还会通过 Apple 的 StateReporting 框架告诉系统自己正在做什么，Instruments 和 `metalperftrace` 就能按游戏状态拆分这份记录。`games/totk/scripts/metal_perf_report.py` 把追踪整理成报告。两者都是诊断功能：默认关闭；你不主动运行，就不会为游戏额外记录、收集或发送任何东西；打开后画面、游戏逻辑和帧节奏都不变。

## 用法

```sh
# 1. 带状态上报游玩（可选：逐帧数据，用于百分位）
open --env TOTK_STATE_REPORTING=1 --env METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1 "Tears of the Kingdom.app"
# 2. 退出游戏后，把最近 15 分钟整理成报告
python3 games/totk/scripts/metal_perf_report.py collect --last 15m --out ~/Desktop/totk-perf \
    --app "Tears of the Kingdom.app" --note route=lookout-landing
# 或者处理之前收集的追踪
python3 games/totk/scripts/metal_perf_report.py report MetalPerfTrace_....atrc --out ~/Desktop/totk-perf --app "Tears of the Kingdom.app"
```

输入文件（`TOTK_INPUT_FILE`）里的 `flag state_reporting 1` / `0` 可以在游戏中开、关上报。报告目录里有：`trace.atrc`（Apple 原始追踪，Instruments 能打开；里面有这段时间内**所有**用 Metal 绘制的进程的统计、进程名和 PID，分享前先检查）、`performance.json`（只含游戏：整局、可见游玩、每次状态变化、各状态的数据和每秒时间线）、`performance.md`（同样内容的可读版）、`metadata.json`（每局的游戏和版本、源码提交、运行时 UUID；`--app` 正是运行的那个构建时，还有运行时和启动器的 SHA-256；游戏报告的 Mac 型号和系统、启动时影响测量的开关；你的 `--note`、追踪的 SHA-256、生成报告的 Mac；不含用户路径、密钥、存档或设备标识）。退出码：0 成功；2 参数错误；3 没有 `metalperftrace`（macOS 26）或它失败；4 追踪里没有 `totk-native`。失败的步骤会写进报告，不会被略过。

采集范围要从游戏启动之前开始：追踪里只有范围内发生的状态变化，加上系统大约每 90 秒重报一次的当前状态（报告里标为“开始于追踪之前”）。采集前先退出游戏，最后一个状态才有结束点。

## 状态

两个域。只报告移植层确知的事：标题、菜单、世界、读取画面都不报告，因为移植层没有可靠的办法区分它们。

- **`org.macnativegames.Lifecycle`**：`starting`（窗口已打开）、`shaderWarmup (archive reused)` / `(archive rebuilt)`（启动预热开始、启动画面还在；管线存档里有没有这台 Mac 的管线）、`focused`（画面已显示、窗口有键盘焦点）、`unfocused`（画面已显示、焦点在别的应用；Mac 版从不暂停游戏，游戏照常运行；`TOTK_ASSUME_FOCUS=1` 的测试局全程是这个状态）、`settings`（设置面板打开，游戏照跑但收不到输入）、`occluded`（窗口被挡住或在别的空间，macOS 停止出图，这段数据不代表游戏性能）、`closing`（窗口关闭中）、`reportingOff`（用 `flag state_reporting 0` 关掉了上报）。stable metadata 里有游戏、版本、源码提交、运行时 UUID、翻译链、Mac 型号、芯片、系统，以及启动时影响测量的开关（帧率、MetalFX、TAA、分辨率、画面比例、节奏和出图测试开关、Metal HUD 和 signpost；不含路径，都没设时为 `defaults`）。
- **`org.macnativegames.Graphics`**：渲染器实际使用的配置写成一个标签，例如 `2560x1440 | MetalFX+output | TAA | FI 50+50 | held 50 | fullscreen@120Hz`。依次是：游戏画面尺寸（不是 16:9 时还有画面比例）；MetalFX 放大（`+output` 表示窗口输出级也开着）；TAA；补帧模式；帧率上限及来源（`cap N` 不限帧时钟；`cap N/game interval` 不限帧时钟再除以游戏自己的出图间隔，王国之泪的帧率控制器会在 1、2、3 个刷新之间切换；`lock N` 锁帧；`held N` 补帧锁定；`movie 30` 旷野之息的预渲染影片；`game interval`、`display interval`、`no cap` 对应测试开关；锁定的帧率除不尽刷新率、改用游戏自己的时钟时加 `own clock`）；不走显示节拍时写 `present thread`，显示节拍停过（窗口被挡）时加 `(display link stalled)`；窗口或全屏、所在屏幕的刷新率；节奏还在用别的屏幕刷新率时写 `(paced N Hz)`。stable metadata 里有对应字段，另有 drawable 尺寸（`view`），所以改窗口大小在 Instruments 里显示为同一状态的新区间。`metalperftrace` 只按标签区分状态，所以每个状态都有自己的标签。变化会立即上报；半秒内的后续变化合并，等配置稳定半秒（最多 2 秒）再报，拖动窗口只有两次上报。

各阶段实际有没有运行，取决于画面内容（菜单没有深度可平滑，场景和画布一样大就不需要放大），所以不作为状态，而是放进计数。每次状态变化都带上游戏自己的计数（volatile metadata）：`gameFrames`（游戏出图次数，与 FPS 框同一口径）、`generatedFrames`（交给屏幕的补帧）、`mergedFrames`（被显示节拍合并掉的游戏帧）、`upscaledFrames` 和 `antiAliasedFrames`（跑了 MetalFX 放大或边缘平滑的游戏帧）、`paceTicks1/2/3`（按 1、2、3 个及以上刷新出图的游戏帧）、`shaderTranslations`、`librariesCreated`、`pipelinesCreated`、`creationMs`、`footprintMiB`、`metalAllocatedMiB`、`scene`（当时的内部分辨率），以及采样时间 `sampledAt`。

## 怎么读报告

- 三种帧率，互相不加不减：**输出 FPS** 是窗口实际显示的 drawable 数（来自追踪；开补帧时每个生成帧都是单独的 drawable）；**游戏 FPS** 是两次计数之间游戏自己的出图次数；**生成 / 合并** 是交给屏幕的补帧数、被合并掉的游戏帧数。开补帧时，报告不会把输出帧率说成游戏帧率。
- **可见游玩**和各状态的 **visible** 列只用游戏画面显示期间（focused、unfocused、settings）完整落在其中的每秒数据：不含启动、预热、窗口被挡、关闭，也不含跨越状态变化的那一秒。旁边同时给出 `metalperftrace` 自己的按状态聚合，它覆盖该状态的每一秒，不管另一个域当时是什么状态。
- `metalperftrace` 只保留每秒汇总（次数、最小、平均、最大、标准差）。**p50 / p90 / p95 / p99 需要逐帧数据**：用 `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1` 启动游戏，或在游戏运行时执行 `metalperftrace setup --enable per-frame-metrics --pid <pid>`。有了逐帧数据，报告按所有 drawable 用最近秩法给出整局和各状态的百分位；它们描述的是窗口的 drawable（含补帧），不是游戏自己的帧。没有逐帧数据时明确写“不可用”，不从每秒汇总推算。
- `metalperftrace` 的逐 drawable 数据里有自己的 MetalFX 补帧字段，但对本移植的补帧它是 false（macOS 27.2 实测），所以报告不用它：补帧数一律取游戏自己的 `generatedFrames`。
- Metal 自己的着色器编译统计需要 `METAL_PERFORMANCE_SHADER_COMPILER_STATS_SIGNPOSTS=1`；上面那些游戏计数一直都有。
- 聚合按整秒进行，不足一秒的状态会标为“未聚合”；窗口没出过 drawable 的状态（预热期间的启动画面）也会被跳过。`metalperftrace` 不给原因，报告只写追踪里看得到的事实。
- 源码提交是 App 打包时记录的（看不出未提交的改动）；能确定构建身份的是运行时 UUID 和 SHA-256。

## 平台

macOS 27 运行时用 `dlopen` 打开 StateReporting（不加 `-framework`），任何 SDK 都能构建。macOS 26 上构建的 App 没有这个框架可打开：设了开关时日志里说明一次，游戏照常运行；那里也没有 `metalperftrace`，报告命令以退出码 3 结束并说明原因（用 macOS 27 SDK 构建的 App 本来就只能在 macOS 27 上运行）。iPhone、iPad、Vision Pro 上不编入这部分代码（只在 `TARGET_OS_OSX` 下），行为不变；`flag state_reporting 1` 会在日志里说明只支持 Mac。

## 开销

关闭（默认）时，每个挂点只读一个原子整数，不建队列、不加载框架、不调用 Objective-C。打开时，游戏线程每帧比较一份 17 个整数的快照；所有 StateReporting 调用都在单独的队列上；帧路径上不增加渲染通道、GPU 等待或锁（快照只在变化时才在互斥锁下发布）。在 M5 Pro 上单独计时，打开上报时游戏线程的挂点每帧约 2 ns（每 1 万帧上报一次变化时也是如此）。在游戏里按仓库的方法实测（同一个二进制，以 30 秒窗口交替切换开关），差异落在窗口之间的自然波动以内，数据见 PR。
