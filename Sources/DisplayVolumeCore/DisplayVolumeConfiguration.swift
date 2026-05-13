import Foundation

public struct DisplayVolumeConfiguration: Codable, Equatable, Sendable {
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

public struct DisplayVolumeConfigurationStore {
    public static let appIdentifier = "tech.soit.MacDisplayVolume"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func load() -> DisplayVolumeConfiguration {
        guard
            let data = defaults.data(forKey: Self.appIdentifier),
            let configuration = try? JSONDecoder().decode(DisplayVolumeConfiguration.self, from: data)
        else {
            return DisplayVolumeConfiguration()
        }
        return configuration
    }

    public func save(_ configuration: DisplayVolumeConfiguration) throws {
        let data = try JSONEncoder().encode(configuration)
        defaults.set(data, forKey: Self.appIdentifier)
    }
}
