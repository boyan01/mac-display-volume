import Foundation

public struct DisplayVolumeConfiguration: Codable, Equatable, Sendable {
    public static let supportedBufferFrameSizes = [64, 128, 256]

    public var targetOutputDeviceUID: String
    public var preferredBufferFrameSize: Int

    public init(
        targetOutputDeviceUID: String = "",
        preferredBufferFrameSize: Int = 128
    ) {
        self.targetOutputDeviceUID = targetOutputDeviceUID
        self.preferredBufferFrameSize = preferredBufferFrameSize
    }
}
