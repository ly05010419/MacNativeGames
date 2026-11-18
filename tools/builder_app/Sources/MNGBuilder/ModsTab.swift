import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ModsState: Decodable {
    struct Mod: Decodable, Identifiable {
        let id, name: String
        let enabled, valid, bad: Bool
        let code: Bool?                 // nil: no code a build can merge, so no code switch
        let details: [String]
        let confirm_remove: String
    }
    struct Backups: Decodable { let names: [String]; let text, confirm: String }
    struct Status: Decodable { let ok: Bool; let text: String }
    let name, version, reason, note, empty: String
    let checked: Bool
    let changed, code_pending, not_applied, switched_off, code_line: String
    let mods: [Mod]
    let backups: Backups?
    let labels: [String: String]
    let archives: [String]
    let status: Status?             // only in the answer of add
}

private enum ModsText {
    static let up = T.s("Move up", "上移")
    static let down = T.s("Move down", "下移")
    static let cancel = T.s("Cancel", "取消")
}

/// The player's own mods (tab_mods.py over tools/builder's mod models): add a folder or an archive, switch on, order,
/// remove, Apply, the player's own backups. Every text of the old window's Mods tab comes from Python, in the Mac's
/// language; the page holds no rule.
struct ModsPage: View {
    let app: String
    @State private var state: ModsState?
    @State private var error: String?
    @State private var changed = ""         // a quick read after a change keeps the last full check's line
    @State private var status = ""
    @State private var statusOK = true
    @State private var busy = false
    @State private var removing: ModsState.Mod?
    @State private var clearingBackups = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let s = state {
                Text("\(s.name) · \(s.version)").font(.headline)
                if !s.reason.isEmpty {
                    Text(s.reason).foregroundStyle(.secondary)
                } else {
                    content(s)
                }
            } else if error == nil {
                ProgressView()
            }
        }
        .task { await load() }
        .confirmationDialog(removing?.confirm_remove ?? "", isPresented: Binding(get: { removing != nil },
                                                                                set: { if !$0 { removing = nil } })) {
            Button(state?.labels["remove"] ?? "", role: .destructive) {
                if let m = removing { change("remove", ["mod": m.id]) }
                removing = nil
            }
            Button(ModsText.cancel, role: .cancel) { removing = nil }
        }
        .confirmationDialog(state?.backups?.confirm ?? "", isPresented: $clearingBackups) {
            Button(state?.labels["remove_backups"] ?? "", role: .destructive) {
                if let names = state?.backups?.names { change("remove_backups", ["names": names]) }
            }
            Button(ModsText.cancel, role: .cancel) {}
        }
    }

    @ViewBuilder private func content(_ s: ModsState) -> some View {
        Text(s.note).font(.callout).foregroundStyle(.secondary)
        HStack {
            Button(s.labels["add"] ?? "") { pick(archive: false) }
            Button(s.labels["add_archive"] ?? "") { pick(archive: true) }
        }
        .disabled(busy)
        ForEach([changed, s.code_pending, s.not_applied, s.switched_off].filter { !$0.isEmpty }, id: \.self) { line in
            Label(line, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
        }
        if !s.code_line.isEmpty { Label(s.code_line, systemImage: "info.circle").font(.callout).foregroundStyle(.secondary) }
        if s.mods.isEmpty { Text(s.empty).foregroundStyle(.secondary) }
        ForEach(Array(s.mods.enumerated()), id: \.element.id) { index, mod in
            row(mod, first: index == 0, last: index == s.mods.count - 1, labels: s.labels)
        }
        if let b = s.backups {
            HStack {
                Text(b.text).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button(s.labels["remove_backups"] ?? "") { clearingBackups = true }.disabled(busy)
            }
        }
        Divider()
        HStack(alignment: .top) {
            if !status.isEmpty {
                Text(status).foregroundStyle(statusOK ? Color.green : Color.red).textSelection(.enabled)
            }
            Spacer()
            Button(s.labels["apply"] ?? "") { apply() }.buttonStyle(.borderedProminent).disabled(busy)
        }
    }

    private func row(_ mod: ModsState.Mod, first: Bool, last: Bool, labels: [String: String]) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Toggle(mod.name, isOn: Binding(get: { mod.enabled }, set: { change("enable", ["mod": mod.id, "on": $0]) }))
                        .toggleStyle(.switch).font(.headline).disabled(busy || !mod.valid)
                    Spacer()
                    Button(labels["remove"] ?? "") { removing = mod }.disabled(busy)
                    Button { change("move", ["mod": mod.id, "step": -1]) } label: { Image(systemName: "chevron.up") }
                        .help(ModsText.up).disabled(busy || first)
                    Button { change("move", ["mod": mod.id, "step": 1]) } label: { Image(systemName: "chevron.down") }
                        .help(ModsText.down).disabled(busy || last)
                }
                if let code = mod.code {
                    Toggle(labels["code_toggle"] ?? "", isOn: Binding(get: { code }, set: { change("code", ["mod": mod.id, "on": $0]) }))
                        .toggleStyle(.switch).font(.caption).disabled(busy).padding(.leading, 16)
                }
                ForEach(mod.details, id: \.self) { d in
                    Text(d).font(.caption).foregroundStyle(mod.bad ? Color.red : Color.secondary).textSelection(.enabled)
                        .padding(.leading, 16)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func show(_ s: ModsState) {
        if s.checked { changed = s.changed }
        state = s
        error = nil
    }

    /// The full read (import, checks again): on opening the page; Apply answers with one too.
    private func load() async {
        do { show(try await TabClient.decode(ModsState.self, "mods", "state", app: app, args: ["full": true])) }
        catch { self.error = error.localizedDescription }
    }

    /// One change; the answer is the new state (a quick read, as the old tab did after a toggle or a move).
    private func change(_ action: String, _ args: [String: Any]) {
        guard !busy else { return }
        busy = true
        Task {
            do { show(try await TabClient.decode(ModsState.self, "mods", action, app: app, args: args)) }
            catch { status = error.localizedDescription; statusOK = false }
            busy = false
        }
    }

    private func pick(archive: Bool) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = !archive
        panel.canChooseFiles = archive
        if archive {
            panel.allowedContentTypes = (state?.archives ?? ["zip"]).compactMap { UTType(filenameExtension: $0) }
        }
        guard !busy, panel.runModal() == .OK, let url = panel.url else { return }
        busy = true
        status = state?.labels["adding"] ?? ""; statusOK = true
        Task {
            do {
                let s = try await TabClient.decode(ModsState.self, "mods", "add", app: app, args: ["path": url.path])
                show(s)
                status = s.status?.text ?? ""; statusOK = s.status?.ok ?? true
            } catch { status = error.localizedDescription; statusOK = false }
            busy = false
        }
    }

    private func apply() {
        struct Applied: Decodable { let ok: Bool; let message: String; let lines: [String]; let state: ModsState }
        guard !busy else { return }
        busy = true
        status = state?.labels["applying"] ?? ""; statusOK = true
        Task {
            do {
                let r = try await TabClient.decode(Applied.self, "mods", "apply", app: app)
                show(r.state)
                status = ([r.message] + r.lines).filter { !$0.isEmpty }.joined(separator: "\n"); statusOK = r.ok
            } catch { status = error.localizedDescription; statusOK = false }
            busy = false
        }
    }
}
