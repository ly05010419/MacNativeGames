import SwiftUI
import UIKit
import XCTest
@testable import DuoHarness

final class DuoHostingOrientationTests: XCTestCase {
    @MainActor
    func testProductionHostingControllerSupportsAllOrientationsInCurrentWindow() async {
        guard let window = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .flatMap(\.windows)
            .first(where: \.isKeyWindow),
              let presenter = window.rootViewController else {
            XCTFail("Expected the harness root in its phone window scene")
            return
        }
        XCTAssertNil(presenter.presentedViewController, "The harness root should be available for a temporary full-screen host")

        let host = DmHostingController(rootView: Color.clear)
        let unattachedMask = host.supportedInterfaceOrientations
        let unattachedIdiom = host.traitCollection.userInterfaceIdiom
        host.modalPresentationStyle = .fullScreen
        let presented = expectation(description: "full-screen SwiftUI host presented")
        presenter.present(host, animated: false) { presented.fulfill() }
        await fulfillment(of: [presented], timeout: 5)
        let attachedMask = host.supportedInterfaceOrientations
        let attachedIdiom = host.traitCollection.userInterfaceIdiom
        let orientation = window.windowScene?.effectiveGeometry.interfaceOrientation.rawValue ?? -1

        let dismissed = expectation(description: "full-screen SwiftUI host dismissed")
        host.dismiss(animated: false) { dismissed.fulfill() }
        await fulfillment(of: [dismissed], timeout: 5)

        print("[DUO_HOST_ORIENTATION] phoneWindowIdiom=\(window.traitCollection.userInterfaceIdiom.rawValue) sceneOrientation=\(orientation) unattachedIdiom=\(unattachedIdiom.rawValue) unattachedMask=0x\(String(unattachedMask.rawValue, radix: 16)) attachedIdiom=\(attachedIdiom.rawValue) attachedMask=0x\(String(attachedMask.rawValue, radix: 16)) all=0x\(String(UIInterfaceOrientationMask.all.rawValue, radix: 16))")
        XCTAssertNil(presenter.presentedViewController, "Restore the harness root after the probe")
        XCTAssertEqual(attachedIdiom, window.traitCollection.userInterfaceIdiom)
        XCTAssertEqual(unattachedMask, .all)
        XCTAssertEqual(attachedMask, .all, "The production full-screen SwiftUI host must preserve the app's four-orientation policy")
    }
}
