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

// How the device delivers raw samples. Shared mode is always float32; exclusive
// mode may hand back integer PCM, which the capture thread converts to float.
enum SampleFmt { SF_FLOAT32 = 0, SF_INT32, SF_INT24, SF_INT16 };

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
    int                  deviceChannelCount = 0;       // actual WASAPI device-format channels
    int                  sampleFmt          = SF_FLOAT32; // raw device sample format
    int                  bytesPerSample     = 4;        // raw device sample size in bytes
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

// Convert one raw sample (at byte pointer p) to normalised float [-1, 1].
static inline float cniSampleToFloat(const BYTE* p, int sf)
{
    switch (sf)
    {
        case SF_FLOAT32: return *(const float*)p;
        case SF_INT32:   return (float)(*(const int32_t*)p) * (1.0f / 2147483648.0f);
        case SF_INT24:
        {
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x00800000) v |= ~0x00FFFFFF; // sign-extend 24→32
            return (float)v * (1.0f / 8388608.0f);
        }
        case SF_INT16:   return (float)(*(const int16_t*)p) * (1.0f / 32768.0f);
    }
    return 0.f;
}

// Classify a WAVEFORMATEX into a SampleFmt + byte size. Returns false if the
// format is one we cannot read directly (caller should reject it).
static bool cniClassifyFormat(const WAVEFORMATEX* f, int* sf, int* bps)
{
    bool isFloat = false, isPcm = false;
    WORD bits = f->wBitsPerSample;

    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)      isFloat = true;
    else if (f->wFormatTag == WAVE_FORMAT_PCM)        isPcm   = true;
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)f;
        if (IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) isFloat = true;
        else if (IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))   isPcm   = true;
    }

    if (isFloat && bits == 32) { *sf = SF_FLOAT32; *bps = 4; return true; }
    if (isPcm)
    {
        if (bits == 32) { *sf = SF_INT32; *bps = 4; return true; }
        if (bits == 24) { *sf = SF_INT24; *bps = 3; return true; }
        if (bits == 16) { *sf = SF_INT16; *bps = 2; return true; }
    }
    return false;
}

// Convert + channel-extract a packet into the ring buffer. Reads numFrames of
// devCh samples each (raw format sf/bps), writes the first reqCh channels per
// frame as float. Handles both devCh == reqCh and devCh > reqCh uniformly.
static void cniConvertExtractWrite(Session* s, const BYTE* data, uint32_t numFrames)
{
    const int sf    = s->sampleFmt;
    const int bps   = s->bytesPerSample;
    const int devCh = s->deviceChannelCount;
    const int reqCh = s->channelCount;
    const int frameBytes = devCh * bps;
    if (reqCh <= 0) return;

    // Write only WHOLE frames. The ring buffer holds floats and clamps a write to
    // the free space, which (capacity is a power of two, mask is odd) can be an odd
    // sample count. A partial-frame write would shift the interleaved stream by
    // half a frame and permanently swap L/R. So drop any excess up front, keeping
    // every cni_rb_write frame-aligned.
    uint32_t freeFrames = cni_rb_free_space(s->ringBuffer) / (uint32_t)reqCh;
    if (numFrames > freeFrames) numFrames = freeFrames;
    if (numFrames == 0) return;

    const int batchFrames = 512 / (reqCh > 0 ? reqCh : 1);
    float     tmp[512];
    uint32_t  left = numFrames, done = 0;
    while (left > 0)
    {
        int batch = ((int)left < batchFrames) ? (int)left : batchFrames;
        for (int fi = 0; fi < batch; fi++)
        {
            const BYTE* frame = data + (size_t)(done + fi) * frameBytes;
            for (int c = 0; c < reqCh; c++)
                tmp[fi * reqCh + c] = cniSampleToFloat(frame + (size_t)c * bps, sf);
        }
        cni_rb_write(s->ringBuffer, tmp, (uint32_t)(batch * reqCh));
        done += (uint32_t)batch;
        left -= (uint32_t)batch;
    }
}

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
        // Wake on either event with a 10 ms safety timeout. In event-driven mode
        // (EVENTCALLBACK) readyEvent fires once per engine period. In timer-driven
        // mode (resample path, no EVENTCALLBACK) readyEvent never fires and the
        // timeout drives draining. Either way we drain all available packets below,
        // so a timeout is not skipped — only the stopEvent breaks the loop.
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10);
        if (waitResult == WAIT_OBJECT_0) break; // stopEvent (index 0)

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
                // Convert the raw device format to float and extract the requested
                // channels (handles float32 shared mode and integer exclusive mode).
                cniConvertExtractWrite(s, pData, numFrames);
                gFramesCaptured += numFrames;
            }
            else if (numFrames > 0 && s->channelCount > 0)
            {
                // Silent flag — push frame-aligned zeros to keep the ring buffer in
                // sync. Clamp to whole frames that fit so no cni_rb_write is
                // truncated to an odd sample count (which would swap L/R).
                const uint32_t reqCh = (uint32_t)s->channelCount;
                uint32_t freeFrames = cni_rb_free_space(s->ringBuffer) / reqCh;
                uint32_t frames     = (numFrames < freeFrames) ? numFrames : freeFrames;
                uint32_t total      = frames * reqCh;
                float    zeroBuf[512] = { 0 };
                uint32_t written = 0;
                while (written < total)
                {
                    uint32_t chunk = (total - written < 512) ? (total - written) : 512;
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
    bool   eventDriven   = true; // false when the classic path falls back to timer-driven
    UINT32 client3Period = 0;
    int    errCode       = -10; // updated at each failure point for diagnostics


    gSession.audioClient->GetMixFormat(&pwfx);
    if (!pwfx) { errCode = -5; goto cleanup; }


    {
        // Determine the device's native (engine) sample rate and whether it
        // matches what Unity/Csound expects.
        DWORD wantedSr   = (sampleRate > 0.f) ? (DWORD)sampleRate : pwfx->nSamplesPerSec;
        bool  ratesMatch = (pwfx->nSamplesPerSec == wantedSr);

        // Channel mask of the device's mix format (preserved when resampling).
        DWORD devMask = 0;
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            devMask = ((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask;

        WAVEFORMATEXTENSIBLE wfxEx       = {};
        WAVEFORMATEXTENSIBLE exWfx       = {}; // exclusive-mode format (if used)
        WAVEFORMATEX*        pwfxClosest = nullptr;
        WAVEFORMATEX*        useFormat   = nullptr;
        bool                 tryClient3  = false;

        if (ratesMatch)
        {
            // Device already runs at Unity's rate — no resampling needed.
            // Probe our requested format; on S_FALSE fall back to the device's
            // closest (usually the native channel count at the same rate) and
            // extract the requested channels in the capture thread.
            // IAudioClient3 low-latency is eligible on this path.
            wfxEx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            wfxEx.Format.nChannels       = (WORD)channelCount;
            wfxEx.Format.nSamplesPerSec  = wantedSr;
            wfxEx.Format.wBitsPerSample  = 32;
            wfxEx.Format.nBlockAlign     = (WORD)(channelCount * 4);
            wfxEx.Format.nAvgBytesPerSec = wantedSr * (DWORD)wfxEx.Format.nBlockAlign;
            wfxEx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfxEx.Samples.wValidBitsPerSample = 32;
            wfxEx.dwChannelMask = (channelCount == 1) ? SPEAKER_FRONT_CENTER :
                                  (channelCount == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
            wfxEx.SubFormat     = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            hr = gSession.audioClient->IsFormatSupported(
                AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&wfxEx, &pwfxClosest);

            useFormat = (hr == S_OK) ? (WAVEFORMATEX*)&wfxEx : pwfx;
            if (hr == S_FALSE && pwfxClosest) useFormat = pwfxClosest;
            tryClient3 = true;
        }
        else
        {
            // Device runs at a different rate (e.g. 44100) than Unity (48000).
            // Ask the shared-mode engine for the device's native channel layout
            // at Unity's rate in 32-bit float — the engine resamples
            // device→Unity transparently. IAudioClient3 cannot resample, so this
            // path uses the classic shared-mode client (stable, slightly higher
            // latency). We capture all device channels and extract the requested
            // ones, so discrete inputs are preserved (no downmix).
            WORD devCh = pwfx->nChannels;
            wfxEx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            wfxEx.Format.nChannels       = devCh;
            wfxEx.Format.nSamplesPerSec  = wantedSr;
            wfxEx.Format.wBitsPerSample  = 32;
            wfxEx.Format.nBlockAlign     = (WORD)(devCh * 4);
            wfxEx.Format.nAvgBytesPerSec = wantedSr * (DWORD)wfxEx.Format.nBlockAlign;
            wfxEx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfxEx.Samples.wValidBitsPerSample = 32;
            wfxEx.dwChannelMask = devMask ? devMask :
                                  (devCh == 1) ? SPEAKER_FRONT_CENTER :
                                  (devCh == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
            wfxEx.SubFormat     = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            useFormat  = (WAVEFORMATEX*)&wfxEx;
            tryClient3 = false;
        }

        // Defensive: useFormat must be set by one of the branches above.
        if (!useFormat) { errCode = -6; goto cleanup; }

        // deviceChannelCount: what WASAPI delivers per frame (ring-buffer write
        // stride). channelCount: what the caller wants (ring-buffer read stride).
        // When they differ the capture thread extracts only the first
        // channelCount channels from each interleaved frame.
        gSession.deviceChannelCount = useFormat->nChannels;
        gSession.channelCount       = channelCount;
        gSession.sampleRate         = (float)useFormat->nSamplesPerSec;


        // ----------------------------------------------------------------
        // IAudioClient3 path (Windows 10+): target period = ksmps.
        // The WASAPI event fires exactly once per ksmps frames, perfectly
        // matching the Csound processing cadence. Only attempted when the
        // device runs at Unity's rate (IAudioClient3 cannot resample).
        // ----------------------------------------------------------------
        if (tryClient3)
        {
            IAudioClient3* pAC3 = nullptr;
            HRESULT hrQI = gSession.audioClient->QueryInterface(
                    __uuidof(IAudioClient3), (void**)&pAC3);
            if (SUCCEEDED(hrQI))
            {
                UINT32 defPer = 0, fundPer = 0, minPer = 0, maxPer = 0;
                HRESULT hrPer = pAC3->GetSharedModeEnginePeriod(
                    useFormat, &defPer, &fundPer, &minPer, &maxPer);

                if (SUCCEEDED(hrPer))
                {
                    // Choose the engine period so that it is an exact multiple of ksmps.
                    // This guarantees every WASAPI event delivers an integer number of
                    // ksmps frames, keeping the ring-buffer level perfectly stable.
                    //
                    // Strategy: start from ceil(ksmps/fundPer)*fundPer (smallest valid
                    // period >= ksmps). If that is not already a multiple of ksmps, use
                    // LCM(ksmps, fundPer) instead. Cap at 8×ksmps to avoid unreasonably
                    // long periods on drivers with unusual fundPer values.
                    UINT32 wantPer = (UINT32)ksmps;
                    if (fundPer > 1)
                    {
                        UINT32 rounded = ((wantPer + fundPer - 1) / fundPer) * fundPer;
                        if (rounded % wantPer == 0)
                        {
                            wantPer = rounded; // already an exact multiple — no jitter
                        }
                        else
                        {
                            UINT32 g      = ugcd(wantPer, fundPer);
                            UINT32 lcmPer = (wantPer / g) * fundPer;
                            wantPer = (lcmPer <= wantPer * 8u) ? lcmPer : rounded;
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
            // When resampling (device rate != Unity rate) the shared-mode engine
            // needs AUTOCONVERTPCM | SRC_DEFAULT_QUALITY to insert a sample-rate
            // converter — without it Initialize rejects the mismatched format with
            // AUDCLNT_E_UNSUPPORTED_FORMAT.
            DWORD convertFlags = ratesMatch ? 0u :
                (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);

            // Buffer for timer-driven mode: comfortably larger than the 10 ms
            // capture-thread poll so it cannot overrun between drains.
            REFERENCE_TIME hnsBuffer =
                (REFERENCE_TIME)((double)bufferSizeFrames / useFormat->nSamplesPerSec * 1e7);
            REFERENCE_TIME hnsMin = 400000; // 40 ms
            if (hnsBuffer < hnsMin) hnsBuffer = hnsMin;

            // Re-activate a fresh client: a failed Initialize can leave the
            // previous one in an undefined state.
            auto reactivate = [&]() -> bool {
                SafeRelease((IUnknown**)&gSession.audioClient);
                return SUCCEEDED(gSession.device->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    (void**)&gSession.audioClient));
            };

            // One Initialize attempt with logging.
            auto tryInit = [&](DWORD flags, REFERENCE_TIME buf,
                               WAVEFORMATEX* fmt, const char* label) -> HRESULT {
                if (!reactivate()) return E_FAIL;
                HRESULT h = gSession.audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, flags, buf, 0, fmt, nullptr);
                return h;
            };

            hr = E_FAIL;

            // 1. Event-driven with hnsBufferDuration = 0. This is the canonical
            //    shared-mode event-driven pattern; a non-zero duration here is
            //    rejected by some drivers (the AMS-44 returns UNSUPPORTED_FORMAT).
            if (FAILED(hr))
            {
                hr = tryInit(AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convertFlags,
                             0, useFormat, "event,buf0");
                eventDriven = true;
            }

            // 2. Timer-driven with a real buffer (capture thread polls every 10 ms).
            //    Covers drivers that reject EVENTCALLBACK (esp. with AUTOCONVERTPCM).
            if (FAILED(hr))
            {
                hr = tryInit(convertFlags, hnsBuffer, useFormat, "timer");
                eventDriven = false;
            }

            // 3 & 4. Matched-rate fallback to the device's own mix format (always
            //    supported in shared mode), capturing all channels and extracting
            //    the requested ones. Not applicable when resampling (rate differs).
            if (FAILED(hr) && ratesMatch && useFormat != pwfx)
            {
                useFormat = pwfx;
                gSession.deviceChannelCount = pwfx->nChannels;
                gSession.sampleRate         = (float)pwfx->nSamplesPerSec;

                if (FAILED(hr))
                {
                    hr = tryInit(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, pwfx, "mix,event,buf0");
                    eventDriven = true;
                }
                if (FAILED(hr))
                {
                    hr = tryInit(0, hnsBuffer, pwfx, "mix,timer");
                    eventDriven = false;
                }
            }

            // 5. Exclusive-mode fallback. Pro / ASIO-centric drivers (e.g. Zoom)
            //    often reject shared mode entirely but work in exclusive mode,
            //    which also gives the lowest latency. Exclusive does not resample,
            //    so only attempt it when the device runs at Unity's rate.
            //    Probe float32 first, then common integer PCM formats.
            if (FAILED(hr) && ratesMatch)
            {
                const DWORD exSr   = pwfx->nSamplesPerSec;
                const WORD  exCh   = pwfx->nChannels;
                const DWORD exMask = devMask ? devMask :
                                     (exCh == 1) ? SPEAKER_FRONT_CENTER :
                                     (exCh == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;

                struct Cand { WORD bits; WORD valid; bool pcm; };
                const Cand cands[] = {
                    { 32, 32, false }, // float32
                    { 32, 24, true  }, // 24-in-32 int
                    { 24, 24, true  }, // 24-bit packed int
                    { 16, 16, true  }, // 16-bit int
                };

                bool exOk = false;
                for (int ci = 0; ci < 4 && !exOk; ci++)
                {
                    ZeroMemory(&exWfx, sizeof(exWfx));
                    exWfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
                    exWfx.Format.nChannels       = exCh;
                    exWfx.Format.nSamplesPerSec  = exSr;
                    exWfx.Format.wBitsPerSample  = cands[ci].bits;
                    exWfx.Format.nBlockAlign     = (WORD)(exCh * cands[ci].bits / 8);
                    exWfx.Format.nAvgBytesPerSec = exSr * exWfx.Format.nBlockAlign;
                    exWfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    exWfx.Samples.wValidBitsPerSample = cands[ci].valid;
                    exWfx.dwChannelMask          = exMask;
                    exWfx.SubFormat = cands[ci].pcm ? KSDATAFORMAT_SUBTYPE_PCM
                                                    : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

                    if (!reactivate()) break;
                    HRESULT hrFmt = gSession.audioClient->IsFormatSupported(
                        AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&exWfx, nullptr);
                    if (hrFmt != S_OK) continue;

                    REFERENCE_TIME defPer = 0, minPer = 0;
                    gSession.audioClient->GetDevicePeriod(&defPer, &minPer);

                    REFERENCE_TIME per = defPer;
                    hr = gSession.audioClient->Initialize(
                        AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        per, per, (WAVEFORMATEX*)&exWfx, nullptr);

                    // Driver wants a specific aligned buffer size — recompute & retry.
                    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
                    {
                        UINT32 alignedFrames = 0;
                        gSession.audioClient->GetBufferSize(&alignedFrames);
                        REFERENCE_TIME ap =
                            (REFERENCE_TIME)(1e7 * alignedFrames / exSr + 0.5);
                        if (reactivate())
                        {
                            hr = gSession.audioClient->Initialize(
                                AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                ap, ap, (WAVEFORMATEX*)&exWfx, nullptr);
                        }
                    }

                    if (SUCCEEDED(hr))
                    {
                        useFormat = (WAVEFORMATEX*)&exWfx;
                        gSession.deviceChannelCount = exCh;
                        gSession.sampleRate         = (float)exSr;
                        eventDriven = true;
                        exOk = true;
                    }
                }
            }

            if (FAILED(hr))
            {
                errCode = -7; // Initialize failed
            }
        }
        else
        {
            hr = S_OK;
        }

        // Classify the final chosen format so the capture thread knows how to
        // convert raw samples to float (float32 for shared mode, possibly integer
        // PCM for exclusive mode).
        if (SUCCEEDED(hr) && useFormat)
        {
            int sf = SF_FLOAT32, bps = 4;
            // If classification fails, the float32 defaults above are kept.
            cniClassifyFormat(useFormat, &sf, &bps);
            gSession.sampleFmt      = sf;
            gSession.bytesPerSample = bps;
        }

        if (pwfxClosest) CoTaskMemFree(pwfxClosest);
    }

    CoTaskMemFree(pwfx);
    if (FAILED(hr)) goto cleanup;

    gSession.stopEvent  = CreateEvent(nullptr, TRUE,  FALSE, nullptr);
    gSession.readyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    // SetEventHandle is only valid (and required) for EVENTCALLBACK streams.
    // In timer-driven mode readyEvent stays unsignalled and the capture thread's
    // 10 ms poll timeout drives draining instead.
    if (eventDriven)
        gSession.audioClient->SetEventHandle(gSession.readyEvent);

    hr = gSession.audioClient->GetService(
        __uuidof(IAudioCaptureClient), (void**)&gSession.captureClient);
    if (FAILED(hr)) { errCode = -8; goto cleanup; } // GetService failed

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
        if (!gSession.ringBuffer) { errCode = -9; goto cleanup; } // alloc failed
    }

    hr = gSession.audioClient->Start();
    if (FAILED(hr)) { errCode = -11; goto cleanup; }

    gSession.running = true;
    gSession.captureThread = CreateThread(
        nullptr, 0, CaptureThreadProc, &gSession, 0, nullptr);
    if (!gSession.captureThread)
    {
        gSession.running = false;
        gSession.audioClient->Stop();
        errCode = -12; // CreateThread failed
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
    return errCode;
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
