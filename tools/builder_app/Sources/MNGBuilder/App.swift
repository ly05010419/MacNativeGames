import AppKit
import SwiftUI

@main
struct BuilderApp: App {
    @NSApplicationDelegateAdaptor(Delegate.self) var delegate
    @StateObject private var model = Model()

    var body: some Scene {
        Window(T.title, id: "main") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 640, minHeight: 620)
                .overlay(alignment: .bottomTrailing) { VersionLabel() }
        }
        .windowResizability(.contentMinSize)
    }
}

/// The Launcher's version at the window's bottom right (user 2026-11-17: "launcher 右下角 显示 版本号"), from its
/// Info.plist (make_app.py: the release's --version); the bundled code's commit shows on hover.
struct VersionLabel: View {
    private let info = Bundle.main.infoDictionary ?? [:]

    var body: some View {
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let commit = (info["MNGSourceCommit"] as? String).map { String($0.prefix(8)) } ?? "?"
        Text(T.version(version))
            .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(.regularMaterial, in: Capsule())
            .padding(8)
            .help(T.versionHelp(commit))
            .accessibilityIdentifier("launcher-version")
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
        NSApp.setActivationPolicy(.regular)   // a bare executable (swift run) gets a Dock icon and focus too
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
