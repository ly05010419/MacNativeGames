// swift-tools-version:6.0
// The drag-and-drop builder window (docs/builder-app.md). make_app.py builds it into "MNG Launcher.app".
import PackageDescription

let package = Package(
    name: "MNGBuilder",
    platforms: [.macOS(.v15)],
    targets: [
        .executableTarget(name: "MNGBuilder", path: "Sources/MNGBuilder"),
    ],
    swiftLanguageModes: [.v5]
)
