import SwiftUI

@main
struct MaxPS4App: App {
    @UIApplicationDelegateAdaptor(MaxPS4AppDelegate.self) private var appDelegate
    @StateObject private var emulator = MaxPS4Emulator()
    @AppStorage(MaxPS4Language.key) private var language = ""

    init() {
        // Once: TSO and native textures became on by default (Sonic Mania
        // stutters without TSO); values stored before, app-wide or per game,
        // are forgotten so every game starts from the new defaults.
        let defaults = UserDefaults.standard
        let migrated = "maxps4.options.defaults2"
        if !defaults.bool(forKey: migrated) {
            for key in defaults.dictionaryRepresentation().keys
            where key.hasPrefix("maxps4.fex.tso") || key.hasPrefix("maxps4.metal.nativeTextures") {
                defaults.removeObject(forKey: key)
            }
            defaults.set(true, forKey: migrated)
        }
    }

    var body: some Scene {
        WindowGroup {
            MaxPS4HomeView()
                .environmentObject(emulator)
                .environment(\.locale, MaxPS4Language.locale(for: language))
        }
    }
}
