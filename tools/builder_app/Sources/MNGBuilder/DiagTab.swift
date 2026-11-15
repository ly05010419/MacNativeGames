import AppKit
import SwiftUI

/// 诊断模式 (user 2026-11-13: "诊断模式 就是 快速模式加上所有诊断", "方便检查 渲染等等问题"): the game started with every
/// diagnostic switch and a run folder (tools/builder_jobs/tab_diag.py start); while it runs, a screenshot, one frame's
/// passes, a fragment shader left out and put back (each with a screenshot), a normal close. The commands are checked in
/// Python before they reach the game's input file: a line the game does not take ends the game.
struct DiagPage: View {
    let app: String
    struct DiagState: Decodable { let running: Bool; let run: String?; let shots: [String]; let log: String? }
    @State private var state: DiagState?
    @State private var line = ""
    @State private var shader = ""
    @State private var busy = false
    @State private var timer: Timer?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(T.diagIntro).font(.callout).foregroundStyle(.secondary)
            HStack {
                Button(T.diagStart) { act("start") }.buttonStyle(.borderedProminent).disabled(busy || state?.running == true)
                Text(state?.running == true ? T.diagRunning : T.diagNotRunning)
                    .foregroundStyle(state?.running == true ? .green : .secondary)
            }
            if state?.running == true {
                HStack {
                    Button(T.diagShot) { act("command", ["what": "shot"]) }
                    Button(T.diagPassdump) { act("command", ["what": "passdump"]) }
                    Button(T.diagClose) { act("command", ["what": "close"]) }
                }.disabled(busy)
                HStack {
                    TextField(T.diagShader, text: $shader).frame(width: 220).font(.body.monospaced())
                    Button(T.diagSkip) { skip("skip") }
                    Button(T.diagUnskip) { skip("unskip") }
                }.disabled(busy)
            }
            if let run = state?.run {
                HStack {
                    Button(T.diagFolder) { act("reveal") }
                    if let log = state?.log { Button(T.diagLog) { NSWorkspace.shared.open(URL(fileURLWithPath: log)) } }
                    Text(run).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
                if let last = state?.shots.last { Text(last).font(.caption.monospaced()).foregroundStyle(.secondary) }
            }
            if !line.isEmpty { Text(line).font(.callout).foregroundStyle(.secondary).textSelection(.enabled) }
        }
        .task { await refresh() }
        .onAppear { timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { _ in Task { await refresh() } } }
        .onDisappear { timer?.invalidate() }
    }

    /// The shader left out (or put back), then a screenshot of the result: the before/after pair is two presses.
    private func skip(_ what: String) {
        let key = shader.trimmingCharacters(in: .whitespaces).lowercased()
        act("command", ["what": what, "key": key], then: ["what": "shot"])
    }

    private func act(_ action: String, _ args: [String: Any] = [:], then: [String: Any]? = nil) {
        busy = true
        Task {
            do {
                let data = try await TabClient.call("diag", action, app: app, args: args)
                let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
                line = (object?["line"] as? String).map(T.diagSent) ?? (object?["run"] as? String ?? "")
                if let then {
                    try await Task.sleep(nanoseconds: 300_000_000)   // the game takes the first line before the screenshot
                    _ = try await TabClient.call("diag", "command", app: app, args: then)
                }
            } catch { line = error.localizedDescription }
            busy = false
            await refresh()
        }
    }

    private func refresh() async { state = try? await TabClient.decode(DiagState.self, "diag", "state", app: app) }
}
