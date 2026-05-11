// cni_coreaudio.mm
// CoreAudio multi-channel audio input plugin for CsoundUnity (macOS).
// Exposes a flat C API callable from C# via P/Invoke.
// Build target: universal macOS bundle (arm64 + x86_64), C++17 or later.

#include "cni_ringbuffer.h"
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <Foundation/Foundation.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>

#define CNI_API extern "C" __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static const uint32_t kRingBufferMultiplier = 8; // ring = 8× requested buffer size

struct DeviceInfo
{
    AudioDeviceID deviceId;
    std::string   name;
    int           maxInputChannels;
    float         nominalSampleRate;
};

static std::vector<DeviceInfo> gDevices;
static std::mutex              gMutex;      // protects gDevices enumeration

struct Session
{
    AudioDeviceID       deviceId       = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId       = nullptr;
    CniRingBuffer*      ringBuffer     = nullptr;
    int                 channelCount   = 0;
    std::atomic<bool>   running        { false };
    int                 latencyFrames  = 0;
};

static Session gSession;

// ---------------------------------------------------------------------------
// Device enumeration helpers
// ---------------------------------------------------------------------------

static std::string GetDeviceName(AudioDeviceID deviceId)
{
    AudioObjectPropertyAddress prop = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef cfName = nullptr;
    UInt32 size = sizeof(cfName);
    OSStatus err = AudioObjectGetPropertyData(deviceId, &prop, 0, nullptr, &size, &cfName);
    if (err != noErr || !cfName) return "Unknown";

    char buf[256];
    CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cfName);
    return std::string(buf);
}

static int GetInputChannelCount(AudioDeviceID deviceId)
{
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(deviceId, &prop, 0, nullptr, &size);
    if (err != noErr || size == 0) return 0;

    AudioBufferList* list = (AudioBufferList*)malloc(size);
    err = AudioObjectGetPropertyData(deviceId, &prop, 0, nullptr, &size, list);
    int total = 0;
    if (err == noErr)
        for (UInt32 i = 0; i < list->mNumberBuffers; i++)
            total += list->mBuffers[i].mNumberChannels;
    free(list);
    return total;
}

static float GetNominalSampleRate(AudioDeviceID deviceId)
{
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(deviceId, &prop, 0, nullptr, &size, &rate);
    return (float)rate;
}

// ---------------------------------------------------------------------------
// IO callback
// ---------------------------------------------------------------------------

static OSStatus IOCallback(AudioDeviceID           /*inDevice*/,
                           const AudioTimeStamp*   /*inNow*/,
                           const AudioBufferList*   inInputData,
                           const AudioTimeStamp*   /*inInputTime*/,
                           AudioBufferList*        /*outOutputData*/,
                           const AudioTimeStamp*   /*inOutputTime*/,
                           void*                   inClientData)
{
    Session* s = (Session*)inClientData;
    if (!s->running || !s->ringBuffer || !inInputData || inInputData->mNumberBuffers == 0)
        return noErr;

    // CoreAudio delivers non-interleaved float32 buffers, one per channel.
    // We interleave them into the ring buffer.
    int channels   = s->channelCount;
    UInt32 frames  = inInputData->mBuffers[0].mDataByteSize / sizeof(float);
    int bufSize    = channels * frames;

    // Stack-allocate up to 8192 samples to avoid heap in the callback.
    float  stackBuf[8192];
    float* tmp = (bufSize <= 8192) ? stackBuf : (float*)malloc(bufSize * sizeof(float));
    if (!tmp) return noErr;

    for (UInt32 frame = 0; frame < frames; frame++)
        for (int ch = 0; ch < channels && ch < (int)inInputData->mNumberBuffers; ch++)
        {
            float* src = (float*)inInputData->mBuffers[ch].mData;
            tmp[frame * channels + ch] = src ? src[frame] : 0.f;
        }

    cni_rb_write(s->ringBuffer, tmp, (uint32_t)bufSize);

    if (bufSize > 8192) free(tmp);
    return noErr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

CNI_API int cni_get_device_count()
{
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices.clear();

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &size) != noErr)
        return 0;

    int count = (int)(size / sizeof(AudioDeviceID));
    std::vector<AudioDeviceID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, ids.data()) != noErr)
        return 0;

    for (auto id : ids)
    {
        int ch = GetInputChannelCount(id);
        if (ch > 0)
            gDevices.push_back({ id, GetDeviceName(id), ch, GetNominalSampleRate(id) });
    }
    return (int)gDevices.size();
}

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
    return gDevices[index].maxInputChannels;
}

CNI_API float cni_get_device_nominal_sample_rate(int index)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= (int)gDevices.size()) return 0.f;
    return gDevices[index].nominalSampleRate;
}

// Returns 0 on success, negative error code on failure.
CNI_API int cni_open(int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate)
{
    if (gSession.running) cni_close();

    std::lock_guard<std::mutex> lock(gMutex);
    if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;

    AudioDeviceID devId = gDevices[deviceIndex].deviceId;
    int maxCh           = gDevices[deviceIndex].maxInputChannels;
    if (channelCount < 1) channelCount = 1;
    if (channelCount > maxCh) channelCount = maxCh;

    // Try to set the requested buffer size (latency).
    {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        // Query allowed range first.
        AudioObjectPropertyAddress rangeProp = {
            kAudioDevicePropertyBufferFrameSizeRange,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioValueRange range = { 0, 0 };
        UInt32 rangeSize = sizeof(range);
        AudioObjectGetPropertyData(devId, &rangeProp, 0, nullptr, &rangeSize, &range);

        UInt32 bfs = (UInt32)bufferSizeFrames;
        if (range.mMaximum > 0)
        {
            if (bfs < (UInt32)range.mMinimum) bfs = (UInt32)range.mMinimum;
            if (bfs > (UInt32)range.mMaximum) bfs = (UInt32)range.mMaximum;
        }
        UInt32 propSize = sizeof(bfs);
        AudioObjectSetPropertyData(devId, &prop, 0, nullptr, propSize, &bfs);
        // Read back the actual value.
        AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &propSize, &bfs);
        bufferSizeFrames = (int)bfs;
    }

    // Allocate ring buffer: kRingBufferMultiplier × (bufferSize × channelCount).
    uint32_t ringCap = (uint32_t)(bufferSizeFrames * channelCount * kRingBufferMultiplier);
    gSession.ringBuffer = cni_rb_create(ringCap);
    if (!gSession.ringBuffer) return -2;

    gSession.deviceId     = devId;
    gSession.channelCount = channelCount;

    // Query device latency.
    {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyLatency,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 latency = 0, sz = sizeof(latency);
        AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &sz, &latency);
        gSession.latencyFrames = (int)latency + bufferSizeFrames;
    }

    // Register IO callback and start the device.
    OSStatus err = AudioDeviceCreateIOProcID(devId, IOCallback, &gSession, &gSession.ioProcId);
    if (err != noErr) { cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr; return -3; }

    err = AudioDeviceStart(devId, gSession.ioProcId);
    if (err != noErr)
    {
        AudioDeviceDestroyIOProcID(devId, gSession.ioProcId);
        cni_rb_destroy(gSession.ringBuffer);
        gSession.ringBuffer = nullptr;
        gSession.ioProcId   = nullptr;
        return -4;
    }

    gSession.running = true;
    return 0;
}

CNI_API void cni_close()
{
    if (!gSession.running) return;
    gSession.running = false;

    if (gSession.deviceId != kAudioObjectUnknown && gSession.ioProcId)
    {
        AudioDeviceStop(gSession.deviceId, gSession.ioProcId);
        AudioDeviceDestroyIOProcID(gSession.deviceId, gSession.ioProcId);
        gSession.ioProcId = nullptr;
    }

    cni_rb_destroy(gSession.ringBuffer);
    gSession.ringBuffer    = nullptr;
    gSession.deviceId      = kAudioObjectUnknown;
    gSession.channelCount  = 0;
    gSession.latencyFrames = 0;
}

// Called from the Unity audio thread.
// Reads exactly frameCount * channelCount interleaved samples into outBuffer.
// Zero-fills on underrun.
// Returns the number of frames actually read from the ring buffer.
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

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}
