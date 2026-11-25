import DisplayVolumeCore
import Testing

@Test
func configurationUsesDriverBackedDefaults() {
    let configuration = DisplayVolumeConfiguration()

    #expect(configuration.targetOutputDeviceUID == "")
    #expect(configuration.preferredBufferFrameSize == 128)
}

@Test
func driverStatusParsesPrimingState() {
    let status = DriverStatus.parse(
        "running=1,target=yes,available=yes,published=yes,restoreDefault=no,priming=yes,queuedFrames=512,queuedMS=10.67,bufferFrames=128,targetBufferFrames=512,targetBufferMS=10.67,targetIOFrames=256,targetIOMS=5.33,dropped=0,underruns=0,sampleRate=48000"
    )

    #expect(status.isRunning)
    #expect(status.targetAlive)
    #expect(status.targetAvailable)
    #expect(status.devicePublished)
    #expect(!status.willRestoreDefaultOutput)
    #expect(status.isPriming)
    #expect(status.queuedFrames == 512)
    #expect(status.targetBufferFrames == 512)
    #expect(status.targetIOFrames == 256)
    #expect(status.targetStartError == 0)
    #expect(!status.hasTargetStartError)
    #expect(status.targetStartErrorCodeDescription == nil)
}

@Test
func driverStatusParsesTargetStartError() {
    let status = DriverStatus.parse(
        "running=1,target=no,available=yes,published=yes,restoreDefault=no,priming=yes,queuedFrames=2048,queuedMS=42.67,bufferFrames=64,targetBufferFrames=0,targetBufferMS=0.00,targetIOFrames=0,targetIOMS=0.00,dropped=1024,underruns=0,sampleRate=48000,targetStartError=1937010544"
    )

    #expect(status.hasTargetStartError)
    #expect(status.targetStartError == 1937010544)
    #expect(status.targetStartErrorCodeDescription == "'stop' (1937010544)")
}

@Test
func driverStatusFormatsNonFourCCTargetStartError() {
    let status = DriverStatus(targetStartError: -50)

    #expect(status.hasTargetStartError)
    #expect(status.targetStartErrorCodeDescription == "-50")
}
