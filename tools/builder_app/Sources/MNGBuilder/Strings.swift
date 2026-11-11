import Foundation

/// Every visible text, in Chinese when the Mac's first language is Chinese, else English.
enum T {
    static let zh = (Locale.preferredLanguages.first ?? "en").hasPrefix("zh")
    static func s(_ en: String, _ cn: String) -> String { zh ? cn : en }

    static let title = s("MacNativeGames Builder", "MacNativeGames 打包器")
    static let dropHere = s("Drop your game files here", "把游戏文件拖到这里")
    static let dropHint = s("The game (XCI, NSP or a Switch 2 dump folder), its update, prod.keys and, for an NSP, your firmware folder. A whole folder works too. Nothing is copied.",
                            "游戏本体（XCI、NSP 或 Switch 2 的游戏文件夹）、更新、prod.keys；NSP 还要固件文件夹。整个文件夹拖进来也行。文件不会被复制。")
    static let choose = s("Choose Files…", "选择文件…")
    static let clear = s("Clear", "清空")
    static let files = s("Files", "文件")
    static let build = s("Build and Play", "打包并游玩")
    static let rebuild = s("Build Again", "重新打包")
    static let play = s("Play", "游玩")
    static let cancel = s("Cancel", "停止")
    static let showInFinder = s("Show in Finder", "在访达中显示")
    static let openLog = s("Open Log", "打开日志")
    static let copyError = s("Copy Error", "拷贝错误")
    static let settings = s("Folders", "文件夹")
    static let output = s("Apps go to", "App 放在")
    static let work = s("Work folders in", "工作目录放在")
    static let change = s("Change…", "更改…")
    static let thisMac = s("This Mac", "这台 Mac")
    static let fix = s("Fix", "修复")
    static let recheck = s("Check Again", "重新检查")
    static let blocked = s("Not ready", "还不能打包")
    static let ready = s("Ready to build", "可以打包")
    static let building = s("Building", "正在打包")
    static let doneOK = s("Done. Starting the game…", "打包完成，正在启动游戏…")
    static let failed = s("The build stopped", "打包停下了")
    static let cancelled = s("Cancelled. Build again to go on where it stopped.", "已停止。再按一次打包会从停下的地方接着做。")
    static let cancelling = s("Stopping…", "正在停止…")
    static let log = s("Log", "日志")
    static let firstBuild = s("First build on this Mac: the bar follows the build's four stages, no time estimate yet.",
                              "这台 Mac 第一次打这个游戏：进度条按四个阶段走，还没有剩余时间。")
    static let noRepo = s("This app does not know its MacNativeGames folder: make it again with tools/builder_app/make_app.py.",
                          "这个 App 不知道 MacNativeGames 源码在哪：用 tools/builder_app/make_app.py 重新生成它。")
    static let existing = s("Already built", "已经打包过")

    static func stepTitle(_ n: Int) -> String { zh ? "第\(["一", "二", "三"][n - 1])步" : "Step \(n)" }
    static let step1 = s("Choose the game", "选择游戏")
    static let step1Hint = s("Drop the game here, or choose it: an XCI or NSP with its update (a folder that holds them works too), or a Switch 2 dump folder. An NSP also needs your unpacked firmware folder. Nothing is copied.",
                             "把游戏拖到这里，或点“选择…”：XCI 或 NSP 和它的更新（放在一个文件夹里，直接选文件夹也行），或 Switch 2 的游戏文件夹。NSP 还要解开的固件文件夹。文件不会被复制。")
    static let chooseGame = s("Choose Game…", "选择游戏…")
    static let step2 = s("Choose your keys", "选择密钥")
    static let step2Hint = s("The prod.keys dumped from your own Switch. It is remembered for every game.",
                             "从你自己的 Switch 导出的 prod.keys。选一次，以后每个游戏都用它。")
    static let chooseKeys = s("Choose prod.keys…", "选择 prod.keys…")
    static let noKeysNeeded = s("This game needs no keys: go on to step 3.", "这个游戏不需要密钥，直接做第三步。")
    static let keysOK = s("These keys open the game.", "这份密钥能打开这个游戏。")
    static let step3 = s("Build and play", "打包并游玩")
    static let step3Hint = s("Builds the app (20–40 minutes the first time, much less after), signs it and starts the game.",
                             "打包 App（第一次 20–40 分钟，之后快得多）、签名，然后启动游戏。")
    static let doFirst = s("Do the steps above first.", "先完成上面的步骤。")
    static let macOK = s("This Mac has everything the build needs.", "这台 Mac 已经具备打包需要的一切。")

    static func role(_ r: String) -> String {
        switch r {
        case "keys": return s("Keys", "密钥")
        case "base": return s("Game", "游戏本体")
        case "update": return s("Update", "更新")
        case "dlc": return s("DLC", "DLC")
        case "ns2_dump": return s("Switch 2 game", "Switch 2 游戏")
        case "nup": return s("Game package", "游戏包")
        case "firmware": return s("Firmware", "固件")
        case "fonts": return s("Shared fonts", "系统字体")
        default: return s("Not used", "不用")
        }
    }

    static func stage(_ name: String) -> String {
        switch name {
        case "code": return s("Rewriting the game's code", "改写游戏代码")
        case "data": return s("Extracting the game's data", "解出游戏数据")
        case "update": return s("Merging the update", "合并更新")
        case "shaders": return s("Preparing the shaders", "准备着色器")
        case "app": return s("Assembling and signing the app", "组装并签名 App")
        default: return name
        }
    }

    static func minutes(_ seconds: Double) -> String {
        let m = Int((seconds / 60).rounded(.up))
        return zh ? "约 \(m) 分钟" : "about \(m) min"
    }

    static func clock(_ seconds: Double) -> String {
        let s = Int(seconds)
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    static func left(_ text: String) -> String { zh ? "还剩 \(text)" : "\(text) left" }
    static func elapsed(_ text: String) -> String { zh ? "已用 \(text)" : "\(text) elapsed" }
}
