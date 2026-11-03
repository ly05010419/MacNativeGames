# iOS layout and input harness (Duo harness)

This XCTest app compiles the production iOS gamepad, layout and hosting-controller code with inert host-runtime stubs. Its fixture `game_profile.h` supplies no game data, so the harness does not generate or require a game profile or extracted files.

The Simulator unit tests cover portrait and landscape layout, safe areas, valid and invalid division regions, 16:9 fitting and the Fill picture, touch hit ownership (the menu button against − / +), multitouch and video coordinates, pose-change input release, the in-game menu's layout pick and its live settings (picture, console mode, rumble, frame-rate counter) on the production root view controller, the drawable's pixel cap, controller-to-player slots, stale controller callbacks, and preservation of a seeded controller-motion sample across layout changes. The optional virtual-controller case runs when Simulator exposes one. They use synthetic touches, controller snapshots, hinge statuses and division callbacks; they do not exercise UIKit's own hit-testing of real touches, the menu's presentation or system touch cancellation.

The harness uses synthetic host state and does not validate an installed game, physical hinge sensors or device motion.

From the repository root:

```sh
cd dm/ios/tests/duo
xcodegen generate --spec project.yml
xcodebuild test -project DuoHarness.xcodeproj -scheme DuoHarness \
  -destination 'platform=iOS Simulator,id=<simulator-udid>' \
  -parallel-testing-enabled NO CODE_SIGNING_ALLOWED=NO
```

To build and install the harness app for a simulator:

```sh
DERIVED_DATA="$(mktemp -d "${TMPDIR:-/tmp}/duo-harness.XXXXXX")"
xcodebuild build-for-testing -project DuoHarness.xcodeproj -scheme DuoHarness \
  -destination 'generic/platform=iOS Simulator' -derivedDataPath "$DERIVED_DATA" \
  CODE_SIGNING_ALLOWED=NO
DEVICE=<simulator-udid>
APP="$DERIVED_DATA/Build/Products/Debug-iphonesimulator/DuoHarness.app"
xcrun simctl install "$DEVICE" "$APP"
xcrun simctl launch "$DEVICE" org.nxnative.DuoHarness
```
