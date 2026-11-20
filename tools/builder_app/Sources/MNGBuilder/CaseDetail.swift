import SwiftUI

/// How the middle case opens over the stage (user 2026-11-19: "launcher 盒子 翻转 改成 盒子 打开" for the Mac, "旧方案 保留
/// 可以切换"): `.book`, the case opens like a real one (2026-11-19, "从中间 打开"): its front cover swings open about
/// the spine on its left, which ends at the stage's middle, the tray with the actions on the right; `.turn`, the whole case turns over (2026-11-17, the iPhone / iPad Launcher's).
enum CaseOpening: String, CaseIterable {
    case book, turn
    /// The Mac Launcher's setting (UserDefaults; HomePage, UsageStatsSettings).
    static let key = "caseOpening"
}

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
    var opening = CaseOpening.turn
    /// The picture printed on the insert's inner side, one picture across both halves of the opened case, as a real
    /// case shows it through its clear plastic (user 2026-11-19, a photo of an opened case: "这样效果"); nil (a game not
    /// built yet), a plain dark tray and the cover's front on the open cover.
    var inside: PlatformImage? = nil
    /// The cartridge in its slot: only for a game that is built (the Mac) or installed (the iPhone); an empty case for
    /// one that is not (user 2026-11-19: "没安装的 去掉 左边盒子里的 sd卡").
    var cartridge = true
    /// Closes the case: a tap on the ground round it (no close button, user 2026-11-19 "去掉 关闭按钮 点击背景 关闭"), or
    /// Escape on the Mac (CoverFlow).
    let close: () -> Void
    @Environment(\.displayScale) var displayScale   // the blurred pictures are drawn at it (FrozenBlur)
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
        if opening == .book {
            book(p: p)
        } else {
            turned(width: width, height: height, p: p).position(x: x, y: y)
        }
    }

    /// `.turn`: the whole case turns half a circle as it grows; the opened case shows from the quarter turn on.
    private func turned(width: CGFloat, height: CGFloat, p: CGFloat) -> some View {
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
    }

    /// `.book`: the case opens like a real one lying on the stage (user 2026-11-19: "从中间 打开 就像真实 打开盒子").
    /// It keeps its own shape, only growing a little; its front cover swings open about the spine on its left edge, back
    /// and over to the left, while the case slides so that the spine ends at the stage's middle. Opened, the two halves
    /// are the same size side by side: the cover on the left (past the quarter turn its inner side shows, the insert's
    /// picture seen through the clear sleeve), the tray on the right holding the game's actions. Closing plays it
    /// backwards and ends on the case's front where it stands.
    private func book(p: CGFloat) -> some View {
        let fromW = stage.w * stage.frontScale, fromH = stage.h * stage.frontScale
        // the opened case: as tall as the stage lets it be, both halves together within its width
        let openH = min(stage.height * 0.9, stage.width * 0.92 / 2 * stage.h / stage.w)
        let openW = openH * stage.w / stage.h
        let caseW = fromW + (openW - fromW) * p, caseH = fromH + (openH - fromH) * p
        let k = caseW / stage.w                   // the case's scale (its shape kept: caseH / stage.h too)
        // the hinge: the closed case's left edge at first, the stage's middle at the end
        let hinge0 = stage.backCenter.x - fromW / 2
        let hinge = hinge0 + (stage.width / 2 - hinge0) * p
        let top = stage.backCenter.y + (stage.height / 2 - stage.backCenter.y) * p - caseH / 2
        let angle = -180 * Double(p)              // about the spine: the cover goes back and over to the left
        let openCorner = stage.corner * openW / stage.w
        let grow = caseW / openW                  // the opened case's halves, laid out at openW x openH, scaled to now
        let spineW = caseW * 0.07
        let shown = Double(max(0, (p - 0.3) / 0.7))
        // the blurred pictures, made once at their opened size (FrozenBlur), never blurred again frame by frame
        // heavier than the ground's (radius 22) and milky over it, so the case's inside reads as frosted glass set apart
        // from the frosted ground (user 2026-11-19: "打开 盒子的 毛玻璃 效果 应该更重 和背景 毛玻璃有区分"; it was 16)
        let frost = inside.flatMap { FrozenBlur.image($0, width: 2 * openW, height: openH, radius: 44, saturation: 1.2,
                                                      scale: displayScale) }
        // the ground frosted too, the Mac as the iPhone (user 2026-11-19: "算了 mac 背景也是毛玻璃", after trying it clear)
        let ground = inside.flatMap { FrozenBlur.image($0, width: stage.width, height: stage.height, radius: 22,
                                                       saturation: 1, scale: displayScale) }
        return ZStack(alignment: .topLeading) {
            // the ground behind the opened case, coming in as it opens: the game's picture over the whole stage, blurred
            // and dimmed so the case stands out (user 2026-11-19: "整个后面 黑色 背景 换成封面"); without one, the stage's
            // own dark ground
            ZStack {
                LinearGradient(colors: [Color(white: 0.17), Color(white: 0.03)], startPoint: .top, endPoint: .bottom)
                if let ground {
                    Image(platformImage: ground).resizable()
                        .frame(width: stage.width, height: stage.height)
                        .overlay(Color.black.opacity(0.4))
                } else {
                    RadialGradient(colors: [.white.opacity(0.07), .clear], center: .center, startRadius: 0,
                                   endRadius: stage.width * 0.45)
                }
            }
            .frame(width: stage.width, height: stage.height)
            .opacity(Double(p))
            // a tap on it, round the case, closes the case (user 2026-11-19: "去掉 关闭按钮 点击背景 关闭")
            .contentShape(Rectangle())
            .onTapGesture(perform: close)
            // the tray right of the spine, under the cover: the inside picture's right half and the actions. Laid out
            // once at the opened size and only scaled and moved as the case grows (user 2026-11-19: "右边的 翻页 一直在
            // 抖动": laid out anew every frame at the growing size, its type and buttons stepped a whole pixel at a time
            // against the smoothly moving picture, for seconds in slow motion as the spring settled back)
            // Closed, it is pressed in and leans with the cover over it, so its clear rim never shows round the cover
            // (user 2026-11-19: "透明的边框 和红色封面 不一致 开始的时候"); from the quarter turn on it lies flat.
            tray(width: openW, height: openH, corner: openCorner, shown: shown, frost: frost)
                .shadow(color: .black.opacity(0.6 * Double(p)), radius: 18, y: 12)
                .frame(width: openW, height: openH)
                .modifier(PressedLean(stage: stage, press: press, lean: angle > -90 ? lean : .zero))
                .scaleEffect(grow, anchor: .topLeading)
                .offset(x: hinge, y: top)
            // the spine between the two halves: a groove of clear plastic
            LinearGradient(colors: [.white.opacity(0.10), .white.opacity(0.35), .black.opacity(0.25), .white.opacity(0.30),
                                    .white.opacity(0.10)], startPoint: .leading, endPoint: .trailing)
                .frame(width: spineW, height: caseH)
                .offset(x: hinge - spineW / 2, y: top)
                .opacity(Double(p))
            // the cover, hinged on its left edge at the spine: its front until edge-on, then its inner side, drawn
            // mirrored so it reads the right way round once it lies open
            Group {
                if angle > -90 {
                    PressedFront(stage: stage, press: press, lean: lean, lit: lit, scale: k) { cover }
                } else if let inside, let frost {
                    // the inside picture's left half under the cover's clear plastic, its clips on the outer edge
                    // and the cartridge in its slot in the middle of it (user 2026-11-19: "放在 左边 中间", then "大50%")
                    // (laid out once at the opened size, then scaled, as the tray)
                    ZStack {
                        Self.frosted(frost, left: true, width: openW, height: openH)
                        Self.plastic(width: openW, height: openH, corner: openCorner, outerLeading: true)
                        if cartridge {
                            Self.cartridge(inside, width: openW * 0.405, height: openW * 0.405 * 1.36)
                        }
                    }
                    .frame(width: openW, height: openH)
                    .clipShape(RoundedRectangle(cornerRadius: openCorner, style: .continuous))
                    .scaleEffect(x: -grow, y: grow)
                    .shadow(color: .black.opacity(0.6 * Double(p)), radius: 18, y: 12)
                } else {
                    cover.frame(width: stage.w, height: stage.h)
                        .overlay(CaseFront(light: Double(lit), at: UnitPoint(x: (1 + lean.x) / 2, y: (1 + lean.y) / 2)))
                        .clipShape(RoundedRectangle(cornerRadius: stage.corner, style: .continuous))
                        .scaleEffect(x: -k, y: k)
                        .shadow(color: .black.opacity(0.6 * Double(p)), radius: 18, y: 12)
                }
            }
            .transition(.identity)
            .frame(width: caseW, height: caseH)
            .rotation3DEffect(.degrees(angle), axis: (x: 0, y: 1, z: 0), anchor: .leading, perspective: 0.45)
            // the turn only draws it on the left: laid out it stays over the tray, and took every click meant for the
            // actions (2026-11-20, 1.1.0: 极速更新 and 启动游戏 did nothing); it has no control of its own
            .allowsHitTesting(false)
            .offset(x: hinge, y: top)
        }
        .frame(width: stage.width, height: stage.height, alignment: .topLeading)
        .environment(\.colorScheme, .dark)
    }

    /// The tray, the case's right half, as it lies open: the frosted inside picture's right half under clear plastic
    /// (clips on its outer edge) and the game's actions right on it (the cartridge is in the middle of the left half);
    /// without a picture, the plain dark tray.
    private func tray(width: CGFloat, height: CGFloat, corner: CGFloat, shown: Double, frost: PlatformImage?) -> some View {
        let inset = max(12, width * 0.07)
        return ZStack(alignment: .topLeading) {
            if let frost {
                Self.frosted(frost, left: false, width: width, height: height)
            } else {
                LinearGradient(colors: [Color(white: 0.14), Color(white: 0.07)], startPoint: .top, endPoint: .bottom)
            }
            Self.plastic(width: width, height: height, corner: corner, outerLeading: false)
            panel
                .padding(inset)
                .frame(width: width, height: height, alignment: .topLeading)
                .opacity(shown)
                // black type on the light frosted glass ("盒子里 黑色字体"); white on the plain dark tray
                .environment(\.colorScheme, frost == nil ? .dark : .light)
        }
        .frame(width: width, height: height, alignment: .topLeading)
        .clipShape(RoundedRectangle(cornerRadius: corner, style: .continuous))
    }

    /// One half of the inside picture, the whole of it frosted (user 2026-11-19: "毛玻璃特效", then "整个后面 背景 改成
    /// 毛玻璃 效果，而不是 两块 毛玻璃"; a dark wash over the system material had come out a muddy grey-brown): the
    /// picture fills the two halves side by side and is blurred as one, its colour lifted, so the blur runs on across
    /// the spine; this half is cut out of it, a thin dark wash over it for the type. `frost` is that picture already
    /// blurred at 2·width x height (FrozenBlur, radius 44, saturation 1.2): nothing is blurred frame by frame.
    private static func frosted(_ frost: PlatformImage, left: Bool, width: CGFloat, height: CGFloat) -> some View {
        Image(platformImage: frost).resizable()
            .frame(width: 2 * width, height: height)
            .offset(x: left ? width / 2 : -width / 2)
            .frame(width: width, height: height)
            .clipped()
            // milky, as frosted glass, light enough for black type (user 2026-11-19: "盒子里 黑色字体")
            .overlay(Color.white.opacity(0.45))
    }

    /// A half's clear plastic: a sheen across it, its thick rim, the sleeve's inner edge and two clips on the outer edge.
    private static func plastic(width: CGFloat, height: CGFloat, corner: CGFloat, outerLeading: Bool) -> some View {
        let clip = RoundedRectangle(cornerRadius: 2, style: .continuous).fill(.white.opacity(0.45))
            .frame(width: max(3, width * 0.028), height: height * 0.07)
        return ZStack {
            LinearGradient(colors: [.white.opacity(0.18), .clear, .clear, .white.opacity(0.08)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
            RoundedRectangle(cornerRadius: corner, style: .continuous)
                .strokeBorder(.white.opacity(0.6), lineWidth: max(2, width * 0.014))
            RoundedRectangle(cornerRadius: corner * 0.7, style: .continuous)
                .strokeBorder(.white.opacity(0.22), lineWidth: 1)
                .padding(width * 0.032)
            VStack { clip; Spacer(); clip }
                .padding(.vertical, height * 0.1)
                .padding(.horizontal, width * 0.012)
                .frame(maxWidth: .infinity, alignment: outerLeading ? .leading : .trailing)
        }
        .frame(width: width, height: height)
        .allowsHitTesting(false)
    }

    /// The cartridge in its slot: a clear well, the dark cartridge in it, its red label band and the game's picture.
    private static func cartridge(_ image: PlatformImage, width: CGFloat, height: CGFloat) -> some View {
        let w = width * 0.8, h = height * 0.84
        return ZStack {
            RoundedRectangle(cornerRadius: width * 0.1, style: .continuous).fill(.black.opacity(0.3))
            RoundedRectangle(cornerRadius: width * 0.1, style: .continuous).strokeBorder(.white.opacity(0.55), lineWidth: 1.5)
            VStack(spacing: 0) {
                Rectangle().fill(Color(red: 0.86, green: 0.07, blue: 0.1)).frame(height: h * 0.16)
                Image(platformImage: image).resizable().aspectRatio(contentMode: .fill)
                    .frame(width: w * 0.86, height: h * 0.66).clipped()
                    .padding(.top, h * 0.05)
                Spacer(minLength: 0)
            }
            .frame(width: w, height: h)
            .background(Color(white: 0.13))
            .clipShape(RoundedRectangle(cornerRadius: w * 0.09, style: .continuous))
            .shadow(color: .black.opacity(0.5), radius: 3, y: 2)
        }
        .frame(width: width, height: height)
        .allowsHitTesting(false)
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
            // a tap on it, off the cover and the actions, closes the case (user 2026-11-19: "去掉 关闭按钮 点击背景 关闭")
            .contentShape(Rectangle())
            .onTapGesture(perform: close)
        }
        .clipShape(RoundedRectangle(cornerRadius: 10 + 4 * shown, style: .continuous))
        .environment(\.colorScheme, .dark)
    }

    /// The turn is over: the opened case is drawn flat (no mirror, no half turn), and its cover takes the pointer.
    private var settled: Bool { open >= 0.999 }

    /// The cover's share of the opened case's width (the sketch's 44%).
    static var coverShare: CGFloat { 0.44 }
}

/// The closed case's press and lean on another part of it than its front (the tray under the cover, CaseDetail's book):
/// the same turns and the same pressed-in scale as PressedFront gives the front, about the part's own centre, the press
/// animated on its own spring as there.
struct PressedLean: ViewModifier, Animatable {
    let stage: CoverFlowGeometry
    var press: CGFloat
    let lean: CGPoint

    var animatableData: CGFloat {
        get { press }
        set { press = newValue }
    }

    func body(content: Content) -> some View {
        let deeper = 1 + 0.8 * Double(press)   // CoverFlowGeometry.lean, as PressedFront
        content
            .rotation3DEffect(.degrees(Double(lean.x) * 9 * deeper), axis: (x: 0, y: 1, z: 0), perspective: 0.5)
            .rotation3DEffect(.degrees(Double(-lean.y) * 7 * deeper), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
            .scaleEffect(stage.pressedScale(press))
    }
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
            .environment(\.caseParallax, lean)   // its layers apart as on the stage, coming together as the lean goes
            .overlay(CaseFront(light: Double(lit + 0.35 * press), at: UnitPoint(x: (1 + lean.x) / 2, y: (1 + lean.y) / 2)))
            .clipShape(RoundedRectangle(cornerRadius: stage.corner, style: .continuous))
            .rotation3DEffect(.degrees(Double(lean.x) * 9 * deeper), axis: (x: 0, y: 1, z: 0), perspective: 0.5)
            .rotation3DEffect(.degrees(Double(-lean.y) * 7 * deeper), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
            .scaleEffect(scale * stage.pressedScale(press))
    }
}
