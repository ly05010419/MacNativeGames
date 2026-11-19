import SwiftUI
import UIKit

@main
struct LauncherApp: App {
    var body: some Scene {
        WindowGroup { HomeView().preferredColorScheme(.dark).statusBarHidden() }
    }
}

/// Which games are installed: a game's app is installed when its URL scheme opens (canOpenURL). Checked again whenever
/// the Launcher comes to the front, so a game installed from the Mac meanwhile shows up.
@MainActor
final class Library: ObservableObject {
    @Published private(set) var installed: Set<String> = []
    @Published var message: String?

    func refresh() {
        installed = Set(Games.all.filter { UIApplication.shared.canOpenURL(Games.url($0)) })
    }

    func launch(_ game: String) {
        guard installed.contains(game) else { return }   // not on this device: no note (user 2026-11-19: "去掉 还没安装 提示")
        UIApplication.shared.open(Games.url(game)) { [weak self] ok in
            if !ok { self?.message = T.couldNotOpen(T.gameName(game)) }
        }
    }
}

/// The whole screen, landscape only (user 2026-11-19: "ios 只要这个 不要和mac 一模一样", "默认 横版 不要竖版"): the Mac
/// Launcher's Cover Flow of game cases and nothing else, the same views (CoverFlow.swift, CoverArt.swift, shared with the
/// Mac), no arrows. A swipe moves the cases; a tap on the middle case turns it over and opens it, Play on its right.
struct HomeView: View {
    @StateObject private var library = Library()
    @Environment(\.scenePhase) private var phase
    @State private var chosen = Games.all[0]
    @State private var launches = 0   // a game started from the opened case: it turns back to its front, as on the Mac
    @StateObject private var motion = MotionTilt()   // the phone's turn leans the middle case, as the Mac's pointer

    /// Installed games first, then the rest, each in the catalog's order (as the Mac: built ones first).
    private var order: [String] {
        let have = Games.all.filter { library.installed.contains($0) }
        return have + Games.all.filter { !have.contains($0) }
    }
    private var center: Binding<Int> {
        Binding(get: { order.firstIndex(of: chosen) ?? 0 }, set: { chosen = order[$0] })
    }

    var body: some View {
        GeometryReader { screen in
            let cases = order
            // the stage is the whole screen, under the notch and the home indicator too (user 2026-11-19: "整个背景 都不是
            // 全屏， 做成全屏"): 1.47 cases high (CoverFlowGeometry.top 0.09 + the case and its reflection 1.38)
            let size = screen.size.height / 1.47
            // a tap on the middle case opens it as on the Mac (user 2026-11-19: "手机 点击 打开 效果 没有了", then "安装
            // iphone ，效果一样"): its cover swings open like a real case's, the cartridge on the left, its name and Play
            // on the right, over the game's picture
            CoverFlow(count: cases.count, index: center, size: size, launches: launches, name: { T.gameName(cases[$0]) },
                      arrows: false, opening: .book,
                      inside: { CoverImages.shared.cover(at: Games.picturePath(cases[$0]))?.picture },
                      corner: 0, groundBlur: 22, tiltInput: motion.tilt) { i in
                front(cases[i])
            } spine: { i in
                CaseSpine(game: cases[i])
            } back: { i, live in
                CaseBack(title: T.gameName(cases[i])) { panel(cases[i], live: live, wide: false) }
            } detail: { i, live in
                panel(cases[i], live: live, wide: true)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .ignoresSafeArea()
        .background(Color.black.ignoresSafeArea())
        .onAppear { library.refresh(); chosen = order.first ?? chosen; motion.start() }
        .onChange(of: phase) { _, now in
            if now == .active { library.refresh(); motion.start() } else { motion.stop() }   // no motion in the background
        }
        .alert(library.message ?? "", isPresented: Binding(get: { library.message != nil }, set: { if !$0 { library.message = nil } })) {
            Button("OK", role: .cancel) {}
        }
    }

    /// The opened case's right side (wide) and its back during the turn: the game's name and Play when it is on this
    /// device; nothing else (user 2026-11-19: "去掉 还没安装 提示"). live false: the drawn copy (turn, reflection).
    /// The start as the Mac's (user 2026-11-19: "红色按钮 改成绿色和mac 一样 然后小一点放下面 和mac 一样"): green, at
    /// the bottom of the opened case's right side, the column's width.
    @ViewBuilder private func panel(_ game: String, live: Bool, wide: Bool) -> some View {
        VStack(alignment: .leading, spacing: wide ? 10 : 8) {
            Text(T.gameName(game)).font(wide ? .title3.weight(.bold) : .callout.weight(.semibold)).lineLimit(2)
            Text(T.gamePlatforms(game)).font(wide ? .caption : .caption2).foregroundStyle(.secondary)
            Spacer(minLength: 0)
            if library.installed.contains(game) {
                Button {
                    launches += 1
                    library.launch(game)
                } label: {
                    Label(T.play, systemImage: "play.fill")
                        .font(.callout.weight(.semibold)).frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent).tint(.green).controlSize(wide ? .regular : .small)
            }
        }
        .allowsHitTesting(live)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        // in from the opened case's edges, off its top and bottom (user 2026-11-19: "右边 文字 和按钮 都往中间一点 离上下
        // 一定距离"): the phone's case is small, the Mac's margins alone kept them against its rim
        .padding(.horizontal, wide ? 10 : 0).padding(.vertical, wide ? 14 : 0)
        .environment(\.colorScheme, .dark)
    }

    /// A case's front, in its own colours, the same for every game, installed or not (user 2026-11-19: "不要变黑 正常颜色",
    /// "去掉 还没安装 提示").
    private func front(_ game: String) -> some View {
        let cover = CoverImages.shared.cover(at: Games.picturePath(game))   // decoded and blurred once, not on every frame
        return CoverFront(game: game, picture: cover?.picture, picturePath: cover?.path, backdrop: cover?.backdrop) {
            Text(T.gameName(game)).font(.headline).foregroundStyle(.white)
                .multilineTextAlignment(.center).padding(.horizontal, 12)
        }
    }
}
