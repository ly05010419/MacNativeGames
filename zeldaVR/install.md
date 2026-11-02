# Zelda VR: Tears of the Kingdom on a Meta Quest 3

[English](#english) · [中文](#中文)

## English

Tears of the Kingdom 1.4.3 runs on your Mac as usual, and its picture goes to a **Meta Quest 3** over the USB-C cable.
It is not a flat screen floating in the headset: the game draws the view for every head pose.

- **6DoF**: turning your head turns the game camera (3 degrees of freedom, always on). With **Head position (6DoF)** on in
  the VR page, leaning and crouching move the camera too (all 6 degrees of freedom, up to a metre each way).
- **First person** (Link's head hidden, the camera at his eyes) or **third person**. `P` switches between them (eased
  over 0.6 s); a camera spring smooths the head's bob while running.
- **Game sound in the headset** (the Mac's speakers go quiet while it is connected).
- **Quest 3 controllers and motion gestures** (swing, shield, bow, whistle): built in, not yet tested on a headset. A
  controller on the Mac (DualSense, Xbox) works as always.
- Measured on an M4 Mac: 1680×1760 per eye at 150 Mbps, about 72 fps in the headset, 80–90 ms latency.

VR is part of the game's runtime and off unless the VR launcher starts the game; the normal app is not affected.

### What you need

- An Apple silicon Mac with macOS 26, 16 GB of memory or more, and everything the normal build needs
  ([docs/install.md](../docs/install.md#what-you-need)): your own Tears of the Kingdom base game, the **1.4.3 update**
  and your `prod.keys`.
- Xcode, or its command line tools (`xcode-select --install`): the streamer is built with `swift`.
- [Homebrew](https://brew.sh): the installer adds `adb` (android-platform-tools) with it.
- A Meta Quest 3, a USB-C **data** cable (a charge-only cable shows no headset), and a room bright enough for tracking.

### 1. Prepare the Quest (once)

1. **Developer mode.** Make a developer organization at [developers.meta.com](https://developers.meta.com/horizon/)
   with your Meta account (free; a phone number or payment card verifies it). In the **Meta Horizon** app on your phone:
   Devices → your Quest 3 → Headset settings → **Developer mode** on. Restart the headset.
2. **USB debugging.** Connect the Quest to the Mac with the cable and put it on. Answer **Allow USB debugging?** with
   **Always allow from this computer** ticked, then **Allow**. If the question does not come, unplug and plug in again.
3. **Guardian.** Set up your play area as usual; without tracking (a dark room) the Quest covers the picture with its
   own dialog.

The installer does the rest on the headset: it installs the ALVR client (version 20.14.0 from ALVR's GitHub releases,
checked by its SHA-256) and answers its microphone question for good, since that dialog would hide the connection.
ALVR then appears in the Quest's Library under **Unknown sources**.

### 2. Install on the Mac

From the repository folder, with the Quest on the cable:

```
zeldaVR/install_vr.sh -- "<base game .xci or .nsp>" -k "<prod.keys>" --update "<1.4.3 update .nsp/.nsz>"
```

The game files go after `--`, exactly as for `build_app.py`. It does four steps:

1. **The game app with VR** (`build_app.py --vr`): as the normal build, plus the picture modes VR uses. The first build
   takes 20–40 minutes and needs about 40 GB free; do not stop it.
2. **The streamer**: downloads [BasaltVR](https://github.com/frenchbaguetteman/BasaltVR) at a fixed revision, applies
   `zeldaVR/streamer/basaltvr.patch` (our changes: sizes for a native client, the game's sound, HEVC, pacing by the
   encoder, restart when the Quest stops showing frames) and builds `basalt-streamer` (about a minute).
3. **The Quest**: ALVR and its microphone setting (above).
4. **"塞尔达 VR" on your Desktop**: a launcher for the next steps.

Everything goes into `~/zeldaVR-work` (`--work <folder>` puts it elsewhere). Run it again after `git pull`: every step
whose inputs did not change is skipped. Other options: `--skip-quest` (no headset connected now), `--no-app`,
`--no-desktop`, `--name <Desktop name>`.

### 3. Play

1. Connect the Quest with the cable and put it on.
2. Double-click **塞尔达 VR** on the Desktop. It wakes the Quest, opens ALVR on it and starts the game; the game starts
   the streamer itself and connects within a few seconds. The Mac window shows both eyes.
3. Load your save as usual. Your saves and settings are the normal app's.

| Action | Keyboard | Quest 3 controllers |
|---|---|---|
| First / third person | `P` | VR page |
| Recentre (look straight ahead) | `O` | press both sticks |
| Settings panel | `1` | hold left X for 0.6 s |

**The VR page** of the settings panel (open it with `1`, the last tab): per-eye resolution, **Head position (6DoF)**,
first or third person, first person camera spring, camera distance in third person, Link's head, motion controls, and
whether the Mac window shows the Quest's lens shape. The game render resolution there is the display page's setting;
VR adds headset-shaped native 4K, 4K + MetalFX and native 6K.

### When something is wrong

- **The installer stops at "no Quest on the cable"**: developer mode, the USB-debugging question and a data cable.
  `adb devices` must list the headset as `device`.
- **Black picture in the headset**: open ALVR on the Quest again, or `adb shell am force-stop alvr.client.stable` and
  double-click the launcher again. A Guardian or permission dialog in the headset blocks it too.
- **Stutter or high latency**: lower the per-eye resolution on the VR page (1680×1760 keeps 72 fps on an M4).
- **The picture is not straight ahead**: recentre (`O`, or both sticks).

To remove it: delete `~/zeldaVR-work` and the Desktop launcher; on the Quest, uninstall ALVR from Library → Unknown
sources.

## 中文

《王国之泪》1.4.3 照常在 Mac 上运行，画面通过 USB-C 数据线送到 **Meta Quest 3**。不是把平面画面挂在头显里：游戏按每一个头部姿态重新渲染画面。

- **6DoF**：转头，游戏镜头跟着转（3 个自由度，一直开着）。在 VR 页打开 **头部位置（6DoF）** 后，探身、下蹲也会移动镜头（完整的 6 个自由度，每个方向最多 1 米）。
- **第一人称**（隐藏林克的头，镜头在眼睛的位置）或**第三人称**，按 `P` 切换（0.6 秒平滑过渡）；奔跑时头部的晃动由镜头弹性平滑掉。
- **游戏声音进头显**（头显连着时 Mac 扬声器静音）。
- **Quest 3 手柄和体感手势**（挥剑、举盾、拉弓、口哨）：已经做进去，还没在头显里实测。Mac 上连的手柄（DualSense、Xbox）照常能用。
- M4 Mac 实测：每眼 1680×1760、150 Mbps 时头显约 72 帧，延迟 80–90 ms。

VR 是游戏运行时的一部分，只有用 VR 启动器打开游戏时才开；普通 App 不受影响。

### 需要准备

- 苹果芯片 Mac、macOS 26、16 GB 及以上内存，以及普通打包需要的一切（[docs/install.zh-CN.md](../docs/install.zh-CN.md#需要准备)）：你自己的《王国之泪》本体、**1.4.3 更新**和 `prod.keys`。
- Xcode 或它的命令行工具（`xcode-select --install`）：串流程序用 `swift` 编译。
- [Homebrew](https://brew.sh)：安装脚本用它装 `adb`（android-platform-tools）。
- Meta Quest 3、能传数据的 USB-C 线（只能充电的线连上也看不到头显），以及足够亮、能正常追踪的房间。

### 1. 准备 Quest（只做一次）

1. **开发者模式**：用你的 Meta 账号在 [developers.meta.com](https://developers.meta.com/horizon/) 建一个开发者组织（免费，需要用手机号或银行卡验证）。在手机上的 **Meta Horizon** App 里：设备 → 你的 Quest 3 → 头显设置 → 打开 **开发者模式**，然后重启头显。
2. **USB 调试**：用数据线把 Quest 连到 Mac，戴上头显。出现 **允许 USB 调试吗？** 时勾选 **始终允许这台电脑**，再点 **允许**。没出现就拔掉线重新插。
3. **安全边界（Guardian）**：照常设置游玩区域；追踪不到（房间太暗）时，Quest 会用自己的对话框挡住画面。

头显上剩下的事由安装脚本做：从 ALVR 的 GitHub 发布页下载 ALVR 客户端 20.14.0，核对 SHA-256 后安装，并把它的麦克风权限固定设为拒绝（那个对话框会挡住连接）。之后 ALVR 出现在 Quest 资料库的 **未知来源** 里。

### 2. 在 Mac 上安装

在仓库目录里，Quest 插着数据线：

```
zeldaVR/install_vr.sh -- "<本体 .xci 或 .nsp>" -k "<prod.keys>" --update "<1.4.3 更新 .nsp/.nsz>"
```

游戏文件写在 `--` 后面，和 `build_app.py` 的参数完全一样。它做四步：

1. **带 VR 的游戏 App**（`build_app.py --vr`）：和普通打包一样，另外生成 VR 用的画面档位。第一次要 20–40 分钟、约 40 GB 可用空间，中途不要停。
2. **串流程序**：下载固定版本的 [BasaltVR](https://github.com/frenchbaguetteman/BasaltVR)，打上 `zeldaVR/streamer/basaltvr.patch`（我们的改动：原生客户端的画面尺寸、游戏声音、HEVC、按编码器的节奏出帧、Quest 不再显示画面时自动重启），编译 `basalt-streamer`（约 1 分钟）。
3. **Quest**：安装 ALVR、设置麦克风权限（见上一节）。
4. **桌面上的“塞尔达 VR”**：之后用它启动。

所有东西都放在 `~/zeldaVR-work`（`--work <目录>` 换位置）。`git pull` 之后再跑一次即可，输入没变的步骤会自动跳过。其他选项：`--skip-quest`（现在没连头显）、`--no-app`、`--no-desktop`、`--name <桌面名字>`。

### 3. 开始玩

1. 用数据线连上 Quest，戴上。
2. 双击桌面上的 **塞尔达 VR**。它会唤醒 Quest、在头显里打开 ALVR 并启动游戏；游戏自己启动串流程序，几秒内连上。Mac 窗口显示两只眼睛的画面。
3. 照常读档。存档和设置就是普通 App 的那一份。

| 操作 | 键盘 | Quest 3 手柄 |
|---|---|---|
| 第一人称 / 第三人称 | `P` | VR 页 |
| 摆正（对准正前方） | `O` | 同时按下两个摇杆 |
| 设置面板 | `1` | 按住左手 X 0.6 秒 |

**设置面板的 VR 页**（按 `1` 打开，最后一个标签）：每眼分辨率、**头部位置（6DoF）**、第一人称 / 第三人称、第一人称镜头弹性、第三人称摄像机距离、林克的头、体感操作，以及 Mac 窗口是否按 Quest 镜片的形状显示。那里的“游戏渲染分辨率”就是画面页的同一个设置；VR 另外多了按头显形状的原生 4K、4K + MetalFX 和原生 6K。

### 出问题时

- **安装脚本停在 “no Quest on the cable”**：检查开发者模式、USB 调试的询问、是不是数据线。`adb devices` 里头显要显示为 `device`。
- **头显里黑屏**：在 Quest 里重新打开 ALVR，或者执行 `adb shell am force-stop alvr.client.stable` 后再双击启动器。头显里的安全边界或权限对话框也会挡住画面。
- **卡顿或延迟高**：在 VR 页调低每眼分辨率（M4 上 1680×1760 能保持 72 帧）。
- **画面不在正前方**：摆正（`O` 或同时按两个摇杆）。

卸载：删除 `~/zeldaVR-work` 和桌面上的启动器；在 Quest 的资料库 → 未知来源里卸载 ALVR。
