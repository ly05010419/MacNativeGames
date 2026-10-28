# 存档通过 WebDAV 同步（macOS、iPhone、iPad）

存档跟着玩家走：Mac 和 iPhone 之间通过任意 WebDAV 服务器同步——坚果云、Nextcloud、
自建的 Apache/nginx，或者家里某台 Mac 上跑的 `games/totk/scripts/mini_webdav.py`。
不需要 iCloud，不需要任何签名权限，不用注册别的东西。配置了服务器之前，此功能是关闭的。

## 为什么是 WebDAV

一个存档根目录很小：`slot_00`..`slot_05`，每个玩过的槽位里有 `progress.sav` 和
`caption.sav`，几个文件、每个几 MB（布局见迁移文档
[save-migration.md](save-migration.md)），几秒钟就能传完。iCloud Drive 在 iPhone
上需要 iCloud 容器权限，免费 Apple ID 签不出来；WebDAV 什么权限都不要，游戏跑在
哪里它就在哪里可用。

## 同步什么

只同步账号的存档根目录——Mac 上是 `profile/save/<account>/`
（`~/Library/Application Support/MacNativeGames/TOTK/`），iPhone 上是
`Documents/save/`。着色器缓存、game slot、RomFS 镜像都不是存档，留在本地。每台
设备保留自己的账号身份，存档按内容移动，游戏不关心是哪台设备写的。某台设备第一次
同步前，先把游戏启动一次，让账号身份生成。

## 怎么开启

三个值，各平台同一个名字。Mac 上最省事的是游戏自带的面板：**游戏里按 1**，在面板
底部的「WebDAV 地址 / 用户名 / 密码」里填好，上方的「存档同步（WebDAV）」行负责
开关（没填地址前开关是灰的）。这里写入的值和下面的 `defaults write` 等效，下次启动
游戏时同步生效。也可以用终端：

```sh
defaults write org.macnativegames.totk TOTK_WEBDAV_URL https://dav.jianguoyun.com/dav
defaults write org.macnativegames.totk TOTK_WEBDAV_USER you@example.com
defaults write org.macnativegames.totk TOTK_WEBDAV_PASSWORD <应用密码>
```

（BotW 用 `org.macnativegames.botw`。）iPhone 上是 **设置 › TOTK › 存档同步
（WebDAV）** 的三个输入框。同名的环境变量优先——包括 `TOTK_WEBDAV_ENABLED=0`，设了
它就会整体关掉同步。坚果云和 Nextcloud 请用应用专用密码，不要用账号密码。

存档在服务器上落在 `MacNativeGames/<game>/saves/`，首次同步时自动建目录。

## 合并规则

游戏每次启动时同步一次；iPhone 上退到后台或退出应用时再多同步一次（正在写存档就
推迟到下次启动）。它比较每个文件的三份版本：本机的、服务器的、上次同步的清单
（`sync-manifest.txt`，两边各存一份）：

- 只有一侧改了 → 该侧胜出并传过去；
- 任一侧新出现的文件 → 复制到另一侧；
- 本机删了而服务器没变 → 删除同步过去；
- 服务器删了而本机没变 → 把本机文件重新传上去，清空的服务器不会清掉玩家的存档；
- 两边都改了 → **本机文件继续作为游玩存档**，服务器那份保存在
  `profile/sync-backups/sync-conflict-<UTC>/`，然后两边重新一致。一次同步里第一次
  下载覆盖本机文件之前，整个存档根目录会先拷到 `sync-backups/before-sync-<UTC>/`。

永远不会删除本机文件。恢复备份和导入备份一样
（[save-migration.md](save-migration.md)）：退出游戏，把当前 profile 挪开，把备份
挪回原位。

## 预期行为

- Mac 上一次玩的进度在**下次启动时**上传（launcher 启动游戏后自己就没了）；iPhone
  是离开游戏时上传。实际用法：iPhone 上玩完，Mac 上启动一次游戏就能收到。
- 日志走调试日志（`[save-sync]` 行），默认关闭。
- 两台设备同时同步没有互相协调；合并规则和备份保证结果是安全的（最坏情况是一份
  冲突备份），但还是建议等一台同步完再启动另一台。

## 不注册账号也能试

`scripts/mini_webdav.py` 是个家用网络的小 WebDAV 服务器：

```sh
python3 games/totk/scripts/mini_webdav.py --root ~/dav --user me --password secret &
defaults write org.macnativegames.totk TOTK_WEBDAV_URL http://<这台Mac的IP>:8442
```

引擎本身有 `tests/test_save_sync.c` 覆盖（`make check`），也可以不开游戏跑一次真实
同步：

```sh
make -C games/totk probe-save-sync PROFILE=/tmp/save-sync-profile \
    TOTK_WEBDAV_URL=http://127.0.0.1:8442 TOTK_WEBDAV_USER=me TOTK_WEBDAV_PASSWORD=secret
```
