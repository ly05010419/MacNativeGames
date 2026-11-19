import CryptoKit
import Foundation
import SwiftUI

/// Each game's official key art, the picture its case shows (user 2026-11-19: every cover the way Ocarina of Time,
/// Donkey Kong Bananza and Pokopia already got theirs, Nintendo's own image server; uncropped, so the case's insert is
/// cut from it with room to spare). Resources/covers.json holds each picture's address and SHA-256 only. A Launcher
/// built on this Mac carries the pictures (make_app.py, Resources/Covers/<game>.jpg); the released one carries none and
/// fetches each on the player's Mac the first time it is shown, into its own caches folder, checked against the
/// SHA-256. Until it is there, offline, or if it does not match, the case shows the game app's icon as before.
@MainActor final class OfficialCovers: ObservableObject {
    static let shared = OfficialCovers()
    struct Shot: Decodable { let url: String; let sha256: String }
    /// `center`: where across the picture the case's front is cut, 0 left ... 1 right (0.5 when not given; Tears of
    /// the Kingdom's and Breath of the Wild's wide key art round Link, user 2026-11-19).
    struct Pin: Decodable { let url: String; let sha256: String; let center: Double? }
    private struct Spec: Decodable { let covers: [String: Pin] }

    private let pins: [String: Pin]
    @Published private(set) var fetched: [String: String] = [:]
    private var asked: Set<String> = []

    private init() {
        let spec = Bundle.main.url(forResource: "covers", withExtension: "json")
            .flatMap { try? Data(contentsOf: $0) }
            .flatMap { try? JSONDecoder().decode(Spec.self, from: $0) }
        pins = spec?.covers ?? [:]
    }

    /// The picture of a game: the app's own, else this Mac's copy, fetched the first time it is asked for; nil until
    /// then, or when it cannot be had.
    func path(for game: String) -> String? {
        guard let pin = pins[game] else { return nil }
        return file(game, Shot(url: pin.url, sha256: pin.sha256))
    }

    /// Where across the game's official picture the front is cut (Pin.center).
    func center(for game: String) -> CGFloat { CGFloat(pins[game]?.center ?? 0.5) }

    /// Covers/<name>.jpg in the app, else this Mac's copy, fetched once a run when missing.
    private func file(_ name: String, _ shot: Shot) -> String? {
        if let own = Bundle.main.resourceURL?.appendingPathComponent("Covers/\(name).jpg"),
           FileManager.default.fileExists(atPath: own.path) {
            return own.path
        }
        let copy = Self.folder.appendingPathComponent("\(name)-\(shot.sha256.prefix(12)).jpg")
        if FileManager.default.fileExists(atPath: copy.path) { return copy.path }
        if let known = fetched[name] { return known }
        guard !asked.contains(name), let source = URL(string: shot.url) else { return nil }
        asked.insert(name)   // one try a run: offline, the icon stays till the next start
        Task.detached(priority: .utility) {
            guard let (data, response) = try? await URLSession.shared.data(from: source),
                  (response as? HTTPURLResponse)?.statusCode == 200,
                  SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == shot.sha256 else { return }
            try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            guard (try? data.write(to: copy, options: .atomic)) != nil else { return }
            await MainActor.run { self.fetched[name] = copy.path }
        }
        return nil
    }

    /// <caches>/<the Launcher's bundle ID>/covers: macOS may clear it; the pictures are fetched again then.
    nonisolated static var folder: URL {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        return caches.appendingPathComponent(Bundle.main.bundleIdentifier ?? "org.macnativegames.launcher")
            .appendingPathComponent("covers")
    }
}
