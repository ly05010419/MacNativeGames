import SwiftUI

struct SettingsState: Decodable {
    struct Choice: Decodable, Hashable { let id: String; let label: String }
    struct Option: Decodable, Identifiable { let id, label, description, `where`: String; let choices: [Choice]; let chosen: String }
    let name: String
    let version: String
    let reason: String
    let options: [Option]
    let notes: [String]
}

/// The game's start options (tab_settings.py over tools/builder/start_options.py) and the GPU check; the card's Start game starts it.
struct SettingsPage: View {
    let app: String
    @State private var state: SettingsState?
    @State private var error: String?
    @State private var gpu = "checking"
    @State private var busy = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red).textSelection(.enabled) }
            if let s = state {
                Text("\(s.name) · \(s.version)").font(.headline)
                if !s.reason.isEmpty { Text(s.reason).foregroundStyle(.secondary) }
                ForEach(s.options) { o in
                    GroupBox {
                        VStack(alignment: .leading, spacing: 4) {
                            Picker(o.label, selection: Binding(get: { o.chosen }, set: { choose(o.id, $0) })) {
                                ForEach(o.choices, id: \.id) { c in Text(c.label).tag(c.id) }
                            }
                            .disabled(busy)
                            Text(o.description).font(.caption).foregroundStyle(.secondary)
                            Text(o.where).font(.caption2).foregroundStyle(.tertiary)
                        }
                    }
                }
                ForEach(s.notes, id: \.self) { n in Label(n, systemImage: "info.circle").font(.callout).foregroundStyle(.secondary) }
                // the game starts with the card's Start game (GameActions), which passes these choices (user 2026-11-12:
                // "设置里 去掉 游玩按钮 统一使用 启动游戏按钮")
                Text(T.gpu(gpu)).font(.caption).foregroundStyle(.secondary)
            } else if error == nil {
                ProgressView()
            }
        }
        .task { await load(); await probe() }
    }

    private func load() async {
        do { state = try await TabClient.decode(SettingsState.self, "settings", "state", app: app); error = nil }
        catch { self.error = error.localizedDescription }
    }

    private func probe() async {
        struct Probe: Decodable { let gpu: String }
        gpu = (try? await TabClient.decode(Probe.self, "settings", "probe", app: app).gpu) ?? "unknown"
    }

    private func choose(_ option: String, _ choice: String) {
        busy = true
        Task {
            do { state = try await TabClient.decode(SettingsState.self, "settings", "choose", app: app,
                                                    args: ["option": option, "choice": choice]); error = nil }
            catch { self.error = error.localizedDescription }
            busy = false
        }
    }
}
