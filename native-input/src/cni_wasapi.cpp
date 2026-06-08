// cni_wasapi.cpp
// WASAPI audio input plugin for CsoundUnity (Windows).
// Exposes the same flat C API as cni_coreaudio.mm and cni_aaudio.cpp.
// Build target: x86_64 DLL (CsoundNativeInput.dll).
//
// Capture runs on a dedicated high-priority thread using shared-mode WASAPI.
// Samples are pushed into a lock-free ring buffer; the Unity audio thread
// reads from it via cni_read_frames().

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>           // AvSetMmThreadCharacteristics
#include <combaseapi.h>
#include <wchar.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>

#include "cni_ringbuffer.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "uuid.lib")

#define CNI_API extern "C" __declspec(dllexport)

static const uint32_t kRingBufferMultiplier = 8;

// ---------------------------------------------------------------------------
// Device descriptor
// ---------------------------------------------------------------------------

struct DeviceInfo
{
    std::wstring id;
    std::string  name;
    int          maxChannels;
    float        nominalSampleRate;
};

static std::vector<DeviceInfo> gDevices;
static std::mutex              gMutex;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

struct Session
{
    IMMDevice*          device        = nullptr;
    IAudioClient*       audioClient   = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    CniRingBuffer*      ringBuffer    = nullptr;
    HANDLE              captureThread = nullptr;
    HANDLE              stopEvent     = nullptr;
    int                 channelCount  = 0;
    int                 latencyFrames = 0;
    std::atomic<bool>   running       { false };
    float               sampleRate    = 0.f;
};

static Session gSession;
static std::atomic<uint64_t> gFramesCaptured { 0 };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], sz, nullptr, nullptr);
    return s;
}

static void SafeRelease(IUnknown** ppUnk)
{
    if (ppUnk && *ppUnk) { (*ppUnk)->Release(); *ppUnk = nullptr; }
}

// ---------------------------------------------------------------------------
// Capture thread
// Calls GetNextPacketSize / GetBuffer / ReleaseBuffer in a tight loop,
// sleeping between packets using the audio client's event handle.
// ---------------------------------------------------------------------------

static DWORD WINAPI CaptureThreadProc(LPVOID param)
{
    Session* s = (Session*)param;

    // Elevate thread to audio priority.
    DWORD taskIndex = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);

    // Each IAudioCaptureClient call must happen on a COM-initialised thread.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (s->running)
    {
        // Wait for new data (audio engine signals every ~10 ms in shared mode).
        DWORD waitResult = WaitForSingleObject(s->stopEvent, 20 /*ms timeout*/);
        if (waitResult == WAIT_OBJECT_0) break; // stop event signalled

        UINT32 packetSize = 0;
        if (FAILED(s->captureClient->GetNextPacketSize(&packetSize))) break;

        while (packetSize > 0)
        {
            BYTE*  pData       = nullptr;
            UINT32 numFrames   = 0;
            DWORD  flags       = 0;

            HRESULT hr = s->captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (pData && numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                // WASAPI shared mode delivers 32-bit float in most modern Windows setups,
                // but the mix format can vary. We requested WAVE_FORMAT_IEEE_FLOAT at Open().
                uint32_t total = numFrames * (uint32_t)s->channelCount;
                cni_rb_write(s->ringBuffer, (const float*)pData, total);
                gFramesCaptured.fetch_add(numFrames, std::memory_order_relaxed);
            }
            else if (numFrames > 0)
            {
                // Silent flag: push zeros to keep the ring buffer advancing.
                uint32_t total = numFrames * (uint32_t)s->channelCount;
                float zeroBuf[4096];
                uint32_t written = 0;
                while (written < total)
                {
                    uint32_t chunk = (total - written < 4096) ? (total - written) : 4096;
                    memset(zeroBuf, 0, chunk * sizeof(float));
                    cni_rb_write(s->ringBuffer, zeroBuf, chunk);
                    written += chunk;
                }
            }

            s->captureClient->ReleaseBuffer(numFrames);
            s->captureClient->GetNextPacketSize(&packetSize);
        }
    }

    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

CNI_API int cni_get_device_count()
{
    std::lock_guard<std::mutex> lock(gMutex);
    gDevices.clear();

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum   = nullptr;
    IMMDeviceCollection* pColl   = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) { CoUninitialize(); return 0; }

    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr)) { SafeRelease((IUnknown**)&pEnum); CoUninitialize(); return 0; }

    UINT count = 0;
    pColl->GetCount(&count);

    for (UINT i = 0; i < count; i++)
    {
        IMMDevice*      pDev  = nullptr;
        IPropertyStore* pProp = nullptr;

        if (FAILED(pColl->Item(i, &pDev))) continue;

        DeviceInfo info;
        info.maxChannels      = 2; // default; refined from mix format below
        info.nominalSampleRate = 0.f;

        // Device ID
        LPWSTR pwszId = nullptr;
        if (SUCCEEDED(pDev->GetId(&pwszId)))
        {
            info.id = pwszId;
            CoTaskMemFree(pwszId);
        }

        // Friendly name
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProp)))
        {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(pProp->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR)
                info.name = WideToUtf8(var.pwszVal);
            PropVariantClear(&var);
        }
        if (info.name.empty()) info.name = "Audio Input Device";

        // Get channel count and sample rate from mix format.
        IAudioClient* pClient = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient)))
        {
            WAVEFORMATEX* pwfx = nullptr;
            if (SUCCEEDED(pClient->GetMixFormat(&pwfx)) && pwfx)
            {
                info.maxChannels       = pwfx->nChannels;
                info.nominalSampleRate = (float)pwfx->nSamplesPerSec;
                CoTaskMemFree(pwfx);
            }
            pClient->Release();
        }

        gDevices.push_back(info);

        SafeRelease((IUnknown**)&pProp);
        SafeRelease((IUnknown**)&pDev);
    }

    SafeRelease((IUnknown**)&pColl);
    SafeRelease((IUnknown**)&pEnum);
    CoUninitialize();
    return (int)gDevices.size();
}

CNI_API void cni_get_device_name(int index, char* outName, int maxLen)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (!outName || maxLen <= 0) return;
    if (index < 0 || index >= (int)gDevices.size()) { outName[0] = '\0'; return; }
    strncpy_s(outName, maxLen, gDevices[index].name.c_str(), _TRUNCATE);
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

CNI_API int cni_open(int deviceIndex, int channelCount, int bufferSizeFrames, float sampleRate)
{
    if (gSession.running) cni_close();

    std::wstring deviceId;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;
        deviceId = gDevices[deviceIndex].id;
        int maxCh = gDevices[deviceIndex].maxChannels;
        if (channelCount < 1)    channelCount = 1;
        if (channelCount > maxCh) channelCount = maxCh;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) { CoUninitialize(); return -2; }

    hr = pEnum->GetDevice(deviceId.c_str(), &gSession.device);
    SafeRelease((IUnknown**)&pEnum);
    if (FAILED(hr)) { CoUninitialize(); return -3; }

    hr = gSession.device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&gSession.audioClient);
    if (FAILED(hr)) { SafeRelease((IUnknown**)&gSession.device); CoUninitialize(); return -4; }

    // Use the device's mix format (shared mode always uses this).
    WAVEFORMATEX* pwfx = nullptr;
    gSession.audioClient->GetMixFormat(&pwfx);
    if (!pwfx) { goto cleanup; }

    // Override channel count and sample rate to match what we need.
    // For shared mode we accept the device's mix format and convert if necessary.
    // Simple path: if the device is already float32, use it directly.
    // Otherwise fall back to whatever the device reports.
    {
        // Build a 32-bit float format at the device's native sample rate.
        WAVEFORMATEXTENSIBLE wfxEx = {};
        wfxEx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfxEx.Format.nChannels       = (WORD)channelCount;
        wfxEx.Format.nSamplesPerSec  = pwfx->nSamplesPerSec;
        wfxEx.Format.wBitsPerSample  = 32;
        wfxEx.Format.nBlockAlign     = (WORD)(channelCount * 4);
        wfxEx.Format.nAvgBytesPerSec = wfxEx.Format.nSamplesPerSec * wfxEx.Format.nBlockAlign;
        wfxEx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfxEx.Samples.wValidBitsPerSample = 32;
        wfxEx.dwChannelMask          = (channelCount == 1) ? SPEAKER_FRONT_CENTER :
                                       (channelCount == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) :
                                                              0;
        wfxEx.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        WAVEFORMATEX* pwfxClosest = nullptr;
        hr = gSession.audioClient->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&wfxEx, &pwfxClosest);

        WAVEFORMATEX* useFormat = (hr == S_OK) ? (WAVEFORMATEX*)&wfxEx : pwfx;
        if (hr == S_FALSE && pwfxClosest) useFormat = pwfxClosest;

        // Convert latency hint to 100-ns units.
        REFERENCE_TIME hnsBufferDuration =
            (REFERENCE_TIME)((double)bufferSizeFrames / useFormat->nSamplesPerSec * 1e7);

        hr = gSession.audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsBufferDuration, 0,
            useFormat, nullptr);

        gSession.channelCount = useFormat->nChannels;
        gSession.sampleRate   = (float)useFormat->nSamplesPerSec;

        if (pwfxClosest) CoTaskMemFree(pwfxClosest);
    }

    CoTaskMemFree(pwfx);
    if (FAILED(hr)) goto cleanup;

    // Event handle for data-ready notification.
    gSession.stopEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gSession.audioClient->SetEventHandle(gSession.stopEvent);

    hr = gSession.audioClient->GetService(
        __uuidof(IAudioCaptureClient), (void**)&gSession.captureClient);
    if (FAILED(hr)) goto cleanup;

    {
        UINT32 bufSize = 0;
        gSession.audioClient->GetBufferSize(&bufSize);
        gSession.latencyFrames = (int)bufSize;

        uint32_t ringCap = bufSize * gSession.channelCount * kRingBufferMultiplier;
        gSession.ringBuffer = cni_rb_create(ringCap);
        if (!gSession.ringBuffer) goto cleanup;
    }

    hr = gSession.audioClient->Start();
    if (FAILED(hr)) goto cleanup;

    gSession.running = true;
    gSession.captureThread = CreateThread(
        nullptr, 0, CaptureThreadProc, &gSession, 0, nullptr);
    if (!gSession.captureThread) { gSession.running = false; gSession.audioClient->Stop(); goto cleanup; }

    CoUninitialize();
    return 0;

cleanup:
    SafeRelease((IUnknown**)&gSession.captureClient);
    SafeRelease((IUnknown**)&gSession.audioClient);
    SafeRelease((IUnknown**)&gSession.device);
    if (gSession.stopEvent)  { CloseHandle(gSession.stopEvent);  gSession.stopEvent  = nullptr; }
    if (gSession.ringBuffer) { cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr; }
    CoUninitialize();
    return -10;
}

CNI_API void cni_close()
{
    if (!gSession.running && !gSession.captureThread) return;

    gSession.running = false;
    if (gSession.stopEvent)
        SetEvent(gSession.stopEvent); // wake capture thread so it exits

    if (gSession.captureThread)
    {
        WaitForSingleObject(gSession.captureThread, 2000);
        CloseHandle(gSession.captureThread);
        gSession.captureThread = nullptr;
    }

    if (gSession.audioClient) gSession.audioClient->Stop();

    SafeRelease((IUnknown**)&gSession.captureClient);
    SafeRelease((IUnknown**)&gSession.audioClient);
    SafeRelease((IUnknown**)&gSession.device);

    if (gSession.stopEvent) { CloseHandle(gSession.stopEvent); gSession.stopEvent = nullptr; }
    cni_rb_destroy(gSession.ringBuffer);
    gSession.ringBuffer    = nullptr;
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

CNI_API int cni_is_running()
{
    return gSession.running ? 1 : 0;
}

CNI_API uint64_t cni_get_frames_captured()
{
    return gFramesCaptured.load(std::memory_order_relaxed);
}
