#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
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

constexpr UInt32 kChannels = 2;
constexpr UInt32 kBytesPerChannel = sizeof(Float32);
constexpr UInt32 kBytesPerFrame = kChannels * kBytesPerChannel;
constexpr UInt32 kRingCapacityFrames = 8192;
constexpr UInt32 kMaxQueuedFrames = 2048;
constexpr UInt32 kDefaultBufferFrames = 128;
constexpr Float64 kDefaultSampleRate = 48000.0;
constexpr Float32 kMinDB = -60.0f;
constexpr Float32 kMaxDB = 0.0f;

AudioServerPlugInHostRef gHost = nullptr;
UInt32 gRefCount = 1;

std::mutex gStateMutex;
dispatch_queue_t gTargetQueue = dispatch_queue_create("tech.soit.MacDisplayVolume.target", DISPATCH_QUEUE_SERIAL);
std::array<Float32, kRingCapacityFrames * kChannels> gRing {};
UInt32 gReadFrame = 0;
UInt32 gQueuedFrames = 0;
UInt32 gRunningClients = 0;
UInt32 gPreferredBufferFrames = kDefaultBufferFrames;
Float64 gSampleRate = kDefaultSampleRate;
Float32 gVolumeLeft = 1.0f;
Float32 gVolumeRight = 1.0f;
bool gMute = false;
bool gBoxAcquired = true;
UInt64 gZeroTimestampSeed = 1;
UInt64 gAnchorHostTime = 0;
Float64 gAnchorSampleTime = 0.0;
UInt64 gFramesWritten = 0;
UInt64 gFramesRead = 0;
UInt64 gDroppedFrames = 0;
UInt64 gUnderruns = 0;
AudioDeviceID gTargetDevice = kAudioObjectUnknown;
AudioDeviceIOProcID gTargetIOProcID = nullptr;
CFStringRef gTargetUID = nullptr;

struct StateSnapshot {
    UInt32 queuedFrames;
    UInt32 preferredBufferFrames;
    Float64 sampleRate;
    UInt64 droppedFrames;
    UInt64 underruns;
    bool targetAlive;
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

bool IsSupportedTargetSampleRate(Float64 sampleRate) {
    return std::abs(sampleRate - kDefaultSampleRate) < 1.0;
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
    gZeroTimestampSeed += 1;
    gAnchorHostTime = AudioGetCurrentHostTime();
    gAnchorSampleTime = 0.0;
}

void FlushRingLocked() {
    gReadFrame = 0;
    gQueuedFrames = 0;
    gFramesWritten = 0;
    gFramesRead = 0;
    ReanchorTimelineLocked();
}

void StoreFrames(const Float32 *input, UInt32 frames) {
    if (input == nullptr || frames == 0) {
        return;
    }

    std::unique_lock<std::mutex> lock(gStateMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    for (UInt32 frame = 0; frame < frames; ++frame) {
        if (gQueuedFrames >= kRingCapacityFrames) {
            gReadFrame = (gReadFrame + 1) % kRingCapacityFrames;
            gQueuedFrames -= 1;
            gDroppedFrames += 1;
        }

        UInt32 writeFrame = (gReadFrame + gQueuedFrames) % kRingCapacityFrames;
        gRing[(writeFrame * kChannels) + 0] = input[(frame * kChannels) + 0];
        gRing[(writeFrame * kChannels) + 1] = input[(frame * kChannels) + 1];
        gQueuedFrames += 1;
        gFramesWritten += 1;
    }

    if (gQueuedFrames > kMaxQueuedFrames) {
        UInt32 excess = gQueuedFrames - kMaxQueuedFrames;
        gReadFrame = (gReadFrame + excess) % kRingCapacityFrames;
        gQueuedFrames = kMaxQueuedFrames;
        gDroppedFrames += excess;
    }
}

UInt32 FetchFrames(Float32 *output, UInt32 frames) {
    if (output == nullptr || frames == 0) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(gStateMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::memset(output, 0, frames * kBytesPerFrame);
        return 0;
    }
    UInt32 produced = std::min(frames, gQueuedFrames);
    Float32 gainLeft = gMute ? 0.0f : gVolumeLeft;
    Float32 gainRight = gMute ? 0.0f : gVolumeRight;

    for (UInt32 frame = 0; frame < produced; ++frame) {
        UInt32 readFrame = (gReadFrame + frame) % kRingCapacityFrames;
        output[(frame * kChannels) + 0] = gRing[(readFrame * kChannels) + 0] * gainLeft;
        output[(frame * kChannels) + 1] = gRing[(readFrame * kChannels) + 1] * gainRight;
    }

    if (produced < frames) {
        std::memset(output + (produced * kChannels), 0, (frames - produced) * kBytesPerFrame);
        if (gRunningClients > 0) {
            gUnderruns += 1;
        }
    }

    gReadFrame = (gReadFrame + produced) % kRingCapacityFrames;
    gQueuedFrames -= produced;
    gFramesRead += produced;
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

CFStringRef CopyDeviceUID(AudioDeviceID deviceID) {
    if (deviceID == kAudioObjectUnknown) {
        return nullptr;
    }

    AudioObjectPropertyAddress address = MakeAddress(kAudioDevicePropertyDeviceUID);
    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &uid);
    return status == noErr ? uid : nullptr;
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

AudioDeviceID FirstNonVirtualOutputDevice() {
    AudioObjectPropertyAddress address = MakeAddress(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
    }

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
        return kAudioObjectUnknown;
    }

    for (AudioDeviceID deviceID : devices) {
        CFStringRef uid = CopyDeviceUID(deviceID);
        if (uid == nullptr) {
            continue;
        }
        bool isSelf = IsVirtualDeviceUID(uid);
        CFRelease(uid);
        if (isSelf) {
            continue;
        }

        AudioObjectPropertyAddress streamAddress = MakeAddress(
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeOutput
        );
        UInt32 streamSize = 0;
        if (AudioObjectGetPropertyDataSize(deviceID, &streamAddress, 0, nullptr, &streamSize) != noErr || streamSize == 0) {
            continue;
        }

        std::vector<std::byte> storage(streamSize);
        if (AudioObjectGetPropertyData(deviceID, &streamAddress, 0, nullptr, &streamSize, storage.data()) != noErr) {
            continue;
        }
        const AudioBufferList *buffers = reinterpret_cast<const AudioBufferList *>(storage.data());
        for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
            if (buffers->mBuffers[index].mNumberChannels > 0) {
                return deviceID;
            }
        }
    }

    return kAudioObjectUnknown;
}

void SaveTargetUID(CFStringRef uid) {
    if (gHost != nullptr && uid != nullptr) {
        gHost->WriteToStorage(gHost, CFSTR("targetOutputDeviceUID"), uid);
    }
}

void SavePreferredBuffer(UInt32 preferredBufferFrames) {
    if (gHost != nullptr) {
        CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &preferredBufferFrames);
        gHost->WriteToStorage(gHost, CFSTR("preferredBufferFrameSize"), number);
        CFRelease(number);
    }
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
                gPreferredBufferFrames = std::clamp<UInt32>(static_cast<UInt32>(value), 64, 256);
            }
        }
        CFRelease(data);
    }
}

OSStatus TargetDeviceIOProc(AudioObjectID,
                            const AudioTimeStamp *,
                            const AudioBufferList *,
                            const AudioTimeStamp *,
                            AudioBufferList *outOutputData,
                            const AudioTimeStamp *,
                            void *) {
    UInt32 frames = 0;
    if (outOutputData != nullptr && outOutputData->mNumberBuffers > 0 && outOutputData->mBuffers[0].mDataByteSize > 0) {
        UInt32 channels = std::max<UInt32>(outOutputData->mBuffers[0].mNumberChannels, 1);
        frames = outOutputData->mBuffers[0].mDataByteSize / (sizeof(Float32) * channels);
    }
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
    }

    if (targetDevice != kAudioObjectUnknown && targetIOProcID != nullptr) {
        AudioDeviceStop(targetDevice, targetIOProcID);
        AudioDeviceDestroyIOProcID(targetDevice, targetIOProcID);
    }
}

void StartTargetDevice() {
    StopTargetDevice();

    CFStringRef targetUID = nullptr;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        targetUID = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : nullptr;
    }

    AudioDeviceID target = kAudioObjectUnknown;
    if (targetUID != nullptr) {
        if (IsEmptyString(targetUID) || IsVirtualDeviceUID(targetUID)) {
            CFRelease(targetUID);
            return;
        }
        target = DeviceIDForUID(targetUID);
        CFRelease(targetUID);
    } else {
        target = FirstNonVirtualOutputDevice();
        if (target != kAudioObjectUnknown) {
            CFStringRef uid = CopyDeviceUID(target);
            if (uid != nullptr) {
                CFStringRef uidToSave = nullptr;
                {
                    std::lock_guard<std::mutex> lock(gStateMutex);
                    if (gTargetUID != nullptr) {
                        CFRelease(gTargetUID);
                    }
                    gTargetUID = uid;
                    uidToSave = CFStringCreateCopy(nullptr, uid);
                }
                SaveTargetUID(uidToSave);
                if (uidToSave != nullptr) {
                    CFRelease(uidToSave);
                }
            }
        }
    }

    if (target == kAudioObjectUnknown) {
        return;
    }
    if (!IsSupportedTargetSampleRate(DeviceNominalSampleRate(target))) {
        return;
    }

    AudioDeviceIOProcID ioProcID = nullptr;
    if (AudioDeviceCreateIOProcID(target, TargetDeviceIOProc, nullptr, &ioProcID) != noErr) {
        return;
    }

    if (AudioDeviceStart(target, ioProcID) != noErr) {
        AudioDeviceDestroyIOProcID(target, ioProcID);
        return;
    }

    bool shouldStop = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gRunningClients > 0) {
            gTargetDevice = target;
            gTargetIOProcID = ioProcID;
        } else {
            shouldStop = true;
        }
    }
    if (shouldStop) {
        AudioDeviceStop(target, ioProcID);
        AudioDeviceDestroyIOProcID(target, ioProcID);
    }
}

StateSnapshot SnapshotLocked() {
    return StateSnapshot {
        gQueuedFrames,
        gPreferredBufferFrames,
        gSampleRate,
        gDroppedFrames,
        gUnderruns,
        gTargetDevice != kAudioObjectUnknown && gTargetIOProcID != nullptr,
    };
}

CFStringRef CopyStatusString() {
    std::lock_guard<std::mutex> lock(gStateMutex);
    StateSnapshot snapshot = SnapshotLocked();
    double queuedMS = snapshot.sampleRate > 0 ? (static_cast<double>(snapshot.queuedFrames) / snapshot.sampleRate) * 1000.0 : 0.0;
    char buffer[512];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "running=%u,target=%s,queuedFrames=%u,queuedMS=%.2f,bufferFrames=%u,dropped=%llu,underruns=%llu,sampleRate=%.0f",
        gRunningClients,
        snapshot.targetAlive ? "yes" : "no",
        snapshot.queuedFrames,
        queuedMS,
        snapshot.preferredBufferFrames,
        snapshot.droppedFrames,
        snapshot.underruns,
        snapshot.sampleRate
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
    case kAudioBoxPropertyBoxUID:
    case kAudioBoxPropertyTransportType:
    case kAudioBoxPropertyHasAudio:
    case kAudioBoxPropertyHasVideo:
    case kAudioBoxPropertyHasMIDI:
    case kAudioBoxPropertyIsProtected:
    case kAudioBoxPropertyAcquired:
    case kAudioBoxPropertyAcquisitionFailed:
    case kAudioBoxPropertyDeviceList:
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
        return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput;
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

    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(nullptr, uuid);
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

OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    gHost = host;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        FlushRingLocked();
    }
    LoadConfigurationFromStorage();
    return noErr;
}

OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *) {
    return noErr;
}

OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *) {
    return noErr;
}

OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64 action, void *) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    if (action > 0) {
        if (static_cast<Float64>(action) != kDefaultSampleRate) {
            return kAudioHardwareIllegalOperationError;
        }
        gSampleRate = kDefaultSampleRate;
        FlushRingLocked();
    }
    return noErr;
}

OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) {
    return noErr;
}

Boolean HasProperty(AudioServerPlugInDriverRef,
                    AudioObjectID objectID,
                    pid_t,
                    const AudioObjectPropertyAddress *address) {
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

OSStatus IsPropertySettable(AudioServerPlugInDriverRef,
                            AudioObjectID objectID,
                            pid_t,
                            const AudioObjectPropertyAddress *address,
                            Boolean *isSettable) {
    if (address == nullptr || isSettable == nullptr || !HasProperty(nullptr, objectID, 0, address)) {
        LogUnknownProperty("IsPropertySettable", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }

    *isSettable = false;
    if (objectID == kObjectIDBox) {
        *isSettable = address->mSelector == kAudioBoxPropertyAcquired;
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

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef,
                             AudioObjectID objectID,
                             pid_t clientPID,
                             const AudioObjectPropertyAddress *address,
                             UInt32,
                             const void *,
                             UInt32 *outDataSize) {
    if (address == nullptr || outDataSize == nullptr || !HasProperty(nullptr, objectID, clientPID, address)) {
        LogUnknownProperty("GetPropertyDataSize", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }

    switch (address->mSelector) {
    case kAudioObjectPropertyOwnedObjects:
        if (objectID == kObjectIDPlugIn) {
            *outDataSize = 2 * sizeof(AudioObjectID);
        } else if (objectID == kObjectIDDevice) {
            *outDataSize = 4 * sizeof(AudioObjectID);
        } else {
            *outDataSize = 0;
        }
        return noErr;
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyBoxList:
    case kAudioBoxPropertyDeviceList:
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

OSStatus GetPropertyData(AudioServerPlugInDriverRef,
                         AudioObjectID objectID,
                         pid_t clientPID,
                         const AudioObjectPropertyAddress *address,
                         UInt32 qualifierDataSize,
                         const void *qualifierData,
                         UInt32 inDataSize,
                         UInt32 *outDataSize,
                         void *outData) {
    if (address == nullptr || outDataSize == nullptr || outData == nullptr || !HasProperty(nullptr, objectID, clientPID, address)) {
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
            *outDataSize = WriteObjectIDs(outData, inDataSize, ids, 2);
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
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, gBoxAcquired ? 1 : 0);
        return noErr;
    }
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (inDataSize < sizeof(AudioObjectID)) { return kAudioHardwareBadPropertySizeError; }
        AudioObjectID deviceID = kAudioObjectUnknown;
        if (qualifierDataSize == sizeof(CFStringRef) && qualifierData != nullptr) {
            auto uid = *static_cast<CFStringRef const *>(qualifierData);
            if (uid != nullptr && CFStringCompare(uid, CFSTR("tech.soit.MacDisplayVolume.Device"), 0) == kCFCompareEqualTo) {
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
        *outDataSize = WriteObjectIDs(outData, inDataSize, ids, gBoxAcquired ? 1 : 0);
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
        *static_cast<UInt32 *>(outData) = gBoxAcquired ? 1 : 0;
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
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyUsesVariableBufferFrameSizes:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 0;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioStreamPropertyIsActive:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        *static_cast<UInt32 *>(outData) = 1;
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceIsRunningSomewhere:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<UInt32 *>(outData) = gRunningClients > 0 ? 1 : 0;
        }
        *outDataSize = sizeof(UInt32);
        return noErr;
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyActualSampleRate:
        if (inDataSize < sizeof(Float64)) { return kAudioHardwareBadPropertySizeError; }
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<Float64 *>(outData) = gSampleRate;
        }
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
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<UInt32 *>(outData) = gPreferredBufferFrames;
        }
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
    case kPropertyBufferFrameSize:
        if (inDataSize < sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<UInt32 *>(outData) = gPreferredBufferFrames;
        }
        *outDataSize = sizeof(UInt32);
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
            info[1] = { kPropertyBufferFrameSize, kAudioServerPlugInCustomPropertyDataTypeNone, kAudioServerPlugInCustomPropertyDataTypeNone };
            info[2] = { kPropertyStatus, kAudioServerPlugInCustomPropertyDataTypeCFString, kAudioServerPlugInCustomPropertyDataTypeNone };
            info[3] = { kPropertyReset, kAudioServerPlugInCustomPropertyDataTypeNone, kAudioServerPlugInCustomPropertyDataTypeNone };
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
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<AudioStreamBasicDescription *>(outData) = StreamFormatForSampleRate(gSampleRate);
        }
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
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<Float32 *>(outData) = objectID == kObjectIDVolumeLeft ? gVolumeLeft : gVolumeRight;
        }
        *outDataSize = sizeof(Float32);
        return noErr;
    case kAudioLevelControlPropertyDecibelValue:
    case kAudioLevelControlPropertyConvertScalarToDecibels:
        if (inDataSize < sizeof(Float32)) { return kAudioHardwareBadPropertySizeError; }
        {
            Float32 value = *static_cast<Float32 *>(outData);
            if (address->mSelector == kAudioLevelControlPropertyDecibelValue) {
                std::lock_guard<std::mutex> lock(gStateMutex);
                value = objectID == kObjectIDVolumeLeft ? gVolumeLeft : gVolumeRight;
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
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            *static_cast<UInt32 *>(outData) = gMute ? 1 : 0;
        }
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

OSStatus SetPropertyData(AudioServerPlugInDriverRef,
                         AudioObjectID objectID,
                         pid_t,
                         const AudioObjectPropertyAddress *address,
                         UInt32,
                         const void *,
                         UInt32 inDataSize,
                         const void *inData) {
    if (address == nullptr || !HasProperty(nullptr, objectID, 0, address)) {
        LogUnknownProperty("SetPropertyData", objectID, address);
        return kAudioHardwareUnknownPropertyError;
    }
    if (inData == nullptr && !(objectID == kObjectIDDevice && address->mSelector == kPropertyReset)) {
        return kAudioHardwareBadPropertySizeError;
    }

    if (objectID == kObjectIDDevice) {
        switch (address->mSelector) {
        case kAudioDevicePropertyNominalSampleRate:
            if (inDataSize != sizeof(Float64)) { return kAudioHardwareBadPropertySizeError; }
            if (*static_cast<const Float64 *>(inData) != kDefaultSampleRate) { return kAudioHardwareIllegalOperationError; }
            return noErr;
        case kAudioDevicePropertyBufferFrameSize:
        case kPropertyBufferFrameSize: {
            if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
            bool didChangeBufferFrames = false;
            UInt32 value = std::clamp(*static_cast<const UInt32 *>(inData), static_cast<UInt32>(64), static_cast<UInt32>(256));
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                if (gPreferredBufferFrames != value) {
                    gPreferredBufferFrames = value;
                    FlushRingLocked();
                    didChangeBufferFrames = true;
                }
            }
            if (didChangeBufferFrames) {
                SavePreferredBuffer(value);
            }
            Notify(kObjectIDDevice, kAudioDevicePropertyBufferFrameSize);
            Notify(kObjectIDDevice, kPropertyBufferFrameSize);
            return noErr;
        }
        case kPropertyTargetUID: {
            if (inDataSize != sizeof(CFStringRef)) { return kAudioHardwareBadPropertySizeError; }
            bool shouldStartTarget = false;
            CFStringRef uidToSave = nullptr;
            CFStringRef newUID = *static_cast<CFStringRef const *>(inData);
            if (IsVirtualDeviceUID(newUID)) {
                return kAudioHardwareIllegalOperationError;
            }
            if (!IsEmptyString(newUID)) {
                AudioDeviceID newTarget = DeviceIDForUID(newUID);
                if (newTarget != kAudioObjectUnknown && !IsSupportedTargetSampleRate(DeviceNominalSampleRate(newTarget))) {
                    return kAudioHardwareIllegalOperationError;
                }
            }
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                if (gTargetUID != nullptr) {
                    CFRelease(gTargetUID);
                }
                gTargetUID = newUID != nullptr ? CFStringCreateCopy(nullptr, newUID) : nullptr;
                uidToSave = gTargetUID != nullptr ? CFStringCreateCopy(nullptr, gTargetUID) : nullptr;
                FlushRingLocked();
                shouldStartTarget = gRunningClients > 0;
            }
            SaveTargetUID(uidToSave);
            if (uidToSave != nullptr) {
                CFRelease(uidToSave);
            }
            dispatch_async(gTargetQueue, ^{
                StopTargetDevice();
                if (shouldStartTarget) {
                    StartTargetDevice();
                }
            });
            Notify(kObjectIDDevice, kPropertyTargetUID);
            Notify(kObjectIDDevice, kPropertyStatus);
            return noErr;
        }
        case kPropertyReset:
            {
                std::lock_guard<std::mutex> lock(gStateMutex);
                FlushRingLocked();
            }
            Notify(kObjectIDDevice, kPropertyStatus);
            return noErr;
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }

    if (objectID == kObjectIDBox && address->mSelector == kAudioBoxPropertyAcquired) {
        if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        bool acquired = *static_cast<const UInt32 *>(inData) != 0;
        bool shouldStopTarget = false;
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            gBoxAcquired = acquired;
            if (!gBoxAcquired) {
                FlushRingLocked();
                shouldStopTarget = true;
            }
        }
        if (shouldStopTarget) {
            StopTargetDevice();
        }
        Notify(kObjectIDBox, kAudioBoxPropertyAcquired);
        Notify(kObjectIDBox, kAudioBoxPropertyDeviceList);
        Notify(kObjectIDPlugIn, kAudioPlugInPropertyDeviceList);
        Notify(kObjectIDPlugIn, kAudioObjectPropertyOwnedObjects);
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

        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            if (objectID == kObjectIDVolumeLeft) {
                gVolumeLeft = scalar;
            } else {
                gVolumeRight = scalar;
            }
        }
        Notify(objectID, kAudioLevelControlPropertyScalarValue);
        Notify(objectID, kAudioLevelControlPropertyDecibelValue);
        return noErr;
    }

    if (objectID == kObjectIDMute && address->mSelector == kAudioBooleanControlPropertyValue) {
        if (inDataSize != sizeof(UInt32)) { return kAudioHardwareBadPropertySizeError; }
        {
            std::lock_guard<std::mutex> lock(gStateMutex);
            gMute = *static_cast<const UInt32 *>(inData) != 0;
        }
        Notify(kObjectIDMute, kAudioBooleanControlPropertyValue);
        return noErr;
    }

    return kAudioHardwareUnknownPropertyError;
}

OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }

    bool shouldStartTarget = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gRunningClients == std::numeric_limits<UInt32>::max()) {
            return kAudioHardwareIllegalOperationError;
        }

        gRunningClients += 1;
        if (gRunningClients == 1) {
            FlushRingLocked();
            shouldStartTarget = true;
        }
    }

    if (shouldStartTarget) {
        dispatch_async(gTargetQueue, ^{
            StartTargetDevice();
        });
    }

    Notify(kObjectIDDevice, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}

OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID deviceID, UInt32) {
    if (deviceID != kObjectIDDevice) {
        return kAudioHardwareBadDeviceError;
    }

    bool shouldStopTarget = false;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gRunningClients == 0) {
            return kAudioHardwareIllegalOperationError;
        }

        gRunningClients -= 1;
        if (gRunningClients == 0) {
            FlushRingLocked();
            shouldStopTarget = true;
        }
    }

    if (shouldStopTarget) {
        dispatch_async(gTargetQueue, ^{
            StopTargetDevice();
        });
    }

    Notify(kObjectIDDevice, kAudioDevicePropertyDeviceIsRunning);
    return noErr;
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef,
                          AudioObjectID,
                          UInt32,
                          Float64 *sampleTime,
                          UInt64 *hostTime,
                          UInt64 *seed) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    UInt64 currentHostTime = AudioGetCurrentHostTime();
    Float64 elapsedSeconds = 0.0;
    if (gAnchorHostTime > 0 && currentHostTime > gAnchorHostTime) {
        elapsedSeconds = static_cast<Float64>(AudioConvertHostTimeToNanos(currentHostTime - gAnchorHostTime)) / 1000000000.0;
    }
    UInt32 periodFrames = std::max<UInt32>(gPreferredBufferFrames, 1);
    UInt64 elapsedFrames = static_cast<UInt64>(std::floor(elapsedSeconds * gSampleRate));
    UInt64 alignedFrames = (elapsedFrames / periodFrames) * periodFrames;
    UInt64 alignedHostOffset = AudioConvertNanosToHostTime(
        static_cast<UInt64>((static_cast<Float64>(alignedFrames) / gSampleRate) * 1000000000.0)
    );

    if (sampleTime != nullptr) {
        *sampleTime = gAnchorSampleTime + static_cast<Float64>(alignedFrames);
    }
    if (hostTime != nullptr) {
        *hostTime = gAnchorHostTime + alignedHostOffset;
    }
    if (seed != nullptr) {
        *seed = gZeroTimestampSeed;
    }
    return noErr;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef,
                           AudioObjectID,
                           UInt32,
                           UInt32 operationID,
                           Boolean *willDo,
                           Boolean *willDoInPlace) {
    if (willDo != nullptr) {
        *willDo = operationID == kAudioServerPlugInIOOperationWriteMix;
    }
    if (willDoInPlace != nullptr) {
        *willDoInPlace = true;
    }
    return noErr;
}

OSStatus BeginIOOperation(AudioServerPlugInDriverRef,
                          AudioObjectID,
                          UInt32,
                          UInt32,
                          UInt32,
                          const AudioServerPlugInIOCycleInfo *) {
    return noErr;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef,
                       AudioObjectID deviceID,
                       AudioObjectID streamID,
                       UInt32,
                       UInt32 operationID,
                       UInt32 frameCount,
                       const AudioServerPlugInIOCycleInfo *,
                       void *ioMainBuffer,
                       void *) {
    if (deviceID != kObjectIDDevice || streamID != kObjectIDStreamOutput) {
        return kAudioHardwareBadObjectError;
    }
    if (operationID == kAudioServerPlugInIOOperationWriteMix && ioMainBuffer != nullptr) {
        StoreFrames(static_cast<const Float32 *>(ioMainBuffer), frameCount);
    }
    return noErr;
}

OSStatus EndIOOperation(AudioServerPlugInDriverRef,
                        AudioObjectID,
                        UInt32,
                        UInt32,
                        UInt32,
                        const AudioServerPlugInIOCycleInfo *) {
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
