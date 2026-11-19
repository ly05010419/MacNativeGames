import CoreMotion
import SwiftUI
import UIKit

/// How the phone is turned, as the Mac's pointer over the middle case (user 2026-11-19: "mac 的 鼠标 在封面上的 效果
/// （关闭的时候） 放在iphone上 用 陀螺仪 模拟"): -1...1 across and down the case, like the pointer's place on it. Turned
/// to the right (its right side going away), the case leans as under a pointer on its right; its top going away, as
/// under a pointer at its top. The rest it is measured from follows the phone slowly (about two seconds), so a phone
/// held still at any angle shows the case straight, and only a turn leans it.
@MainActor
final class MotionTilt {
    /// Not published: each reading is sent to the Cover Flow (feed), which redraws the middle case only; a published
    /// value redrew the whole home page 65 times a second (user 2026-11-19: "越用越慢").
    private(set) var tilt = CGPoint.zero
    let feed = TurnFeed()

    private let manager = CMMotionManager()
    private var rest: CMQuaternion?
    /// A turn of this many radians (about 9°) is the case's edge, and the lean goes on to half again as far as the Mac's
    /// pointer can take it (user 2026-11-19: "iphone 上 效果 明显一点"; at 17° and no further it hardly showed).
    private static let full = 0.15
    private static let most = 1.5
    /// How much of the way to the phone's attitude the rest moves each update (60 a second): about 4 s to settle.
    private static let follow = 0.012

    func start() {
        guard manager.isDeviceMotionAvailable, !manager.isDeviceMotionActive else { return }
        manager.deviceMotionUpdateInterval = 1.0 / 60
        manager.startDeviceMotionUpdates(to: .main) { [weak self] motion, _ in
            guard let motion else { return }
            MainActor.assumeIsolated { self?.update(motion.attitude.quaternion) }
        }
    }

    func stop() {
        manager.stopDeviceMotionUpdates()
        rest = nil
        tilt = .zero
        feed.send(.zero)
    }

    private func update(_ now: CMQuaternion) {
        guard let from = rest else { rest = now; return }
        // the turn from the rest to now, in the phone's own axes (x right, y up, z out of the screen when upright)
        var turn = Self.multiply(Self.conjugate(from), now)
        if turn.w < 0 { turn = CMQuaternion(x: -turn.x, y: -turn.y, z: -turn.z, w: -turn.w) }
        let about = (x: 2 * turn.x, y: 2 * turn.y)   // small angles: the rotation's axis times its angle
        // the screen's axes in the phone's, as the screen is held (landscape only)
        let homeRight = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.interfaceOrientation != .landscapeLeft
        let aboutUp = homeRight ? about.x : -about.x          // turning about the screen's upright axis
        let aboutAcross = homeRight ? -about.y : about.y      // turning about its left-to-right axis
        let clamp = { (v: Double) in CGFloat(max(-Self.most, min(Self.most, v / Self.full))) }
        // a change too small to see (under 0.002 of a full lean: a hundredth of a point) is not passed on: the sensor's
        // jitter redrew the whole stage at 60 a second with the phone held still ("ab 都做", A)
        var next = CGPoint(x: clamp(aboutUp), y: clamp(aboutAcross))
        // the rest has all but caught up with the phone: the case straight, and nothing more sent until it turns again
        // (it crept on in ever smaller steps, each one a redraw: "基准角度追平以后就停住")
        if abs(next.x) < 0.01, abs(next.y) < 0.01 { next = .zero }
        if next != tilt, next == .zero || abs(next.x - tilt.x) >= 0.002 || abs(next.y - tilt.y) >= 0.002 {
            tilt = next
            feed.send(next)
            // the time each reading arrives, to measure how long it takes to show (the lean lines, same precision)
            CoverFlowTrace.note(String(format: "motion %.3f,%.3f", tilt.x, tilt.y))
        }
        rest = Self.toward(from, now, Self.follow)
    }

    private static func conjugate(_ q: CMQuaternion) -> CMQuaternion { CMQuaternion(x: -q.x, y: -q.y, z: -q.z, w: q.w) }

    private static func multiply(_ a: CMQuaternion, _ b: CMQuaternion) -> CMQuaternion {
        CMQuaternion(x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
    }

    /// `a` moved the fraction `t` of the way to `b` (normalised; the shorter way round).
    private static func toward(_ a: CMQuaternion, _ b: CMQuaternion, _ t: Double) -> CMQuaternion {
        let dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
        let s = dot < 0 ? -1.0 : 1.0
        let q = CMQuaternion(x: a.x + (s * b.x - a.x) * t, y: a.y + (s * b.y - a.y) * t,
                             z: a.z + (s * b.z - a.z) * t, w: a.w + (s * b.w - a.w) * t)
        let n = (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w).squareRoot()
        return CMQuaternion(x: q.x / n, y: q.y / n, z: q.z / n, w: q.w / n)
    }
}
