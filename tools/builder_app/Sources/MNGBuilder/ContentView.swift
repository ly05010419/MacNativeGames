import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// Three numbered groups from top to bottom: 1 the game, 2 the keys, 3 this Mac and the build. A step whose earlier
/// step is not done is dimmed; a done step shows a check mark.
struct ContentView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                StepBox(number: 1, title: T.step1, done: model.gameReady, enabled: true) { GameStep() }
                StepBox(number: 2, title: T.step2, done: model.gameReady && model.keysReady, enabled: model.gameReady) { KeysStep() }
                StepBox(number: 3, title: T.step3, done: model.phase == .done,
                        enabled: model.gameReady && model.keysReady) { BuildStep() }
            }
            .padding(20)
        }
    }
}

struct StepBox<Content: View>: View {
    let number: Int
    let title: String
    let done: Bool
    let enabled: Bool
    @ViewBuilder let content: Content

    var body: some View {
        GroupBox {
            content
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 4)
                .disabled(!enabled)
        } label: {
            HStack(spacing: 8) {
                ZStack {
                    Circle().fill(done ? Color.green : (enabled ? Color.accentColor : Color.secondary.opacity(0.5)))
                        .frame(width: 22, height: 22)
                    if done { Image(systemName: "checkmark").font(.caption.bold()).foregroundStyle(.white) }
                    else { Text("\(number)").font(.caption.bold()).foregroundStyle(.white) }
                }
                Text(T.stepTitle(number)).font(.headline).foregroundStyle(.secondary)
                Text(title).font(.headline)
            }
        }
        .opacity(enabled ? 1 : 0.55)
    }
}

/// A dashed area that takes dropped files, with a button that opens a panel for the same.
struct DropZone: View {
    let hint: String
    let button: String
    let directories: Bool
    let busy: Bool
    let take: ([URL]) -> Void
    @State private var over = false

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "square.and.arrow.down.on.square").font(.system(size: 26)).foregroundStyle(.secondary)
            Text(hint).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Button(button, action: choose).disabled(busy)
        }
        .frame(maxWidth: .infinity)
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 10).fill(over ? Color.accentColor.opacity(0.12) : Color.secondary.opacity(0.06)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(over ? Color.accentColor : Color.secondary.opacity(0.4),
                                                                  style: StrokeStyle(lineWidth: 1.5, dash: [6, 4])))
        .onDrop(of: [.fileURL], isTargeted: $over) { providers in
            guard !busy else { return false }
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    if let url { DispatchQueue.main.async { take([url]) } }
                }
            }
            return true
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = directories
        panel.canChooseDirectories = directories
        panel.canChooseFiles = true
        if panel.runModal() == .OK { take(panel.urls) }
    }
}

struct GameStep: View {
    @EnvironmentObject var model: Model
    private var busy: Bool { model.phase == .building || model.phase == .cancelling }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            DropZone(hint: T.step1Hint, button: T.chooseGame, directories: true, busy: busy) { model.add($0) }
            if let error = model.classifyError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
            let items = (model.report?.items ?? []).filter { $0.role != "keys" }
            if !items.isEmpty {
                HStack {
                    if let name = model.plan?.app_name { Text(name).font(.title3.weight(.semibold)) }
                    Spacer()
                    Button(T.clear) { model.clear() }.disabled(busy)
                }
                ForEach(items) { FileRow(item: $0) }
            }
            ForEach(model.plan?.notes ?? [], id: \.self) { Text($0).font(.caption).foregroundStyle(.secondary) }
            ForEach((model.plan?.blocks ?? []).filter { !(model.plan?.keys_problems ?? []).contains($0) }, id: \.self) {
                Label($0, systemImage: "xmark.octagon").foregroundStyle(.red)
            }
        }
    }
}

struct KeysStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let plan = model.plan, !plan.needs_keys {
                Label(T.noKeysNeeded, systemImage: "checkmark.circle").foregroundStyle(.secondary)
            } else {
                Text(T.step2Hint).font(.callout).foregroundStyle(.secondary)
                HStack {
                    Image(systemName: "key.fill").foregroundStyle(model.keysReady ? Color.green : Color.secondary)
                    Text(model.plan?.keys.map { ($0 as NSString).lastPathComponent } ?? "—")
                    if let keys = model.plan?.keys {
                        Text((keys as NSString).deletingLastPathComponent).font(.caption).foregroundStyle(.secondary)
                            .lineLimit(1).truncationMode(.middle)
                    }
                    Spacer()
                    Button(T.chooseKeys) {
                        let panel = NSOpenPanel()
                        panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
                        if panel.runModal() == .OK { model.setKeys(panel.url) }
                    }
                }
                .onDrop(of: [.fileURL], isTargeted: nil) { providers in
                    _ = providers.first?.loadObject(ofClass: URL.self) { url, _ in
                        if let url { DispatchQueue.main.async { model.setKeys(url) } }
                    }
                    return true
                }
                if model.keysReady { Label(T.keysOK, systemImage: "checkmark.circle.fill").foregroundStyle(.green) }
                ForEach(model.plan?.keys_problems ?? [], id: \.self) { Label($0, systemImage: "xmark.octagon").foregroundStyle(.red) }
            }
        }
    }
}

struct FileRow: View {
    @EnvironmentObject var model: Model
    let item: ClassifyReport.Item

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Image(systemName: item.role == "ignored" ? "minus.circle" : (used ? "checkmark.circle.fill" : "circle"))
                .foregroundStyle(item.role == "ignored" ? Color.secondary : Color.accentColor)
            Text(T.role(item.role)).frame(width: 96, alignment: .leading).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text((item.path as NSString).lastPathComponent).lineLimit(1).truncationMode(.middle).strikethrough(!used)
                if !item.note.isEmpty { Text(item.note).font(.caption).foregroundStyle(.secondary) }
            }
        }
    }

    private var used: Bool {
        guard let p = model.plan else { return false }
        return [p.base, p.update, p.keys, p.firmware, p.fonts].contains(item.path) || p.dlc.contains(item.path)
    }
}

struct BuildStep: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(T.step3Hint).font(.callout).foregroundStyle(.secondary)
            ChecksList()
            FoldersSection()
            ActionSection()
        }
    }
}

struct ChecksList: View {
    @EnvironmentObject var model: Model

    var body: some View {
        let failing = model.checks.filter { !$0.ok }
        if failing.isEmpty {
            if !model.checks.isEmpty { Label(T.macOK, systemImage: "desktopcomputer").foregroundStyle(.secondary) }
        } else {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(failing) { c in
                    HStack(alignment: .firstTextBaseline) {
                        Image(systemName: c.id == "disk" ? "exclamationmark.triangle" : "xmark.octagon")
                            .foregroundStyle(c.id == "disk" ? .orange : .red)
                        VStack(alignment: .leading) {
                            Text(c.text)
                            if let fix = c.fix { Text(fix).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled) }
                        }
                        Spacer()
                        if c.fix != nil { Button(T.fix) { model.fix(c) } }
                    }
                }
                Button(T.recheck) { Task { await model.runChecks() } }
            }
        }
    }
}

struct FoldersSection: View {
    @EnvironmentObject var model: Model

    var body: some View {
        DisclosureGroup(T.settings) {
            VStack(alignment: .leading, spacing: 6) {
                row(T.output, $model.outputDir)
                row(T.work, $model.workRoot)
            }
            .padding(.top, 4)
        }
        .disabled(model.phase == .building)
    }

    private func row(_ label: String, _ value: Binding<String>) -> some View {
        HStack {
            Text(label).frame(width: 110, alignment: .leading).foregroundStyle(.secondary)
            Text(value.wrappedValue).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
            Spacer()
            Button(T.change) {
                let panel = NSOpenPanel()
                panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
                if panel.runModal() == .OK, let url = panel.url { value.wrappedValue = url.path }
            }
        }
    }
}
