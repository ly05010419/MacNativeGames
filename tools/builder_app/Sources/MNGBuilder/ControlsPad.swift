import SwiftUI

/// An Xbox controller drawn for the Controls page, as the removed Tk window drew it (the same 340 x 252 grid and
/// places): each of the 16 buttons is clickable, shows its Xbox label, and the marked one is filled with the accent
/// colour and outlined (and named in the caption under it, never by colour alone). The centre button is no input.
struct ControlsPad: View {
    let buttons: [ControlsView.Pad]
    let marked: String?
    let press: (String) -> Void

    private static let body: [CGPoint] = [(70, 60), (270, 60), (310, 85), (328, 150), (318, 215), (290, 240), (258, 232),
                                          (232, 195), (170, 188), (108, 195), (82, 232), (50, 240), (22, 215), (12, 150),
                                          (30, 85)].map { CGPoint(x: $0.0, y: $0.1) }
    private static let round: [String: (CGFloat, CGFloat, CGFloat)] = [
        "south": (255, 131, 12), "east": (280, 106, 12), "west": (230, 106, 12), "north": (255, 81, 12),
        "l3": (95, 106, 24), "r3": (215, 160, 22), "view": (147, 102, 9), "menu": (193, 102, 9)]
    private static let boxes: [String: CGRect] = [
        "l2": CGRect(x: 42, y: 6, width: 70, height: 24), "r2": CGRect(x: 228, y: 6, width: 70, height: 24),
        "l1": CGRect(x: 32, y: 36, width: 90, height: 18), "r1": CGRect(x: 218, y: 36, width: 90, height: 18)]
    private static let arms: [String: CGRect] = [
        "up": CGRect(x: 121, y: 128, width: 18, height: 23), "down": CGRect(x: 121, y: 169, width: 18, height: 23),
        "left": CGRect(x: 98, y: 151, width: 23, height: 18), "right": CGRect(x: 139, y: 151, width: 23, height: 18)]
    private static let arrows = ["up": "▲", "down": "▼", "left": "◀", "right": "▶"]
    private static let short = ["view": "", "menu": ""]

    var body: some View {
        ZStack(alignment: .topLeading) {
            outline.fill(Color(red: 0.2, green: 0.22, blue: 0.27))
            outline.stroke(Color.secondary.opacity(0.6), lineWidth: 2)
            Circle().fill(Color.secondary.opacity(0.35)).frame(width: 24, height: 24).position(x: 170, y: 78)
            ForEach(Array(Self.boxes.keys), id: \.self) { name in
                let r = Self.boxes[name]!
                key(name, RoundedRectangle(cornerRadius: 4), size: r.size, at: CGPoint(x: r.midX, y: r.midY))
            }
            ForEach(Array(Self.round.keys), id: \.self) { name in
                let (x, y, r) = Self.round[name]!
                key(name, Circle(), size: CGSize(width: 2 * r, height: 2 * r), at: CGPoint(x: x, y: y))
            }
            ForEach(["view", "menu"], id: \.self) { name in
                Text(name.capitalized).font(.system(size: 9)).foregroundStyle(.white)
                    .position(x: Self.round[name]!.0, y: Self.round[name]!.1 + 18).allowsHitTesting(false)
            }
            ForEach(Array(Self.arms.keys), id: \.self) { name in
                let r = Self.arms[name]!
                key(name, Rectangle(), size: r.size, at: CGPoint(x: r.midX, y: r.midY))
            }
        }
        .frame(width: 340, height: 252)
    }

    private var outline: Path {
        Path { p in
            let pts = Self.body
            func mid(_ a: CGPoint, _ b: CGPoint) -> CGPoint { CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2) }
            p.move(to: mid(pts[pts.count - 1], pts[0]))
            for i in pts.indices { p.addQuadCurve(to: mid(pts[i], pts[(i + 1) % pts.count]), control: pts[i]) }
            p.closeSubpath()
        }
    }

    private func key<S: Shape>(_ name: String, _ shape: S, size: CGSize, at centre: CGPoint) -> some View {
        let pad = buttons.first { $0.id == name }
        let chosen = marked == name
        let label = Self.arrows[name] ?? Self.short[name] ?? pad?.label ?? name
        return Button { press(name) } label: {
            ZStack {
                shape.fill(chosen ? Color.accentColor : Color(white: 0.42))
                shape.stroke(chosen ? Color.white : Color.secondary.opacity(0.7), lineWidth: chosen ? 3 : 1)
                Text(label).font(.system(size: size.height > 20 ? 12 : 9, weight: .semibold)).foregroundStyle(.white)
            }
            .frame(width: size.width, height: size.height)
            .contentShape(shape)
        }
        .buttonStyle(.plain)
        .help(pad?.caption ?? "")
        .accessibilityLabel(pad?.label ?? name)
        .accessibilityAddTraits(chosen ? .isSelected : [])
        .position(centre)
    }
}
