import Foundation

public struct DriverStatus: Equatable, Sendable {
    public var isRunning: Bool
    public var targetAlive: Bool
    public var targetAvailable: Bool
    public var devicePublished: Bool
    public var willRestoreDefaultOutput: Bool
    public var isPriming: Bool
    public var queuedFrames: Int
    public var queuedMilliseconds: Double
    public var bufferFrames: Int
    public var targetBufferFrames: Int
    public var targetBufferMilliseconds: Double
    public var targetIOFrames: Int
    public var targetIOMilliseconds: Double
    public var droppedFrames: Int
    public var underruns: Int
    public var sampleRate: Double
    public var targetStartError: Int32

    public var hasTargetStartError: Bool {
        targetStartError != 0
    }

    public var targetStartErrorCodeDescription: String? {
        guard targetStartError != 0 else {
            return nil
        }

        let unsigned = UInt32(bitPattern: targetStartError)
        let bytes = [
            UInt8((unsigned >> 24) & 0xff),
            UInt8((unsigned >> 16) & 0xff),
            UInt8((unsigned >> 8) & 0xff),
            UInt8(unsigned & 0xff),
        ]
        if bytes.allSatisfy({ $0 >= 0x20 && $0 <= 0x7e }),
           let fourCC = String(bytes: bytes, encoding: .ascii) {
            return "'\(fourCC)' (\(targetStartError))"
        }
        return "\(targetStartError)"
    }

    public init(
        isRunning: Bool = false,
        targetAlive: Bool = false,
        targetAvailable: Bool = false,
        devicePublished: Bool = false,
        willRestoreDefaultOutput: Bool = false,
        isPriming: Bool = false,
        queuedFrames: Int = 0,
        queuedMilliseconds: Double = 0,
        bufferFrames: Int = 0,
        targetBufferFrames: Int = 0,
        targetBufferMilliseconds: Double = 0,
        targetIOFrames: Int = 0,
        targetIOMilliseconds: Double = 0,
        droppedFrames: Int = 0,
        underruns: Int = 0,
        sampleRate: Double = 0,
        targetStartError: Int32 = 0
    ) {
        self.isRunning = isRunning
        self.targetAlive = targetAlive
        self.targetAvailable = targetAvailable
        self.devicePublished = devicePublished
        self.willRestoreDefaultOutput = willRestoreDefaultOutput
        self.isPriming = isPriming
        self.queuedFrames = queuedFrames
        self.queuedMilliseconds = queuedMilliseconds
        self.bufferFrames = bufferFrames
        self.targetBufferFrames = targetBufferFrames
        self.targetBufferMilliseconds = targetBufferMilliseconds
        self.targetIOFrames = targetIOFrames
        self.targetIOMilliseconds = targetIOMilliseconds
        self.droppedFrames = droppedFrames
        self.underruns = underruns
        self.sampleRate = sampleRate
        self.targetStartError = targetStartError
    }

    public static func parse(_ rawValue: String) -> DriverStatus {
        var values: [String: String] = [:]
        for item in rawValue.split(separator: ",") {
            let parts = item.split(separator: "=", maxSplits: 1)
            guard parts.count == 2 else { continue }
            values[String(parts[0])] = String(parts[1])
        }

        return DriverStatus(
            isRunning: (Int(values["running"] ?? "0") ?? 0) > 0,
            targetAlive: values["target"] == "yes",
            targetAvailable: values["available"] == "yes",
            devicePublished: values["published"] == "yes",
            willRestoreDefaultOutput: values["restoreDefault"] == "yes",
            isPriming: values["priming"] == "yes",
            queuedFrames: Int(values["queuedFrames"] ?? "0") ?? 0,
            queuedMilliseconds: Double(values["queuedMS"] ?? "0") ?? 0,
            bufferFrames: Int(values["bufferFrames"] ?? "0") ?? 0,
            targetBufferFrames: Int(values["targetBufferFrames"] ?? "0") ?? 0,
            targetBufferMilliseconds: Double(values["targetBufferMS"] ?? "0") ?? 0,
            targetIOFrames: Int(values["targetIOFrames"] ?? "0") ?? 0,
            targetIOMilliseconds: Double(values["targetIOMS"] ?? "0") ?? 0,
            droppedFrames: Int(values["dropped"] ?? "0") ?? 0,
            underruns: Int(values["underruns"] ?? "0") ?? 0,
            sampleRate: Double(values["sampleRate"] ?? "0") ?? 0,
            targetStartError: Int32(values["targetStartError"] ?? "0") ?? 0
        )
    }
}
