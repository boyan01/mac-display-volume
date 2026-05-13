import Foundation
import SwiftUI

struct MenuBarContentView: View {
    @Environment(AudioSettingsStore.self) private var store
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading) {
            Text("Mac Display Volume")
                .font(.headline)

            Divider()

            Text("Output: \(store.defaultOutputDevice?.name ?? "Unknown")")
            Text("Target: \(store.selectedTargetDevice?.name ?? "Not selected")")
            Text("Latency: \(String(format: "%.1f", store.driverStatus.queuedMilliseconds)) ms")
            Text("Target alive: \(store.driverStatus.targetAlive ? "Yes" : "No")")

            Divider()

            Button("Use Virtual Output") {
                store.setVirtualDeviceAsSystemOutput()
            }
            .disabled(store.virtualDevice == nil)

            Button("Use Target Directly") {
                store.setTargetAsSystemOutput()
            }
            .disabled(store.configuration.targetOutputDeviceUID.isEmpty)

            Button("Reset Relay") {
                store.resetRelay()
            }
            .disabled(store.virtualDevice == nil)

            Button("Restart coreaudiod") {
                store.restartCoreAudio()
            }

            Divider()

            Button("Settings...") {
                openWindow(id: "main")
            }

            Button("Refresh") {
                store.refresh()
            }
        }
        .frame(minWidth: 260)
    }
}
