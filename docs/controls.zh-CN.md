# 操作与设置

[← 安装](install.zh-CN.md) · [English](controls.md)

装好 App 之后怎么玩：手柄、键盘、鼠标、震动、体感和设置面板（1 键）。

在 Mac 上，macOS 能识别的手柄（如 DualSense、Xbox 手柄）会映射为 Switch Pro 手柄。PlayStation 和 Xbox 手柄的四个面键按位置映射，和游戏里的按键提示位置一致：右边是 A、下边是 B、上边是 X、左边是 Y（圆圈 / 叉 / 三角 / 方块；Xbox 的 B / A / Y / X）。任天堂手柄按标签映射。按 **6** 可以手动选手柄类型（Xbox、PlayStation 按位置，Switch 按标签）；类型为自动时，设置 `TOTK_FACE_BUTTONS=label` 可以让所有手柄都按标签映射。震动和体感需要手柄及 macOS 提供对应能力，并非所有手柄都有陀螺仪。macOS 不给震动能力的手柄（显示为通用 HID 手柄）仍然可以震：游戏会按手柄配置表，把震动直接写进它的 HID 设备（内置：8BitDo Ultimate 2 Wireless，固件 1.03 及以上）。要加别的手柄，在 `controls.txt` 旁边的 `rumble-profiles.txt` 里每个手柄写一行：`厂商号 产品号 报告ID 长度 低频偏移 高频偏移 [1]`（最后可选的 1 表示高频马达很弱，由低频马达承担较强的一边；十进制或 `0x` 十六进制；格式见 `games/totk/runtime/gamepad_feedback.m` 里配置表上方的注释；SDL 的 HIDAPI 驱动里能查到很多手柄的报告格式）。下面两行是按 SDL 的 HIDAPI 驱动推出来的，**没有在手柄上试过**（抄进 `rumble-profiles.txt` 试，能不能用请告诉我们）：8BitDo Pro 3 `0x2dc8 0x6009 0x05 5 1 2`，NVIDIA Shield V103 `0x0955 0x7210 0x01 7 2 4`。报告里带固定命令字节的手柄（Xbox 360、GameSir、飞智）暂时写不成配置行。`TOTK_RUMBLE_HID=0` 关闭这个功能；`TOTK_RUMBLE_HID_FLOOR`（0..255）和 `TOTK_RUMBLE_HID_HOLD_MS` 决定游戏里很弱很短的震动被加强多少、多维持多久。Mac 键盘映射按美式布局对应的物理键位：

| 按键 | Switch | | 按键 | Switch |
|---|---|---|---|---|
| W A S D | 左摇杆 | | 方向键 | 右摇杆 |
| J | A | | K、空格 | B |
| I | X（跳） | | U | Y（攻击） |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | 十字键 左 / 上 / 右 / 下 | | Z / C | 左 / 右摇杆按下 |
| Return | +（菜单） | | Tab、Esc | −（地图） |

手柄布局（Mac）：默认按位置对应 Switch 的布局，每种手柄都是右边的面键为 A、下边为 B、上边为 X、左边为 Y（任天堂手柄按标签，结果相同；`TOTK_FACE_BUTTONS=label` 让 PlayStation、Xbox 手柄改为按标签）。要改任何按钮，在 `~/Library/Application Support/MacNativeGames/TOTK/controls.txt` 写 `实体按钮 = Switch 按钮` 的行；没写的按钮保持默认，`#` 开始注释，`none` 表示不用这个按钮。实体按钮按位置命名：`south` `east` `west` `north`（面键），`l1` `r1` `l2` `r2` `l3` `r3`，`menu`（Options / Menu），`view`（Create / View），`up` `down` `left` `right`（十字键），`touchpad`（DualSense / DualShock 4 的触控板按下，默认不用；`touchpad = minus` 打开普尔亚平板）；打开鼠标模式（设置面板）后还有 `mouse_left` `mouse_right` `mouse_middle` `mouse4` `mouse5`（两个侧键，默认不用）。Switch 按钮：`a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `lstick` `rstick` `up` `down` `left` `right`。默认布局和下面这个文件相同（按位置）：

```
east = a
south = b
north = x
west = y
mouse_left = y
mouse_right = zr aim mouse_left
mouse_middle = zl
```

要不按住就跑，可以让两行驱动奔跑键：`south = b tap`（像 GTA 一样连续点按下方面键，每次按下保持 400 毫秒）或 `l3 = b toggle`（按一下左摇杆再移动；摇杆回到中间时停止）。可选的模式写在 Switch 按钮后面：`hold`（默认，按住时保持）、`tap`、`toggle`，以及 `aim <开火键>`：按住时保持，但开火键也按下时松开。`l2 = zr aim r2` 用 L2 拉弓、R2 射出，L2 松开时 R2 才按自己那一行生效。Switch 的 B 键随情境变化：攀爬时按住 B 会松手，所以攀爬时如果 toggle 还开着，角色会掉下来。攀爬前先松开摇杆，或改用 `tap`。

启动器每次启动都会检查这个文件，有错时弹窗指出第一行错误。删除文件即恢复默认布局。

**震动（Mac，设置面板）：** 游戏的 HD 震动由手柄的触觉马达播放（macOS 提供触觉功能的手柄，如 DualSense），默认开启。设置面板（1 键）里有“震动”开关、“震动强度”（0.5× 到 4×）和“体感瞄准”开关：开关立即生效，强度从下一次震动起生效；游戏启动时没开的震动或体感，打开后从下次启动起生效，该行会写明。面板把选择保存在 `controls.txt` 旁边的 `settings.json` 里，每次启动沿用：`rumble`（`on` 或 `off`）、`rumble-gain`（震动强度，大于 0、最大 4 的数，默认 1：`2` 让游戏的震动加倍，超过满强度的部分按满强度播放）和 `gyro`（`on` 或 `off`，见下面的体感瞄准）。也可以自己写这些文件，例如在 `~/Library/Application Support/MacNativeGames/TOTK`（旷野之息是 `BotW`）里写 `"rumble":"off"`。设置了 `TOTK_RUMBLE`、`TOTK_RUMBLE_GAIN` 或 `TOTK_GYRO` 时，环境变量仍然优先于文件，面板里对应一行显示为灰色的当前值。文件里写了游戏不认识的内容时保持默认值并写进日志，不会让游戏无法启动。

**体感瞄准（Mac）：** 手柄的陀螺仪会作为 Switch Pro 手柄的体感传给游戏，默认开启（设置面板的“体感瞄准”开关、`gyro` 写 `off` 或设 `TOTK_GYRO=0` 即关闭）。支持向 macOS 提供体感数据的手柄（DualSense、DualShock），以及 Switch 风格的 Pro 手柄（包括 Switch 模式下的 8BitDo），后者直接从手柄自己的 HID 报告读取体感、使用手柄出厂校准。蓝牙连接的 8BitDo Ultimate 2 Wireless（固件 1.03 及以上）macOS 不给它体感，游戏改从它的 HID 报告里读取（`games/totk/runtime/hid_imu.h` 里列着这类手柄）。Xbox 手柄没有陀螺仪；macOS 不认的普通 HID 手柄只有按键和摇杆。在游戏选项里打开体感操作，然后转动手柄瞄准（弓、究极手等）。`TOTK_GYRO_DEADZONE=<度/秒>` 设置小幅转动的死区；转动方向不对时用 `TOTK_GYRO_AXES` 重映射轴向（例如 `-x,y,-z`）。iPhone / iPad 见下面的“手机体感与震动”。已在 Mac 上用王国之泪测试。

**DualSense 自适应扳机（Mac）：** 用 DualSense 时，林克拉弓期间 R2（ZR）会像弓弦一样越按越紧，箭射出或收弓的一刻立即放松；其他时候两个扳机都没有阻力，L2 从不加阻力。游戏不会把拉弓状态告诉手柄，所以运行时每 10 毫秒从游戏内存里读取一次（`runtime/bow_state.h`，只读；支持旷野之息 1.9.0 和王国之泪 1.4.3，其他版本不动扳机）。USB 和蓝牙都能用；蓝牙下拉弓刚开始约 30–45 毫秒还没有阻力。默认开启：设置面板（1 键）里的“DualSense 拉弓扳机”开关可以立即关闭或打开，选择保存在 `controls.txt` 旁边的 `settings.json` 的 `dualsense-triggers`（内容为 `off` 或 `on`，也可以自己写）。`TOTK_DUALSENSE_TRIGGERS=on` / `off` 优先于该文件，此时开关显示为灰色的当前值。已知限制：王国之泪打开菜单时会保持拉弓状态，所以 R2 的阻力会持续到关闭菜单。`TOTK_TRIGGER_LOG=1` 记录扳机的模式和状态。已在 Mac 上用两款游戏测试，DualSense 蓝牙连接。

**鼠标（Mac，设置面板）：** 在设置面板里打开或关闭“鼠标操作”，选择保存在 `controls.txt` 旁边的 `settings.json` 的 `mouse`，下次启动沿用。打开时只要游戏窗口在前台，窗口就锁住指针（隐藏，留在窗口里）；按 ⌘Tab 或打开设置面板就释放，给马取名这类文字输入时也会释放。移动鼠标转动视角：按 yuzu“鼠标平移”的思路（这里重新实现）驱动右摇杆，鼠标位移累加成摇杆偏转，每 10 毫秒交给游戏一次然后逐渐衰减，所以鼠标一直移动视角就一直转，停下后平滑停住。同样的移动也会转动手柄的陀螺仪：在游戏选项里打开体感操作后，拉弓、究极手等瞄准直接跟着鼠标走；鼠标模式开着时陀螺仪只听鼠标的（关掉后恢复手柄的体感）。按键：左键攻击（Y），右键拉弓（ZR）、按住时点左键射出，中键锁定（ZL）；可以像其他手柄按钮一样在 `controls.txt` 里改。设置：`TOTK_MOUSE_SENSITIVITY` 1–100（默认 50：移动 20 个点就是摇杆推满），`TOTK_MOUSE_INVERT_Y=1` 上下反转，`TOTK_MOUSE_COUNTERWEIGHT`（默认 20：最小的移动也至少推动摇杆的 20%，越过游戏的死区），`TOTK_MOUSE_DECAY`（18）和 `TOTK_MOUSE_MIN_DECAY`（6）调整停下来的快慢，`TOTK_MOUSE_GYRO=0` 只用摇杆、不用陀螺仪。

**设置面板（Mac，1 键）：** 按 **1** 打开或关闭设置面板（Esc 或右上角 ✕ 也能关；按住面板空白处可以拖动，下次在同一位置打开）。用手柄时，**按住 L2 + R2 + L3 一秒**（Xbox：LT + RT + LS；Switch：ZL + ZR + 按下左摇杆）打开或关闭面板。要按住一秒，是因为这几个键在游戏里是 ZL + ZR + L3，潜行拉弓时可能会同时按到一下。面板里用十字键或左摇杆上下选行、左右改值，A（PlayStation 是 ×）确定，B（○）关闭。面板开着时键盘、鼠标和手柄都不会传给游戏；关掉后，松开手柄所有按键和摇杆，手柄就又回到游戏。每一项选了立刻生效：
- **性能信息：** 全部（窗口右上角的小框：FPS 和分辨率——先输出分辨率，括号里是游戏的内部 3D 分辨率，如 `FPS 60  1920x1080 (1920x1080)`——、着色器和渲染管线计数、负载一行（GPU 在前，后面是 CPU 各核心），以及打包时的提交号）、只显示 FPS、关。`TOTK_HUD=corners` 改为画在画面四角。
- **帧率图：** 在叠加框下面画最近 30 秒的帧率曲线，默认关。选择保存在 `controls.txt` 旁边的 `settings.json` 的 `fps-graph`（`on` 或 `off`）。
- **锁帧：** 关、30、40、60 帧，帧率严格锁定（能整除显示器刷新率时按刷新节拍，否则按自己的时钟）。
- **补帧（MetalFX）：** 关、30+30、40+40、50+50（游戏锁在 30 / 40 / 50 帧，MetalFX 在每两帧之间补一帧）、半刷新率（游戏跑显示器刷新率的一半，120 Hz 屏上是 60+60，100 Hz 上是 50+50，每次刷新都换一张图）、不限帧。选了补帧会自动关掉锁帧，反过来也一样。
- **分辨率：** 720p、1080p、1080p + MetalFX 放大到窗口、2K（2560×1440，游戏画布和界面）、2K + MetalFX 放大（4K 屏）。首次开局是 1080p，选择会保留到下次启动。窗口模式下窗口立刻变成对应大小（屏幕放不下就缩小）。旷野之息 1.9.0 只有 1080p 和 MetalFX 两项。
- **画面比例：** 16:9、16:10、21:9、32:9（1.2.1、1.4.3、旷野之息 1.6.0 和 1.9.0）。相机两侧看到更多（16:10 时保持纵向视野；`TOTK_ASPECT_VERT=1`，1.4.3，实验性，改为上下看到更多，部分界面特效位置不对）；HUD、菜单和地图保持原比例、在中间 16:9；跟随世界物体的标记暂时不跟着移。窗口模式下窗口变成这个形状，全屏时画面保持这个比例、四周留黑边。`TOTK_ASPECT=2.39`（或 `3440x1440`）从这个比例开局，并加进列表。
- **边缘平滑（MetalFX 时间性）：** 3D 场景的时间性抗锯齿，开局关（`TOTK_TAA=1` 开局就开；1.4.3 和旷野之息）；`TOTK_ANISO=16` 另外打开 16 倍各向异性过滤。
- **手柄类型：** 自动（会写出认出的手柄，如“自动（PlayStation）”）、Xbox、PlayStation、Switch。Xbox 和 PlayStation 按位置映射面键，Switch 按标签。记在 `controls.txt` 旁边的 `settings.json` 的 `controller-type`；通用 HID 手柄保持 Switch 式。游戏里提示的按键图标不跟着变（见 `make_button_icons.py`）。
- **DualSense 拉弓扳机（R2）：** 开 / 关（见上面“DualSense 自适应扳机”；旷野之息 1.9.0 和王国之泪 1.4.3）。
- **鼠标操作：** 鼠标模式开 / 关（见上面“鼠标”）。
- **震动、震动强度（0.5× 到 4×）和体感瞄准：** 见上面“震动”和“体感瞄准”。
- 底部两个按钮：**贡献着色器和管线** 把游戏遇到、公共清单里还没有的着色器和管线发出去（这台 Mac 登录了 GitHub 命令行 `gh` 时在后台直接提交；否则打开 GitHub 提交页，并在访达里显示文件，拖进去即可）；**给作者点赞** 打开 GitHub 项目页。

叠加信息、锁帧或补帧、分辨率、画面比例和边缘平滑的选择保存在 `controls.txt` 旁边的 `settings.json`（`graphics-overlay`、`graphics-frame`、`graphics-output`、`graphics-aspect`、`graphics-taa`），下次启动沿用；分辨率和画面比例要等对应补丁就绪、并且这个选项可用时才恢复。启动时显式设了 `TOTK_MFX`、`TOTK_TAA`、`TOTK_ASPECT`、`TOTK_MFX_OUTPUT`、`TOTK_MFX_UPSCALE`，或 `TOTK_OUTPUT_RES` 不是 `auto` 时，以环境变量为准。其他保存的设置也是同一个对象的键：`controller-type`、`mouse`、`fps-graph`、`rumble`、`rumble-gain`、`gyro`、`dualsense-triggers`、`keyboard-layout`，写法如 `"mouse":"off"`。整个 JSON 只读一次、保存时整体写回；目录里旧版留下的 `*.txt` 保留不删，但不再被读取。`controls.txt` 仍是独立文件：那是按键布局，不是设置。

2–7 键不再是快捷键。iPhone / iPad 使用触屏控件或兼容手柄，没有设置面板。触屏按 Switch 手柄排布：左上 L、ZL，右上 R、ZR，中间 − 和 +，左边十字键，右边 A、B、X、Y。十字键在左摇杆上方，A、B、X、Y 在右摇杆上方，L3、R3（按下摇杆）在左下角和右下角。在左半屏避开按钮的地方按住拖动是左摇杆，在右半屏是转镜头（右摇杆）；摇杆以手指按下的位置为中心。接上手柄时触屏控件自动隐藏：碰一下屏幕就会显示，可以照常使用，手指离开 3 秒后再次隐藏。

**手机体感与震动：** 没有连接手柄时，手机的 CoreMotion 传感器作为游戏的六轴设备：虚拟摇杆照常移动、转视角，在游戏选项里打开体感操作后，倾斜手机即可瞄准弓或究极手。游戏震动交给手机的 Taptic Engine，左右两路混合成一路，并用感知曲线和短保持让小幅震动也能被感觉到（`TOTK_PHONE_HAPTICS_GAIN`，默认 1.5）。系统“设置”App › App › 本游戏中有“手機體感”和“手機震動”两个独立开关，默认开启，修改后重新启动游戏生效。接上手柄后手机自动让位、停止，断开后恢复；进后台停止，并随左右横屏方向调整体感轴向。`TOTK_PHONE_GYRO=0` / `TOTK_PHONE_HAPTICS=0` 仍可通过 `devicectl` 覆盖对应设置；`TOTK_GYRO=0` / `TOTK_RUMBLE=0` 仍分别关闭全部体感 / 震动。iPad 等没有触觉马达的设备不播放手机震动。

**超宽屏界面布局（可选，1.4.3）：** 选了宽画面比例时 HUD 和菜单留在中间 16:9。想让 HUD 贴到 21:9 或 32:9 屏幕的四角，可以用你自己的游戏文件按屏幕比例生成布局再启动：`.venv/bin/python scripts/make_romfs_patches.py --version v143 --aspect 3440x1440`（写到 `extracted/v143/romfs-patches-3440x1440`；`--hud center` 让 HUD 留在中间）和 `scripts/make_output_res_overlay.py 3440x1440 --version v143`，然后用 `TOTK_OUTPUT_RES=3440x1440 TOTK_ASPECT=3440x1440 TOTK_UI_SQUEEZE=0 TOTK_ROMFS_PATCHES=extracted/v143/romfs-patches-3440x1440` 启动（在工作目录里）。布局在启动时读取，所以这一局一直是宽屏，不配合面板里的画面比例使用。每个要改的界面面板都按名字查找，写入前核对游戏原版的值。

菜单栏左侧、应用菜单之后以游戏自己的菜单显示实时帧率（“60 FPS”，补帧显示生成帧时为“50+50 FPS”），与窗口里的性能信息显示什么无关；全屏时随菜单栏隐藏。`TOTK_MENUBAR_FPS=0` 不在菜单栏显示；`TOTK_TITLE_FPS=1` 时窗口标题末尾也显示同样的帧率（“… Ver. 1.4.3 — 60 FPS”）。

原生 2K 将游戏合成画布和 UI 改为 2560×1440，3D 场景仍由游戏独立的渲染比例控制，并非强制所有内部通道以 1440p 渲染。手动 MetalFX 在较小窗口中也可使用，输出放大会跟随实际窗口尺寸，包括 4K。

语言：游戏和启动器自身的文字（启动画面、着色器预热进度、提示叠层、键盘输入提示）跟随 Mac 的系统语言。启动器文字在中文系统上显示中文，其他语言显示英文。地区变体和主机一样：美洲以外的英语（en-GB、en-AU 等）选英式英语，温度显示摄氏度（美式英语显示华氏度）；加拿大的法语选加拿大法语，美洲的西班牙语选拉美西班牙语；1.4.3 版本中，巴西葡萄牙语会选择游戏官方的 pt-BR 文本。简体和繁体中文按文字（zh-Hans / zh-Hant）区分，没有文字标记时按地区区分（台湾、香港、澳门为繁体）。只想让本游戏使用另一种语言时，可在“系统设置 › 通用 › 语言与地区 › 应用程序”中为它选择，或设置它的首选语言，例如 `defaults write org.macnativegames.totk AppleLanguages -array pt-BR`（`defaults delete org.macnativegames.totk AppleLanguages` 可撤销）。如需指定，可用 `TOTK_LANG` 设置游戏语言（例如 `open --env TOTK_LANG=zh-Hans "Tears of the Kingdom.app"`），用 `TOTK_UI_LANG=zh` / `en` 设置启动器文字。

## 文字输入（Mac）

游戏要你输入文字（给马取名）时，窗口变暗，出现一个屏幕键盘和上方的输入框。王国之泪和旷野之息都用它，不改游戏代码。可以照常用 Mac 键盘打字（能选中、粘贴、用输入法；回车确认，Esc 取消），也可以点键盘上的键，或者用手柄：

| 操作 | PlayStation | Xbox | Switch |
|---|---|---|---|
| 在键之间移动 | 十字键 / 左摇杆 | 十字键 / 左摇杆 | 十字键 / 左摇杆 |
| 输入选中的键 | × | A | A |
| 取消 | ○ | B | B |
| 删除 | □ | X | Y |
| 空格 | △ | Y | X |
| 大小写 | L3 | 按下左摇杆 | 按下左摇杆 |
| AZERTY / QWERTY | R3 | 按下右摇杆 | 按下右摇杆 |
| 移动光标 | L1 / R1 | LB / RB | L / R |
| 字母 / 符号 | L2 | LT | ZL |
| 确认 | R2 / Options | RT / Menu | ZR / + |

- 键上标着对应的手柄按键，按手柄类型（自动或设置面板里选的）、按位置显示；改过 `controls.txt` 也不影响。
- 键盘打开时手柄用来打字，不操作游戏；松开所有按键和摇杆后才交还给游戏。
- 键盘页里有拉丁字母、法语重音字母、数字和标点；其他文字用 Mac 的输入法。
- 输入内容遵守游戏的限制：长度和不允许的字符。
- 键盘上的文字按 Mac 的语言显示中文、法语或英语（`TOTK_UI_LANG` 可改）。
- 法语系统默认 AZERTY，其他默认 QWERTY；选择保存在 `controls.txt` 旁边的 `settings.json` 的 `keyboard-layout`。
- 不需要游戏文件就能检查：`python -m unittest tests.test_text_keyboard`；`games/totk/tests/text_keyboard/test.py --preview` 打开它看效果。

## 按键图标

`python3.14 games/totk/scripts/make_button_icons.py <模组文件夹> --app <Tears of the Kingdom.app> --style keyboard`（或 `--style xbox`）生成一个模组，把游戏按键提示里的 Switch 图标换成 Mac 键盘按键（A B X Y 对应 J K I U、W A S D、方向键等）或 Xbox 按键（按 Switch 位置排列的 B A Y X、LB/RB、LT/RT、Menu/View）。它用你自己的 RomFS 重绘每种语言字体包里的图标字体（项目里不含任何游戏内容），所以每种游戏语言（包括韩语和中文）都会换成新图标，并写好 manifest，同时把游戏的资源大小表调大以容纳变大的字体（不调的话游戏启动时就会停止）；文件夹里已有的文件会保留。用旧版脚本生成的模组只覆盖其他语言，请加 `--replace` 重新生成。需要 Python 3.14 和 `python3.14 -m pip install fonttools`。文件夹里已有字体包（别的模组的，或者上次生成的）时会停下，加 `--replace` 才覆盖，换风格也要加它。Xbox 图标对应默认的按位置映射；设了 `TOTK_FACE_BUTTONS=label` 或在 `controls.txt` 里改过面键时会对不上。删除模组里的 `Font` 文件夹和 `System/Resource/ResourceSizeTable.*` 后运行 `make_mod_manifest.py` 即可恢复。以固定图片显示的提示（部分教程动画）仍是 Switch 图标。
