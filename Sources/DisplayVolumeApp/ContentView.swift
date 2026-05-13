import DisplayVolumeCore
import SwiftUI

struct ContentView: View {
    @Environment(AudioSettingsStore.self) private var store

    var body: some View {
        @Bindable var store = store

        VStack(alignment: .leading, spacing: 20) {
            header

            Form {
                Section("Driver") {
                    LabeledContent("Virtual device") {
                        Text(store.virtualDevice?.name ?? String(localized: "Not installed"))
                            .foregroundStyle(store.virtualDevice == nil ? .red : .secondary)
                    }

                    LabeledContent("Status") {
                        Text(driverStatusText)
                            .foregroundStyle(store.driverStatus.isPriming && store.driverStatus.isRunning ? .orange : .secondary)
                    }

                    LabeledContent("Queued latency") {
                        Text("\(String(format: "%.1f", store.driverStatus.queuedMilliseconds)) ms")
                            .foregroundStyle(store.driverStatus.queuedMilliseconds > 50 ? .orange : .secondary)
                    }

                    LabeledContent("Health") {
                        Text(
                            String(
                                format: String(localized: "dropped %lld, underruns %lld"),
                                store.driverStatus.droppedFrames,
                                store.driverStatus.underruns
                            )
                        )
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Output") {
                    Picker("Target display audio", selection: $store.configuration.targetOutputDeviceUID) {
                        Text("Choose a device").tag("")
                        ForEach(store.devices) { device in
                            if device.uid != AudioHardware.virtualDeviceUID,
                               AudioHardware.isSupportedTargetSampleRate(device.nominalSampleRate) {
                                Text(device.name).tag(device.uid)
                            }
                        }
                    }
                    .onChange(of: store.configuration.targetOutputDeviceUID) { _, newValue in
                        store.selectTargetDevice(newValue)
                    }

                    LabeledContent("Current system output") {
                        Text(store.defaultOutputDevice?.name ?? String(localized: "Unknown"))
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Latency") {
                    Picker("Preferred buffer", selection: $store.configuration.preferredBufferFrameSize) {
                        ForEach(DisplayVolumeConfiguration.supportedBufferFrameSizes, id: \.self) { frameSize in
                            Text("\(frameSize) frames").tag(frameSize)
                        }
                    }
                    .pickerStyle(.segmented)
                    .onChange(of: store.configuration.preferredBufferFrameSize) { _, newValue in
                        store.setPreferredBufferFrameSize(newValue)
                    }
                }

            }
            .formStyle(.grouped)

            HStack {
                Button {
                    store.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }

                Spacer()

                Button {
                    store.setTargetAsSystemOutput()
                } label: {
                    Label("Use Target Directly", systemImage: "speaker.wave.2")
                }

                Button {
                    store.setVirtualDeviceAsSystemOutput()
                } label: {
                    Label("Use Virtual Output", systemImage: "slider.horizontal.3")
                }
                .disabled(store.virtualDevice == nil)
                .buttonStyle(.borderedProminent)
            }

            HStack {
                Button {
                    store.applyDriverConfiguration()
                } label: {
                    Label("Apply Driver Config", systemImage: "checkmark.circle")
                }
                .disabled(store.virtualDevice == nil)

                Button {
                    store.resetRelay()
                } label: {
                    Label("Reset Relay", systemImage: "waveform.path")
                }
                .disabled(store.virtualDevice == nil)

                Button(role: .destructive) {
                    store.restartCoreAudio()
                } label: {
                    Label("Restart coreaudiod", systemImage: "arrow.triangle.2.circlepath")
                }
            }

            if let errorMessage = store.errorMessage {
                Text(errorMessage)
                    .font(.callout)
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
            }
        }
        .padding(24)
    }

    private var driverStatusText: String {
        if !store.driverStatus.isRunning {
            return String(localized: "Idle")
        }
        if store.driverStatus.isPriming {
            return String(localized: "Priming")
        }
        return String(localized: "Running")
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Mac Display Volume")
                .font(.title2.weight(.semibold))
            Text("Software volume for fixed-volume display audio outputs.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }
}
