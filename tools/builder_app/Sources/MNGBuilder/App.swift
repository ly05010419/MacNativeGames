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

final class Delegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)   // a bare executable (swift run) gets a Dock icon and focus too
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
