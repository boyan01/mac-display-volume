import SwiftUI

@main
struct DisplayVolumeApp: App {
    @State private var store = AudioSettingsStore()

    var body: some Scene {
        MenuBarExtra("Mac Display Volume", systemImage: "slider.horizontal.3") {
            MenuBarContentView()
                .environment(store)
        }
        .menuBarExtraStyle(.menu)

        Window("Mac Display Volume", id: "main") {
            ContentView()
                .environment(store)
                .frame(minWidth: 640, minHeight: 520)
        }
        .windowResizability(.contentMinSize)
    }
}
