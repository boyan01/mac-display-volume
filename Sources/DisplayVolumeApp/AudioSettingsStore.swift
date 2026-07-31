import DisplayVolumeCore
import Foundation
import Observation

@MainActor
@Observable
final class AudioSettingsStore {
    var devices: [AudioDeviceInfo] = []
    var defaultOutputDevice: AudioDeviceInfo?
    var virtualDevice: AudioDeviceInfo?
    var isDriverInstalled = false
    var driverStatus = DriverStatus()
    var configuration = DisplayVolumeConfiguration()
    var errorMessage: String?

    private let hardware = AudioHardware()
    private let defaults = UserDefaults.standard
    private var statusRefreshTask: Task<Void, Never>?

    init() {
        refresh()
        startDriverStatusRefresh()
    }

    var selectedTargetDevice: AudioDeviceInfo? {
        devices.first { $0.uid == configuration.targetOutputDeviceUID }
    }

    var selectedTargetName: String? {
        selectedTargetDevice?.name ?? defaults.string(forKey: "targetOutputDeviceName")
    }

    func refresh() {
        do {
            devices = try hardware.outputDevices()
            defaultOutputDevice = try hardware.defaultOutputDevice()
            virtualDevice = try hardware.virtualDevice()
            isDriverInstalled = hardware.isDriverInstalled()
            if isDriverInstalled {
                driverStatus = try hardware.driverStatus()
                configuration.targetOutputDeviceUID = try hardware.driverTargetOutputUID()
                configuration.preferredBufferFrameSize = try hardware.driverPreferredBufferFrameSize()
                if let selectedTargetDevice {
                    defaults.set(selectedTargetDevice.name, forKey: "targetOutputDeviceName")
                }
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func startDriverStatusRefresh() {
        statusRefreshTask?.cancel()
        statusRefreshTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard !Task.isCancelled else {
                    return
                }
                guard let self else {
                    return
                }
                self.refresh()
            }
        }
    }

    func selectTargetDevice(_ uid: String) {
        configuration.targetOutputDeviceUID = uid
        if let device = devices.first(where: { $0.uid == uid }) {
            defaults.set(device.name, forKey: "targetOutputDeviceName")
        } else if uid.isEmpty {
            defaults.removeObject(forKey: "targetOutputDeviceName")
        }
        applyDriverConfiguration()
    }

    func setPreferredBufferFrameSize(_ value: Int) {
        configuration.preferredBufferFrameSize = value
        do {
            try hardware.setPreferredBufferFrameSize(UInt32(value))
            if let driverStatus = try? hardware.driverStatus() {
                self.driverStatus = driverStatus
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func setTargetAsSystemOutput() {
        guard !configuration.targetOutputDeviceUID.isEmpty else {
            errorMessage = String(localized: "Choose a target output device first.")
            return
        }

        do {
            try hardware.setDefaultOutputDevice(uid: configuration.targetOutputDeviceUID)
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func setVirtualDeviceAsSystemOutput() {
        do {
            applyDriverConfiguration()
            try hardware.setVirtualDeviceAsDefaultOutput()
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func applyDriverConfiguration() {
        do {
            try hardware.applyDriverConfiguration(configuration)
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func resetRelay() {
        do {
            try hardware.resetRelay()
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func restartCoreAudio() {
        Task {
            do {
                try await hardware.restartCoreAudio()
                refresh()
            } catch {
                errorMessage = error.localizedDescription
            }
        }
    }
}
