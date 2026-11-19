import Foundation
import UIKit

/// The iPhone / iPad Launcher's texts. The Mac Launcher's `enum T` is in its Strings.swift (Mac-only); the games' names,
/// platforms, colours and `s` come from the shared GameStrings.swift (install.py copies it in), an extension of this.
enum T {
    static let homeSubtitle = s("Your games on this device", "这台设备上的游戏")
    static let play = s("Start game", "启动游戏")   // the Mac's words (Strings.swift startGame)
    static let installed = s("Installed on this device", "已装在这台设备上")
    static let notInstalled = s("Not on this device", "还没装到这台设备")
    static let howToInstall = s("Build it in MNG Launcher on your Mac (its iPhone / iPad version) and install it with the cable; it shows up here.",
                                "在 Mac 上的 MNG Launcher 打包它的 iPhone / iPad 版，用数据线装上，这里就会出现。")
    static let tapToTurn = s("Tap the case to turn it over", "点盒子翻到背面")
    static func couldNotOpen(_ name: String) -> String { s("Could not open \(name)", "没能打开\(name)") }
}

/// The games the iPhone / iPad Launcher shows, and the URL scheme each game's app answers (games/*/ios/tools/build_ipa.py
/// url_scheme puts it in the app's Info.plist; project.yml's LSApplicationQueriesSchemes lists every one, else
/// canOpenURL says no: tests/test_ios_launcher.py). The Launcher cannot build, install or read another app: it only sees
/// whether the scheme opens, and opens it.
enum Games {
    /// Every game with an iPhone / iPad build in this repository (games/totk/ios: totk, botw, dkb, pkp; games/oot/ios;
    /// games/dread/ios; games/tmnt/ios), in the Mac Launcher's order (HomePage.games), then the iPhone-only ones
    static let all = ["oot", "totk", "botw", "dkb", "pkp", "dread", "tmnt"]

    static func url(_ game: String) -> URL { URL(string: "mng-\(game)://launch")! }

    /// The game's picture, put into this app by its install from the player's own Mac app of the game
    /// (install.py: Covers/<game>.png); nil when that game was not built on the Mac.
    static func picturePath(_ game: String) -> String? {
        Bundle.main.path(forResource: game, ofType: "png", inDirectory: "Covers")
    }
}
// The pictures are decoded and blurred once by CoverImages, shared with the Mac Launcher (Shared/CoverArt.swift).
