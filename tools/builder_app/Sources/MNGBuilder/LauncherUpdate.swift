import AppKit
import SwiftUI

/// MNG Launcher replaces itself with a newer release and opens it again (user 2026-11-18: "点一下就自动下载、替换、重开"的
/// 真正更新按钮, "放在 右上角，有更新 才显示（测试时候例外）"; until then the version label only linked to the disk image).
/// The window holds no rule: launcher_update.py (in the signed app's Resources beside source_updates.py; in a checkout
/// tools/builder_app) downloads the release, takes it only when it is this Launcher's own newer release (code signature,
/// notarization) and starts the script that swaps the two once this process has quit. It prints one JSON object a line:
/// found, download, verify, install, error.
enum LauncherUpdate {
    enum Phase: Equatable { case idle, starting, downloading(Double), verifying, installing, failed(String) }

    /// Tests ("测试时候例外"): MNG_LAUNCHER_UPDATE_TEST shows the button without a newer release and lets the same version
    /// be installed again. A value that is an address ("http://127.0.0.1:8000/releases.json") is read as the release
    /// list; any other value ("1") keeps the Launcher's own list. No check of the download changes.
    static let test: String? = ProcessInfo.processInfo.environment["MNG_LAUNCHER_UPDATE_TEST"].flatMap { $0.isEmpty ? nil : $0 }
    static var testFeed: String? { test.flatMap { $0.contains("://") ? $0 : nil } }
    /// Tests: MNG_LAUNCHER_UPDATE_CLICK=1 with the above presses the button two seconds after the start.
    static let clicksItself = test != nil && ProcessInfo.processInfo.environment["MNG_LAUNCHER_UPDATE_CLICK"] == "1"

    /// The signed app's own copy; in a Launcher made from a checkout, the checkout's.
    static var script: URL? {
        let fm = FileManager.default
        let candidates = [Bundle.main.resourceURL?.appendingPathComponent("launcher_update.py"),
                          Paths.repo?.appendingPathComponent("tools/builder_app/launcher_update.py")]
        return candidates.compactMap { $0 }.first { fm.fileExists(atPath: $0.path) }
    }

    /// What the script left for the next start when it could not put the new Launcher in place.
    static let outcome = Paths.support.appendingPathComponent("launcher-update/result.json")
}

extension Model {
    /// The version the button offers, or nil for no button: a newer release (newerLauncher: a release, and one higher
    /// than this); in a test whatever the list holds (""); after a failure the button stays to be pressed again.
    var launcherUpdateOffer: String? {
        if let newer = newerLauncher { return newer.version }
        if LauncherUpdate.test != nil { return "" }
        if case .failed = launcherUpdate { return "" }
        return nil
    }

    /// A build or a game's update runs the Python inside this app: the Launcher is not replaced under it.
    var launcherUpdateBlocked: Bool { jobRunning }

    var launcherUpdateRunning: Bool {
        switch launcherUpdate {
        case .idle, .failed: return false
        default: return true
        }
    }

    func updateLauncher() {
        guard !launcherUpdateRunning, !launcherUpdateBlocked else { return }
        guard let script = LauncherUpdate.script else { launcherUpdate = .failed(T.updateNoScript); return }
        launcherUpdate = .starting
        launcherUpdateManual = newerLauncher?.url
        var arguments = ["update", "--app", Bundle.main.bundleURL.path, "--support", Paths.support.path,
                         "--pid", String(ProcessInfo.processInfo.processIdentifier)]
        if let feed = LauncherUpdate.testFeed { arguments += ["--feed", feed] }
        if LauncherUpdate.test != nil { arguments.append("--same") }
        do {
            launcherUpdateJob = try Job(script: script, arguments: arguments,
                                        onLine: { [weak self] line in self?.launcherUpdateLine(line) },
                                        ended: { [weak self] status in self?.launcherUpdateEnded(status) })
        } catch {
            launcherUpdate = .failed(error.localizedDescription)
        }
    }

    private func launcherUpdateLine(_ line: String) {
        guard let data = line.data(using: .utf8),
              let event = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let kind = event["event"] as? String else { return }   // anything else the script's Python says
        switch kind {
        case "found":
            launcherUpdate = .downloading(0)
        case "download":
            if let done = event["done"] as? Double, let total = event["total"] as? Double, total > 0 {
                launcherUpdate = .downloading(min(1, done / total))
            }
        case "verify":
            launcherUpdate = .verifying
        case "install":
            // the script outside the app waits for this process to end, puts the new app in its place and opens it
            launcherUpdate = .installing
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NSApp.terminate(nil) }
        case "error":
            if let manual = (event["manual"] as? String).flatMap({ URL(string: $0) }), manual.scheme == "https",
               Model.launcherHosts.contains(manual.host ?? "") {
                launcherUpdateManual = manual
            }
            launcherUpdate = .failed(event["message"] as? String ?? T.updateFailed)
        default:
            break
        }
    }

    private func launcherUpdateEnded(_ status: Int32) {
        launcherUpdateJob = nil
        if status != 0, launcherUpdateRunning { launcherUpdate = .failed(T.updateFailed) }   // it ended without saying why
    }

    /// The last update's outcome, read once at the start: the new Launcher says nothing (it is the proof); the old one,
    /// opened again because the swap could not be made, says so and keeps the button.
    func readLauncherUpdateOutcome() {
        let file = LauncherUpdate.outcome
        if let data = try? Data(contentsOf: file) {
            try? FileManager.default.removeItem(at: file)
            if let outcome = try? JSONSerialization.jsonObject(with: data) as? [String: Any], outcome["ok"] as? Bool == false {
                launcherUpdate = .failed(T.updateNotReplaced)
            }
        } else if LauncherUpdate.clicksItself {
            // tests: the button presses itself two seconds after the start, unless this start is the one after an update
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.updateLauncher() }
        }
    }
}

/// The button at the home page's top right, only while there is something to update to: its label is the version, then
/// what the update is doing; a failure says why below it and offers the plain download.
struct LauncherUpdateButton: View {
    @EnvironmentObject var model: Model

    var body: some View {
        if let version = model.launcherUpdateOffer {
            switch model.launcherUpdate {
            case .idle, .failed:
                Button { model.updateLauncher() } label: {
                    Label(T.updateTo(version), systemImage: "arrow.down.circle.fill")
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.launcherUpdateBlocked)
                .help(model.launcherUpdateBlocked ? T.updateBlocked : T.updateHelp)
                .accessibilityIdentifier("launcher-update")
            case .starting, .downloading, .verifying, .installing:
                HStack(spacing: 8) {
                    if case .downloading(let part) = model.launcherUpdate {
                        ProgressView(value: part).frame(width: 90)
                    } else {
                        ProgressView().controlSize(.small)
                    }
                    Text(T.updateProgress(model.launcherUpdate)).font(.callout.monospacedDigit()).foregroundStyle(.secondary)
                }
                .accessibilityIdentifier("launcher-update-progress")
            }
        }
    }
}

/// Why the update did not happen, under the page's top row, with the plain download beside it.
struct LauncherUpdateFailure: View {
    @EnvironmentObject var model: Model

    var body: some View {
        if case .failed(let message) = model.launcherUpdate {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Spacer()
                Label(message, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout).foregroundStyle(.red).multilineTextAlignment(.trailing).textSelection(.enabled)
                if let manual = model.launcherUpdateManual ?? model.newerLauncher?.url {
                    Button(T.updateManual) { NSWorkspace.shared.open(manual) }.buttonStyle(.link)
                        .accessibilityIdentifier("launcher-update-manual")
                }
            }
            .accessibilityIdentifier("launcher-update-failure")
        }
    }
}
