import Foundation

/// What `classify` prints (tools/builder_jobs/classify.py report()).
struct ClassifyReport: Codable {
    struct Item: Codable, Identifiable, Hashable {
        var id: String { path }
        let path: String
        let role: String
        let game: String?
        let title: String?
        let version: Int?
        let note: String
    }
    struct Plan: Codable {
        var game: String?
        var app_name: String?
        var base: String?
        var update: String?
        var update_version: Int?
        var dlc: [String]
        var keys: String?
        var firmware: String?
        var fonts: String?
        var games_found: [String]
        var needs_keys: Bool
        var keys_problems: [String]
        var blocks: [String]
        var notes: [String]
    }
    let items: [Item]
    let plan: Plan
}

/// What `checks` prints (tools/builder_jobs/checks.py).
struct ChecksReport: Codable {
    struct Check: Codable, Identifiable {
        let id: String
        let ok: Bool
        let text: String
        let fix: String?
        let action: String?
    }
    let checks: [Check]
}

/// What `apps` prints (tools/builder_jobs/installed.py): the game apps on this Mac and the update each needs.
struct InstalledReport: Codable {
    struct App: Codable, Identifiable, Equatable {
        var id: String { app }
        let app: String
        let name: String
        let game: String?
        let version: String?
        let kind: String        // express, fast, full or error (update_runtime.py --plan)
        let reason: String
        let work: String?
        let commit: String?
    }
    let apps: [App]
}

/// The request `build` reads (tools/builder_jobs/build_job.py command()): dropped files (plan, into app when that game
/// is installed), or an installed app to update (update) without files.
struct BuildRequest: Codable {
    var plan: ClassifyReport.Plan? = nil
    var output_dir: String? = nil
    var work_root: String? = nil
    var platform: String? = nil
    var app: String? = nil
    var update: String? = nil
    var work: String? = nil       // with update: a work folder moved on the Storage page, which the app records from then on
    var shaders: Bool? = nil      // with update: only its shaders and pipelines, from the game's newest list (shader_update.py)
}

/// One line `build` prints (tools/builder_jobs/events.py); fields a kind does not have are nil.
struct BuildEvent: Decodable {
    let event: String
    let t: Double?
    // plan
    let game: String?, app: String?, work: String?, log: String?, measured: Bool?, kind: String?
    // stage / step
    let index: Int?, total: Int?, name: String?, text: String?
    // progress
    let fraction: Double?, left: Double?, elapsed: Double?, step_done: Int?, step_total: Int?
    // done
    let ok: Bool?, cancelled: Bool?, message: String?, detail: String?, step_log: String?, note: String?
}
