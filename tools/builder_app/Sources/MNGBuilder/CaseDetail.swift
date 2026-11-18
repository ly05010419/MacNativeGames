import SwiftUI

/// The middle case turned over and opened over the whole stage in one move (user 2026-11-17, from the sketch: "翻转后
/// 放大 全屏 然后 左边是封面 右边现在的 按钮", then "翻转的同时 放大 不是翻转完"): as `open` goes 0 → 1 it grows from the
/// case's front at rest (its place and size on the stage) to the stage, turning half a circle about its upright axis
/// the same way the case turns (CoverFlowGeometry.turnWay: the left edge going back first). Up to a quarter turn its
/// front shows; past it, edge-on, the opened case, laid out mirrored so it reads the right way round when the turn ends:
/// the case's front on the left (44%, as large as the space lets it be), the game's actions on the right (`panel`), a
/// close button top right. Closing plays it backwards and ends on the case's front where it stands.
struct CaseDetail<Cover: View, Panel: View>: View, Animatable {
    let stage: CoverFlowGeometry
    var open: CGFloat
    /// The pointer over the opened cover, -1...1 from its centre, and its light (0...1): the cover leans and lights as
    /// the middle case does (user 2026-11-18: "封面 要依然支持 鼠标效果"; CoverFlow.leanCover).
    var lean = CGPoint.zero
    var lit: CGFloat = 0
    /// How far the case is pressed in by the click that opens it (CoverFlowGeometry.press): the turn starts from the
    /// case as the stage drew it under the button, pressed and leaning, and springs back out as it turns (user
    /// 2026-11-18: "翻页 不是在按下 再一次压下后 开始的 貌似从默认状态开始的 所以不连贯").
    var press: CGFloat = 0
    let close: () -> Void
    @ViewBuilder let cover: Cover
    @ViewBuilder let panel: Panel

    /// Animatable by open, so every frame is drawn from its own value: which face shows is decided frame by frame (as a
    /// plain view SwiftUI drew only the start and the end and faded one into the other, both faces showing at once,
    /// mirrored; 2026-11-17, a screenshot 0.1 s into the move). The cover's lean and light animate the same way. Not the
    /// press: it springs out on a quicker, bouncier spring than the turn, and a spring started on one part of a view's
    /// animatableData takes the whole of it over, so the turn ran on it (2026-11-18, "翻页 速度快了很多": open went to
    /// 1.11 and back in the trace); PressedFront animates it on its own.
    var animatableData: AnimatablePair<CGFloat, AnimatablePair<CGFloat, AnimatablePair<CGFloat, CGFloat>>> {
        get { AnimatablePair(open, AnimatablePair(lit, AnimatablePair(lean.x, lean.y))) }
        set { open = newValue.first; lit = newValue.second.first; lean = CGPoint(x: newValue.second.second.first, y: newValue.second.second.second) }
    }

    var body: some View {
        let p = open
        let _ = CoverFlowTrace.note(String(format: "detail open %.3f", Double(p)))
        let fromW = stage.w * stage.frontScale, fromH = stage.h * stage.frontScale
        let width = fromW + (stage.width - fromW) * p, height = fromH + (stage.height - fromH) * p
        let x = stage.backCenter.x + (stage.width / 2 - stage.backCenter.x) * p
        let y = stage.backCenter.y + (stage.height / 2 - stage.backCenter.y) * p
        ZStack {
            if p < 0.5 {   // its front, as the stage draws it (rim and sleeve, lean, light, press), growing as it turns away
                PressedFront(stage: stage, press: press, lean: lean, lit: lit, scale: min(width / stage.w, height / stage.h)) {
                    cover
                }
                .transition(.identity)   // edge-on: one face gives way to the other at once, no fade
            } else {       // the opened case, seen from behind until the turn ends; settled, laid flat as it is
                // (the mirror and the half turn cancel out there, and a lean inside them would come out reversed)
                opened(width: width, height: height, shown: (p - 0.5) * 2)
                    .scaleEffect(x: settled ? 1 : -1, y: 1)
                    .transition(.identity)
            }
        }
        .frame(width: width, height: height)
        .rotation3DEffect(.degrees(settled ? 0 : 180 * Double(p) * CoverFlowGeometry.turnWay), axis: (x: 0, y: 1, z: 0),
                          perspective: 0.45)
        .position(x: x, y: y)
    }

    private func opened(width: CGFloat, height: CGFloat, shown: CGFloat) -> some View {
        let scale = min(height * 0.84 / stage.h, width * Self.coverShare * 0.82 / stage.w)
        return HStack(spacing: 0) {
            ZStack {
                // leaning away under the pointer (the middle case's 9° and 7°: the part under it goes back), its light
                // where the pointer is
                cover.frame(width: stage.w, height: stage.h)
                    .overlay(CaseFront(light: Double(lit), at: UnitPoint(x: (1 + lean.x) / 2, y: (1 + lean.y) / 2)))
                    .clipShape(RoundedRectangle(cornerRadius: stage.corner, style: .continuous))
                    .shadow(color: .black.opacity(0.7), radius: 22, y: 14)
                    .rotation3DEffect(.degrees(Double(lean.x) * 9), axis: (x: 0, y: 1, z: 0), perspective: 0.5)
                    .rotation3DEffect(.degrees(Double(-lean.y) * 7), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
                    .scaleEffect(scale)
            }
            // the whole height: the cover is drawn larger than its own size (scaleEffect does not change layout), and a
            // column only as tall as the unscaled cover cut its band and its name off (2026-11-17, "这个坏了")
            .frame(width: width * Self.coverShare, height: height)
            .clipped()
            // level with the cover: its top line at the cover's top, its bottom (the start button) at the cover's bottom
            // (user 2026-11-17: "文字 高度对齐 启动游戏按钮 放在 黑色背景 右下角")
            let margin = max(12, (height - stage.h * scale) / 2)
            panel
                .padding(.leading, 26).padding(.trailing, margin).padding(.vertical, margin)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .frame(width: width, height: height)
        // one ground for the whole of it, the stage's own, a soft light behind the cover (user 2026-11-17: "黑色 背景 用
        // 封面 后面的就行 不要 这样 隔开 突兀", after a cover side and an actions side of two shades)
        .background {
            ZStack {
                LinearGradient(colors: [Color(white: 0.17), Color(white: 0.03)], startPoint: .top, endPoint: .bottom)
                RadialGradient(colors: [.white.opacity(0.07), .clear], center: UnitPoint(x: Self.coverShare / 2, y: 0.45),
                               startRadius: 0, endRadius: width * 0.4)
            }
        }
        .overlay(alignment: .topTrailing) {
            Button(action: close) {
                Image(systemName: "xmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.white.opacity(0.85))
                    .frame(width: 30, height: 30).background(.white.opacity(0.12), in: Circle())
            }
            .buttonStyle(.plain).padding(12).opacity(Double(shown))
            .help(T.turnBack)
        }
        .clipShape(RoundedRectangle(cornerRadius: 10 + 4 * shown, style: .continuous))
        .environment(\.colorScheme, .dark)
    }

    /// The turn is over: the opened case is drawn flat (no mirror, no half turn), and its cover takes the pointer.
    private var settled: Bool { open >= 0.999 }

    /// The cover's share of the opened case's width (the sketch's 44%).
    static var coverShare: CGFloat { 0.44 }
}

/// The opening case's front as the stage draws the middle case (CoverFlowGeometry.lean, CaseFront's light): leaning
/// under the pointer, lit, pressed in by `press`, at `scale` of its own size. Animatable by the press alone, so the press
/// springs out on its own spring while CaseDetail turns on the turn's (lean, light and scale come from it per frame).
struct PressedFront<Cover: View>: View, Animatable {
    let stage: CoverFlowGeometry
    var press: CGFloat
    let lean: CGPoint
    let lit: CGFloat
    let scale: CGFloat
    @ViewBuilder let cover: Cover

    var animatableData: CGFloat {
        get { press }
        set { press = newValue }
    }

    var body: some View {
        let deeper = 1 + 0.8 * Double(press)   // CoverFlowGeometry.lean
        cover.frame(width: stage.w, height: stage.h)
            .overlay(CaseFront(light: Double(lit + 0.35 * press), at: UnitPoint(x: (1 + lean.x) / 2, y: (1 + lean.y) / 2)))
            .clipShape(RoundedRectangle(cornerRadius: stage.corner, style: .continuous))
            .rotation3DEffect(.degrees(Double(lean.x) * 9 * deeper), axis: (x: 0, y: 1, z: 0), perspective: 0.5)
            .rotation3DEffect(.degrees(Double(-lean.y) * 7 * deeper), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
            .scaleEffect(scale * stage.pressedScale(press))
    }
}
