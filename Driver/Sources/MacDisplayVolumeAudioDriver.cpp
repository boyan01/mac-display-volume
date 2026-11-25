#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr AudioObjectID kObjectIDPlugIn = kAudioObjectPlugInObject;
constexpr AudioObjectID kObjectIDBox = 2;
constexpr AudioObjectID kObjectIDDevice = 3;
constexpr AudioObjectID kObjectIDStreamOutput = 4;
constexpr AudioObjectID kObjectIDVolumeLeft = 5;
constexpr AudioObjectID kObjectIDVolumeRight = 6;
constexpr AudioObjectID kObjectIDMute = 7;

constexpr AudioObjectPropertySelector kPropertyTargetUID = 'tgud';
constexpr AudioObjectPropertySelector kPropertyBufferFrameSize = 'bfsz';
constexpr AudioObjectPropertySelector kPropertyStatus = 'stat';
constexpr AudioObjectPropertySelector kPropertyReset = 'rset';
constexpr UInt64 kChangeActionSetSampleRate = 1;

constexpr UInt32 kChannels = 2;
constexpr UInt32 kBytesPerChannel = sizeof(Float32);
constexpr UInt32 kBytesPerFrame = kChannels * kBytesPerChannel;
constexpr UInt32 kRingCapacityFrames = 8192;
constexpr UInt32 kMaxQueuedFrames = 2048;
constexpr UInt32 kDefaultBufferFrames = 128;
constexpr UInt32 kZeroTimestampPeriodFrames = 16384;
constexpr int64_t kTargetRecoveryDelayNanos = 1000000000;
constexpr int64_t kMaxTargetRecoveryDelayNanos = 30000000000;
constexpr int64_t kTargetIdleStopDelayNanos = 2000000000;
constexpr int64_t kDefaultOutputRestoreDelayNanos = 250000000;
constexpr UInt32 kDefaultOutputRestoreAttempts = 8;
constexpr UInt64 kTargetStallRecoveryNanos = 1000000000;
constexpr Float64 kDefaultSampleRate = 48000.0;
constexpr Float32 kMinDB = -60.0f;
constexpr Float32 kMaxDB = 0.0f;

AudioServerPlugInHostRef gHost = nullptr;
UInt32 gRefCount = 1;
extern AudioServerPlugInDriverRef gDriverRef;

std::mutex gStateMutex;
dispatch_queue_t gTargetQueue = dispatch_queue_create("tech.soit.MacDisplayVolume.target", DISPATCH_QUEUE_SERIAL);
std::array<std::atomic<Float32>, kRingCapacityFrames * kChannels> gRing {};
std::atomic<UInt64> gReadPosition { 0 };
std::atomic<UInt64> gWritePosition { 0 };
std::atomic<UInt32> gRunningClients { 0 };
std::atomic<UInt32> gPreferredBufferFrames { kDefaultBufferFrames };
std::atomic<Float64> gSampleRate { kDefaultSampleRate };
std::atomic<Float64> gRequestedSampleRate { kDefaultSampleRate };
std::atomic<Float32> gVolumeLeft { 1.0f };
std::atomic<Float32> gVolumeRight { 1.0f };
std::atomic_bool gMute { false };
std::atomic_bool gStreamOutputActive { true };
std::atomic_bool gRelayPriming { true };
std::atomic_bool gBoxAcquired { true };
std::atomic_bool gTargetAvailable { false };
std::atomic_bool gDevicePublished { false };
std::atomic_bool gRestoreDefaultOutput { false };
std::atomic_bool gRestoreDefaultSystemOutput { false };
std::atomic<UInt64> gZeroTimestampSeed { 1 };
std::atomic<UInt64> gAnchorHostTime { 0 };
std::atomic<Float64> gAnchorSampleTime { 0.0 };
std::atomic<UInt64> gFramesWritten { 0 };
std::atomic<UInt64> gFramesRead { 0 };
std::atomic<UInt64> gDroppedFrames { 0 };
std::atomic<UInt64> gUnderruns { 0 };
std::atomic<UInt64> gTargetStartGeneration { 0 };
std::atomic<UInt64> gTargetStopGeneration { 0 };
std::atomic_bool gTargetActive { false };
std::atomic_bool gTargetRecoveryScheduled { false };
std::atomic<UInt32> gTargetRecoveryFailures { 0 };
std::atomic<OSStatus> gTargetStartError { noErr };
std::atomic<UInt64> gLastTargetIOHostTime { 0 };
std::atomic<UInt32> gTargetBufferFrames { 0 };
std::atomic<UInt32> gTargetIOFrames { 0 };
AudioDeviceID gTargetDevice = kAudioObjectUnknown;
AudioDeviceIOProcID gTargetIOProcID = nullptr;
AudioDeviceID gObservedTargetDevice = kAudioObjectUnknown;
CFStringRef gTargetUID = nullptr;

struct StateSnapshot {
    UInt32 queuedFrames;
    UInt32 preferredBufferFrames;
    Float64 sampleRate;
    UInt64 droppedFrames;
    UInt64 underruns;
    bool targetAlive;
    bool targetAvailable;
    bool devicePublished;
    bool restoreDefaultOutput;
    bool relayPriming;
    UInt32 targetBufferFrames;
    UInt32 targetIOFrames;
    OSStatus targetStartError;
};

AudioObjectPropertyAddress MakeAddress(AudioObjectPropertySelector selector,
                                       AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                       AudioObjectPropertyElement element = kAudioObjectPropertyElementMain) {
    return AudioObjectPropertyAddress { selector, scope, element };
}

std::array<char, 5> SelectorString(AudioObjectPropertySelector selector) {
    return {
        static_cast<char>((selector >> 24) & 0xff),
        static_cast<char>((selector >> 16) & 0xff),
        static_cast<char>((selector >> 8) & 0xff),
        static_cast<char>(selector & 0xff),
        '\0',
    };
}

void LogUnknownProperty(const char *operation, AudioObjectID objectID, const AudioObjectPropertyAddress *address) {
    if (address == nullptr) {
        return;
    }
    auto selector = SelectorString(address->mSelector);
    auto scope = SelectorString(address->mScope);
    os_log_error(
        OS_LOG_DEFAULT,
        "MDV %{public}s unknown property object=%u selector=%{public}s scope=%{public}s element=%u",
        operation,
        objectID,
        selector.data(),
        scope.data(),
        address->mElement
    );
}

bool IsEmptyString(CFStringRef value) {
    return value == nullptr || CFStringGetLength(value) == 0;
}

bool IsVirtualDeviceUID(CFStringRef value) {
    return value != nullptr && CFStringCompare(value, CFSTR("tech.soit.MacDisplayVolume.Device"), 0) == kCFCompareEqualTo;
}

bool StringValuesEqual(CFStringRef lhs, CFStringRef rhs) {
    if (IsEmptyString(lhs) && IsEmptyString(rhs)) {
        return true;
    }
    return lhs != nullptr && rhs != nullptr && CFStringCompare(lhs, rhs, 0) == kCFCompareEqualTo;
}

bool IsSupportedTargetSampleRate(Float64 sampleRate) {
    return std::abs(sampleRate - kDefaultSampleRate) < 1.0;
}

bool HasSupportedStreamShape(const AudioStreamBasicDescription &format) {
    return format.mFormatID == kAudioFormatLinearPCM &&
           format.mFormatFlags == static_cast<AudioFormatFlags>(
               static_cast<UInt32>(kAudioFormatFlagIsFloat) |
               static_cast<UInt32>(kAudioFormatFlagsNativeEndian) |
               static_cast<UInt32>(kAudioFormatFlagIsPacked)
           ) &&
           format.mBytesPerPacket == kBytesPerFrame &&
           format.mFramesPerPacket == 1 &&
           format.mBytesPerFrame == kBytesPerFrame &&
           format.mChannelsPerFrame == kChannels &&
           format.mBitsPerChannel == kBytesPerChannel * 8;
}

bool HasValidSampleTime(const AudioTimeStamp &timeStamp) {
    return (timeStamp.mFlags & kAudioTimeStampSampleTimeValid) != 0;
}

bool IsLateWriteMix(const AudioServerPlugInIOCycleInfo *cycleInfo, UInt32 frameCount) {
    if (cycleInfo == nullptr ||
        !HasValidSampleTime(cycleInfo->mCurrentTime) ||
        !HasValidSampleTime(cycleInfo->mOutputTime)) {
        return false;
    }

    return cycleInfo->mCurrentTime.mSampleTime >
           cycleInfo->mOutputTime.mSampleTime + static_cast<Float64>(frameCount);
}

CFUUIDRef CreateFactoryUUID() {
    return CFUUIDCreateFromString(nullptr, CFSTR("8D4899D8-BD44-4D54-A6B0-6F2971B9B934"));
}

AudioClassID ClassIDForObject(AudioObjectID objectID) {
    switch (objectID) {
    case kObjectIDPlugIn:
        return kAudioPlugInClassID;
    case kObjectIDBox:
        return kAudioBoxClassID;
    case kObjectIDDevice:
        return kAudioDeviceClassID;
    case kObjectIDStreamOutput:
        return kAudioStreamClassID;
    case kObjectIDVolumeLeft:
    case kObjectIDVolumeRight:
        return kAudioVolumeControlClassID;
    case kObjectIDMute:
        return kAudioMuteControlClassID;
    default:
        return kAudioObjectClassID;
    }
}

AudioClassID BaseClassIDForObject(AudioObjectID objectID) {
    switch (objectID) {
    case kObjectIDPlugIn:
    case kObjectIDBox:
    case kObjectIDDevice:
    case kObjectIDStreamOutput:
        return kAudioObjectClassID;
    case kObjectIDVolumeLeft:
    case kObjectIDVolumeRight:
        return kAudioLevelControlClassID;
    case kObjectIDMute:
        return kAudioBooleanControlClassID;
    default:
        return kAudioObjectClassID;
    }
}

UInt32 WriteObjectIDs(void *outData, UInt32 inDataSize, const AudioObjectID *ids, UInt32 count) {
    UInt32 writableCount = std::min(count, inDataSize / static_cast<UInt32>(sizeof(AudioObjectID)));
    if (writableCount > 0) {
        std::memcpy(outData, ids, writableCount * sizeof(AudioObjectID));
    }
    return writableCount * sizeof(AudioObjectID);
}

AudioStreamBasicDescription StreamFormatForSampleRate(Float64 sampleRate) {
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = static_cast<AudioFormatFlags>(
        static_cast<UInt32>(kAudioFormatFlagIsFloat) |
        static_cast<UInt32>(kAudioFormatFlagsNativeEndian) |
        static_cast<UInt32>(kAudioFormatFlagIsPacked)
    );
    format.mBytesPerPacket = kBytesPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = kBytesPerChannel * 8;
    return format;
}

Float32 ScalarToDB(Float32 scalar) {
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    if (scalar <= 0.0f) {
        return kMinDB;
    }
    return ((scalar * scalar) * (kMaxDB - kMinDB)) + kMinDB;
}

Float32 DBToScalar(Float32 db) {
    db = std::clamp(db, kMinDB, kMaxDB);
    return std::sqrt((db - kMinDB) / (kMaxDB - kMinDB));
}

void Notify(AudioObjectID objectID,
            AudioObjectPropertySelector selector,
            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
            AudioObjectPropertyElement element = kAudioObjectPropertyElementMain) {
    if (gHost == nullptr) {
        return;
    }
    AudioObjectPropertyAddress address = MakeAddress(selector, scope, element);
    gHost->PropertiesChanged(gHost, objectID, 1, &address);
}

void ReanchorTimelineLocked() {
    gZeroTimestampSeed.fetch_add(1, std::memory_order_relaxed);
    gAnchorHostTime.store(AudioGetCurrentHostTime(), std::memory_order_release);
    gAnchorSampleTime.store(0.0, std::memory_order_release);
}

void FlushRingLocked(bool resetHealth = false) {
    UInt64 writePosition = gWritePosition.load(std::memory_order_acquire);
    gReadPosition.store(writePosition, std::memory_order_release);
    gRelayPriming.store(true, std::memory_order_release);
    gFramesWritten.store(0, std::memory_order_relaxed);
    gFramesRead.store(0, std::memory_order_relaxed);
    if (resetHealth) {
        gDroppedFrames.store(0, std::memory_order_relaxed);
        gUnderruns.store(0, std::memory_order_relaxed);
        gTargetStartError.store(noErr, std::memory_order_release);
        gTargetRecoveryFailures.store(0, std::memory_order_release);
    }
    ReanchorTimelineLocked();
}

UInt64 AdvanceReadPositionAtLeast(UInt64 minimumReadPosition) {
    UInt64 current = gReadPosition.load(std::memory_order_acquire);
    while (current < minimumReadPosition) {
        if (gReadPosition.compare_exchange_weak(
                current,
                minimumReadPosition,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return minimumReadPosition - current;
        }
    }
    return 0;
}

UInt32 QueuedFrames() {
    UInt64 writePosition = gWritePosition.load(std::memory_order_acquire);
    UInt64 readPosition = gReadPosition.load(std::memory_order_acquire);
    if (writePosition <= readPosition) {
        return 0;
    }
    return static_cast<UInt32>(std::min<UInt64>(writePosition - readPosition, std::numeric_limits<UInt32>::max()));
}

UInt32 PrebufferFramesFor(UInt32 requestedFrames) {
    UInt32 preferredFrames = gPreferredBufferFrames.load(std::memory_order_relaxed);
    UInt32 preferredPrebuffer = std::clamp<UInt32>(preferredFrames * 4, 512, kMaxQueuedFrames / 2);
    return std::min<UInt32>(kMaxQueuedFrames, std::max(preferredPrebuffer, requestedFrames));
}

void ScheduleTargetRecovery();

int64_t TargetRecoveryDelayNanos() {
    UInt32 failures = gTargetRecoveryFailures.load(std::memory_order_acquire);
    if (failures == 0) {
        return kTargetRecoveryDelayNanos;
    }
    UInt32 shift = std::min<UInt32>(failures, 5);
    int64_t delay = kTargetRecoveryDelayNanos << shift;
    return std::min<int64_t>(delay, kMaxTargetRecoveryDelayNanos);
}

bool TargetNeedsRecovery() {
    if (!gTargetAvailable.load(std::memory_order_acquire) ||
        gRunningClients.load(std::memory_order_acquire) == 0) {
        return false;
    }

    if (!gTargetActive.load(std::memory_order_acquire)) {
        return gTargetStartError.load(std::memory_order_acquire) != noErr;
    }

    UInt64 lastTargetIOHostTime = gLastTargetIOHostTime.load(std::memory_order_acquire);
    if (lastTargetIOHostTime == 0) {
        return false;
    }

    UInt64 currentHostTime = AudioGetCurrentHostTime();
    if (currentHostTime <= lastTargetIOHostTime) {
        return false;
    }

    UInt64 elapsedNanos = AudioConvertHostTimeToNanos(currentHostTime - lastTargetIOHostTime);
    return elapsedNanos >= kTargetStallRecoveryNanos;
}

void StoreFrames(const Float32 *input, UInt32 frames) {
    if (input == nullptr || frames == 0) {
        return;
    }

    if (frames > kRingCapacityFrames) {
        UInt32 skippedFrames = frames - kRingCapacityFrames;
        input += skippedFrames * kChannels;
        frames = kRingCapacityFrames;
        gDroppedFrames.fetch_add(skippedFrames, std::memory_order_relaxed);
    }

    UInt64 writePosition = gWritePosition.load(std::memory_order_relaxed);
    for (UInt32 frame = 0; frame < frames; ++frame) {
        UInt64 absoluteFrame = writePosition + frame;
        UInt32 ringFrame = static_cast<UInt32>(absoluteFrame % kRingCapacityFrames);
        gRing[(ringFrame * kChannels) + 0].store(input[(frame * kChannels) + 0], std::memory_order_relaxed);
        gRing[(ringFrame * kChannels) + 1].store(input[(frame * kChannels) + 1], std::memory_order_relaxed);
    }

    UInt64 afterWritePosition = writePosition + frames;
    gWritePosition.store(afterWritePosition, std::memory_order_release);
    gFramesWritten.fetch_add(frames, std::memory_order_relaxed);

    if (afterWritePosition > kMaxQueuedFrames) {
        UInt64 minimumReadPosition = afterWritePosition - kMaxQueuedFrames;
        UInt64 advancedFrames = AdvanceReadPositionAtLeast(minimumReadPosition);
        if (advancedFrames > 0) {
            gDroppedFrames.fetch_add(advancedFrames, std::memory_order_relaxed);
            if (TargetNeedsRecovery()) {
                ScheduleTargetRecovery();
            }
        }
    }
}

UInt32 FetchFrames(Float32 *output, UInt32 frames) {
    if (output == nullptr || frames == 0) {
        return 0;
    }

    UInt64 readPosition = gReadPosition.load(std::memory_order_acquire);
    UInt64 writePosition = gWritePosition.load(std::memory_order_acquire);
    UInt64 availableFrames = writePosition > readPosition ? writePosition - readPosition : 0;
    UInt32 prebufferFrames = PrebufferFramesFor(frames);
    bool wasPriming = gRelayPriming.load(std::memory_order_acquire);
    if (wasPriming && availableFrames < prebufferFrames) {
        std::memset(output, 0, frames * kBytesPerFrame);
        return 0;
    }

    if (wasPriming) {
        gRelayPriming.store(false, std::memory_order_release);
    }

    UInt32 produced = std::min<UInt32>(frames, static_cast<UInt32>(std::min<UInt64>(availableFrames, std::numeric_limits<UInt32>::max())));
    bool muted = gMute.load(std::memory_order_relaxed);
    Float32 gainLeft = muted ? 0.0f : gVolumeLeft.load(std::memory_order_relaxed);
    Float32 gainRight = muted ? 0.0f : gVolumeRight.load(std::memory_order_relaxed);

    for (UInt32 frame = 0; frame < produced; ++frame) {
        UInt64 absoluteFrame = readPosition + frame;
        UInt32 ringFrame = static_cast<UInt32>(absoluteFrame % kRingCapacityFrames);
        output[(frame * kChannels) + 0] = gRing[(ringFrame * kChannels) + 0].load(std::memory_order_relaxed) * gainLeft;
        output[(frame * kChannels) + 1] = gRing[(ringFrame * kChannels) + 1].load(std::memory_order_relaxed) * gainRight;
    }

    if (produced < frames) {
        std::memset(output + (produced * kChannels), 0, (frames - produced) * kBytesPerFrame);
        gRelayPriming.store(true, std::memory_order_release);
        if (gRunningClients.load(std::memory_order_relaxed) > 0) {
            gUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    UInt64 consumedPosition = readPosition + produced;
    AdvanceReadPositionAtLeast(consumedPosition);
    gFramesRead.fetch_add(produced, std::memory_order_relaxed);
    return produced;
}

void FillOutputBufferList(AudioBufferList *outOutputData, UInt32 frames) {
    if (outOutputData == nullptr) {
        return;
    }

    std::array<Float32, 4096 * kChannels> scratch {};
    UInt32 remaining = frames;
    UInt32 offset = 0;

    while (remaining > 0) {
        UInt32 chunk = std::min<UInt32>(remaining, 4096);
        FetchFrames(scratch.data(), chunk);

        for (UInt32 bufferIndex = 0; bufferIndex < outOutputData->mNumberBuffers; ++bufferIndex) {
            AudioBuffer &buffer = outOutputData->mBuffers[bufferIndex];
            if (buffer.mData == nullptr) {
                continue;
            }

            UInt32 bufferChannels = std::max<UInt32>(buffer.mNumberChannels, 1);
            UInt32 bufferFrames = static_cast<UInt32>(buffer.mDataByteSize / (sizeof(Float32) * bufferChannels));
            if (offset >= bufferFrames) {
                continue;
            }
            UInt32 writableFrames = std::min<UInt32>(chunk, bufferFrames - offset);
            Float32 *target = static_cast<Float32 *>(buffer.mData) + (offset * bufferChannels);

            if (outOutputData->mNumberBuffers == 1 && bufferChannels >= 2) {
                for (UInt32 frame = 0; frame < writableFrames; ++frame) {
                    target[(frame * bufferChannels) + 0] = scratch[(frame * kChannels) + 0];
                    target[(frame * bufferChannels) + 1] = scratch[(frame * kChannels) + 1];
                    for (UInt32 channel = 2; channel < bufferChannels; ++channel) {
                        target[(frame * bufferChannels) + channel] = 0.0f;
                    }
                }
            } else {
                UInt32 sourceChannel = std::min<UInt32>(bufferIndex, 1);
                for (UInt32 frame = 0; frame < writableFrames; ++frame) {
                    target[frame] = scratch[(frame * kChannels) + sourceChannel];
                }
            }
        }

        remaining -= chunk;
        offset += chunk;
    }
}

AudioDeviceID DeviceIDForUID(CFStringRef uid) {
    if (uid == nullptr) {
        return kAudioObjectUnknown;
    }

    AudioObjectPropertyAddress address = MakeAddress(kAudioHardwarePropertyTranslateUIDToDevice);
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);
    UInt32 qualifierSize = sizeof(uid);
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, qualifierSize, &uid, &size, &deviceID);
    return status == noErr ? deviceID : kAudioObjectUnknown;
}

Float64 DeviceNominalSampleRate(AudioDeviceID deviceID) {
    if (deviceID == kAudioObjectUnknown) {
        return 0.0;
    }

    AudioObjectPropertyAddress address = MakeAddress(kAudioDevicePropertyNominalSampleRate);
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &sampleRate);
    return status == noErr ? sampleRate : 0.0;
}

UInt32 DeviceBufferFrameSize(AudioDeviceID deviceID) {
    if (deviceID == kAudioObjectUnknown) {
        return 0;
    }

    AudioObjectPropertyAddress address = MakeAddress(kAudioDevicePropertyBufferFrameSize);
    UInt32 frames = 0;
    UInt32 size = sizeof(frames);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &frames);
    return status == noErr ? frames : 0;
}

UInt32 DeviceOutputChannelCount(AudioDeviceID deviceID) {
    if (deviceID == kAudioObjectUnknown) {
        return 0;
    }

    AudioObjectPropertyAddress address = MakeAddress(
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput
    );
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(deviceID, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }

    std::vector<std::byte> storage(size);
    if (AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, storage.data()) != noErr) {
        return 0;
    }

    UInt32 channels = 0;
    const AudioBufferList *buffers = reinterpret_cast<const AudioBufferList *>(storage.data());
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
        channels += buffers->mBuffers[index].mNumberChannels;
    }
    return channels;
}

bool DeviceIsAlive(AudioDeviceID deviceID) {
    if (deviceID == kAudioObjectUnknown) {
        return false;
    }

    AudioObjectPropertyAddress address = MakeAddress(kAudioDevicePropertyDeviceIsAlive);
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    return AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &alive) == noErr && alive != 0;
}

AudioDeviceID SystemDevice(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address = MakeAddress(selector);
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &deviceID) == noErr
        ? deviceID
        : kAudioObjectUnknown;
}

bool SetSystemDevice(AudioObjectPropertySelector selector, AudioDeviceID deviceID) {
    AudioObjectPropertyAddress address = MakeAddress(selector);
    UInt32 size = sizeof(deviceID);
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, size, &deviceID) == noErr;
}

void SaveTargetUID(CFStringRef uid) {
    if (gHost != nullptr) {
        gHost->WriteToStorage(
            gHost,
            CFSTR("targetOutputDeviceUID"),
            uid != nullptr ? uid : CFSTR("")
        );
    }
}

void SavePreferredBuffer(UInt32 preferredBufferFrames) {
    if (gHost != nullptr) {
        CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &preferredBufferFrames);
        gHost->WriteToStorage(gHost, CFSTR("preferredBufferFrameSize"), number);
        CFRelease(number);
    }
}

void SaveDefaultOutputRestoreIntent() {
    if (gHost == nullptr) {
        return;
    }
    gHost->WriteToStorage(
        gHost,
        CFSTR("restoreDefaultOutput"),
        gRestoreDefaultOutput.load(std::memory_order_acquire) ? kCFBooleanTrue : kCFBooleanFalse
    );
    gHost->WriteToStorage(
        gHost,
        CFSTR("restoreDefaultSystemOutput"),
        gRestoreDefaultSystemOutput.load(std::memory_order_acquire) ? kCFBooleanTrue : kCFBooleanFalse
    );
}

bool LoadStoredBoolean(CFStringRef key) {
    if (gHost == nullptr) {
        return false;
    }

    CFPropertyListRef data = nullptr;
    bool value = false;
    if (gHost->CopyFromStorage(gHost, key, &data) == noErr && data != nullptr) {
        if (CFGetTypeID(data) == CFBooleanGetTypeID()) {
            value = CFBooleanGetValue(static_cast<CFBooleanRef>(data));
        }
        CFRelease(data);
    }
    return value;
}

void LoadConfigurationFromStorage() {
    if (gHost == nullptr) {
        return;
    }

    CFPropertyListRef data = nullptr;
    if (gHost->CopyFromStorage(gHost, CFSTR("targetOutputDeviceUID"), &data) == noErr && data != nullptr) {
        if (CFGetTypeID(data) == CFStringGetTypeID()) {
            CFStringRef storedUID = static_cast<CFStringRef>(data);
            std::lock_guard<std::mutex> lock(gStateMutex);
            if (gTargetUID != nullptr) {
                CFRelease(gTargetUID);
            }
            gTargetUID = IsVirtualDeviceUID(storedUID)
                ? CFStringCreateWithCString(nullptr, "", kCFStringEncodingUTF8)
                : CFStringCreateCopy(nullptr, storedUID);
        }
        CFRelease(data);
    }

    data = nullptr;
    if (gHost->CopyFromStorage(gHost, CFSTR("preferredBufferFrameSize"), &data) == noErr && data != nullptr) {
        if (CFGetTypeID(data) == CFNumberGetTypeID()) {
            SInt32 value = 0;
            if (CFNumberGetValue(static_cast<CFNumberRef>(data), kCFNumberSInt32Type, &value)) {
                std::lock_guard<std::mutex> lock(gStateMutex);
                gPreferredBufferFrames.store(
                    std::clamp<UInt32>(static_cast<UInt32>(std::max<SInt32>(value, 0)), 64, 256),
                    std::memory_order_release
                );
            }
        }
        CFRelease(data);
    }

    gRestoreDefaultOutput.store(LoadStoredBoolean(CFSTR("restoreDefaultOutput")), std::memory_order_release);
    gRestoreDefaultSystemOutput.store(
        LoadStoredBoolean(CFSTR("restoreDefaultSystemOutput")),
        std::memory_order_release
    );
}

OSStatus TargetDeviceIOProc(AudioObjectID,
                            const AudioTimeStamp *,
                            const AudioBufferList *,
                            const AudioTimeStamp *,
                            AudioBufferList *outOutputData,
                            const AudioTimeStamp *,
                            void *) {
    gLastTargetIOHostTime.store(AudioGetCurrentHostTime(), std::memory_order_release);
    UInt32 frames = 0;
    if (outOutputData != nullptr && outOutputData->mNumberBuffers > 0 && outOutputData->mBuffers[0].mDataByteSize > 0) {
        UInt32 channels = std::max<UInt32>(outOutputData->mBuffers[0].mNumberChannels, 1);
        frames = outOutputData->mBuffers[0].mDataByteSize / (sizeof(Float32) * channels);
    }
    gTargetIOFrames.store(frames, std::memory_order_release);
    FillOutputBufferList(outOutputData, frames);
    return noErr;
}

void StopTargetDevice() {
    AudioDeviceID targetDevice = kAudioObjectUnknown;
    AudioDeviceIOProcID targetIOProcID = nullptr;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        targetDevice = gTargetDevice;
        targetIOProcID = gTargetIOProcID;
        gTargetDevice = kAudioObjectUnknown;
        gTargetIOProcID = nullptr;
        gTargetActive.store(false, std::memory_order_release);
        gTargetBufferFrames.store(0, std::memory_order_release);
        gTargetIOFrames.store(0, std::memory_order_release);
    }

    if (targetDevice != kAudioObjectUnknown && targetIOProcID != nullptr) {
        AudioDeviceStop(targetDevice, targetIOProcID);
        AudioDeviceDestroyIOProcID(targetDevice, targetIOProcID);
    }
    gTargetBufferFrames.store(0, std::memory_order_release);
    gTargetIOFrames.store(0, std::memory_order_release);
}

void StartTargetDevice() {
    StopTargetDevice();

    CFStringRef targetUID = nullptr;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        targetUID = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : nullptr;
    }

    if (targetUID == nullptr || IsEmptyString(targetUID) || IsVirtualDeviceUID(targetUID)) {
        if (targetUID != nullptr) {
            CFRelease(targetUID);
        }
        return;
    }
    AudioDeviceID target = DeviceIDForUID(targetUID);
    CFRelease(targetUID);

    if (!gTargetAvailable.load(std::memory_order_acquire) || target == kAudioObjectUnknown) {
        return;
    }
    if (!IsSupportedTargetSampleRate(DeviceNominalSampleRate(target))) {
        return;
    }
    UInt32 targetBufferFrames = DeviceBufferFrameSize(target);

    AudioDeviceIOProcID ioProcID = nullptr;
    OSStatus createStatus = AudioDeviceCreateIOProcID(target, TargetDeviceIOProc, nullptr, &ioProcID);
    if (createStatus != noErr) {
        gTargetStartError.store(createStatus, std::memory_order_release);
        gTargetRecoveryFailures.fetch_add(1, std::memory_order_acq_rel);
        os_log_error(
            OS_LOG_DEFAULT,
            "MDV AudioDeviceCreateIOProcID failed for target=%u status=%d",
            target,
            static_cast<int>(createStatus)
        );
        Notify(kObjectIDBox, kPropertyStatus);
        if (gDevicePublished.load(std::memory_order_acquire)) {
            Notify(kObjectIDDevice, kPropertyStatus);
        }
        return;
    }

    OSStatus startStatus = AudioDeviceStart(target, ioProcID);
    if (startStatus != noErr) {
        AudioDeviceDestroyIOProcID(target, ioProcID);
        gTargetStartError.store(startStatus, std::memory_order_release);
        gTargetRecoveryFailures.fetch_add(1, std::memory_order_acq_rel);
        os_log_error(
            OS_LOG_DEFAULT,
            "MDV AudioDeviceStart failed for target=%u status=%d",
            target,
            static_cast<int>(startStatus)
        );
        Notify(kObjectIDBox, kPropertyStatus);
        if (gDevicePublished.load(std::memory_order_acquire)) {
            Notify(kObjectIDDevice, kPropertyStatus);
        }
        return;
    }

    bool shouldStop = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gRunningClients.load(std::memory_order_relaxed) > 0) {
            gTargetDevice = target;
            gTargetIOProcID = ioProcID;
            gLastTargetIOHostTime.store(AudioGetCurrentHostTime(), std::memory_order_release);
            gTargetBufferFrames.store(targetBufferFrames, std::memory_order_release);
            gTargetIOFrames.store(0, std::memory_order_release);
            gTargetStartError.store(noErr, std::memory_order_release);
            gTargetRecoveryFailures.store(0, std::memory_order_release);
            gTargetActive.store(true, std::memory_order_release);
        } else {
            shouldStop = true;
        }
    }
    if (shouldStop) {
        AudioDeviceStop(target, ioProcID);
        AudioDeviceDestroyIOProcID(target, ioProcID);
    } else {
        Notify(kObjectIDBox, kPropertyStatus);
        if (gDevicePublished.load(std::memory_order_acquire)) {
            Notify(kObjectIDDevice, kPropertyStatus);
        }
    }
}

void CancelScheduledTargetStart() {
    gTargetStartGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void CancelScheduledTargetStop() {
    gTargetStopGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void ScheduleStartTargetDevice() {
    CancelScheduledTargetStop();
    UInt64 generation = gTargetStartGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    dispatch_async(gTargetQueue, ^{
        if (gTargetStartGeneration.load(std::memory_order_acquire) != generation) {
            return;
        }
        if (gRunningClients.load(std::memory_order_acquire) == 0) {
            return;
        }
        StartTargetDevice();
    });
}

void ScheduleIdleStopTargetDevice() {
    CancelScheduledTargetStart();
    UInt64 generation = gTargetStopGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kTargetIdleStopDelayNanos), gTargetQueue, ^{
        if (gTargetStopGeneration.load(std::memory_order_acquire) != generation) {
            return;
        }
        if (gRunningClients.load(std::memory_order_acquire) > 0) {
            return;
        }
        StopTargetDevice();
        Notify(kObjectIDDevice, kPropertyStatus);
    });
}

void NotifyDevicePublicationChanged() {
    Notify(kObjectIDBox, kAudioBoxPropertyDeviceList);
    Notify(kObjectIDPlugIn, kAudioPlugInPropertyDeviceList);
    Notify(kObjectIDPlugIn, kAudioObjectPropertyOwnedObjects);
    Notify(kObjectIDBox, kPropertyStatus);
}

void CaptureDefaultOutputRestoreIntent() {
    AudioDeviceID virtualDevice = DeviceIDForUID(CFSTR("tech.soit.MacDisplayVolume.Device"));
    gRestoreDefaultOutput.store(
        virtualDevice != kAudioObjectUnknown &&
            SystemDevice(kAudioHardwarePropertyDefaultOutputDevice) == virtualDevice,
        std::memory_order_release
    );
    gRestoreDefaultSystemOutput.store(
        virtualDevice != kAudioObjectUnknown &&
            SystemDevice(kAudioHardwarePropertyDefaultSystemOutputDevice) == virtualDevice,
        std::memory_order_release
    );
    SaveDefaultOutputRestoreIntent();
}

void ScheduleRestoreDefaultOutputs(UInt32 attempt = 0) {
    if (!gRestoreDefaultOutput.load(std::memory_order_acquire) &&
        !gRestoreDefaultSystemOutput.load(std::memory_order_acquire)) {
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDefaultOutputRestoreDelayNanos), gTargetQueue, ^{
        if (!gDevicePublished.load(std::memory_order_acquire)) {
            return;
        }

        AudioDeviceID virtualDevice = DeviceIDForUID(CFSTR("tech.soit.MacDisplayVolume.Device"));
        bool changed = false;
        if (virtualDevice != kAudioObjectUnknown &&
            gRestoreDefaultOutput.load(std::memory_order_acquire) &&
            SetSystemDevice(kAudioHardwarePropertyDefaultOutputDevice, virtualDevice)) {
            gRestoreDefaultOutput.store(false, std::memory_order_release);
            changed = true;
        }
        if (virtualDevice != kAudioObjectUnknown &&
            gRestoreDefaultSystemOutput.load(std::memory_order_acquire) &&
            SetSystemDevice(kAudioHardwarePropertyDefaultSystemOutputDevice, virtualDevice)) {
            gRestoreDefaultSystemOutput.store(false, std::memory_order_release);
            changed = true;
        }
        if (changed) {
            SaveDefaultOutputRestoreIntent();
            Notify(kObjectIDBox, kPropertyStatus);
        }

        if (attempt + 1 < kDefaultOutputRestoreAttempts &&
            (gRestoreDefaultOutput.load(std::memory_order_acquire) ||
             gRestoreDefaultSystemOutput.load(std::memory_order_acquire))) {
            ScheduleRestoreDefaultOutputs(attempt + 1);
        }
    });
}

void UpdateDevicePublication(bool captureRestoreIntent) {
    bool shouldPublish = gBoxAcquired.load(std::memory_order_acquire) &&
                         gTargetAvailable.load(std::memory_order_acquire);
    bool wasPublished = gDevicePublished.exchange(shouldPublish, std::memory_order_acq_rel);
    if (wasPublished == shouldPublish) {
        return;
    }

    if (!shouldPublish) {
        if (captureRestoreIntent) {
            CaptureDefaultOutputRestoreIntent();
        }
        CancelScheduledTargetStart();
        StopTargetDevice();
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            FlushRingLocked();
        }
    }

    NotifyDevicePublicationChanged();
    if (shouldPublish) {
        ScheduleRestoreDefaultOutputs();
        if (gRunningClients.load(std::memory_order_acquire) > 0) {
            ScheduleStartTargetDevice();
        }
    }
}

void SetTargetAvailability(bool available) {
    bool wasAvailable = gTargetAvailable.exchange(available, std::memory_order_acq_rel);
    if (wasAvailable == available) {
        return;
    }
    gTargetStartError.store(noErr, std::memory_order_release);
    gTargetRecoveryFailures.store(0, std::memory_order_release);
    UpdateDevicePublication(!available);
    Notify(kObjectIDBox, kPropertyStatus);
    if (gDevicePublished.load(std::memory_order_acquire)) {
        Notify(kObjectIDDevice, kAudioDevicePropertyDeviceIsAlive);
        Notify(kObjectIDDevice, kPropertyStatus);
    }
}

OSStatus TargetAvailabilityListener(AudioObjectID,
                                    UInt32,
                                    const AudioObjectPropertyAddress *,
                                    void *);

void SetTargetPropertyListeners(AudioDeviceID deviceID, bool enabled) {
    if (deviceID == kAudioObjectUnknown) {
        return;
    }

    std::array<AudioObjectPropertyAddress, 3> addresses {
        MakeAddress(kAudioDevicePropertyDeviceIsAlive),
        MakeAddress(kAudioDevicePropertyNominalSampleRate),
        MakeAddress(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput),
    };
    for (auto &address : addresses) {
        if (enabled) {
            AudioObjectAddPropertyListener(deviceID, &address, TargetAvailabilityListener, nullptr);
        } else {
            AudioObjectRemovePropertyListener(deviceID, &address, TargetAvailabilityListener, nullptr);
        }
    }
}

void EvaluateTargetAvailability() {
    CFStringRef targetUID = nullptr;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        targetUID = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : nullptr;
    }

    AudioDeviceID targetDevice = kAudioObjectUnknown;
    if (targetUID != nullptr && !IsEmptyString(targetUID) && !IsVirtualDeviceUID(targetUID)) {
        targetDevice = DeviceIDForUID(targetUID);
    }
    if (targetUID != nullptr) {
        CFRelease(targetUID);
    }

    if (targetDevice != gObservedTargetDevice) {
        SetTargetPropertyListeners(gObservedTargetDevice, false);
        gObservedTargetDevice = targetDevice;
        SetTargetPropertyListeners(gObservedTargetDevice, true);
    }

    bool available = targetDevice != kAudioObjectUnknown &&
                     DeviceIsAlive(targetDevice) &&
                     DeviceOutputChannelCount(targetDevice) > 0 &&
                     IsSupportedTargetSampleRate(DeviceNominalSampleRate(targetDevice));
    SetTargetAvailability(available);
}

OSStatus TargetAvailabilityListener(AudioObjectID,
                                    UInt32,
                                    const AudioObjectPropertyAddress *,
                                    void *) {
    dispatch_async(gTargetQueue, ^{
        EvaluateTargetAvailability();
    });
    return noErr;
}

void ScheduleTargetRecovery() {
    bool expected = false;
    if (!gTargetRecoveryScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    int64_t delayNanos = TargetRecoveryDelayNanos();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayNanos), gTargetQueue, ^{
        gTargetRecoveryScheduled.store(false, std::memory_order_release);
        if (!TargetNeedsRecovery()) {
            return;
        }
        StopTargetDevice();
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            FlushRingLocked();
        }
        StartTargetDevice();
        Notify(kObjectIDDevice, kPropertyStatus);
    });
}

StateSnapshot SnapshotLocked() {
    return StateSnapshot {
        QueuedFrames(),
        gPreferredBufferFrames.load(std::memory_order_acquire),
        gSampleRate.load(std::memory_order_acquire),
        gDroppedFrames.load(std::memory_order_acquire),
        gUnderruns.load(std::memory_order_acquire),
        gTargetActive.load(std::memory_order_acquire),
        gTargetAvailable.load(std::memory_order_acquire),
        gDevicePublished.load(std::memory_order_acquire),
        gRestoreDefaultOutput.load(std::memory_order_acquire) ||
            gRestoreDefaultSystemOutput.load(std::memory_order_acquire),
        gRelayPriming.load(std::memory_order_acquire),
        gTargetBufferFrames.load(std::memory_order_acquire),
        gTargetIOFrames.load(std::memory_order_acquire),
        gTargetStartError.load(std::memory_order_acquire),
    };
}

CFStringRef CopyStatusString() {
    std::lock_guard<std::mutex> lock(gStateMutex);
    StateSnapshot snapshot = SnapshotLocked();
    double queuedMS = snapshot.sampleRate > 0 ? (static_cast<double>(snapshot.queuedFrames) / snapshot.sampleRate) * 1000.0 : 0.0;
    double targetBufferMS = snapshot.sampleRate > 0 ? (static_cast<double>(snapshot.targetBufferFrames) / snapshot.sampleRate) * 1000.0 : 0.0;
    double targetIOMS = snapshot.sampleRate > 0 ? (static_cast<double>(snapshot.targetIOFrames) / snapshot.sampleRate) * 1000.0 : 0.0;
    char buffer[512];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "running=%u,target=%s,available=%s,published=%s,restoreDefault=%s,priming=%s,queuedFrames=%u,queuedMS=%.2f,bufferFrames=%u,targetBufferFrames=%u,targetBufferMS=%.2f,targetIOFrames=%u,targetIOMS=%.2f,dropped=%llu,underruns=%llu,sampleRate=%.0f,targetStartError=%d",
        gRunningClients.load(std::memory_order_acquire),
        snapshot.targetAlive ? "yes" : "no",
        snapshot.targetAvailable ? "yes" : "no",
        snapshot.devicePublished ? "yes" : "no",
        snapshot.restoreDefaultOutput ? "yes" : "no",
        snapshot.relayPriming ? "yes" : "no",
        snapshot.queuedFrames,
        queuedMS,
        snapshot.preferredBufferFrames,
        snapshot.targetBufferFrames,
        targetBufferMS,
        snapshot.targetIOFrames,
        targetIOMS,
        snapshot.droppedFrames,
        snapshot.underruns,
        snapshot.sampleRate,
        static_cast<int>(snapshot.targetStartError)
    );
    return CFStringCreateWithCString(nullptr, buffer, kCFStringEncodingUTF8);
}

bool IsStreamObject(AudioObjectID objectID) {
    return objectID == kObjectIDStreamOutput;
}

bool IsControlObject(AudioObjectID objectID) {
    return objectID == kObjectIDVolumeLeft || objectID == kObjectIDVolumeRight || objectID == kObjectIDMute;
}

Boolean HasPlugInProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyBundleID:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyBoxList:
    case kAudioPlugInPropertyTranslateUIDToBox:
        return true;
    default:
        return false;
    }
}

Boolean HasBoxProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyCustomPropertyInfoList:
    case kAudioBoxPropertyBoxUID:
    case kAudioBoxPropertyTransportType:
    case kAudioBoxPropertyHasAudio:
    case kAudioBoxPropertyHasVideo:
    case kAudioBoxPropertyHasMIDI:
    case kAudioBoxPropertyIsProtected:
    case kAudioBoxPropertyAcquired:
    case kAudioBoxPropertyAcquisitionFailed:
    case kAudioBoxPropertyDeviceList:
    case kPropertyTargetUID:
    case kPropertyBufferFrameSize:
    case kPropertyStatus:
    case kPropertyReset:
        return true;
    default:
        return false;
    }
}

Boolean HasDeviceProperty(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyCustomPropertyInfoList:
        return true;
    default:
        break;
    }

    switch (selector) {
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyPlugIn:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyClockAlgorithm:
    case kAudioDevicePropertyClockIsStable:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceIsRunningSomewhere:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyHogMode:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyActualSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertyStreamConfiguration:
        return scope == kAudioObjectPropertyScopeGlobal ||
               scope == kAudioObjectPropertyScopeInput ||
               scope == kAudioObjectPropertyScopeOutput;
    case kAudioDevicePropertyBufferFrameSize:
    case kAudioDevicePropertyBufferFrameSizeRange:
    case kAudioDevicePropertyUsesVariableBufferFrameSizes:
    case kAudioDevicePropertyIOCycleUsage:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
        return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput;
    case kPropertyTargetUID:
    case kPropertyBufferFrameSize:
    case kPropertyStatus:
    case kPropertyReset:
        return true;
    default:
        return false;
    }
}

Boolean HasStreamProperty(AudioObjectPropertySelector selector) {
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyElementName:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        return true;
    default:
        return false;
    }
}

Boolean HasControlProperty(AudioObjectID objectID, AudioObjectPropertySelector selector) {
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyElementName:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioControlPropertyScope:
    case kAudioControlPropertyElement:
        return true;
    case kAudioLevelControlPropertyScalarValue:
    case kAudioLevelControlPropertyDecibelValue:
    case kAudioLevelControlPropertyDecibelRange:
    case kAudioLevelControlPropertyConvertScalarToDecibels:
    case kAudioLevelControlPropertyConvertDecibelsToScalar:
        return objectID == kObjectIDVolumeLeft || objectID == kObjectIDVolumeRight;
    case kAudioBooleanControlPropertyValue:
        return objectID == kObjectIDMute;
    default:
        return false;
    }
}

HRESULT QueryInterface(void *driver, REFIID uuid, LPVOID *interface) {
    if (driver == nullptr || interface == nullptr) {
        return E_POINTER;
    }
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }

    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(nullptr, uuid);
    if (requestedUUID == nullptr) {
        *interface = nullptr;
        return E_NOINTERFACE;
    }
    if (CFEqual(requestedUUID, kAudioServerPlugInDriverInterfaceUUID) || CFEqual(requestedUUID, IUnknownUUID)) {
        *interface = driver;
        gRefCount += 1;
    } else {
        *interface = nullptr;
    }
    CFRelease(requestedUUID);
    return *interface == nullptr ? E_NOINTERFACE : S_OK;
}

ULONG AddRef(void *) {
    return ++gRefCount;
}

ULONG Release(void *) {
    if (gRefCount > 0) {
        gRefCount -= 1;
    }
    if (gRefCount == 0) {
        CFUUIDRef factoryUUID = CreateFactoryUUID();
        CFPlugInRemoveInstanceForFactory(factoryUUID);
        CFRelease(factoryUUID);
    }
    return gRefCount;
}

OSStatus Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    gHost = host;
    gRequestedSampleRate.store(kDefaultSampleRate, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        FlushRingLocked();
    }
    LoadConfigurationFromStorage();
    AudioObjectPropertyAddress devicesAddress = MakeAddress(kAudioHardwarePropertyDevices);
    AudioObjectAddPropertyListener(
        kAudioObjectSystemObject,
        &devicesAddress,
        TargetAvailabilityListener,
        nullptr
    );
    TargetAvailabilityListener(kAudioObjectSystemObject, 1, &devicesAddress, nullptr);
    return noErr;
}

OSStatus CreateDevice(AudioServerPlugInDriverRef driver, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus DestroyDevice(AudioServerPlugInDriverRef driver, AudioObjectID) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus AddDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, const AudioServerPlugInClientInfo *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadObjectError;
    }
    return noErr;
}

OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, const AudioServerPlugInClientInfo *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadObjectError;
    }
    return noErr;
}

OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, UInt64 action, void *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadObjectError;
    }

    switch (action) {
    case 0:
        return noErr;
    case kChangeActionSetSampleRate: {
        Float64 requestedSampleRate = gRequestedSampleRate.load(std::memory_order_acquire);
        if (!IsSupportedTargetSampleRate(requestedSampleRate)) {
            return kAudioHardwareIllegalOperationError;
        }

        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            gSampleRate.store(requestedSampleRate, std::memory_order_release);
            FlushRingLocked();
        }
        Notify(kObjectIDDevice, kPropertyStatus);
        return noErr;
    }
    default:
        return kAudioHardwareIllegalOperationError;
    }
}

OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, UInt64 action, void *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadObjectError;
    }
    if (action == kChangeActionSetSampleRate) {
        gRequestedSampleRate.store(gSampleRate.load(std::memory_order_acquire), std::memory_order_release);
    }
    return noErr;
}

Boolean HasProperty(AudioServerPlugInDriverRef driver,
                    AudioObjectID objectID,
                    pid_t,
                    const AudioObjectPropertyAddress *address) {
    if (driver != gDriverRef) {
        return false;
    }
    if (address == nullptr) {
        return false;
    }
    if (objectID == kObjectIDPlugIn) {
        return HasPlugInProperty(address->mSelector);
    }
    if (objectID == kObjectIDBox) {
        return HasBoxProperty(address->mSelector);
    }
    if (objectID == kObjectIDDevice) {
        return HasDeviceProperty(address->mSelector, address->mScope);
    }
    if (IsStreamObject(objectID)) {
        return HasStreamProperty(address->mSelector);
    }
    if (IsControlObject(objectID)) {
        return HasControlProperty(objectID, address->mSelector);
    }
    return false;
}

OSStatus IsPropertySettable(AudioServerPlugInDriverRef driver,
                            AudioObjectID objectID,
                            pid_t,
                            const AudioObjectPropertyAddress *address,
                            Boolean *isSettable) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (address == nullptr || isSettable == nullptr || !HasProperty(driver, objectID, 0, address)) {
        LogUnknownProperty("IsPropertySettable", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }

    *isSettable = false;
    if (objectID == kObjectIDBox) {
        *isSettable = address->mSelector == kAudioBoxPropertyAcquired ||
                      address->mSelector == kPropertyTargetUID ||
                      address->mSelector == kPropertyBufferFrameSize ||
                      address->mSelector == kPropertyReset;
    } else if (objectID == kObjectIDDevice) {
        switch (address->mSelector) {
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyBufferFrameSize:
        case kPropertyTargetUID:
        case kPropertyBufferFrameSize:
        case kPropertyReset:
            *isSettable = true;
            break;
        default:
            break;
        }
    } else if (objectID == kObjectIDStreamOutput) {
        switch (address->mSelector) {
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *isSettable = true;
            break;
        default:
            break;
        }
    } else if (objectID == kObjectIDVolumeLeft || objectID == kObjectIDVolumeRight) {
        *isSettable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
                      address->mSelector == kAudioLevelControlPropertyDecibelValue;
    } else if (objectID == kObjectIDMute) {
        *isSettable = address->mSelector == kAudioBooleanControlPropertyValue;
    }
    return noErr;
}

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef driver,
                             AudioObjectID objectID,
                             pid_t clientPID,
                             const AudioObjectPropertyAddress *address,
                             UInt32,
                             const void *,
                             UInt32 *outDataSize) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (address == nullptr || outDataSize == nullptr || !HasProperty(driver, objectID, clientPID, address)) {
        LogUnknownProperty("GetPropertyDataSize", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }

    switch (address->mSelector) {
    case kAudioObjectPropertyOwnedObjects:
        if (objectID == kObjectIDPlugIn) {
            *outDataSize = (gDevicePublished.load(std::memory_order_acquire) ? 2 : 1) * sizeof(AudioObjectID);
        } else if (objectID == kObjectIDDevice) {
            *outDataSize = 4 * sizeof(AudioObjectID);
        } else {
            *outDataSize = 0;
        }
        return noErr;
    case kAudioPlugInPropertyDeviceList:
    case kAudioBoxPropertyDeviceList:
        *outDataSize = gDevicePublished.load(std::memory_order_acquire) ? sizeof(AudioObjectID) : 0;
        return noErr;
    case kAudioPlugInPropertyBoxList:
        *outDataSize = sizeof(AudioObjectID);
        return noErr;
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyControlList:
        if (objectID == kObjectIDDevice && address->mSelector == kAudioObjectPropertyControlList) {
            *outDataSize = 3 * sizeof(AudioObjectID);
        } else {
            *outDataSize = sizeof(AudioObjectID);
        }
        return noErr;
    case kAudioObjectPropertyCustomPropertyInfoList:
        *outDataSize = 4 * sizeof(AudioServerPlugInCustomPropertyInfo);
        return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyElementName:
    case kAudioPlugInPropertyBundleID:
    case kAudioBoxPropertyBoxUID:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kPropertyTargetUID:
    case kPropertyStatus:
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *outDataSize = sizeof(AudioStreamBasicDescription);
        return noErr;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        *outDataSize = sizeof(AudioStreamRangedDescription);
        return noErr;
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyBufferFrameSizeRange:
    case kAudioLevelControlPropertyDecibelRange:
        *outDataSize = sizeof(AudioValueRange);
        return noErr;
    case kAudioDevicePropertyStreamConfiguration:
        *outDataSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer);
        return noErr;
    case kAudioDevicePropertyPreferredChannelLayout:
        *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (kChannels * sizeof(AudioChannelDescription));
        return noErr;
    case kPropertyBufferFrameSize:
    case kPropertyReset:
        *outDataSize = sizeof(CFPropertyListRef);
        return noErr;
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyActualSampleRate:
        *outDataSize = sizeof(Float64);
        return noErr;
    case kAudioDevicePropertyIOCycleUsage:
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioDevicePropertyHogMode:
        *outDataSize = sizeof(pid_t);
        return noErr;
    default:
        *outDataSize = sizeof(UInt32);
        return noErr;
    }
}

OSStatus GetPropertyData(AudioServerPlugInDriverRef driver,
                         AudioObjectID objectID,
                         pid_t clientPID,
                         const AudioObjectPropertyAddress *address,
                         UInt32 qualifierDataSize,
                         const void *qualifierData,
                         UInt32 inDataSize,
                         UInt32 *outDataSize,
                         void *outData) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (address == nullptr || outDataSize == nullptr || outData == nullptr || !HasProperty(driver, objectID, clientPID, address)) {
        LogUnknownProperty("GetPropertyData", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }

    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
        if (inDataSize < sizeof(AudioClassID)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioClassID *>(outData) = BaseClassIDForObject(objectID);
        *outDataSize = sizeof(AudioClassID);
        return noErr;
    case kAudioObjectPropertyClass:
        if (inDataSize < sizeof(AudioClassID)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioClassID *>(outData) = ClassIDForObject(objectID);
        *outDataSize = sizeof(AudioClassID);
        return noErr;
    case kAudioObjectPropertyOwner:
        if (inDataSize < sizeof(AudioObjectID)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioObjectID *>(outData) =
            objectID == kObjectIDPlugIn ? kAudioObjectUnknown :
            objectID == kObjectIDBox ? kObjectIDPlugIn :
            objectID == kObjectIDDevice ? kObjectIDPlugIn :
            kObjectIDDevice;
        *outDataSize = sizeof(AudioObjectID);
        return noErr;
    case kAudioObjectPropertyManufacturer:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CFSTR("soit.tech");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyElementName:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) =
            objectID == kObjectIDBox ? CFSTR("Mac Display Volume Box") :
            objectID == kObjectIDStreamOutput ? CFSTR("Mac Display Volume Output Stream") :
            objectID == kObjectIDVolumeLeft ? CFSTR("Left Volume") :
            objectID == kObjectIDVolumeRight ? CFSTR("Right Volume") :
            objectID == kObjectIDMute ? CFSTR("Mute") :
            CFSTR("Mac Display Volume");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioObjectPropertyOwnedObjects: {
        if (objectID == kObjectIDPlugIn) {
            const AudioObjectID ids[] = { kObjectIDBox, kObjectIDDevice };
            *outDataSize = WriteObjectIDs(
                outData,
                inDataSize,
                ids,
                gDevicePublished.load(std::memory_order_acquire) ? 2 : 1
            );
        } else if (objectID == kObjectIDDevice) {
            const AudioObjectID ids[] = { kObjectIDStreamOutput, kObjectIDVolumeLeft, kObjectIDVolumeRight, kObjectIDMute };
            *outDataSize = WriteObjectIDs(outData, inDataSize, ids, 4);
        } else {
            *outDataSize = 0;
        }
        return noErr;
    }
    case kAudioPlugInPropertyBundleID:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CFSTR("tech.soit.MacDisplayVolume.AudioServerPlugIn");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioPlugInPropertyDeviceList: {
        const AudioObjectID ids[] = { kObjectIDDevice };
        *outDataSize = WriteObjectIDs(
            outData,
            inDataSize,
            ids,
            gDevicePublished.load(std::memory_order_acquire) ? 1 : 0
        );
        return noErr;
    }
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (inDataSize < sizeof(AudioObjectID)) { return kAudioHardwareBadPropertySizeError; }
        AudioObjectID deviceID = kAudioObjectUnknown;
        if (qualifierDataSize == sizeof(CFStringRef) && qualifierData != nullptr) {
            auto uid = *static_cast<CFStringRef const *>(qualifierData);
            if (gDevicePublished.load(std::memory_order_acquire) &&
                uid != nullptr &&
                CFStringCompare(uid, CFSTR("tech.soit.MacDisplayVolume.Device"), 0) == kCFCompareEqualTo) {
                deviceID = kObjectIDDevice;
            }
        }
        *static_cast<AudioObjectID *>(outData) = deviceID;
        *outDataSize = sizeof(AudioObjectID);
        return noErr;
    }
    case kAudioPlugInPropertyBoxList: {
        const AudioObjectID ids[] = { kObjectIDBox };
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, 1);
        return noErr;
    }
    case kAudioPlugInPropertyTranslateUIDToBox:
        if (inDataSize < sizeof(AudioObjectID)) { return kAudioHardwareBadPropertySizeError; }
        {
            AudioObjectID boxID = kAudioObjectUnknown;
            if (qualifierDataSize == sizeof(CFStringRef) && qualifierData != nullptr) {
                auto uid = *static_cast<CFStringRef const *>(qualifierData);
                if (uid != nullptr && CFStringCompare(uid, CFSTR("tech.soit.MacDisplayVolume.Box"), 0) == kCFCompareEqualTo) {
                    boxID = kObjectIDBox;
                }
            }
            *static_cast<AudioObjectID *>(outData) = boxID;
        }
        *outDataSize = sizeof(AudioObjectID);
        return noErr;
    case kAudioBoxPropertyBoxUID:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CFSTR("tech.soit.MacDisplayVolume.Box");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioBoxPropertyDeviceList: {
        const AudioObjectID ids[] = { kObjectIDDevice };
        *outDataSize = WriteObjectIDs(
            outData,
            inDataSize,
            ids,
            gDevicePublished.load(std::memory_order_acquire) ? 1 : 0
        );
        return noErr;
    }
    case kAudioDevicePropertyTransportType:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = kAudioDeviceTransportTypeVirtual;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyPlugIn:
        if (inDataSize < sizeof(AudioObjectID)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioObjectID *>(outData) = kObjectIDPlugIn;
        *outDataSize = sizeof(AudioObjectID);
        return noErr;
    case kAudioBoxPropertyHasAudio:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 1;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioBoxPropertyHasVideo:
    case kAudioBoxPropertyHasMIDI:
    case kAudioBoxPropertyIsProtected:
    case kAudioBoxPropertyAcquisitionFailed:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioBoxPropertyAcquired:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gBoxAcquired.load(std::memory_order_acquire) ? 1 : 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceUID:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CFSTR("tech.soit.MacDisplayVolume.Device");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioDevicePropertyModelUID:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CFSTR("tech.soit.MacDisplayVolume.Model");
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kAudioDevicePropertyRelatedDevices: {
        const AudioObjectID ids[] = { kObjectIDDevice };
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, 1);
        return noErr;
    }
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyUsesVariableBufferFrameSizes:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyClockDomain:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyClockAlgorithm:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = kAudioDeviceClockAlgorithmRaw;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyClockIsStable:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 1;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceIsAlive:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gTargetAvailable.load(std::memory_order_acquire) ? 1 : 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 1;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioStreamPropertyIsActive:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gStreamOutputActive.load(std::memory_order_acquire) ? 1 : 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceIsRunningSomewhere:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gRunningClients.load(std::memory_order_acquire) > 0 ? 1 : 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyActualSampleRate:
        if (inDataSize < sizeof(Float64)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<Float64 *>(outData) = gSampleRate.load(std::memory_order_acquire);
        *outDataSize = sizeof(Float64);
        return noErr;
    case kAudioDevicePropertyHogMode:
        if (inDataSize < sizeof(pid_t)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<pid_t *>(outData) = -1;
        *outDataSize = sizeof(pid_t);
        return noErr;
    case kAudioDevicePropertyIOCycleUsage:
        if (inDataSize < sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<Float32 *>(outData) = 0.0f;
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        if (inDataSize < sizeof(AudioValueRange)) { return kAudioHardwareBadPropertySizeError; }
        static_cast<AudioValueRange *>(outData)->mMinimum = kDefaultSampleRate;
        static_cast<AudioValueRange *>(outData)->mMaximum = kDefaultSampleRate;
        *outDataSize = sizeof(AudioValueRange);
        return noErr;
    case kAudioDevicePropertyZeroTimeStampPeriod:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = kZeroTimestampPeriodFrames;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyStreams: {
        const AudioObjectID ids[] = { kObjectIDStreamOutput };
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, address->mScope == kAudioObjectPropertyScopeInput ? 0 : 1);
        return noErr;
    }
    case kAudioObjectPropertyControlList: {
        const AudioObjectID ids[] = { kObjectIDVolumeLeft, kObjectIDVolumeRight, kObjectIDMute };
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, 3);
        return noErr;
    }
    case kAudioDevicePropertyStreamConfiguration:
        if (inDataSize < offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer)) { return kAudioHardwareBadPropertySizeError; }
        {
            AudioBufferList *list = static_cast<AudioBufferList *>(outData);
            list->mNumberBuffers = address->mScope == kAudioDevicePropertyScopeInput ? 0 : 1;
            if (list->mNumberBuffers == 1) {
                list->mBuffers[0].mNumberChannels = kChannels;
                list->mBuffers[0].mDataByteSize = 0;
                list->mBuffers[0].mData = nullptr;
            }
        }
        *outDataSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer);
        return noErr;
    case kAudioDevicePropertyBufferFrameSize:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gPreferredBufferFrames.load(std::memory_order_acquire);
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kPropertyBufferFrameSize:
        if (inDataSize < sizeof(CFPropertyListRef)) { return kAudioHardwareBadPropertySizeError; }
        {
            UInt32 value = 0;
            value = gPreferredBufferFrames.load(std::memory_order_acquire);
            *static_cast<CFPropertyListRef *>(outData) = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
        }
        *outDataSize = sizeof(CFPropertyListRef);
        return noErr;
    case kAudioDevicePropertyBufferFrameSizeRange:
        if (inDataSize < sizeof(AudioValueRange)) { return kAudioHardwareBadPropertySizeError; }
        static_cast<AudioValueRange *>(outData)->mMinimum = 64;
        static_cast<AudioValueRange *>(outData)->mMaximum = 256;
        *outDataSize = sizeof(AudioValueRange);
        return noErr;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        if (inDataSize < 2 * sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        static_cast<UInt32 *>(outData)[0] = 1;
        static_cast<UInt32 *>(outData)[1] = 2;
        *outDataSize = 2 * sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyPreferredChannelLayout:
        if (inDataSize < offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription)) {
            return kAudioHardwareBadPropertySizeError;
        }
        {
            AudioChannelLayout *layout = static_cast<AudioChannelLayout *>(outData);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = kChannels;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            layout->mChannelDescriptions[0].mChannelFlags = 0;
            layout->mChannelDescriptions[1].mChannelFlags = 0;
        }
        *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
        return noErr;
    case kAudioObjectPropertyCustomPropertyInfoList:
        if (inDataSize < 4 * sizeof(AudioServerPlugInCustomPropertyInfo)) { return kAudioHardwareBadPropertySizeError; }
        {
            auto *info = static_cast<AudioServerPlugInCustomPropertyInfo *>(outData);
            info[0] = { kPropertyTargetUID, kAudioServerPlugInCustomPropertyDataTypeCFString, kAudioServerPlugInCustomPropertyDataTypeNone };
            info[1] = { kPropertyBufferFrameSize, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone };
            info[2] = { kPropertyStatus, kAudioServerPlugInCustomPropertyDataTypeCFString, kAudioServerPlugInCustomPropertyDataTypeNone };
            info[3] = { kPropertyReset, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone };
        }
        *outDataSize = 4 * sizeof(AudioServerPlugInCustomPropertyInfo);
        return noErr;
    case kAudioStreamPropertyDirection:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioStreamPropertyTerminalType:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = kAudioStreamTerminalTypeSpeaker;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioStreamPropertyStartingChannel:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 1;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        if (inDataSize < sizeof(AudioStreamBasicDescription)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioStreamBasicDescription *>(outData) = StreamFormatForSampleRate(gSampleRate.load(std::memory_order_acquire));
        *outDataSize = sizeof(AudioStreamBasicDescription);
        return noErr;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        if (inDataSize < sizeof(AudioStreamRangedDescription)) { return kAudioHardwareBadPropertySizeError; }
        static_cast<AudioStreamRangedDescription *>(outData)->mFormat = StreamFormatForSampleRate(kDefaultSampleRate);
        static_cast<AudioStreamRangedDescription *>(outData)->mSampleRateRange.mMinimum = kDefaultSampleRate;
        static_cast<AudioStreamRangedDescription *>(outData)->mSampleRateRange.mMaximum = kDefaultSampleRate;
        *outDataSize = sizeof(AudioStreamRangedDescription);
        return noErr;
    case kAudioControlPropertyScope:
        if (inDataSize < sizeof(AudioObjectPropertyScope)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioObjectPropertyScope *>(outData) = kAudioObjectPropertyScopeOutput;
        *outDataSize = sizeof(AudioObjectPropertyScope);
        return noErr;
    case kAudioControlPropertyElement:
        if (inDataSize < sizeof(AudioObjectPropertyElement)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<AudioObjectPropertyElement *>(outData) =
            objectID == kObjectIDVolumeLeft ? 1 :
            objectID == kObjectIDVolumeRight ? 2 :
            kAudioObjectPropertyElementMain;
        *outDataSize = sizeof(AudioObjectPropertyElement);
        return noErr;
    case kAudioLevelControlPropertyScalarValue:
        if (inDataSize < sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<Float32 *>(outData) = objectID == kObjectIDVolumeLeft
            ? gVolumeLeft.load(std::memory_order_acquire)
            : gVolumeRight.load(std::memory_order_acquire);
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioLevelControlPropertyDecibelValue:
    case kAudioLevelControlPropertyConvertScalarToDecibels:
        if (inDataSize < sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
        {
            Float32 value = *static_cast<Float32 *>(outData);
            if (address->mSelector == kAudioLevelControlPropertyDecibelValue) {
                value = objectID == kObjectIDVolumeLeft
                    ? gVolumeLeft.load(std::memory_order_acquire)
                    : gVolumeRight.load(std::memory_order_acquire);
            }
            *static_cast<Float32 *>(outData) = ScalarToDB(value);
        }
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioLevelControlPropertyDecibelRange:
        if (inDataSize < sizeof(AudioValueRange)) { return kAudioHardwareBadPropertySizeError; }
        static_cast<AudioValueRange *>(outData)->mMinimum = kMinDB;
        static_cast<AudioValueRange *>(outData)->mMaximum = kMaxDB;
        *outDataSize = sizeof(AudioValueRange);
        return noErr;
    case kAudioLevelControlPropertyConvertDecibelsToScalar:
        if (inDataSize < sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<Float32 *>(outData) = DBToScalar(*static_cast<Float32 *>(outData));
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioBooleanControlPropertyValue:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = gMute.load(std::memory_order_acquire) ? 1 : 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kPropertyTargetUID:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<CFStringRef *>(outData) = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : CFSTR("");
        }
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    case kPropertyStatus:
        if (inDataSize < sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<CFStringRef *>(outData) = CopyStatusString();
        *outDataSize = sizeof(CFStringRef);
        return noErr;
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus SetPropertyData(AudioServerPlugInDriverRef driver,
                         AudioObjectID objectID,
                         pid_t,
                         const AudioObjectPropertyAddress *address,
                         UInt32,
                         const void *,
                         UInt32 inDataSize,
                         const void *inData) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (address == nullptr || !HasProperty(driver, objectID, 0, address)) {
        LogUnknownProperty("SetPropertyData", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }
    if (inData == nullptr &&
        !((objectID == kObjectIDDevice || objectID == kObjectIDBox) &&
          address->mSelector == kPropertyReset)) {
        return kAudioHardwareBadPropertySizeError;
    }

    if (objectID == kObjectIDDevice || objectID == kObjectIDBox) {
        switch (address->mSelector) {
        case kPropertyBufferFrameSize: {
            if (inDataSize != sizeof(CFPropertyListRef)) { return kAudioHardwareBadPropertySizeError; }
            CFPropertyListRef propertyList = *static_cast<CFPropertyListRef const *>(inData);
            if (propertyList == nullptr || CFGetTypeID(propertyList) != CFNumberGetTypeID()) {
                return kAudioHardwareBadPropertySizeError;
            }
            SInt32 rawValue = 0;
            if (!CFNumberGetValue(static_cast<CFNumberRef>(propertyList), kCFNumberSInt32Type, &rawValue)) {
                return kAudioHardwareBadPropertySizeError;
            }
            bool didChangeBufferFrames = false;
            UInt32 value = std::clamp<UInt32>(static_cast<UInt32>(std::max<SInt32>(rawValue, 0)), 64, 256);
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                if (gPreferredBufferFrames.load(std::memory_order_acquire) != value) {
                    gPreferredBufferFrames.store(value, std::memory_order_release);
                    FlushRingLocked();
                    didChangeBufferFrames = true;
                }
            }
            if (didChangeBufferFrames) {
                SavePreferredBuffer(value);
                Notify(kObjectIDBox, kPropertyBufferFrameSize);
                if (gDevicePublished.load(std::memory_order_acquire)) {
                    Notify(kObjectIDDevice, kAudioDevicePropertyBufferFrameSize);
                    Notify(kObjectIDDevice, kPropertyBufferFrameSize);
                }
            }
            return noErr;
        }
        case kPropertyTargetUID: {
            if (inDataSize != sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
            CFStringRef uidToSave = nullptr;
            CFStringRef newUID = *static_cast<CFStringRef const *>(inData);
            if (IsVirtualDeviceUID(newUID)) {
                return kAudioHardwareIllegalOperationError;
            }
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                if (StringValuesEqual(gTargetUID, newUID)) {
                    return noErr;
                }
                if (gTargetUID != nullptr) {
                    CFRelease(gTargetUID);
                }
                gTargetUID = newUID != nullptr ? CFStringCreateCopy(nullptr, newUID) : nullptr;
                uidToSave = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : nullptr;
                gTargetStartError.store(noErr, std::memory_order_release);
                gTargetRecoveryFailures.store(0, std::memory_order_release);
                FlushRingLocked();
            }
            SaveTargetUID(uidToSave);
            if (uidToSave != nullptr) {
                CFRelease(uidToSave);
            }
            dispatch_async(gTargetQueue, ^{
                SetTargetAvailability(false);
                EvaluateTargetAvailability();
            });
            Notify(kObjectIDBox, kPropertyTargetUID);
            Notify(kObjectIDBox, kPropertyStatus);
            if (gDevicePublished.load(std::memory_order_acquire)) {
                Notify(kObjectIDDevice, kPropertyTargetUID);
                Notify(kObjectIDDevice, kPropertyStatus);
            }
            return noErr;
        }
        case kPropertyReset:
            if (inDataSize != 0) {
                if (inDataSize != sizeof(CFPropertyListRef)) { return kAudioHardwareBadPropertySizeError; }
                CFPropertyListRef propertyList = *static_cast<CFPropertyListRef const *>(inData);
                if (propertyList == nullptr || CFGetTypeID(propertyList) != CFNumberGetTypeID()) {
                    return kAudioHardwareBadPropertySizeError;
                }
            }
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                FlushRingLocked(true);
            }
            Notify(kObjectIDBox, kPropertyStatus);
            if (gDevicePublished.load(std::memory_order_acquire)) {
                Notify(kObjectIDDevice, kPropertyStatus);
            }
            return noErr;
        default:
            break;
        }
    }

    if (objectID == kObjectIDDevice) {
        switch (address->mSelector) {
        case kAudioDevicePropertyNominalSampleRate: {
            if (inDataSize != sizeof(Float64)) { return kAudioHardwareBadPropertySizeError; }
            Float64 requestedSampleRate = *static_cast<const Float64 *>(inData);
            if (!IsSupportedTargetSampleRate(requestedSampleRate)) {
                return kAudioHardwareIllegalOperationError;
            }

            Float64 currentSampleRate = gSampleRate.load(std::memory_order_acquire);
            gRequestedSampleRate.store(requestedSampleRate, std::memory_order_release);
            if (requestedSampleRate != currentSampleRate && gHost != nullptr) {
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    gHost->RequestDeviceConfigurationChange(gHost, kObjectIDDevice, kChangeActionSetSampleRate, nullptr);
                });
            }
            return noErr;
        }
        case kAudioDevicePropertyBufferFrameSize: {
            if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
            bool didChangeBufferFrames = false;
            UInt32 value = std::clamp(*static_cast<const UInt32 *>(inData), static_cast<UInt32>(64), static_cast<UInt32>(256));
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                if (gPreferredBufferFrames.load(std::memory_order_acquire) != value) {
                    gPreferredBufferFrames.store(value, std::memory_order_release);
                    FlushRingLocked();
                    didChangeBufferFrames = true;
                }
            }
            if (didChangeBufferFrames) {
                SavePreferredBuffer(value);
                Notify(kObjectIDDevice, kAudioDevicePropertyBufferFrameSize);
                Notify(kObjectIDDevice, kPropertyBufferFrameSize);
            }
            return noErr;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }

    if (objectID == kObjectIDStreamOutput) {
        switch (address->mSelector) {
        case kAudioStreamPropertyIsActive:
            if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
            {
                bool isActive = *static_cast<const UInt32 *>(inData) != 0;
                bool didChange = gStreamOutputActive.exchange(isActive, std::memory_order_acq_rel) != isActive;
                if (didChange) {
                    Notify(kObjectIDStreamOutput, kAudioStreamPropertyIsActive);
                }
            }
            return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (inDataSize != sizeof(AudioStreamBasicDescription)) { return kAudioHardwareBadPropertySizeError; }
            const auto &requestedFormat = *static_cast<const AudioStreamBasicDescription *>(inData);
            if (!HasSupportedStreamShape(requestedFormat)) {
                return kAudioDeviceUnsupportedFormatError;
            }
            if (!IsSupportedTargetSampleRate(requestedFormat.mSampleRate)) {
                return kAudioHardwareIllegalOperationError;
            }

            Float64 currentSampleRate = gSampleRate.load(std::memory_order_acquire);
            gRequestedSampleRate.store(requestedFormat.mSampleRate, std::memory_order_release);
            if (requestedFormat.mSampleRate != currentSampleRate && gHost != nullptr) {
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    gHost->RequestDeviceConfigurationChange(gHost, kObjectIDDevice, kChangeActionSetSampleRate, nullptr);
                });
            }
            return noErr;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }

    if (objectID == kObjectIDBox && address->mSelector == kAudioBoxPropertyAcquired) {
        if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        bool acquired = *static_cast<const UInt32 *>(inData) != 0;
        if (gBoxAcquired.exchange(acquired, std::memory_order_acq_rel) == acquired) {
            return noErr;
        }
        dispatch_async(gTargetQueue, ^{
            UpdateDevicePublication(false);
        });
        Notify(kObjectIDBox, kAudioBoxPropertyAcquired);
        Notify(kObjectIDBox, kPropertyStatus);
        return noErr;
    }

    if (objectID == kObjectIDVolumeLeft || objectID == kObjectIDVolumeRight) {
        Float32 scalar = 1.0f;
        if (address->mSelector == kAudioLevelControlPropertyScalarValue) {
            if (inDataSize != sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
            scalar = std::clamp(*static_cast<const Float32 *>(inData), 0.0f, 1.0f);
        } else if (address->mSelector == kAudioLevelControlPropertyDecibelValue) {
            if (inDataSize != sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
            scalar = DBToScalar(*static_cast<const Float32 *>(inData));
        } else {
            return kAudioHardwareUnknownPropertyError;
        }

        if (objectID == kObjectIDVolumeLeft) {
            gVolumeLeft.store(scalar, std::memory_order_release);
        } else {
            gVolumeRight.store(scalar, std::memory_order_release);
        }
        Notify(objectID, kAudioLevelControlPropertyScalarValue);
        Notify(objectID, kAudioLevelControlPropertyDecibelValue);
        return noErr;
    }

    if (objectID == kObjectIDMute && address->mSelector == kAudioBooleanControlPropertyValue) {
        if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        gMute.store(*static_cast<const UInt32 *>(inData) != 0, std::memory_order_release);
        Notify(kObjectIDMute, kAudioBooleanControlPropertyValue);
        return noErr;
    }

    return kAudioHardwareUnknownPropertyError;
}

OSStatus StartIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, UInt32) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }
    if (!gDevicePublished.load(std::memory_order_acquire)) {
        return kAudioHardwareBadDeviceError;
    }

    bool shouldStartTarget = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        UInt32 runningClients = gRunningClients.load(std::memory_order_acquire);
        if (runningClients == std::numeric_limits<UInt32>::max()) {
            return kAudioHardwareIllegalOperationError;
        }

        runningClients += 1;
        gRunningClients.store(runningClients, std::memory_order_release);
        if (runningClients == 1) {
            CancelScheduledTargetStop();
            gTargetRecoveryFailures.store(0, std::memory_order_release);
            FlushRingLocked();
            shouldStartTarget = !gTargetActive.load(std::memory_order_acquire);
        }
    }

    if (shouldStartTarget) {
        ScheduleStartTargetDevice();
    }

    Notify(kObjectIDDevice, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}

OSStatus StopIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceID, UInt32) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }

    bool shouldStopTarget = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        UInt32 runningClients = gRunningClients.load(std::memory_order_acquire);
        if (runningClients == 0) {
            return kAudioHardwareIllegalOperationError;
        }

        runningClients -= 1;
        gRunningClients.store(runningClients, std::memory_order_release);
        if (runningClients == 0) {
            FlushRingLocked();
            shouldStopTarget = true;
        }
    }

    if (shouldStopTarget) {
        ScheduleIdleStopTargetDevice();
    }

    Notify(kObjectIDDevice, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef driver,
                          AudioObjectID deviceID,
                          UInt32,
                          Float64 *sampleTime,
                          UInt64 *hostTime,
                          UInt64 *seed) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }
    UInt64 currentHostTime = AudioGetCurrentHostTime();
    UInt64 anchorHostTime = gAnchorHostTime.load(std::memory_order_acquire);
    Float64 anchorSampleTime = gAnchorSampleTime.load(std::memory_order_acquire);
    Float64 sampleRate = gSampleRate.load(std::memory_order_acquire);
    Float64 elapsedSeconds = 0.0;
    if (anchorHostTime > 0 && currentHostTime > anchorHostTime) {
        elapsedSeconds = static_cast<Float64>(AudioConvertHostTimeToNanos(currentHostTime - anchorHostTime)) / 1000000000.0;
    }
    UInt32 periodFrames = kZeroTimestampPeriodFrames;
    UInt64 elapsedFrames = static_cast<UInt64>(std::floor(elapsedSeconds * sampleRate));
    UInt64 alignedFrames = (elapsedFrames / periodFrames) * periodFrames;
    UInt64 alignedHostOffset = AudioConvertNanosToHostTime(
        static_cast<UInt64>((static_cast<Float64>(alignedFrames) / sampleRate) * 1000000000.0)
    );

    if (sampleTime != nullptr) {
        *sampleTime = anchorSampleTime + static_cast<Float64>(alignedFrames);
    }
    if (hostTime != nullptr) {
        *hostTime = anchorHostTime + alignedHostOffset;
    }
    if (seed != nullptr) {
        *seed = gZeroTimestampSeed.load(std::memory_order_acquire);
    }
    return noErr;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef driver,
                           AudioObjectID deviceID,
                           UInt32,
                           UInt32 operationID,
                           Boolean *willDo,
                           Boolean *willDoInPlace) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }
    if (willDo != nullptr) {
        *willDo = operationID == kAudioServerPlugInIOOperationWriteMix;
    }
    if (willDoInPlace != nullptr) {
        *willDoInPlace = true;
    }
    return noErr;
}

OSStatus BeginIOOperation(AudioServerPlugInDriverRef driver,
                          AudioObjectID deviceID,
                          UInt32,
                          UInt32,
                          UInt32,
                          const AudioServerPlugInIOCycleInfo *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }
    return noErr;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef driver,
                       AudioObjectID deviceID,
                       AudioObjectID streamID,
                       UInt32,
                       UInt32 operationID,
                       UInt32 frameCount,
                       const AudioServerPlugInIOCycleInfo *cycleInfo,
                       void *ioMainBuffer,
                       void *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice || streamID != kObjectIDStreamOutput) {
        return kAudioHardwareBadObjectError;
    }
    if (operationID != kAudioServerPlugInIOOperationWriteMix) {
        return kAudioHardwareIllegalOperationError;
    }
    if (IsLateWriteMix(cycleInfo, frameCount)) {
        return kAudioHardwareUnspecifiedError;
    }
    if (ioMainBuffer != nullptr) {
        StoreFrames(static_cast<const Float32 *>(ioMainBuffer), frameCount);
    }
    return noErr;
}

OSStatus EndIOOperation(AudioServerPlugInDriverRef driver,
                        AudioObjectID deviceID,
                        UInt32,
                        UInt32,
                        UInt32,
                        const AudioServerPlugInIOCycleInfo *) {
    if (driver != gDriverRef) {
        return kAudioHardwareBadObjectError;
    }
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }
    return noErr;
}

AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation,
};
AudioServerPlugInDriverInterface *gDriverInterfacePointer = &gDriverInterface;
AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePointer;

} // namespace

extern "C" __attribute__((visibility("default"))) void *MacDisplayVolume_Create(
    CFAllocatorRef,
    CFUUIDRef requestedTypeUUID
) {
    if (!CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }
    CFUUIDRef factoryUUID = CreateFactoryUUID();
    CFPlugInAddInstanceForFactory(factoryUUID);
    CFRelease(factoryUUID);
    return gDriverRef;
}
