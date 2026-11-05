import SwiftUI
import UIKit

// Full-screen SwiftUI presentations must preserve the game's four-orientation policy.
final class DmHostingController<Content: View>: UIHostingController<Content> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .all }
}
