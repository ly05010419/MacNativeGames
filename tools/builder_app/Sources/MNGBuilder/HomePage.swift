import AppKit
import SwiftUI

/// The first page: every game MNG Launcher builds, as cards. A game already built shows its own app's picture (the icon
/// its build made from the player's own files; the Launcher ships no game art) and opens its actions; a game not built
/// yet shows a plus that goes to the Build page with that game chosen (user 2026-11-12).
struct HomePage: View {
    @EnvironmentObject var model: Model
    @State private var chosen = HomePage.games[0]   // the case in the middle of the Cover Flow, by game (not by place:
                                                    // a game built moves forward and stays the chosen one)
    @State private var defaulted = false   // the first built game was chosen once, when the page first showed
    @State private var gamePage: String?   // the page a case's back opened (T.gamePages), shown under the Cover Flow
    @State private var launches = 0        // games started from a back: the Cover Flow grows the case over the stage

    static let games = ["oot", "totk", "botw", "eow", "odyssey", "acnh", "dkb", "pkp"]

    /// The cases' order (user 2026-11-17: "已经安装的游戏 放前面，没有安装的游戏盒子 放后面"): the built games, then the
    /// ones not built yet, each in the catalog's order.
    private var order: [String] {
        let built = Self.games.filter { g in model.installed.contains { $0.game == g } }
        return built + Self.games.filter { !built.contains($0) }
    }
    private var selected: String { order.contains(chosen) ? chosen : order[0] }
    private var center: Binding<Int> {
        Binding(get: { order.firstIndex(of: selected) ?? 0 }, set: { chosen = order[$0] })
    }

    /// The first built game is open when the page shows (user 2026-11-12: "启动 app 默认选中第一个游戏").
    private func chooseFirst() {
        if let built = model.homeGame {   // a build just finished: its game in the middle
            chosen = built; defaulted = true; model.homeGame = nil
            return
        }
        guard !defaulted, model.installedLoaded else { return }
        guard let first = order.first(where: { g in model.installed.contains { $0.game == g } }) else { return }
        chosen = first
        defaulted = true
    }

    /// Add this game (under the middle case, and on the back of a game not built yet): the Build page with it chosen.
    private func open(_ game: String) {
        guard model.installedLoaded, !model.installed.contains(where: { $0.game == game }) else { return }
        model.buildGame(game)
    }

    /// Whose job a box at the bottom shows (its own line says what the job is doing): the updated app's game, else
    /// its file name (an app another program updates may not be in the list); a build's game from the Build page.
    private var jobGame: String {
        if let running = model.updating {
            return model.installed.first { $0.app == running }?.game.map(T.gameName)
                ?? (running as NSString).lastPathComponent
        }
        return model.plan?.game.map(T.gameName) ?? ""
    }

    var body: some View {
        ScrollView(.vertical, showsIndicators: false) {   // no scroll bar beside the stage (user 2026-11-18: "去掉 滚动条")
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 14) {
                    Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 56, height: 56)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("MNG Launcher").font(.largeTitle.weight(.bold))
                        Text(T.homeSubtitle).foregroundStyle(.secondary)
                    }
                    Spacer()
                    // the terminal with an AI helper (TerminalTab.swift; user 2026-11-13)
                    Button { model.page = "terminal" } label: { Label(T.terminal, systemImage: "terminal") }
                    // a newer MNG Launcher: one click downloads it, puts it in this one's place and opens it again
                    // (LauncherUpdate.swift; user 2026-11-18: "放在 右上角，有更新 才显示")
                    LauncherUpdateButton()
                }
                LauncherUpdateFailure()   // why the update did not happen, with the plain download
                // The games as the iPod classic's Cover Flow (user 2026-11-17): every one is reached by the arrows,
                // the arrow keys, a drag or a click on a side cover, at any window width
                let cases = order
                CoverFlow(count: cases.count, index: center, launches: launches, name: { T.gameName(cases[$0]) }) { i in
                    let game = cases[i]
                    GameCard(game: game, app: model.installed.first { $0.game == game }, loading: !model.installedLoaded,
                             selected: false, cover: true, tap: {})
                } spine: { i in
                    CaseSpine(game: cases[i])
                } back: { i, live in
                    // the case turned over (scheme A, user 2026-11-17): the game's actions on its back
                    let game = cases[i]
                    CaseBack(title: T.gameName(game)) {
                        if let app = model.installed.first(where: { $0.game == game }) {
                            GameActions(app: app, section: $gamePage, live: live) { launches += 1 }.id(app.app)
                        } else {
                            EmptyBack(ready: model.installedLoaded, live: live) { open(game) }
                        }
                    }
                } detail: { i, live in
                    // the case opened over the stage ("左边是封面 右边现在的 按钮"): the same actions, laid out wide
                    let game = cases[i]
                    if let app = model.installed.first(where: { $0.game == game }) {
                        GameActions(app: app, section: $gamePage, live: live, wide: true) { launches += 1 }.id(app.app)
                    } else {
                        EmptyBack(ready: model.installedLoaded, live: live, wide: true, name: T.gameName(game)) { open(game) }
                    }
                }
                // what the middle cover is, as the iPod writes it under the cover
                VStack(spacing: 4) {
                    Text(T.gameName(selected)).font(.title2.weight(.semibold))
                    Text(T.gamePlatforms(selected)).font(.callout).foregroundStyle(.secondary)
                    if model.installedLoaded, !model.installed.contains(where: { $0.game == selected }) {
                        Button { open(selected) } label: { Label(T.addGame, systemImage: "plus") }
                            .buttonStyle(.borderedProminent).controlSize(.large).padding(.top, 6)
                            .help(T.addGameHelp(T.gameName(selected)))
                    }
                }
                .frame(maxWidth: .infinity)
                if let error = model.installedError {
                    Label(T.appsScanFailed(error), systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                        .textSelection(.enabled)
                }
                let shown = model.installed.first(where: { $0.game == selected })
                // a page the back opened (按键, 存储…): too big for a case, so under it; pressed again or ✕, it closes
                if let app = shown, let id = gamePage, model.pageApps.contains(where: { $0.app == app.app }) {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Text(T.gamePages.first { $0.0 == id }?.1 ?? id).font(.title3.weight(.semibold))
                                Spacer()
                                Button { gamePage = nil } label: { Image(systemName: "xmark.circle.fill").font(.title3) }
                                    .buttonStyle(.plain).foregroundStyle(.secondary)
                            }
                            GamePage(id: id, app: app.app)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                    .id(app.app + id)
                }
                // this game's update, started from its back: its progress and Cancel under the Cover Flow
                if let app = shown, model.updating == app.app {
                    GroupBox { ActionSection(updateOnly: true).frame(maxWidth: .infinity, alignment: .leading).padding(6) }
                }
                // an update running for another game, or one another program started: its progress shows here too, until
                // it ends well or is cancelled (the game opened, or the player chose so); a failure stays, with its log
                if let running = model.updating, running != shown?.app, model.phase != .done, model.phase != .cancelled {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) { Text(jobGame).font(.title3.weight(.semibold)); ActionSection(updateOnly: true) }
                            .frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                }
                // a build from the Build page: after 返回 its progress and its Cancel show here too (else greyed buttons, no word why)
                if model.updating == nil, model.jobRunning {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) { Text(jobGame).font(.title3.weight(.semibold)); ActionSection() }
                            .frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                }
            }
            .padding(24)
        }
        .onAppear(perform: chooseFirst)
        .onChange(of: selected) { gamePage = nil }   // another game in the middle: its own pages start closed
        .onChange(of: model.installed) { chooseFirst() }
        .onChange(of: model.installedLoaded) { chooseFirst() }
    }
}

struct GameCard: View {
    let game: String
    let app: InstalledReport.App?
    let loading: Bool
    let selected: Bool
    /// A case's front in the Cover Flow: the red band and the picture (the name is written under the middle case).
    var cover = false
    let tap: () -> Void

    @ObservedObject var crops = CoverCrops.shared   // not private: the memberwise init stays usable

    private var picturePath: String? {
        guard let app else { return nil }
        // the icon the app's Info.plist names (Odyssey: OdysseyIcon, Animal Crossing: AnimalCrossingIcon), else AppIcon
        // (2026-11-18: Odyssey's case showed no picture, the Launcher looked only for AppIcon.icns)
        let bundle = URL(fileURLWithPath: app.app)
        let info = NSDictionary(contentsOf: bundle.appendingPathComponent("Contents/Info.plist"))
        let name = (info?["CFBundleIconFile"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "AppIcon"
        let file = (name as NSString).pathExtension.isEmpty ? name + ".icns" : name
        return bundle.appendingPathComponent("Contents/Resources").appendingPathComponent(file).path
    }

    private var picture: NSImage? { picturePath.flatMap { NSImage(contentsOfFile: $0) } }

    var body: some View {
        if cover { coverBody } else { cardBody }
    }

    /// The picture on a case's front in the Cover Flow (CoverFlow handles the clicks: it knows which case is where).
    private var coverBody: some View {
        // as a real case (user 2026-11-17, a Switch 2 case): the red band across the top, the insert's picture under it
        GeometryReader { geo in
            let band = geo.size.height * CaseHeader.share
            VStack(spacing: 0) {
                CaseHeader(game: game).frame(height: band)
                coverArt.frame(width: geo.size.width, height: geo.size.height - band).clipped()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)   // the case's front (CoverFlow sizes it)
        .clipped()
    }

    /// The insert under the band: the game's square picture cut to a tall rectangle round its title (CoverCrops), its
    /// lower edge fading into the same picture lightly blurred, which carries on to the bottom with the game's name.
    private var coverArt: some View {
        ZStack {
            if let picture, let picturePath {
                GeometryReader { geo in
                    let tall = min(geo.size.height, geo.size.width / CoverCrops.keep)
                    ZStack(alignment: .top) {
                        Image(nsImage: picture).resizable().scaledToFill()
                            .frame(width: geo.size.width, height: geo.size.height).blur(radius: 10).clipped()
                            .overlay(Color.black.opacity(0.25))
                        CroppedPicture(picture: picture, center: crops.center(of: picturePath))
                            .frame(width: geo.size.width, height: tall)
                            .mask(LinearGradient(stops: [.init(color: .black, location: 0.88), .init(color: .clear, location: 1)],
                                                 startPoint: .top, endPoint: .bottom))
                        VStack {
                            Spacer(minLength: 0)
                            Text(T.gameName(game)).font(.system(size: geo.size.width * 0.075, weight: .bold))
                                .foregroundStyle(.white).multilineTextAlignment(.center).lineLimit(2).minimumScaleFactor(0.6)
                                .padding(.horizontal, geo.size.width * 0.06).shadow(color: .black.opacity(0.6), radius: 3)
                                .frame(height: max(0, geo.size.height - tall * 0.96))
                        }
                    }
                }
            } else {
                LinearGradient(colors: T.gameColors(game), startPoint: .topLeading, endPoint: .bottomTrailing)
                if loading {
                    ProgressView().controlSize(.small).tint(.white)
                } else {
                    VStack(spacing: 10) {
                        Image(systemName: "plus").font(.system(size: 36, weight: .semibold)).foregroundStyle(.white)
                            .frame(width: 76, height: 76).background(.white.opacity(0.22), in: Circle())
                        Text(T.gameName(game)).font(.callout.weight(.semibold)).foregroundStyle(.white)
                            .multilineTextAlignment(.center).padding(.horizontal, 12)
                    }
                }
            }
        }
    }

    private var cardBody: some View {
        Button(action: tap) {
            // The text sets the card's size; the picture sits behind it so a wide picture can't widen the card.
            VStack(alignment: .leading, spacing: 4) {
                Text(T.gamePlatforms(game)).font(.caption2.weight(.medium)).foregroundStyle(.white.opacity(0.85))
                Text(T.gameName(game)).font(.title3.weight(.semibold)).foregroundStyle(.white)
                Spacer()
                if loading {
                    Spacer()
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small).tint(.white)
                        Text(T.findingApps).font(.callout.weight(.medium)).foregroundStyle(.white)
                    }
                } else if app == nil {
                    HStack { Spacer()
                        Image(systemName: "plus").font(.system(size: 40, weight: .semibold)).foregroundStyle(.white)
                            .frame(width: 84, height: 84).background(.white.opacity(0.22), in: Circle())
                        Spacer() }
                    Spacer()
                    Text(T.addGame).font(.callout.weight(.medium)).foregroundStyle(.white)
                } else if app != nil {
                    HStack(alignment: .bottom) {
                        Spacer()
                        Image(systemName: "chevron.right").font(.caption.weight(.bold)).foregroundStyle(.black)
                            .frame(width: 26, height: 26).background(.white.opacity(0.9), in: Circle())
                    }
                }
            }
            .padding(16)
            .frame(width: 230, height: 300, alignment: .topLeading)
            .background {
                ZStack {
                    if let picture {
                        Image(nsImage: picture).resizable().scaledToFill()
                    } else {
                        LinearGradient(colors: T.gameColors(game), startPoint: .topLeading, endPoint: .bottomTrailing)
                    }
                    LinearGradient(colors: [.black.opacity(0.55), .clear], startPoint: .top, endPoint: .center)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                .stroke(selected ? Color.accentColor : .clear, lineWidth: 3))
            .contentShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            .shadow(color: .black.opacity(0.18), radius: 8, y: 4)
        }
        .buttonStyle(.plain)
        .help(loading ? T.findingApps : app == nil ? T.addGameHelp(T.gameName(game)) : app!.app)
    }
}

/// What can be done with a built game: the fastest update that fits (or a full build), its pages; the update's progress.
/// What a built game's case back shows, kept per app (scheme A, user 2026-11-17): the Cover Flow draws the back, its
/// reflection, and lays the live one on top, three views that must show the same start status, switches and check.
@MainActor final class GameCardState: ObservableObject {
    private static var cards: [String: GameCardState] = [:]
    static func of(_ app: String) -> GameCardState {
        if let card = cards[app] { return card }
        let card = GameCardState()
        cards[app] = card
        return card
    }
    @Published var gpu = "checking"   // MetalFX frame interpolation on this GPU (Tears of the Kingdom, Breath of the Wild)
    @Published var starting = false
    @Published var startStatus = ""
    @Published var shaderLine = ""          // the Check shaders result, in words (tools/builder_jobs/shaders.py)
    @Published var shaderUpdate = false     // the newest list adds something: the Update shaders button shows
    @Published var checkingShaders = false
    @Published var saver: GameActions.MemorySaver?         // 开启省内存模式 (tools/builder_jobs/memory_saver.py)
    @Published var diagnostics: GameActions.MemorySaver?   // 诊断模式 (the same module, its 'diagnostics' switch)
    @Published var saverNote = ""
    var asked = false   // the switches were asked for: the first back drawn (as the case starts turning) asks once
}

/// A built game's actions, on its case's back (scheme A): start, the switches, its update, the shader check, and the
/// buttons of its pages, which open under the Cover Flow (`section`).
struct GameActions: View {
    @EnvironmentObject var model: Model
    let app: InstalledReport.App
    @Binding var section: String?   // the game page open under the Cover Flow (T.gamePages), none at first
    /// The back laid on top, which takes clicks and Return; the Cover Flow's drawn copies only show the same state.
    let live: Bool
    let started: () -> Void   // the game is being started: its case grows over the stage (CoverFlow.launches)
    @ObservedObject private var state: GameCardState

    /// Laid out for the case opened over the stage (CaseDetail): the game's name and platforms on top, larger type and
    /// controls, the pages in one row; on the case's back (false) small, in one narrow column.
    let wide: Bool
    /// The big buttons' width: opened, 200 points (user 2026-11-17: "按钮 不要 无限长 … 200px就行"), at the left like the
    /// text ("极速更新 不要居中了"; the start alone sits bottom right); on the narrow back, its whole width.
    private var buttonWidth: CGFloat { wide ? 200 : .infinity }

    init(app: InstalledReport.App, section: Binding<String?>, live: Bool, wide: Bool = false, started: @escaping () -> Void = {}) {
        self.app = app
        self._section = section
        self.live = live
        self.wide = wide
        self.started = started
        self._state = ObservedObject(wrappedValue: GameCardState.of(app.app))
    }

    private var gpu: String { get { state.gpu } nonmutating set { state.gpu = newValue } }
    private var starting: Bool { get { state.starting } nonmutating set { state.starting = newValue } }
    private var startStatus: String { get { state.startStatus } nonmutating set { state.startStatus = newValue } }
    private var shaderLine: String { get { state.shaderLine } nonmutating set { state.shaderLine = newValue } }
    private var shaderUpdate: Bool { get { state.shaderUpdate } nonmutating set { state.shaderUpdate = newValue } }
    private var checkingShaders: Bool { get { state.checkingShaders } nonmutating set { state.checkingShaders = newValue } }
    private var saver: MemorySaver? { get { state.saver } nonmutating set { state.saver = newValue } }
    private var diagnostics: MemorySaver? { get { state.diagnostics } nonmutating set { state.diagnostics = newValue } }
    private var saverNote: String { get { state.saverNote } nonmutating set { state.saverNote = newValue } }

    /// A card switch's state (memory_saver.py state): the game reads its file at its next start.
    struct MemorySaver: Decodable { let available: Bool; let on: Bool; let file: String; let unknown_word: String? }

    /// The switches, as the game will read them at its next start (their files next to its controls.txt): read as soon
    /// as the case starts turning (the first back drawn), so they are there when the live back is laid on; the live back
    /// reads them again.
    private func loadSaver() async {
        guard live || !state.asked else { return }
        state.asked = true
        do {
            saver = try await TabClient.decode(MemorySaver.self, "storage", "memory_saver", app: app.app)
            diagnostics = try await TabClient.decode(MemorySaver.self, "storage", "diagnostics", app: app.app)
            saverNote = ""
        } catch { saverNote = error.localizedDescription }
    }
    private func setSaver(_ on: Bool) {
        Task {
            do {
                saver = try await TabClient.decode(MemorySaver.self, "storage", "set_memory_saver", app: app.app, args: ["on": on])
                saverNote = T.memorySaverNext
            } catch { saverNote = error.localizedDescription; await loadSaver() }
        }
    }
    private func setDiagnostics(_ on: Bool) {
        Task {
            do {
                diagnostics = try await TabClient.decode(MemorySaver.self, "storage", "set_diagnostics", app: app.app, args: ["on": on])
                saverNote = T.memorySaverNext
            } catch { saverNote = error.localizedDescription; await loadSaver() }
        }
    }

    /// Tears of the Kingdom and Breath of the Wild start through the Settings page's Play (tab_settings.play): the choices
    /// only Play passes (the 16x ground) go with it, and interpolation waits for the GPU check. The others open as apps.
    private var startsWithSettings: Bool { Model.pages(app.game).contains("settings") }

    /// What the game's newest list adds to this app (tools/builder_jobs shaders: nothing is changed).
    private func checkShaders() async {
        checkingShaders = true; shaderUpdate = false
        defer { checkingShaders = false }
        do {
            let data = try await Job.collect(["shaders", "--app", app.app])
            let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            if object?["ok"] as? Bool == true, let r = object?["result"] as? [String: Any] {
                shaderLine = r["line"] as? String ?? ""; shaderUpdate = r["can_update"] as? Bool ?? false
            } else {
                shaderLine = object?["error"] as? String ?? String(decoding: data, as: UTF8.self)
            }
        } catch {
            shaderLine = error.localizedDescription
        }
    }

    /// The big green start (user 2026-11-12); Return on the turned-over case. Opened, it sits in the bottom-right corner
    /// (user 2026-11-17: "启动游戏按钮 放在 黑色背景 右下角"); on the narrow back, first under the version.
    private var startButton: some View {
        Button(action: start) {
            Label(T.startGame, systemImage: "play.fill").font((wide ? Font.title3 : .callout).weight(.semibold))
                .frame(maxWidth: buttonWidth).padding(.vertical, wide ? 4 : 0)
        }
        .buttonStyle(.borderedProminent).tint(.green).controlSize(.large)
        .disabled(starting || (model.jobRunning && model.updating == app.app))
        .keyboardShortcut(live ? KeyboardShortcut.defaultAction : nil)
    }

    var body: some View {
        Group {
            if wide {   // the opened case: its details from the top, the start in the bottom-right corner
                VStack(alignment: .trailing, spacing: 8) {
                    ScrollView(.vertical, showsIndicators: false) { details }
                    if !startStatus.isEmpty { Text(startStatus).font(.caption).foregroundStyle(.secondary) }
                    startButton
                }
            } else {    // a case's back is small (232 x 375 before its 8%): one column, scrolling if a reason is long
                ScrollView(.vertical, showsIndicators: false) { details }
            }
        }
        .task {
            guard live, startsWithSettings else { return }
            struct Probe: Decodable { let gpu: String }
            gpu = (try? await TabClient.decode(Probe.self, "settings", "probe", app: app.app).gpu) ?? "unknown"
        }
        .task(id: app.app) { await loadSaver() }   // the backend says which games have the switch (memory_saver.GAMES)
    }

    private var details: some View {
            VStack(alignment: .leading, spacing: wide ? 12 : 7) {
                if wide {   // the opened case: the game's name and platforms over its actions
                    VStack(alignment: .leading, spacing: 6) {
                        Text(T.gameName(app.game ?? "")).font(.title.weight(.bold)).padding(.trailing, 40)
                        HStack(spacing: 6) {
                            ForEach(T.gamePlatforms(app.game ?? "").components(separatedBy: " · "), id: \.self) { p in
                                Text(p).font(.caption).foregroundStyle(.secondary)
                                    .padding(.horizontal, 8).padding(.vertical, 2).background(.white.opacity(0.1), in: Capsule())
                            }
                        }
                    }
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("\(app.name) \(T.version(app.version))").font((wide ? Font.callout : .caption).weight(.semibold)).lineLimit(2)
                        .help(app.app)   // where the app is
                    Text(T.kind(app.kind)).font((wide ? Font.callout : .caption2).weight(.semibold))
                        .foregroundStyle(app.kind == "express" || app.kind == "fast" ? .green : .secondary)
                    if app.kind != "express" { Text(app.reason).font(wide ? .caption : .caption2).foregroundStyle(.secondary).lineLimit(4) }
                }
                if !wide {   // the start, first on the narrow back (opened: the bottom-right corner, body)
                    startButton
                    if !startStatus.isEmpty { Text(startStatus).font(.caption2).foregroundStyle(.secondary) }
                }
                // under the start, any time, game running or not: it counts from the game's next start (user 2026-11-16)
                if let saver, saver.available {
                    Toggle(T.memorySaver, isOn: Binding(get: { saver.on }, set: { setSaver($0) }))
                        .help(T.memorySaverHelp)
                    if let w = saver.unknown_word { Text(T.memorySaverUnknown(w)).font(.caption2).foregroundStyle(.orange) }
                }
                // user 2026-11-16: "诊断模式 也改成 CheckBox"; not in the players' download (user 2026-11-17: "正式下载版
                // 也去掉 诊断模式"), except while it is still on from an earlier Launcher, so it can be turned off
                if let diagnostics, diagnostics.available, !Paths.isRelease || diagnostics.on {
                    Toggle(T.diagnostics, isOn: Binding(get: { diagnostics.on }, set: { setDiagnostics($0) }))
                        .help(T.diagnosticsHelp)
                }
                if !saverNote.isEmpty { Text(saverNote).font(.caption2).foregroundStyle(.secondary) }
                // One button named after the update this app can have (user 2026-11-12): express, fast, or a full build
                // from its game files (the build page, this game chosen).
                let busy = model.phase == .building || model.phase == .cancelling
                switch app.kind {
                case "express", "fast":
                    Button { model.update(app) } label: { Text(T.updateButton(app.kind)).frame(maxWidth: buttonWidth) }
                        .buttonStyle(.borderedProminent).disabled(busy)
                case "full":
                    Button { if let g = app.game { model.buildGame(g) } } label: { Text(T.updateButton(app.kind)).frame(maxWidth: buttonWidth) }
                        .buttonStyle(.borderedProminent).disabled(busy)
                default:
                    EmptyView()   // the reason above says why it cannot be updated now
                }
                if app.shaders == "title" {   // a quick app: the rest of its shaders, made while it is not played (user 2026-11-13)
                    Button { model.updateShaders(app) } label: { Text(T.fillShaders).frame(maxWidth: buttonWidth) }
                        .buttonStyle(.borderedProminent).disabled(busy)
                    Text(T.quickApp).font(.caption2).foregroundStyle(.secondary)
                }
                if Model.shaderGames.contains(app.game ?? "") {   // only the shaders and pipelines, from the newest list (user 2026-11-13)
                    HStack(spacing: 6) {
                        Button(T.checkShaders) { Task { await checkShaders() } }.disabled(checkingShaders || busy)
                        if shaderUpdate {
                            Button(T.updateShaders) { shaderUpdate = false; shaderLine = ""; model.updateShaders(app) }
                                .buttonStyle(.borderedProminent).disabled(busy)
                        }
                        if checkingShaders { ProgressView().controlSize(.mini) }
                    }
                    let said = checkingShaders ? T.checkingShaders : shaderLine
                    if !said.isEmpty { Text(said).font(.caption2).foregroundStyle(.secondary) }
                }
                if model.pageApps.contains(where: { $0.app == app.app }) {   // the pages serve these games only
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 5), count: wide ? 6 : 3), spacing: wide ? 8 : 5) {
                        ForEach(T.gamePages.filter { Model.pages(app.game).contains($0.0) }, id: \.0) { id, label in
                            // under the Cover Flow, not a page of its own (user 2026-11-12); pressed again, it closes
                            Button { section = section == id ? nil : id } label: { Text(label).frame(maxWidth: .infinity) }
                                .buttonStyle(.bordered).tint(section == id ? .accentColor : nil)
                        }
                        if Model.quickGames.contains(app.game ?? "") {   // 快速启动, after the game's pages (user 2026-11-15)
                            Button(T.quickStart) { model.quickStart(app) }.buttonStyle(.bordered).help(T.quickStartHelp)
                                .disabled(model.phase == .building || model.phase == .cancelling)
                        }
                    }
                }
            }
            .controlSize(wide ? .regular : .small)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func start() {
        starting = true; startStatus = T.starting
        started()
        if !startsWithSettings {
            NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: app.app), configuration: .init()) { _, error in
                Task { @MainActor in startStatus = error?.localizedDescription ?? T.startedApp; starting = false }
            }
            return
        }
        struct Played: Decodable { let ok: Bool; let reason: String; let detail: String; let held: [String] }
        Task {
            do {
                let r = try await TabClient.decode(Played.self, "settings", "play", app: app.app, args: ["gpu": gpu])
                startStatus = r.ok ? (r.held.isEmpty ? T.started : T.startedWithout(r.held))
                                   : [r.reason, r.detail].filter { !$0.isEmpty }.joined(separator: " ")
            } catch { startStatus = error.localizedDescription }
            starting = false
        }
    }
}

/// The back of a game not built yet (scheme A): the Build page's three steps, and the button that opens it.
private struct EmptyBack: View {
    let ready: Bool          // the installed apps were scanned: the game is known to be missing
    let live: Bool           // the back on top: Return adds the game
    var wide = false         // the case opened over the stage: its name on top, larger type (GameActions.wide)
    var name = ""
    let add: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: wide ? 14 : 8) {
            if wide { Text(name).font(.title.weight(.bold)).padding(.trailing, 40) }
            Text(T.notBuiltYet).font((wide ? Font.body : .caption).weight(.semibold))
            ForEach(Array([T.page1, T.page2, T.page3].enumerated()), id: \.offset) { n, step in
                Label { Text(step).font(wide ? .body : .caption) } icon: { Image(systemName: "\(n + 1).circle.fill").foregroundStyle(.secondary) }
            }
            Button(action: add) { Label(T.addGame, systemImage: "plus").font((wide ? Font.title3 : .callout).weight(.semibold)).frame(maxWidth: wide ? 200 : .infinity) }
                .buttonStyle(.borderedProminent).controlSize(.large).padding(.top, 4)
                .disabled(!ready)
                .keyboardShortcut(live ? KeyboardShortcut.defaultAction : nil)
        }
    }
}

/// One of a built game's pages, shown under its buttons; each asks tools/builder_jobs about this app.
struct GamePage: View {
    let id: String
    let app: String

    var body: some View {
        switch id {
        case "settings": SettingsPage(app: app)
        case "mods": ModsPage(app: app)
        case "controls": ControlsPage(app: app)
        case "storage": StoragePage(app: app)
        default: SavesPage(app: app)
        }
    }
}
