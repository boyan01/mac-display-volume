import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

@main
struct DisplayVolumeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var store = AudioSettingsStore()

    var body: some Scene {
        WindowGroup("Mac Display Volume", id: "main") {
            ContentView()
                .environment(store)
                .frame(minWidth: 700, minHeight: 560)
        }
        .defaultSize(width: 820, height: 640)
        .windowResizability(.contentMinSize)
    }
}
