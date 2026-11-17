import QuartzCore
import SwiftUI

/// The games as an iPod classic's Cover Flow of real game cases (user 2026-11-17: "改成 苹果 ipod 音乐封面 那种效果",
/// "ipod classic", "Carousel 效果", "最好 有厚度 想一个 真正的 游戏cd盒子 一样", "不是 cd 盒 是 Switch 游戏盒子"): every
/// game is a Switch-style game case with depth (105 x 170 x 11 mm, red plastic, rounded all the way through; a red band
/// across the top saying what the game runs on, no Nintendo logo: the repository carries nothing of theirs). The chosen
/// one faces the player in the middle;
/// the others stand turned towards it on both sides, packed close, so their spine shows; each has its reflection on a
/// dark floor. The faces are flat views placed in 3D by one camera (projectionEffect with the same matrices), so the
/// spine's width is what the turn shows, not drawn by guess. A side box, an arrow, the arrow keys or a drag brings a box
/// to the middle; a click on the middle one (or Return) is `open`.
struct CoverFlow<Front: View, Spine: View>: View {
    let count: Int
    @Binding var index: Int
    var size: CGFloat = 375   // the case's height (user 2026-11-17: half again as big)
    @ViewBuilder let front: (Int) -> Front
    @ViewBuilder let spine: (Int) -> Spine
    let open: (Int) -> Void

    @State private var drag: CGFloat = 0
    @State private var dragging = false
    @FocusState private var focused: Bool

    private var position: Double { Double(index) - Double(drag / (size * 0.35)) }
    private var height: CGFloat { CoverFlowGeometry.top(size) + size * 1.38 }   // the room above, the box, its reflection
    private let spring = Animation.spring(response: 0.45, dampingFraction: 0.86)

    var body: some View {
        GeometryReader { geo in
            CoverFlowStage(count: count, size: size, width: geo.size.width, height: height, position: position,
                           front: front, spine: spine)
                .contentShape(Rectangle())
                .gesture(SpatialTapGesture().onEnded { tap in
                    guard !dragging else { return }
                    let stage = CoverFlowGeometry(count: count, size: size, width: geo.size.width, height: height, position: position)
                    guard let i = stage.cover(at: tap.location) else { return }
                    if i == index { open(i) } else { withAnimation(spring) { index = i } }
                })
        }
        .frame(height: height)
        .background(LinearGradient(colors: [Color(white: 0.17), Color(white: 0.03)], startPoint: .top, endPoint: .bottom))
        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .simultaneousGesture(DragGesture(minimumDistance: 6)
            .onChanged { v in dragging = true; drag = v.translation.width }
            .onEnded { _ in
                let to = min(max(Int(position.rounded()), 0), count - 1)
                withAnimation(spring) { index = to; drag = 0 }
                DispatchQueue.main.async { dragging = false }   // the click a drag ends with is not a tap
            })
        .overlay(alignment: .leading) { arrow("chevron.left", by: -1) }
        .overlay(alignment: .trailing) { arrow("chevron.right", by: 1) }
        .focusable().focused($focused).focusEffectDisabled()
        .onKeyPress(.leftArrow) { move(-1); return .handled }
        .onKeyPress(.rightArrow) { move(1); return .handled }
        .onKeyPress(.return) { open(index); return .handled }
        .onAppear { focused = true }
        .accessibilityIdentifier("cover-flow")
    }

    private func arrow(_ symbol: String, by step: Int) -> some View {
        let possible = (0..<count).contains(index + step)
        return Button { move(step) } label: {
            Image(systemName: symbol).font(.title3.weight(.bold)).foregroundStyle(.black)
                .frame(width: 36, height: 36).background(.white.opacity(0.85), in: Circle()).shadow(radius: 4)
        }
        .buttonStyle(.plain).padding(.horizontal, 10)
        .opacity(possible ? 1 : 0).disabled(!possible)
    }

    private func move(_ step: Int) {
        let to = min(max(index + step, 0), count - 1)
        withAnimation(spring) { index = to }
    }
}

/// Where every box is for a position (the cover in the middle, fractional while moving), and the matrices that place
/// each face. Row vectors, as CATransform3D: a point p of a face's own view maps to p * face * box * camera.
struct CoverFlowGeometry {
    let count: Int
    let size: CGFloat
    let width: CGFloat
    let height: CGFloat
    let position: Double

    // a Switch game case: 105 x 170 x 11 mm; the depth shown a little more, so the turned ones read as boxes
    var w: CGFloat { size * 0.62 }
    var h: CGFloat { size }
    var depth: CGFloat { size * 0.075 }
    /// The room above the middle box (user 2026-11-17: "和 上面 保持一点距离").
    static func top(_ size: CGFloat) -> CGFloat { size * 0.09 }
    var floorY: CGFloat { Self.top(size) + h }   // the boxes stand on it; their reflections hang below it
    /// Between a box and its reflection (user 2026-11-17: "阴影 和上面的盒子 保持一点距离").
    var gap: CGFloat { size * 0.025 }
    private let tilt = 64.0 * Double.pi / 180
    private var eye: CGFloat { size * 3.2 } // the camera's distance: the perspective

    /// Place from the middle, in boxes, and how far turned (0 in the middle, ±1 once turned).
    func place(_ i: Int) -> (d: Double, side: Double) {
        let d = Double(i) - position
        return (d, max(-1, min(1, d)))
    }

    /// The box's turn and place: turned towards the middle, the first beside it at a gap, the rest packed closer, the
    /// turned ones set back.
    func boxTransform(_ i: Int, mirrored: Bool) -> CATransform3D {
        let (d, side) = place(i)
        let x = CGFloat(side) * w * 1.02 + CGFloat(d - side) * w * 0.3
        let z = -CGFloat(abs(side)) * w * 0.6
        let a = -side * tilt
        var r = CATransform3DIdentity
        r.m11 = CGFloat(cos(a)); r.m13 = CGFloat(-sin(a))
        r.m31 = CGFloat(sin(a)); r.m33 = CGFloat(cos(a))
        var m = r
        if mirrored {   // upside down about the floor (the box's bottom, y = h/2 from its middle), a gap below it
            var flip = CATransform3DIdentity
            flip.m22 = -1; flip.m42 = h + gap
            m = CATransform3DConcat(flip, m)
        }
        m = CATransform3DConcat(m, CATransform3DMakeTranslation(x, 0, z))
        var camera = CATransform3DIdentity
        camera.m34 = -1 / eye
        m = CATransform3DConcat(m, camera)
        return CATransform3DConcat(m, CATransform3DMakeTranslation(width / 2, floorY - h / 2, 0))
    }

    /// The front: its view (w x h) on the box's front plane.
    var frontPlacement: CATransform3D { CATransform3DMakeTranslation(-w / 2, -h / 2, depth / 2) }

    /// The case's corner radius, the same on the front and all the way through the box.
    var corner: CGFloat { w * 0.05 }

    /// A side's flat part: its view (depth x (h - 2 corner)) on the box's left (the spine) or right side, front edge at
    /// the front; above and below it the side turns into the rounded corners (`slicePlacement`).
    func sidePlacement(left: Bool) -> CATransform3D {
        var m = CATransform3DIdentity
        m.m11 = 0; m.m13 = left ? 1 : -1      // the view's x runs along the box's depth
        m.m31 = left ? -1 : 1; m.m33 = 0
        m.m41 = left ? -w / 2 : w / 2; m.m42 = -h / 2 + corner; m.m43 = left ? -depth / 2 : depth / 2
        return m
    }

    /// The rounded outline's layers through the box (user 2026-11-17: "盒子 上面和下面 有缺角"): a case is its front's
    /// rounded rectangle carried back through its depth, so the outline is drawn at `slices` depths from back to front
    /// and the corners are solid and round, not a square side beside a rounded front.
    static let slices = 12
    func slicePlacement(_ k: Int) -> CATransform3D {
        CATransform3DMakeTranslation(-w / 2, -h / 2, -depth / 2 + depth * CGFloat(k) / CGFloat(Self.slices - 1))
    }

    /// The order to draw in: the farthest first.
    var drawOrder: [Int] { (0..<count).sorted { abs(place($0).d) > abs(place($1).d) } }

    /// The box whose front is under a point of the stage, the nearest first.
    func cover(at point: CGPoint) -> Int? {
        for i in drawOrder.reversed() {
            let m = CATransform3DConcat(frontPlacement, boxTransform(i, mirrored: false))
            let corners = [CGPoint(x: 0, y: 0), CGPoint(x: w, y: 0), CGPoint(x: w, y: h), CGPoint(x: 0, y: h)].map { project($0, m) }
            if inside(point, corners) { return i }
        }
        return nil
    }

    private func project(_ p: CGPoint, _ m: CATransform3D) -> CGPoint {
        let x = p.x * m.m11 + p.y * m.m21 + m.m41
        let y = p.x * m.m12 + p.y * m.m22 + m.m42
        let w = p.x * m.m14 + p.y * m.m24 + m.m44
        return CGPoint(x: x / w, y: y / w)
    }

    private func inside(_ p: CGPoint, _ quad: [CGPoint]) -> Bool {
        var sign: CGFloat = 0
        for k in 0..<quad.count {
            let a = quad[k], b = quad[(k + 1) % quad.count]
            let cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)
            if cross != 0 {
                if sign == 0 { sign = cross } else if (sign > 0) != (cross > 0) { return false }
            }
        }
        return true
    }
}

/// The boxes for one position. Animatable by the position, so a move redraws every frame of the spring.
private struct CoverFlowStage<Front: View, Spine: View>: View, Animatable {
    let count: Int
    let size: CGFloat
    let width: CGFloat
    let height: CGFloat
    var position: Double
    let front: (Int) -> Front
    let spine: (Int) -> Spine

    var animatableData: Double {
        get { position }
        set { position = newValue }
    }

    var body: some View {
        let g = CoverFlowGeometry(count: count, size: size, width: width, height: height, position: position)
        ZStack(alignment: .topLeading) {
            // the reflections: the boxes upside down under the floor, fading into it
            ZStack(alignment: .topLeading) {
                ForEach(g.drawOrder, id: \.self) { i in box(i, g, mirrored: true) }
            }
            .frame(width: width, height: height, alignment: .topLeading)   // the mask covers the whole stage, not one face
            // one picture first, then faint: an opacity on a plain ZStack is given to every face on its own, and the
            // farther boxes' reflections showed through the nearer ones (user 2026-11-17: "阴影 透视了")
            .compositingGroup()
            .opacity(0.32)
            .mask(LinearGradient(stops: [.init(color: .black, location: (g.floorY + g.gap) / height),
                                         .init(color: .clear, location: min(1, (g.floorY + g.gap + size * 0.42) / height))],
                                 startPoint: .top, endPoint: .bottom))
            ForEach(g.drawOrder, id: \.self) { i in box(i, g, mirrored: false) }
        }
        .frame(width: width, height: height, alignment: .topLeading)
        .allowsHitTesting(false)
    }

    @ViewBuilder
    private func box(_ i: Int, _ g: CoverFlowGeometry, mirrored: Bool) -> some View {
        let side = g.place(i).side
        let m = g.boxTransform(i, mirrored: mirrored)
        let shade = abs(side) * 0.18   // the turned ones darker, as away from the light
        ZStack(alignment: .topLeading) {
            // only the side that faces the camera: a box turned towards the middle shows its outer side, so one on the
            // left shows its spine (the hinge side), one on the right its opening edge (each side's normal against the
            // line of sight)
            if abs(side) > 0.02 {
                ForEach(0..<CoverFlowGeometry.slices, id: \.self) { k in
                    RoundedRectangle(cornerRadius: g.corner, style: .continuous)
                        .fill(Color(red: 0.6, green: 0.03, blue: 0.06))
                        .overlay(RoundedRectangle(cornerRadius: g.corner, style: .continuous).fill(Color.black.opacity(shade)))
                        .frame(width: g.w, height: g.h)
                        .projectionEffect(ProjectionTransform(CATransform3DConcat(g.slicePlacement(k), m)))
                }
            }
            if side < -0.02 {
                spine(i).frame(width: g.depth, height: g.h - 2 * g.corner)
                    .overlay(Color.black.opacity(0.1 + shade))
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.sidePlacement(left: true), m)))
            } else if side > 0.02 {
                CaseEdge().frame(width: g.depth, height: g.h - 2 * g.corner)
                    .overlay(Color.black.opacity(0.1 + shade))
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.sidePlacement(left: false), m)))
            }
            front(i).frame(width: g.w, height: g.h)
                .overlay(CaseFront())
                .overlay(Color.black.opacity(shade))
                .clipShape(RoundedRectangle(cornerRadius: g.corner, style: .continuous))   // the case's rounded corners
                .projectionEffect(ProjectionTransform(CATransform3DConcat(g.frontPlacement, m)))
        }
    }
}

/// The case's red plastic (sides, rim).
let caseRed = Color(red: 0.86, green: 0.07, blue: 0.1)

/// A game case's front: the red plastic rim round the insert and the clear sleeve's shine over it.
private struct CaseFront: View {
    var body: some View {
        GeometryReader { geo in
            let rim = geo.size.width * 0.03
            ZStack {
                RoundedRectangle(cornerRadius: geo.size.width * 0.05, style: .continuous)
                    .strokeBorder(caseRed, lineWidth: rim)
                LinearGradient(stops: [.init(color: .white.opacity(0.3), location: 0), .init(color: .white.opacity(0.07), location: 0.42),
                                       .init(color: .clear, location: 0.48)], startPoint: .topLeading, endPoint: .bottomTrailing)
                    .padding(rim)
            }
        }
        .allowsHitTesting(false)
    }
}

/// The red band across the top of a case's front, as on a Switch case; it says what the game runs on (as the cards did,
/// user 2026-11-17: Ocarina of Time "Switch 2 · Mac"), never a maker's logo.
struct CaseHeader: View {
    /// The band's share of the front's height (a Switch 2 case: about an eighth).
    static let share: CGFloat = 0.12
    let game: String

    var body: some View {
        GeometryReader { geo in
            ZStack {
                LinearGradient(colors: [caseRed, Color(red: 0.74, green: 0.04, blue: 0.08)], startPoint: .top, endPoint: .bottom)
                Text(T.gamePlatforms(game))
                    .font(.system(size: geo.size.height * 0.36, weight: .heavy)).foregroundStyle(.white)
                    .lineLimit(1).minimumScaleFactor(0.5)
                    .padding(.horizontal, geo.size.width * 0.08)
            }
        }
    }
}

/// A case's opening edge, seen on a box right of the middle: the red plastic, rounded by the light.
private struct CaseEdge: View {
    var body: some View {
        LinearGradient(colors: [caseRed.opacity(0.75), caseRed, Color(red: 0.5, green: 0.02, blue: 0.05)],
                       startPoint: .leading, endPoint: .trailing)
    }
}

/// A game case's spine, seen on the boxes left of the middle: the red plastic with the game's name written down it, as
/// on a shelf (no maker's logo).
struct CaseSpine: View {
    let game: String

    var body: some View {
        GeometryReader { geo in
            ZStack {
                LinearGradient(colors: [Color(red: 0.55, green: 0.03, blue: 0.06), caseRed, Color(red: 0.62, green: 0.03, blue: 0.07)],
                               startPoint: .leading, endPoint: .trailing)
                Text(T.gameName(game))
                    .font(.system(size: max(7, geo.size.width * 0.48), weight: .bold)).foregroundStyle(.white)
                    .lineLimit(1).fixedSize()
                    .rotationEffect(.degrees(90))
                    .frame(width: geo.size.width, height: geo.size.height)
            }
        }
    }
}
