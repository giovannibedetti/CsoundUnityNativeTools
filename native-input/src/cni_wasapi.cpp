// cni_wasapi.cpp
// WASAPI audio input plugin for CsoundUnity (Windows).
// Exposes the same flat C API as cni_coreaudio.mm and cni_aaudio.cpp.
// Build target: x86_64 DLL (CsoundNativeInput.dll).
//
// Architecture: event-driven capture thread + lock-free ring buffer.
//
//   capture thread ──WASAPI event──> ring buffer (16 × period)
//   Unity audio thread ──cni_read_frames()──> ring buffer ──> spin
//
// IAudioClient3 (Windows 10+) targets an engine period of ksmps frames,
// rounded to the nearest valid multiple (LCM(ksmps, fundPer) when
// ksmps < fundPer, otherwise ceil(ksmps/fundPer)×fundPer). The WASAPI
// event then fires exactly once per period, delivering an exact integer
// multiple of ksmps frames — both sides run at the same rate so the ring
// buffer stays at ≈ 0-1 period of fill (latency contribution ≈ half a period).
//
// Classic fallback (IAudioClient on Windows 7/8 / old drivers): uses
// bufferSizeFrames as the period hint. Ring buffer is sized generously.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <combaseapi.h>
#include <wchar.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "cni_ringbuffer.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "uuid.lib")

#define CNI_API extern "C" __declspec(dllexport)

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
    IMMDevice*           device             = nullptr;
    IAudioClient*        audioClient        = nullptr;
    IAudioCaptureClient* captureClient      = nullptr;
    CniRingBuffer*       ringBuffer         = nullptr;
    HANDLE               captureThread      = nullptr;
    HANDLE               stopEvent          = nullptr; // manual-reset, set by cni_close
    HANDLE               readyEvent         = nullptr; // auto-reset, set by WASAPI
    int                  channelCount       = 0;       // requested channels (ring-buffer stride)
    int                  deviceChannelCount = 0;       // actual WASAPI mix-format channels
    int                  latencyFrames      = 0;
    std::atomic<bool>    running            { false };
    float                sampleRate         = 0.f;
};

static Session               gSession;
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

// Greatest-common-divisor (Euclidean). Used by cni_open for LCM period alignment.
static inline UINT32 ugcd(UINT32 a, UINT32 b) { while (b) { UINT32 t = a % b; a = b; b = t; } return a; }

// ---------------------------------------------------------------------------
// Capture thread
// Wakes on the WASAPI ready event (fires once per engine period).
// Extracts the requested channels and pushes them into the ring buffer.
// ---------------------------------------------------------------------------

static DWORD WINAPI CaptureThreadProc(LPVOID param)
{
    Session* s = (Session*)param;

    DWORD taskIndex = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HANDLE events[2] = { s->stopEvent, s->readyEvent };

    while (s->running)
    {
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 20);
        if (waitResult == WAIT_OBJECT_0) break; // stopEvent
        if (waitResult == WAIT_TIMEOUT)  continue;

        UINT32 packetFrames = 0;
        if (FAILED(s->captureClient->GetNextPacketSize(&packetFrames))) break;

        while (packetFrames > 0)
        {
            BYTE*  pData     = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;

            HRESULT hr = s->captureClient->GetBuffer(
                &pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (pData && numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                const int devCh = s->deviceChannelCount;
                const int reqCh = s->channelCount;

                if (devCh == reqCh)
                {
                    cni_rb_write(s->ringBuffer,
                                 (const float*)pData,
                                 numFrames * (uint32_t)reqCh);
                }
                else
                {
                    // Extract the first reqCh channels from each interleaved frame.
                    const float* src        = (const float*)pData;
                    const int    batchFrames = 512 / reqCh;
                    float        tmp[512];
                    uint32_t     left = numFrames, done = 0;
                    while (left > 0)
                    {
                        int batch = ((int)left < batchFrames) ? (int)left : batchFrames;
                        for (int fi = 0; fi < batch; fi++)
                            for (int c = 0; c < reqCh; c++)
                                tmp[fi * reqCh + c] = src[(done + fi) * devCh + c];
                        cni_rb_write(s->ringBuffer, tmp, (uint32_t)(batch * reqCh));
                        done += batch;
                        left -= batch;
                    }
                }
                gFramesCaptured += numFrames;
            }
            else if (numFrames > 0)
            {
                // Silent flag — push zeros to keep the ring buffer in sync.
                uint32_t total = numFrames * (uint32_t)s->channelCount;
                float     zeroBuf[4096];
                uint32_t  written = 0;
                while (written < total)
                {
                    uint32_t chunk = (total - written < 4096) ? (total - written) : 4096;
                    memset(zeroBuf, 0, chunk * sizeof(float));
                    cni_rb_write(s->ringBuffer, zeroBuf, chunk);
                    written += chunk;
                }
            }

            s->captureClient->ReleaseBuffer(numFrames);
            s->captureClient->GetNextPacketSize(&packetFrames);
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

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDeviceCollection* pColl = nullptr;

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
        info.maxChannels       = 2;
        info.nominalSampleRate = 0.f;

        LPWSTR pwszId = nullptr;
        if (SUCCEEDED(pDev->GetId(&pwszId))) { info.id = pwszId; CoTaskMemFree(pwszId); }

        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProp)))
        {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(pProp->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR)
                info.name = WideToUtf8(var.pwszVal);
            PropVariantClear(&var);
        }
        if (info.name.empty()) info.name = "Audio Input Device";

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

CNI_API void cni_close();

// ---------------------------------------------------------------------------
// cni_open
//
// Opens WASAPI capture in shared mode with event-driven delivery.
// Tries IAudioClient3 (Windows 10+) to set the engine period to ksmps
// frames — the WASAPI event then fires exactly once per ksmps cycle,
// matching the Csound processing cadence for minimum latency.
// Falls back to classic IAudioClient on older drivers.
//
// Parameters:
//   deviceIndex     – index from the last cni_get_device_count() call
//   channelCount    – number of input channels to return to the caller
//   bufferSizeFrames– latency hint for the classic fallback path
//   sampleRate      – must match Unity's output rate
//   ksmps           – Csound ksmps; target for the IAudioClient3 period
// ---------------------------------------------------------------------------
CNI_API int cni_open(int deviceIndex, int channelCount,
                     int bufferSizeFrames, float sampleRate, int ksmps)
{
    if (gSession.running) cni_close();
    gFramesCaptured = 0;

    if (ksmps <= 0) ksmps = 128;

    std::wstring deviceId;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (deviceIndex < 0 || deviceIndex >= (int)gDevices.size()) return -1;
        deviceId = gDevices[deviceIndex].id;
        int maxCh = gDevices[deviceIndex].maxChannels;
        if (channelCount < 1)     channelCount = 1;
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

    WAVEFORMATEX* pwfx = nullptr;
    bool   usedClient3   = false;
    UINT32 client3Period = 0;

    gSession.audioClient->GetMixFormat(&pwfx);
    if (!pwfx) goto cleanup;

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
        wfxEx.dwChannelMask = (channelCount == 1) ? SPEAKER_FRONT_CENTER :
                              (channelCount == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        wfxEx.SubFormat     = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        WAVEFORMATEX* pwfxClosest = nullptr;
        hr = gSession.audioClient->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&wfxEx, &pwfxClosest);

        WAVEFORMATEX* useFormat = (hr == S_OK) ? (WAVEFORMATEX*)&wfxEx : pwfx;
        if (hr == S_FALSE && pwfxClosest) useFormat = pwfxClosest;

        // deviceChannelCount: what WASAPI actually delivers per frame.
        // channelCount: what the caller wants (ring-buffer stride).
        // When the device's shared-mode format is stereo but the caller
        // requests mono, these differ and the capture thread extracts
        // only the first channelCount channels from each frame.
        gSession.deviceChannelCount = useFormat->nChannels;
        gSession.channelCount       = channelCount;
        gSession.sampleRate         = (float)useFormat->nSamplesPerSec;

        // ----------------------------------------------------------------
        // IAudioClient3 path (Windows 10+): target period = ksmps.
        // The WASAPI event fires exactly once per ksmps frames, perfectly
        // matching the Csound processing cadence.
        // ----------------------------------------------------------------
        {
            IAudioClient3* pAC3 = nullptr;
            if (SUCCEEDED(gSession.audioClient->QueryInterface(
                    __uuidof(IAudioClient3), (void**)&pAC3)))
            {
                UINT32 defPer = 0, fundPer = 0, minPer = 0, maxPer = 0;
                HRESULT hrPer = pAC3->GetSharedModeEnginePeriod(
                    useFormat, &defPer, &fundPer, &minPer, &maxPer);

                if (SUCCEEDED(hrPer))
                {
                    UINT32 wantPer = (UINT32)ksmps;
                    if (fundPer > 1)
                    {
                        if (wantPer < fundPer)
                        {
                            // ksmps is smaller than the engine's fundamental period step.
                            // Use LCM(ksmps, fundPer) so the WASAPI event delivers an
                            // exact integer multiple of ksmps frames each time — this
                            // eliminates the ring-buffer level oscillation that causes
                            // pops at low ksmps values (e.g. ksmps=32, fundPer=48 → 96).
                            UINT32 g = ugcd(wantPer, fundPer);
                            wantPer  = (wantPer / g) * fundPer;
                        }
                        else
                        {
                            // ksmps >= fundPer: round up to the nearest fundPer multiple.
                            wantPer = ((wantPer + fundPer - 1) / fundPer) * fundPer;
                        }
                    }
                    if (wantPer < minPer) wantPer = minPer;
                    if (wantPer > maxPer) wantPer = maxPer;

                    HRESULT hrInit = pAC3->InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, wantPer, useFormat, nullptr);

                    if (SUCCEEDED(hrInit)) { client3Period = wantPer; usedClient3 = true; }
                }
                pAC3->Release();
            }
        }

        if (!usedClient3)
        {
            REFERENCE_TIME hnsBuffer =
                (REFERENCE_TIME)((double)bufferSizeFrames / useFormat->nSamplesPerSec * 1e7);
            hr = gSession.audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                hnsBuffer, 0, useFormat, nullptr);
        }
        else
        {
            hr = S_OK;
        }

        if (pwfxClosest) CoTaskMemFree(pwfxClosest);
    }

    CoTaskMemFree(pwfx);
    if (FAILED(hr)) goto cleanup;

    gSession.stopEvent  = CreateEvent(nullptr, TRUE,  FALSE, nullptr);
    gSession.readyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gSession.audioClient->SetEventHandle(gSession.readyEvent);

    hr = gSession.audioClient->GetService(
        __uuidof(IAudioCaptureClient), (void**)&gSession.captureClient);
    if (FAILED(hr)) goto cleanup;

    {
        UINT32 bufSize = 0;
        gSession.audioClient->GetBufferSize(&bufSize);
        gSession.latencyFrames = usedClient3 ? (int)client3Period : (int)bufSize;

        // Ring buffer: 16× the engine period in samples.
        // The steady-state fill level is ≈ 0–1 period (latency-neutral); the
        // extra headroom absorbs OS scheduling jitter between the WASAPI event
        // thread and the Unity audio thread without causing underruns.
        uint32_t period = usedClient3 ? client3Period : (uint32_t)bufSize;
        uint32_t ringCap = period * (uint32_t)gSession.channelCount * 16;
        gSession.ringBuffer = cni_rb_create(ringCap);
        if (!gSession.ringBuffer) goto cleanup;
    }

    hr = gSession.audioClient->Start();
    if (FAILED(hr)) goto cleanup;

    gSession.running = true;
    gSession.captureThread = CreateThread(
        nullptr, 0, CaptureThreadProc, &gSession, 0, nullptr);
    if (!gSession.captureThread)
    {
        gSession.running = false;
        gSession.audioClient->Stop();
        goto cleanup;
    }

    CoUninitialize();
    return 0;

cleanup:
    SafeRelease((IUnknown**)&gSession.captureClient);
    SafeRelease((IUnknown**)&gSession.audioClient);
    SafeRelease((IUnknown**)&gSession.device);
    if (gSession.stopEvent)  { CloseHandle(gSession.stopEvent);  gSession.stopEvent  = nullptr; }
    if (gSession.readyEvent) { CloseHandle(gSession.readyEvent); gSession.readyEvent = nullptr; }
    if (gSession.ringBuffer) { cni_rb_destroy(gSession.ringBuffer); gSession.ringBuffer = nullptr; }
    CoUninitialize();
    return -10;
}

CNI_API void cni_close()
{
    if (!gSession.running && !gSession.captureThread) return;

    gSession.running = false;
    if (gSession.stopEvent)
        SetEvent(gSession.stopEvent);

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

    if (gSession.stopEvent)  { CloseHandle(gSession.stopEvent);  gSession.stopEvent  = nullptr; }
    if (gSession.readyEvent) { CloseHandle(gSession.readyEvent); gSession.readyEvent = nullptr; }
    cni_rb_destroy(gSession.ringBuffer);
    gSession.ringBuffer         = nullptr;
    gSession.channelCount       = 0;
    gSession.deviceChannelCount = 0;
    gSession.latencyFrames      = 0;
    gSession.sampleRate         = 0.f;
}

// ---------------------------------------------------------------------------
// cni_read_frames
//
// Called by the Unity audio thread once per ksmps cycle, before PerformKsmps.
// Reads frameCount × channelCount interleaved floats from the ring buffer.
// Zero-fills on underrun.
// ---------------------------------------------------------------------------
CNI_API int cni_read_frames(float* outBuffer, int frameCount, int channelCount)
{
    if (!gSession.running || !gSession.ringBuffer || !outBuffer) return 0;
    uint32_t total = (uint32_t)(frameCount * channelCount);
    return (int)cni_rb_read(gSession.ringBuffer, outBuffer, total) / channelCount;
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
    return gFramesCaptured.load();
}
