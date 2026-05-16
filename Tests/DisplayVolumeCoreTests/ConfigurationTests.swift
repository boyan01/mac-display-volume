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
        "running=1,target=yes,priming=yes,queuedFrames=512,queuedMS=10.67,bufferFrames=128,targetBufferFrames=512,targetBufferMS=10.67,targetIOFrames=256,targetIOMS=5.33,dropped=0,underruns=0,sampleRate=48000"
    )

    #expect(status.isRunning)
    #expect(status.targetAlive)
    #expect(status.isPriming)
    #expect(status.queuedFrames == 512)
    #expect(status.targetBufferFrames == 512)
    #expect(status.targetIOFrames == 256)
}
