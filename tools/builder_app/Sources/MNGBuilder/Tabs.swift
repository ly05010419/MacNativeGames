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

/// The installed apps the pages serve (Tears of the Kingdom, Breath of the Wild), and the one chosen.
struct GamePicker: View {
    @EnvironmentObject var model: Model

    var body: some View {
        let apps = model.pageApps
        if apps.isEmpty {
            Text(T.noPageGame).foregroundStyle(.secondary)
        } else {
            Picker(T.game, selection: Binding(get: { model.pageApp ?? apps[0].app }, set: { model.pageApp = $0 })) {
                ForEach(apps) { a in Text("\(a.name) \(a.version ?? "")").tag(a.app) }
            }
            .frame(maxWidth: 360)
        }
    }
}

/// A page's frame: the game picker, then its content for the chosen app (or why there is none).
struct PageFrame<Content: View>: View {
    @EnvironmentObject var model: Model
    @ViewBuilder let content: (String) -> Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                GamePicker()
                if let app = model.currentPageApp { content(app).id(app) }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
