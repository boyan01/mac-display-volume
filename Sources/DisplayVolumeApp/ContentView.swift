import DisplayVolumeCore
import SwiftUI

struct ContentView: View {
    @Environment(AudioSettingsStore.self) private var store
    @State private var showingBufferHelp = false
    @State private var showingLatencyHelp = false

    var body: some View {
        @Bindable var store = store

        VStack(spacing: 0) {
            header

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    ViewThatFits(in: .horizontal) {
                        HStack(alignment: .top, spacing: 16) {
                            outputPanel(selection: $store.configuration.targetOutputDeviceUID)
                            latencyPanel(selection: $store.configuration.preferredBufferFrameSize)
                        }

                        VStack(spacing: 16) {
                            outputPanel(selection: $store.configuration.targetOutputDeviceUID)
                            latencyPanel(selection: $store.configuration.preferredBufferFrameSize)
                        }
                    }

                    driverPanel
                }
                .padding(20)
            }
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            HStack(spacing: 10) {
                Button {
                    store.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }

                Menu {
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

                    Divider()

                    Button(role: .destructive) {
                        store.restartCoreAudio()
                    } label: {
                        Label("Restart coreaudiod", systemImage: "arrow.triangle.2.circlepath")
                    }
                } label: {
                    Label("Driver Actions", systemImage: "ellipsis.circle")
                }

                Spacer()

                Button {
                    store.setTargetAsSystemOutput()
                } label: {
                    Label("Use Target Directly", systemImage: "speaker.wave.2")
                }
                .disabled(store.configuration.targetOutputDeviceUID.isEmpty)

                Button {
                    store.setVirtualDeviceAsSystemOutput()
                } label: {
                    Label("Use Virtual Output", systemImage: "slider.horizontal.3")
                }
                .disabled(store.virtualDevice == nil || store.configuration.targetOutputDeviceUID.isEmpty)
                .buttonStyle(.borderedProminent)
            }
            .controlSize(.regular)
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            .background(.bar)
        }
        .background(.regularMaterial)
    }

    private var header: some View {
        HStack(spacing: 14) {
            Image(systemName: "speaker.wave.2.fill")
                .font(.title2)
                .foregroundStyle(.white)
                .frame(width: 44, height: 44)
                .background(.blue, in: RoundedRectangle(cornerRadius: 8, style: .continuous))

            VStack(alignment: .leading, spacing: 3) {
                Text("Mac Display Volume")
                    .font(.title2.weight(.semibold))
                Text(headerMessage)
                    .font(.callout)
                    .foregroundStyle(headerMessageColor)
            }

            Spacer()

            Label(driverStatusText, systemImage: statusSymbol)
                .font(.callout.weight(.medium))
                .foregroundStyle(statusColor)
                .padding(.horizontal, 11)
                .padding(.vertical, 7)
                .background(statusColor.opacity(0.12), in: Capsule())
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
        .background(.bar)
    }

    private func outputPanel(selection: Binding<String>) -> some View {
        sectionPanel("Output", systemImage: "hifispeaker.2") {
            VStack(alignment: .leading, spacing: 12) {
                Picker("Target display audio", selection: selection) {
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

                infoRow("Selected target", value: selectedTargetName, systemImage: "display")
                infoRow("System output", value: systemOutputName, systemImage: "speaker")
            }
        }
    }

    private func latencyPanel(selection: Binding<Int>) -> some View {
        sectionPanel("Latency", systemImage: "speedometer") {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 8) {
                    Label("Preferred buffer", systemImage: "rectangle.compress.vertical")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    helpButton(
                        isPresented: $showingBufferHelp,
                        text: "Preferred buffer controls how many frames the relay preloads before starting or recovering. Lower values reduce latency; higher values are more tolerant of scheduling jitter."
                    )
                    Spacer()
                }

                Picker("Preferred buffer", selection: selection) {
                    ForEach(DisplayVolumeConfiguration.supportedBufferFrameSizes, id: \.self) { frameSize in
                        Text("\(frameSize) frames").tag(frameSize)
                    }
                }
                .pickerStyle(.segmented)
                .onChange(of: store.configuration.preferredBufferFrameSize) { _, newValue in
                    store.setPreferredBufferFrameSize(newValue)
                }

                infoRow("Buffer", value: "\(store.configuration.preferredBufferFrameSize) frames", systemImage: "rectangle.compress.vertical")
                infoRow("Target buffer", value: targetBufferText, systemImage: "rectangle.stack")
                infoRow("Target IO", value: targetIOText, systemImage: "waveform")
                explainedInfoRow(
                    "Queued latency",
                    value: latencyText,
                    systemImage: "timer",
                    valueColor: latencyColor,
                    isPresented: $showingLatencyHelp,
                    explanation: "Queued latency is the amount of audio currently waiting in the relay buffer. Dropped frames and underruns indicate playback instability."
                )
                infoRow("Dropped", value: "\(store.driverStatus.droppedFrames)", systemImage: "arrow.down.forward.and.arrow.up.backward", valueColor: droppedColor)
                infoRow("Underruns", value: "\(store.driverStatus.underruns)", systemImage: "waveform.path.badge.minus", valueColor: underrunColor)
            }
        }
    }

    private var driverPanel: some View {
        sectionPanel("Driver", systemImage: "waveform.path.ecg") {
            VStack(alignment: .leading, spacing: 12) {
                infoRow("Virtual device", value: virtualDeviceName, systemImage: "externaldrive.connected.to.line.below")
                infoRow("Target", value: targetStateText, systemImage: "antenna.radiowaves.left.and.right")

                if let errorMessage = store.errorMessage {
                    Divider()
                    Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                        .font(.callout)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                }
            }
        }
    }

    private func sectionPanel<Content: View>(
        _ title: LocalizedStringKey,
        systemImage: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            Label(title, systemImage: systemImage)
                .font(.headline)
                .foregroundStyle(.primary)

            content()
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .stroke(.separator.opacity(0.55), lineWidth: 1)
        }
    }

    private func infoRow(
        _ title: LocalizedStringKey,
        value: String,
        systemImage: String,
        valueColor: Color = .primary
    ) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Label(title, systemImage: systemImage)
                .font(.callout)
                .foregroundStyle(.secondary)

            Spacer(minLength: 16)

            Text(value)
                .font(.callout.weight(.medium))
                .foregroundStyle(valueColor)
                .multilineTextAlignment(.trailing)
                .lineLimit(2)
        }
    }

    private func explainedInfoRow(
        _ title: LocalizedStringKey,
        value: String,
        systemImage: String,
        valueColor: Color,
        isPresented: Binding<Bool>,
        explanation: LocalizedStringKey
    ) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Label(title, systemImage: systemImage)
                .font(.callout)
                .foregroundStyle(.secondary)

            helpButton(isPresented: isPresented, text: explanation)

            Spacer(minLength: 16)

            Text(value)
                .font(.callout.weight(.medium))
                .foregroundStyle(valueColor)
                .monospacedDigit()
                .multilineTextAlignment(.trailing)
        }
    }

    private func helpButton(
        isPresented: Binding<Bool>,
        text: LocalizedStringKey
    ) -> some View {
        Button {
            isPresented.wrappedValue = true
        } label: {
            Image(systemName: "info.circle")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .buttonStyle(.plain)
        .popover(isPresented: isPresented) {
            Text(text)
                .font(.callout)
                .foregroundStyle(.primary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(width: 280, alignment: .leading)
                .padding(14)
        }
    }

    private var isDriverWorking: Bool {
        store.virtualDevice != nil &&
            store.driverStatus.isRunning &&
            !store.driverStatus.isPriming &&
            store.driverStatus.targetAlive
    }

    private var headerMessage: String {
        if isDriverWorking {
            return String(localized: "Driver is working. You can quit the app anytime.")
        }
        if store.virtualDevice == nil {
            return String(localized: "Driver is not installed.")
        }
        if store.driverStatus.isPriming {
            return String(localized: "Driver is priming the relay buffer.")
        }
        return String(localized: "Choose a target output and switch to the virtual device.")
    }

    private var headerMessageColor: Color {
        isDriverWorking ? .green : .secondary
    }

    private var driverStatusText: String {
        if store.virtualDevice == nil {
            return String(localized: "Missing")
        }
        if !store.driverStatus.isRunning {
            return String(localized: "Idle")
        }
        if store.driverStatus.isPriming {
            return String(localized: "Priming")
        }
        return String(localized: "Running")
    }

    private var statusSymbol: String {
        if store.virtualDevice == nil {
            return "exclamationmark.triangle.fill"
        }
        if !store.driverStatus.isRunning {
            return "pause.circle.fill"
        }
        if store.driverStatus.isPriming {
            return "hourglass.circle.fill"
        }
        return "checkmark.circle.fill"
    }

    private var statusColor: Color {
        if store.virtualDevice == nil {
            return .red
        }
        if !store.driverStatus.isRunning {
            return .secondary
        }
        if store.driverStatus.isPriming {
            return .orange
        }
        return .green
    }

    private var latencyText: String {
        String(format: "%.1f ms", store.driverStatus.queuedMilliseconds)
    }

    private var targetBufferText: String {
        frameLatencyText(
            frames: store.driverStatus.targetBufferFrames,
            milliseconds: store.driverStatus.targetBufferMilliseconds
        )
    }

    private var targetIOText: String {
        frameLatencyText(
            frames: store.driverStatus.targetIOFrames,
            milliseconds: store.driverStatus.targetIOMilliseconds
        )
    }

    private func frameLatencyText(frames: Int, milliseconds: Double) -> String {
        guard frames > 0 else {
            return String(localized: "Unknown")
        }
        return String(format: String(localized: "%lld frames / %.1f ms"), Int64(frames), milliseconds)
    }

    private var latencyColor: Color {
        store.driverStatus.queuedMilliseconds > 50 ? .orange : .secondary
    }

    private var droppedColor: Color {
        store.driverStatus.droppedFrames > 0 ? .orange : .secondary
    }

    private var underrunColor: Color {
        store.driverStatus.underruns > 0 ? .orange : .secondary
    }

    private var selectedTargetName: String {
        store.selectedTargetDevice?.name ?? String(localized: "Not selected")
    }

    private var systemOutputName: String {
        store.defaultOutputDevice?.name ?? String(localized: "Unknown")
    }

    private var virtualDeviceName: String {
        store.virtualDevice?.name ?? String(localized: "Not installed")
    }

    private var targetStateText: String {
        if !store.driverStatus.isRunning {
            return String(localized: "Idle")
        }
        return store.driverStatus.targetAlive ? String(localized: "Connected") : String(localized: "Waiting")
    }

}
