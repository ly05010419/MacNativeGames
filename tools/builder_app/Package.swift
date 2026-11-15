// swift-tools-version:6.0
// The drag-and-drop builder window (docs/builder-app.md). make_app.py builds it into "MNG Launcher.app".
import PackageDescription

let package = Package(
    name: "MNGBuilder",
    platforms: [.macOS(.v15)],
    dependencies: [
        // The terminal page (TerminalTab.swift; user 2026-11-13: an AI helper in the Launcher). MIT; an exact release at
        // least two weeks old (v1.20.0, 2026-08-18) that the Command Line Tools' Swift 6.2 compiles: v1.99.2 needs
        // Swift 6.3 (PowerlineRenderer.swift, 2026-11-15), and a player builds this with the Command Line Tools.
        // Package.resolved pins it and what it brings.
        .package(url: "https://github.com/migueldeicaza/SwiftTerm", exact: "1.20.0"),
    ],
    targets: [
        .executableTarget(name: "MNGBuilder", dependencies: [.product(name: "SwiftTerm", package: "SwiftTerm")],
                          path: "Sources/MNGBuilder"),
    ],
    swiftLanguageModes: [.v5]
)
