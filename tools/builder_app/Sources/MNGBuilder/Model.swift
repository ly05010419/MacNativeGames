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
    @Published var installedLoaded = false   // the first scan finished: until then no game is shown as missing
    @Published var installedError: String?   // why the last scan failed (the home page shows it)
    @Published var updating: String?     // the installed app being updated (its progress shows in its list)
    @Published var kind = "full"         // express, fast or full: what the running job does
    @Published var page = "home"         // the page shown: home (the games) or build
    @Published var buildFor: String?     // the game a card's plus chose for the Build page (its files go to step 1)
    @Published var outputDir: String { didSet { save() } }
    @Published var workRoot: String { didSet { save(); Task { await runChecks() } } }

    private var job: Job?
    // A build or update started elsewhere (tools/mng.py in a terminal, an AI): followed through tools/builder_jobs/shared.py
    @Published var externalSource: String?
    @Published var startedHere = true   // false for a job another program started: its end starts no game
    private var externalPid: pid_t = 0
    private var eventsPath: String?
    private var offset: UInt64 = 0
    private var pending = Data()
    private var watcher: Timer?
    private var follower: Timer?
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
        watcher = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.lookForExternal() }
        }
        loadRememberedInstalled()   // the games of the last look at once; the look below brings them up to date
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
        page = "build"   // files given to the app (open -a, the Dock): the steps they fill in
        save()
        Task { await classify() }
    }

    /// The pages besides building each game has (tools/builder/games.py PAGES, which the backend enforces;
    /// tests/test_launcher_pages.py checks the two agree). Ocarina of Time: Controls, Storage and Saves (user 2026-11-13).
    static let gamePages: [String: [String]] = [
        "totk": ["settings", "mods", "controls", "storage", "saves"],
        "botw": ["settings", "mods", "controls", "storage", "saves"],
        "oot": ["controls", "storage", "saves"],
    ]
    static func pages(_ game: String?) -> [String] { game.flatMap { gamePages[$0] } ?? [] }
    /// The installed apps the pages besides building serve.
    var pageApps: [InstalledReport.App] { installed.filter { !Model.pages($0.game).isEmpty } }

    /// Where the last look at the installed games is kept, so the games page shows them at once (user 2026-11-12).
    private static let rememberedInstalled = Paths.support.appendingPathComponent("installed.json")

    /// The games of the last look that are still there; nothing when there was none (the page then waits for the look).
    private func loadRememberedInstalled() {
        guard let data = try? Data(contentsOf: Self.rememberedInstalled),
              let report = try? JSONDecoder().decode(InstalledReport.self, from: data) else { return }
        installed = report.apps.filter { FileManager.default.fileExists(atPath: $0.app) }
        installedLoaded = true
    }

    func loadInstalled() async {
        do {
            let data = try await Job.collect(["apps", "--apps-dir", outputDir])
            installed = try JSONDecoder().decode(InstalledReport.self, from: data).apps
            installedError = nil
            try? FileManager.default.createDirectory(at: Paths.support, withIntermediateDirectories: true)
            try? data.write(to: Self.rememberedInstalled, options: .atomic)
        } catch {
            installedError = error.localizedDescription   // the games of the last look stay; the page says why this failed
        }
        installedLoaded = true
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

    /// Only the shaders and pipelines the game's newest shared list adds (shader_update.py; user 2026-11-13).
    func updateShaders(_ app: InstalledReport.App) {
        guard phase != .building && phase != .cancelling else { return }
        updating = app.app
        start(BuildRequest(update: app.app, shaders: true))
    }
    /// The games whose shaders the Check shaders button compares with their newest list (shader_update.py LISTS).
    static let shaderGames: Set<String> = ["totk", "botw", "oot"]

    /// work: a folder the Storage page moved the work folder to; the update records it in the app.
    func update(path: String, work: String? = nil) {
        guard phase != .building && phase != .cancelling else { return }
        updating = path
        start(BuildRequest(update: path, work: work))
    }

    private func start(_ request: BuildRequest) {
        startedHere = true
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

    func cancel() {
        if externalSource != nil, externalPid > 0 { kill(externalPid, SIGINT) } else { job?.cancel() }
        phase = .cancelling
    }

    /// The Mac's one job, when another program started it: its record (Builder/jobs/current.json) names its pid and its
    /// events file, which is read from the start and followed, so the window shows it as its own and can stop it.
    func lookForExternal() {
        guard job == nil, externalSource == nil, phase != .building, phase != .cancelling else { return }
        let jobs = Paths.support.appendingPathComponent("jobs")
        guard let data = try? Data(contentsOf: jobs.appendingPathComponent("current.json")),
              let r = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let pid = r["pid"] as? Int, kill(pid_t(pid), 0) == 0,
              (r["source"] as? String) != Job.source else { return }
        externalPid = pid_t(pid); externalSource = r["source"] as? String ?? "?"; startedHere = false
        eventsPath = r["events"] as? String; offset = 0; pending = Data()
        updating = r["app"] as? String ?? externalSource
        phase = .building; stage = ""; stageText = ""; step = ""; fraction = 0; left = nil; elapsed = 0; lines = []
        failure = nil; note = ""
        follower = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.readExternal() }
        }
    }

    private func readExternal() {
        guard let path = eventsPath else { return endExternal() }
        // the file becomes last.ndjson when the job ends: read what is left from there
        let last = (path as NSString).deletingLastPathComponent + "/last.ndjson"
        if let h = FileHandle(forReadingAtPath: path) ?? FileHandle(forReadingAtPath: last) {
            try? h.seek(toOffset: offset)
            let data = h.readDataToEndOfFile(); try? h.close()
            offset += UInt64(data.count); pending.append(data)
            while let nl = pending.firstIndex(of: 0x0A) {
                let line = String(decoding: pending[pending.startIndex..<nl], as: UTF8.self)
                pending.removeSubrange(pending.startIndex...nl)
                handle(line)
            }
        }
        if phase != .building && phase != .cancelling { return endExternal() }
        if kill(externalPid, 0) != 0 && !FileManager.default.fileExists(atPath: path) {
            phase = .failed; endExternal()   // gone without its done line
        }
    }

    private func endExternal() {
        follower?.invalidate(); follower = nil
        externalSource = nil; externalPid = 0; eventsPath = nil
        Task { await loadInstalled() }
    }

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
                phase = .done; fraction = 1; appPath = e.app ?? appPath
                if startedHere { play() }   // a job started elsewhere ends without starting the game
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
