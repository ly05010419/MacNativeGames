import AppKit
import SwiftUI

/// The pages besides building (the old window's Settings, Mods, Controls, Storage and Saves, moved here 2026-11-12):
/// each asks tools/builder_jobs `tab <tab> <action> --app <app>` and shows its JSON answer. The rules stay in Python.
enum TabClient {
    struct Refused: LocalizedError { let message: String; var errorDescription: String? { message } }

    /// The `result` of one call as JSON data, to decode into the page's own type; a refusal throws its words.
    static func call(_ tab: String, _ action: String, app: String, args: [String: Any] = [:]) async throws -> Data {
        let json = String(decoding: try JSONSerialization.data(withJSONObject: args), as: UTF8.self)
        let data = try await Job.collect(["tab", tab, action, "--app", app, "--args", json])
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw Refused(message: String(decoding: data, as: UTF8.self))
        }
        if object["ok"] as? Bool == true, let result = object["result"] {
            return try JSONSerialization.data(withJSONObject: result, options: .fragmentsAllowed)
        }
        throw Refused(message: object["error"] as? String ?? String(decoding: data, as: UTF8.self))
    }

    static func decode<T: Decodable>(_ type: T.Type, _ tab: String, _ action: String, app: String,
                                     args: [String: Any] = [:]) async throws -> T {
        try JSONDecoder().decode(type, from: try await call(tab, action, app: app, args: args))
    }
}
