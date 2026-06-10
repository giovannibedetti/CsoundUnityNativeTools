// cni_aaudio.cpp
// AAudio low-latency audio input plugin for CsoundUnity (Android).
// Exposes the same flat C API as cni_coreaudio.mm so the C# bridge is identical.
// Requires Android API level 26+ (AAudio). Returns error on older devices;
// the C# layer activates the Unity Microphone API fallback in that case.
// Build target: arm64-v8a (primary), armeabi-v7a stub.

#include "cni_ringbuffer.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>

#define CNI_API  extern "C" __attribute__((visibility("default")))
#define LOG_TAG  "CsoundNativeInput"
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,   LOG_TAG, __VA_ARGS__)

// Forward declaration
CNI_API void cni_close();
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,   LOG_TAG, __VA_ARGS__)

static const uint32_t kRingBufferMultiplier = 8;

// ---------------------------------------------------------------------------
// Device descriptor
// AAudio doesn't expose per-device names before API 28.
// We synthesise a minimal list: index 0 = default mic, 1..N = USB/BT if available.
// ---------------------------------------------------------------------------

struct DeviceInfo
{
    int         deviceId;   // AAudio device ID (0 = unspecified = default)
    std::string name;
    int         maxChannels;
};

static std::vector<DeviceInfo> gDevices;
static std::mutex              gMutex;

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

struct Session
{
    AAudioStream*           stream        = nullptr;
    CniRingBuffer*          ringBuffer    = nullptr;
    int                     channelCount  = 0;
    std::atomic<bool>       running       { false };
    int                     latencyFrames = 0;
    std::atomic<uint32_t>   underrunCount { 0 };
    std::atomic<uint32_t>   overrunCount  { 0 };
    // Pre-fill: hold off reading until the ring buffer has accumulated this many
    // samples. Absorbs the jitter between Unity's audio-block drain and AAudio's
    // gradual refill, preventing the systematic underruns that happen when Unity
    // reads faster than AAudio can fill at startup.
    std::atomic<bool>       prefilled     { false };
    uint32_t                prefillTarget { 0 };
};

static Session gSession;
static std::atomic<uint64_t> gFramesCaptured { 0 };

// ---------------------------------------------------------------------------
// AAudio data callback (called on a high-priority audio thread by AAudio)
// ---------------------------------------------------------------------------

static aaudio_data_callback_result_t DataCallback(
    AAudioStream* /*stream*/,
    void*          userData,
    void*          audioData,
    int32_t        numFrames)
{
    Session* s = (Session*)userData;
    if (!s->running || !s->ringBuffer) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    // Clamp to whole frames before writing. cni_rb_write truncates to free space,
    // which may not be a multiple of channelCount — a partial-frame write shifts
    // the interleave and permanently swaps L/R for all subsequent reads.
    uint32_t freeFrames = cni_rb_free_space(s->ringBuffer) / (uint32_t)s->channelCount;
    uint32_t frames     = (uint32_t)numFrames;
    if (frames > freeFrames)
    {
        s->overrunCount.fetch_add(1, std::memory_order_relaxed);
        frames = freeFrames;
    }
    if (frames > 0)
    {
        cni_rb_write(s->ringBuffer, (const float*)audioData, frames * (uint32_t)s->channelCount);
        gFramesCaptured.fetch_add(frames, std::memory_order_relaxed);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void ErrorCallback(AAudioStream* /*stream*/, void* userData, aaudio_result_t error)
{
    Session* s = (Session*)userData;
    LOGE("AAudio stream error: %s (%d)", AAudio_convertResultToText(error), error);
    s->running = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// On Android the device list is limited.
// We always include index 0 (default mic) and attempt to detect USB/BT
// devices via AAudioStreamBuilder device ID queries.
CNI_API int cni_get_device_count()
{
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices.clear();

    // Index 0: default microphone (AAudio device ID 0 = AAUDIO_UNSPECIFIED)
    gDevices.push_back({ 0, "Default Microphone", 1 });

    // For API >= 28 we could use AudioManager to enumerate devices,
    // but that requires JNI. For now return only the default mic.
    // This is the safe baseline; USB/BT mics are accessible via deviceId selection
    // once the user provides a concrete device ID from the Android AudioManager.
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
    return gDevices[index].maxChannels;
}

CNI_API float cni_get_device_nominal_sample_rate(int index)
{
    // AAudio selects sample rate automatically to match the hardware preferred rate.
    // Return 0 to indicate "not applicable / auto".
    return 0.f;
}

// Returns 0 on success, negative on failure.
// On failure the C# layer should activate the Unity Microphone API fallback.
// ksmps / exclusiveMode are accepted to keep the C ABI identical to the Windows
// (WASAPI) backend so the shared P/Invoke signature matches on every platform.
// They are unused here: AAudio manages buffer size and there is no
// shared/exclusive distinction.
CNI_API int cni_open(int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate,
                     int ksmps, int exclusiveMode)
{
    (void)ksmps; (void)exclusiveMode;
    if (gSession.running) cni_close();

    {
        std::lock_guard<std::mutex> lock(gMutex);
        // Auto-populate if cni_get_device_count() was never called.
        if (gDevices.empty())
            gDevices.push_back({ 0, "Default Microphone", 1 });
        if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;
    }

    if (channelCount < 1) channelCount = 1;
    if (channelCount > 2) channelCount = 2; // Android mic is typically mono or stereo

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK)
    {
        LOGE("AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(res));
        return -2;
    }

    int deviceId = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        deviceId = gDevices[deviceIndex].deviceId;
    }

    AAudioStreamBuilder_setDirection(builder,        AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder,           AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder,     channelCount);
    AAudioStreamBuilder_setSampleRate(builder,       (int32_t)sampleRate);
    AAudioStreamBuilder_setPerformanceMode(builder,  AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
#if __ANDROID_API__ >= 28
    AAudioStreamBuilder_setInputPreset(builder,      AAUDIO_INPUT_PRESET_UNPROCESSED);
#endif
    AAudioStreamBuilder_setDataCallback(builder,     DataCallback,  &gSession);
    AAudioStreamBuilder_setErrorCallback(builder,    ErrorCallback, &gSession);

    if (deviceId != 0)
        AAudioStreamBuilder_setDeviceId(builder, deviceId);

    // Optionally request a specific buffer size (hint only — AAudio may ignore it).
    if (bufferSizeFrames > 0)
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, bufferSizeFrames * 2);

    // Try exclusive mode first for minimum latency (MMAP path).
    // Falls back to shared mode if the device or OS denies it.
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    res = AAudioStreamBuilder_openStream(builder, &gSession.stream);
    if (res != AAUDIO_OK)
    {
        LOGD("Exclusive mode unavailable (%s), retrying shared.", AAudio_convertResultToText(res));
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        res = AAudioStreamBuilder_openStream(builder, &gSession.stream);
    }
    AAudioStreamBuilder_delete(builder);

    if (res != AAUDIO_OK)
    {
        LOGE("AAudioStreamBuilder_openStream failed: %s", AAudio_convertResultToText(res));
        return -3;
    }

    // Read back the actual parameters chosen by AAudio.
    int32_t actualFramesPerBurst = AAudioStream_getFramesPerBurst(gSession.stream);
    int32_t actualChannels       = AAudioStream_getChannelCount(gSession.stream);
    aaudio_sharing_mode_t actualSharing = AAudioStream_getSharingMode(gSession.stream);

    gSession.channelCount  = actualChannels;
    gSession.latencyFrames = actualFramesPerBurst * 2; // approximate

    // Size the ring buffer so it can hold at least one full Unity DSP block.
    // bufferSizeFrames is max(_requestedBufferFrames, AudioSettings.bufferSize) from C#,
    // so it reflects the actual OnAudioFilterRead block size (e.g. 1024 frames).
    // Without this, a 96-frame burst × 8 = 768-frame ring is smaller than Unity's
    // 1024-frame block, guaranteeing underruns on every audio callback.
    uint32_t ringBase = (uint32_t)std::max({actualFramesPerBurst, ksmps, bufferSizeFrames});
    uint32_t ringCap  = ringBase * (uint32_t)actualChannels * kRingBufferMultiplier;
    gSession.ringBuffer = cni_rb_create(ringCap);
    if (!gSession.ringBuffer)
    {
        AAudioStream_close(gSession.stream);
        gSession.stream = nullptr;
        return -4;
    }

    res = AAudioStream_requestStart(gSession.stream);
    if (res != AAUDIO_OK)
    {
        LOGE("AAudioStream_requestStart failed: %s", AAudio_convertResultToText(res));
        cni_rb_destroy(gSession.ringBuffer);
        AAudioStream_close(gSession.stream);
        gSession.ringBuffer = nullptr;
        gSession.stream     = nullptr;
        return -5;
    }

    gSession.running = true;
    gSession.underrunCount.store(0,     std::memory_order_relaxed);
    gSession.overrunCount.store(0,      std::memory_order_relaxed);
    gSession.prefilled.store(false,     std::memory_order_relaxed);
    // Wait for 2× Unity DSP block before consuming — absorbs scheduling jitter.
    gSession.prefillTarget = (uint32_t)bufferSizeFrames * (uint32_t)actualChannels * 2;
    LOGD("AAudio input opened: %d ch @ %d Hz, burst %d frames, sharing=%s",
         actualChannels,
         (int)AAudioStream_getSampleRate(gSession.stream),
         actualFramesPerBurst,
         (actualSharing == AAUDIO_SHARING_MODE_EXCLUSIVE) ? "EXCLUSIVE" : "SHARED");
    return 0;
}

CNI_API void cni_close()
{
    if (!gSession.running && !gSession.stream) return;
    gSession.running = false;

    if (gSession.stream)
    {
        AAudioStream_requestStop(gSession.stream);
        AAudioStream_close(gSession.stream);
        gSession.stream = nullptr;
    }

    cni_rb_destroy(gSession.ringBuffer);
    gSession.ringBuffer    = nullptr;
    gSession.channelCount  = 0;
    gSession.latencyFrames = 0;
    gSession.underrunCount.store(0,  std::memory_order_relaxed);
    gSession.overrunCount.store(0,   std::memory_order_relaxed);
    gSession.prefilled.store(false,  std::memory_order_relaxed);
    gSession.prefillTarget = 0;
    gFramesCaptured.store(0, std::memory_order_relaxed);
}

CNI_API int cni_read_frames(float* outBuffer, int frameCount, int channelCount)
{
    if (!gSession.running || !gSession.ringBuffer || !outBuffer) return 0;
    uint32_t total = (uint32_t)(frameCount * channelCount);

    // Pre-fill: output silence until the ring buffer has accumulated enough data
    // to cover a full Unity DSP block plus jitter headroom. Without this, Unity
    // drains the buffer faster than AAudio fills it on the first few calls,
    // causing systematic underruns that produce audible clicks.
    if (!gSession.prefilled.load(std::memory_order_relaxed))
    {
        if (cni_rb_available(gSession.ringBuffer) < gSession.prefillTarget)
        {
            memset(outBuffer, 0, total * sizeof(float));
            return 0;
        }
        gSession.prefilled.store(true, std::memory_order_relaxed);
    }

    uint32_t got = cni_rb_read(gSession.ringBuffer, outBuffer, total);
    if (got < total)
        gSession.underrunCount.fetch_add(1, std::memory_order_relaxed);
    return (int)got / channelCount;
}

CNI_API uint32_t cni_get_underrun_count()
{
    return gSession.underrunCount.load(std::memory_order_relaxed);
}

CNI_API uint32_t cni_get_overrun_count()
{
    return gSession.overrunCount.load(std::memory_order_relaxed);
}

CNI_API void cni_reset_xrun_counts()
{
    gSession.underrunCount.store(0, std::memory_order_relaxed);
    gSession.overrunCount.store(0,  std::memory_order_relaxed);
}

CNI_API uint64_t cni_get_frames_captured()
{
    return gFramesCaptured.load(std::memory_order_relaxed);
}

CNI_API int cni_get_input_latency_frames()
{
    return gSession.latencyFrames;
}

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}
