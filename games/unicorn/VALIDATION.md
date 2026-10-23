# 圣兽之王 Mac 验证记录

环境：Apple M5、16 GiB、macOS 26.4。仅验证 macOS ARM64；没有构建或测试 iOS。
版本：亚洲版 `010054B01AD92000`，1.05 / v262144；DLC `010054B01AD93001`、`010054B01AD93002`。
本轮验证分支为 `codex/unicorn-pr-readiness`，基于上游 `a988b5e`；提交形式为 Draft，尚未请求合并。

## 2026-10-23：干净构建与回归

在新的 `UNICORN_build-1.05/pr-validation/clean` 内，从原始本体、更新、两个 DLC 的 NSP 开始，
重新提取 ExeFS、合并 RomFS、提取 DLC、改写 main/sdk、生成运行表并编译 App。
未复制旧的提取数据、改写模块、原生目标文件或着色器缓存；使用本机已有 Python 依赖和 Homebrew 库。
SPIRV-Cross 从 Makefile 固定的提交重新获取并编译。首次网络请求在沙箱内失败，在允许网络访问后完成。

| 检查 | 结果 | 范围 |
|---|---|---|
| 本体＋更新 RomFS | 通过 | 六层 IVFC；3,591,689,668 字节，SHA-256 与 expected-v1.05.json 相同 |
| main/sdk 改写 | 通过 | 全新输入重新生成，结构检查通过，准备阶段固定哈希匹配 |
| DLC 提取和查询 | 通过 | 六层 IVFC、镜像与包哈希、编号 1/2、分页与边界 |
| App 构建与签名 | 通过 | `make runtime shader-tools app`、`codesign --verify --deep --strict` |
| 根目录源码测试 | 通过 | 303 项，跳过 1 项，其余通过；不是游戏完整性测试 |
| Opus 多流 | 通过 | `make test-opus`，六声道 PCM 对照、final range、reset、截断包拒绝 |
| 内存探针 | 通过 | 新构建 1.05 的 `TOTK_HEADLESS=1 ... probe-memory` |
| CPU 图形探针 | 通过 | `TOTK_HEADLESS=1 ... probe-gfx`，对象布局、录制、预测状态；不代表 GPU 画面通过 |
| 图形负对照 | 符合预期 | `TOTK_PROBE_NEGATIVE=24/26/27` 分别拒绝错误变体、未翻译阶段和缺深度的猜测 |
| NSZ 命令 | 通过 | 一个 DLC 从原始 NSZ 转换后，NSP SHA-256 与已验证输入一致；未将此次小包验证冒充完整本体解压 |

构建文档现显式设置 `DM_ROOT` 和 `LLVM_BIN`，让结构检查缓存留在工作目录内，并列出 LLVM 依赖。
指纹记录于 `clean/build-inputs.sha256`。源码、生成文件、游戏和日志始终分开；原始文件不会被修改。

## 图形探针崩溃的原因

原探针 `dm_nvn_probe_prediction` 构造了 `nstage=1`、control 指针为空的模拟程序。
Bayonetta 3 的分阶段绑定路径会在检查翻译缓存前读取 `control+0x714` 的阶段编号，
因此探针在地址 `0x714` 处触发 SIGSEGV。命令
`TOTK_HEADLESS=1 ./build/unicorn-native rewritten/v262144 probe-gfx` 在修复前退出 139，
符号化栈为 `state_op → pso_predict_rest → dm_nvn_probe_prediction`。

修复只给测试程序补上有效的零初始化顶点 control 元数据；保留“程序未翻译”的缓存状态及全部断言，
没有跳过预测检查或给生产读取增加空指针兜底。相同命令退出 0，三个语义破坏负对照仍退出 134 并报告对应错误。
该修复仅位于本游戏的独立副本。

## 实际游玩与温控

测试使用原 profile 的独立副本，开始前记录原 profile 各文件的 SHA-256。
已从新游戏进入雨夜教程战场，选择伊蕾妮雅队、下达移动和进攻指令，并实际观察到交战后的 WIN 画面，
人物、雨景和结算画面可见；不是完整画面对照。解锁后继续通过两次遭遇和序章剧情，进入“独角兽之戒”。
在游戏保存菜单写入槽位 01，界面显示 LV 01、PLAY TIME 0:41，生成 5,121,040 字节的 `UCSAVEFILE01.DAT`。
退出旧进程 59781，确认其不存在，再启动新进程 65943；通过标题 Load Game 选择槽位 01，恢复同一战场和出击提示。
槽位文件读取前后 SHA-256 一致；正式 profile 的全部三个文件与测试前指纹完全相同。
早先锁屏造成的中断不再阻塞这项验证；这次结果是实际游戏保存与跨进程载入，不是文件系统探针。
1.05 标题、中文开场和 DLC 编号查询已经验证；DLC 纹章菜单仍未验证。

按用户选择先进行温度监测和提醒，再决定控制策略。检测器放在仓库外部提交范围的本地工具目录，
不加入游戏运行层依赖。CPU/GPU 平均传感器温度、整机功耗与系统热压力分开记录；
短时间采样不能证明持续游玩达到热平衡，也不能由 GPU 频率单独判定热降频。

独立 macmon 0.10.0 已在本机读到有效 CPU/GPU 摄氏度和功耗；一次 12 样本、每样本约 2 秒的
保活开启记录，平均 CPU 61.62°C、GPU 66.48°C，GPU 3.47 W、整机 16.20 W。
另一次关闭保活的探索记录为 CPU 62.18°C、GPU 64.06°C，GPU 1.71 W、整机 15.80 W。
两段采样的场景稳定性、后台负载和窗口可见性尚未形成完整对照记录，不能据此宣布降温或提帧成功；
它们证明传感器与现有 `keepalive_us` 测试开关可用。没有修改正式启动默认值或加入自动温控。
一次 Foundation `ProcessInfo.thermalState` 读取为 0（nominal）；这是热压力分级，不是摄氏度。
检测器对阈值触发、重复提醒抑制、降温 3°C 后恢复提醒、缺失传感器和 JSONL 输出的检查通过。

重启读档后补做一次 A/B/A：同一窗口 2498×1405、同一战场和固定镜头，不进行 UI 操作或编译，
每次设置现有诊断开关后留 3 秒调整时钟，再采集 15 个约 2 秒的传感器样本。
三段均约 30.47 秒，日志按采样开始/结束的字节偏移截取；首个 300 帧计时块可能跨边界，统计时舍弃。
采样前后窗口均可捕获，位置及任务提示相同；游戏昼夜光照继续变化，未对后台进程和环境温度做实验室控制。

| 阶段 | CPU 均温 | GPU 均温 | CPU 功耗 | GPU 功耗 | 整机功耗 | 300 帧块 p50 范围 / p95 范围 |
|---|---:|---:|---:|---:|---:|---|
| A1 保活 1000 µs | 54.60°C | 57.11°C | 1.24 W | 1.66 W | 13.22 W | 19.14–19.29 / 22.88–23.49 ms |
| B 保活 0 | 54.60°C | 56.16°C | 1.88 W | 1.15 W | 12.70 W | 19.24–19.57 / 22.42–22.67 ms |
| A2 恢复 1000 µs | 54.95°C | 57.69°C | 1.27 W | 1.86 W | 13.35 W | 19.25–19.38 / 23.30–24.07 ms |

本次关闭保活时 GPU 平均功耗较两次开启低约 0.5–0.7 W，CPU 功耗同时增加约 0.6 W；
整机功耗低约 0.5–0.6 W，帧时间中位数仍约 19.3 ms（约 52 FPS），没有显著提帧证据。
GPU 工作 p50 从开启时约 3–4 ms 增至约 8–9 ms，说明关闭保活确实存在执行延迟取舍。
这只是单场景短时结果，不据此修改默认保活或启用自动温控。所有样本均未触发 80°C 提醒。
另有推进剧情期间的 30 秒监测：CPU 61.2–61.9°C，GPU 66.1–67.1°C；不能与不同场景的上述均温直接作降温对比。

证据保存在被忽略的 `UNICORN_build-1.05/pr-validation/`：`clean/`、`source-tests.log`、
`test-opus.log`、`probe-gfx-after.log`、`probe-gfx-negative*.log`、`probe-addons-clean.log`。
此次增加 `gameplay-resumed.log`、`save-before-restart.json`、`load-new-process.log`、
`controlled-phases.json`、`controlled-{a1-on,b-off,a2-on}.{jsonl,log,txt}` 和 `thermal-gameplay-resumed.jsonl`。
旧版 1.0.0 的记录仍保留，但当前源码固定为 1.05，不能把旧二进制留存称作当前源码支持两版。

## Draft 前清理回归

移除 BY3 1.2.0 的补丁生成、分辨率生成脚本及 `BY3_PATCHES` 装载入口；
保留图形层实际调用的关闭状态接口。符号化工具与系统字体错误提示改为当前 1.05 工作目录。
README 将旧版验证移到历史说明，并统一当前 303 项源码测试结果。

清理后的 `make runtime shader-tools app test-opus` 通过；重新编译运行层后，
`probe-memory`、CPU `probe-gfx`、`probe-addons` 均退出 0，App 严格签名检查通过。
根目录测试重新运行 303 项，跳过 1 项，其余通过。该次清理没有重新进行游戏战斗测试；
上文游玩与跨进程存档结果来自清理前版本，默认启动原本不执行任何 BY3 补丁。
日志为 `pr-cleanup-build.log`、`pr-cleanup-source-tests.log` 和 `pr-cleanup-probe-*.log`。
