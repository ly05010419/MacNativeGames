import AppKit
import SwiftUI

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
    static let clean = T.s("Clean", "清理")
    static let cleanTitle = T.s("Clean the build cache?", "清理打包缓存？")
    static let measuring = T.s("Measuring…", "正在计算大小…")
    static let measure = T.s("Measure space used", "计算空间占用大小")
    static let cancel = T.s("Cancel", "取消")
    static let working = T.s("Working…", "处理中…")
    static let busy = T.s("A build or an update is running: cleaning and moving wait until it ends.",
                          "正在打包或更新：结束后才能清理或移动。")
    static func cleanFallback(_ path: String) -> String {
        T.s("Delete everything inside \(path)? Saves and the app are not touched.", "删除 \(path) 里的全部内容？存档和 App 不受影响。")
    }
}

/// The work folder the app records and the game's shader cache (tab_storage.py over tools/builder/storage.py): where
/// they are and their sizes; the work folder can be cleaned after a question saying what that costs the next update.
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
    @State private var cleaning: StorageState.Folder?

    private var building: Bool { model.phase == .building || model.phase == .cancelling || model.updating != nil }

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
        .confirmationDialog(StorageText.cleanTitle, isPresented: Binding(get: { cleaning != nil }, set: { if !$0 { cleaning = nil } }),
                            presenting: cleaning) { f in
            Button(StorageText.clean, role: .destructive) { clean(f) }
            Button(StorageText.cancel, role: .cancel) {}
        } message: { f in Text(f.confirm.isEmpty ? StorageText.cleanFallback(f.path) : f.confirm) }
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
                // Only the work folder can be cleaned here, after a question saying what it costs the next update; the
                // shader cache shows where it is and its size (user 2026-11-12). `mng page storage clean|move` do more.
                if f.id == "work" {
                    Text(f.note).font(.caption2).foregroundStyle(.tertiary)
                    Button(StorageText.clean) { cleaning = f }
                        .disabled(working || building || measuring.contains(f.id) || !f.exists || f.kind == "unreachable")
                        .padding(.top, 4)
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

    private func clean(_ f: StorageState.Folder) {
        struct Cleaned: Decodable { let message: String }
        working = true
        Task {
            do {
                let r = try await TabClient.decode(Cleaned.self, "storage", "clean", app: app, args: ["folder": f.id])
                report(r.message, failed: false)
            } catch { report(error.localizedDescription, failed: true) }
            working = false
            await load()   // the folders again, without the cleaned one's old size; the button measures it
        }
    }
}
