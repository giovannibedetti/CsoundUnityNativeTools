// cni_apple.mm
// Unified native audio input plugin for macOS, iOS and visionOS.
// Uses AudioUnit (AUHAL on macOS, RemoteIO on iOS/visionOS) for capture and
// AVAudioSession (iOS/visionOS) or AudioObjectGetPropertyData (macOS) for
// device enumeration and selection.
//
// The public API surface is identical to cni_wasapi.cpp and cni_aaudio.cpp.
//
// Build targets:
//   macOS   → MODULE bundle   (CsoundNativeInput.bundle), universal arm64+x86_64
//   iOS     → STATIC library  (libCsoundNativeInput.a), arm64
//   visionOS→ STATIC library  (libCsoundNativeInput.a), arm64

#include "cni_ringbuffer.h"

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#if TARGET_OS_OSX
  #include <CoreAudio/CoreAudio.h>
  #import <AVFoundation/AVFoundation.h>  // for microphone permission request
#endif

#if TARGET_OS_IOS || TARGET_OS_VISION
  #import <AVFoundation/AVFoundation.h>
#endif

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdlib>

#define CNI_API extern "C" __attribute__((visibility("default")))

static const uint32_t kRingBufferMultiplier = 8;

// ---------------------------------------------------------------------------
// Device descriptor (platform-agnostic)
// ---------------------------------------------------------------------------

struct DeviceInfo
{
#if TARGET_OS_OSX
    AudioDeviceID nativeId;
#elif TARGET_OS_IOS || TARGET_OS_VISION
    NSString*     portUID;      // AVAudioSessionPortDescription.uid
#endif
    std::string   name;
    int           maxChannels;
    float         nominalSampleRate; // 0 on iOS/visionOS (AVAudioSession picks it)
};

static std::vector<DeviceInfo> gDevices;
static std::mutex              gMutex;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

struct Session
{
    AudioUnit           audioUnit      = nullptr;
    CniRingBuffer*      ringBuffer     = nullptr;

    // Pre-allocated AudioBufferList for AudioUnitRender.
    // Non-interleaved: one AudioBuffer per channel.
    AudioBufferList*    captureABL     = nullptr;
    float**             channelBufs    = nullptr; // per-channel raw buffers
    uint32_t            ablFrames      = 0;       // capacity of channelBufs in frames

    // Temp interleaved buffer for ring-buffer write.
    float*              interleaveBuf  = nullptr;

    int                 channelCount   = 0;
    int                 latencyFrames  = 0;
    float               sampleRate     = 0.f;
    std::atomic<bool>   running        { false };
};

static Session gSession;
static std::atomic<uint64_t> gFramesCaptured { 0 }; // incremented per-callback when render succeeds

// ---------------------------------------------------------------------------
// AudioUnit render callback (called on the audio thread, shared all platforms)
// ---------------------------------------------------------------------------

static OSStatus InputCallback(void*                       inRefCon,
                              AudioUnitRenderActionFlags* ioActionFlags,
                              const AudioTimeStamp*       inTimeStamp,
                              UInt32                      /*inBusNumber*/,
                              UInt32                      inNumberFrames,
                              AudioBufferList*            /*ioData*/)
{
    Session* s = (Session*)inRefCon;
    if (!s->running) return noErr;

    // Grow per-channel buffers if needed (should not happen after init but be safe).
    if (inNumberFrames > s->ablFrames)
    {
        for (int ch = 0; ch < s->channelCount; ch++)
        {
            free(s->channelBufs[ch]);
            s->channelBufs[ch] = (float*)malloc(inNumberFrames * sizeof(float));
        }
        free(s->interleaveBuf);
        s->interleaveBuf = (float*)malloc(inNumberFrames * s->channelCount * sizeof(float));
        s->ablFrames = inNumberFrames;

        for (int ch = 0; ch < s->channelCount; ch++)
        {
            s->captureABL->mBuffers[ch].mData          = s->channelBufs[ch];
            s->captureABL->mBuffers[ch].mDataByteSize  = inNumberFrames * sizeof(float);
        }
    }

    // Reset byte sizes (AudioUnitRender may change them).
    for (int ch = 0; ch < s->channelCount; ch++)
        s->captureABL->mBuffers[ch].mDataByteSize = inNumberFrames * sizeof(float);

    // Pull samples from the AudioUnit's input bus.
    OSStatus renderErr = AudioUnitRender(s->audioUnit, ioActionFlags, inTimeStamp,
                                         1 /*input bus*/, inNumberFrames, s->captureABL);
    if (renderErr != noErr)
    {
        // Common causes:
        //   -10863 (kAudioUnitErr_NoConnection) → microphone permission denied on macOS
        //   -10877 (kAudioComponentErr_InstanceInvalidated) → device disconnected
        // Log once per unique error code to avoid flooding.
        static OSStatus lastErr = noErr;
        if (renderErr != lastErr)
        {
            lastErr = renderErr;
            NSLog(@"[CsoundNativeInput] AudioUnitRender error: %d", (int)renderErr);
        }
        return noErr;
    }
    // Interleave non-interleaved channel buffers → ring buffer.
    int ch = s->channelCount;
    for (UInt32 f = 0; f < inNumberFrames; f++)
        for (int c = 0; c < ch; c++)
            s->interleaveBuf[f * ch + c] = ((float*)s->captureABL->mBuffers[c].mData)[f];

    // Clamp to whole frames before writing. cni_rb_write truncates to free space,
    // which may not be a multiple of channelCount — a partial-frame write shifts
    // the interleave and permanently swaps L/R for all subsequent reads.
    uint32_t freeFrames = cni_rb_free_space(s->ringBuffer) / (uint32_t)ch;
    uint32_t frames     = (inNumberFrames < freeFrames) ? (uint32_t)inNumberFrames : freeFrames;
    cni_rb_write(s->ringBuffer, s->interleaveBuf, frames * (uint32_t)ch);
    gFramesCaptured.fetch_add(frames, std::memory_order_relaxed);
    return noErr;
}

// ---------------------------------------------------------------------------
// Allocate / free session capture buffers
// ---------------------------------------------------------------------------

static bool AllocCaptureBuffers(Session* s, int channelCount, uint32_t framesPerCallback)
{
    size_t ablSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * channelCount;
    s->captureABL  = (AudioBufferList*)calloc(1, ablSize);
    s->channelBufs = (float**)calloc(channelCount, sizeof(float*));
    if (!s->captureABL || !s->channelBufs) return false;

    s->captureABL->mNumberBuffers = (UInt32)channelCount;
    for (int c = 0; c < channelCount; c++)
    {
        s->channelBufs[c] = (float*)calloc(framesPerCallback, sizeof(float));
        if (!s->channelBufs[c]) return false;
        s->captureABL->mBuffers[c].mNumberChannels = 1;
        s->captureABL->mBuffers[c].mDataByteSize   = framesPerCallback * sizeof(float);
        s->captureABL->mBuffers[c].mData           = s->channelBufs[c];
    }
    s->interleaveBuf = (float*)calloc(framesPerCallback * channelCount, sizeof(float));
    s->ablFrames     = framesPerCallback;
    return s->interleaveBuf != nullptr;
}

static void FreeCaptureBuffers(Session* s)
{
    if (s->channelBufs)
    {
        for (int c = 0; c < s->channelCount; c++)
            free(s->channelBufs[c]);
        free(s->channelBufs);
        s->channelBufs = nullptr;
    }
    free(s->captureABL);  s->captureABL  = nullptr;
    free(s->interleaveBuf); s->interleaveBuf = nullptr;
    s->ablFrames = 0;
}

// ---------------------------------------------------------------------------
// Shared AudioUnit setup (after device selection / AVAudioSession config)
// ---------------------------------------------------------------------------

// devId is only used on macOS (binds AUHAL to the selected device before Initialize).
// Pass 0 on iOS/visionOS — the device is selected via AVAudioSession, not here.
#if TARGET_OS_OSX
static int SetupAudioUnit(Session* s, int channelCount, int bufferSizeFrames, float sampleRate,
                          AudioDeviceID devId)
#else
static int SetupAudioUnit(Session* s, int channelCount, int bufferSizeFrames, float sampleRate)
#endif
{
    AudioComponentDescription desc = {};
    desc.componentType    = kAudioUnitType_Output;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
#if TARGET_OS_OSX
    desc.componentSubType = kAudioUnitSubType_HALOutput;
#else
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#endif

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) return -10;
    if (AudioComponentInstanceNew(comp, &s->audioUnit) != noErr) return -11;

    // Enable input bus (bus 1), disable output bus (bus 0).
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input,  1, &one,  sizeof(one));
    AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &zero, sizeof(zero));

    // Non-interleaved 32-bit float, N channels.
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate       = sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
                            | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    fmt.mChannelsPerFrame = (UInt32)channelCount;
    fmt.mBytesPerPacket   = sizeof(float);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = sizeof(float);

    AudioUnitSetProperty(s->audioUnit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

    // Install render callback on input bus.
    AURenderCallbackStruct cb = { InputCallback, s };
    AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 1, &cb, sizeof(cb));

    // 4× headroom covers SRC-expanded frame counts (e.g. 44100→48000 ≈ 1.09×)
    // and devices that snap bufferSizeFrames up to the next power-of-two.
    UInt32 maxFrames = (UInt32)bufferSizeFrames * 4;
    AudioUnitSetProperty(s->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

#if TARGET_OS_OSX
    // On macOS (AUHAL), the device MUST be bound before AudioUnitInitialize.
    // Doing it afterwards is silently ignored and the unit captures from the
    // wrong (or no) device.
    AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &devId, sizeof(devId));
#endif

    if (AudioUnitInitialize(s->audioUnit) != noErr) return -12;

    return 0;
}

// ===========================================================================
// Platform-specific device enumeration + cni_open device selection
// ===========================================================================

#if TARGET_OS_OSX
// ---------------------------------------------------------------------------
// macOS: AudioObjectGetPropertyData
// ---------------------------------------------------------------------------

static std::string MacGetDeviceName(AudioDeviceID devId)
{
    AudioObjectPropertyAddress prop = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef cfName = nullptr; UInt32 sz = sizeof(cfName);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &sz, &cfName);
    if (!cfName) return "Unknown";
    char buf[256];
    CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cfName);
    return buf;
}

static int MacGetInputChannels(AudioDeviceID devId)
{
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(devId, &prop, 0, nullptr, &sz) != noErr || sz == 0) return 0;
    AudioBufferList* list = (AudioBufferList*)malloc(sz);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &sz, list);
    int total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++)
        total += list->mBuffers[i].mNumberChannels;
    free(list);
    return total;
}

static float MacGetSampleRate(AudioDeviceID devId)
{
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0; UInt32 sz = sizeof(rate);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &sz, &rate);
    return (float)rate;
}

CNI_API int cni_get_device_count()
{
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices.clear();
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &sz) != noErr) return 0;
    int n = (int)(sz / sizeof(AudioDeviceID));
    std::vector<AudioDeviceID> ids(n);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &sz, ids.data());
    for (auto id : ids)
    {
        int ch = MacGetInputChannels(id);
        if (ch > 0)
            gDevices.push_back({ id, MacGetDeviceName(id), ch, MacGetSampleRate(id) });
    }
    return (int)gDevices.size();
}

static int PlatformOpen(Session* s, int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate)
{
    // macOS 10.14+: verify microphone permission.
    // NOTE: do NOT request permission here — requesting from a non-main thread (or while
    // blocking the main thread) prevents the system dialog from appearing.
    // Permission must be requested from C# via Application.RequestUserAuthorization before
    // calling cni_open. We only check the current status and bail early if denied.
    {
        AVAuthorizationStatus status =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted)
        {
            fprintf(stderr, "[CsoundNativeInput] Microphone permission denied (status=%d). "
                            "Grant access in System Settings → Privacy & Security → Microphone.\n",
                            (int)status);
            return -40; // permission denied / restricted
        }
        // NotDetermined: caller should have requested permission first (C# side).
        // Authorized: proceed normally.
    }

    AudioDeviceID devId;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;
        devId = gDevices[deviceIndex].nativeId;
        int maxCh = gDevices[deviceIndex].maxChannels;
        if (channelCount > maxCh) channelCount = maxCh;
    }

    // Try to set the requested I/O buffer size on the device.
    {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 bfs = (UInt32)bufferSizeFrames, sz = sizeof(bfs);
        AudioObjectSetPropertyData(devId, &prop, 0, nullptr, sz, &bfs);
    }

    s->channelCount = channelCount;
    // Use the rate Unity requests (AudioSettings.outputSampleRate) so that the
    // AUHAL output-scope format matches what the Unity audio thread expects.
    // When the hardware nominal rate differs (e.g. mic at 44100, Unity at 48000)
    // CoreAudio applies transparent SRC — far better than the ring buffer
    // draining at 6900 samples/s and producing periodic underrun glitches.
    float hwRate    = MacGetSampleRate(devId);
    s->sampleRate   = (sampleRate > 0.f) ? sampleRate : hwRate;

    // devId is passed into SetupAudioUnit so it can bind the device BEFORE
    // AudioUnitInitialize (macOS AUHAL requirement).
    int err = SetupAudioUnit(s, channelCount, bufferSizeFrames, s->sampleRate, devId);
    if (err != 0) return err;

    // Query the actual buffer frame size from the device.
    // The device binding happened inside SetupAudioUnit (before Initialize), so the
    // device ID is valid and after AudioObjectSetPropertyData has been applied.
    {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 bfs = (UInt32)bufferSizeFrames;
        UInt32 sz  = sizeof(bfs);
        AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &sz, &bfs);
        s->latencyFrames = (int)bfs;
    }

    return 0;
}

#elif TARGET_OS_IOS || TARGET_OS_VISION
// ---------------------------------------------------------------------------
// iOS / visionOS: AVAudioSession
// ---------------------------------------------------------------------------

CNI_API int cni_get_device_count()
{
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices.clear();

    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSArray<AVAudioSessionPortDescription*>* inputs = session.availableInputs;

    if (!inputs || inputs.count == 0)
    {
        // Fallback: built-in mic always present.
        gDevices.push_back({ nil, "Built-in Microphone", 1, 0.f });
        return 1;
    }

    int idx = 0;
    for (AVAudioSessionPortDescription* port in inputs)
    {
        DeviceInfo info;
        info.portUID         = [port.UID retain];
        info.name            = port.portName ? [port.portName UTF8String] : "Microphone";
        info.maxChannels     = (int)port.channels.count;
        info.nominalSampleRate = 0.f; // AVAudioSession picks sample rate automatically
        if (info.maxChannels < 1) info.maxChannels = 1;
        gDevices.push_back(info);
        idx++;
    }
    return (int)gDevices.size();
}

static int PlatformOpen(Session* s, int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate)
{
    NSString* portUID = nil;
    int maxCh = 1;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;
        portUID = gDevices[deviceIndex].portUID;
        maxCh   = gDevices[deviceIndex].maxChannels;
        if (channelCount > maxCh) channelCount = maxCh;
    }

    s->channelCount = channelCount;
    s->sampleRate   = sampleRate; // AVAudioSession will try to honour this

    NSError* error = nil;
    AVAudioSession* session = [AVAudioSession sharedInstance];

    // Configure session for low-latency multi-channel input.
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:AVAudioSessionCategoryOptionAllowBluetooth |
                         AVAudioSessionCategoryOptionDefaultToSpeaker
                   error:&error];
    [session setMode:AVAudioSessionModeMeasurement error:&error];
    [session setPreferredSampleRate:sampleRate error:&error];
    [session setPreferredIOBufferDuration:(double)bufferSizeFrames / sampleRate error:&error];

    // Select the device.
    if (portUID)
    {
        for (AVAudioSessionPortDescription* port in session.availableInputs)
        {
            if ([port.UID isEqualToString:portUID])
            {
                [session setPreferredInput:port error:&error];
                break;
            }
        }
    }

    // Request channel count.
    [session setPreferredInputNumberOfChannels:channelCount error:&error];

    [session setActive:YES error:&error];
    if (error)
    {
        NSLog(@"[CsoundNativeInput] AVAudioSession error: %@", error.localizedDescription);
        return -20;
    }

    // Read back actual values chosen by AVAudioSession.
    s->sampleRate   = (float)session.sampleRate;
    s->channelCount = (int)session.inputNumberOfChannels;
    if (s->channelCount < 1) s->channelCount = 1;

    int err = SetupAudioUnit(s, s->channelCount, bufferSizeFrames, s->sampleRate);
    if (err != 0) return err;

    // Report actual latency from AVAudioSession (convert duration → frames).
    s->latencyFrames = (int)(session.IOBufferDuration * s->sampleRate + 0.5);
    return 0;
}

#endif // TARGET_OS_IOS || TARGET_OS_VISION

// ===========================================================================
// Shared public API
// ===========================================================================

CNI_API void cni_get_device_name(int index, char* outName, int maxLen)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (!outName || maxLen <= 0) return;
    if (index < 0 || index >= (int)gDevices.size()) { outName[0] = '\0'; return; }
    strncpy(outName, gDevices[index].name.c_str(), maxLen - 1);
    outName[maxLen - 1] = '\0';
}

CNI_API int cni_get_device_channel_count(int index)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= (int)gDevices.size()) return 0;
    return gDevices[index].maxChannels;
}

CNI_API float cni_get_device_nominal_sample_rate(int index)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= (int)gDevices.size()) return 0.f;
    return gDevices[index].nominalSampleRate;
}

// Forward declaration — cni_close is defined below but called from cni_open.
CNI_API void cni_close();

// ksmps / exclusiveMode are accepted to keep the C ABI identical to the Windows
// (WASAPI) backend so the shared P/Invoke signature matches on every platform.
// They are unused here: CoreAudio manages buffer size and latency via the
// AudioUnit, and there is no shared/exclusive distinction.
CNI_API int cni_open(int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate,
                     int ksmps, int exclusiveMode)
{
    (void)ksmps; (void)exclusiveMode;
    if (gSession.running) cni_close();

    if (channelCount < 1) channelCount = 1;
    if (bufferSizeFrames < 32) bufferSizeFrames = 32;

    int err = PlatformOpen(&gSession, deviceIndex, channelCount, bufferSizeFrames, sampleRate);
    if (err != 0) return err;

    // Allocate capture buffers (non-interleaved, one per channel).
    // 4× to absorb SRC-expanded frame counts without realloc in the first callback.
    uint32_t framesHint = (uint32_t)(bufferSizeFrames * 4);
    if (!AllocCaptureBuffers(&gSession, gSession.channelCount, framesHint))
    {
        AudioComponentInstanceDispose(gSession.audioUnit);
        gSession.audioUnit = nullptr;
        return -30;
    }

    // Allocate ring buffer.
    uint32_t ringCap = framesHint * (uint32_t)gSession.channelCount * kRingBufferMultiplier;
    gSession.ringBuffer = cni_rb_create(ringCap);
    if (!gSession.ringBuffer)
    {
        FreeCaptureBuffers(&gSession);
        AudioComponentInstanceDispose(gSession.audioUnit);
        gSession.audioUnit = nullptr;
        return -31;
    }

    gSession.running = true;

    if (AudioOutputUnitStart(gSession.audioUnit) != noErr)
    {
        gSession.running = false;
        cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr;
        FreeCaptureBuffers(&gSession);
        AudioComponentInstanceDispose(gSession.audioUnit); gSession.audioUnit = nullptr;
        return -32;
    }

    return 0;
}

CNI_API void cni_close()
{
    if (!gSession.running && !gSession.audioUnit) return;
    gSession.running = false;

    if (gSession.audioUnit)
    {
        AudioOutputUnitStop(gSession.audioUnit);
        AudioUnitUninitialize(gSession.audioUnit);
        AudioComponentInstanceDispose(gSession.audioUnit);
        gSession.audioUnit = nullptr;
    }

#if TARGET_OS_IOS || TARGET_OS_VISION
    NSError* err = nil;
    [[AVAudioSession sharedInstance] setActive:NO error:&err];
    // Release retained port UIDs.
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto& d : gDevices)
            if (d.portUID) { [d.portUID release]; d.portUID = nil; }
    }
#endif

    cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr;
    FreeCaptureBuffers(&gSession);
    gSession.channelCount  = 0;
    gSession.latencyFrames = 0;
    gSession.sampleRate    = 0.f;
    gFramesCaptured.store(0, std::memory_order_relaxed);
}

CNI_API int cni_read_frames(float* outBuffer, int frameCount, int channelCount)
{
    if (!gSession.running || !gSession.ringBuffer || !outBuffer) return 0;
    int total = frameCount * channelCount;
    return (int)cni_rb_read(gSession.ringBuffer, outBuffer, (uint32_t)total) / channelCount;
}

CNI_API int cni_get_input_latency_frames()
{
    return gSession.latencyFrames;
}

// Returns total audio frames successfully captured by the CoreAudio callback since cni_open.
// If this stays at 0 while running, the AudioUnit is not receiving data
// (wrong device, permission issue, or hardware error).
CNI_API uint64_t cni_get_frames_captured()
{
    return gFramesCaptured.load(std::memory_order_relaxed);
}

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}
