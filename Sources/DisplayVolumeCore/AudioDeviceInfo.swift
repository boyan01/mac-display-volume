import CoreAudio
import Foundation

public struct AudioDeviceInfo: Identifiable, Equatable, Sendable {
    public let id: AudioDeviceID
    public let uid: String
    public let name: String
    public let transportType: UInt32
    public let outputChannelCount: Int
    public let nominalSampleRate: Double

    public init(
        id: AudioDeviceID,
        uid: String,
        name: String,
        transportType: UInt32,
        outputChannelCount: Int,
        nominalSampleRate: Double
    ) {
        self.id = id
        self.uid = uid
        self.name = name
        self.transportType = transportType
        self.outputChannelCount = outputChannelCount
        self.nominalSampleRate = nominalSampleRate
    }
}
