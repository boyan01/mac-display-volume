import CoreAudio
import Foundation

public enum AudioHardwareError: Error, LocalizedError {
    case propertySize(AudioObjectID, AudioObjectPropertySelector, OSStatus)
    case propertyRead(AudioObjectID, AudioObjectPropertySelector, OSStatus)
    case propertyWrite(AudioObjectID, AudioObjectPropertySelector, AudioObjectPropertyScope, OSStatus)
    case deviceNotFound(String)
    case invalidTargetDevice(String)
    case unsupportedSampleRate(String, Double)
    case commandFailed(String)

    public var errorDescription: String? {
        switch self {
        case let .propertySize(objectID, selector, status):
            "Could not read property size for object \(objectID), selector \(selector.fourCC), status \(status)."
        case let .propertyRead(objectID, selector, status):
            "Could not read property for object \(objectID), selector \(selector.fourCC), status \(status)."
        case let .propertyWrite(objectID, selector, scope, status):
            "Could not write property for object \(objectID), selector \(selector.fourCC), scope \(scope.fourCC), status \(status)."
        case let .deviceNotFound(name):
            "Audio device not found: \(name)."
        case let .invalidTargetDevice(name):
            "Invalid target audio device: \(name)."
        case let .unsupportedSampleRate(name, sampleRate):
            "Audio device \(name) uses \(sampleRate.formatted()) Hz. Mac Display Volume currently supports 48,000 Hz targets only."
        case let .commandFailed(message):
            message
        }
    }
}

public struct AudioHardware: Sendable {
    public static let virtualDeviceName = "Mac Display Volume"
    public static let virtualDeviceUID = "tech.soit.MacDisplayVolume.Device"
    public static let supportedTargetSampleRate = 48_000.0
    public static let targetUIDSelector = fourCC("tgud")
    public static let bufferFrameSizeSelector = fourCC("bfsz")
    public static let statusSelector = fourCC("stat")
    public static let resetSelector = fourCC("rset")

    public init() {}

    public static func isSupportedTargetSampleRate(_ sampleRate: Double) -> Bool {
        abs(sampleRate - supportedTargetSampleRate) < 1.0
    }

    public func outputDevices() throws -> [AudioDeviceInfo] {
        try allDeviceIDs().compactMap { deviceID in
            guard let channelCount = try? outputChannelCount(for: deviceID), channelCount > 0 else {
                return nil
            }

            return AudioDeviceInfo(
                id: deviceID,
                uid: (try? stringProperty(deviceID, kAudioDevicePropertyDeviceUID)) ?? "",
                name: (try? stringProperty(deviceID, kAudioObjectPropertyName)) ?? "Unknown Device",
                transportType: (try? uint32Property(deviceID, kAudioDevicePropertyTransportType)) ?? 0,
                outputChannelCount: channelCount,
                nominalSampleRate: (try? doubleProperty(deviceID, kAudioDevicePropertyNominalSampleRate)) ?? 0
            )
        }
        .sorted { lhs, rhs in
            lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending
        }
    }

    public func defaultOutputDevice() throws -> AudioDeviceInfo? {
        let deviceID = try deviceIDProperty(kAudioHardwarePropertyDefaultOutputDevice)
        return try outputDevices().first { $0.id == deviceID }
    }

    public func virtualDevice() throws -> AudioDeviceInfo? {
        try outputDevices().first { $0.uid == Self.virtualDeviceUID }
    }

    public func setDefaultOutputDevice(uid: String) throws {
        let deviceID = try deviceID(forUID: uid)
        try setSystemDeviceProperty(kAudioHardwarePropertyDefaultOutputDevice, deviceID)
        try setSystemDeviceProperty(kAudioHardwarePropertyDefaultSystemOutputDevice, deviceID)
    }

    public func setVirtualDeviceAsDefaultOutput() throws {
        try setDefaultOutputDevice(uid: Self.virtualDeviceUID)
    }

    public func applyDriverConfiguration(_ configuration: DisplayVolumeConfiguration) throws {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        try validateTargetDevice(uid: configuration.targetOutputDeviceUID)
        try setStringProperty(
            virtualDeviceID,
            Self.targetUIDSelector,
            configuration.targetOutputDeviceUID
        )
        try setPreferredBufferFrameSize(UInt32(configuration.preferredBufferFrameSize), virtualDeviceID: virtualDeviceID)
    }

    public func setPreferredBufferFrameSize(_ value: UInt32) throws {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        try setPreferredBufferFrameSize(value, virtualDeviceID: virtualDeviceID)
    }

    private func setPreferredBufferFrameSize(_ value: UInt32, virtualDeviceID: AudioDeviceID) throws {
        try setNumberProperty(
            virtualDeviceID,
            Self.bufferFrameSizeSelector,
            value
        )
    }

    private func validateTargetDevice(uid: String) throws {
        guard !uid.isEmpty else {
            return
        }
        guard uid != Self.virtualDeviceUID else {
            throw AudioHardwareError.invalidTargetDevice(Self.virtualDeviceName)
        }

        let targetDeviceID = try deviceID(forUID: uid)
        let name = (try? stringProperty(targetDeviceID, kAudioObjectPropertyName)) ?? uid
        let sampleRate = try doubleProperty(targetDeviceID, kAudioDevicePropertyNominalSampleRate)
        guard Self.isSupportedTargetSampleRate(sampleRate) else {
            throw AudioHardwareError.unsupportedSampleRate(name, sampleRate)
        }
    }

    public func driverTargetOutputUID() throws -> String {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        return try stringProperty(virtualDeviceID, Self.targetUIDSelector)
    }

    public func driverPreferredBufferFrameSize() throws -> Int {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        return Int(try numberProperty(virtualDeviceID, Self.bufferFrameSizeSelector))
    }

    public func driverStatus() throws -> DriverStatus {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        return DriverStatus.parse(try stringProperty(virtualDeviceID, Self.statusSelector))
    }

    public func resetRelay() throws {
        let virtualDeviceID = try deviceID(forUID: Self.virtualDeviceUID)
        try setEmptyProperty(virtualDeviceID, Self.resetSelector)
    }

    public func restartCoreAudio() async throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = [
            "-e",
            "do shell script \"/usr/bin/killall coreaudiod\" with administrator privileges",
        ]

        let pipe = Pipe()
        process.standardError = pipe
        
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            process.terminationHandler = { p in
                if p.terminationStatus == 0 {
                    continuation.resume()
                } else {
                    let data = try? pipe.fileHandleForReading.readToEnd()
                    let message = String(data: data ?? Data(), encoding: .utf8) ?? "Failed to restart coreaudiod."
                    continuation.resume(throwing: AudioHardwareError.commandFailed(message))
                }
            }
            do {
                try process.run()
            } catch {
                continuation.resume(throwing: error)
            }
        }
    }

    private func allDeviceIDs() throws -> [AudioDeviceID] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0,
            nil,
            &dataSize
        )
        guard status == noErr else {
            throw AudioHardwareError.propertySize(
                AudioObjectID(kAudioObjectSystemObject),
                kAudioHardwarePropertyDevices,
                status
            )
        }

        var devices = Array(
            repeating: AudioDeviceID(0),
            count: Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        )
        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0,
            nil,
            &dataSize,
            &devices
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(
                AudioObjectID(kAudioObjectSystemObject),
                kAudioHardwarePropertyDevices,
                status
            )
        }

        return devices
    }

    private func deviceID(forUID uid: String) throws -> AudioDeviceID {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var cfUID = uid as CFString
        var deviceID = AudioDeviceID(0)
        var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = withUnsafePointer(to: &cfUID) { uidPointer in
            AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject),
                &address,
                UInt32(MemoryLayout<CFString>.size),
                uidPointer,
                &dataSize,
                &deviceID
            )
        }
        guard status == noErr, deviceID != 0 else {
            throw AudioHardwareError.deviceNotFound(uid)
        }
        return deviceID
    }

    private func setSystemDeviceProperty(
        _ selector: AudioObjectPropertySelector,
        _ deviceID: AudioDeviceID
    ) throws {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value = deviceID
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0,
            nil,
            UInt32(MemoryLayout<AudioDeviceID>.size),
            &value
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyWrite(
                AudioObjectID(kAudioObjectSystemObject),
                selector,
                kAudioObjectPropertyScopeGlobal,
                status
            )
        }
    }

    private func setStringProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector,
        _ value: String
    ) throws {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var cfValue = value as CFString
        let status = withUnsafePointer(to: &cfValue) { pointer in
            AudioObjectSetPropertyData(
                objectID,
                &address,
                0,
                nil,
                UInt32(MemoryLayout<CFString>.size),
                pointer
            )
        }
        guard status == noErr else {
            throw AudioHardwareError.propertyWrite(objectID, selector, kAudioObjectPropertyScopeGlobal, status)
        }
    }

    private func setNumberProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector,
        _ value: UInt32
    ) throws {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var mutableValue = value
        guard let number = CFNumberCreate(nil, .sInt32Type, &mutableValue) else {
            throw AudioHardwareError.propertyWrite(objectID, selector, kAudioObjectPropertyScopeGlobal, kAudioHardwareIllegalOperationError)
        }
        var propertyList: CFPropertyList = number
        let status = withUnsafePointer(to: &propertyList) { pointer in
            AudioObjectSetPropertyData(
                objectID,
                &address,
                0,
                nil,
                UInt32(MemoryLayout<CFPropertyList>.size),
                pointer
            )
        }
        guard status == noErr else {
            throw AudioHardwareError.propertyWrite(objectID, selector, kAudioObjectPropertyScopeGlobal, status)
        }
    }

    private func setEmptyProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector
    ) throws {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var unused: UInt8 = 0
        let status = withUnsafePointer(to: &unused) { pointer in
            AudioObjectSetPropertyData(
                objectID,
                &address,
                0,
                nil,
                0,
                pointer
            )
        }
        guard status == noErr else {
            throw AudioHardwareError.propertyWrite(objectID, selector, kAudioObjectPropertyScopeGlobal, status)
        }
    }

    private func setUInt32Property(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector,
        _ value: UInt32,
        fallbackScope: AudioObjectPropertyScope? = nil
    ) throws {
        var mutableValue = value
        let status = setUInt32Property(
            objectID,
            selector,
            &mutableValue,
            scope: kAudioObjectPropertyScopeGlobal
        )
        if status == noErr {
            return
        }
        if status == kAudioHardwareUnknownPropertyError, let fallbackScope {
            var fallbackValue = value
            let fallbackStatus = setUInt32Property(
                objectID,
                selector,
                &fallbackValue,
                scope: fallbackScope
            )
            if fallbackStatus == noErr {
                return
            }
            throw AudioHardwareError.propertyWrite(objectID, selector, fallbackScope, fallbackStatus)
        }
        throw AudioHardwareError.propertyWrite(objectID, selector, kAudioObjectPropertyScopeGlobal, status)
    }

    private func setUInt32Property(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector,
        _ value: inout UInt32,
        scope: AudioObjectPropertyScope
    ) -> OSStatus {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: scope,
            mElement: kAudioObjectPropertyElementMain
        )
        return AudioObjectSetPropertyData(
            objectID,
            &address,
            0,
            nil,
            UInt32(MemoryLayout<UInt32>.size),
            &value
        )
    }

    private func deviceIDProperty(_ selector: AudioObjectPropertySelector) throws -> AudioDeviceID {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value = AudioDeviceID(0)
        var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0,
            nil,
            &dataSize,
            &value
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(
                AudioObjectID(kAudioObjectSystemObject),
                selector,
                status
            )
        }
        return value
    }

    private func stringProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector
    ) throws -> String {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFString>? = nil
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFString>>.size)
        let status = withUnsafeMutablePointer(to: &value) { pointer in
            AudioObjectGetPropertyData(objectID, &address, 0, nil, &dataSize, pointer)
        }
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(objectID, selector, status)
        }
        guard let unmanagedValue = value else {
            return ""
        }
        return unmanagedValue.takeRetainedValue() as String
    }

    private func uint32Property(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector
    ) throws -> UInt32 {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: UInt32 = 0
        var dataSize = UInt32(MemoryLayout<UInt32>.size)
        let status = AudioObjectGetPropertyData(
            objectID,
            &address,
            0,
            nil,
            &dataSize,
            &value
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(objectID, selector, status)
        }
        return value
    }

    private func numberProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector
    ) throws -> UInt32 {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFPropertyList>? = nil
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFPropertyList>>.size)
        let status = withUnsafeMutablePointer(to: &value) { pointer in
            AudioObjectGetPropertyData(objectID, &address, 0, nil, &dataSize, pointer)
        }
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(objectID, selector, status)
        }
        guard let unmanagedValue = value else {
            throw AudioHardwareError.propertyRead(objectID, selector, kAudioHardwareIllegalOperationError)
        }
        let propertyList = unmanagedValue.takeRetainedValue()
        guard CFGetTypeID(propertyList) == CFNumberGetTypeID() else {
            throw AudioHardwareError.propertyRead(objectID, selector, kAudioHardwareIllegalOperationError)
        }
        var result: UInt32 = 0
        guard CFNumberGetValue((propertyList as! CFNumber), .sInt32Type, &result) else {
            throw AudioHardwareError.propertyRead(objectID, selector, kAudioHardwareIllegalOperationError)
        }
        return result
    }

    private func doubleProperty(
        _ objectID: AudioObjectID,
        _ selector: AudioObjectPropertySelector
    ) throws -> Double {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Double = 0
        var dataSize = UInt32(MemoryLayout<Double>.size)
        let status = AudioObjectGetPropertyData(
            objectID,
            &address,
            0,
            nil,
            &dataSize,
            &value
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(objectID, selector, status)
        }
        return value
    }

    private func outputChannelCount(for deviceID: AudioDeviceID) throws -> Int {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &dataSize)
        guard status == noErr else {
            throw AudioHardwareError.propertySize(
                deviceID,
                kAudioDevicePropertyStreamConfiguration,
                status
            )
        }

        let bufferListPointer = UnsafeMutableRawPointer.allocate(
            byteCount: Int(dataSize),
            alignment: MemoryLayout<AudioBufferList>.alignment
        )
        defer { bufferListPointer.deallocate() }

        status = AudioObjectGetPropertyData(
            deviceID,
            &address,
            0,
            nil,
            &dataSize,
            bufferListPointer
        )
        guard status == noErr else {
            throw AudioHardwareError.propertyRead(
                deviceID,
                kAudioDevicePropertyStreamConfiguration,
                status
            )
        }

        let bufferList = bufferListPointer.assumingMemoryBound(to: AudioBufferList.self)
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        return buffers.reduce(0) { count, buffer in
            count + Int(buffer.mNumberChannels)
        }
    }
}

extension AudioObjectPropertySelector {
    var fourCC: String {
        let bytes = [
            UInt8((self >> 24) & 0xff),
            UInt8((self >> 16) & 0xff),
            UInt8((self >> 8) & 0xff),
            UInt8(self & 0xff),
        ]
        return String(bytes: bytes, encoding: .macOSRoman) ?? "\(self)"
    }
}

private func fourCC(_ value: StaticString) -> AudioObjectPropertySelector {
    var result: UInt32 = 0
    let text = value.withUTF8Buffer { buffer in
        Array(buffer)
    }
    precondition(text.count == 4)
    for byte in text {
        result = (result << 8) | UInt32(byte)
    }
    return result
}
