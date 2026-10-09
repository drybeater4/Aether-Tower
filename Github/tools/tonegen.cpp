#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <vector>
#include <cstdio>
#pragma comment(lib, "winmm.lib")
int main() {
    WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 44100; wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 44100 * 4;
    HWAVEOUT h; if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { puts("open failed"); return 1; }
    std::vector<short> buf(44100 * 2 * 20); for (size_t i = 0; i < buf.size() / 2; ++i) { short v = (short)(8000 * sin(i * 2 * 3.14159 * 440 / 44100)); buf[i * 2] = buf[i * 2 + 1] = v; }
    WAVEHDR hd{}; hd.lpData = (LPSTR)buf.data(); hd.dwBufferLength = (DWORD)(buf.size() * 2); waveOutPrepareHeader(h, &hd, sizeof hd); waveOutWrite(h, &hd, sizeof hd);
    printf("pid=%lu\n", GetCurrentProcessId()); fflush(stdout); Sleep(20000); return 0;
}
