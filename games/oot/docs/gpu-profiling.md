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

`commit`、`fence` 和 GPU feedback 行记录提交捕获的所属帧，异步回调不再读取
可能已经前进的全局帧号。旧版本的 GPU 行记录的是回调时当前帧；分析历史数据
仍须用 `commit` 编号和 `fence` 关联。CPU 回放时间包含
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

每次 Metal 4 提交还标记为 `F<帧> CB<唯一序号> <用途> fence<栅栏> tl<时间线提交号>`。
包含中途/最终提交、上传、出图、插帧和独立诊断提交；出图用排队项目的帧号。
同一个 Metal 对象可以被反复提交，裸对象 ID 不能唯一标识一次执行。
使用 CB 序号连接 CPU 提交与 GPU 事件，使用非零 tl 编号连接自身时间线，
先检查编号唯一性、覆盖和 GPU/CPU 正负时差。驱动映射表中的 `commit-id`
不能未经核对便当作原编码器的所属提交；未标注/无法连接的工作保留未知。

同一个编码器的多个标签可能在 Instruments 中合并显示，这不是多次独立
计时。Instruments 自身的显示帧号会横跨两帧
游戏工作，不能用它代替游戏帧号。未确定身份的工作单独保留。

标准 Metal System Trace 默认 Counter Set 为 None，Shader Timeline 关闭。
需要硬件瓶颈和着色器采样时，在 Metal Application 选 Performance Limiters、
开启 Enable Shader Timeline，将模板保存到项目外，再通过 `xctrace --template`
使用。录制后先导出 TOC，核对 counter/profile 配置及实际行数；
`gpu-shader-profiler-sample` 为空不能解释成着色器成本为零。硬件计数和 PC 采样
仍是诊断工作负载，不拿它的帧率当正常 FPS，不把函数名相同的不同变体混为一条。

## 描述符准备原因

设置 `OOT_DESCRIPTOR_PROFILE_FILE=<外部 TSV>`，用 `flag descriptor_profile 1`
开始、`flag descriptor_profile 0` 结束并刷新。P 行记录池身份、前后世代、
描述符修订、CPU 脏页/提交/内存世代、全扫原因、检查数、重试和主机耗时；
S 行记录实际存储对象、布局、持有序号与释放原因；C 行是该窗口计数。
最多记录 65,536 行，`rows_dropped` 非零时不可宣称完整覆盖。
全扫原因位：1 替换池，2 描述符日志溢出，4 序号日志溢出，8 池内存生命周期，
16 遇到全局序号变化。诊断开销不用于正常性能对照。

## 实体手柄输入链路

`OOT_INPUT_TRACE=1` 或运行中 `flag input_trace 1` 启用有界分层日志，
`flag input_trace 0` 关闭。每次启用最多 16,384 行，默认关闭，不改变输入。
依次观察 callback（系统事件/设备/物理按键）、focus/blocked（焦点策略）、
mapped（映射输出）、read（含 UI 捕获状态）、guest（实际 Npad ID、style、
允许/屏蔽状态、采样号、属性、按键和摇杆）。每行有序号和同一主机时钟。
自动片头按键不会生成 callback，不能作为实体手柄验收。连接、电量和焦点
正常也不能证明实体按键已到；没有真实事件时保留未验收状态。

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
`TOTK_IGNORE_USER_INPUT=0`只是不清除输入；`TOTK_ASSUME_FOCUS=1`仍不向macOS申请真实前台焦点，
后台GameController选项默认关闭时，连接成功也可能收不到按键。需要随时交接的采集使用真实前台启动，
分别核对新局`window gained focus`、设备连接、`[input] user input (controller)`。
用户在对话里要求操作也算交接，不能等一个可能被后台策略拦掉的事件才停脚本。
输入闲置看门狗的activity只跟踪真实输入，不能用不断增长的渲染日志冒充活动。

### 着色器记录必须验证是否改变原始画面

2026-11-06 的外部实验在描述符表尾追加 GPU 位图地址，并在 MSL 的池索引处记录访问。
985 份源码检查、178 份改写后的 Metal 编译、77 项 GPU 小用例检查都通过；
小用例覆盖索引边界、越界回落、同提交内换表、原输出一致和采集退役，负对照也被检出。
但真实游戏截图出现大面积异常亮斑。保持同一构建和源码编译路径、仅关闭 MSL 插入后画面正常。

因此该实验被撤下，代码只保留为源码目录外的失败补丁。37 份 GPU 完成记录中的访问数量
只代表修改后的运行，不能用来证明原游戏只需要这些纹理，也不能据此删减资源或宣称画质相同。
API 成功和 GPU 完成状态独立于画面一致性。再次采用这类记录前，需要真实片元着色器的
完整输入与输出对照；简单计算核的输出相同不足以覆盖片元阶段。

后续优先从编译器 IR 导出句柄的确切来源，采集原着色器实际绑定的输入缓冲。
涉及 GPU 前序写入时，在正确 GPU 时序上复制、完成后读回；不以宿主旧副本替代。
证据位于外部 `codex/oot-native-20261105/performance-90fps/descriptor-usage/`，
`review.json` 明确记录了这次画面一致性失败和使用限制。

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

### v13 范围缓存后的记录（2026-11-06）

外部 `performance-90fps/range-critical-path/` 保存新的 Metal System Trace、Time Profiler、
逐派发与时间线。导出共 97,168 条 GPU 区间，其中游戏 93,485 条；显式标签覆盖
295 个游戏帧的部分提交，没有歧义的标签连接。该覆盖不包含所有未标记提交，不能
把已归因区间的并集当成完整帧 GPU 忙时。主网格通道的 Vertex 中位约 2.18ms，
80 次前置计算约 3.16ms，四次后处理计算约 1.82ms。470 次逐派发、五帧的
线程组、间接网格哨兵和非负时间检查均通过。Precise 数据仍只作诊断。

提交线程 Time Profiler 样本中，`descriptor_pool_gpu` 的调用链占约 59%；
这是下一处宿主准备热点，不能直接当作可节省的整帧比例。既有 `pool_validate_once`
候选的真实同局对照未证明稳定收益，继续默认关闭；不能只凭热点或 GPU 小用例通过
就开启它。新的正常范围缓存默认配置已在同一 App、活动存档和树屋验证，约 58fps，
仍未达到稳定实际 90fps。

本次还确认三个 Metal command-buffer 对象分别在 CPU 导出中重复提交 46、55、56 次。
只按对象 ID 合并全部 GPU 区间，再取最后一次 CPU 提交，会产生约负七秒的错误时差。
外部 `submission-gap-analysis.json` 因而只是未验收的探索结果；不能拿其中的空档比例
声称 CPU 导致 GPU 等待。必须使用提交实例身份，或通过明确的编码器标签、提交/栅栏
连接；同样不能将 callback 时的游戏帧号当作该 GPU 工作所属帧。
