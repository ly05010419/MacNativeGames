import AppKit
import SwiftUI

/// The game's keyboard layout (keyboard.txt, else the defaults; tab_controls.keyboard over keyboard_layout) and the old
/// page's key test: while it runs, each key pressed in this window is named with the actions the layout gives it. Key
/// codes are the runtime's own table (keyboard_map.h); nothing is sent to the game, and Command shortcuts still work.
struct ControlsKeyboard: View {
    let app: String

    struct Layout: Decodable {
        struct Action: Decodable, Identifiable { let id, title: String; let keys: [String] }
        struct Key: Decodable { let name, label: String; let actions: [String] }
        let source, note, `where`: String
        let texts, formats: [String: String]
        let actions: [Action]
        let keys: [String: Key]
        func text(_ k: String) -> String { texts[k] ?? k }
        func format(_ k: String, _ values: [String: String]) -> String {
            values.reduce(formats[k] ?? k) { $0.replacingOccurrences(of: "{\($1.key)}", with: $1.value) }
        }
    }

    @State private var layout: Layout?
    @State private var error: String?
    @State private var testing = false
    @State private var monitor: Any?
    @State private var stopAt: Task<Void, Never>?
    @State private var pressed = ""
    @State private var actions = ""
    @State private var bound = false
    @State private var detail = ""

    private static let unbound = [54: "Command", 55: "Command", 57: "Caps Lock", 63: "fn"]
    private static let modifierKeys: [Int: NSEvent.ModifierFlags] = [
        56: .shift, 60: .shift, 59: .control, 62: .control, 58: .option, 61: .option, 54: .command, 55: .command,
        57: .capsLock, 63: .function]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let error { Label(error, systemImage: "xmark.octagon").foregroundStyle(.red) }
            if let l = layout {
                Text(l.text("title")).font(.headline)
                Text(l.text("note")).font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button(testing ? l.text("stop") : l.text("start")) { testing ? stop() : start() }
                    Text(testing ? l.text("running") : l.text("stopped")).font(.caption).foregroundStyle(.secondary)
                }
                if testing || !pressed.isEmpty {
                    Text(pressed.isEmpty ? l.text("none") : pressed)
                    if !actions.isEmpty { Text(actions).font(.callout).foregroundStyle(bound ? .primary : .secondary) }
                    if !detail.isEmpty { Text(detail).font(.caption).foregroundStyle(.secondary) }
                }
                if !l.note.isEmpty { Text(l.note).font(.caption).foregroundStyle(.orange) }
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 3) {
                    ForEach(l.actions) { a in
                        GridRow {
                            Text(a.title)
                            Text(a.keys.isEmpty ? "—" : a.keys.joined(separator: ", ")).foregroundStyle(.secondary)
                        }
                        .font(.callout)
                    }
                }
                .padding(.top, 4)
                if !l.source.isEmpty { Text(l.source).font(.caption2).foregroundStyle(.tertiary).textSelection(.enabled) }
                Text(l.where).font(.caption).foregroundStyle(.secondary)
            }
        }
        .task { await load() }
        .onDisappear { stop() }
    }

    private func load() async {
        do { layout = try await TabClient.decode(Layout.self, "controls", "keyboard", app: app); error = nil }
        catch { self.error = error.localizedDescription }
    }

    private func start() {
        guard layout != nil, monitor == nil else { return }
        testing = true; pressed = ""; actions = ""; detail = ""
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { event in
            handle(event)
            // plain keys stop here, so the test does not press the page's buttons; Command shortcuts go on
            return event.type == .keyDown && !event.modifierFlags.contains(.command) ? nil : event
        }
        stopAt = Task { try? await Task.sleep(for: .seconds(30)); if !Task.isCancelled { stop() } }
    }

    private func stop() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil; testing = false
        stopAt?.cancel(); stopAt = nil
    }

    private func handle(_ event: NSEvent) {
        guard testing, let l = layout else { return }
        let code = Int(event.keyCode)
        if event.type == .flagsChanged {
            guard let flag = Self.modifierKeys[code], event.modifierFlags.contains(flag) else { return }   // a release
        }
        if event.type == .keyDown, code == 53 { stop(); return }                                          // Escape
        let key = l.keys[String(code)]
        let label = key?.label ?? Self.unbound[code] ?? event.charactersIgnoringModifiers?.uppercased() ?? "?"
        let own = Self.modifierKeys[code]
        let mods = [(NSEvent.ModifierFlags.shift, "Shift"), (.control, "Control"), (.option, "Option"), (.command, "Command")]
            .filter { event.modifierFlags.contains($0.0) && $0.0 != own }.map(\.1)
        pressed = mods.isEmpty ? l.format("key", ["key": label])
                               : l.format("modifiers", ["modifiers": mods.joined(separator: "+"), "key": label])
        bound = !(key?.actions.isEmpty ?? true)
        actions = bound ? l.format("actions", ["key": label, "actions": key!.actions.joined(separator: ", ")])
                        : l.format("unbound", ["key": label])
        var parts: [String] = []
        if event.type == .keyDown, let c = event.characters, c.count == 1,
           let s = c.unicodeScalars.first, !CharacterSet.whitespacesAndNewlines.contains(s),
           !CharacterSet.controlCharacters.contains(s), s.properties.generalCategory != .privateUse {
            parts.append(l.format("char", ["char": c]))
        }
        parts.append(l.format("code", ["code": String(code)]))
        detail = parts.joined(separator: " · ")
    }
}
