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
            } else if error == nil {
                ProgressView()
            }
        }
        .task { await load() }
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
