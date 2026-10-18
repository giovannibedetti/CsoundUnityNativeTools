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
#include <unistd.h>   // usleep, while waiting for a device rate change to take

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

    // macOS only. Set when we changed the device's nominal sample rate and therefore owe it a
    // restore: deviceId is what to put back, restoreRate is the value we found it on. Zero means
    // we changed nothing, which is the default.
    uint32_t            deviceId       = 0;
    double              restoreRate    = 0.0;
};

static Session gSession;
static std::atomic<uint64_t> gFramesCaptured  { 0 }; // incremented per-callback when render succeeds
static std::atomic<int32_t>  gLastRenderError { 0 }; // last OSStatus from AudioUnitRender (0 = no error)

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
        // Store the error so cni_get_last_render_error() can report it to the C# side.
        // Common causes:
        //   -10863 (kAudioUnitErr_NoConnection) → device not bound before format set, or
        //                                         microphone permission denied in macOS Privacy
        //   -10877 (kAudioComponentErr_InstanceInvalidated) → device disconnected
        gLastRenderError.store((int32_t)renderErr, std::memory_order_relaxed);
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
    //
    // Every one of these used to be called for its side effect with the OSStatus thrown away, so a
    // setup that half failed looked exactly like one that worked and only showed up later as
    // kAudioUnitErr_NoConnection from AudioUnitRender, with nothing saying why. Each is checked now
    // and gets its own return code.
    UInt32 one = 1, zero = 0;
    OSStatus st = AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_EnableIO,
                                       kAudioUnitScope_Input,  1, &one,  sizeof(one));
    if (st != noErr)
    {
        NSLog(@"[CsoundNativeInput] EnableIO(input) failed: %d", (int)st);
        return -13;
    }
    // Not fatal: some devices refuse to have the output bus disabled, and capture still works.
    st = AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    if (st != noErr)
        NSLog(@"[CsoundNativeInput] EnableIO(output off) failed: %d — continuing", (int)st);

#if TARGET_OS_OSX
    // On macOS (AUHAL) bind the device BEFORE setting the stream format.
    // The AUHAL needs to know the hardware capabilities (channel count, supported
    // sample rates) to validate the format we're about to request, and to set up
    // sample-rate conversion if the requested rate differs from the device rate.
    // Binding it after the format set, even if still before AudioUnitInitialize, leaves the input bus
    // without a hardware association when the format is committed, so this order is the right one.
    // It was once believed to be the cause of kAudioUnitErr_NoConnection (-10863); it is not. That was
    // measured to come from declaring a sample rate the hardware was not running at — see the two
    // modes below.
    // Skip binding if devId == kAudioObjectUnknown (placeholder / permission not yet
    // granted): the AUHAL will use the system default input device automatically,
    // and AudioOutputUnitStart() will trigger the macOS permission dialog.
    if (devId != kAudioObjectUnknown)
    {
        st = AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0, &devId, sizeof(devId));
        if (st != noErr)
        {
            NSLog(@"[CsoundNativeInput] CurrentDevice(%u) failed: %d", (unsigned)devId, (int)st);
            return -14;
        }
    }
#endif

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

    st = AudioUnitSetProperty(s->audioUnit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
    if (st != noErr)
    {
        NSLog(@"[CsoundNativeInput] StreamFormat(%.0f Hz, %dch) refused: %d",
              (double)sampleRate, channelCount, (int)st);
        return -15;
    }

    // Read back what the unit kept, on both sides of the input bus: the input scope is the hardware
    // side, the output scope is ours. When they disagree on the rate the unit is supposed to convert,
    // and when it cannot, AudioUnitRender fails — so the two numbers are worth having in the log the
    // one time someone needs them.
    {
        AudioStreamBasicDescription in = {}, out = {};
        UInt32 sz = sizeof(in);
        AudioUnitGetProperty(s->audioUnit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,  1, &in,  &sz);
        sz = sizeof(out);
        AudioUnitGetProperty(s->audioUnit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 1, &out, &sz);
        NSLog(@"[CsoundNativeInput] input bus: hardware side %.0f Hz %uch, our side %.0f Hz %uch",
              in.mSampleRate,  (unsigned)in.mChannelsPerFrame,
              out.mSampleRate, (unsigned)out.mChannelsPerFrame);
    }

    // Install render callback on input bus.
    AURenderCallbackStruct cb = { InputCallback, s };
    st = AudioUnitSetProperty(s->audioUnit, kAudioOutputUnitProperty_SetInputCallback,
                              kAudioUnitScope_Global, 1, &cb, sizeof(cb));
    if (st != noErr)
    {
        NSLog(@"[CsoundNativeInput] SetInputCallback failed: %d", (int)st);
        return -16;
    }

    // 4× headroom covers SRC-expanded frame counts (e.g. 44100→48000 ≈ 1.09×)
    // and devices that snap bufferSizeFrames up to the next power-of-two.
    UInt32 maxFrames = (UInt32)bufferSizeFrames * 4;
    AudioUnitSetProperty(s->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

    st = AudioUnitInitialize(s->audioUnit);
    if (st != noErr)
    {
        NSLog(@"[CsoundNativeInput] AudioUnitInitialize failed: %d", (int)st);
        return -12;
    }

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

    // macOS 14+ Sonoma / 15 Sequoia: kAudioDevicePropertyStreamConfiguration with input scope
    // returns an empty AudioBufferList (mNumberBuffers=0) when microphone permission is
    // NotDetermined or Denied, causing MacGetInputChannels() to return 0 for every device.
    // On Sequoia even kAudioHardwarePropertyDefaultInputDevice may be restricted.
    //
    // Unconditional fallback: if we found no input devices (regardless of the reason),
    // add a placeholder "Default Input" entry with nativeId = kAudioObjectUnknown (0).
    // PlatformOpen() will query kAudioHardwarePropertyDefaultInputDevice at open time;
    // if that also fails, the AUHAL proceeds without an explicit device binding and uses
    // whatever the system default is.  AudioOutputUnitStart() will trigger the macOS
    // permission dialog when authorization is NotDetermined.
    // Once the user grants access, call EnumerateDevices() again for the real device list.
    if (gDevices.empty())
    {
        NSLog(@"[CsoundNativeInput] cni_get_device_count: all devices reported 0 input channels. "
              @"Likely cause: microphone permission not yet granted (macOS 14+/15+). "
              @"Adding placeholder 'Default Input' (id=kAudioObjectUnknown). "
              @"cni_open() will attempt to start the AUHAL, which may trigger the permission dialog. "
              @"Grant access in System Settings -> Privacy & Security -> Microphone, "
              @"then call EnumerateDevices() again for correct channel counts.");
        gDevices.push_back({ kAudioObjectUnknown, "Default Input", 2, 0.f });
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

    // If devId is kAudioObjectUnknown (placeholder added when permission was not granted),
    // try to resolve the real default input device now.  On macOS 14+/15+ this query may
    // succeed even though kAudioDevicePropertyStreamConfiguration was restricted during
    // enumeration.  If it fails, devId stays kAudioObjectUnknown and SetupAudioUnit()
    // will skip the explicit device binding, letting the AUHAL use the system default.
    if (devId == kAudioObjectUnknown)
    {
        AudioObjectPropertyAddress defProp = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(devId);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defProp, 0, nullptr, &sz, &devId);
        NSLog(@"[CsoundNativeInput] PlatformOpen: placeholder device — resolved default input id=%u%s",
              (unsigned)devId,
              devId == kAudioObjectUnknown ? " (still unknown, proceeding without device binding)" : "");
    }

    // Try to set the requested I/O buffer size on the device (skip for unknown id).
    if (devId != kAudioObjectUnknown)
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

    // Two modes, and sampleRate is what picks between them:
    //
    //   <= 0   leave the device alone and capture at whatever rate it is already on. Nothing
    //          outside this process is touched, which is why it is the default: a microphone
    //          shared with every other app on the machine is not ours to retune.
    //   >  0   put the device on that rate, remembering the rate we found it on so cni_close can
    //          put it back. This is what lets a 44.1 kHz microphone feed a 48 kHz Csound with no
    //          resampling anywhere.
    //
    // Either way the AudioUnit format below is built from the rate the device ACTUALLY ends up on,
    // read back from the device rather than assumed. Declaring a rate the hardware is not running
    // at does not buy transparent sample-rate conversion on an AUHAL input bus: it buys
    // kAudioUnitErr_NoConnection (-10863) and a render callback that never fires once. Measured on
    // a MacBook microphone sitting at 44100 while Unity asked for 48000 — the capture never
    // started, and the error said nothing about rates.
    float hwRate = (devId != kAudioObjectUnknown) ? MacGetSampleRate(devId) : 0.f;

    const float rateGap = hwRate > sampleRate ? hwRate - sampleRate : sampleRate - hwRate;
    if (sampleRate > 0.f && devId != kAudioObjectUnknown && hwRate > 0.f && rateGap > 1.f)
    {
        AudioObjectPropertyAddress rateProp = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        Float64 wanted = (Float64)sampleRate;
        if (AudioObjectSetPropertyData(devId, &rateProp, 0, nullptr,
                                       sizeof(wanted), &wanted) == noErr)
        {
            // A restore is owed only now, and only to the rate we found it on.
            s->restoreRate = (double)hwRate;
            s->deviceId    = (uint32_t)devId;

            // The set returns as soon as the request is accepted; the device reconfigures on its own
            // time, and until it has, it still reports the old rate. Building the AudioUnit in that
            // window gives it a format the hardware is not on yet, and the capture never starts —
            // which is exactly what turning this option on did before this wait existed. Poll instead
            // of assuming, with a ceiling so a device that never settles cannot hang the open.
            float settled = MacGetSampleRate(devId);
            for (int waited = 0; waited < 200 && settled > 0.f &&
                                 (settled > sampleRate ? settled - sampleRate
                                                       : sampleRate - settled) > 1.f; waited++)
            {
                usleep(10000);                       // 10 ms, so at most 2 s in total
                settled = MacGetSampleRate(devId);
            }
            NSLog(@"[CsoundNativeInput] device settled at %.0f Hz", (double)settled);
            hwRate = settled;
        }
        NSLog(@"[CsoundNativeInput] asked the device for %.0f Hz, it is now at %.0f Hz%s",
              (double)sampleRate, (double)hwRate,
              s->restoreRate > 0.0 ? " (restored on close)" : " (the device refused, capturing at its own rate)");
    }

    s->sampleRate = hwRate > 0.f ? hwRate : (sampleRate > 0.f ? sampleRate : 44100.f);

    // devId is passed into SetupAudioUnit so it can bind the device BEFORE
    // AudioUnitInitialize (macOS AUHAL requirement).
    // If devId == kAudioObjectUnknown, SetupAudioUnit() skips the binding.
    int err = SetupAudioUnit(s, channelCount, bufferSizeFrames, s->sampleRate, devId);
    if (err != 0) return err;

    // Query the actual buffer frame size from the device (skip for unknown id).
    if (devId != kAudioObjectUnknown)
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
    else
    {
        s->latencyFrames = bufferSizeFrames; // best estimate without hardware info
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

    // 0 means "whatever the session is already on" — the same contract as the macOS branch, where
    // it means "do not retune the device". Here there is nothing to retune, so it simply asks for
    // the rate already in force rather than for 0 Hz.
    if (sampleRate <= 0.f)
    {
        sampleRate = (float)[AVAudioSession sharedInstance].sampleRate;
        if (sampleRate <= 0.f) sampleRate = 48000.f;
    }
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

// Returns the index into the enumerated list of the system's current default input device,
// or -1 when the list is empty. Enumerate first: this reads the same vector as the getters above.
CNI_API int cni_get_default_device_index()
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gDevices.empty()) return -1;

#if TARGET_OS_OSX
    AudioObjectPropertyAddress defProp = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID defId = kAudioObjectUnknown;
    UInt32 sz = sizeof(defId);
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &defProp,
                                             0, nullptr, &sz, &defId);
    if (st == noErr && defId != kAudioObjectUnknown)
        for (int i = 0; i < (int)gDevices.size(); i++)
            if (gDevices[i].nativeId == defId) return i;

    // The query failed, or named a device that enumeration could not see — which is the
    // placeholder case: permission was not granted, so the list holds one entry that
    // resolves to the default at open time anyway.
    return 0;
#else
    // iOS and visionOS capture through the session's current input route, which *is* the
    // system default; the enumeration puts it first.
    return 0;
#endif
}

// Returns the channels the open session actually captures, which is the request limited to
// what the device owns. 0 when closed. Read it after cni_open: on iOS the count is negotiated
// with the audio session, so no capability query predicts it.
CNI_API int cni_get_channel_count()
{
    return gSession.channelCount;
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

#if !TARGET_OS_IOS && !TARGET_OS_VISION
    // Put the device back on the rate we found it on. Only ever set when we changed it, so a
    // session that left the device alone cannot disturb it here either.
    if (gSession.restoreRate > 0.0 && gSession.deviceId != 0)
    {
        AudioObjectPropertyAddress rateProp = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        Float64 back = (Float64)gSession.restoreRate;
        AudioObjectSetPropertyData((AudioDeviceID)gSession.deviceId, &rateProp, 0, nullptr,
                                   sizeof(back), &back);
        NSLog(@"[CsoundNativeInput] device sample rate restored to %.0f Hz", (double)back);
    }
    gSession.restoreRate = 0.0;
    gSession.deviceId    = 0;
#endif

    cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr;
    FreeCaptureBuffers(&gSession);
    gSession.channelCount  = 0;
    gSession.latencyFrames = 0;
    gSession.sampleRate    = 0.f;
    gFramesCaptured.store(0,  std::memory_order_relaxed);
    gLastRenderError.store(0, std::memory_order_relaxed);
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

// Frames the device hands over in one delivery, which is what the ring has to absorb between
// reads, and so what the caller has to size its reserve on. 0 when closed, or before the first
// delivery. Every backend reports the same thing here: not a buffer capacity, not a round trip.
// On these backends the figure is the device's own buffer, which is already one delivery.
CNI_API int cni_get_device_period_frames()
{
    return gSession.running ? gSession.latencyFrames : 0;
}

// Returns total audio frames successfully captured by the CoreAudio callback since cni_open.
// If this stays at 0 while running, check cni_get_last_render_error() for the cause.
CNI_API uint64_t cni_get_frames_captured()
{
    return gFramesCaptured.load(std::memory_order_relaxed);
}

// Returns the last OSStatus error returned by AudioUnitRender inside the capture callback.
// 0 = no error (render is succeeding).
// -10863 (kAudioUnitErr_NoConnection): device was not bound before stream format was set,
//   or microphone permission is denied in System Settings → Privacy & Security → Microphone.
// -10877 (kAudioComponentErr_InstanceInvalidated): device was disconnected.
// Reset to 0 on cni_open / cni_close.
CNI_API int32_t cni_get_last_render_error()
{
    return gLastRenderError.load(std::memory_order_relaxed);
}

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}
