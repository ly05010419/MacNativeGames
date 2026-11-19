import Darwin
import QuartzCore
import UIKit

/// Every 2 s, while MNG_COVERFLOW_TRACE=1: how much memory the Launcher holds, how warm the phone is, and how many
/// screen refreshes went by without a new frame from it (user 2026-11-19: "越用越慢 刚开始还好 用一会 就慢了"; its own
/// work stayed the same for two minutes, so what grows over a longer use is recorded, not guessed).
@MainActor
final class Health: NSObject {
    static let shared = Health()
    private var link: CADisplayLink?
    private var last: CFTimeInterval = 0
    private var since: CFTimeInterval = 0
    private var frames = 0, dropped = 0
    private var worst: CFTimeInterval = 0

    func start() {
        guard CoverFlowTrace.on, link == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.add(to: .main, forMode: .common)
        self.link = link
        since = CACurrentMediaTime()
    }

    @objc private func tick(_ link: CADisplayLink) {
        let now = link.timestamp
        if last > 0 {
            let gap = now - last
            let refresh = link.duration > 0 ? link.duration : 1.0 / 60
            frames += 1
            if gap > refresh * 1.5 { dropped += Int((gap / refresh).rounded()) - 1 }
            worst = max(worst, gap)
        }
        last = now
        guard now - since >= 2 else { return }
        CoverFlowTrace.note(String(format: "health footprint %.0f MB thermal %@ display %d frames, %d dropped, worst %.1f ms",
                                   Self.footprint(), Self.thermal(), frames, dropped, worst * 1000))
        since = now; frames = 0; dropped = 0; worst = 0
    }

    /// The memory the system counts against the app (what it is ended for when too much), in MB.
    private static func footprint() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        return result == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }

    private static func thermal() -> String {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: return "nominal"
        case .fair: return "fair"
        case .serious: return "serious"
        case .critical: return "critical"
        @unknown default: return "unknown"
        }
    }
}
