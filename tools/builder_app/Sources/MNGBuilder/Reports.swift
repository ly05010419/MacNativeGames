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

/// The request `build` reads (tools/builder_jobs/build_job.py command()).
struct BuildRequest: Codable {
    let plan: ClassifyReport.Plan
    let output_dir: String
    let work_root: String
    let platform: String
}

/// One line `build` prints (tools/builder_jobs/events.py); fields a kind does not have are nil.
struct BuildEvent: Decodable {
    let event: String
    let t: Double?
    // plan
    let game: String?, app: String?, work: String?, log: String?, measured: Bool?
    // stage / step
    let index: Int?, total: Int?, name: String?, text: String?
    // progress
    let fraction: Double?, left: Double?, elapsed: Double?, step_done: Int?, step_total: Int?
    // done
    let ok: Bool?, cancelled: Bool?, message: String?, detail: String?, step_log: String?
}
