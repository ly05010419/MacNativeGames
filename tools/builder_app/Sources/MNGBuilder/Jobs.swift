import Foundation

/// Where the repository and its Python are, and how a job of tools/builder_jobs is started. The window holds no rule:
/// it runs `python -m tools.builder_jobs <job>` in the repository and shows what it prints.
enum Paths {
    /// A Release carries an immutable source archive and a small installer beside its Python.
    /// Run that installer for each job: it reuses the archive's hash directory after the first extraction.
    static var bundledJob: URL? {
        guard ProcessInfo.processInfo.environment["MNG_REPO"] == nil,
              Bundle.main.object(forInfoDictionaryKey: "MNGSourceArchiveSHA256") is String else { return nil }
        return Bundle.main.resourceURL?.appendingPathComponent("bundled_job.py")
    }

    /// The checkout this app was made from (make_app.py writes MNGRepo into Info.plist).
    static var repo: URL? {
        if let path = ProcessInfo.processInfo.environment["MNG_REPO"] { return URL(fileURLWithPath: path) }
        guard let path = Bundle.main.object(forInfoDictionaryKey: "MNGRepo") as? String else { return nil }
        return URL(fileURLWithPath: path)
    }

    /// The Python the app carries (Contents/Resources/python), else the one make_app.py recorded, else Homebrew's.
    static var python: URL {
        let fm = FileManager.default
        if let res = Bundle.main.resourceURL {
            let own = res.appendingPathComponent("python/bin/python3.14")
            if fm.isExecutableFile(atPath: own.path) { return own }
        }
        if let rec = Bundle.main.object(forInfoDictionaryKey: "MNGPython") as? String, fm.isExecutableFile(atPath: rec) {
            return URL(fileURLWithPath: rec)
        }
        return URL(fileURLWithPath: "/opt/homebrew/bin/python3.14")
    }

    static let support = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/MacNativeGames/Builder")

    static func configure(_ process: Process, arguments: [String]) throws {
        process.executableURL = python
        if let script = bundledJob {
            process.arguments = ["-u", script.path] + arguments
            process.currentDirectoryURL = script.deletingLastPathComponent()
        } else {
            guard let repo else { throw JobError.noRepo }
            process.arguments = ["-u", "-m", "tools.builder_jobs"] + arguments
            process.currentDirectoryURL = repo
        }
        var env = ProcessInfo.processInfo.environment
        env["PYTHONPATH"] = process.currentDirectoryURL?.path
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["MNG_LANG"] = T.zh ? "zh" : "en"
        env["MNG_JOB_SOURCE"] = Job.source
        process.environment = env
    }
}

/// One running job: its output lines come back on the main thread, one call per line, then `ended` with the exit code.
final class Job {
    static let source = "MNG Launcher"
    private let process = Process()
    private var buffer = Data()

    init(_ arguments: [String], onLine: @escaping (String) -> Void, ended: @escaping (Int32) -> Void) throws {
        try Paths.configure(process, arguments: arguments)
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let self else { return }
            if data.isEmpty { handle.readabilityHandler = nil; return }
            self.buffer.append(data)
            while let nl = self.buffer.firstIndex(of: 0x0A) {
                let line = String(decoding: self.buffer[self.buffer.startIndex..<nl], as: UTF8.self)
                self.buffer.removeSubrange(self.buffer.startIndex...nl)
                DispatchQueue.main.async { onLine(line) }
            }
        }
        process.terminationHandler = { p in
            // the last lines are read by the handler above before the pipe closes; give it that moment
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { ended(p.terminationStatus) }
        }
        try process.run()
    }

    /// The job forwards it to the build's process group (SIGINT, then SIGKILL 10 s later).
    func cancel() { if process.isRunning { process.interrupt() } }

    /// Runs a job to its end and returns everything it printed (for the one-object jobs: classify, checks).
    static func collect(_ arguments: [String]) async throws -> Data {
        let p = Process()
        try Paths.configure(p, arguments: arguments)
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        try p.run()
        return try await withCheckedThrowingContinuation { cont in
            DispatchQueue.global().async {
                let data = out.fileHandleForReading.readDataToEndOfFile()
                let problem = err.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                if p.terminationStatus == 0 { cont.resume(returning: data) }
                else { cont.resume(throwing: JobError.failed(String(decoding: problem, as: UTF8.self))) }
            }
        }
    }
}

enum JobError: LocalizedError {
    case noRepo, failed(String)
    var errorDescription: String? {
        switch self {
        case .noRepo: return T.noRepo
        case .failed(let text): return text.split(separator: "\n").suffix(8).joined(separator: "\n")
        }
    }
}
