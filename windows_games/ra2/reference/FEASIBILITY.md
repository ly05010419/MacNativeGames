# Red Alert 2 → Apple Silicon：可行性记录

标注：VERIFIED（已用命令复现）/ OBSERVED（看到但未系统验证）/ HYPOTHESIS / FALSIFIED / UNRESOLVED。
输入：用户的 Steam 正版，app 2229850（见 ../install.zh-CN.md）。

## §1 输入（VERIFIED）

| 文件 | SHA-256 | 事实 |
|---|---|---|
| `game.exe` | `73288c03b58d370be268ca6d156b4e33bfdb2066dc980359467d8852ff3b00df` | PE32 i386，MSVC，未加壳；PE 时间戳 0x3b1ebbed（2001-06-07，即 1.006）；ImageBase 0x400000，**无重定位**；`.text` 0x401000 大小 0x3985cd；入口 0x785aa0 |
| `Blowfish.dll` | （待记录） | PE32，ImageBase 0x11000000，有重定位；4 个导出（COM 服务器） |

`pe_symbols.py` 输出在 `game/generated/symbols/{game,blowfish}/`。

## §2 导入与 x86recomp HLE 覆盖（VERIFIED，x86recomp `1c90fe3`）

统计方法：x86recomp `src/runtime/hle` 中所有 `HLE`/`HLE64`/`HLE_NAMED`/`HLE_NAMED_X`/`RECOMP_WS2` 注册，与 `imports.tsv` 按 (dll 小写, 名称或 #序号) 比对。

| 模块 | 导入 | 已实现 | 覆盖 |
|---|---|---|---|
| GAME.EXE | 368 | 234 | 64% |
| Blowfish.dll | 83 | 77 | 93% |

GAME.EXE 缺失 134 个，按用途分：

| 类别 | 缺失 | 预计何时需要 |
|---|---|---|
| `DirectDrawCreate`（ddraw v1 接口） | 1 + 整套 IDirectDraw/IDirectDrawSurface v1–v3 vtable | M4，必需 |
| `DirectSoundCreate`（dsound #1） | 1 | M4，必需（现有 dsound 只有 #11 DirectSoundCreate8） |
| ole32（OleInitialize、CoRegisterClassObject、StgCreateDocfile、OleSaveToStream…） | 13 | M3（COM 初始化）/ M5（存档；按 [MS-CFB] 自写，Wine 仅作行为参考） |
| oleaut32（序号） | 9 | M3，待查用途 |
| user32 对话框类（DialogBoxParamA、GetDlgItem…）等 | 33 | HYPOTHESIS：多数只在错误/调试路径 |
| gdi32（TextOutA、SaveDC、CreateDIBSection…） | 11 | M4 |
| kernel32（串口 Comm*、控制台、CompareString、lstrcatA…） | 32 | 串口/控制台多半走不到；其余按需 |
| imm32 | 5 | M4（中文输入） |
| comctl32 ImageList_* | 7 | HYPOTHESIS：走不到 |
| wsock32（序号） | 14 | M7（局域网） |
| binkw32 | 3 | 已做：`binkw32.cpp` 用 ffmpeg 解码 Bink（菜单动画、战役动画）；只有开机标志 WESTLOGO 被启动器跳过（`launcher.cpp`） |
| advapi32 Reg* | 4 | M3 |

Blowfish.dll 缺：RegDeleteKeyA、CompareStringA/W、FatalAppExitA、HeapValidate、lstrcatA。

## §3 Ghidra 分析（VERIFIED）

Ghidra 12.1.4 headless + x86recomp `tools/ghidra/ExportFunctions.java`/`ExportJumpTables.java`：
GAME.EXE 8,077 个函数、406 个跳转表；Blowfish.dll 382 个函数、50 个跳转表。

## §4 第一次 lift（VERIFIED）

`recomp_lift --exe GAME.EXE … --module Blowfish.dll …`，50 秒：
24,412 个函数（Ghidra/导出 8,459；指针扫描发现 15,953，其中 15,864 为弱函数），跳转表 377 + 恢复 2，指令 2,619,104，214 个文件，307 MB。

**未实现点 1,078 个：**
- 950 个在弱函数里：从数据误当入口的地址开始解码。抽查 `0x4022B0`、`0x401D56` 都落在真实指令中间（反汇编确认）。因为没有重定位表，指针扫描分不清数据和地址，这在预期之内。HYPOTHESIS：这些点不可达；需要像 reFrozen 那样用 `data` manifest 条目逐一排除，或用审计证明不可达。
- **128 个在 Ghidra 真函数里，全部是 MMX**（13 种：movq、psrlw、pxor、movd、punpcklbw、psrad、punpckhwd、packuswb、pmulhw、emms、packssdw、pmullw、paddusb），位于 8 个函数：`0x797440`、`0x79777A`、`0x797ABA`、`0x797DFA`（调用点 0x538d08 一带）和 `0x7984C0`、`0x798602`、`0x798740`、`0x798880`（调用点 0x5ddd55 一带）。调用方按一个 0–3 的变量走跳转表选择变体，不受 CPUID 门控（OBSERVED）。→ **MMX 必须实现**。

上游 x86recomp 没有 MMX（`dispatch.cpp` 注释："no supported game has MMX code"）。reFrozen 的补丁 0003/0010 有 MMX，但 reFrozen 是 GPL-3.0；我们按 Intel SDM 自写，配手算测试。

## §5 MMX（VERIFIED）

- `third_party/x86recomp/src/runtime/mmx.h`：MMn 是 x87 物理寄存器 Rn 的有效数字部分；除 EMMS 外的 MMX 指令都执行 TOP=0、全部标为有效，写入时把 bits 79–64 置为全 1；EMMS 把全部标为空（SDM Vol.1 §9.5）。
- `tools/lifter/emit.cpp` 的 `EmitMmx`：movd、movq、pxor、psrlw、psrad、punpcklbw（内存形式只读 32 位）、punpckhwd、packuswb、packssdw、pmulhw、pmullw、paddusb、emms。
- `tests/mmx_test.cpp`：期望值全部按 SDM 手算，全部通过。
- 重新 lift 后：**Ghidra 真函数里的未实现点 = 0**；剩下的 862 个全部在弱函数里（§4）。

## §6 链接与启动（VERIFIED）

- `build/main/ra2`：Mach-O arm64，92 MB；编译 0 错误，约 210 秒（`-j 6`，M4 16 GB）。
- run1（不含 MMX）：从 `0x785AA0`（CRT 入口）→ `0x784EAF` → `0x784FD3` → `0x4D48D0` → `0x525030` → `0x76B0A0`，停在 `wsock32.dll!#1111`（EnumProtocolsA）。
- 给 x86recomp 补上：ws2_32 的函数同时以名字和序号注册到 wsock32.dll（Winsock 1.1 的序号相同）；`WSAStartup` 按请求的版本返回（以前固定 2.2）；`EnumProtocolsA` 按 MSDN 报告 TCP/UDP。
- run2：越过网络初始化，停在 `ole32.dll!OleInitialize`（调用方 `0x73A8E0`，CRT 初始化阶段的全局构造函数）。以未实现导入的方式明确停下，没有崩溃。

## §7 结论（当前）

- **M1（指令层）**：达成。只缺 MMX，已补齐，真函数里 0 个未实现点。
- **M2（链接、启动）**：达成。原生 arm64 可执行文件能执行 CRT 初始化，进入游戏代码，并在第一个缺失的系统接口处明确停下。
- 下一步 M3：COM/OLE（OleInitialize、CoRegisterClassObject、CoCreateInstance → 重编译的 Blowfish.dll）、注册表、MIX 读取。
