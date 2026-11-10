# 浮点原子加法（RED.F32）：过曝 / 白雾的根因与原生实现

2026-11-07。翻译链 `oot-native-v15-native-float-atomics`，翻译器补丁 `0028-float-atomic-add.patch`、`0029-native-float-atomic-add.patch`。

## 症状与根因

村庄、树屋等开阔场景整帧发白、暖色偏黄、远处一层白纱（截图平均亮度 199/255，近白像素 15%）。

翻译器（vendored Ryujinx 1.3.2）的 `EmitAtomicOp` 对 Ampere 的 `RED.F32.FTZ.RN`（`AtomSize.F32FtzRn`，操作码 0x98e、类型位 73..76 = 3）只记一行 `Invalid reduction type` 日志，整条指令丢掉。OOT 语料 42,845 个着色器里 34 个用它（416 条）：22 个 Foliage / TerrainGround 片元着色器（各 18 条）和 12 个 AglShader 计算着色器——agl 的场景分析把亮度累加进存储缓冲，CPU 读回算曝光；累加始终为 0，曝光按最暗场景提亮。nxnative 对同一游戏有同样结论。

`totk_xlate` 的 `Accessor` 原来没有实现 `IGpuAccessor.Log`，这类丢指令的日志哪里都看不到；现在进 notes（种子 `_b.json`），`translator:` 前缀。

## 为什么不能用 CAS 循环

Metal 的存储缓冲在翻译器里声明成 `uint[]`，合法 SPIR-V 无法从 uint 指针发 `OpAtomicFAddEXT`。第一版用 compare-and-swap 循环：

- v1 把循环外读到的旧值一直用来比较，优化器丢掉了循环内的重赋值，一次争用就永不终止 → GPU 僵死 → WindowServer 看门狗 → 内核 panic（15:49）。
- v2 每次迭代重读（`castest.m` 隔离测试：1,024 / 65,536 / 1M / 4M 线程争用同一个字均终止且精确），但高争用代价约 3.4 µs 一次。片元着色器对整幅画面逐像素各做 18 次累加，一帧要跑几十秒，与挂起无异（19:45 由 GPU 完成看门狗终止）。

## 原生实现（v15）

- 新 IR 指令 `AtomicAddF32`（`Instruction.cs`、`InstructionInfo.cs`、`EmitterContextInsts.cs`），`EmitAtomicOp` 对 F32 发它。
- `ResourceManager.TryGetStorageBufferFloatBinding`：为同一个存储槽再声明一个 `float[]` 视图绑定 `{stage}_sf{slot}`（同一块内存绑两次）；`GlobalToStorage` 的存储路径对视图发 `AtomicAddF32`，SPIR-V 后端出 `OpAtomicFAddEXT` 并声明 `AtomicFloat32AddEXT` + `SPV_EXT_shader_atomic_float_add`（`ShaderProperties.UsesFloatAtomicAdd`）。原始地址路径（GmemMap 池表）没有 float 视图，保留每次重读的 CAS 回退 `GenerateCasAddF32`。
- 两处必须同步的清单：`Optimizer.HasSideEffects`、`AstOptimizer.HasSideEffects` 按枚举列原子指令，不列新指令就被当死代码删；`GlobalToStorage.CanUseInlineStorageOp` 要排除它，否则内联改写直接落到 uint 绑定。
- spirv-cross 对合法 SPIR-V 输出 `atomic_fetch_add_explicit((device atomic_float*)&cp_sf3._m0[i], v, memory_order_relaxed)`；`Service.cs MslVersion` 见到操作码 6035 选 MSL 3.1；反射把 `_sf<N>` 当作槽 N 的 `sbuf`（`float_view=true`），运行时按槽号独立取址（`nvn_mtl.m` 的 SBUFP 路径），不用改。
- `spirv-val --target-env vulkan1.2` 通过。隔离测试 `castest_native.m`：4,194,304 次争用加法 2.5 ms、精确。

## 实机（m05，同 App、同活动档、原画质）

截图平均亮度 199 → 125，近白像素 15.1% → 0.0%，RGB 211/200/162 → 133/126/95；38.4 fps（修前同条件约 38–39），带浮点原子的计算派发 0.44 → 0.47 ms；正常退出，无 GPU 报错、无看门狗触发。证据 `MacNativeGames-artifacts/claude/oot-fps90/m05`。

## 同时加入的保护

- `nvn_mtl.m` GPU 完成看门狗（`TOTK_GPU_HANG_S`，默认 10 s，0 关）：命令缓冲提交后超时没有任何完成就结束进程，让内核收回 GPU 上下文；命令缓冲报 Timeout / Hang / InnocentVictim 第二次也终止。GPU 僵死时 Metal 不报任何错，只有“提交了却不完成”这一个信号。
- `scripts/dev/game_slot.py`：持有者进程全部已死、僵尸或卡在退出态（`ps` 状态 E）时自动轮换锁文件（`rotate_stuck_lock`，history `rotate-lock`）。被终止的游戏卡在 E 状态时握着 flock，否则只能重启。
- 构建脚本：翻译并行数 4 → 核数−2；种子清单 25 万文件与反射 JSON 校验、库哈希改成线程池。

## 规则

改变 GPU 执行结构的翻译器改动（循环、原子、同步），先在隔离测试里按游戏实际规模（像素数 × 次数）证明耗时有上限，再进真实游戏。

## Apple M5：编译器编不了的那一个着色器（#2070，2026-11-10）

- 现象（报告者实测）：Apple M5（`applegpu_g17g`）、macOS 26.6.2，`MTLCompilerService` 把 AIR 编成 GPU 代码时崩溃（AGX3 `cannot select … llvm.agx3.disable.smashing.float`），只要 `device atomic_float` 上做 `atomic_fetch_add` / `atomic_exchange`；load / store / compare-exchange 和整数 fetch_add 正常。OOT 34 个用浮点原子的着色器里，M5 上只 `278f4837…_b` 一个计算着色器编不过（形状相关）；M4 / macOS 26.5.2 都能编。
- 那个着色器：每个线程往 `cp_sf0._m0[0]` 加 0 或 1（统计像素数），16×16 一组。
- 做法（运行时，`nvn_float_rmw.h` + `nvn_mtl.m` 的 `frmw_*`）：只在原生管线因编译服务中断建不出来时，把这个着色器的 MSL 里每个 `device` 浮点加法改写成“SIMD 组内同地址的车道先 `simd_sum`，一条车道用 uint compare-exchange（失败时重读）写一次”，每条车道拿回的旧值 = 组的旧值 + 排在它前面的车道之和（合法的原子顺序）。不需要屏障，任何位置都合法。改过的着色器按 GPU 记进 `pipelines/float-rmw-lowered.txt`，打包时由 `build_app.mac_pipeline_files` 写成 App 的 `pipelines-<arch>-lowered.txt`，之后不再先试原生写法（不再弄崩编译服务）。`OOT_FLOAT_RMW_LOWER=1` 强制改写所有带浮点加法的计算着色器（测试用）。
- M4 隔离测试（1536×864 线程全加到同一个字，结果都精确）：原生 0.11 ms；改写后 26–29 ms；线程组级合并 3–5 ms（要往 main0 加线程组内存和参数，没做）；逐线程 CAS 约 9 s（不能用）。所以只改编不过的那一个，其余保持原生。
- 已验证（M4）：改写文本测试；改写后的内核在 GPU 上总和与旧值都精确（`tests/float_rmw_gpu_test.m`）；种子里 12 个带浮点加法的计算着色器改写后全部编得过、建得出管线；运行时 `pipeline-archive` 强制改写建出 `278f4837…` 那条管线，0 失败。没验证：M5 上真的编过、实际耗时、画面曝光（交给 #2070 报告者）。
