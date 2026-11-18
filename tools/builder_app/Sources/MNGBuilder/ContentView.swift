import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// The games page, the Build page (three pages: 1 this Mac, 2 the files and keys, 3 the build) or the terminal.
struct ContentView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        // No bar on top (user 2026-11-12): the games are the first page; building opens from a card's plus and has
        // its own way back; a game's settings, mods, controls, storage and saves open under its card.
        if model.page == "build" { BuildPage() } else if model.page == "terminal" { TerminalPage() } else { HomePage() }
    }
}

/// Building a game from its files, one page a step (user 2026-11-17: "第一步 检查 所有工具 是否齐全 引导安装，第二步 选择
/// 文件 包括key，第三部 打包。 每个步骤 一个页面 （不要现在这样 都放在一起）"), as NX2Apple's and S2Arm's setup do: a bar of
/// one bar on top (user 2026-11-17: "放最上面 不要返回按钮， 上一步 就是继续返回了"): Back, the three steps (a done one has
/// a check mark and opens with a click), Next; then the page. Back on page 1 (or while a build keeps page 3) is the way
/// back to the games: there is no separate one. Next opens only when the page is done (Model.canOpen).
struct BuildPage: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 16) {
                Button(action: back) { Label(T.back, systemImage: "chevron.left").frame(minWidth: 90) }
                    .controlSize(.large)
                    .keyboardShortcut("[", modifiers: .command)   // as Back in Finder and Safari
                    .accessibilityIdentifier("build-back")
                StepBar()
                Button { model.buildStep += 1 } label: { Text(T.next).frame(minWidth: 90) }
                    .buttonStyle(.borderedProminent).controlSize(.large).keyboardShortcut(.defaultAction)
                    .disabled(model.buildStep >= 3 || !model.canOpen(model.buildStep + 1))
                    .opacity(model.buildStep < 3 ? 1 : 0)   // kept in place on page 3, so the steps stay centred
                    .accessibilityIdentifier("build-next")
            }
            .padding(.horizontal, 20).padding(.vertical, 14)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    switch model.buildStep {
                    case 1: MacStep()
                    case 2: FilesStep()
                    default: BuildStep()
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(20)
            }
        }
    }

    /// The page before, or the games from page 1 (and from page 3 while a build runs, which keeps it there).
    private func back() {
        if model.buildStep > 1 && model.canOpen(model.buildStep - 1) { model.buildStep -= 1 } else { model.page = "home" }
    }
}

/// The three steps in a row: the current one filled, a done one green with a check mark; a click opens one that can open.
struct StepBar: View {
    @EnvironmentObject var model: Model

    private func done(_ n: Int) -> Bool {
        switch n {
        case 1: return model.macReady
        case 2: return model.gameReady && model.keysReady
        default: return model.phase == .done
        }
    }

    var body: some View {
        HStack(spacing: 10) {
            ForEach(1...3, id: \.self) { n in
                if n > 1 { Rectangle().fill(Color.secondary.opacity(0.35)).frame(height: 1).frame(maxWidth: 60) }
                Button { model.buildStep = n } label: { item(n) }
                    .buttonStyle(.plain).disabled(!model.canOpen(n)).accessibilityIdentifier("build-step-\(n)")
            }
        }
        .frame(maxWidth: .infinity)
    }

    private func item(_ n: Int) -> some View {
        let current = model.buildStep == n
        let ok = done(n)
        return HStack(spacing: 8) {
            ZStack {
                Circle().fill(ok ? Color.green : (current ? Color.accentColor : Color.secondary.opacity(0.45)))
                    .frame(width: 24, height: 24)
                if ok { Image(systemName: "checkmark").font(.caption.bold()).foregroundStyle(.white) }
                else { Text("\(n)").font(.caption.bold()).foregroundStyle(.white) }
            }
            VStack(alignment: .leading, spacing: 0) {
                Text(T.stepTitle(n)).font(.caption).foregroundStyle(.secondary)
                Text([T.page1, T.page2, T.page3][n - 1]).font(current ? .headline : .body)
                    .foregroundStyle(current ? Color.primary : Color.secondary)
            }
        }
        .opacity(model.canOpen(n) ? 1 : 0.5)
        .contentShape(Rectangle())
    }
}

/// Page 1: what this Mac needs, each line with its state; a missing one with its fix (an installer, or a command to copy).
/// The work folder's free space is page 3's: it depends on the place chosen there.
struct MacStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(T.page1).font(.title2.weight(.semibold))
            Text(T.page1Hint).font(.callout).foregroundStyle(.secondary)
            let shown = model.checks.filter { $0.id != "disk" }
            if shown.isEmpty {
                HStack { ProgressView().controlSize(.small); Text(T.checking).foregroundStyle(.secondary) }
            }
            ForEach(shown) { c in
                HStack(alignment: .firstTextBaseline) {
                    Image(systemName: c.ok ? "checkmark.circle.fill" : "xmark.octagon.fill").foregroundStyle(c.ok ? .green : .red)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(c.text)
                        if !c.ok, let fix = c.fix {
                            Text(fix).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                        }
                    }
                    Spacer()
                    if !c.ok, c.fix != nil { Button(c.action == nil ? T.copyCommand : T.fix) { model.fix(c) } }
                }
            }
            if model.macReady { Label(T.macOK, systemImage: "desktopcomputer").foregroundStyle(.green) }
            Button(T.recheck) { Task { await model.runChecks() } }
        }
    }
}

/// Page 2: the game's files, then the keys.
struct FilesStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 8) {
                Text(T.gameFiles).font(.title2.weight(.semibold))
                GameStep()
            }
            Divider()
            VStack(alignment: .leading, spacing: 8) {
                Text(T.keys).font(.title2.weight(.semibold))
                KeysStep()   // open before the game is told: a card without a title ID in its name is told by the keys
            }
        }
    }
}

/// A dashed area that takes dropped files, with a button that opens a panel for the same.
struct DropZone: View {
    let hint: String
    let button: String
    let directories: Bool
    let busy: Bool
    let take: ([URL]) -> Void
    @State private var over = false

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "square.and.arrow.down.on.square").font(.system(size: 26)).foregroundStyle(.secondary)
            Text(hint).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Button(button, action: choose).disabled(busy)
        }
        .frame(maxWidth: .infinity)
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 10).fill(over ? Color.accentColor.opacity(0.12) : Color.secondary.opacity(0.06)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(over ? Color.accentColor : Color.secondary.opacity(0.4),
                                                                  style: StrokeStyle(lineWidth: 1.5, dash: [6, 4])))
        .onDrop(of: [.fileURL], isTargeted: $over) { providers in
            guard !busy else { return false }
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    if let url { DispatchQueue.main.async { take([url]) } }
                }
            }
            return true
        }
    }

    private func choose() {
        if let urls = Self.pick(directories: directories) { take(urls) }
    }

    /// The open panel of the drop area and of the Choose Again button: nil when cancelled.
    static func pick(directories: Bool) -> [URL]? {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = directories
        panel.canChooseDirectories = directories
        panel.canChooseFiles = true
        return panel.runModal() == .OK ? panel.urls : nil
    }
}

struct GameStep: View {
    @EnvironmentObject var model: Model
    private var busy: Bool { model.phase == .building || model.phase == .cancelling }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let game = model.buildFor {   // chosen with a card's plus on the first page
                HStack(alignment: .firstTextBaseline) {
                    Label(T.buildingFor(T.gameName(game)), systemImage: "plus.circle.fill").font(.headline)
                    Spacer()
                    Button(T.anyGame) { model.buildFor = nil }
                }
                Text(T.filesFor(game)).font(.callout).foregroundStyle(.secondary)
                if let other = model.plan?.game, other != game {
                    Label(T.otherGameDropped(T.gameName(other)), systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                }
            }
            let items = (model.report?.items ?? []).filter { $0.role != "keys" }
            if items.isEmpty {
                DropZone(hint: T.step1Hint, button: T.chooseGame, directories: true, busy: busy) { model.add($0) }
            } else {
                // Chosen (user 2026-11-17: "选择rom后 显示rom的名称 下面小按钮显示 重新选择， 去掉 这个巨大的 框"): the
                // game's name and its files, and a small button that chooses them again; more files still drop here.
                VStack(alignment: .leading, spacing: 6) {
                    Text(model.plan?.app_name ?? T.gameFiles).font(.title3.weight(.semibold))
                    ForEach(items) { FileRow(item: $0) }
                    Button(T.chooseAgain) { if let urls = DropZone.pick(directories: true) { model.replace(urls) } }
                        .controlSize(.small).disabled(busy)
                }
                .onDrop(of: [.fileURL], isTargeted: nil) { providers in
                    guard !busy else { return false }
                    for p in providers {
                        _ = p.loadObject(ofClass: URL.self) { url, _ in
                            if let url { DispatchQueue.main.async { model.add([url]) } }
                        }
                    }
                    return true
                }
            }
            if let error = model.classifyError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
            ForEach(model.plan?.notes ?? [], id: \.self) { Text($0).font(.caption).foregroundStyle(.secondary) }
            ForEach((model.plan?.blocks ?? []).filter { !(model.plan?.keys_problems ?? []).contains($0) && !model.toolBlocks.contains($0) }, id: \.self) {
                Label($0, systemImage: "xmark.octagon").foregroundStyle(.red)
            }
        }
    }
}

struct KeysStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let plan = model.plan, !plan.needs_keys {
                Label(T.noKeysNeeded, systemImage: "checkmark.circle").foregroundStyle(.secondary)
            } else {
                Text(T.step2Hint).font(.callout).foregroundStyle(.secondary)
                HStack {
                    Image(systemName: "key.fill").foregroundStyle(model.keysReady ? Color.green : Color.secondary)
                    // the plan's keys, or before any game file is chosen (no plan yet) the keys kept for every game
                    let keys = model.plan?.keys ?? model.keysPath
                    Text(keys.map { ($0 as NSString).lastPathComponent } ?? "—")
                    if let keys {
                        Text((keys as NSString).deletingLastPathComponent).font(.caption).foregroundStyle(.secondary)
                            .lineLimit(1).truncationMode(.middle)
                    }
                    Spacer()
                    Button(T.chooseKeys) {
                        let panel = NSOpenPanel()
                        panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
                        if panel.runModal() == .OK { model.setKeys(panel.url) }
                    }
                }
                .onDrop(of: [.fileURL], isTargeted: nil) { providers in
                    _ = providers.first?.loadObject(ofClass: URL.self) { url, _ in
                        if let url { DispatchQueue.main.async { model.setKeys(url) } }
                    }
                    return true
                }
                if model.keysReady { Label(T.keysOK, systemImage: "checkmark.circle.fill").foregroundStyle(.green) }
                ForEach(model.plan?.keys_problems ?? [], id: \.self) { Label($0, systemImage: "xmark.octagon").foregroundStyle(.red) }
            }
        }
    }
}

struct FileRow: View {
    @EnvironmentObject var model: Model
    let item: ClassifyReport.Item

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Image(systemName: item.role == "ignored" ? "minus.circle" : (used ? "checkmark.circle.fill" : "circle"))
                .foregroundStyle(item.role == "ignored" ? Color.secondary : Color.accentColor)
            Text(T.role(item.role)).frame(width: 96, alignment: .leading).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text((item.path as NSString).lastPathComponent).lineLimit(1).truncationMode(.middle).strikethrough(!used)
                if !item.note.isEmpty { Text(item.note).font(.caption).foregroundStyle(.secondary) }
            }
        }
    }

    private var used: Bool {
        guard let p = model.plan else { return false }
        return [p.base, p.update, p.keys, p.firmware, p.fonts].contains(item.path) || p.dlc.contains(item.path)
    }
}


/// Page 3: what will be built, where, what this game still needs on this Mac (Ocarina of Time: Xcode's Metal compiler,
/// with a way to get it), the work folder's free space, and the build.
struct BuildStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(model.plan?.app_name ?? T.page3).font(.title2.weight(.semibold))
                Spacer()
            }
            Text(T.step3Hint).font(.callout).foregroundStyle(.secondary)
            // what this game needs on this Mac (classify tool_needs): the command line tools have an installer to start,
            // Xcode's Metal toolchain the App Store and a command to copy
            ForEach(Array(model.toolBlocks.enumerated()), id: \.offset) { i, block in
                HStack(alignment: .firstTextBaseline) {
                    Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
                    Text(block).textSelection(.enabled)
                    Spacer()
                    if model.toolNeed(i) == "clt" {
                        Button(T.installCLT) { model.installCommandLineTools() }.buttonStyle(.borderedProminent)
                    } else {
                        Button(T.getXcode) { NSWorkspace.shared.open(URL(string: "macappstore://apps.apple.com/app/id497799835")!) }
                        Button(T.copyCommand) {
                            NSPasteboard.general.clearContents(); NSPasteboard.general.setString(T.metalCommand, forType: .string)
                        }
                    }
                    Button(T.recheck) { Task { await model.classify() } }
                }
            }
            LocationSection()
            if let disk = model.checks.first(where: { $0.id == "disk" }), !disk.ok,
               !model.location.isEmpty || model.installedAppForPlan != nil {
                Label(disk.text, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
            }
            ActionSection()
        }
    }
}

/// Step 3's location (user 2026-11-17: "让用户 自己选择位置 不要有默认位置，用户选一个位置 自动生成app目录和build目录
/// 然后打包"): no default, a new app is not built before one is chosen; the app and build folders under it are shown and
/// made when the build starts (Model.start). A game already installed is updated where it is, so it needs none.
struct LocationSection: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let app = model.installedAppForPlan {
                Label(T.updatesInPlace(app), systemImage: "arrow.triangle.2.circlepath").font(.callout).foregroundStyle(.secondary)
                    .textSelection(.enabled)
            } else {
                Text(T.locationHint).font(.callout).foregroundStyle(.secondary)
                HStack {
                    Text(T.location).frame(width: 60, alignment: .leading).foregroundStyle(.secondary)
                    Text(model.location.isEmpty ? T.locationNone : model.location).lineLimit(1).truncationMode(.middle)
                        .foregroundStyle(model.location.isEmpty ? Color.orange : Color.primary).textSelection(.enabled)
                    Spacer()
                    Button(model.location.isEmpty ? T.chooseLocation : T.change, action: choose)
                        .buttonStyle(.borderedProminent).tint(model.location.isEmpty ? .accentColor : .secondary)
                }
                if !model.location.isEmpty {
                    Text(T.locationParts(model.outputDir, (model.workRoot as NSString).appendingPathComponent(model.plan?.game ?? "")))
                        .font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
        }
        .disabled(model.phase == .building || model.phase == .cancelling)
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.prompt = T.chooseLocation
        if panel.runModal() == .OK, let url = panel.url { model.location = url.path }
    }
}
