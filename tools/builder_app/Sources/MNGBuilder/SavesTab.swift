import AppKit
import SwiftUI

struct SavesState: Decodable {
    struct Source: Decodable, Identifiable {
        let kind, title_id, game, path, name, line: String
        let files, bytes: Int
        let can_backup: Bool
        var id: String { "\(kind)/\(title_id)" }
    }
    struct Backup: Decodable, Identifiable {
        let path, name, game, game_name, title_id, source, created, size, line: String
        let files, bytes, hashed: Int
        var id: String { path }
    }
    let game_name, note, hash_note, root, root_line, backups_line, running_line: String
    let sources: [Source]
    let backups: [Backup]
    let running: Bool
}

struct RestorePlan: Decodable, Identifiable {
    struct Counts: Decodable { let write, kept, same: Int }
    let backup: SavesState.Backup
    let target: String
    let write, kept, same: [String]
    let counts: Counts
    let plan_id, message, kept_line: String
    let running: Bool
    var id: String { plan_id }
}

/// Texts only this page shows (the rest come from tab_saves.py in the language asked).
private enum ST {
    static let title = T.s("Saves", "存档")
    static let label = T.s("Label (optional)", "标签（可不填）")
    static let backup = T.s("Back up", "备份")
    static let backupTo = T.s("Back up to…", "备份到…")
    static let restore = T.s("Restore…", "恢复…")
    static let restoreOther = T.s("Restore from another folder…", "从其他文件夹恢复…")
    static let show = T.s("Show in Finder", "在访达中显示")
    static let openFolder = T.s("Open backups folder", "打开备份文件夹")
    static let refresh = T.s("Read again", "重新读取")
    static let backups = T.s("Backups", "备份")
    static let reading = T.s("Reading the saves…", "正在读取存档…")
    static let backingUp = T.s("Backing up…", "正在备份…")
    static let restoring = T.s("Backing up the current save, then restoring…", "先备份当前存档，再恢复…")
    static let confirmTitle = T.s("Restore this backup?", "恢复这份备份？")
    static let confirm = T.s("Restore", "恢复")
    static let cancel = T.s("Cancel", "取消")
    static let chooseRoot = T.s("Choose where to keep backups.", "选择备份放在哪里。")
    static let chooseBackup = T.s("Choose a backup folder to restore.", "选择要恢复的备份文件夹。")
    static func written(_ paths: [String], _ count: Int) -> String {
        guard count > 0 else { return T.s("Nothing to write.", "没有要写入的文件。") }
        let shown = paths.prefix(8).joined(separator: "\n")
        let more = count > 8 ? "\n" + T.s("… and \(count - 8) more", "……还有 \(count - 8) 个") : ""
        return T.s("Written:", "将写入：") + "\n" + shown + more
    }
    static func hashed(_ b: SavesState.Backup) -> String {
        T.s("\(b.hashed) of \(b.files) files with SHA-256", "\(b.files) 个文件中 \(b.hashed) 个记了 SHA-256")
    }
}

/// Where the game keeps its save, backups (plain folders plus backup.json) and restore (tab_saves.py over
/// tools/builder/saves.py). A restore shows its plan and asks first; Python refuses it while the game runs.
struct SavesPage: View {
    let app: String
    @State private var state: SavesState?
    @State private var error: String?
    @State private var status = ""
    @State private var statusIsError = false
    @State private var busy = false
    @State private var label = ""
    @State private var plan: RestorePlan?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let s = state {
                Text(ST.title).font(.headline)
                Text(s.note).font(.caption).foregroundStyle(.secondary)
                HStack {
                    Text(s.root_line).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    Button(ST.openFolder) { reveal(nil) }.controlSize(.small)
                }
                if !s.running_line.isEmpty {
                    Label(s.running_line, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                }
                TextField(ST.label, text: $label).frame(maxWidth: 280).disabled(busy)
                ForEach(s.sources) { source in sourceRow(source, s) }
                Text(ST.backups).font(.headline).padding(.top, 6)
                Text(s.backups_line).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                ForEach(s.backups) { b in backupRow(b, s) }
                Text(s.hash_note).font(.caption2).foregroundStyle(.tertiary)
                HStack {
                    Button(ST.restoreOther) { if let f = Self.chooseFolder(ST.chooseBackup, s.root) { showPlan(f) } }
                        .disabled(busy || s.running)
                    Button(ST.refresh) { Task { await load() } }.disabled(busy)
                }
                if !status.isEmpty {
                    Text(status).foregroundStyle(statusIsError ? .red : .secondary).textSelection(.enabled)
                }
            } else if error == nil {
                ProgressView(ST.reading)
            }
        }
        .task { await load() }
        .alert(ST.confirmTitle, isPresented: Binding(get: { plan != nil }, set: { if !$0 { plan = nil } }), presenting: plan) { p in
            Button(ST.confirm, role: .destructive) { restore(p) }
            Button(ST.cancel, role: .cancel) {}
        } message: { p in
            Text([p.message, ST.written(p.write, p.counts.write), p.kept_line].joined(separator: "\n\n"))
        }
    }

    private func sourceRow(_ source: SavesState.Source, _ s: SavesState) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Text(source.name)
                Text(source.path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                Text(source.line).font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button(ST.backup) { backup(source, root: nil) }
                    Button(ST.backupTo) { if let r = Self.chooseFolder(ST.chooseRoot, s.root) { backup(source, root: r) } }
                }
                .controlSize(.small)
                .disabled(busy || !source.can_backup)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func backupRow(_ b: SavesState.Backup, _ s: SavesState) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Text(b.name).textSelection(.enabled)
                Text(b.line).font(.caption).foregroundStyle(.secondary)
                Text(ST.hashed(b) + (b.title_id.isEmpty ? "" : " · \(b.title_id)")).font(.caption2).foregroundStyle(.tertiary)
                HStack {
                    Button(ST.restore) { showPlan(b.path) }.disabled(busy || s.running)
                    Button(ST.show) { reveal(b.path) }
                }
                .controlSize(.small)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func load() async {
        do { state = try await TabClient.decode(SavesState.self, "saves", "state", app: app); error = nil }
        catch { self.error = error.localizedDescription }
    }

    private func say(_ text: String, error: Bool = false) { status = text; statusIsError = error }

    private func backup(_ source: SavesState.Source, root: String?) {
        struct Made: Decodable { let message: String }
        var args: [String: Any] = ["kind": source.kind, "title_id": source.title_id, "label": label]
        if let root { args["root"] = root }
        busy = true; say(ST.backingUp)
        Task {
            do {
                let made = try await TabClient.decode(Made.self, "saves", "backup", app: app, args: args)
                await load(); say(made.message)
            } catch { say(error.localizedDescription, error: true) }
            busy = false
        }
    }

    private func showPlan(_ folder: String) {
        busy = true; say("")
        Task {
            do { plan = try await TabClient.decode(RestorePlan.self, "saves", "plan", app: app, args: ["backup": folder]) }
            catch { say(error.localizedDescription, error: true) }
            busy = false
        }
    }

    private func restore(_ p: RestorePlan) {
        struct Done: Decodable { let message: String }
        busy = true; say(ST.restoring)
        Task {
            do {
                let done = try await TabClient.decode(Done.self, "saves", "restore", app: app,
                                                      args: ["backup": p.backup.path, "plan_id": p.plan_id])
                await load(); say(done.message)
            } catch { await load(); say(error.localizedDescription, error: true) }
            busy = false
        }
    }

    private func reveal(_ path: String?) {
        struct Opened: Decodable { let opened: String }
        Task {
            do { _ = try await TabClient.decode(Opened.self, "saves", "reveal", app: app, args: path.map { ["path": $0] } ?? [:]) }
            catch { say(error.localizedDescription, error: true) }
        }
    }

    @MainActor private static func chooseFolder(_ message: String, _ start: String) -> String? {
        let panel = NSOpenPanel()
        panel.message = message
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = URL(fileURLWithPath: start)
        return panel.runModal() == .OK ? panel.url?.path : nil
    }
}
