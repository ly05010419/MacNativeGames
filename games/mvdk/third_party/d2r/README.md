# D2R 改写器来源

上游：https://github.com/jhaizhou-ops/d2r-mobile

固定提交：`9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`

复制文件：`dm/tools/patch_nso.py`、`dm/verify/structck.py`、
`dm/verify/equiv.py`、`dm/verify/nsoimg.py`。MIT 许可证见同目录 LICENSE。
`call_window.py` 为本工程新增的调用窗口编码器。

本地改动：

1. 识别 `br x18` 跳转表时，向前查看的指令数量从 8 增加到 32。
《王国之泪》v0 的 `main+0x590574` 在地址计算与跳转之间插入了浮点指令，
原来的八条窗口无法读到 `main+0x590550` 的 `adr x18`。
2. 为 `main+0x9c9858: blr x18` 增加两字 C 窗口：重放前一条栈存储，
   保存/恢复借用寄存器，通过 K18 线程槽准备 x30，然后回到原调用位置执行
   `blr x30`。因此原 SP、参数和产生 LR 的 PC 保持一致。检查已知直接分支、
   函数入口、动态定义与相对重定位，拒绝绕过准备字进入第二字的情况。
3. 独占窗口出口表的第一个条目改为默认落空出口 hi，修复两个外部分支窗口
   条件未成立时落入条件成立出口的问题。
4. 结构检查器理解 C 窗口并逐指令核对其模板和出口；无游戏专用宿主门时不要求
   D2R 的 hle.h，涉及宿主门的 CUTS 仍需显式提供已登记接口。
5. 等价检查器在 C 窗口内继续执行原调用位置的 BLR，再比较被调用函数入口状态。
   其余判据保持原有方式；独占访问仍使用原有 deexcl 测试模型。

6. 自旋等待（`patch_nso.py`「Spin waits」）：从 nxnative（https://github.com/seedds/nxnative ，MIT，
   提交 066f2af、6d34984，许可证见 `games/totk/third_party/licenses/nxnative.txt`）移植循环搜索与复制。
   只对 `SPIN_BUILDS` 登记的构建号生效（现为旷野之息 1.9.0 main）；没有代码内数据表，改为要求循环在同一个
   eh_frame 函数里；已改写的字按与解码映像（`IMG0`）的差异判定；门用本工程的 `runtime_gate`，宿主侧为
   `threads.c` 的 `runtime_spin_wait`。结构检查器 ⑫ 核对回跳、跳板、副本与门块。

main/sdk/subsdk0 均已使用本地版本重新生成并完成全量结构和逐站点等价检查。
新增调用方式另有递归、异常目标、负对照与 M4 原生多线程测试；修复的独占窗口
另有未简化的 LDXR/STXR 成功路径与注入失败状态测试。
复现入口：`make verify`，完整范围说明见项目 README 与 claude.md。

`../../runtime/gate.S` adapts the register-preserving `dm_gate` section of
`dm/host/entry.S` from the same pinned upstream commit, under the included MIT
license. The loader and service dispatch are separate TOTK-specific code.
