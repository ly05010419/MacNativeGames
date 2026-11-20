#if os(macOS)
import AppKit   // the trackpad's two-finger swipe (watchSwipes); the iPhone / iPad Launcher shares the rest of this file
#endif
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
/// to the middle.
///
/// The middle box turns over (user 2026-11-17, scheme A of the sketch: "先做 a"): a click on it or Return turns it round
/// in place (the left edge going back first, so its opening edge shows; "这个方向好"), a little towards the player, the
/// others stepping back; its back holds the game's actions (`back`, drawn). Once the turn has settled the case opens over
/// the whole stage ("翻转后 放大 全屏 然后 左边是封面 右边现在的 按钮"): CaseDetail grows from the back's place and size
/// (the turned box faces the camera, so the projection there is a scale and a move: CoverFlowGeometry.backScale,
/// backCenter) to the stage, its front on the left and the live actions on the right (`detail`; Return starts the game
/// there). Esc or its close button closes it back to the case's back and turns the case round.
/// `back(i, live)`: the drawn back during the turn (live false).
///
/// How it feels, as in the sketch's A page (user 2026-11-17: "web页面 a 很好 有鼠标 光影 和 盒子动的特效"): the middle case
/// leans away under the pointer and a light follows it over the front; a drag or a two-finger swipe on the trackpad
/// carries on with its speed and settles on a case, held back past either end and let go; a game started from the opened
/// case turns the case back to its front, the page of the games, as closing it does (`launches` counts the starts; user
/// 2026-11-18: "点击 启动游戏 就翻页返回主页 不要放大封面" — until then its cover grew over the whole stage and faded).
struct CoverFlow<Front: View, Spine: View, Back: View, Detail: View>: View {
    let count: Int
    @Binding var index: Int
    var size: CGFloat = 375   // the case's height (user 2026-11-17: half again as big)
    var launches = 0          // bumped when a game starts from the opened case: the case turns back to its front
    var name: (Int) -> String = { _ in "" }   // a case's game, floated over it while the pointer pulls it out
    /// Set: a click (a tap) or Return on the middle case calls it with the case, pressed in and let go, and the case does
    /// not turn over (the iPhone / iPad Launcher: the cases only, a tap starts the game; user 2026-11-19 "ios 只要这个
    /// 不要和mac 一模一样"). nil (the Mac): it turns over and opens.
    var onOpen: ((Int) -> Void)? = nil
    /// The arrow buttons at either side (the Mac); false on the iPhone / iPad, where a swipe moves the cases (user
    /// 2026-11-19: "左右箭头不要").
    var arrows = true
    /// How the middle case opens (CaseOpening): the Mac's setting, book by default; the iPhone / iPad turn it over.
    var opening = CaseOpening.turn
    /// The picture spread across a case's inside, seen when it opens like a book (CaseDetail's `inside`); nil, a plain tray.
    var inside: (Int) -> PlatformImage? = { _ in nil }
    /// Whether case i holds its cartridge when it opens: a game built (the Mac) or installed (the iPhone); none, an empty
    /// slot (user 2026-11-19: "没安装的 去掉 左边盒子里的 sd卡").
    var cartridge: (Int) -> Bool = { _ in true }
    /// Every animation of the stage this many times slower (user 2026-11-19: "慢速模式 … 慢1 2 3 5 10 20 50倍 我要看动画效果").
    var slow: Double = 1
    /// The stage's corners: the Mac, a rounded panel in the window; the iPhone / iPad, 0, the stage is the whole screen
    /// and its corners the screen's own (user 2026-11-19: "整个背景 都不是全屏， 做成全屏").
    var corner: CGFloat = 14
    /// The phone's turn in place of the pointer (the iPhone / iPad: MotionTilt, user 2026-11-19 "mac 的 鼠标 在封面上的
    /// 效果（关闭的时候） 放在iphone上 用 陀螺仪 模拟"): -1...1 across and down the middle case; nil (the Mac), the pointer.
    /// Handed over by a call, not as a value of the view, so a turn redraws the middle case only, not what holds it.
    var turns: TurnFeed? = nil
    /// An animation of the stage at the slow-motion rate.
    private func paced(_ animation: Animation) -> Animation { animation.speed(1 / max(1, slow)) }
    @ViewBuilder let front: (Int) -> Front
    @ViewBuilder let spine: (Int) -> Spine
    @ViewBuilder let back: (Int, Bool) -> Back
    @ViewBuilder let detail: (Int, Bool) -> Detail   // (case, live): live false for its reflection

    @State private var drag: CGFloat = 0
    @State private var dragging = false
    @State private var flipped = false     // the middle box shows its back
    @State private var expanded = false    // the middle case is (being) opened over the stage: CaseDetail draws it
    @State private var open: CGFloat = 0   // how far, turning and growing at once: 0 its front at rest, 1 the stage
    @State private var coverLean = CGPoint.zero   // the pointer over the opened case's cover, -1...1 (leanCover)
    @State private var coverLit: CGFloat = 0
    @State private var detailPress: CGFloat = 0   // the press the opening case turns from (turn(true)), springing out
    @State private var press = 0.0             // the middle case pressed down by a click (pressDown / release)
    @State private var pressing = false        // the mouse button is down (the press gesture has begun)
    @State private var pressedMiddle = false   // ...and it went down on the middle case
    /// The middle case's lean and light (the pointer or the phone's turn): held apart, so changing them redraws only the
    /// middle case (MiddleLean), not the stage and every case in it (user 2026-11-19: on the iPhone every reading of the
    /// phone's turn, 65 a second, redrew all ten cases, and a frame in six was dropped).
    @State private var leaning = CaseLean()
    private var tilt: CGPoint { leaning.tilt }
    private var lit: Double { leaning.lit }
    @State private var pulls = CaseAmounts()   // the folded case under the pointer, pulled out a little (1), the rest 0
    @State private var pulled = -1             // which one that is (-1: none)
    @State private var pulledLight = UnitPoint(x: 0.5, y: 0.3)   // where the pointer is on its front
    @State private var moving = false          // the cases are moving to a new middle: the pointer pulls none out
    @State private var hovering = false    // the pointer is over the stage: a sideways swipe moves the cases
    @State private var swipeMonitor: Any?
    @State private var swipes = 0          // swipe events so far: the last one's wait settles the cases
    @FocusState private var focused: Bool

    /// Where the cases are: the index less the drag, held back past either end (half a case at most, as a rubber band).
    private var position: Double {
        let p = Double(index) - Double(drag / (size * 0.35)), n = Double(count - 1)
        func pull(_ x: Double) -> Double { 0.55 * (1 - 1 / (1 + x * 1.8)) }
        return p < 0 ? -pull(-p) : p > n ? n + pull(p - n) : p
    }
    private var height: CGFloat { CoverFlowGeometry.top(size) + size * 1.38 }   // the room above, the box, its reflection
    private var spring: Animation { paced(.spring(response: 0.45, dampingFraction: 0.86)) }
    /// Opening and closing, either way the case opens: 95% of the way 0.25 s after the click (user 2026-11-18: "改成
    /// 0.25秒"), with no overshoot (user 2026-11-19: "去掉 5% 反弹 改成 0%", after "中间交锋处 一直不稳定 左边的 盒子
    /// 一直来回晃动": 5% past and back took the cover some 9° past flat, in perspective); critically damped.
    private var turnSpring: Animation { paced(.spring(response: 0.4, dampingFraction: 1)) }
    private var openingTurn: Animation { paced(.spring(response: 0.33, dampingFraction: 1)) }

    var body: some View {
        GeometryReader { geo in
            // the stage as it is shown (with the pulled-out case where it goes): clicks, the pointer and the name follow
            // what the player sees (2026-11-17: judged at rest, the case under the pointer and the one pulled out
            // differed once the others moved aside, and the row jumped)
            let stage = CoverFlowGeometry(count: count, size: size, width: geo.size.width, height: height, position: position,
                                          pulls: pulls)
            ZStack(alignment: .topLeading) {
                CoverFlowStage(count: count, size: size, width: geo.size.width, height: height, position: position,
                               // the book opens over the stage's own fading-out: the cases need not step back frame by
                               // frame under it (each frame redrew every case's outline, MNG_COVERFLOW_TRACE 2026-11-19)
                               flip: opening == .book ? 0 : Double(open), hideCenter: expanded, leaning: leaning,
                               pulls: pulls, press: press, pulledLight: pulledLight,
                               front: front, spine: spine, back: back)
                    .opacity(Double(1 - open))   // the opened case covers the stage
                    .contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { tap in
                        guard !dragging else { return }
                        // turned over by this same click (the press below opens it as the button comes up, and this
                        // tap ends after it): not a click to close it (2026-11-18, "翻页超快 然后黑屏": it closed at once
                        // and the turn's own animation then left the stage hidden)
                        if flipped {
                            CoverFlowTrace.note("tap while turned over, opened by this click \(pressedMiddle)")
                            if !pressedMiddle { turn(false) }
                            return
                        }
                        guard let i = stage.cover(at: tap.location) else { return }
                        CoverFlowTrace.note("tap case \(i) (middle \(index)), pulled \(pulled)")
                        if i != index { go(to: i) }   // the middle case turns on the button coming up (below)
                    })
                    // the mouse button down on the middle case presses it in (user 2026-11-18: "点击的时候 再来一下 感觉
                    // 点击上了的效果"); up, however long it was held ("鼠标松开", "长按 也翻页"), it turns from there,
                    // springing back out as it goes ("从默认状态开始的 所以不连贯": turn(true)); dragged away, it does not
                    .simultaneousGesture(DragGesture(minimumDistance: 0)
                        .onChanged { v in
                            guard !pressing else { return }
                            pressing = true
                            pressedMiddle = !flipped && !dragging && stage.cover(at: v.startLocation) == index
                            guard pressedMiddle else { return }
                            withAnimation(pressDown) { press = 1 }
                        }
                        .onEnded { v in
                            pressing = false
                            let moved = hypot(v.translation.width, v.translation.height)
                            guard pressedMiddle, !flipped, moved < 6,
                                  stage.cover(at: v.location) == index else { release(); return }
                            CoverFlowTrace.note("click middle case \(index)")
                            openMiddle()
                        })
                    .onContinuousHover(coordinateSpace: .local) { phase in
                        switch phase {
                        case .active(let p): hovering = true; lean(at: p, stage)
                        case .ended: hovering = false; lean(at: nil, stage)
                        }
                    }
                // no name floating over the pulled-out case (user 2026-11-19: "盒子上面的 这个名字 去掉"): its spine and
                // front say it
                if expanded {
                    // its reflection on the floor, as every case has: turning and growing, shrinking and settling with
                    // it (user 2026-11-17: "返回的时候 底下阴影 没有跟上 要真实 阴影 也是从大变小"); upside down about
                    // the line the stage's reflections turn about (boxTransform: floorY + gap/2), faint and fading
                    CaseDetail(stage: stage, open: open, opening: opening, inside: inside(index), cartridge: cartridge(index), close: {}) {
                        front(index)
                    } panel: {
                        detail(index, false)
                    }
                    .frame(width: geo.size.width, height: height)
                    .scaleEffect(x: 1, y: -1, anchor: UnitPoint(x: 0.5, y: (stage.floorY + stage.gap / 2) / height))
                    .compositingGroup()
                    .opacity(0.32)
                    .mask(LinearGradient(stops: [.init(color: .black, location: (stage.floorY + stage.gap) / height),
                                                 .init(color: .clear, location: min(1, (stage.floorY + stage.gap + size * 0.42) / height))],
                                         startPoint: .top, endPoint: .bottom))
                    .allowsHitTesting(false)
                    // the middle case turning and growing into the opened case: cover left, actions right
                    CaseDetail(stage: stage, open: open, lean: coverLean, lit: coverLit, press: detailPress, opening: opening,
                               inside: inside(index), cartridge: cartridge(index), close: { turn(false) }) {
                        front(index)
                    } panel: {
                        detail(index, true)
                    }
                    .onContinuousHover(coordinateSpace: .local) { phase in   // the stage's coordinates (it fills it)
                        switch phase {
                        case .active(let p): leanCover(at: p, stage)
                        case .ended: leanCover(at: nil, stage)
                        }
                    }
                }
            }
            .frame(width: geo.size.width, height: height, alignment: .topLeading)
        }
        .frame(height: height)
        .background(LinearGradient(colors: [Color(white: 0.17), Color(white: 0.03)], startPoint: .top, endPoint: .bottom))
        .clipShape(RoundedRectangle(cornerRadius: corner, style: .continuous))
        .simultaneousGesture(DragGesture(minimumDistance: 6)
            .onChanged { v in turn(false); lean(at: nil, nil); release(); dragging = true; drag = v.translation.width }
            .onEnded { v in
                // it carries on with the drag's speed (where the drag would have ended), then settles on a case
                let thrown = Double(index) - Double(v.predictedEndTranslation.width / (size * 0.35))
                let to = min(max(Int(thrown.rounded()), 0), count - 1)
                go(to: to)
                DispatchQueue.main.async { dragging = false }   // the click a drag ends with is not a tap
            }, including: flipped ? .subviews : .all)   // turned or opened: its own controls take the pointer
        .overlay(alignment: .leading) { if arrows { arrow("chevron.left", by: -1) } }
        .overlay(alignment: .trailing) { if arrows { arrow("chevron.right", by: 1) } }
        .focusable().focused($focused).focusEffectDisabled()
        .onKeyPress(.leftArrow) { move(-1); return .handled }
        .onKeyPress(.rightArrow) { move(1); return .handled }
        // turned over, Return is the back's own (its start button is the default action)
        .onKeyPress(.return) {   // pressed and let go as a click is
            if flipped { return .ignored }
            withAnimation(pressDown) { press = 1 }
            let at = index
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {   // pressed in: it turns from there
                if index == at, !dragging { openMiddle() } else { release() }
            }
            return .handled
        }
        .onKeyPress(.escape) { guard flipped else { return .ignored }; turn(false); return .handled }
        .onChange(of: index) { turn(false); settleMove() }
        .onChange(of: launches) { turn(false) }   // a game was started: back to the games, the way the case was opened
        .onAppear {
            focused = true; watchSwipes()
            turns?.onTurn = { turn in follow(turn) }   // the phone's turn (MotionTilt)
        }
        .onDisappear {
            #if os(macOS)
            if let m = swipeMonitor { NSEvent.removeMonitor(m) }
            #endif
            swipeMonitor = nil
        }
        .accessibilityIdentifier("cover-flow")
    }

    /// The pointer over the cases (nil: gone). The middle case leans away under it, its light where the pointer is; a
    /// folded case under it is pulled out a little, as off a shelf, its game's name over it, the cases beyond it moving
    /// aside (the sketch's, user 2026-11-17). Which case is under the pointer is the stage as shown (`stage.cover`, with
    /// the current pull: from the middle outwards, the first whose front holds the point; a nearer case covers a farther
    /// one), so the case that reacts is the one the player sees there. Only while the fronts show and nothing moves.
    /// The middle case leans and lights as the phone turns, as under the Mac's pointer (lean(at:)): only while it rests
    /// closed in the middle; its light grows with the turn, none while the phone is held still. Followed as it comes (the
    /// motion is smooth already, 60 a second), not on a spring.
    private func follow(_ turn: CGPoint) {
        let resting = !flipped && !dragging && !moving && abs(position - Double(index)) < 0.01
        let to = resting ? turn : .zero
        let on = resting ? min(1, Double(hypot(turn.x, turn.y)) * 2) : 0
        guard to != tilt || on != lit else { return }
        var still = Transaction()
        still.disablesAnimations = true
        withTransaction(still) { leaning.set(to, on) }
    }

    private func lean(at p: CGPoint?, _ stage: CoverFlowGeometry?) {
        var to = CGPoint.zero, on = 0.0, side = -1
        if let p, let stage, !flipped, !dragging, !moving, abs(position - Double(index)) < 0.01, let i = stage.cover(at: p) {
            if i == index {
                to = CGPoint(x: (p.x - stage.backCenter.x) / (stage.w / 2), y: (p.y - stage.backCenter.y) / (stage.h / 2))
                on = 1
            } else {
                side = i
                let q = stage.frontQuad(i), xs = q.map(\.x), ys = q.map(\.y)
                let left = xs.min() ?? 0, right = xs.max() ?? 1, top = ys.min() ?? 0, bottom = ys.max() ?? 1
                pulledLight = UnitPoint(x: (p.x - left) / max(1, right - left), y: (p.y - top) / max(1, bottom - top))
            }
        }
        if to != tilt || on != lit {
            withAnimation(paced(.spring(response: 0.35, dampingFraction: 0.8))) { leaning.set(to, on) }
        }
        if side != pulled { CoverFlowTrace.note("pull case \(side) (was \(pulled)), middle \(index), position \(position)") }
        if side != pulled {
            withAnimation(paced(.spring(response: 0.32, dampingFraction: 0.8))) { pulled = side; pulls = CaseAmounts.one(side, of: count) }
        }
    }

    /// The opened case's cover leans and lights under the pointer as the middle case does (user 2026-11-18: "封面 要依然
    /// 支持 鼠标效果"): only once it has opened (open 1), the cover where CaseDetail draws it (coverCenter, coverScale).
    private func leanCover(at p: CGPoint?, _ stage: CoverFlowGeometry) {
        var to = CGPoint.zero, on: CGFloat = 0
        if let p, flipped, open >= 1 {
            let x = (p.x - stage.coverCenter.x) / (stage.w * stage.coverScale / 2)
            let y = (p.y - stage.coverCenter.y) / (stage.h * stage.coverScale / 2)
            if abs(x) <= 1, abs(y) <= 1 { to = CGPoint(x: x, y: y); on = 1 }
        }
        guard to != coverLean || on != coverLit else { return }
        withAnimation(paced(.spring(response: 0.35, dampingFraction: 0.8))) { coverLean = to; coverLit = on }
    }

    /// A new middle (a click, the arrows, a drag or a swipe): the case pulled out under the pointer goes back with the
    /// same spring as the move and none is pulled out until the move has settled (2026-11-17, the frame record: a clicked
    /// case kept its pull all the way to the middle, and the pointer's next move took it back, a jump after the move).
    private func settleMove() {
        withAnimation(spring) {
            pulled = -1
            pulls = CaseAmounts.one(-1, of: count)
            leaning.set(.zero, 0)
        }
    }

    /// Moves the cases to put case `to` in the middle (every way of moving comes here): `moving` until the move's own
    /// animation is over, so the pointer pulls no case out of a row that is still sliding.
    private func go(to: Int) {
        moving = true
        withAnimation(spring, completionCriteria: .removed) { index = to; drag = 0 } completion: { moving = false }
    }

    /// A two-finger sideways swipe on the trackpad moves the cases while the pointer is over the stage, with the
    /// trackpad's own momentum after the fingers lift, and settles on a case when it stops; an up-and-down swipe still
    /// scrolls the page. (SwiftUI has no scroll-wheel event of its own here: an app-local NSEvent monitor.)
    /// On an iPhone or iPad the finger's drag (above) is the swipe: nothing to watch.
    private func watchSwipes() {
        #if os(macOS)
        guard swipeMonitor == nil else { return }
        swipeMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
            let dx = event.scrollingDeltaX, dy = event.scrollingDeltaY, fine = event.hasPreciseScrollingDeltas
            let used = MainActor.assumeIsolated { swipe(dx: dx, dy: dy, fine: fine) }
            return used ? nil : event
        }
        #endif
    }

    private func swipe(dx: CGFloat, dy: CGFloat, fine: Bool) -> Bool {
        guard hovering, fine, !flipped, abs(dx) > abs(dy) * 1.2 || (dragging && abs(dx) >= abs(dy)) else { return false }
        turn(false); lean(at: nil, nil)
        dragging = true
        drag += dx
        swipes += 1
        let mine = swipes
        Task { @MainActor in   // settles once the swipe and its momentum have stopped for a moment
            try? await Task.sleep(for: .milliseconds(120))
            guard mine == swipes else { return }
            let to = min(max(Int(position.rounded()), 0), count - 1)
            go(to: to)
            dragging = false
        }
        return true
    }

    /// Opens the middle case (true): it turns and grows over the stage at once ("翻转的同时 放大 不是翻转完"), one spring
    /// for both, CaseDetail drawing it from its front at rest (the stage hides its own copy of it), the others stepping
    /// back as it goes. Closes it (false): the same backwards, ending on its front where it stands.
    private var pressDown: Animation { paced(.spring(response: 0.14, dampingFraction: 0.9)) }

    /// Lets the pressed middle case go: a bouncy spring, so it comes back a little past where it rests, then settles.
    private func release() {
        guard press != 0 else { return }
        withAnimation(paced(.spring(response: 0.28, dampingFraction: 0.42))) { press = 0 }
    }

    /// The middle case clicked: onOpen when it is set (it springs back out), else it turns over and opens.
    private func openMiddle() {
        if let onOpen {
            release()
            onOpen(index)
        } else {
            turn(true)
        }
    }

    private func turn(_ over: Bool) {
        if over {
            guard !flipped else { return }
            // CaseDetail takes the case over as the stage shows it: leaning under the pointer, lit, pressed in; the turn
            // lays the lean and the light down, and the press springs back out (a little past) as it goes (user
            // 2026-11-18: "翻页 不是在按下 再一次压下后 开始的 … 不连贯")
            coverLean = tilt; coverLit = CGFloat(lit); detailPress = CGFloat(press)
            press = 0   // the stage's own copy is hidden from now on
            open = 0; expanded = true; flipped = true
            DispatchQueue.main.async {   // inserted at 0 first, then it moves
                guard flipped else { return }   // closed again before it began: nothing to open
                withAnimation(openingTurn) { open = 1; coverLean = .zero; coverLit = 0 }
                withAnimation(paced(.spring(response: 0.3, dampingFraction: 0.5))) { detailPress = 0 }
            }
        } else {
            guard flipped else { return }
            flipped = false
            withAnimation(turnSpring, completionCriteria: .removed) { open = 0; coverLean = .zero; coverLit = 0 } completion: {
                if !flipped { expanded = false }
            }
        }
    }

    private func arrow(_ symbol: String, by step: Int) -> some View {
        let possible = (0..<count).contains(index + step)
        return Button { move(step) } label: {
            Image(systemName: symbol).font(.title3.weight(.bold)).foregroundStyle(.black)
                .frame(width: 36, height: 36).background(.white.opacity(0.85), in: Circle()).shadow(radius: 4)
        }
        .buttonStyle(.plain).padding(.horizontal, 10)
        .opacity(possible && !flipped ? 1 : 0).disabled(!possible || flipped)
    }

    private func move(_ step: Int) {
        guard !expanded else { return }   // the opened case keeps the arrow keys for itself
        turn(false)
        let to = min(max(index + step, 0), count - 1)
        go(to: to)
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
    var flip: Double = 0   // the middle box turned over: 0 its front, 1 its back (CoverFlow.turn)
    var pointer = CGPoint.zero   // the pointer over the middle front, -1...1 from its centre (CoverFlow.lean)
    var pulls = CaseAmounts()    // how far each folded case is pulled out under the pointer (CoverFlow.lean)
    var press: Double = 0        // the middle case pressed by a click: further in, leaning more (CoverFlow.pressDown)
    /// Which way the middle case turns over: +1 its right edge going back first (half way its spine shows), -1 the other
    /// way (its opening edge shows; user 2026-11-17: "试一试 相反的 翻转方向"). Its back ends the same either way.
    static let turnWay: Double = -1

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

    /// The box in the middle (the one that turns over).
    var center: Int { min(max(Int(position.rounded()), 0), count - 1) }
    /// How far it comes towards the player as it turns over: its back then shows 8% larger than a front.
    var lift: CGFloat { eye * (1 - 1 / 1.08) - depth / 2 }
    /// The back of the turned-over box faces the camera at depth/2 + lift: its scale and centre on the stage, where the
    /// live back is laid (camera.m34 = -1/eye, then the move to the stage's centre line).
    var backScale: CGFloat { 1 / (1 - (depth / 2 + lift) / eye) }
    var backCenter: CGPoint { CGPoint(x: width / 2, y: floorY - h / 2) }
    /// The opened case's cover (CaseDetail): as large as its left 44% and the stage's height let it be, and its centre.
    var coverScale: CGFloat { min(height * 0.84 / h, width * 0.44 * 0.82 / w) }
    var coverCenter: CGPoint { CGPoint(x: width * 0.22, y: height / 2) }
    /// The middle case's front at rest is at depth/2 towards the camera: its scale on the stage (centred at backCenter),
    /// where CaseDetail starts.
    var frontScale: CGFloat { 1 / (1 - (depth / 2) / eye) }
    /// The middle front pressed `press` in (boxTransform: w·0.1·press further from the camera): its scale against the
    /// front at rest, for CaseDetail to start the turn where the press left it.
    func pressedScale(_ press: CGFloat) -> CGFloat { (eye - depth / 2) / (eye - depth / 2 + w * 0.1 * press) }

    /// The middle case leaning away under the pointer (the sketch's: up to 9° sideways, 7° up and down): the part under
    /// the pointer goes back, as if pressed. Rotations as `boxTransform`'s (row vectors).
    var lean: CATransform3D {
        let deeper = 1 + 0.8 * press   // a click presses it further where it is pressed
        let ay = Double(pointer.x) * 9 * deeper * Double.pi / 180, ax = -Double(pointer.y) * 7 * deeper * Double.pi / 180
        var ry = CATransform3DIdentity
        ry.m11 = CGFloat(cos(ay)); ry.m13 = CGFloat(-sin(ay)); ry.m31 = CGFloat(sin(ay)); ry.m33 = CGFloat(cos(ay))
        var rx = CATransform3DIdentity
        rx.m22 = CGFloat(cos(ax)); rx.m23 = CGFloat(sin(ax)); rx.m32 = CGFloat(-sin(ax)); rx.m33 = CGFloat(cos(ax))
        return CATransform3DConcat(ry, rx)
    }
    /// How far box i is turned over (the middle one) and how far it steps back for it (the others).
    func turned(_ i: Int) -> (over: Double, away: Double) { i == center ? (flip, 0) : (0, flip) }

    /// How far the folded cases nearer the middle than i, on its side, are pulled out: i moves aside by that.
    func pushed(_ i: Int) -> Double {
        let d = Double(i) - position
        return (0..<count).reduce(0) { sum, j in
            let dj = Double(j) - position
            return j != i && (dj > 0) == (d > 0) && abs(dj) < abs(d) ? sum + pulls[j] : sum
        }
    }

    /// The box's turn and place: turned towards the middle, the first beside it at a gap, the rest packed closer, the
    /// turned ones set back. The middle box turned over spins round its upright axis and comes forward; the others
    /// step back and apart for it. A folded case under the pointer is drawn a little out of the row, as a book on a
    /// shelf: the cases beyond it move aside a fifth of a case, so more of its front shows, and it comes forward a
    /// little (a tenth of a case), turns 15% towards the player and rises 1.5%, staying behind the nearer ones (user
    /// 2026-11-17: "效果 太夸张 都出来了", after it came all the way out over its neighbours).
    func boxTransform(_ i: Int, mirrored: Bool) -> CATransform3D {
        let (d, side) = place(i)
        let (over, away) = turned(i)
        let pull = CGFloat(pulls[i]), aside: CGFloat = d > 0 ? 1 : d < 0 ? -1 : 0
        let x = (CGFloat(side) * w * 1.02 + CGFloat(d - side) * w * 0.3) * (1 + 0.35 * CGFloat(away))
            + aside * (CGFloat(pushed(i)) * w * 0.2 + pull * w * 0.02)
        let y = -pull * h * 0.015
        let pressed = i == center ? CGFloat(press) * w * 0.1 : 0   // the middle case pushed in by a click, then back
        let z = -CGFloat(abs(side)) * w * 0.6 + lift * CGFloat(over) - w * 0.5 * CGFloat(away) + pull * w * 0.1 - pressed
        let a = -side * tilt * (1 - 0.15 * Double(pull)) + Double.pi * over * Self.turnWay
        var r = CATransform3DIdentity
        r.m11 = CGFloat(cos(a)); r.m13 = CGFloat(-sin(a))
        r.m31 = CGFloat(sin(a)); r.m33 = CGFloat(cos(a))
        var m = i == center ? CATransform3DConcat(lean, r) : r
        if mirrored {   // upside down about the floor (the box's bottom, y = h/2 from its middle), a gap below it
            var flip = CATransform3DIdentity
            flip.m22 = -1; flip.m42 = h + gap
            m = CATransform3DConcat(flip, m)
        }
        m = CATransform3DConcat(m, CATransform3DMakeTranslation(x, y, z))
        var camera = CATransform3DIdentity
        camera.m34 = -1 / eye
        m = CATransform3DConcat(m, camera)
        return CATransform3DConcat(m, CATransform3DMakeTranslation(width / 2, floorY - h / 2, 0))
    }

    /// The front: its view (w x h) on the box's front plane.
    var frontPlacement: CATransform3D { CATransform3DMakeTranslation(-w / 2, -h / 2, depth / 2) }
    /// The back: its view (w x h) on the back plane, mirrored left to right so it reads the right way round once the box
    /// has turned half a circle.
    var backPlacement: CATransform3D {
        var m = CATransform3DIdentity
        m.m11 = -1; m.m41 = w / 2; m.m42 = -h / 2; m.m43 = -depth / 2
        return m
    }

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

    /// The order to draw in: the farthest first (a pulled-out case keeps its place: the nearer ones stay over it).
    var drawOrder: [Int] { (0..<count).sorted { abs(place($0).d) > abs(place($1).d) } }

    /// The four corners of box i's front on the stage (top left, top right, bottom right, bottom left).
    func frontQuad(_ i: Int) -> [CGPoint] {
        let m = CATransform3DConcat(frontPlacement, boxTransform(i, mirrored: false))
        return [CGPoint(x: 0, y: 0), CGPoint(x: w, y: 0), CGPoint(x: w, y: h), CGPoint(x: 0, y: h)].map { project($0, m) }
    }

    /// The box whose front is under a point of the stage, the nearest first.
    func cover(at point: CGPoint) -> Int? {
        drawOrder.reversed().first { inside(point, frontQuad($0)) }
    }

    /// The rounded outline of a w x h view (the case's corner radius), placed by m and projected: a polygon on the stage.
    /// The corner layers through a box are drawn from these in one Canvas (CoverFlowStage.box).
    func outline(_ m: CATransform3D) -> Path {
        var path = Path()
        let r = corner, steps = 6
        let centres = [CGPoint(x: w - r, y: r), CGPoint(x: w - r, y: h - r), CGPoint(x: r, y: h - r), CGPoint(x: r, y: r)]
        for (k, c) in centres.enumerated() {   // top right, bottom right, bottom left, top left: a quarter circle each
            let start = -Double.pi / 2 + Double(k) * Double.pi / 2
            for s in 0...steps {
                let t = start + Double(s) / Double(steps) * Double.pi / 2
                let p = project(CGPoint(x: c.x + r * CGFloat(cos(t)), y: c.y + r * CGFloat(sin(t))), m)
                if k == 0 && s == 0 { path.move(to: p) } else { path.addLine(to: p) }
            }
        }
        path.closeSubpath()
        return path
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

/// The boxes for one position and turn. Animatable by both, so a move or a turn redraws every frame of the spring.
private struct CoverFlowStage<Front: View, Spine: View, Back: View>: View, Animatable {
    let count: Int
    let size: CGFloat
    let width: CGFloat
    let height: CGFloat
    var position: Double
    var flip: Double
    let hideCenter: Bool     // the middle case is drawn by CaseDetail (turning and growing over the stage)
    /// The middle case's lean and light: watched by the middle case alone (MiddleLean), so they redraw nothing else.
    let leaning: CaseLean
    var pulls: CaseAmounts   // the folded cases pulled out under the pointer
    var press: Double        // the middle case pressed by a click (CoverFlow.pressDown)
    let pulledLight: UnitPoint   // where the pointer is on the pulled-out case's front
    let front: (Int) -> Front
    let spine: (Int) -> Spine
    let back: (Int, Bool) -> Back

    var animatableData: AnimatablePair<AnimatablePair<Double, Double>, AnimatablePair<CaseAmounts, Double>> {
        get { AnimatablePair(AnimatablePair(position, flip), AnimatablePair(pulls, press)) }
        set {
            position = newValue.first.first; flip = newValue.first.second
            pulls = newValue.second.first; press = newValue.second.second
        }
    }

    var body: some View {
        let g = CoverFlowGeometry(count: count, size: size, width: width, height: height, position: position, flip: flip,
                                  pulls: pulls, press: press)
        let _ = CoverFlowTrace.frame(position: position, flip: flip, lit: 0, tilt: (0, 0), pulls: pulls, order: g.drawOrder)
        ZStack(alignment: .topLeading) {
            // the reflections: the boxes upside down under the floor, fading into it
            ZStack(alignment: .topLeading) {
                ForEach(g.drawOrder.filter { !hideCenter || $0 != g.center }, id: \.self) { i in placed(i, g, mirrored: true) }
            }
            .frame(width: width, height: height, alignment: .topLeading)   // the mask covers the whole stage, not one face
            // one picture first, then faint: an opacity on a plain ZStack is given to every face on its own, and the
            // farther boxes' reflections showed through the nearer ones (user 2026-11-17: "阴影 透视了")
            .compositingGroup()
            .opacity(0.32)
            .mask(LinearGradient(stops: [.init(color: .black, location: (g.floorY + g.gap) / height),
                                         .init(color: .clear, location: min(1, (g.floorY + g.gap + size * 0.42) / height))],
                                 startPoint: .top, endPoint: .bottom))
            ForEach(g.drawOrder.filter { !hideCenter || $0 != g.center }, id: \.self) { i in placed(i, g, mirrored: false) }
        }
        .frame(width: width, height: height, alignment: .topLeading)
        .allowsHitTesting(false)
    }

    /// A case in its place: the middle one through MiddleLean, which alone redraws as it leans and lights.
    @ViewBuilder
    private func placed(_ i: Int, _ g: CoverFlowGeometry, mirrored: Bool) -> some View {
        if i == g.center {
            MiddleLean(leaning: leaning) { tilt, lit in
                var leant = g
                leant.pointer = tilt
                return AnyView(box(i, leant, mirrored: mirrored, lit: lit))
            }
        } else {
            box(i, g, mirrored: mirrored, lit: 0)
        }
    }

    @ViewBuilder
    private func box(_ i: Int, _ g: CoverFlowGeometry, mirrored: Bool, lit: Double) -> some View {
        let tiltX = Double(g.pointer.x), tiltY = Double(g.pointer.y)
        let side = g.place(i).side
        let (over, away) = g.turned(i)
        let turning = over > 0.02 && over < 0.98   // half way round, its spine faces the camera
        let leaning = i == g.center ? Double(g.pointer.x) : 0   // leaning right shows its spine a little, left its edge
        let m = g.boxTransform(i, mirrored: mirrored)
        // the turned ones darker, as away from the light, a pulled-out one lit again; dimmer behind a turned-over one
        let shade = abs(side) * 0.18 * (1 - 0.85 * pulls[i]) + away * 0.45
        ZStack(alignment: .topLeading) {
            // only the side that faces the camera: a box turned towards the middle shows its outer side, so one on the
            // left shows its spine (the hinge side), one on the right its opening edge (each side's normal against the
            // line of sight); the middle one turning over (rotated by π·over) shows its spine
            if abs(side) > 0.02 || turning || abs(leaning) > 0.02 {
                // the rounded outline's layers through the box in one Canvas, their projected outlines filled: as twelve
                // projected views each they were 144 of the ~400 views SwiftUI placed again every frame (sample, 2026-11-17:
                // the main thread busy 62% while the pointer moved over the cases, nearly all in its display list)
                let layers = (0..<CoverFlowGeometry.slices).map { g.outline(CATransform3DConcat(g.slicePlacement($0), m)) }
                let bounds = layers.reduce(CGRect.null) { $0.union($1.boundingRect) }.insetBy(dx: -1, dy: -1)
                let plastic = Color(red: 0.6 * (1 - shade), green: 0.03 * (1 - shade), blue: 0.06 * (1 - shade))
                if !bounds.isNull {
                    Canvas { ctx, _ in
                        ctx.translateBy(x: -bounds.minX, y: -bounds.minY)
                        for layer in layers { ctx.fill(layer, with: .color(plastic)) }
                    }
                    .frame(width: bounds.width, height: bounds.height)
                    .offset(x: bounds.minX, y: bounds.minY)
                }
            }
            if side < -0.02 || (turning && CoverFlowGeometry.turnWay > 0) || leaning > 0.02 {
                spine(i).frame(width: g.depth, height: g.h - 2 * g.corner)
                    .overlay(Color.black.opacity(0.1 + shade))
                    // hits: none, CoverFlowStage takes no clicks; CoverFlow finds the case from its geometry (cover(at:))
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.sidePlacement(left: true), m)))
            } else if side > 0.02 || (turning && CoverFlowGeometry.turnWay < 0) || leaning < -0.02 {
                CaseEdge().frame(width: g.depth, height: g.h - 2 * g.corner)
                    .overlay(Color.black.opacity(0.1 + shade))
                    // hits: none, CoverFlowStage takes no clicks
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.sidePlacement(left: false), m)))
            }
            if over > 0.5 {   // past half way the back faces the camera
                back(i, false).frame(width: g.w, height: g.h)
                    .overlay(CaseRim())
                    .clipShape(RoundedRectangle(cornerRadius: g.corner, style: .continuous))
                    // hits: none, CoverFlowStage takes no clicks
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.backPlacement, m)))
            } else {
                front(i).frame(width: g.w, height: g.h)
                    // its layers move apart as it leans, the middle case only (CoverArt.swift caseParallax)
                    .environment(\.caseParallax, i == g.center ? CGPoint(x: tiltX, y: tiltY) : .zero)
                    .overlay(CaseFront(light: mirrored ? 0 : i == g.center ? lit + 0.35 * press : pulls[i],
                                       at: i == g.center ? UnitPoint(x: (1 + tiltX) / 2, y: (1 + tiltY) / 2) : pulledLight))
                    .overlay(Color.black.opacity(shade))
                    .clipShape(RoundedRectangle(cornerRadius: g.corner, style: .continuous))   // the case's rounded corners
                    // hits: none, CoverFlowStage takes no clicks
                    .projectionEffect(ProjectionTransform(CATransform3DConcat(g.frontPlacement, m)))
            }
        }
    }
}

/// The middle case's lean (-1...1 across and down, the pointer's place on it or the phone's turn) and its light (0...1),
/// held by CoverFlow but watched only by MiddleLean: a change redraws the middle case (and its reflection), not the
/// stage, the other cases and their outlines (user 2026-11-19, the iPhone: "越用越慢", "陀螺仪 效果 慢半拍").
@MainActor
final class CaseLean: ObservableObject {
    @Published private(set) var tilt = CGPoint.zero
    @Published private(set) var lit = 0.0

    func set(_ tilt: CGPoint, _ lit: Double) {
        if self.tilt != tilt { self.tilt = tilt }
        if self.lit != lit { self.lit = lit }
    }
}

/// The phone's turns handed to the Cover Flow by a call (MotionTilt sends, CoverFlow.follow receives), so the view
/// holding the stage is not drawn again for each reading.
@MainActor
final class TurnFeed {
    var onTurn: ((CGPoint) -> Void)?
    func send(_ turn: CGPoint) { onTurn?(turn) }
}

/// The middle case, drawn again whenever its lean or light changes (and only it); animated as they spring (the Mac's
/// pointer) or followed as they come (the phone's turn).
private struct MiddleLean: View {
    @ObservedObject var leaning: CaseLean
    let draw: (CGPoint, Double) -> AnyView

    var body: some View {
        Leant(x: Double(leaning.tilt.x), y: Double(leaning.tilt.y), lit: leaning.lit, draw: draw)
    }

    private struct Leant: View, Animatable {
        var x: Double
        var y: Double
        var lit: Double
        let draw: (CGPoint, Double) -> AnyView

        var animatableData: AnimatablePair<Double, AnimatablePair<Double, Double>> {
            get { AnimatablePair(x, AnimatablePair(y, lit)) }
            set { x = newValue.first; y = newValue.second.first; lit = newValue.second.second }
        }

        var body: some View {
            let _ = CoverFlowTrace.lean(x, y, lit)
            draw(CGPoint(x: x, y: y), lit)
        }
    }
}

/// The case's red plastic rim round an insert (a back has no clear sleeve's shine).
struct CaseRim: View {
    var body: some View {
        GeometryReader { geo in
            RoundedRectangle(cornerRadius: geo.size.width * 0.05, style: .continuous)
                .strokeBorder(caseRed, lineWidth: geo.size.width * 0.03)
        }
        .allowsHitTesting(false)
    }
}

/// A case's back, turned over (scheme A): the red band with the game's name, and the dark insert holding `content` (the
/// game's actions, HomePage GameActions; what a build needs, for a game not built yet).
struct CaseBack<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    var body: some View {
        GeometryReader { geo in
            VStack(spacing: 0) {
                ZStack {
                    LinearGradient(colors: [caseRed, Color(red: 0.74, green: 0.04, blue: 0.08)], startPoint: .top, endPoint: .bottom)
                    Text(title)
                        .font(.system(size: geo.size.height * CaseHeader.share * 0.36, weight: .heavy)).foregroundStyle(.white)
                        .lineLimit(1).minimumScaleFactor(0.5)
                        .padding(.horizontal, geo.size.width * 0.08)
                }
                .frame(height: geo.size.height * CaseHeader.share)
                VStack(alignment: .leading, spacing: 6) {
                    content
                    Spacer(minLength: 0)
                    Text(T.turnBack).font(.system(size: 9)).foregroundStyle(.tertiary).frame(maxWidth: .infinity)
                }
                .padding(.horizontal, geo.size.width * 0.075).padding(.top, 9).padding(.bottom, geo.size.width * 0.05)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .background(LinearGradient(colors: [Color(white: 0.15), Color(white: 0.08)], startPoint: .top, endPoint: .bottom))
            }
        }
        .environment(\.colorScheme, .dark)
    }
}

/// The case's red plastic (sides, rim).
let caseRed = Color(red: 0.86, green: 0.07, blue: 0.1)

/// A game case's front: the red plastic rim round the insert and the clear sleeve's shine over it; `light` (0...1) adds
/// the pointer's light on the sleeve, centred `at` (the sketch's moving highlight).
struct CaseFront: View {
    var light = 0.0
    var at = UnitPoint(x: 0.3, y: 0.2)

    var body: some View {
        GeometryReader { geo in
            let rim = geo.size.width * 0.03
            ZStack {
                RoundedRectangle(cornerRadius: geo.size.width * 0.05, style: .continuous)
                    .strokeBorder(caseRed, lineWidth: rim)
                LinearGradient(stops: [.init(color: .white.opacity(0.3), location: 0), .init(color: .white.opacity(0.07), location: 0.42),
                                       .init(color: .clear, location: 0.48)], startPoint: .topLeading, endPoint: .bottomTrailing)
                    .padding(rim)
                    .opacity(1 - 0.5 * light)
                if light > 0.01 {
                    RadialGradient(colors: [.white.opacity(0.42 * light), .white.opacity(0.12 * light), .clear],
                                   center: at, startRadius: 0, endRadius: geo.size.width * 0.8)
                        .blendMode(.screen)
                        .padding(rim)
                }
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
