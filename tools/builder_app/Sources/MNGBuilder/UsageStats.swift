import CryptoKit
import Foundation
import SwiftUI

/// Active players (user 2026-11-19: "统计 每日活跃用户 … 日活跃 周活跃 月活跃 流失率"; on by default, can be turned off in the
/// settings): the Launcher's side of games/totk/runtime/mng_active.h, with the same files, so the Launcher and every game of
/// this Mac share one install id and one off switch. At most one report a day (UTC):
///   POST https://mac-native-games.com/v1/ping {"install": <32 hex>, "app": "launcher", "platform": "mac", "version": ...}
/// Nothing else is sent; the site keeps the install only as a salted hash and no address (MacNativeGamesWeb lib/active.js).
enum UsageStats {
    static let url = URL(string: "https://mac-native-games.com/v1/ping")!
    static let app = "launcher"

    /// ~/Library/Application Support/MacNativeGames (MNG_HOME: the tests), as mng_active_dir.
    static var dir: URL {
        if let home = ProcessInfo.processInfo.environment["MNG_HOME"], !home.isEmpty { return URL(fileURLWithPath: home) }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/MacNativeGames")
    }
    static var offFile: URL { dir.appendingPathComponent("usage-stats-off") }
    static var idFile: URL { dir.appendingPathComponent("install-id.txt") }
    static var sentFile: URL { dir.appendingPathComponent("usage-sent").appendingPathComponent("\(app).txt") }

    static var on: Bool {
        get { !FileManager.default.fileExists(atPath: offFile.path) }
        set {
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            if newValue { try? FileManager.default.removeItem(at: offFile) }
            else { try? Data("off\n".utf8).write(to: offFile) }
        }
    }

    static func isID(_ s: String) -> Bool { s.count == 32 && s.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) } }

    /// The shared install id, made by whichever program comes first and whole when it appears: written to a file of
    /// this program's own, then linked to the name, which fails when another program linked its id first (as
    /// mng_active_install).
    static func install() -> String? {
        for pass in 0..<2 {
            if let v = try? String(contentsOf: idFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines), isID(v) { return v }
            if pass == 1 { return nil }
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let id = SymmetricKey(size: .bits128).withUnsafeBytes { Data($0) }.map { String(format: "%02x", $0) }.joined()
            let mine = dir.appendingPathComponent(".install-id.\(getpid()).\(id)").path
            let fd = open(mine, O_WRONLY | O_CREAT | O_EXCL, 0o644)
            guard fd >= 0 else { continue }
            let w = id.utf8CString.withUnsafeBufferPointer { write(fd, $0.baseAddress, 32) }
            close(fd)
            let linked = w == 32 && link(mine, idFile.path) == 0
            unlink(mine)
            if linked { return id }
        }
        return nil
    }

    static func today() -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX"); f.timeZone = TimeZone(identifier: "UTC"); f.dateFormat = "yyyy-MM-dd"
        return f.string(from: Date())
    }

    /// The report's body, or nil: off, already sent today, no id.
    static func body(version: String, day: String) -> Data? {
        guard on, (try? String(contentsOf: sentFile, encoding: .utf8)) != day, let id = install() else { return nil }
        return try? JSONSerialization.data(withJSONObject: ["install": id, "app": app, "platform": "mac", "version": version])
    }

    /// Once at the start, ten seconds in, off the main thread; a failure is tried again at the next start.
    static func start() {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 10) {
            let day = today()
            guard let data = body(version: version, day: day) else { return }
            var r = URLRequest(url: url, timeoutInterval: 20)
            r.httpMethod = "POST"; r.httpBody = data
            r.setValue("application/json", forHTTPHeaderField: "Content-Type")
            URLSession.shared.dataTask(with: r) { _, response, error in
                guard error == nil, let code = (response as? HTTPURLResponse)?.statusCode, (200..<300).contains(code) else { return }
                try? FileManager.default.createDirectory(at: sentFile.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? Data(day.utf8).write(to: sentFile, options: .atomic)
            }.resume()
        }
    }
}

/// MNG Launcher > Settings… (⌘,): the one switch, for the Launcher and every game of this Mac.
struct UsageStatsSettings: View {
    @State private var on = UsageStats.on

    var body: some View {
        Form {
            Toggle(T.usageStats, isOn: Binding(get: { on }, set: { on = $0; UsageStats.on = $0 }))
            Text(T.usageStatsHelp).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(width: 460)
    }
}
