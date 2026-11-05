# Save sync through a chosen folder

On Mac, use the app menu's **Save Sync → Choose Sync Folder…**. On iPhone or iPad, choose the same shared folder from the startup offer or the system settings entry. Changes take effect at the next launch. With no folder selected, saves stay local.

Each game and game user has its own copy. Devices must use the same game user ID to exchange saves; matching display names alone do not identify the same user. The game continues to read and write its local save folder. The shared folder contains copies under `<game>/Users/<user ID>/Saves/current`; local sync state and backups are also separate for each user.

Downloads replace the local save only after its backup succeeds. Another device's newer generation stops uploads from the older session. An incomplete remote copy or a mismatched user ID is left untouched. A save still being written is not uploaded. Keep an independent backup before first enabling sync; concurrent offline play can leave competing progress, and files within a save are never merged.

An older setup's unscoped `Saves/current` and `cloud-sync.plist` are preserved. They are not automatically assigned to a user. Confirm the originating user before migrating such a copy.

The integration has not been exercised against a real shared folder or on an iPhone/iPad in this change.

## 中文

Mac 在 App 菜单的“存档同步 → 选择同步文件夹”选择共享目录；iPhone、iPad 在启动提示或系统设置中选择同一个目录，下次启动生效。未选择目录时不进行同步。

不同游戏、不同游戏用户的存档分别保存。两台设备必须使用相同的游戏用户 ID；显示名称相同并不代表同一用户。游戏仍读写本地存档，共享目录只保存副本；本地同步状态和备份也按用户隔离。

下载覆盖本地存档前必须成功备份。远程出现较新代次后，旧会话停止上传。远程副本不完整、用户 ID 不匹配或本地尚在存档时，不覆盖或上传该副本。首次开启前请保留独立备份；两台设备离线同时游玩可能产生不同进度，存档内部文件不会自动合并。

旧版没有按用户隔离的 `Saves/current`、`cloud-sync.plist` 保留原处，不自动归给当前用户，迁移前须确认原用户。本次没有用真实共享目录或手机、平板进行运行验证。
