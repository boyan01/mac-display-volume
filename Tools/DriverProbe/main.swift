import CoreAudio
import DisplayVolumeCore
import Foundation

private enum ProbeError: Error, CustomStringConvertible {
    case help(String)
    case usage(String)
    case coreAudio(String, OSStatus)
    case missingVirtualDevice
    case missingSupportedTarget
    case unexpected(String)

    var description: String {
        switch self {
        case let .help(message):
            return message
        case let .usage(message):
            return message
        case let .coreAudio(operation, status):
            return "\(operation) failed with OSStatus \(status)."
        case .missingVirtualDevice:
            return "Mac Display Volume device was not found in CoreAudio."
        case .missingSupportedTarget:
            return "No non-virtual 48 kHz output device is available as a relay target."
        case let .unexpected(message):
            return message
        }
    }
}

private struct Options {
    var mutating = false
    var configureTarget = false
    var requireTarget = false
    var setDefaultVirtual = false
    var setDefaultTarget = false
    var reset = false
    var quiet = false
}

private func parseOptions() throws -> Options {
    var options = Options()
    for argument in CommandLine.arguments.dropFirst() {
        switch argument {
        case "--mutating":
            options.mutating = true
        case "--configure-target":
            options.configureTarget = true
        case "--require-target":
            options.requireTarget = true
        case "--set-default-virtual":
            options.setDefaultVirtual = true
        case "--set-default-target":
            options.setDefaultTarget = true
        case "--reset":
            options.reset = true
        case "--quiet":
            options.quiet = true
        case "--help", "-h":
            throw ProbeError.help("""
            Usage: DriverProbe [--mutating] [--configure-target] [--require-target] [--set-default-virtual|--set-default-target] [--reset] [--quiet]

            --mutating            Exercise writable HAL properties by writing current values back.
            --configure-target    Set the driver target to the first non-virtual 48 kHz output.
            --require-target      Fail if no non-virtual 48 kHz output target is available.
            --set-default-virtual Set Mac Display Volume as the system default output.
            --set-default-target  Set the first non-virtual 48 kHz target as the system default output.
            --reset               Reset relay health counters before reading status.
            --quiet               Print only failures.
            """)
        default:
            throw ProbeError.usage("Unknown argument: \(argument)")
        }
    }
    if options.setDefaultVirtual && options.setDefaultTarget {
        throw ProbeError.usage("--set-default-virtual and --set-default-target cannot be used together.")
    }
    return options
}

private func address(
    _ selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain
    )
}

private func require(_ status: OSStatus, _ operation: String) throws {
    guard status == noErr else {
        throw ProbeError.coreAudio(operation, status)
    }
}

private func property<T>(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
    as type: T.Type
) throws -> T {
    var propertyAddress = address(selector, scope: scope)
    let pointer = UnsafeMutablePointer<T>.allocate(capacity: 1)
    defer {
        pointer.deallocate()
    }
    var dataSize = UInt32(MemoryLayout<T>.size)
    try require(
        AudioObjectGetPropertyData(objectID, &propertyAddress, 0, nil, &dataSize, pointer),
        "read \(selector.fourCC) on object \(objectID)"
    )
    return pointer.pointee
}

private func setProperty<T>(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    value: inout T,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
) throws {
    var propertyAddress = address(selector, scope: scope)
    let status = withUnsafeBytes(of: &value) { buffer in
        AudioObjectSetPropertyData(
            objectID,
            &propertyAddress,
            0,
            nil,
            UInt32(buffer.count),
            buffer.baseAddress!
        )
    }
    try require(status, "write \(selector.fourCC) on object \(objectID)")
}

private func propertyDataSize(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
) throws -> UInt32 {
    var propertyAddress = address(selector, scope: scope)
    var dataSize: UInt32 = 0
    try require(
        AudioObjectGetPropertyDataSize(objectID, &propertyAddress, 0, nil, &dataSize),
        "read size for \(selector.fourCC) on object \(objectID)"
    )
    return dataSize
}

private func objectIDArray(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
) throws -> [AudioObjectID] {
    let dataSize = try propertyDataSize(objectID: objectID, selector: selector, scope: scope)
    guard dataSize > 0 else {
        return []
    }

    var values = Array(repeating: AudioObjectID(0), count: Int(dataSize) / MemoryLayout<AudioObjectID>.size)
    var mutableDataSize = dataSize
    var propertyAddress = address(selector, scope: scope)
    try require(
        AudioObjectGetPropertyData(objectID, &propertyAddress, 0, nil, &mutableDataSize, &values),
        "read \(selector.fourCC) on object \(objectID)"
    )
    return values.filter { $0 != 0 }
}

private func isSettable(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
) throws -> Bool {
    var propertyAddress = address(selector, scope: scope)
    var result = DarwinBoolean(false)
    try require(
        AudioObjectIsPropertySettable(objectID, &propertyAddress, &result),
        "check settable \(selector.fourCC) on object \(objectID)"
    )
    return result.boolValue
}

private func translateUID(_ uid: String) throws -> AudioObjectID {
    var propertyAddress = address(kAudioHardwarePropertyTranslateUIDToDevice)
    var cfUID = uid as CFString
    var deviceID = AudioDeviceID(0)
    var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)
    let status = withUnsafePointer(to: &cfUID) { uidPointer in
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            UInt32(MemoryLayout<CFString>.size),
            uidPointer,
            &dataSize,
            &deviceID
        )
    }
    try require(status, "translate UID \(uid)")
    return deviceID
}

private func setDefaultOutput(_ deviceID: AudioDeviceID) throws {
    var value = deviceID
    try setProperty(
        objectID: AudioObjectID(kAudioObjectSystemObject),
        selector: kAudioHardwarePropertyDefaultOutputDevice,
        value: &value
    )
    try setProperty(
        objectID: AudioObjectID(kAudioObjectSystemObject),
        selector: kAudioHardwarePropertyDefaultSystemOutputDevice,
        value: &value
    )
}

private func exerciseStreamProperties(deviceID: AudioObjectID, streamID: AudioObjectID, mutating: Bool) throws {
    let isActive = try property(
        objectID: streamID,
        selector: kAudioStreamPropertyIsActive,
        as: UInt32.self
    )
    _ = try isSettable(objectID: streamID, selector: kAudioStreamPropertyIsActive)

    let virtualFormat = try property(
        objectID: streamID,
        selector: kAudioStreamPropertyVirtualFormat,
        as: AudioStreamBasicDescription.self
    )
    _ = try isSettable(objectID: streamID, selector: kAudioStreamPropertyVirtualFormat)

    let physicalFormat = try property(
        objectID: streamID,
        selector: kAudioStreamPropertyPhysicalFormat,
        as: AudioStreamBasicDescription.self
    )
    _ = try isSettable(objectID: streamID, selector: kAudioStreamPropertyPhysicalFormat)

    let nominalSampleRate = try property(
        objectID: deviceID,
        selector: kAudioDevicePropertyNominalSampleRate,
        as: Double.self
    )
    _ = try isSettable(objectID: deviceID, selector: kAudioDevicePropertyNominalSampleRate)

    guard AudioHardware.isSupportedTargetSampleRate(nominalSampleRate),
          AudioHardware.isSupportedTargetSampleRate(virtualFormat.mSampleRate),
          AudioHardware.isSupportedTargetSampleRate(physicalFormat.mSampleRate) else {
        throw ProbeError.unexpected("Virtual stream is not reporting the expected 48 kHz format.")
    }

    if mutating {
        var active = isActive
        try setProperty(objectID: streamID, selector: kAudioStreamPropertyIsActive, value: &active)

        var sampleRate = nominalSampleRate
        try setProperty(objectID: deviceID, selector: kAudioDevicePropertyNominalSampleRate, value: &sampleRate)

        var newVirtualFormat = virtualFormat
        try setProperty(objectID: streamID, selector: kAudioStreamPropertyVirtualFormat, value: &newVirtualFormat)

        var newPhysicalFormat = physicalFormat
        try setProperty(objectID: streamID, selector: kAudioStreamPropertyPhysicalFormat, value: &newPhysicalFormat)
    }
}

private func exerciseControls(deviceID: AudioObjectID, mutating: Bool) throws {
    let controls = try objectIDArray(objectID: deviceID, selector: kAudioObjectPropertyControlList)
    guard !controls.isEmpty else {
        throw ProbeError.unexpected("Virtual device does not expose volume/mute controls.")
    }

    for controlID in controls {
        let classID = try property(
            objectID: controlID,
            selector: kAudioObjectPropertyClass,
            as: AudioClassID.self
        )
        switch classID {
        case kAudioVolumeControlClassID:
            let scalar = try property(
                objectID: controlID,
                selector: kAudioLevelControlPropertyScalarValue,
                as: Float32.self
            )
            _ = try isSettable(objectID: controlID, selector: kAudioLevelControlPropertyScalarValue)
            _ = try isSettable(objectID: controlID, selector: kAudioLevelControlPropertyDecibelValue)
            if mutating {
                var value = scalar
                try setProperty(objectID: controlID, selector: kAudioLevelControlPropertyScalarValue, value: &value)
            }
        case kAudioMuteControlClassID:
            let mute = try property(
                objectID: controlID,
                selector: kAudioBooleanControlPropertyValue,
                as: UInt32.self
            )
            _ = try isSettable(objectID: controlID, selector: kAudioBooleanControlPropertyValue)
            if mutating {
                var value = mute
                try setProperty(objectID: controlID, selector: kAudioBooleanControlPropertyValue, value: &value)
            }
        default:
            break
        }
    }
}

private func runProbe(options: Options) throws {
    let hardware = AudioHardware()
    let outputs = try hardware.outputDevices()
    guard let virtualDevice = outputs.first(where: { $0.uid == AudioHardware.virtualDeviceUID }) else {
        throw ProbeError.missingVirtualDevice
    }

    let translatedID = try translateUID(AudioHardware.virtualDeviceUID)
    guard translatedID == virtualDevice.id else {
        throw ProbeError.unexpected(
            "UID translation returned \(translatedID), but device list returned \(virtualDevice.id)."
        )
    }

    let supportedTargets = outputs.filter {
        $0.uid != AudioHardware.virtualDeviceUID &&
        AudioHardware.isSupportedTargetSampleRate($0.nominalSampleRate)
    }
    if options.requireTarget && supportedTargets.isEmpty {
        throw ProbeError.missingSupportedTarget
    }

    if options.configureTarget, let target = supportedTargets.first {
        let configuration = DisplayVolumeConfiguration(
            targetOutputDeviceUID: target.uid,
            preferredBufferFrameSize: 128
        )
        try hardware.applyDriverConfiguration(configuration)
    }

    if options.setDefaultVirtual {
        try setDefaultOutput(virtualDevice.id)
    }

    if options.setDefaultTarget {
        guard let target = supportedTargets.first else {
            throw ProbeError.missingSupportedTarget
        }
        try setDefaultOutput(target.id)
    }

    if options.reset {
        try hardware.resetRelay()
    }

    _ = try hardware.driverTargetOutputUID()
    _ = try hardware.driverPreferredBufferFrameSize()
    let status = try hardware.driverStatus()

    let streams = try objectIDArray(
        objectID: virtualDevice.id,
        selector: kAudioDevicePropertyStreams,
        scope: kAudioObjectPropertyScopeOutput
    )
    guard let outputStream = streams.first else {
        throw ProbeError.unexpected("Virtual device does not expose an output stream.")
    }

    try exerciseStreamProperties(deviceID: virtualDevice.id, streamID: outputStream, mutating: options.mutating)
    try exerciseControls(deviceID: virtualDevice.id, mutating: options.mutating)

    if !options.quiet {
        print("DriverProbe OK")
        print("virtualDevice=\(virtualDevice.id) name=\"\(virtualDevice.name)\" uid=\"\(virtualDevice.uid)\"")
        print("outputs=\(outputs.count) supportedTargets=\(supportedTargets.count)")
        print("stream=\(outputStream)")
        print("status running=\(status.isRunning) target=\(status.targetAlive) priming=\(status.isPriming) queued=\(status.queuedFrames) queuedMS=\(String(format: "%.2f", status.queuedMilliseconds)) buffer=\(status.bufferFrames) targetBuffer=\(status.targetBufferFrames) targetBufferMS=\(String(format: "%.2f", status.targetBufferMilliseconds)) targetIO=\(status.targetIOFrames) targetIOMS=\(String(format: "%.2f", status.targetIOMilliseconds)) dropped=\(status.droppedFrames) underruns=\(status.underruns)")
    }
}

do {
    try runProbe(options: try parseOptions())
} catch let error as ProbeError {
    if case let .help(message) = error {
        print(message)
        exit(0)
    }
    if case let .usage(message) = error {
        fputs("\(message)\n", stderr)
        exit(2)
    }
    fputs("DriverProbe failed: \(error.description)\n", stderr)
    exit(1)
} catch {
    fputs("DriverProbe failed: \(error)\n", stderr)
    exit(1)
}

private extension AudioObjectPropertySelector {
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
