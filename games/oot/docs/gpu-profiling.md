# GPU 帧归因

诊断默认关闭。使用现有共享游戏槽、桌面 App 和活动存档；采集结束后正常
`close`。捕获数据写到源码目录外。不得把带 GPU 时间戳、抓图、资源转储或
Instruments 的区间拿来报告正常 FPS。

## 连续帧时间线

设置 `TOTK_TIMELINE=<外部文件>:<首帧>:<帧数>`，同时设置
`TOTK_TIMELINE_GPU_ONLY=1`。后者只过滤客体线程唤醒、阻塞和虚拟核等待事件，
保留 NVN 提交、回放、GPU 完成、fence 等待、通道、出图和回读事件。
不设置时保留原来的完整线程诊断。

文件在指定窗口结束 **再过 30 帧** 后写出。退出前必须观察到文件和
`[timeline] 写出` 日志；提前退出会丢失尚未写出的窗口。事件容量为 1,048,576，
达到容量的结果不能当作完整采集。

GPU feedback 行的 `frame` 是回调发生时的当前帧。用 `commit` 的编号和
`fence` 对应 GPU 区间与等待，不能按回调行的帧号关联。CPU 回放时间包含
阻塞，GPU command-buffer 时间包含调度跨度；这些列不能直接相加。

## 编码器和实际计算派发

设置 `TOTK_PASS_GPU_FILE=<外部 TSV>`，运行中发送 `flag pass_gpu 5`。
原来的前 12 列保持顺序，追加身份和时间：

```
frame encoder kind ms draws width height format colors depth loads first_shader_key
fence pass_before start_ns end_ns dispatch_count
```

上面为方便阅读折行，实际每条是一行 17 列。`encoder` 是提交内序号，必须
与 `fence` 一起使用；`pass_before` 是编码器打开前的通道计数。`first_shader_key`
保存完整变体键，但一个计算编码器可能有许多着色器，整段时间不能归给首个键。
`[gpu-work]` 日志记录其中每次派发的完整键、提交、编码器、网格、线程组和
间接参数地址。日志中的间接网格不是 GPU 参数真值。

需要逐次派发的 GPU 时间和实际间接网格时，再设置
`OOT_DISPATCH_GPU_FILE=<外部 TSV>`，仍用 `pass_gpu` 指定采样窗口：

```
frame fence encoder dispatch key ms grid_x grid_y grid_z tg_x tg_y tg_z
indirect_address start_ticks end_ticks grid_sentinel
```

每行 16 列。`dispatch` 是提交内从零开始的计数；线程组来自实际 `uint16_t[3]`
反射。间接网格在消费前由 GPU 复制，完成后才读回，哨兵为 `0x47524944`。
直接网格来自实际调用参数。单提交最多采样 512 次派发，分析时核对采样数量
与 `[gpu-work]`，不能默认为全覆盖。

计时采用 Metal 4 compute encoder 的 `Precise` 时间戳，会串行化诊断工作；
只用于识别具体内核成本。它不提供 shader 内部指令占比，也不证明对应的
时间全部位于整帧关键路径。核对非负时间、文件列数、shader 变体、线程组、
哨兵和实际派发覆盖后，才使用结果。

## Instruments

`DM_NVN_ENC_LABEL=1` 为计算编码器增加游戏帧号、提交和完整 shader 键，
计算管线也有 shader 标签。录制 Metal System Trace 后，至少验证导出的
`metal-gpu-intervals` 有真实游戏进程数据；仅有 `.trace` 或退出码 0 不够。

同一个编码器的多个标签可能在 Instruments 中合并显示，这不是多次独立
计时。按完整 command-buffer / encoder ID 关联 CPU 和 GPU，游戏帧身份
从明确的 `F… s… cs…` 标签取得。Instruments 自身的显示帧号会横跨两帧
游戏工作，不能用它代替游戏帧号。未确定身份的工作单独保留。

## 纹理内存和生命周期

遇到 GPU `Insufficient Memory` 时，可设置以下默认关闭的诊断，输出放在源码目录外：

- `OOT_STORAGE_MEMORY_FILE=<TSV>`：在已有低频内存报告处输出共享存储的确切布局和持有序号。
- `OOT_TEXTURE_LIFETIME_FILE=<TSV>`：记录所有 NVN 纹理对象的初始化、再次初始化、Finalize，
  以及宿主进入释放函数的事件。不启用慢速纹理查找诊断，不依赖每次启动都会变化的存储地址。

存储文件每行 18 列：

```
frame fence object storage guest_bytes width height depth levels target samples
metal_format allocated_bytes holders idle_fence detached serials_csv finalized_flags_csv
```

两个 CSV 按位置对应。`holders` 说明宿主序号表仍持有资源，不独立证明 NVN 对象仍活着。
分配字节属于这个 Metal 基础纹理；不要按每个序号再加一次。同地址不同布局也不能直接判为垃圾。

生命周期文件每行 17 列：

```
sequence uptime_ns cpu_frame event object serial storage guest_bytes width height
depth levels target nvn_format texture_flags pool pool_flags
```

`pool_flags` 仅在 `initialize` 时读取；异步释放时不解引用可能已经销毁的池对象。
存储采样与事件来自不同线程，同一 CPU 帧内不能仅按帧号推断先后。
`host_release` 标记进入释放函数，不能解释为该时刻 GPU 已完成或物理存储已释放。
低频内存报告会刷新事件文件；异常退出后的末尾不完整行不进入分析。

`DM_NVN_LIFELOG_MEM=<guest address>` 可在已有尺寸生命周期诊断中改为跟踪指定地址的所有布局；
跨启动复用旧地址前必须重新确认。未匹配到 Initialize / Finalize 的日志不能用于判断目标的生命周期。

性能采集保留真实手柄输入。用户接管后作废测量窗口并取消脚本操作和定时关闭。

## 本机验证（2026-11-06）

证据根目录为外部 `codex/oot-native-20261105/performance-60fps/`：

- `gpu-work-20261106-dispatch/`：940 次派发、10 帧，每帧 94 次；全部线程组
  与实际缓存反射一致，哨兵和时间有效。Instruments 导出 70,252 条 GPU 区间，
  其中游戏进程 60,384 条；明确标签覆盖 190 个游戏帧的部分 command buffer。
- `gpu-work-20261106-replay-input/`：272,581 个时间线事件，829 个连续
  present 间隔；窗口尾部尚未返回的 7 条 feedback 不进入完整性结论。
- `compute-hotspot-replay/`：热点 `fa3033d0…` 的两帧完整 GPU 输入和输出
  离线重放。280 次输出稳定；原生半字写入候选与原实现、真实游戏输出逐字节
  一致，每张 2048×1152。候选仅省约 0.1 ms，留在外部实验目录，未接入交付。

本轮建立测量和归因，没有新的整机 FPS 提升结论，尚未达到 60fps。
