import SwiftUI

/// One amount per case, animated as one value (SwiftUI animates a VectorArithmetic): how far each folded case is pulled
/// out under the pointer (CoverFlow; user 2026-11-17: "其他收起来的 应该也有相应的互动效果"). A case missing from `v`
/// counts as 0, so two of them with different lengths still add.
struct CaseAmounts: VectorArithmetic {
    var v: [Double] = []

    static var zero: CaseAmounts { CaseAmounts() }
    subscript(_ i: Int) -> Double { v.indices.contains(i) ? v[i] : 0 }

    /// Case i of n pulled out, the others not (i < 0: none).
    static func one(_ i: Int, of n: Int) -> CaseAmounts {
        CaseAmounts(v: (0..<max(n, 0)).map { $0 == i ? 1 : 0 })
    }

    private static func each(_ a: CaseAmounts, _ b: CaseAmounts, _ f: (Double, Double) -> Double) -> CaseAmounts {
        CaseAmounts(v: (0..<max(a.v.count, b.v.count)).map { f(a[$0], b[$0]) })
    }
    static func + (a: CaseAmounts, b: CaseAmounts) -> CaseAmounts { each(a, b, +) }
    static func - (a: CaseAmounts, b: CaseAmounts) -> CaseAmounts { each(a, b, -) }
    static func += (a: inout CaseAmounts, b: CaseAmounts) { a = a + b }
    static func -= (a: inout CaseAmounts, b: CaseAmounts) { a = a - b }
    mutating func scale(by rhs: Double) { v = v.map { $0 * rhs } }
    var magnitudeSquared: Double { v.reduce(0) { $0 + $1 * $1 } }
    static func == (a: CaseAmounts, b: CaseAmounts) -> Bool { (a - b).magnitudeSquared == 0 }
}
