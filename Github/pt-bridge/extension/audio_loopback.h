// Captures everything one process (and its children) plays and plays it again in THIS process, so a single application
// (Pizza Tower) carries both games' audio. Uses the Windows 10 2004+/11 "process loopback" capture; the captured process's own
// output can be muted per session (the capture is taken before the session volume).
#pragma once
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <audioclientactivationparams.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cmath>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

namespace praudio {

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IAudioClient* client = nullptr;
    HRESULT hr = E_FAIL;
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) { *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); return S_OK; }
        if (riid == __uuidof(IAgileObject)) { *ppv = static_cast<IAgileObject*>(this); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT ahr = E_FAIL; IUnknown* unk = nullptr;
        hr = op->GetActivateResult(&ahr, &unk);
        if (SUCCEEDED(hr) && SUCCEEDED(ahr) && unk) { hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client); unk->Release(); }
        else if (SUCCEEDED(hr)) hr = ahr;
        SetEvent(done);
        return S_OK;
    }
};

struct Bridge {
    std::atomic<bool> run{ false };
    std::atomic<float> gain{ 1.0f };
    std::atomic<float> rms{ 0.0f };
    std::atomic<int> state{ 0 };            // 0 idle, 1 running, -1 failed
    std::atomic<long> hr_fail{ 0 };
    std::atomic<int> stage{ 0 };
    std::atomic<long> npk{ 0 }, nsilent{ 0 }, nframes{ 0 }, nroom0{ 0 };
    std::atomic<int> maxabs{ 0 };
    std::thread th;
    DWORD pid = 0;
    bool mute_target = true;         // keep the target's own output (nearly) silent: it is heard through this process instead
    float target_volume = 0.004f;    // (the capture is taken after the session volume, so it is made up for with gain)

    void set_session_volume(float vol, bool unmute_always) {
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
            int n = 0; se->GetCount(&n);
            for (int i = 0; i < n; ++i) {
                IAudioSessionControl* c = nullptr; if (FAILED(se->GetSession(i, &c))) continue;
                IAudioSessionControl2* c2 = nullptr;
                if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) {
                    DWORD p = 0; c2->GetProcessId(&p);
                    if (p == pid) { ISimpleAudioVolume* v = nullptr; if (SUCCEEDED(c2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&v))) { v->SetMute(FALSE, nullptr); v->SetMasterVolume(vol, nullptr); v->Release(); } }
                    c2->Release();
                }
                c->Release();
            }
        }
        if (se) se->Release(); if (mgr) mgr->Release(); if (dev) dev->Release(); if (en) en->Release();
    }


    float session_peak() {
        float peak = -1; IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return -2;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
            int n = 0; se->GetCount(&n);
            for (int i = 0; i < n; ++i) {
                IAudioSessionControl* c = nullptr; if (FAILED(se->GetSession(i, &c))) continue;
                IAudioSessionControl2* c2 = nullptr;
                if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) {
                    DWORD p = 0; c2->GetProcessId(&p);
                    if (p == pid) { IAudioMeterInformation* m = nullptr; if (SUCCEEDED(c2->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&m))) { float v = 0; m->GetPeakValue(&v); if (v > peak) peak = v; m->Release(); } }
                    c2->Release();
                }
                c->Release();
            }
        }
        if (se) se->Release(); if (mgr) mgr->Release(); if (dev) dev->Release(); if (en) en->Release();
        return peak;
    }

    void worker() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 32;
        wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8; wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        IAudioClient* cap = nullptr; IAudioCaptureClient* cc = nullptr;
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ren = nullptr; IAudioRenderClient* rc = nullptr;
        HANDLE capEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto fail = [&](HRESULT h) { hr_fail = (long)h; state = -1; };
        stage = 1;
        do {
            AUDIOCLIENT_ACTIVATION_PARAMS ap{}; ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            ap.ProcessLoopbackParams.TargetProcessId = pid; ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            PROPVARIANT pv{}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = (BYTE*)&ap;
            ActivateHandler h; IActivateAudioInterfaceAsyncOperation* op = nullptr;
            HRESULT r = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, &h, &op);
            if (FAILED(r)) { fail(r); break; }
            WaitForSingleObject(h.done, 5000);
            if (op) op->Release();
            if (FAILED(h.hr) || !h.client) { fail(h.hr); break; }
            cap = h.client;
            stage = 2;
            r = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, &wf, nullptr);
            if (FAILED(r)) { fail(r); break; }
            cap->SetEventHandle(capEv);
            if (FAILED(r = cap->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))) { fail(r); break; }
            if (FAILED(r = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) { fail(r); break; }
            if (FAILED(r = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { fail(r); break; }
            if (FAILED(r = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ren))) { fail(r); break; }
            stage = 3;
            if (FAILED(r = ren->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 400000, 0, &wf, nullptr))) { fail(r); break; }   // 40 ms
            UINT32 rbuf = 0; ren->GetBufferSize(&rbuf);
            if (FAILED(r = ren->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) { fail(r); break; }
            cap->Start(); ren->Start();
            state = 1;
            DWORD lastmute = 0;
            while (run) {
                WaitForSingleObject(capEv, 20);
                if (mute_target && GetTickCount() - lastmute > 1000) { lastmute = GetTickCount(); set_session_volume(target_volume, true); }
                UINT32 pkt = 0;
                while (run && SUCCEEDED(cc->GetNextPacketSize(&pkt)) && pkt > 0) {
                    BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                    if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    ++npk; nframes += (long)frames; if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++nsilent; else { const float* q = (const float*)data; float m = 0; for (UINT32 i = 0; i < frames * 2; ++i) { float a2 = q[i] < 0 ? -q[i] : q[i]; if (a2 > m) m = a2; } int mi = (int)(m * 10000); if (mi > maxabs.load()) maxabs = mi; }
                    UINT32 pad = 0; ren->GetCurrentPadding(&pad);
                    UINT32 room = rbuf > pad ? rbuf - pad : 0;
                    UINT32 n = frames < room ? frames : room; if (n == 0) ++nroom0;
                    double acc = 0;
                    if (n > 0) {
                        BYTE* out = nullptr;
                        if (SUCCEEDED(rc->GetBuffer(n, &out))) {
                            float g = gain.load() * (mute_target ? 1.0f / target_volume : 1.0f);
                            float* o = (float*)out;
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(out, 0, n * wf.nBlockAlign);
                            else {
                                const float* in = (const float*)data;
                                for (UINT32 i = 0; i < n * 2; ++i) { float v = in[i] * g; if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f; o[i] = v; acc += (double)v * v; }
                            }
                            rc->ReleaseBuffer(n, 0);
                        }
                    }
                    { float cur = n ? (float)std::sqrt(acc / (n * 2.0)) : 0.0f; if (cur > rms.load()) rms = cur; }
                    cc->ReleaseBuffer(frames);
                }
            }
            cap->Stop(); ren->Stop();
        } while (false);
        if (rc) rc->Release(); if (ren) ren->Release(); if (dev) dev->Release(); if (en) en->Release();
        if (cc) cc->Release(); if (cap) cap->Release();
        CloseHandle(capEv);
        if (mute_target) set_session_volume(1.0f, true);
        CoUninitialize();
        if (state == 1) state = 0;
    }
    void start(DWORD target_pid) { if (run) return; pid = target_pid; run = true; state = 0; th = std::thread([this] { worker(); }); }
    void stop() { if (!run) return; run = false; if (th.joinable()) th.join(); }
};

}  // namespace praudio
