import AppKit
import Foundation

/// The window's state: the dropped files and what the job layer made of them, this Mac's checks, and one build at a
/// time. Paths are remembered (UserDefaults), never the files' contents.
@MainActor
final class Model: ObservableObject {
    enum Phase: Equatable { case idle, building, cancelling, done, failed, cancelled }

    @Published var paths: [String] = []
    @Published var keysPath: String?
    @Published var report: ClassifyReport?
    @Published var classifyError: String?
    @Published var checks: [ChecksReport.Check] = []
    @Published var phase: Phase = .idle
    @Published var stage = ""
    @Published var stageText = ""
    @Published var step = ""
    @Published var stepCount: (Int, Int)?
    @Published var fraction = 0.0
    @Published var left: Double?
    @Published var elapsed = 0.0
    @Published var measured = false
    @Published var lines: [String] = []
    @Published var failure: BuildEvent?
    @Published var appPath: String?
    @Published var note = ""            // the Mods page's line after a build (a code patch, mods to apply again)
    @Published var installed: [InstalledReport.App] = []
    @Published var updating: String?     // the installed app being updated (its progress shows in its list)
    @Published var kind = "full"         // express, fast or full: what the running job does
    @Published var pageApp: String?      // the app the pages besides building show (Settings, Mods, …)
    @Published var outputDir: String { didSet { save() } }
    @Published var workRoot: String { didSet { save(); Task { await runChecks() } } }

    private var job: Job?
    private let defaults = UserDefaults.standard

    init() {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        outputDir = defaults.string(forKey: "outputDir") ?? home + "/Applications"
        // Empty: the one folder rule (build_app.default_work, MacNativeGames-work beside the apps folder). The window's
        // first default (Application Support/…/Builder/work) counts as empty, so its apps keep the folder they record.
        let saved = defaults.string(forKey: "workRoot") ?? ""
        workRoot = saved == Paths.support.appendingPathComponent("work").path ? "" : saved
        paths = defaults.stringArray(forKey: "paths") ?? []
        keysPath = defaults.string(forKey: "keys")
        NotificationCenter.default.addObserver(forName: .mngOpenFiles, object: nil, queue: .main) { [weak self] note in
            let urls = note.object as? [URL] ?? []
            Task { @MainActor in self?.open(urls) }
        }
        let pending = Delegate.pending; Delegate.pending = []
        Task { await runChecks(); await loadInstalled(); open(pending); if !paths.isEmpty { await classify() } }
    }

    /// Files given to the app (open -a "MNG Launcher" <files>, or dropped on its icon): keys go to step 2, the rest to
    /// step 1, so a player's AI can start the Launcher with everything filled in.
    func open(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        let keys = urls.filter { $0.pathExtension.lowercased() == "keys" }
        if let k = keys.last { keysPath = k.path }
        let rest = urls.filter { $0.pathExtension.lowercased() != "keys" }
        if !rest.isEmpty { paths = rest.map { $0.path } }
        save()
        Task { await classify() }
    }

    /// The installed apps the pages besides building serve (Tears of the Kingdom, Breath of the Wild: tools/builder's games).
    var pageApps: [InstalledReport.App] { installed.filter { $0.game == "totk" || $0.game == "botw" } }
    var currentPageApp: String? {
        if let a = pageApp, pageApps.contains(where: { $0.app == a }) { return a }
        return pageApps.first?.app
    }

    func loadInstalled() async {
        do {
            let data = try await Job.collect(["apps", "--apps-dir", outputDir])
            installed = try JSONDecoder().decode(InstalledReport.self, from: data).apps
        } catch {
            installed = []
        }
    }

    private func save() {
        defaults.set(outputDir, forKey: "outputDir")
        defaults.set(workRoot, forKey: "workRoot")
        defaults.set(paths, forKey: "paths")
        defaults.set(keysPath, forKey: "keys")
    }

    var plan: ClassifyReport.Plan? { report?.plan }
    var blocks: [String] {
        (plan?.blocks ?? []) + checks.filter { !$0.ok && $0.id != "disk" }.map { $0.text }
    }
    var canBuild: Bool { phase != .building && phase != .cancelling && plan?.game != nil && blocks.isEmpty }

    /// The app a build of the current plan writes, when it is already there.
    var existingApp: String? {
        guard let name = plan?.app_name else { return nil }
        if let game = plan?.game, let app = installed.first(where: { $0.game == game })?.app { return app }
        let path = (outputDir as NSString).appendingPathComponent(name + ".app")
        return FileManager.default.fileExists(atPath: path) ? path : nil
    }

    func add(_ urls: [URL]) {
        let new = urls.map { $0.path }.filter { !paths.contains($0) }
        guard !new.isEmpty else { return }
        paths += new
        save()
        Task { await classify() }
    }

    func clear() {
        paths = []; report = nil; classifyError = nil; save()
    }

    /// Step 2: the keys are chosen on their own and kept for every game (a path, never the file).
    func setKeys(_ url: URL?) {
        keysPath = url?.path
        save()
        Task { await classify() }
    }

    /// Step 1 is done: the base game was found (a game card without a title ID in its name is told by the keys of
    /// step 2). Step 2 is done: the game needs no keys, or the keys open it.
    var gameReady: Bool { plan?.base != nil && (plan?.blocks ?? []).allSatisfy { (plan?.keys_problems ?? []).contains($0) } }
    var keysReady: Bool { plan.map { !$0.needs_keys || ($0.keys != nil && $0.keys_problems.isEmpty) } ?? false }
    var macReady: Bool { checks.allSatisfy { $0.ok || $0.id == "disk" } && !checks.isEmpty }

    func classify() async {
        guard !paths.isEmpty else { report = nil; return }
        do {
            let data = try await Job.collect(["classify"] + paths + (keysPath.map { [$0] } ?? []))
            report = try JSONDecoder().decode(ClassifyReport.self, from: data)
            classifyError = nil
        } catch {
            classifyError = error.localizedDescription
        }
    }

    func runChecks() async {
        do {
            let data = try await Job.collect(["checks", "--work", workRoot])
            checks = try JSONDecoder().decode(ChecksReport.self, from: data).checks
        } catch {
            checks = [ChecksReport.Check(id: "job", ok: false, text: error.localizedDescription, fix: nil, action: nil)]
        }
    }

    /// The fix of a check that a click can start: the developer tools' installer. Others show their command to copy.
    func fix(_ check: ChecksReport.Check) {
        switch check.action {
        case "install-clt":
            run("/usr/bin/xcode-select", ["--install"])
        default:
            if let fix = check.fix { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(fix, forType: .string) }
        }
    }

    private func run(_ tool: String, _ args: [String]) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe; p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { h in
            let text = String(decoding: h.availableData, as: UTF8.self)
            if text.isEmpty { h.readabilityHandler = nil; return }
            DispatchQueue.main.async { self.lines.append(contentsOf: text.split(separator: "\n").map(String.init)) }
        }
        p.terminationHandler = { _ in Task { @MainActor in await self.runChecks() } }
        try? p.run()
    }

    func build() {
        guard let plan, canBuild else { return }
        // an installed app of this game is built into where it is, with the work folder it records
        let installedApp = plan.game.flatMap { g in installed.first(where: { $0.game == g })?.app }
        updating = nil
        start(BuildRequest(plan: plan, output_dir: outputDir, work_root: workRoot, platform: "macos", app: installedApp))
    }

    /// An installed app, without files: the fastest update that fits (express, else fast; the job refuses a full one).
    func update(_ app: InstalledReport.App) { update(path: app.app) }

    /// work: a folder the Storage page moved the work folder to; the update records it in the app.
    func update(path: String, work: String? = nil) {
        guard phase != .building && phase != .cancelling else { return }
        updating = path
        start(BuildRequest(update: path, work: work))
    }

    private func start(_ request: BuildRequest) {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("mng-build-request.json")
        do {
            try FileManager.default.createDirectory(atPath: outputDir, withIntermediateDirectories: true)
            try JSONEncoder().encode(request).write(to: file)
        } catch {
            failure = nil; lines = [error.localizedDescription]; phase = .failed; return
        }
        phase = .building; stage = ""; stageText = ""; step = ""; fraction = 0; left = nil; elapsed = 0; lines = []; failure = nil
        do {
            job = try Job(["build", file.path], onLine: { [weak self] in self?.handle($0) },
                          ended: { [weak self] code in self?.ended(code) })
        } catch {
            lines = [error.localizedDescription]; phase = .failed
        }
    }

    func cancel() { job?.cancel(); phase = .cancelling }

    private func handle(_ line: String) {
        guard let data = line.data(using: .utf8), let e = try? JSONDecoder().decode(BuildEvent.self, from: data) else {
            append(line); return
        }
        switch e.event {
        case "plan": measured = e.measured ?? false; appPath = e.app; kind = e.kind ?? "full"
        case "stage": stage = T.stage(e.name ?? ""); stageText = e.text ?? ""; append("— " + (e.text ?? ""))
        case "step": step = e.name ?? ""; stepCount = nil
        case "progress":
            fraction = e.fraction ?? fraction; left = e.left; elapsed = e.elapsed ?? elapsed; measured = e.measured ?? measured
            if let d = e.step_done, let t = e.step_total { stepCount = (d, t) } else { stepCount = nil }
        case "log": append(e.text ?? "")
        case "cancelling": phase = .cancelling
        case "done":
            note = e.note ?? ""
            if e.ok == true {
                phase = .done; fraction = 1; appPath = e.app ?? appPath; play()
            } else {
                failure = e; phase = (e.cancelled ?? false) ? .cancelled : .failed
            }
        default: break
        }
    }

    private func append(_ line: String) {
        lines.append(line)
        if lines.count > 400 { lines.removeFirst(lines.count - 400) }
    }

    private func ended(_ code: Int32) {
        job = nil
        if phase == .building || phase == .cancelling {   // the job ended without its 'done' line
            phase = code == 0 ? .done : .failed
        }
        Task { await loadInstalled() }   // an app was made or updated: what it needs now
    }

    func play() {
        guard let app = appPath ?? existingApp else { return }
        NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: app), configuration: .init())
    }

    func reveal(_ path: String?) {
        guard let path else { return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }
}
