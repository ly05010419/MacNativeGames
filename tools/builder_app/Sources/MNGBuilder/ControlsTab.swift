import SwiftUI

struct ControlsView: Decodable {
    struct Answer: Decodable, Hashable { let id, label: String }
    struct Row: Decodable, Identifiable {
        let input, name, shown, custom, caption: String
        let button: String?
        let changed: Bool
        var id: String { input }
    }
    struct Pad: Decodable, Identifiable { let id, label, custom, caption: String; let input: String? }
    let game, title, token, mode, broken, message, confirm: String
    let which, ask, pending, on_disk, ok: Bool
    let answers: [Answer]
    let answer: String?
    let rows: [Row]
    let buttons: [Pad]
    let desired: [String: String?]
    let converted, convert: [String]
    let texts: [String: String]

    func text(_ key: String) -> String { texts[key] ?? key }
    var editable: Bool { !rows.isEmpty }
}

struct ControlsIcons: Decodable {
    let offered, outdated: Bool
    let reason, line, button, env, download, switch_off, kept, making: String
    let font_mods: [String]
}

/// Swift-only texts of this page.
enum CT {
    static let confirm = T.s("Please confirm", "请确认")
    static let yes = T.s("Continue", "继续")
    static let no = T.s("Cancel", "取消")
    static let minutes = T.s("This can take a few minutes; the page waits for it.", "可能要几分钟，这一页会等它做完。")
}

/// The old window's Controls tab (tab_controls.py over tools/builder's controller_controls, controls_file,
/// controller_icons and keyboard_layout): which controller, each input's button (a list and a drawn pad), the Xbox
/// letters preset, Save, Cancel changes, Restore defaults, the button icons, and the keyboard layout with its key test.
/// The page keeps only the last answer: its token and the pending layout go back with every call.
struct ControlsPage: View {
    let app: String
    @State private var view: ControlsView?
    @State private var icons: ControlsIcons?
    @State private var error: String?
    @State private var selected: String?
    @State private var marked: String?
    @State private var caption = ""
    @State private var status = ""
    @State private var statusOK = true
    @State private var busy = false
    @State private var making = false
    @State private var question: Question?

    struct Question: Identifiable { let id = UUID(); let text: String; let reply: (Bool) -> Void }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let v = view {
                Text(v.title).font(.headline)
                Text(v.text("note")).font(.caption).foregroundStyle(.secondary)
                header(v)
                if v.editable { editor(v) }
                footer(v)
            } else if error == nil {
                ProgressView()
            }
            Divider().padding(.vertical, 6)
            ControlsKeyboard(app: app)
        }
        .task { await load() }
        .alert(CT.confirm, isPresented: Binding(get: { question != nil }, set: { _ in }), presenting: question) { q in
            Button(CT.yes) { question = nil; q.reply(true) }
            Button(CT.no, role: .cancel) { question = nil; q.reply(false) }
        } message: { q in Text(q.text) }
    }

    @ViewBuilder private func header(_ v: ControlsView) -> some View {
        if !v.broken.isEmpty { Label(v.broken, systemImage: "exclamationmark.triangle").foregroundStyle(.red) }
        if v.which { Text(v.text("which")) }
        if !v.mode.isEmpty { Text(v.mode).font(.caption).foregroundStyle(.secondary) }
        if v.ask {
            HStack {
                ForEach(v.answers, id: \.id) { a in
                    Button(a.label) { answer(a.id) }
                        .buttonStyle(.bordered).tint(v.answer == a.id ? .accentColor : nil).disabled(busy)
                }
            }
        }
    }

    @ViewBuilder private func editor(_ v: ControlsView) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Button(v.text("preset")) { xboxLetters() }.disabled(busy)
            Text(v.text("preset_note")).font(.caption).foregroundStyle(.secondary)
        }
        HStack(alignment: .top, spacing: 18) {
            VStack(spacing: 4) {
                ForEach(v.rows) { r in
                    Button { pick(r) } label: {
                        HStack {
                            Text(r.name)
                            Spacer()
                            Text(r.shown).foregroundStyle(r.changed ? Color.accentColor : .secondary)
                        }
                        .padding(.horizontal, 10).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 6)
                            .fill(selected == r.input ? Color.accentColor.opacity(0.25) : Color.secondary.opacity(0.08)))
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(selected == r.input ? .isSelected : [])
                }
            }
            .frame(minWidth: 260, maxWidth: 340)
            VStack(alignment: .leading, spacing: 8) {
                ControlsPad(buttons: v.buttons, marked: marked) { press($0) }
                Text(caption.isEmpty ? v.text("pick") : caption).font(.callout)
                iconsLine(v)
            }
            .frame(width: 340, alignment: .leading)
        }
    }

    @ViewBuilder private func iconsLine(_ v: ControlsView) -> some View {
        if let i = icons {
            Text(i.line).font(.caption).foregroundStyle(i.outdated ? .red : .secondary)
            if !i.button.isEmpty {
                HStack {
                    Button(i.button) { updateIcons(i) }.disabled(busy || !i.offered || v.pending)
                    if making { ProgressView().controlSize(.small) }
                }
                if making { Text(CT.minutes).font(.caption2).foregroundStyle(.secondary) }
            }
        }
    }

    @ViewBuilder private func footer(_ v: ControlsView) -> some View {
        if !status.isEmpty {
            Text(status).font(.callout).foregroundStyle(statusOK ? Color.green : Color.red).textSelection(.enabled)
        }
        HStack {
            Button(v.text("restore")) { restore() }.disabled(busy || !v.on_disk)
            Spacer()
            Button(v.text("cancel")) { Task { await load(); status = "" } }.disabled(busy || !v.pending)
            Button(v.text("save")) { save() }.buttonStyle(.borderedProminent).disabled(busy || !v.pending)
        }
    }

    // MARK: - calls

    private func args(_ v: ControlsView) -> [String: Any] {
        ["token": v.token, "desired": v.desired.mapValues { $0.map { $0 as Any } ?? NSNull() }, "converted": v.converted]
    }

    private func call(_ action: String, _ args: [String: Any] = [:]) async -> ControlsView? {
        do {
            let v = try await TabClient.decode(ControlsView.self, "controls", action, app: app, args: args)
            error = nil
            return v
        } catch {
            self.error = error.localizedDescription
            return nil
        }
    }

    /// The files as they are now; the pending changes on screen go.
    private func load() async {
        busy = true
        if let v = await call("state") { view = v; selected = nil; marked = nil; caption = "" }
        busy = false
        await loadIcons()
    }

    private func loadIcons() async {
        icons = try? await TabClient.decode(ControlsIcons.self, "controls", "icons_info", app: app)
    }

    private func confirm(_ text: String) async -> Bool {
        await withCheckedContinuation { c in question = Question(text: text) { c.resume(returning: $0) } }
    }

    private func answer(_ id: String) {
        busy = true
        Task {
            if let v = await call("answer", ["controller": id]) { view = v; selected = nil; marked = nil; caption = "" }
            busy = false
            await loadIcons()
        }
    }

    private func pick(_ r: ControlsView.Row) {
        selected = r.input; marked = r.button; caption = r.caption
    }

    /// A drawn button: with a row picked, the swap (custom buttons converted once agreed); without one, what it does.
    private func press(_ button: String) {
        guard let v = view, v.editable, !busy else { return }
        guard let input = selected else {
            marked = button; caption = v.buttons.first { $0.id == button }?.caption ?? ""
            return
        }
        let before = v.rows.first { $0.input == input }?.button
        marked = button
        busy = true
        Task {
            var a = args(v); a["input"] = input; a["button"] = button
            var r = await call("swap", a)
            if let asked = r, !asked.confirm.isEmpty {
                if await confirm(asked.confirm) { a["confirmed"] = true; r = await call("swap", a) } else { r = nil; marked = before }
            }
            if let r { shown(r); if !r.ok { selected = nil; marked = nil } }
            busy = false
        }
    }

    private func xboxLetters() {
        guard let v = view else { return }
        busy = true
        Task {
            var a = args(v)
            var r = await call("xbox_letters", a)
            if let asked = r, !asked.confirm.isEmpty {
                if await confirm(asked.confirm) { a["confirmed"] = true; r = await call("xbox_letters", a) } else { r = nil }
            }
            if let r { shown(r); selected = nil; marked = nil }
            busy = false
        }
    }

    /// A preview's answer: the caption says it; a refusal (the files changed) also goes to the status line.
    private func shown(_ r: ControlsView) {
        view = r; caption = r.message
        if !r.ok { status = r.message; statusOK = false }
    }

    private func save() {
        guard let v = view, v.pending else { return }
        busy = true; status = v.text("saving"); statusOK = true
        Task {
            if let r = await call("save", args(v)) {
                view = r; status = r.message; statusOK = r.ok; selected = nil; marked = nil; caption = ""
            }
            busy = false
            await loadIcons()
        }
    }

    private func restore() {
        guard let v = view else { return }
        Task {
            guard await confirm(v.text("confirm_restore")) else { return }
            busy = true; status = v.text("saving"); statusOK = true
            if let r = await call("restore", ["token": v.token]) {
                view = r; status = r.message; statusOK = r.ok; selected = nil; marked = nil; caption = ""
            }
            busy = false
            await loadIcons()
        }
    }

    /// As the old page: the fontTools download agreed first when the icons venv is missing or broken, other font mods
    /// switched off once agreed (else nothing is made), then one call that makes the mod and applies the mods.
    private func updateIcons(_ i: ControlsIcons) {
        guard i.offered, view?.pending == false else { return }
        Task {
            let install = i.env != "ready"
            if install, !(await confirm(i.download)) { return }
            if !i.font_mods.isEmpty, !(await confirm(i.switch_off)) { status = i.kept; statusOK = false; return }
            struct Made: Decodable { let ok: Bool; let message: String; let icons: ControlsIcons }
            busy = true; making = true; status = i.making; statusOK = true
            do {
                let r = try await TabClient.decode(Made.self, "controls", "icons", app: app,
                                                   args: ["install_env": install, "switch_off": i.font_mods])
                status = r.message; statusOK = r.ok; icons = r.icons
            } catch {
                status = error.localizedDescription; statusOK = false
            }
            making = false; busy = false
        }
    }
}
