import Foundation

public struct DriverStatus: Equatable, Sendable {
    public var isRunning: Bool
    public var targetAlive: Bool
    public var queuedFrames: Int
    public var queuedMilliseconds: Double
    public var bufferFrames: Int
    public var droppedFrames: Int
    public var underruns: Int
    public var sampleRate: Double

    public init(
        isRunning: Bool = false,
        targetAlive: Bool = false,
        queuedFrames: Int = 0,
        queuedMilliseconds: Double = 0,
        bufferFrames: Int = 0,
        droppedFrames: Int = 0,
        underruns: Int = 0,
        sampleRate: Double = 0
    ) {
        self.isRunning = isRunning
        self.targetAlive = targetAlive
        self.queuedFrames = queuedFrames
        self.queuedMilliseconds = queuedMilliseconds
        self.bufferFrames = bufferFrames
        self.droppedFrames = droppedFrames
        self.underruns = underruns
        self.sampleRate = sampleRate
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
            queuedFrames: Int(values["queuedFrames"] ?? "0") ?? 0,
            queuedMilliseconds: Double(values["queuedMS"] ?? "0") ?? 0,
            bufferFrames: Int(values["bufferFrames"] ?? "0") ?? 0,
            droppedFrames: Int(values["dropped"] ?? "0") ?? 0,
            underruns: Int(values["underruns"] ?? "0") ?? 0,
            sampleRate: Double(values["sampleRate"] ?? "0") ?? 0
        )
    }
}
