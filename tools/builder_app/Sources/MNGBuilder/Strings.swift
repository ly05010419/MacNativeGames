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
    /// Step 3's place (user 2026-11-17: "让用户 自己选择位置 不要有默认位置 … 自动生成app目录和build目录").
    static let location = s("Location", "位置")
    static let chooseLocation = s("Choose a Location…", "选择位置…")
    static let locationNone = s("Not chosen yet", "还没选")
    static let locationHint = s("Choose a folder for this game. The app goes into its app folder, the build's work files into its build folder; both are made when the build starts.",
                                "选一个文件夹放这个游戏：App 放进它里面的 app 文件夹，打包的中间文件放进 build 文件夹，开始打包时自动建好。")
    static let locationNeeded = s("Step 3: choose a location first", "第三步：先选一个位置")
    static func locationParts(_ app: String, _ work: String) -> String { zh ? "App：\(app)\n中间文件：\(work)" : "App: \(app)\nWork files: \(work)" }
    static func updatesInPlace(_ app: String) -> String {
        zh ? "这个游戏已经装在 \(app)，会原地更新，用它自己记录的工作目录；不用选位置。" : "This game is installed at \(app) and is updated where it is, with the work folder it records; no location needed."
    }
    /// The Launcher's version, bottom right of the window (user 2026-11-17: "launcher 右下角 显示 版本号").
    static func version(_ v: String) -> String { "MNG Launcher \(v)" }
    static func versionHelp(_ commit: String) -> String { zh ? "自带代码提交 \(commit)" : "Bundled code commit \(commit)" }
    static let thisMac = s("This Mac", "这台 Mac")
    static let fix = s("Fix", "修复")
    static let recheck = s("Check Again", "重新检查")
    static let blocked = s("Not ready", "还不能打包")
    static let ready = s("Ready to build", "可以打包")
    static let building = s("Building", "正在打包")
    static let doneOK = s("Done. The game was opened.", "打包完成，已打开游戏。")   // play() does not wait: no "…ing" left on the page
    static let failed = s("The build stopped", "打包停下了")
    static let cancelled = s("Cancelled. Build again to go on where it stopped.", "已停止。再按一次打包会从停下的地方接着做。")
    static let cancelling = s("Stopping…", "正在停止…")
    static let log = s("Log", "日志")
    static let firstBuild = s("The first build of this kind on this Mac: how many steps and how long are not known yet.",
                              "这台 Mac 第一次做这种打包：还不知道一共几步、要多久。")
    static let noRepo = s("This app does not know its MacNativeGames folder: make it again with tools/builder_app/make_app.py.",
                          "这个 App 不知道 MacNativeGames 源码在哪：用 tools/builder_app/make_app.py 重新生成它。")
    static let existing = s("Already built", "已经打包过")
    /// A built game's own pages, opened under its card (Tears of the Kingdom and Breath of the Wild: tools/builder's games).
    static let gamePages: [(String, String)] = [("settings", s("Settings", "设置")), ("mods", s("Mods", "模组")),
                                                ("controls", s("Controls", "按键")), ("storage", s("Storage", "存储")),
                                                ("saves", s("Saves", "存档"))]
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
    static let updatedOK = s("Updated. The game was opened.", "更新完成，已打开游戏。")
    static let shadersDone = s("Shaders and pipelines updated.", "着色器和管线更新完成。")
    static let checkShaders = s("Check shaders", "检查着色器")
    static let checkingShaders = s("Downloading the newest shader list…", "正在下载最新的着色器清单…")
    static let updateShaders = s("Update shaders", "更新着色器")
    static let quickStart = s("Quick start", "快速启动")   // the card's button and the Build page's switch: one function
    static let quickStartHelp = s("Packs in minutes with the title menu's shaders and every compute, tile-read and mesh pipeline. The rest are translated when the game first uses them, so the first trip into the world is slow; Fill in shaders on the game's card makes it a full app later. A quick app always starts with every diagnostic on (game log, long frames, per-frame record, pipelines), in a run folder under Logs/MacNativeGames/diagnostics.",
                                  "几分钟就打好：带标题菜单的着色器和全部计算、读帧缓冲、网格管线。其余着色器在游戏第一次用到时现场翻译，所以第一次进世界会很慢；之后在游戏卡片上点“补全着色器”变成完整版。快速版启动时总是带全部诊断（游戏日志、长帧、逐帧记录、管线），记录在 Logs/MacNativeGames/diagnostics 下每局一个目录。")
    static let quickApp = s("Quick app: the title menu's shaders and the compute, tile-read and mesh pipelines; starts with every diagnostic on",
                            "快速版：带标题菜单的着色器和计算、读帧缓冲、网格管线；启动时带全部诊断")
    static let fillShaders = s("Fill in shaders", "补全着色器")
    // the card's memory saver switch (user 2026-11-16: "CheckBox 叫 开启省内存模式"; tools/builder_jobs/memory_saver.py)
    static let memorySaver = s("Memory saver mode", "开启省内存模式")
    static let memorySaverHelp = s("Off (the default): every pipeline stays in memory after the warm-up, so nothing is fetched while you play. On: only the title menu's pipelines and every compute, mesh and tile-read pipeline stay; the rest come from the pipeline archive the first time they are drawn (a short hitch each time), and the memory saved shrinks the further you play. Counts from the game's next start; you can change it while the game runs.",
                                   "关（默认）：预热后全部管线常驻内存，游玩中不用临时从存档取。开：只常驻标题菜单的管线和全部计算、网格、贴图读取管线，其余第一次画到时从管线存档取（每次会小卡一下），玩得越远省下的内存越少。下次启动游戏时生效，游戏开着也能改。")
    static let memorySaverNext = s("Counts from the game's next start.", "下次启动游戏时生效。")
    // the card's diagnostics switch (user 2026-11-16: "诊断模式 也改成 CheckBox， 正常启动 可以带上 诊断模式")
    static let diagnostics = s("Diagnostic mode", "诊断模式")
    static let diagnosticsHelp = s("On: the normal start also writes every diagnostic a quick app always has (game log, long frames, per-frame record, pipelines), in a run folder under Logs/MacNativeGames/diagnostics (the newest 5 kept). They cost a little speed; leave it off for normal play. Counts from the game's next start.",
                                   "开：正常启动也带上快速版总带的全部诊断（游戏日志、长帧、逐帧记录、管线），记录在 Logs/MacNativeGames/diagnostics 下每局一个目录（留最近 5 个）。会稍微拖慢游戏，平时玩请关着。下次启动游戏时生效。")
    static func memorySaverUnknown(_ word: String) -> String {
        s("memory-saver.txt says \"\(word)\", which the game reads as off", "memory-saver.txt 里写的是“\(word)”，游戏按关处理")
    }
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

    /// "Stage 2 of 4: Extracting the game's data" (build_app.py's 'i/4' lines; Ocarina of Time and updates have none).
    static func stageOf(_ i: Int, _ n: Int, _ name: String) -> String {
        zh ? "第 \(i) 阶段，共 \(n) 阶段：\(name)" : "Stage \(i) of \(n): \(name)"
    }

    /// "Step 3: Translating the shaders (2 done; 7 steps last time)". A step is a tool the build ran (its log in the work
    /// folder's logs/); the total is this Mac's last build of the same kind, which may skip or add a step this time.
    static func stepLine(_ i: Int, _ last: Int?, _ what: String) -> String {
        let done = i - 1
        if zh {
            var more = done > 0 ? "已完成 \(done) 步" : ""
            if let n = last { more += (more.isEmpty ? "" : "，") + "上次共 \(n) 步" }
            return "现在第 \(i) 步：\(what)" + (more.isEmpty ? "" : "（\(more)）")
        }
        var more = done > 0 ? ["\(done) done"] : []
        if let n = last { more.append("\(n) steps last time") }
        return "Step \(i): \(what)" + (more.isEmpty ? "" : " (\(more.joined(separator: "; ")))")
    }

    /// "400 / 42848 shaders": the running step's own counter.
    static func counted(_ done: Int, _ total: Int, _ unit: String) -> String {
        "\(done) / \(total)" + (unit.isEmpty ? "" : " " + unit)
    }

    /// What a step does, from its log's name (the names build_app.py, update_runtime.py and games/*/build.py give their
    /// logs); an unknown one shows its name.
    static func stepName(_ log: String) -> String {
        let rules: [(String, String, String)] = [   // prefix, English, 中文; the first prefix that matches
            ("native-runtime", "Compiling the game's runtime", "编译游戏运行时"),
            ("runtime-probe", "Checking the runtime", "检查运行时"),
            ("runtime", "Compiling the game's runtime", "编译游戏运行时"),
            ("ns2-corpus", "Collecting the game's shaders", "收集游戏的着色器"),
            ("prepare-shader-seed", "Translating the shaders", "翻译着色器"),
            ("prepare-ns2-seed", "Translating the shaders", "翻译着色器"),
            ("ns2-seed", "Translating the shaders", "翻译着色器"),
            ("compile-shader-libraries", "Compiling the shader libraries", "编译着色器库"),
            ("shader-libs", "Compiling the shader libraries", "编译着色器库"),
            ("shader-list-resolve", "Sorting the warm-up lists", "整理预热清单"),
            ("compute-pipelines", "Building the pipeline archive", "建管线存档"),
            ("mac-pipelines", "Building the pipeline archive", "建管线存档"),
            ("warm-metal-cache", "Warming Metal's cache", "预热 Metal 缓存"),
            ("sign-pipelines", "Signing the pipeline archive", "签名管线存档"),
            ("verify-pipelines", "Checking the pipeline archive", "核对管线存档"),
            ("sign-shaders", "Signing the shader data", "签名着色器数据"),
            ("verify-shaders", "Checking the shader data", "核对着色器数据"),
            ("sign-data", "Signing the game data", "签名游戏数据"),
            ("verify-data", "Checking the game data", "核对游戏数据"),
            ("sign-app", "Signing the app", "签名 App"),
            ("register-app", "Registering the app", "登记 App"),
            ("import-original", "Importing the game files", "导入游戏文件"),
            ("import_dump", "Importing the game files", "导入游戏文件"),
            ("extract_romfs", "Extracting the game data", "解出游戏数据"),
            ("update-romfs", "Merging the update's data", "合并更新的数据"),
            ("verify-original-modules", "Checking the game's code", "核对游戏代码"),
            ("prepare_modules", "Checking the game's code", "核对游戏代码"),
            ("verify-rewrite", "Checking the rewritten code", "核对改写后的代码"),
            ("rewrite", "Rewriting the game's code", "改写游戏代码"),
            ("prepare_runtime", "Preparing the game's tables", "准备游戏数据表"),
            ("prepare_romfs_backend", "Preparing the game data index", "准备游戏数据索引"),
            ("prepare-shared-fonts", "Preparing the system fonts", "准备系统字体"),
            ("shared_fonts", "Preparing the system fonts", "准备系统字体"),
            ("app-icon", "Making the app icon", "做 App 图标"),
            ("app_icon", "Making the app icon", "做 App 图标"),
            ("python-", "Preparing Python", "准备 Python 环境"),
            ("venv", "Preparing Python", "准备 Python 环境"),
            ("pip", "Preparing Python", "准备 Python 环境"),
            ("sources", "Copying the build scripts", "复制打包脚本"),
            ("fetch-", "Downloading a tool", "下载工具"),
            ("hactool", "Building a tool", "编译工具"),
            ("output_res_overlay", "Making the resolution patch", "做分辨率补丁"),
            ("romfs_patches", "Making the game data patches", "做游戏数据补丁"),
            ("launcher", "Compiling the launcher", "编译启动器"),
        ]
        guard let r = rules.first(where: { log.hasPrefix($0.0) }) else { return log }
        return zh ? r.2 : r.1
    }

    /// What a step's counter counts.
    static func stepUnit(_ log: String) -> String {
        if ["prepare-shader-seed", "prepare-ns2-seed", "ns2-seed", "ns2-corpus"].contains(where: log.hasPrefix) { return s("shaders", "个着色器") }
        if ["compile-shader-libraries", "shader-libs"].contains(where: log.hasPrefix) { return s("shader libraries", "个着色器库") }
        if ["compute-pipelines", "mac-pipelines", "warm-metal-cache"].contains(where: log.hasPrefix) { return s("pipelines", "条管线") }
        return ""
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
    /// The running step's own time left, from its counter's rate (user 2026-11-17: the estimate "没有根据 真实情况来").
    static func stepLeft(_ text: String) -> String { zh ? "这一步还剩\(text)" : "this step: \(text) left" }
    static func elapsed(_ text: String) -> String { zh ? "已用 \(text)" : "\(text) elapsed" }
}
