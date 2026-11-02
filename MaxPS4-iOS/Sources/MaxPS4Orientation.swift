import UIKit

// The orientations iOS may use: landscape only while a game is on screen.
@MainActor
final class MaxPS4AppDelegate: NSObject, UIApplicationDelegate {
    static var orientations: UIInterfaceOrientationMask = .all
    func application(_ application: UIApplication,
                     supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        Self.orientations
    }
}

enum MaxPS4Orientation {
    // A game screen turns the iPhone to landscape and keeps it there.
    @MainActor static func landscape() { apply(allowed: .landscape, turnTo: .landscape) }
    // Leaving it: back to portrait for the library, then any orientation.
    @MainActor static func release() { apply(allowed: .all, turnTo: .portrait) }

    @MainActor private static func apply(allowed: UIInterfaceOrientationMask, turnTo target: UIInterfaceOrientationMask) {
        MaxPS4AppDelegate.orientations = allowed
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows {
                var controller = window.rootViewController
                while let current = controller {
                    current.setNeedsUpdateOfSupportedInterfaceOrientations()
                    controller = current.presentedViewController
                }
            }
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: target)) { _ in }
        }
    }
}
