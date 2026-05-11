// cni_aaudio.cpp
// AAudio low-latency audio input plugin for CsoundUnity (Android).
// Exposes the same flat C API as cni_coreaudio.mm so the C# bridge is identical.
// Requires Android API level 26+ (AAudio). Returns error on older devices;
// the C# layer activates the Unity Microphone API fallback in that case.
// Build target: arm64-v8a (primary), armeabi-v7a stub.

#include "cni_ringbuffer.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
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
    AAudioStream*       stream        = nullptr;
    CniRingBuffer*      ringBuffer    = nullptr;
    int                 channelCount  = 0;
    std::atomic<bool>   running       { false };
    int                 latencyFrames = 0;
};

static Session gSession;

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

    // AAudio delivers interleaved float32 samples directly.
    uint32_t total = (uint32_t)(numFrames * s->channelCount);
    cni_rb_write(s->ringBuffer, (const float*)audioData, total);
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
CNI_API int cni_open(int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate)
{
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

    uint32_t ringCap = (uint32_t)(actualFramesPerBurst * actualChannels * kRingBufferMultiplier);
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

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}
