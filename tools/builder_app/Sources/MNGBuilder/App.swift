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
        }
        .windowResizability(.contentMinSize)
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
