import AppKit
import SwiftUI

/// The work folder the app records and the game's shader cache (tab_storage.py over tools/builder/storage.py): where
/// they are, their sizes, Clean (after a confirmation) and Move (the work folder). The old window's Storage tab.
struct StorageTab: View {
    var body: some View { PageFrame { app in StoragePage(app: app) } }
}

struct StorageState: Decodable {
    struct Size: Decodable { let bytes: Int; let files: Int; let shown: String; let errors: [String] }
    struct Folder: Decodable, Identifiable {
        let id, label, path, kind, `where`, note: String
        let exists, movable: Bool
        let size: Size?
        let detail, confirm: String
    }
    let title: String
    let note: String
    let folders: [Folder]
}

/// The words only this page's Swift side says (the rest comes from tab_storage.py, in the same language).
enum StorageText {
    static let recordHint = T.s("The app still records its old work folder: one express update (Build page) makes it use the new one.",
                                "App 记录的还是原来的工作目录：做一次极速更新（进度显示在打包页），它就改用新位置。")
    static let recordNow = T.s("Update now", "现在更新")
    static let clean = T.s("Clean", "清理")
    static let cleanTitle = T.s("Clean this folder?", "清理这个文件夹？")
    static let move = T.s("Move…", "移动…")
    static let moveTitle = T.s("Move the work folder?", "移动工作文件夹？")
    static let moveHere = T.s("Move here", "移到这里")
    static let movePrompt = T.s("Choose the folder to move the cache into.", "选择要把缓存移进去的文件夹。")
    static let measuring = T.s("Measuring…", "正在计算大小…")
    static let measureAgain = T.s("Measure again", "重新计算")
    static let reveal = T.s("Show in Finder", "在访达中显示")
    static let cancel = T.s("Cancel", "取消")
    static let working = T.s("Working…", "处理中…")
    static let busy = T.s("A build or an update is running: cleaning and moving wait until it ends.",
                          "正在打包或更新：结束后才能清理或移动。")
    static func cleanFallback(_ path: String) -> String {
        T.s("Delete everything inside \(path)? Saves and the app are not touched.", "删除 \(path) 里的全部内容？存档和 App 不受影响。")
    }
}

struct StoragePage: View {
    struct MoveQuestion { let target: String; let question: String }

    let app: String
    @EnvironmentObject var model: Model
    @State private var state: StorageState?
    @State private var sizes: [String: StorageState.Folder] = [:]   // a folder's measured view, by id
    @State private var measuring: Set<String> = []
    @State private var error: String?
    @State private var status = ""
    @State private var failed = false
    @State private var working = false
    @State private var cleaning: StorageState.Folder?
    @State private var moving: MoveQuestion?
    @State private var movedTo: String?   // a moved work folder the app does not record yet: one update records it

    private var building: Bool { model.phase == .building || model.phase == .cancelling || model.updating != nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let s = state {
                Text(s.title).font(.headline)
                Text(s.note).font(.callout).foregroundStyle(.secondary)
                ForEach(s.folders) { f in folderBox(sizes[f.id] ?? f) }
                if let target = movedTo {
                    HStack {
                        Text(StorageText.recordHint).font(.callout).foregroundStyle(.secondary)
                        Button(StorageText.recordNow) { model.update(path: app, work: target); movedTo = nil }
                            .buttonStyle(.borderedProminent).disabled(building)
                    }
                }
                if building { Label(StorageText.busy, systemImage: "hourglass").font(.callout).foregroundStyle(.secondary) }
                HStack {
                    Button(StorageText.measureAgain) { Task { await load() } }.disabled(working || !measuring.isEmpty)
                    if working { ProgressView().controlSize(.small); Text(StorageText.working).foregroundStyle(.secondary) }
                }
                if !status.isEmpty {
                    Text(status).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled)
                }
            } else if error == nil {
                ProgressView()
            }
        }
        .task { await load() }
        .confirmationDialog(StorageText.cleanTitle, isPresented: Binding(get: { cleaning != nil }, set: { if !$0 { cleaning = nil } }),
                            presenting: cleaning) { f in
            Button(StorageText.clean, role: .destructive) { clean(f) }
            Button(StorageText.cancel, role: .cancel) {}
        } message: { f in Text(f.confirm.isEmpty ? StorageText.cleanFallback(f.path) : f.confirm) }
        .confirmationDialog(StorageText.moveTitle, isPresented: Binding(get: { moving != nil }, set: { if !$0 { moving = nil } }),
                            presenting: moving) { m in
            Button(StorageText.moveHere) { move(m.target, confirmed: true) }
            Button(StorageText.cancel, role: .cancel) {}
        } message: { m in Text(m.question) }
    }

    @ViewBuilder private func folderBox(_ f: StorageState.Folder) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Text(f.label).font(.body.weight(.medium))
                Text(f.path).font(.callout.monospaced()).textSelection(.enabled)
                Text(f.where).font(.caption).foregroundStyle(.secondary)
                if measuring.contains(f.id) {
                    HStack { ProgressView().controlSize(.small); Text(StorageText.measuring).foregroundStyle(.secondary) }
                } else if !f.detail.isEmpty {
                    Text(f.detail).font(.callout)
                }
                Text(f.note).font(.caption2).foregroundStyle(.tertiary)
                HStack {
                    Button(StorageText.clean) { cleaning = f }
                        .disabled(working || building || measuring.contains(f.id) || !f.exists || f.kind == "unreachable")
                    if f.movable {
                        Button(StorageText.move) { chooseTarget(f) }.disabled(working || building || measuring.contains(f.id))
                    }
                    if f.exists {
                        Button(StorageText.reveal) {
                            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: f.path)])
                        }
                    }
                }
                .padding(.top, 4)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    /// The folders at once, then each one's size (a big work folder takes a while; the shader cache shows first).
    private func load() async {
        do { state = try await TabClient.decode(StorageState.self, "storage", "state", app: app); error = nil }
        catch { self.error = error.localizedDescription; return }
        sizes = [:]
        await withTaskGroup(of: Void.self) { group in
            for f in state?.folders ?? [] { group.addTask { await measure(f.id) } }
        }
    }

    private func measure(_ id: String) async {
        measuring.insert(id)
        do { sizes[id] = try await TabClient.decode(StorageState.Folder.self, "storage", "measure", app: app, args: ["folder": id]) }
        catch { report(error.localizedDescription, failed: true) }
        measuring.remove(id)
    }

    private func report(_ text: String, failed: Bool) { status = text; self.failed = failed }

    private func clean(_ f: StorageState.Folder) {
        struct Cleaned: Decodable { let message: String }
        working = true
        Task {
            do {
                let r = try await TabClient.decode(Cleaned.self, "storage", "clean", app: app, args: ["folder": f.id])
                report(r.message, failed: false)
            } catch { report(error.localizedDescription, failed: true) }
            working = false
            await measure(f.id)
        }
    }

    private func chooseTarget(_ f: StorageState.Folder) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = StorageText.movePrompt
        panel.directoryURL = URL(fileURLWithPath: f.path).deletingLastPathComponent()
        guard panel.runModal() == .OK, let url = panel.url else { return }
        move(url.path, confirmed: false)
    }

    /// Unconfirmed: tab_storage.py checks the target and answers the question to ask (or why not, or that there is
    /// nothing to move); confirmed: it moves.
    private func move(_ target: String, confirmed: Bool) {
        struct Moved: Decodable { let moved: Int; let confirm: String; let message: String; let command: String }
        working = true
        Task {
            do {
                let r = try await TabClient.decode(Moved.self, "storage", "move", app: app,
                                                   args: ["target": target, "confirmed": confirmed])
                if !confirmed && !r.confirm.isEmpty {
                    moving = MoveQuestion(target: target, question: r.confirm)
                } else {
                    report(r.message, failed: false)
                    if confirmed || r.moved == 0 { movedTo = r.command.isEmpty ? nil : target }
                }
            } catch { report(error.localizedDescription, failed: true) }
            working = false
            if confirmed { await load() }
        }
    }
}
