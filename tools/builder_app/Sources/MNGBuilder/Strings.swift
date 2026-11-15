import Foundation
import SwiftUI

/// Every visible text, in Chinese when the Mac's first language is Chinese, else English.
enum T {
    static let zh = (Locale.preferredLanguages.first ?? "en").hasPrefix("zh")
    static func s(_ en: String, _ cn: String) -> String { zh ? cn : en }

    static let title = "MNG Launcher"
    static let dropHere = s("Drop your game files here", "把游戏文件拖到这里")
    static let dropHint = s("The game (XCI, NSP or a Switch 2 dump folder), its update, prod.keys and, for an NSP, your firmware folder. A whole folder works too. Nothing is copied.",
                            "游戏本体（XCI、NSP 或 Switch 2 的游戏文件夹）、更新、prod.keys；NSP 还要固件文件夹。整个文件夹拖进来也行。文件不会被复制。")
    static let choose = s("Choose Files…", "选择文件…")
    static let clear = s("Clear", "清空")
    static let files = s("Files", "文件")
    static let build = s("Build and Play", "打包并游玩")
    static let rebuild = s("Build Again", "重新打包")
    static let startGame = s("Start game", "启动游戏")
    static let startedApp = s("The game is starting.", "游戏已启动。")
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
    /// A built game's own pages, opened under its card (Tears of the Kingdom and Breath of the Wild: tools/builder's games).
    static let gamePages: [(String, String)] = [("settings", s("Settings", "设置")), ("mods", s("Mods", "模组")),
                                                ("controls", s("Controls", "按键")), ("storage", s("Storage", "存储")),
                                                ("saves", s("Saves", "存档")), ("diag", s("Diagnostic mode", "诊断模式"))]
    // 诊断模式 (DiagPage, tools/builder_jobs/tab_diag.py)
    static let diagIntro = s("Starts the game with every diagnostic on (game log, long frames, per-frame record, pipelines) and a run folder of its own. Start game above stays without them.",
                             "用全部诊断开关启动游戏（游戏日志、长帧、逐帧记录、管线），每局一个运行目录。上面的“开始游戏”不带任何诊断。")
    static let diagStart = s("Start in diagnostic mode", "用诊断模式启动")
    static let diagShot = s("Screenshot", "截图")
    static let diagPassdump = s("Record one frame's passes", "记录一帧的通道")
    static let diagShader = s("Fragment shader (16 hex)", "片元着色器（16 位十六进制）")
    static let diagSkip = s("Leave it out + screenshot", "跳过它并截图")
    static let diagUnskip = s("Put it back + screenshot", "恢复它并截图")
    static let diagClose = s("Close the game", "关闭游戏")
    static let diagFolder = s("Open the run folder", "打开运行目录")
    static let diagLog = s("Open the game log", "打开游戏日志")
    static let diagRunning = s("Running in diagnostic mode", "诊断模式运行中")
    static let diagNotRunning = s("Not running", "没有在运行")
    static func diagSent(_ line: String) -> String { s("Sent: \(line)", "已发送：\(line)") }
    // the terminal page (TerminalTab.swift)
    static let terminal = s("AI helper", "AI问题助手")   // the games page's button to the terminal page (user 2026-11-15)
    static let askClaude = s("Ask Claude", "问 Claude")
    static let askCodex = s("Ask Codex", "问 Codex")
    static let askAI = s("Ask the AI", "让 AI 看看")
    static let sendPR = s("Send a PR to the author", "提交 PR 给作者")
    static let sendPRHelp = s("The AI turns the fix made on this Mac into a pull request and shows it to you before sending.",
                              "AI 把这台 Mac 上修好的问题整理成 PR，发之前先给你看。")
    // the user's words, 2026-11-15
    static let terminalIntro = s("Any problem, a build error, an error while the game runs and so on, can be solved here with the built-in AI helper.",
                                 "遇到任何问题，包括打包错误、游戏运行时的错误等等，这里可以用内置 AI 助手解决。")
    static let backToGames = s("Games", "游戏")
    static let homeSubtitle = s("Your games, built from your own files and running natively on this Mac.", "你的游戏，用你自己的文件打包，直接在这台 Mac 上原生运行。")
    static let findingApps = s("Looking for your built games…", "正在查找已打包的游戏…")
    static func appsScanFailed(_ e: String) -> String { s("Could not look for built games: \(e)", "查找已打包的游戏失败：\(e)") }
    /// A game's version as people write it: Switch 2 games record only the update number (Ocarina of Time: 131072,
    /// the "[v131072]" of its update package), so a number alone is shown as "v131072"; "1.4.3" stays as it is.
    static func version(_ v: String?) -> String {
        guard let v, !v.isEmpty else { return "" }
        return v.allSatisfy(\.isNumber) ? "v\(v)" : v
    }
    static let addGame = s("Add this game", "添加这个游戏")
    static func addGameHelp(_ name: String) -> String { s("Build \(name) from your own files", "用你自己的文件打包\(name)") }
    static let anyGame = s("Any game", "不限游戏")
    static func buildingFor(_ name: String) -> String { s("Build \(name)", "打包\(name)") }
    static func otherGameDropped(_ name: String) -> String { s("These files are \(name), not the game chosen on the first page.", "这些文件是\(name)，不是首页选的那个游戏。") }
    static func gameName(_ g: String) -> String {
        switch g {
        case "oot": return s("Zelda: Ocarina of Time", "塞尔达传说 时之笛")
        case "totk": return s("Zelda: Tears of the Kingdom", "塞尔达传说 王国之泪")
        case "botw": return s("Zelda: Breath of the Wild", "塞尔达传说 旷野之息")
        case "dkb": return s("Donkey Kong Bananza", "大金刚 Bananza")
        case "pkp": return s("Pokémon Pokopia", "宝可梦 Pokopia")
        default: return g
        }
    }
    static func gamePlatforms(_ g: String) -> String {
        switch g {
        case "totk": return "Mac · iPhone · iPad · Vision Pro"
        case "botw": return "Mac · iPhone · iPad"
        case "oot": return "Switch 2 · Mac"
        default: return s("Switch 2 · Mac · early", "Switch 2 · Mac · 早期")
        }
    }
    static func gameColors(_ g: String) -> [Color] {
        switch g {
        case "oot": return [Color(red: 0.16, green: 0.42, blue: 0.30), Color(red: 0.05, green: 0.18, blue: 0.14)]
        case "totk": return [Color(red: 0.20, green: 0.55, blue: 0.62), Color(red: 0.06, green: 0.20, blue: 0.30)]
        case "botw": return [Color(red: 0.35, green: 0.45, blue: 0.62), Color(red: 0.10, green: 0.14, blue: 0.24)]
        case "dkb": return [Color(red: 0.85, green: 0.42, blue: 0.12), Color(red: 0.40, green: 0.12, blue: 0.08)]
        default: return [Color(red: 0.45, green: 0.70, blue: 0.35), Color(red: 0.14, green: 0.32, blue: 0.16)]
        }
    }
    static func filesFor(_ g: String) -> String {
        switch g {
        case "totk": return s("Drop the base game (XCI or NSP) and the 1.4.3 update (NSP); an NSP base also needs your unpacked firmware folder.", "拖入游戏本体（XCI 或 NSP）和 1.4.3 更新（NSP）；本体是 NSP 时还要解开的固件文件夹。")
        case "botw": return s("Drop the 1.6.0 card image (XCI), or the base NSP with the 1.9.0 update and its DLC, and a firmware or shared-fonts folder.", "拖入 1.6.0 卡带（XCI），或本体 NSP 加 1.9.0 更新和 DLC，再加固件或系统字体文件夹。")
        case "oot": return s("Drop the Switch 2 dump folder (or the .nup package) and a shared-fonts folder.", "拖入 Switch 2 的游戏文件夹（或 .nup 包）和系统字体文件夹。")
        default: return s("Drop the Switch 2 dump folder with its updates/ folder.", "拖入带 updates/ 文件夹的 Switch 2 游戏文件夹。")
        }
    }
    static func startedBy(_ who: String) -> String { s("Started from \(who); Cancel stops it.", "从 \(who) 开始的；“停止”可以停掉它。") }
    static let notYet = s("This page is being moved here from the old window.", "这一页正在从旧窗口搬过来。")
    static let starting = s("Starting the game…", "正在启动游戏…")
    static let started = s("Started with these settings.", "已按这些设置启动。")
    static func startedWithout(_ held: [String]) -> String {
        s("Started, without: ", "已启动，但这次没用：") + held.joined(separator: ", ")
    }
    static func gpu(_ g: String) -> String {
        switch g {
        case "checking": return s("Checking the GPU for MetalFX frame interpolation…", "正在检查显卡是否支持 MetalFX 补帧…")
        case "yes": return s("This GPU has MetalFX frame interpolation.", "这块显卡支持 MetalFX 补帧。")
        case "no": return s("This GPU has no MetalFX frame interpolation: Play starts the game without it.",
                            "这块显卡不支持 MetalFX 补帧：点游玩时这次不用补帧。")
        default: return s("The GPU could not be checked: frame interpolation is used as chosen.", "没能检查显卡：补帧按所选的用。")
        }
    }
    static func updateButton(_ kind: String) -> String {
        switch kind {
        case "express": return s("Express update", "极速更新")
        case "fast": return s("Fast update", "快速更新")
        default: return s("Full build", "完整打包")
        }
    }
    static let updatedOK = s("Updated. Starting the game…", "更新完成，正在启动游戏…")
    static let shadersDone = s("Shaders and pipelines updated.", "着色器和管线更新完成。")
    static let checkShaders = s("Check shaders", "检查着色器")
    static let checkingShaders = s("Downloading the newest shader list…", "正在下载最新的着色器清单…")
    static let updateShaders = s("Update shaders", "更新着色器")
    static let quickStart = s("Quick start", "快速启动")   // the card's button and the Build page's switch: one function
    static let quickStartHelp = s("Packs in minutes. The rest are translated when the game first uses them, so the first trip into the world is slow; Fill in shaders on the game's card makes it a full app later.",
                                  "几分钟就打好。其余着色器在游戏第一次用到时现场翻译，所以第一次进世界会很慢；之后在游戏卡片上点“补全着色器”变成完整版。")
    static let quickApp = s("Quick app: only the title menu's shaders", "快速版：只带了标题菜单的着色器")
    static let fillShaders = s("Fill in shaders", "补全着色器")
    static let doneElsewhere = s("Done (started elsewhere; the game is not started from here).", "完成（这个任务是在别处开始的，这里不替它开游戏）。")
    static let workAuto = s("Automatic: MacNativeGames-work beside the apps folder (or the folder an app records)",
                            "自动：App 文件夹旁的 MacNativeGames-work（App 记录了工作目录就用它记录的）")
    static let automatic = s("Automatic", "自动")
    static func kind(_ k: String) -> String {
        switch k {
        case "express": return s("Express update (minutes)", "极速更新（几分钟）")
        case "fast": return s("Fast update (new startup shaders)", "快速更新（重做启动着色器）")
        case "full": return s("Needs a full build with its game files", "要用游戏文件完整打包")
        case "shaders": return s("Updating the shaders and pipelines", "正在更新着色器和管线")
        default: return s("Cannot be updated now", "现在不能更新")
        }
    }

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
