import AppKit
import SwiftUI

struct StorageState: Decodable {
    struct Size: Decodable { let bytes: Int; let files: Int; let shown: String; let errors: [String] }
    struct Folder: Decodable, Identifiable {
        let id, label, path, kind, `where`, note: String
        let exists, movable: Bool
        let prunable: Bool?   // the game has work_prune.py: the work folder's 安全清理空间 button
        let size: Size?
        let detail, confirm: String
    }
    let title: String
    let note: String
    let folders: [Folder]
}

/// The words only this page's Swift side says (the rest comes from tab_storage.py, in the same language).
enum StorageText {
    /// What a finished build does to its work folder (the game's work_prune.py), from this page (user 2026-11-18:
    /// "做成一个按钮 放在 存储 里 替换 清理按钮 叫做安全清理空间"). Emptying the whole folder is `mng page storage clean`.
    static let safeClean = T.s("Free space safely", "安全清理空间")
    static let measuring = T.s("Measuring…", "正在计算大小…")
    static let measure = T.s("Measure space used", "计算空间占用大小")
    static let cancel = T.s("Cancel", "取消")
    static let working = T.s("Working…", "处理中…")
    static let busy = T.s("A build or an update is running: cleaning and moving wait until it ends.",
                          "正在打包或更新：结束后才能清理或移动。")
    /// 游戏卸载 (user 2026-11-20): the app, its work folder, game data image and shader cache, deleted for good; the saves
    /// stay unless the switch says so; the player's own game files and keys never go (tools/builder_jobs/uninstall.py).
    static let uninstall = T.s("Uninstall game", "游戏卸载")
    static let uninstallNote = T.s("Deletes the app and what its builds and the game made, for good. Your saves and your own game files stay.",
                                   "直接删除 App 和打包、游戏生成的文件，不能恢复。存档和你自己的游戏文件保留。")
    static let alsoSaves = T.s("Delete the saves and settings too", "也删除存档和设置")
    static let uninstallNow = T.s("Uninstall", "卸载")
    static let stays = T.s("stays", "保留")
}

struct UninstallPlan: Decodable {
    struct Item: Decodable, Identifiable { let id, label, path, size, stays: String }
    let items: [Item]
    let saves: Bool
    let busy, confirm: String
}

/// The work folder the app records and the game's shader cache (tab_storage.py over tools/builder/storage.py): where
/// they are and their sizes; the work folder sheds what no later build reads (安全清理空间), as each build does when it ends.
struct StoragePage: View {
    let app: String
    @EnvironmentObject var model: Model
    @State private var state: StorageState?
    @State private var sizes: [String: StorageState.Folder] = [:]   // a folder's measured view, by id
    @State private var measuring: Set<String> = []
    @State private var error: String?
    @State private var status = ""
    @State private var failed = false
    @State private var working = false
    @State private var uninstallPlan: UninstallPlan?   // the question shown (sheet)
    @State private var uninstallSaves = false

    private var building: Bool { model.jobRunning }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let s = state {
                Text(s.title).font(.headline)
                Text(s.note).font(.callout).foregroundStyle(.secondary)
                ForEach(s.folders) { f in folderBox(sizes[f.id] ?? f) }
                if building { Label(StorageText.busy, systemImage: "hourglass").font(.callout).foregroundStyle(.secondary) }
                HStack {
                    Button(StorageText.measure) { Task { await measureAll() } }.disabled(working || !measuring.isEmpty)
                    if working { ProgressView().controlSize(.small); Text(StorageText.working).foregroundStyle(.secondary) }
                }
                if !status.isEmpty {
                    Text(status).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled)
                }
                GroupBox {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(StorageText.uninstall).font(.body.weight(.medium))
                        Text(StorageText.uninstallNote).font(.caption).foregroundStyle(.secondary)
                        Button(role: .destructive) { askUninstall() } label: { Label(StorageText.uninstall, systemImage: "trash") }
                            .buttonStyle(.borderedProminent).tint(.red)   // red: role .destructive alone stays grey on macOS
                            .disabled(working || building)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else if error == nil {
                ProgressView()
            }
        }
        .task { await load() }
        .sheet(item: Binding(get: { uninstallPlan.map { PlanBox(plan: $0) } }, set: { if $0 == nil { uninstallPlan = nil } })) { box in
            uninstallSheet(box.plan)
        }
    }

    private struct PlanBox: Identifiable { let plan: UninstallPlan; var id: String { plan.items.map(\.path).joined() + "\(plan.saves)" } }

    @ViewBuilder private func uninstallSheet(_ plan: UninstallPlan) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(StorageText.uninstall).font(.title3.weight(.semibold))
            Text(plan.confirm).font(.callout).fixedSize(horizontal: false, vertical: true)
            ForEach(plan.items) { item in
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text(item.label).font(.body.weight(.medium))
                        if !item.size.isEmpty { Text(item.size).foregroundStyle(.secondary) }
                        if !item.stays.isEmpty { Text("\(StorageText.stays): \(item.stays)").foregroundStyle(.orange) }
                    }
                    Text(item.path).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
            Toggle(StorageText.alsoSaves, isOn: Binding(get: { uninstallSaves }, set: { on in uninstallSaves = on; askUninstall() }))
            if !plan.busy.isEmpty { Label(plan.busy, systemImage: "hourglass").foregroundStyle(.red) }
            HStack {
                Spacer()
                Button(StorageText.cancel) { uninstallPlan = nil }.keyboardShortcut(.cancelAction)
                Button(StorageText.uninstallNow, role: .destructive) { runUninstall() }
                    .buttonStyle(.borderedProminent).tint(.red)
                    .disabled(working || !plan.busy.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 560)
    }

    /// The question: what would go, asked of tab_storage.uninstall_plan (again when the saves switch changes).
    private func askUninstall() {
        Task {
            do { uninstallPlan = try await TabClient.decode(UninstallPlan.self, "storage", "uninstall_plan", app: app, args: ["saves": uninstallSaves]) }
            catch { report(error.localizedDescription, failed: true) }
        }
    }

    private func runUninstall() {
        struct Done: Decodable { let message: String; let app_gone: Bool }
        let saves = uninstallSaves
        uninstallPlan = nil
        working = true
        Task {
            do {
                let r = try await TabClient.decode(Done.self, "storage", "uninstall", app: app, args: ["saves": saves, "confirmed": true])
                report(r.message, failed: false)
                if r.app_gone { await model.loadInstalled() }   // the game's card goes back to "not built yet"
            } catch { report(error.localizedDescription, failed: true) }
            working = false
            uninstallSaves = false
        }
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
                // The work folder sheds only what no later build reads, with no question: the next update stays an express
                // one (user 2026-11-18, in place of 2026-11-12's clean, which emptied it). The shader cache shows where it
                // is and its size. `mng page storage clean|move` do more.
                if f.id == "work" {
                    Text(f.note).font(.caption2).foregroundStyle(.tertiary)
                    if f.prunable == true {
                        Button(StorageText.safeClean) { safeClean() }
                            .disabled(working || building || measuring.contains(f.id) || !f.exists)
                            .padding(.top, 4)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    /// The folders with the sizes measured before (and when); measuring a big work folder takes a while, so a new
    /// measurement waits for the button (user 2026-11-12).
    private func load() async {
        do { state = try await TabClient.decode(StorageState.self, "storage", "state", app: app); error = nil }
        catch { self.error = error.localizedDescription; return }
        sizes = [:]
    }

    /// Each folder's size, at once (the shader cache usually shows first).
    private func measureAll() async {
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

    private func safeClean() {
        struct Freed: Decodable { let message: String }
        working = true
        Task {
            do {
                let r = try await TabClient.decode(Freed.self, "storage", "safe_clean", app: app)
                report(r.message, failed: false)
            } catch { report(error.localizedDescription, failed: true) }
            working = false
            await load()             // the folders again, without the old size
            await measure("work")    // and the size now (the trash being deleted is not counted; user 2026-11-18: an old
        }                            // size stayed after a clean, "清理完 还是这么多？")
    }
}
