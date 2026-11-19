import Foundation

/// The Cover Flow's frame-by-frame record, off unless MNG_COVERFLOW_TRACE=1 (user 2026-11-17: "动画结束的时候 会有一次
/// 硬跳 调整"; a jump is found in the animated values, not guessed). Every frame the stage draws writes its animated
/// values to stderr with the time; the pointer's and clicks' decisions are written as notes.
enum CoverFlowTrace {
    static let on = ProcessInfo.processInfo.environment["MNG_COVERFLOW_TRACE"] == "1"
    private static let start = Date()

    static func frame(position: Double, flip: Double, lit: Double, tilt: (Double, Double), pulls: CaseAmounts, order: [Int]) {
        guard on else { return }
        let p = pulls.v.map { String(format: "%.3f", $0) }.joined(separator: ",")
        write(String(format: "frame pos %.4f flip %.3f lit %.3f tilt %.3f,%.3f pulls [%@] order %@",
                     position, flip, lit, tilt.0, tilt.1, p, order.map(String.init).joined(separator: ",")))
    }

    /// The middle case drawn leaning (MiddleLean): its own line, the stage is not drawn for it.
    static func lean(_ x: Double, _ y: Double, _ lit: Double) {
        guard on else { return }
        write(String(format: "lean %.3f,%.3f lit %.3f", x, y, lit))
    }

    static func note(_ text: String) {
        guard on else { return }
        write("note " + text)
    }

    private static func write(_ line: String) {
        let t = Date().timeIntervalSince(start)
        FileHandle.standardError.write(Data(String(format: "%9.4f ", t).appending(line + "\n").utf8))
    }
}
