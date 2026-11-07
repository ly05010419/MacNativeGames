# OOT：其他 GitHub 方案核查（2026-11-07）

本轮通过 GitHub API 读取当前分支、PR 正文及相关源码，并与本机 OOT 的实际翻译器、缓存及 F1040 绘制记录对照。只做调研和既有数据分析；未替换 App、启动游戏或得到新的 FPS 成绩。此前同 App、活动档、原画质下的实验组合约 37–38 FPS，60–90 FPS 目标尚未完成。

## 核查基准

| 仓库 | 本轮在线读取的分支 / HEAD | 注意事项 |
|---|---|---|
| seedds/nxnative | main，`863bc6b789733f8cfb4d9d1488a1e921e17d1417` | 今天还有新合入；本地参考 clone 未同步到该 HEAD。 |
| zortos293/Lichii | nx2，`9fa4a71fa051aa857e2d8f370aa31739a4f062c2` | 下列 267 / 292 / 408 / 415 / 446 / 463 均为未合入 PR，数字属于作者报告。 |
| Djihads80/Eglantine | nx2，`0857e8b4a792ac99b87af6bb6f329127aa7df683` | PR 689 未合入，正文只勾选 Windows 测试。 |
| sylvieistaken/selevet-eglantine | main，`e43eaf27ba1050f3bd5ca0d9ef64f94c950a0963` | 当前是 performance build 10；旧笔记的 performance8 是构建名，本轮同名分支请求返回 404，当前只列出 main。 |
| jhaizhou-ops/d2r-mobile | main，`6a3f1ee29ac6f27cd26cdd9eea1d29696106dac1` | 最新 Android 版本与 UI 更新不能当作 OOT / M4 的 FPS 证据。 |
| nanotchec/nxnative | main，`056e41fed45f749570533779da6d09787cfb3840` | 分支已经推进；旧 PR 的间接 mesh 缓存思路已有对应实验，不能重复计收益。 |

## 最值得对照的方案

### 1. Selevet：64 / 128 位访问只解析一次地址

[build 9 / 10 说明](https://github.com/sylvieistaken/selevet-eglantine/blob/e43eaf27ba1050f3bd5ca0d9ef64f94c950a0963/tuned/README.md) 明确提到宽全局内存访问只查一次缓冲。实际 [SPIR-V 发射器](https://github.com/sylvieistaken/selevet-eglantine/blob/e43eaf27ba1050f3bd5ca0d9ef64f94c950a0963/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp#L1071) 的 DefineGlobalMemoryFunctions 为宽访问先选缓冲，再从其 32 位视图读写相邻 2 / 4 字；避免不对齐的 vector 视图错位，并省去逐字重复解析。

这与本机数据吻合：fed302… 闭合输入有 7,635,287 次 raw helper 调用，99.60% 已命中区间缓存，继续扩大缓存缺少依据；真实机器指令中又存在 B64 / B128 访问。应研究 typed 翻译中一次解析、逐字访问，量化可覆盖的动态调用数。不能把静态 B128 数量或离线字典重放时间当成整帧收益。

我们的物理地址 helper 与它的 Vulkan SSBO 选择不同。必须证明整个 8 / 16 字节访问都在同一映射内，处理非对齐、跨映射、64 位溢出与未映射回退；保留逐字语义及原子操作，不能只验证起点就直接使用后续物理地址。GPL 源码只参考机制，自行实现。

### 2. Lichii：按真实写入范围标记 GPU 脏页

[PR 415](https://github.com/zortos293/Lichii/pull/415) 从 typed IR 证明 store 的偏移边界，并按本次 grid / local size 求实际范围，未知情况退回整段。作者报告 OOT 约 130 次蒙皮派发每次只写 17–45 KB，却将整段 8 MiB 标为 GPU 修改，引出错误延迟 CPU 更新和 9–27 GB/s 下载。相同 Deku Tree 存档，先经过开场的路径约 24–25 FPS，修后约 44–47 FPS；这是其测试环境的数据。

我们已通过 raw_write_domain 原型证明同类假依赖：同局约 28.3–28.7 → 35.4 FPS，实际 GPU 时间基本没变。上游说明通用 typed 证明比单个 shader 的手工 ABI 审核更完整；下一步应将已有证明接入正式编译器，包含普通 store、原子写、运行时条件、间接派发及不可证明回退。不能照搬其 8 MiB 上限到我们后端。

### 3. Lichii：避免给新纹理上传已经过期的客体数据

[PR 463](https://github.com/zortos293/Lichii/pull/463) 追踪 GPU 写入纹理被回收后哪些整页已过期，CPU 写、解除映射、写回时清除标记；新图完全来自过期页且没有更新的 buffer 写入时，不再上传旧内容。作者在 Ryzen 5 5600 / RX 6700 XT 的 Castle Town 对照：约 18.9–19.2 → 22.8 FPS，每 600 帧上传约 9 GB → 0.8 GB。

这是有资源身份和拷贝字节证据的方向。但我们使用 Metal placement / pool 别名，尚未证明当前镜头也在重复上传这些旧数据；应先从现有资源记录统计新建纹理、首次内容来源及实际上传字节，再决定是否实现。不能按纹理年龄或“稍后可能被覆盖”跳上传。

### 4. Eglantine / Lichii：雾光照读取不能被人为截断

[Eglantine PR 689](https://github.com/Djihads80/Eglantine/pull/689) 与 [Lichii PR 292](https://github.com/zortos293/Lichii/pull/292) 指向同一问题：fog classifier 通过指针表访问距起点约 104–122 MiB 的 128 字节 SH 探针，64 MiB 只读绑定上限使读取为零，丢失蓝天环境项，留下偏暖白雾。方案只放宽 compute 的表指针范围，并限制到设备支持范围；不是简单调低雾。

我们 gmem_map_gpu 保存池完整 lo / hi / delta，没有这条固定 64 MiB 截断；已有闭合输入中的映射长 156,852,224 字节。但这只排除了同一个固定上限，尚未验证我们的雾 shader 实际读到正确 SH 数据。不能直接把上限改成 256 MiB 宣称修好。此前浮点原子加法、gather 常量偏移缺口仍需沿真实曝光 / 雾输入输出核查。

## 不能直接算作本机等画质提速的结果

| 来源 | 实际内容 | 本机结论 |
|---|---|---|
| [Lichii PR 267](https://github.com/zortos293/Lichii/pull/267) | 9950X3D / RTX 4080 / 60 GiB / Linux；GPU accuracy Low、1.5×、Dynamic FPS、CPU 绑核，多项改动组合；重处站立 60、跳跃约 50。 | 不是 M4 单项优化或稳定 60 的证明；各项要拆开验证。 |
| [Lichii PR 408](https://github.com/zortos293/Lichii/pull/408) | 去掉未使用顶点属性造成的重复 PSO key、按预算驱逐、全局 helper DontInline，主要改善现编和长帧。 | 我们最近正常 FPS 窗口现建 library / PSO 为 0，不能用它解释当前稳定的约 37–38 FPS。 |
| [Lichii PR 174](https://github.com/zortos293/Lichii/pull/174) | 软件 HMMA 按 K8 展开，7900 XT 菜单帧 GPU 约 18 → 12.6 ms；整段 K16 展开反而寄存器压力过高。 | 我们 TensorMsl 已使用 simdgroup_multiply_accumulate，缓存中 11 份 MSL 包含该路径，不能重复计算软件展开收益。 |
| [Lichii PR 319](https://github.com/zortos293/Lichii/pull/319) | 可选绕过游戏空间抗锯齿 / 锐化网络，改用 tonemap 结果。 | 改变画面处理，不计入当前原画质优化成绩。 |
| [Selevet build 10](https://github.com/sylvieistaken/selevet-eglantine/commit/e43eaf27ba1050f3bd5ca0d9ef64f94c950a0963) | 默认开启 FSR 3 插帧，在真实帧之间生成帧；build 9 还包含部分 GPU 默认替代网络、关闭遮挡及失败 draw 跳过。 | 显示帧数、真实渲染 FPS、游戏速度必须分开；这些组合不能证明真实渲染达到 60–90。 |
| [Lichii PR 446](https://github.com/zortos293/Lichii/pull/446) | Linux 信号量轮询从 1 ms 改 50 µs；低精度、0.75×下作者约 11% 帧时间改善。 | 我们的 Metal event 等待路径不同，不能直接套 Linux sleep / timer slack。 |
| [MoltenVK PR 2842](https://github.com/KhronosGroup/MoltenVK/pull/2842) | 原生 Metal mesh / 间接绘制及设备真实限制；仍未合入，实测是 M5 Pro，作者仍请求真实 M3 / M4 覆盖。 | 我们 M4 已走 native mesh；可参考 API 语义，不能再声称从模拟 mesh 切原生的一次性收益。 |

## 本轮额外确认的 mesh 分配边界缺口

nxnative 今天的 [d365496d](https://github.com/seedds/nxnative/commit/d365496da3fac6133fa9654a60ac9afb4f63eb39) 按原语块预留索引字节；[3ccb7da0](https://github.com/seedds/nxnative/commit/3ccb7da07a0ae35b90fcaa48d9b8c2cf09bcbaf1) 另修 vertex / primitive 分区、point size 与 topology。

对本机完整请求及 F1040 绘制记录的静态审计：

- 39 份 mesh 请求中，9 份 maxP=126；全部 topology=3、无 point-size / primitive-output SPH 标志。因此本样本直接命中的是原语索引容量，尚不能把其他布局修正都归为当前场景原因。
- MeshStage 当前按 align4(4 + 3×126)=384 字节分配索引区；按 guest 的完整 32 原语块写入契约应为 align4(4 + 3×128)=388，少 4 字节。
- 其中 d4171cf9… 与 e8c66728… 出现在 F1040，分别 8 / 2 次 mesh draw，共 10 次。它们不是仅存在于离线种子。
- 尚未为这两份 shader 捕获边界写入，也未完成 GPU canary 或修复后的实机画面对照。静态容量缺口不等于已经证明本局发生越界，更不等于已定位全部白雾或 FPS 原因。
- 前次 primitive-word 实验只给 fed302… 替换，约 +0.22% FPS 后撤下，不能用于否定其他 mesh 的容量问题。nxnative 最新字节直写在 M6 整帧也没有稳定收益；M3 Pro 的旧 AND / OR 报告包含动态分辨率变化，不照搬数字。

私有小型审计摘要：`MacNativeGames-artifacts/codex/oot-fps-60-90-20261107/upstream-mesh-layout-audit.json`。该文件只存身份、控制字段与计数，不在本仓库放游戏代码。

## 后续顺序与验证边界

1. 对已执行的 maxP126 mesh 做完整块索引容量及 canary 验证，修容量契约；失败必须产生实际地址 / 组号 / 范围证据。图像问题先取证，不能把它包装为已证实的提帧项。
2. 性能优先验证宽 raw 访问的一次解析：先统计动态覆盖，使用已有闭合输入比较完整 GPU 输出，做跨边界反例，再在唯一 App、活动档、原画质下测实际选择次数、GPU 成本和 FPS。
3. 将现有 raw_write_domain 推进为正式 typed 证明；资源上传方向先取得本机实际拷贝量，不为上游的百分比先改代码。
4. 保持真实时间推进；不通过减画质、漏 draw、绕过网络或插帧冒充真实 60–90。所有候选仍需分别报告局部工作、额外成本、整帧结果及画面覆盖。

查过经验 78、79、80、86：上游改动必须接到本机的实际等待链与 GPU 工作上，局部微基准、别的硬件和别的场景不能代替本机整帧验收。GPL 项目只参考机制，不复制源码；本轮无运行时代码改动。
