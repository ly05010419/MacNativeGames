import AppKit
import SwiftUI

@main
struct BuilderApp: App {
    @NSApplicationDelegateAdaptor(Delegate.self) var delegate
    @StateObject private var model = Model()

    var body: some Scene {
        Window(T.title, id: "main") {
            ContentView()
                .frame(minWidth: 640, minHeight: 620)
                .overlay(alignment: .bottomTrailing) { VersionLabel() }
                .environmentObject(model)   // after the overlay: the version label reads the model too
        }
        .windowResizability(.contentMinSize)
        Settings { UsageStatsSettings() }   // MNG Launcher > Settings… (UsageStats.swift)
    }
}

/// The Launcher's version at the window's bottom right (user 2026-11-17: "launcher 右下角 显示 版本号"), from its
/// Info.plist (make_app.py: the release's --version); the bundled code's commit shows on hover.
struct VersionLabel: View {
    @EnvironmentObject var model: Model
    private let info = Bundle.main.infoDictionary ?? [:]

    var body: some View {
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let commit = (info["MNGSourceCommit"] as? String).map { String($0.prefix(8)) } ?? "?"
        // a newer Launcher is out: the label turns blue and its click downloads it (a player had no other way to know)
        if let newer = model.newerLauncher {
            Button { NSWorkspace.shared.open(newer.url) } label: {
                Label(T.versionNewer(version, newer.version), systemImage: "arrow.down.circle.fill")
                    .font(.caption2.monospacedDigit()).foregroundStyle(.white)
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(Color.accentColor, in: Capsule())
                    .contentShape(Capsule())
            }
            .buttonStyle(.plain)
            .padding(8)
            .help(T.versionNewerHelp)
            .accessibilityIdentifier("launcher-version")
        } else {
        // Only a release has a version (user 2026-11-17: "版本号 不对？？？" — a Launcher built from a checkout said
        // 1.0.0); one built here says it is a development build and of which commit.
        Text(Paths.isRelease ? T.version(version) : T.devVersion(commit))
            .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(.regularMaterial, in: Capsule())
            .padding(8)
            .help(T.versionHelp(commit))
            .accessibilityIdentifier("launcher-version")
        }
    }
}

extension Notification.Name {
    static let mngOpenFiles = Notification.Name("MNGOpenFiles")
}

final class Delegate: NSObject, NSApplicationDelegate {
    /// Files opened before the window's model exists (open -a "MNG Launcher" <files> starts the app with them).
    static var pending: [URL] = []

    func application(_ application: NSApplication, open urls: [URL]) {
        Delegate.pending += urls
        NotificationCenter.default.post(name: .mngOpenFiles, object: urls)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // dark always, whatever the Mac's own setting (user 2026-11-19: "默认 改成 黑色背景 不用 白色背景"): the window,
        // Settings and the alerts alike
        NSApp.appearance = NSAppearance(named: .darkAqua)
        NSApp.setActivationPolicy(.regular)   // a bare executable (swift run) gets a Dock icon and focus too
        NSApp.activate(ignoringOtherApps: true)
        UsageStats.start()   // the day's report of active players, unless turned off (UsageStats.swift)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
