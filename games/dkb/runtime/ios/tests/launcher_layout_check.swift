// The Mac settings window (dm/mac/Launcher.swift), run by test_launcher_storage.py: measure the real grouped Form without creating or showing a window.
import AppKit
import SwiftUI

func checkLauncherLayout(_ model: LauncherModel, defaults: UserDefaults) -> Int {
    NSApplication.shared.setActivationPolicy(.prohibited)
    var failed = 0
    func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
    func measure(_ advanced: Bool, width: CGFloat) -> CGFloat {
        defaults.set(advanced, forKey: "window.advanced")
        var extra: CGFloat?
        let view = NSHostingView(rootView: LauncherView(m: model, done: { _ in }, fit: { extra = $0 }).defaultAppStorage(defaults))
        view.sizingOptions = []
        // SwiftUI reports the Form's scroll geometry on a later pass, an estimate first and the measured rows a few ms after it, later still on a slow runner (issue #445): run the
        // loop until the report meets the step's expectation and has held for 0.2 s; after 5 s the check sees whatever was reported last
        func settle(_ want: (CGFloat) -> Bool) {
            let limit = Date().addingTimeInterval(5)
            var held = extra, since = Date()
            while Date() < limit {
                view.layoutSubtreeIfNeeded()
                RunLoop.current.run(until: Date().addingTimeInterval(0.01))
                if extra != held { held = extra; since = Date() } else if let e = extra, want(e), Date().timeIntervalSince(since) >= 0.2 { return }
            }
        }
        view.frame = NSRect(x: 0, y: 0, width: width, height: 660)
        settle { _ in true }
        guard let delta = extra else { expect(false, "native Form reports its scroll content size"); return 0 }
        let height = 660 + delta
        view.frame.size.height = height
        settle { abs($0) < 1 }
        expect(abs(extra ?? .infinity) < 1, "native Form fits its measured height (advanced=\(advanced), width=\(width), height=\(height))")
        defaults.set(!advanced, forKey: "window.advanced")
        settle { advanced ? $0 < 0 : $0 > 0 }
        expect(advanced ? (extra ?? 0) < 0 : (extra ?? 0) > 0, "the same Form reports expansion and collapse after its saved state changes")
        view.frame.size.height += extra ?? 0
        settle { abs($0) < 1 }
        expect(abs(extra ?? .infinity) < 1, "the same Form fits after its saved state changes")
        return height
    }
    for width: CGFloat in [520, 560] {
        // each measurement in its own pool: a hosting view left autoreleased stays alive and lays out its Form again on every later change of window.advanced
        let closed = autoreleasepool { measure(false, width: width) }, open = autoreleasepool { measure(true, width: width) }
        expect(closed > 0 && open > closed, "Advanced expands the measured native content")
    }
    let visible = NSRect(x: -1920, y: 40, width: 1920, height: 900)
    let initial = NSRect(x: -1500, y: 540, width: 560, height: 400)
    let expanded = LauncherWindowLayout.fitted(initial, extraHeight: 300, visible: visible)
    let capped = LauncherWindowLayout.fitted(initial, extraHeight: 1200, visible: visible)
    let collapsed = LauncherWindowLayout.fitted(expanded, extraHeight: -300, visible: visible)
    expect(expanded.maxY == initial.maxY && expanded.height == 700, "expansion preserves the top edge")
    expect(capped == NSRect(x: -1500, y: 40, width: 560, height: 900), "tall content stops at the visible screen frame")
    expect(collapsed == initial, "collapse restores the content height and top edge")
    return failed
}
