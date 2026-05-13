import DisplayVolumeCore
import Foundation
import Testing

@Test
func configurationRoundTripsThroughUserDefaults() throws {
    let suiteName = "tech.soit.MacDisplayVolume.tests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defer { defaults.removePersistentDomain(forName: suiteName) }

    let store = DisplayVolumeConfigurationStore(defaults: defaults)
    let configuration = DisplayVolumeConfiguration(
        targetOutputDeviceUID: "example-device",
        preferredBufferFrameSize: 64
    )

    try store.save(configuration)

    #expect(store.load() == configuration)
}
