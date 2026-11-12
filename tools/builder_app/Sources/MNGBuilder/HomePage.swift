import AppKit
import SwiftUI

/// The first page: every game MNG Launcher builds, as cards. A game already built shows its own app's picture (the icon
/// its build made from the player's own files; the Launcher ships no game art) and opens its actions; a game not built
/// yet shows a plus that goes to the Build page with that game chosen (user 2026-11-12).
struct HomePage: View {
    @EnvironmentObject var model: Model
    @State private var selected: String?
    @State private var defaulted = false   // the first built game was chosen once; a card closed by hand stays closed

    static let games = ["oot", "totk", "botw", "dkb", "pkp"]

    /// The first built game, in the cards' order, is open when the page shows (user 2026-11-12: "启动 app 默认选中第一个游戏").
    private func chooseFirst() {
        guard !defaulted, model.installedLoaded else { return }
        guard let first = Self.games.first(where: { g in model.installed.contains { $0.game == g } }) else { return }
        selected = first
        defaulted = true
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 14) {
                    Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 56, height: 56)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("MNG Launcher").font(.largeTitle.weight(.bold))
                        Text(T.homeSubtitle).foregroundStyle(.secondary)
                    }
                }
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 14) {
                        ForEach(Self.games, id: \.self) { game in
                            let app = model.installed.first { $0.game == game }
                            GameCard(game: game, app: app, loading: !model.installedLoaded, selected: selected == game) {
                                if !model.installedLoaded { return }
                                if app != nil { selected = selected == game ? nil : game }
                                else { model.buildFor = game; model.page = "build" }
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }
                if let error = model.installedError {
                    Label(T.appsScanFailed(error), systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                        .textSelection(.enabled)
                }
                let shown = selected.flatMap { g in model.installed.first(where: { $0.game == g }) }
                if let app = shown { GameActions(app: app).id(app.app) }
                // an update running for another game, or one another program started: its progress shows here too
                if let running = model.updating, running != shown?.app {
                    GroupBox { ActionSection(updateOnly: true).frame(maxWidth: .infinity, alignment: .leading).padding(6) }
                }
            }
            .padding(24)
        }
        .onAppear(perform: chooseFirst)
        .onChange(of: model.installed) { chooseFirst() }
        .onChange(of: model.installedLoaded) { chooseFirst() }
    }
}

struct GameCard: View {
    let game: String
    let app: InstalledReport.App?
    let loading: Bool
    let selected: Bool
    let tap: () -> Void

    private var picture: NSImage? {
        guard let app else { return nil }
        return NSImage(contentsOf: URL(fileURLWithPath: app.app).appendingPathComponent("Contents/Resources/AppIcon.icns"))
    }

    var body: some View {
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
struct GameActions: View {
    @EnvironmentObject var model: Model
    let app: InstalledReport.App
    @State private var section: String?   // the game page open under the buttons (T.gamePages), none at first
    @State private var gpu = "checking"   // MetalFX frame interpolation on this GPU (Tears of the Kingdom, Breath of the Wild)
    @State private var starting = false
    @State private var startStatus = ""

    /// Tears of the Kingdom and Breath of the Wild start through the Settings page's Play (tab_settings.play): the choices
    /// only Play passes (the 16x ground) go with it, and interpolation waits for the GPU check. The others open as apps.
    private var startsWithSettings: Bool { model.pageApps.contains(where: { $0.app == app.app }) }

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("\(app.name) \(T.version(app.version))").font(.title2.weight(.semibold))
                        Text(T.kind(app.kind)).foregroundStyle(app.kind == "express" || app.kind == "fast" ? .green : .secondary)
                        if app.kind != "express" { Text(app.reason).font(.caption).foregroundStyle(.secondary).textSelection(.enabled) }
                        Text(app.app).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                    Spacer()
                    // the big green start, top right (user 2026-11-12)
                    VStack(alignment: .trailing, spacing: 4) {
                        Button(action: start) {
                            Label(T.startGame, systemImage: "play.fill")
                                .font(.title3.weight(.semibold))
                                .padding(.horizontal, 18).padding(.vertical, 8)
                        }
                        .buttonStyle(.borderedProminent).tint(.green).controlSize(.large)
                        .disabled(starting || model.updating == app.app)
                        if !startStatus.isEmpty {
                            Text(startStatus).font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.trailing)
                                .frame(maxWidth: 260, alignment: .trailing)
                        }
                    }
                }
                // One button named after the update this app can have (user 2026-11-12): express, fast, or a full build
                // from its game files (the build page, this game chosen).
                let busy = model.phase == .building || model.phase == .cancelling
                switch app.kind {
                case "express", "fast":
                    Button(T.updateButton(app.kind)) { model.update(app) }.buttonStyle(.borderedProminent).disabled(busy)
                case "full":
                    Button(T.updateButton(app.kind)) { model.buildFor = app.game; model.page = "build" }
                        .buttonStyle(.borderedProminent).disabled(busy)
                default:
                    EmptyView()   // the reason above says why it cannot be updated now
                }
                if model.pageApps.contains(where: { $0.app == app.app }) {   // the pages serve these games only
                    HStack {
                        ForEach(T.gamePages, id: \.0) { id, label in
                            // under the buttons, not a page of its own (user 2026-11-12); pressed again, it closes
                            Button(label) { section = section == id ? nil : id }
                                .buttonStyle(.bordered).tint(section == id ? .accentColor : nil)
                        }
                    }
                    if let section {
                        Divider().padding(.vertical, 4)
                        GamePage(id: section, app: app.app)
                    }
                }
                if model.updating == app.app { ActionSection(updateOnly: true) }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
        .task {
            guard startsWithSettings else { return }
            struct Probe: Decodable { let gpu: String }
            gpu = (try? await TabClient.decode(Probe.self, "settings", "probe", app: app.app).gpu) ?? "unknown"
        }
    }

    private func start() {
        starting = true; startStatus = T.starting
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
