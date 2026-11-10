# Save sync through a chosen folder

On Mac, use the app menu's **Save Sync → Choose Sync Folder…**. On iPhone or iPad, choose the same shared folder from the startup offer or the system settings entry. Changes take effect at the next launch. With no folder selected, saves stay local.

Each game and game user has its own copy. Devices must use the same game user ID to exchange saves; matching display names alone do not identify the same user. The game continues to read and write its local save folder. The shared folder contains copies under `<game>/Users/<user ID>/Saves/current`; local sync state and backups are also separate for each user.

Downloads replace the local save only after its backup succeeds. Another device's newer generation stops uploads from the older session. An incomplete remote copy or a mismatched user ID is left untouched. A save still being written is not uploaded. Keep an independent backup before first enabling sync; concurrent offline play can leave competing progress, and files within a save are never merged.

An older setup's unscoped `Saves/current` and `cloud-sync.plist` are preserved. They are not automatically assigned to a user. Confirm the originating user before migrating such a copy.

The integration has not been exercised against a real shared folder or on an iPhone/iPad in this change.

## Using an iCloud Drive folder

iCloud Drive works as the shared folder. The app needs no iCloud setting of its own.

1. On the Mac, choose **Save Sync → Choose Sync Folder…**. When no folder is chosen yet, the picker opens in iCloud Drive. Choose or create a folder there, for example `iCloud Drive/Switch Saves`.
2. On the iPhone or iPad, choose the same folder in the Files picker: from the offer at start, the Settings app or the Home Screen icon's menu.
3. If the game was running when you chose the folder, quit it and start it again.

One folder holds Tears of the Kingdom and Breath of the Wild side by side. A folder that another Apple ID shares with you also works.

iCloud Drive can keep a file only in the cloud until a device asks for it. This is always so on an iPhone or iPad, and on a Mac when "Optimise Mac Storage" removed the local copy or the folder comes from another Apple ID. When the folder has a newer save, the game asks iCloud for it at start and waits up to 60 seconds. If the download does not finish, the game plays this device's save and does not upload it. At the next start, the game takes the folder's save and keeps this device's save in `cloud-backups`.

If two devices upload at the same time, iCloud Drive keeps the losing files as conflict copies, such as `progress 2.sav`. The game ignores these copies and removes them from the shared folder.

## 中文

Mac 在 App 菜单的“存档同步 → 选择同步文件夹”选择共享目录；iPhone、iPad 在启动提示或系统设置中选择同一个目录，下次启动生效。未选择目录时不进行同步。

不同游戏、不同游戏用户的存档分别保存。两台设备必须使用相同的游戏用户 ID；显示名称相同并不代表同一用户。游戏仍读写本地存档，共享目录只保存副本；本地同步状态和备份也按用户隔离。

下载覆盖本地存档前必须成功备份。远程出现较新代次后，旧会话停止上传。远程副本不完整、用户 ID 不匹配或本地尚在存档时，不覆盖或上传该副本。首次开启前请保留独立备份；两台设备离线同时游玩可能产生不同进度，存档内部文件不会自动合并。

旧版没有按用户隔离的 `Saves/current`、`cloud-sync.plist` 保留原处，不自动归给当前用户，迁移前须确认原用户。本次没有用真实共享目录或手机、平板进行运行验证。

## 使用 iCloud 云盘文件夹

iCloud 云盘可以作为共享目录，App 不需要单独的 iCloud 设置。

1. Mac 上选择“存档同步 → 选择同步文件夹…”，尚未选择文件夹时，选择框默认打开 iCloud 云盘。在其中选择或新建一个文件夹，例如 `iCloud 云盘/Switch Saves`。
2. iPhone、iPad 上在“文件”选择框中选同一个文件夹：启动时的提示、“设置” App 或主屏幕图标的菜单都可以。
3. 如果选择文件夹时游戏正在运行，退出后重新启动。

王国之泪和旷野之息可以共用一个文件夹，分别存放。别的 Apple ID 共享给你的文件夹也可以用。

iCloud 云盘可能只把文件留在云端，等设备请求时才下载：iPhone、iPad 总是如此；Mac 上开启“优化 Mac 储存空间”后移走了本地副本，或文件夹来自另一个 Apple ID 时也是如此。共享目录里有较新的存档时，游戏启动时会请求下载，最多等 60 秒。没有下载完时，这一局使用本机存档，也不上传；下次启动时换成共享目录里的存档，本机存档备份到 `cloud-backups`。

两台设备同时上传时，iCloud 云盘会把冲突的文件另存为 `progress 2.sav` 这样的冲突副本。游戏不使用这些副本，并会从共享目录中删掉它们。
