import Foundation
import SwiftUI

/// The texts and colours the game cases show, shared by the Mac Launcher and the iPhone / iPad Launcher
/// (tools/ios_launcher copies this file with CoverFlow.swift, CaseDetail.swift, CaseAmounts.swift, CoverArt.swift and
/// CoverFlowTrace.swift): the same cases on both. Only Foundation and SwiftUI here.
/// The games both Launchers show: the same cases in the same order on the Mac and the iPhone / iPad (user 2026-11-19:
/// "统一 b 两边数量一样", "顺序也一样"), one list here, read by HomePage.games and the iPhone's Games.all. A game
/// without an iPhone / iPad build shows there too, its platforms line naming where it plays, and no start button.
enum GameCatalog {
    static let all = ["oot", "totk", "botw", "eow", "odyssey", "acnh", "dkb", "pkp", "dread", "tmnt"]
    /// The ones with an iPhone / iPad build in this repository (games/totk/ios: totk, botw, dkb, pkp; games/oot/ios;
    /// games/dread/ios; games/tmnt/ios): the iPhone Launcher looks for their apps (their URL schemes).
    static let onIPhone = ["oot", "totk", "botw", "dkb", "pkp", "dread", "tmnt"]
}

extension T {
    static let zh = (Locale.preferredLanguages.first ?? "en").hasPrefix("zh")
    static func s(_ en: String, _ cn: String) -> String { zh ? cn : en }

    #if os(macOS)
    static let turnBack = s("Click outside or press Esc to turn it back", "点盒子外面或按 Esc 翻回")
    #else
    static let turnBack = s("Tap outside to turn it back", "点盒子外面翻回")
    #endif

    static func gameName(_ g: String) -> String {
        switch g {
        case "oot": return s("Zelda: Ocarina of Time", "塞尔达传说 时之笛")
        case "totk": return s("Zelda: Tears of the Kingdom", "塞尔达传说 王国之泪")
        case "botw": return s("Zelda: Breath of the Wild", "塞尔达传说 旷野之息")
        case "dkb": return s("Donkey Kong Bananza", "大金刚 Bananza")
        case "pkp": return s("Pokémon Pokopia", "宝可梦 Pokopia")
        case "acnh": return s("Animal Crossing: New Horizons", "集合啦！动物森友会")
        case "eow": return s("Zelda: Echoes of Wisdom", "塞尔达传说 智慧的再现")
        case "odyssey": return s("Super Mario Odyssey", "超级马力欧 奥德赛")
        case "dread": return s("Metroid Dread", "密特罗德 生存恐惧")
        case "tmnt": return s("TMNT: Shredder's Revenge", "忍者神龟：施莱德的复仇")
        default: return g
        }
    }
    static func gamePlatforms(_ g: String) -> String {
        switch g {
        case "totk": return "Mac · iPhone · iPad · Vision Pro"
        case "botw": return "Mac · iPhone · iPad"
        case "oot": return "Switch 2 · Mac"
        case "acnh": return s("Mac · experimental", "Mac · 实验性")
        case "eow": return s("Mac · early", "Mac · 早期")
        case "odyssey": return s("Mac · early", "Mac · 早期")
        case "dread": return "Mac · iPhone · iPad"
        case "tmnt": return s("Mac · iPad · early", "Mac · iPad · 早期")
        default: return s("Switch 2 · Mac · early", "Switch 2 · Mac · 早期")
        }
    }
    static func gameColors(_ g: String) -> [Color] {
        switch g {
        case "oot": return [Color(red: 0.16, green: 0.42, blue: 0.30), Color(red: 0.05, green: 0.18, blue: 0.14)]
        case "totk": return [Color(red: 0.20, green: 0.55, blue: 0.62), Color(red: 0.06, green: 0.20, blue: 0.30)]
        case "botw": return [Color(red: 0.35, green: 0.45, blue: 0.62), Color(red: 0.10, green: 0.14, blue: 0.24)]
        case "dkb": return [Color(red: 0.85, green: 0.42, blue: 0.12), Color(red: 0.40, green: 0.12, blue: 0.08)]
        case "odyssey": return [Color(red: 0.86, green: 0.20, blue: 0.18), Color(red: 0.30, green: 0.06, blue: 0.10)]
        case "eow": return [Color(red: 0.55, green: 0.42, blue: 0.78), Color(red: 0.16, green: 0.12, blue: 0.30)]
        case "acnh": return [Color(red: 0.36, green: 0.72, blue: 0.62), Color(red: 0.10, green: 0.32, blue: 0.30)]
        case "dread": return [Color(red: 0.88, green: 0.45, blue: 0.10), Color(red: 0.30, green: 0.08, blue: 0.06)]
        case "tmnt": return [Color(red: 0.30, green: 0.66, blue: 0.22), Color(red: 0.08, green: 0.22, blue: 0.10)]
        default: return [Color(red: 0.45, green: 0.70, blue: 0.35), Color(red: 0.14, green: 0.32, blue: 0.16)]
        }
    }
}
