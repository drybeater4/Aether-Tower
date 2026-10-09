#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cmath>
#pragma comment(lib, "ole32.lib")
int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en; CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    IMMDevice* dev; en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    LPWSTR id; dev->GetId(&id); wprintf(L"device %s\n", id);
    IAudioClient* ac; dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    WAVEFORMATEX* wf; ac->GetMixFormat(&wf); printf("mix: %d ch %d Hz %d bits tag %d\n", wf->nChannels, (int)wf->nSamplesPerSec, wf->wBitsPerSample, wf->wFormatTag);
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, wf, nullptr); printf("init %lx\n", (unsigned long)hr);
    IAudioCaptureClient* cc; ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc); ac->Start();
    for (int i = 0; i < secs * 4; ++i) {
        Sleep(250); double mx = 0; UINT32 pkt = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&pkt)) && pkt > 0) { BYTE* d; UINT32 fr; DWORD fl; cc->GetBuffer(&d, &fr, &fl, nullptr, nullptr); if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) { const float* f = (const float*)d; for (UINT32 k = 0; k < fr * wf->nChannels; ++k) { double a = fabs(f[k]); if (a > mx) mx = a; } } cc->ReleaseBuffer(fr); }
        printf("t=%.2f system peak=%.4f\n", i * 0.25, mx); fflush(stdout);
    }
    return 0;
}
