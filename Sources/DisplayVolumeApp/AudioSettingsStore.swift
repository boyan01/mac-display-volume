import DisplayVolumeCore
import Foundation
import Observation
import ServiceManagement

@MainActor
@Observable
final class AudioSettingsStore {
    var devices: [AudioDeviceInfo] = []
    var defaultOutputDevice: AudioDeviceInfo?
    var virtualDevice: AudioDeviceInfo?
    var driverStatus = DriverStatus()
    var configuration = DisplayVolumeConfiguration()
    var errorMessage: String?

    private let hardware = AudioHardware()
    private let configurationStore = DisplayVolumeConfigurationStore()

    init() {
        configuration = configurationStore.load()
        syncLaunchAtLoginStatus()
        refresh()
    }

    var selectedTargetDevice: AudioDeviceInfo? {
        devices.first { $0.uid == configuration.targetOutputDeviceUID }
    }

    func refresh() {
        do {
            devices = try hardware.outputDevices()
            defaultOutputDevice = try hardware.defaultOutputDevice()
            virtualDevice = try hardware.virtualDevice()
            if virtualDevice != nil {
                driverStatus = try hardware.driverStatus()
                if configuration.targetOutputDeviceUID.isEmpty {
                    let driverTargetUID = try hardware.driverTargetOutputUID()
                    if !driverTargetUID.isEmpty {
                        configuration.targetOutputDeviceUID = driverTargetUID
                        persistConfiguration()
                    }
                }
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func selectTargetDevice(_ uid: String) {
        configuration.targetOutputDeviceUID = uid
        persistConfiguration()
        applyDriverConfiguration()
    }

    func setPreferredBufferFrameSize(_ value: Int) {
        configuration.preferredBufferFrameSize = value
        persistConfiguration()
        applyDriverConfiguration()
    }

    func setLaunchAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            syncLaunchAtLoginStatus(persist: true)
            if enabled, SMAppService.mainApp.status == .requiresApproval {
                errorMessage = "Launch at login requires approval in System Settings."
            }
        } catch {
            syncLaunchAtLoginStatus(persist: true)
            errorMessage = error.localizedDescription
        }
    }

    func setTargetAsSystemOutput() {
        guard !configuration.targetOutputDeviceUID.isEmpty else {
            errorMessage = "Choose a target output device first."
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

    private func persistConfiguration() {
        do {
            try configurationStore.save(configuration)
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func syncLaunchAtLoginStatus(persist: Bool = false) {
        let enabled = SMAppService.mainApp.status == .enabled
        if configuration.launchAtLogin != enabled {
            configuration.launchAtLogin = enabled
        }

        if persist {
            persistConfiguration()
        }
    }
}
