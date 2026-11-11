import AppKit
import SwiftUI

/// The one button, and while it builds: the bar, the stage, the step, the times and the log; at the end, Play or the
/// error with its log.
struct ActionSection: View {
    @EnvironmentObject var model: Model
    @State private var showLog = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            buttons
            if model.phase != .idle { progress }
            if let f = model.failure, model.phase == .failed { failure(f) }
            if !model.lines.isEmpty {
                DisclosureGroup(T.log, isExpanded: $showLog) { logView }
            }
        }
    }

    private var buttons: some View {
        HStack {
            switch model.phase {
            case .building:
                Button(T.cancel, role: .cancel) { model.cancel() }
            case .cancelling:
                Button(T.cancelling) {}.disabled(true)
            default:
                Button(action: model.build) {
                    Text(model.existingApp == nil ? T.build : T.rebuild).frame(minWidth: 160)
                }
                .buttonStyle(.borderedProminent).controlSize(.large).disabled(!model.canBuild)
                if model.existingApp != nil || model.phase == .done {
                    Button(T.play, action: model.play).controlSize(.large)
                    Button(T.showInFinder) { model.reveal(model.appPath ?? model.existingApp) }
                }
            }
            Spacer()
            if model.phase == .idle {
                Text(model.canBuild ? T.ready : (model.plan == nil ? "" : T.blocked))
                    .foregroundStyle(model.canBuild ? Color.green : Color.secondary)
            }
        }
    }

    private var progress: some View {
        VStack(alignment: .leading, spacing: 4) {
            ProgressView(value: model.fraction)
            HStack {
                Text(headline).font(.headline)
                Spacer()
                Text(times).font(.callout.monospacedDigit()).foregroundStyle(.secondary)
            }
            if model.phase == .building || model.phase == .cancelling {
                if !model.stageText.isEmpty { Text(model.stageText).font(.callout).foregroundStyle(.secondary) }
                if !model.step.isEmpty {
                    HStack {
                        Text(model.step).font(.caption.monospaced()).foregroundStyle(.secondary)
                        if let (done, total) = model.stepCount {
                            ProgressView(value: Double(done), total: Double(total)).frame(maxWidth: 160)
                            Text("\(done) / \(total)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                        }
                    }
                }
                if !model.measured { Text(T.firstBuild).font(.caption).foregroundStyle(.secondary) }
            }
            if model.phase == .cancelled { Text(T.cancelled).foregroundStyle(.secondary) }
        }
    }

    private var headline: String {
        switch model.phase {
        case .done: return T.doneOK
        case .failed: return T.failed
        case .cancelling: return T.cancelling
        case .cancelled: return T.cancel
        default: return model.stage.isEmpty ? T.building : model.stage
        }
    }

    private var times: String {
        var parts = [T.elapsed(T.clock(model.elapsed))]
        if model.phase == .building, let left = model.left, model.measured { parts.append(T.left(T.minutes(left))) }
        return parts.joined(separator: " · ")
    }

    private func failure(_ f: BuildEvent) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(f.message ?? "", systemImage: "xmark.octagon.fill").foregroundStyle(.red).textSelection(.enabled)
            if let detail = f.detail, !detail.isEmpty {
                ScrollView { Text(detail).font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
                    .frame(maxHeight: 140)
            }
            HStack {
                if let log = f.step_log ?? f.log { Button(T.openLog) { NSWorkspace.shared.open(URL(fileURLWithPath: log)) } }
                Button(T.copyError) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString([f.message ?? "", f.detail ?? ""].joined(separator: "\n"), forType: .string)
                }
            }
        }
    }

    private var logView: some View {
        ScrollViewReader { reader in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(model.lines.enumerated()), id: \.offset) { i, line in
                        Text(line).font(.caption.monospaced()).textSelection(.enabled).id(i)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(height: 180)
            .onChange(of: model.lines.count) { _, n in reader.scrollTo(n - 1, anchor: .bottom) }
        }
    }
}
